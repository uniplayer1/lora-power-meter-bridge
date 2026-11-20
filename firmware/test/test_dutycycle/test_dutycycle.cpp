#include "../unity_case.h"
// Duty-cycle accounting tests. This is a legal limit, not a tuning knob: the
// firmware must refuse to transmit once 36 s of the last hour are used.

#include <unity.h>

#include "duty_cycle.h"

using namespace core;

void setUp() {}
void tearDown() {}

TEST_CASE("the budget is exactly one percent of an hour") {
  DutyCycleBudget budget;
  TEST_ASSERT_EQUAL_UINT32(3600000, budget.windowMs());
  TEST_ASSERT_EQUAL_UINT32(36000, budget.budgetMs());
  TEST_ASSERT_EQUAL_UINT32(36000, budget.remainingMs(0));
  TEST_ASSERT_TRUE(budget.canTransmit(0, 36000));
}

TEST_CASE("the first millisecond over the budget is refused") {
  DutyCycleBudget budget;
  budget.commit(0, 36000);

  TEST_ASSERT_FALSE(budget.canTransmit(1000, 1));
  TEST_ASSERT_EQUAL_UINT32(0, budget.remainingMs(1000));
  TEST_ASSERT_EQUAL_UINT32(100, budget.usedPercent(1000));
}

TEST_CASE("the window rolls off after an hour") {
  DutyCycleBudget budget;
  budget.commit(0, 36000);

  TEST_ASSERT_FALSE(budget.canTransmit(3599999, 1));
  TEST_ASSERT_TRUE(budget.canTransmit(3600000, 1));
  TEST_ASSERT_EQUAL_UINT32(36000, budget.remainingMs(3600000));
  TEST_ASSERT_EQUAL_UINT32(0, budget.usedPercent(3600000));
}

TEST_CASE("many small transmissions accumulate correctly") {
  DutyCycleBudget budget;
  // A 44 ms packet every 10 s: 360 transmissions in an hour is 15.84 s of the
  // 36 s budget, comfortably legal - and an hour holds far more samples than a
  // fixed-size sample ring could, which is why the accounting is bucketed.
  for (uint32_t i = 0; i < 360; ++i) {
    const uint32_t now = i * 10000;
    TEST_ASSERT_TRUE(budget.canTransmit(now, 44));
    budget.commit(now, 44);
  }
  TEST_ASSERT_EQUAL_UINT32(360 * 44, budget.usedMs(3590 * 1000));
  TEST_ASSERT_EQUAL_UINT32(44, budget.usedPercent(3590 * 1000));
  TEST_ASSERT_EQUAL_UINT32(360, budget.transmissions());
}

TEST_CASE("the scheduler is told how long to wait instead of transmitting") {
  DutyCycleBudget budget;
  budget.commit(0, 36000);

  // At t = 1 s the sample ages out at t = 3600 s.
  TEST_ASSERT_EQUAL_UINT32(3599000, budget.waitFor(1000, 1000));
  TEST_ASSERT_EQUAL_UINT32(0, budget.waitFor(3600000, 1000));
}

TEST_CASE("millis() wraparound does not leak budget") {
  DutyCycleBudget budget;
  const uint32_t start = 0xFFFFF000u;  // wraps ~4 s later
  budget.commit(start, 36000);

  const uint32_t afterWrap = start + 5000u;
  TEST_ASSERT_EQUAL_UINT32(36000, budget.usedMs(afterWrap));
  TEST_ASSERT_FALSE(budget.canTransmit(afterWrap, 1));

  const uint32_t longAfter = start + 3600000u;
  TEST_ASSERT_EQUAL_UINT32(0, budget.usedMs(longAfter));
  TEST_ASSERT_TRUE(budget.canTransmit(longAfter, 36000));
}

TEST_CASE("airtime recorded across the wraparound is all counted") {
  // Buckets used to be picked by (time / span) % 60. 2^32 ms is not a whole
  // number of spans, so after the wrap the index sequence restarts at 0 and
  // runs into buckets that were filled a minute or two earlier - which were
  // then cleared, and their airtime forgotten while still inside the hour.
  DutyCycleBudget budget;
  const uint32_t beforeA = 0xFFFFFFFFu - 70000u;  // two spans before the wrap
  const uint32_t beforeB = 0xFFFFFFFFu - 10000u;  // last span before the wrap
  const uint32_t afterA = 5000u;                  // first span after the wrap
  const uint32_t afterB = 65000u;                 // second span after the wrap
  budget.commit(beforeA, 10000);
  budget.commit(beforeB, 10000);
  budget.commit(afterA, 1000);
  budget.commit(afterB, 1000);
  TEST_ASSERT_EQUAL_UINT32(22000, budget.usedMs(afterB));

  // And it all still leaves the window an hour after each transmission.
  TEST_ASSERT_EQUAL_UINT32(2000, budget.usedMs(beforeB + 3600001u));
  TEST_ASSERT_EQUAL_UINT32(0, budget.usedMs(afterB + 3600001u));
}

TEST_CASE("a meter sending every 15 s is counted exactly through the wraparound") {
  // Four hours of a real schedule centred on the wrap, checked after every
  // packet against a brute-force count of what is inside the window.
  DutyCycleBudget budget;
  const uint32_t span = DutyCycleBudget::kDefaultWindowMs / DutyCycleBudget::kBuckets;
  const uint32_t start = 0u - 2u * 3600000u;
  const uint32_t step = 15000u;
  const uint32_t airtime = 93u;
  const size_t packets = 4u * 3600000u / step;
  static uint32_t stamps[960];
  TEST_ASSERT_TRUE(packets <= sizeof(stamps) / sizeof(stamps[0]));
  for (size_t n = 0; n < packets; ++n) {
    const uint32_t now = start + static_cast<uint32_t>(n) * step;
    budget.commit(now, airtime);
    stamps[n] = now - (now % span);  // the span the packet was booked into
    uint32_t expected = 0;
    for (size_t k = 0; k <= n; ++k) {
      if (now - stamps[k] < DutyCycleBudget::kDefaultWindowMs) expected += airtime;
    }
    TEST_ASSERT_EQUAL_UINT32(expected, budget.usedMs(now));
  }
}

TEST_CASE("reset clears the accounting") {
  DutyCycleBudget budget;
  budget.commit(0, 36000);
  budget.reset();
  TEST_ASSERT_EQUAL_UINT32(0, budget.usedMs(0));
  TEST_ASSERT_EQUAL_UINT32(0, budget.transmissions());
}
