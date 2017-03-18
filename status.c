/**
 *  osd2web plugin for the Video Disk Recorder
 *
 *  status.c
 *
 *  (c) 2017 Jörg Wendel
 *
 * This code is distributed under the terms and conditions of the
 * GNU GENERAL PUBLIC LICENSE. See the file COPYING for details.
 *
 **/

//***************************************************************************
// Includes
//***************************************************************************

#include <algorithm>

#include <vdr/plugin.h>

#include "update.h"
#include "epg2vdr.h"

//***************************************************************************
// Osd Channel Switch
//***************************************************************************

void cUpdate::ChannelSwitch(const cDevice* device, int channelNumber, bool liveView)
{
   if (liveView && channelNumber)
   {
      tell(0, "ChannelSwitch: channelNumber: %d", channelNumber);
      currentChannelNr = channelNumber;
      updatePresentFollowing();
   }
}

//***************************************************************************
// Recording
//***************************************************************************

void cUpdate::OsdProgramme(time_t PresentTime, const char* PresentTitle,
                           const char* PresentSubtitle, time_t FollowingTime,
                           const char* FollowingTitle, const char* FollowingSubtitle)
{
   // haveActualEpg is NOT set if the epg not avalible at channelswich
   //   somtimes the epg is loaded later ...

   if (!haveActualEpg && (!isEmpty(PresentTitle) || !isEmpty(FollowingTitle)))
   {
      tell(0, "OsdProgramme: PresentTitle '%s', FollowingTitle '%s'",
           PresentTitle, FollowingTitle);

      updatePresentFollowing();
   }
}

//***************************************************************************
// Recording
//***************************************************************************

void cUpdate::Recording(const cDevice* Device, const char* Name,
                        const char* FileName, bool On)
{
   // #TODO - to be implemented
}

//***************************************************************************
// Replaying
//***************************************************************************

void cUpdate::Replaying(const cControl* Control, const char* Name,
                        const char* FileName, bool On)
{
   tell(0, "Replaying: Replay '%s', Name '%s', FileName '%s'",
        On ? "Start" : "Stop" , notNull(Name), FileName);

   if (!On)
      return ;

   // Replaying: Replay 'Start', Name 'Fifty Shades of Grey', FileName '/tank/video/Fifty_Shades_of_Grey/2015-12-06.20.13.57-0.rec'
   // Replaying: Replay 'Stop',  Name '(null)',               FileName '/tank/video/Fifty_Shades_of_Grey/2015-12-06.20.13.57-0.rec'

   json_t* oRecording = json_object();

#if defined (APIVERSNUM) && (APIVERSNUM >= 20301)
   const cRecordings* recordings;
   cStateKey stateKey;

   if (!(recordings = cRecordings::GetRecordingsRead(stateKey, 500)))
      tell(0, "Can't get lock for Recordings, retrying later");
#else
   cRecordings* recordings = &Recordings;
#endif

   const cRecording* recording = recordings ? recordings->GetByName(FileName) : 0;

   if (recording)
   {
      recording2Json(oRecording, recording);
   }
   else
   {
      addToJson(oRecording, "name", Name);
      addToJson(oRecording, "filename", FileName);
   }

   cUpdate::pushMessage(oRecording, "replay");

#if defined (APIVERSNUM) && (APIVERSNUM >= 20301)
   if (recordings)
      stateKey.Remove();
#endif
}

//***************************************************************************
// Timer Change
//***************************************************************************

void cUpdate::TimerChange(const cTimer* Timer, eTimerChange Change)
{
   // update timers here only without epg2vdr
   //   with epg2vdr it is updated by a service interface trigger

   if (!epg2vdrIsLoaded)
      updateTimers();
}

//***************************************************************************
// Update Present / Following
//***************************************************************************

