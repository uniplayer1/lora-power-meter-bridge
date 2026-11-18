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
  for (size_t i = 0; i < kBuckets; ++i) {
    buckets_[i].at = 0;
    buckets_[i].ms = 0;
    buckets_[i].used = false;
  }
  transmissions_ = 0;
  transmittedMs_ = 0;
  blockedCount_ = 0;
}

DutyCycleBudget::Bucket& DutyCycleBudget::bucketFor(uint32_t nowMs) {
  // The index recurs once per window, so a bucket holding a different span
  // start is by definition a full window old and safe to reuse.
  const size_t index = static_cast<size_t>((nowMs / spanMs_) % kBuckets);
  Bucket& bucket = buckets_[index];
  const uint32_t spanStart = nowMs - (nowMs % spanMs_);
  if (!bucket.used || bucket.at != spanStart) {
    bucket.at = spanStart;
    bucket.ms = 0;
    bucket.used = true;
  }
  return bucket;
}

uint32_t DutyCycleBudget::usedMs(uint32_t nowMs) const {
  uint32_t used = 0;
  for (size_t i = 0; i < kBuckets; ++i) {
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
  bool counted[kBuckets] = {false};
  uint32_t freed = 0;
  uint32_t wait = 0;

  for (size_t n = 0; n < kBuckets; ++n) {
    int oldest = -1;
    for (size_t i = 0; i < kBuckets; ++i) {
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
