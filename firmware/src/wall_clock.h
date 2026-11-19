#pragma once
// Wall-clock time.
//
// Everything else in the firmware measures ages with millis(); this is only for
// showing when something happened. A board with internet access sets its clock
// by NTP; the other board learns the time over the radio link from it
// (PacketType::TimeSync). NTP always wins over a time received by radio.

#include <Arduino.h>
#include <time.h>

enum class ClockSource : uint8_t {
  None = 0,   // not set since boot
  Ntp = 1,    // from the internet
  Radio = 2,  // from the paired board
};

// Sets the time zone.
void clockBegin();

// Starts NTP once the home network is connected, and reports a sync in the
// event log (the sync itself happens in lwIP).
void clockLoop();

bool clockValid();
ClockSource clockSource();
const char* clockSourceName();  // "ntp", "funk" or ""
// millis() of the last successful sync, 0 if none.
uint32_t clockSyncedAtMs();

// Current time as milliseconds since 1970 (UTC), 0 while the clock is not set.
uint64_t clockEpochMs();

// Applies a time received over the link. Ignored while NTP is in charge, and a
// jump of more than five minutes into the past is refused once the clock is
// set: that is a replayed frame, not drift. Returns true when applied.
bool clockSetFromRadio(uint64_t epochMs);

// Local time formatted with strftime(); writes "" while the clock is not set.
void clockFormat(char* out, size_t len, const char* fmt);
