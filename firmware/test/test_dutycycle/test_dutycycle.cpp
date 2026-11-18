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

TEST_CASE("reset clears the accounting") {
  DutyCycleBudget budget;
  budget.commit(0, 36000);
  budget.reset();
  TEST_ASSERT_EQUAL_UINT32(0, budget.usedMs(0));
  TEST_ASSERT_EQUAL_UINT32(0, budget.transmissions());
}
