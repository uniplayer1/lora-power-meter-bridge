#pragma once
// Legal airtime accounting for the 868.0-868.6 MHz band.
//
// ERC/REC 70-03 Annex 1 sub-band h1.5 (implemented nationally by BNetzA
// Vfg. 91/2025, table 2 row 48) allows 25 mW ERP and a duty cycle of at most
// 1 % over a rolling hour: 36 seconds of transmit time in any 60-minute window.
// Every transmission goes through this accountant, and when the budget is spent
// the scheduler stretches its interval instead of transmitting illegally.
//
// Accounting is bucketed by minute rather than kept as a list of individual
// transmissions: at a 2 s send interval an hour holds 1800 samples, and a
// fixed-size sample ring silently under-counted past its capacity - which is a
// legal failure, not a cosmetic one. 60 one-minute buckets cost 360 bytes and
// cannot overflow. The price is that a bucket counts as used until a full
// window has passed, so the figure is conservative by at most one minute -
// over-reporting airtime, never under-reporting it.
//
// The clock is injected rather than read from millis(), so window roll-off is
// unit-testable without waiting an hour.

#include <cstddef>
#include <cstdint>

namespace core {

class DutyCycleBudget {
 public:
  static constexpr uint32_t kDefaultWindowMs = 60u * 60u * 1000u;  // 1 hour
  static constexpr uint32_t kDefaultBudgetMs = 36u * 1000u;        // 1 % = 36 s
  static constexpr size_t kBuckets = 60;

  DutyCycleBudget(uint32_t windowMs = kDefaultWindowMs,
                  uint32_t budgetMs = kDefaultBudgetMs);

  void configure(uint32_t windowMs, uint32_t budgetMs);

  // Would `airtimeMs` fit right now?
  bool canTransmit(uint32_t nowMs, uint32_t airtimeMs) const;

  // Records a transmission that actually happened.
  void commit(uint32_t nowMs, uint32_t airtimeMs);

  uint32_t usedMs(uint32_t nowMs) const;
  uint32_t remainingMs(uint32_t nowMs) const;

  // How long the scheduler should wait before `airtimeMs` fits again.
  uint32_t waitFor(uint32_t nowMs, uint32_t airtimeMs) const;

  uint32_t windowMs() const { return windowMs_; }
  uint32_t budgetMs() const { return budgetMs_; }
  uint32_t bucketSpanMs() const { return spanMs_; }
  uint32_t usedPercent(uint32_t nowMs) const;

  uint32_t transmissions() const { return transmissions_; }
  uint32_t transmittedMs() const { return transmittedMs_; }
  uint32_t blockedCount() const { return blockedCount_; }
  void noteBlocked() { ++blockedCount_; }
  void reset();

 private:
  struct Bucket {
    uint32_t at;  // start of the minute this bucket covers
    uint32_t ms;
    bool used;
  };

  Bucket& bucketFor(uint32_t nowMs);

  Bucket buckets_[kBuckets];
  uint32_t windowMs_;
  uint32_t budgetMs_;
  uint32_t spanMs_;
  uint32_t transmissions_ = 0;
  uint32_t transmittedMs_ = 0;
  uint32_t blockedCount_ = 0;
};

}  // namespace core
