#include "duty_cycle.h"

namespace core {

DutyCycleBudget::DutyCycleBudget(uint32_t windowMs, uint32_t budgetMs) {
  configure(windowMs, budgetMs);
}

void DutyCycleBudget::configure(uint32_t windowMs, uint32_t budgetMs) {
  windowMs_ = windowMs == 0 ? kDefaultWindowMs : windowMs;
  budgetMs_ = budgetMs == 0 ? kDefaultBudgetMs : budgetMs;
  spanMs_ = windowMs_ / kBuckets;
  if (spanMs_ == 0) spanMs_ = 1;
  reset();
}

void DutyCycleBudget::reset() {
  for (size_t i = 0; i < kSlots; ++i) {
    buckets_[i].at = 0;
    buckets_[i].ms = 0;
    buckets_[i].used = false;
  }
  transmissions_ = 0;
  transmittedMs_ = 0;
  blockedCount_ = 0;
}

DutyCycleBudget::Bucket& DutyCycleBudget::bucketFor(uint32_t nowMs) {
  // Found by its span start, not by an index computed from the time: 2^32 ms is
  // not a whole number of spans, so an index sequence restarts at the millis()
  // wrap and ran into buckets a minute or two old - which were cleared while
  // their airtime was still inside the window. A bucket is only reused once it
  // has left the window; with one slot more than spans per window, one always
  // has, also across the wrap.
  const uint32_t spanStart = nowMs - (nowMs % spanMs_);
  Bucket* reuse = nullptr;
  uint32_t reuseAge = 0;
  for (size_t i = 0; i < kSlots; ++i) {
    Bucket& bucket = buckets_[i];
    if (bucket.used && bucket.at == spanStart) return bucket;
    const uint32_t age = bucket.used ? nowMs - bucket.at : UINT32_MAX;
    if (reuse == nullptr || age > reuseAge) {
      reuse = &bucket;
      reuseAge = age;
    }
  }
  reuse->at = spanStart;
  reuse->ms = 0;
  reuse->used = true;
  return *reuse;
}

uint32_t DutyCycleBudget::usedMs(uint32_t nowMs) const {
  uint32_t used = 0;
  for (size_t i = 0; i < kSlots; ++i) {
    if (!buckets_[i].used) continue;
    // Unsigned arithmetic also handles the millis() wrap correctly.
    if (nowMs - buckets_[i].at < windowMs_) used += buckets_[i].ms;
  }
  return used;
}

uint32_t DutyCycleBudget::remainingMs(uint32_t nowMs) const {
  const uint32_t used = usedMs(nowMs);
  return used >= budgetMs_ ? 0 : budgetMs_ - used;
}

bool DutyCycleBudget::canTransmit(uint32_t nowMs, uint32_t airtimeMs) const {
  return airtimeMs <= remainingMs(nowMs);
}

void DutyCycleBudget::commit(uint32_t nowMs, uint32_t airtimeMs) {
  bucketFor(nowMs).ms += airtimeMs;
  ++transmissions_;
  transmittedMs_ += airtimeMs;
}

uint32_t DutyCycleBudget::waitFor(uint32_t nowMs, uint32_t airtimeMs) const {
  const uint32_t remaining = remainingMs(nowMs);
  if (remaining >= airtimeMs) return 0;

  // Budget frees up as buckets leave the window: walk them oldest first and
  // return the moment enough has expired.
  const uint32_t needed = airtimeMs - remaining;
  bool counted[kSlots] = {false};
  uint32_t freed = 0;
  uint32_t wait = 0;

  for (size_t n = 0; n < kSlots; ++n) {
    int oldest = -1;
    for (size_t i = 0; i < kSlots; ++i) {
      if (counted[i] || !buckets_[i].used) continue;
      if (nowMs - buckets_[i].at >= windowMs_) continue;
      if (oldest < 0 || buckets_[i].at < buckets_[oldest].at) oldest = static_cast<int>(i);
    }
    if (oldest < 0) break;
    counted[oldest] = true;
    freed += buckets_[oldest].ms;
    wait = (buckets_[oldest].at + windowMs_) - nowMs;
    if (freed >= needed) return wait;
  }
  return wait == 0 ? windowMs_ : wait;
}

uint32_t DutyCycleBudget::usedPercent(uint32_t nowMs) const {
  if (budgetMs_ == 0) return 0;
  return static_cast<uint32_t>((static_cast<uint64_t>(usedMs(nowMs)) * 100u) / budgetMs_);
}

}  // namespace core