void cUpdate::updatePresentFollowing()
{
   if (!currentChannelNr)
      return;

   haveActualEpg = no;

#if defined (APIVERSNUM) && (APIVERSNUM >= 20301)
   LOCK_CHANNELS_READ;
   const cChannel* channel = Channels->GetByNumber(currentChannelNr);
#else
   const cChannel* channel = Channels.GetByNumber(currentChannelNr);
#endif

   if (channel)
   {
      tell(0, "update present/following for channel '%s'", channel->Name());

#if defined (APIVERSNUM) && (APIVERSNUM >= 20301)
      LOCK_SCHEDULES_READ;
      const cSchedules* schedules = Schedules;
#else
      cSchedulesLock schedulesLock;
      const cSchedules* schedules = (cSchedules*)cSchedules::Schedules(schedulesLock);
#endif

      json_t* obj = json_object();
      json_t* oStreamInfo = json_object();
      json_t* oChannel = json_object();
      json_t* oPresent = json_object();
      json_t* oFollowing = json_object();

      channel2Json(oChannel, channel);
      stream2Json(oStreamInfo, channel);

      json_object_set_new(obj, "channel", oChannel);
      json_object_set_new(obj, "streaminfo", oStreamInfo);

      const cSchedule* schedule = schedules ? schedules->GetSchedule(channel->GetChannelID()) : 0;

      if (schedule)
      {
         int ownPresent = no;
         int ownFollowing = no;
         const cEvent* present = schedule->GetPresentEvent();
         const cEvent* following = schedule->GetFollowingEvent();
         cPlugin* pEpg2Vdr = cPluginManager::GetPlugin("epg2vdr");

         if (pEpg2Vdr)
         {
            cEpgEvent_Service_V1 data;

            data.in = schedule->GetPresentEvent();

            if (pEpg2Vdr->Service(EPG2VDR_EVENT_SERVICE, &data))
            {
               ownPresent = yes;
               present = data.out;
            }
            else
               tell(0, "EPG2VDR_EVENT_SERVICE failed");

            data.in = schedule->GetFollowingEvent();

            if (pEpg2Vdr->Service(EPG2VDR_EVENT_SERVICE, &data))
            {
               ownFollowing = yes;
               following = data.out;
            }
            else
               tell(0, "EPG2VDR_EVENT_SERVICE failed");
         }

         haveActualEpg = present != 0;

         event2Json(oPresent, present, 0, (eTimerMatch)na, no, cOsdService::osLarge);
         event2Json(oFollowing, following, 0, (eTimerMatch)na, no, cOsdService::osLarge);

         // we need a trigger on start of following event

         nextPresentUpdateAt = following ? following->StartTime() : time(0) + 10;

         if (ownPresent)     { delete present;   present = 0; }
         if (ownFollowing)   { delete following; following = 0; }
      }
      else
      {
         nextPresentUpdateAt = time(0) + 60;
         tell(0, "Info: Can't get schedules");
      }

      json_object_set_new(obj, "present", oPresent);
      json_object_set_new(obj, "following", oFollowing);

      cUpdate::pushMessage(obj, "actual");
   }
}

//***************************************************************************
// Update Timers
//***************************************************************************

void cUpdate::updateTimers()
{
   json_t* oTimers = json_array();

   cPlugin* pEpg2Vdr = cPluginManager::GetPlugin("epg2vdr");

   if (pEpg2Vdr)
   {
      epg2vdrIsLoaded = yes;

      cEpgTimer_Service_V1 data;

      if (pEpg2Vdr->Service(EPG2VDR_TIMER_SERVICE, &data))
      {
         for (auto it = data.epgTimers.begin(); it != data.epgTimers.end(); ++it)
         {
            cEpgTimer_Interface_V1* timer = (*it);

            tell(0, "Got '%s' timer for '%s' - '%s'",
                 timer->isLocal() ? "local" : "remote",
                 timer->File(),
                 timer->hasState('R') ? "timer is recording" : "timer is pending");

            json_t* oTimer = json_object();
            timer2Json(oTimer, timer);
            json_array_append_new(oTimers, oTimer);

            delete timer;
         }

         cUpdate::pushMessage(oTimers, "timers");
      }
   }

   else
   {
#if defined (APIVERSNUM) && (APIVERSNUM >= 20301)
      const cTimers* timers = 0;
      cStateKey stateKey;

      if (!(timers = cTimers::GetTimersRead(stateKey, 500)))
      {
         tell(0, "Can't get lock for updateTimers(), retrying later");
         return ;
      }
#else
      const cTimers* timers = &Timers;
#endif

      for (const cTimer* timer = timers->First(); timer; timer = timers->Next(timer))
      {
         json_t* oTimer = json_object();
         tell(0, "Got timer for '%s' - '%s'", timer->File(),
              timer->Recording() ? "timer is regording" : "timer is pending");

         timer2Json(oTimer, timer);
         json_array_append_new(oTimers, oTimer);
      }

#if defined (APIVERSNUM) && (APIVERSNUM >= 20301)
      stateKey.Remove();
#endif
      cUpdate::pushMessage(oTimers, "timers");
   }

   triggerTimerUpdate = no;
}
