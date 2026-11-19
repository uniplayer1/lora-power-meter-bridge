#include "wall_clock.h"

#include <esp_sntp.h>
#include <sys/time.h>

#include "app_config.h"
#include "config.h"
#include "net.h"

namespace {

// Anything before 2024 means "never set": the ESP32 starts at 1970.
constexpr time_t kValidAfter = 1704067200;

volatile ClockSource source = ClockSource::None;
volatile uint32_t syncedAtMs = 0;
volatile bool ntpJustSynced = false;
bool ntpStarted = false;

void onNtpSync(struct timeval*) {
  // Runs in the lwIP task: keep it to flags.
  source = ClockSource::Ntp;
  syncedAtMs = millis();
  if (syncedAtMs == 0) syncedAtMs = 1;
  ntpJustSynced = true;
}

}  // namespace

void clockBegin() {
  setenv("TZ", TIME_ZONE_POSIX, 1);
  tzset();
}

bool clockValid() { return source != ClockSource::None && time(nullptr) > kValidAfter; }

ClockSource clockSource() { return clockValid() ? source : ClockSource::None; }

const char* clockSourceName() {
  switch (clockSource()) {
    case ClockSource::Ntp: return "ntp";
    case ClockSource::Radio: return "funk";
    default: return "";
  }
}

void clockLoop() {
  // NTP starts the first time the station is online - not at boot: a board
  // that gets its home network during setup has none at boot, and used to
  // never ask for the time until it was restarted. Once started, lwIP
  // re-syncs every hour by itself.
  if (!ntpStarted && netStaUp()) {
    ntpStarted = true;
    sntp_set_time_sync_notification_cb(onNtpSync);
    configTzTime(TIME_ZONE_POSIX, NTP_SERVER_1, NTP_SERVER_2);
    logEvent("clock: asking %s for the time", NTP_SERVER_1);
  }
  if (!ntpJustSynced) return;
  ntpJustSynced = false;
  char text[24];
  clockFormat(text, sizeof(text), "%d.%m.%Y %H:%M:%S");
  logEvent("clock set by NTP: %s", text);
}

uint32_t clockSyncedAtMs() { return syncedAtMs; }

uint64_t clockEpochMs() {
  if (!clockValid()) return 0;
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return static_cast<uint64_t>(tv.tv_sec) * 1000ULL + static_cast<uint64_t>(tv.tv_usec / 1000);
}

bool clockSetFromRadio(uint64_t epochMs) {
  if (clockSource() == ClockSource::Ntp) return false;
  if (epochMs / 1000ULL <= static_cast<uint64_t>(kValidAfter)) return false;
  const uint64_t current = clockEpochMs();
  if (current != 0 && epochMs + 5ULL * 60ULL * 1000ULL < current) {
    logEvent("clock: refused a time from the link %lus in the past",
             static_cast<unsigned long>((current - epochMs) / 1000ULL));
    return false;
  }
  struct timeval tv;
  tv.tv_sec = static_cast<time_t>(epochMs / 1000ULL);
  tv.tv_usec = static_cast<suseconds_t>((epochMs % 1000ULL) * 1000ULL);
  settimeofday(&tv, nullptr);
  const bool first = source == ClockSource::None;
  source = ClockSource::Radio;
  syncedAtMs = millis();
  if (syncedAtMs == 0) syncedAtMs = 1;
  if (first) {
    char text[24];
    clockFormat(text, sizeof(text), "%d.%m.%Y %H:%M:%S");
    logEvent("clock set by the paired board: %s", text);
  }
  return true;
}

void clockFormat(char* out, size_t len, const char* fmt) {
  if (len == 0) return;
  out[0] = '\0';
  if (!clockValid()) return;
  const time_t now = time(nullptr);
  struct tm local;
  localtime_r(&now, &local);
  strftime(out, len, fmt, &local);
}
