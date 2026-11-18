#pragma once
// Timestamp helpers for millis()-based ages.
//
// The firmware passes a `now` snapshot down the call chain, but code that is
// called from several places stamps events with millis() directly. Both are
// "the same clock", yet they are not the same *value*: main.cpp captures `now`
// at the top of the loop and the WiFi or metering work that runs before the
// radio can take hundreds of milliseconds - a D0 wake-up even seconds. Any
// comparison of the stale `now` against a fresh millis() stamp then subtracts a
// larger number from a smaller one and, in unsigned arithmetic, produces
// something near 2^32.
//
// That is not a cosmetic problem: it made a peer that had just answered look
// like it had been silent for 71 minutes, so the link permanently reported
// "offline" while packets were flowing. Every subtraction of two millis()
// values now goes through these, which compare as signed and clamp negatives.

#include <cstdint>

// Seconds since `stampMs`, never negative.
inline uint32_t ageSeconds(uint32_t nowMs, uint32_t stampMs) {
  const int32_t delta = static_cast<int32_t>(nowMs - stampMs);
  return delta > 0 ? static_cast<uint32_t>(delta / 1000) : 0;
}

// True when `stampMs` is older than `limitMs`. With stampMs == 0 (never set)
// this is true, so callers must check "have we ever seen it" separately.
inline bool olderThan(uint32_t nowMs, uint32_t stampMs, uint32_t limitMs) {
  return static_cast<int32_t>(nowMs - stampMs) > static_cast<int32_t>(limitMs);
}
