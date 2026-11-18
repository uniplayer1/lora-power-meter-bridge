#pragma once
// PlatformIO's Unity runner expects the suite to define main() and offers no
// TEST_CASE macro. This header adds both: test cases register themselves, and
// main runs everything that registered, with proper per-test reporting.
//
// Included from each suite as "../unity_case.h" (suites live one level down).

#include <unity.h>

#include <cstddef>

struct UnityCase {
  const char* name;
  void (*fn)();
};

inline UnityCase* unityCaseTable() {
  static UnityCase table[128];
  return table;
}

inline size_t& unityCaseCount() {
  static size_t count = 0;
  return count;
}

inline bool unityCaseRegister(const char* name, void (*fn)()) {
  if (unityCaseCount() < 128) {
    unityCaseTable()[unityCaseCount()++] = UnityCase{name, fn};
  }
  return true;
}

#define UNITY_CASE_CONCAT_(a, b) a##b
#define UNITY_CASE_CONCAT(a, b) UNITY_CASE_CONCAT_(a, b)

#define TEST_CASE(desc)                                                              \
  static void UNITY_CASE_CONCAT(unity_case_, __LINE__)(void);                        \
  static const bool UNITY_CASE_CONCAT(unity_reg_, __LINE__) __attribute__((used)) =   \
      unityCaseRegister(desc, UNITY_CASE_CONCAT(unity_case_, __LINE__));              \
  static void UNITY_CASE_CONCAT(unity_case_, __LINE__)(void)

// PlatformIO parses Unity's "file:line:name:PASS" lines with a pattern that
// allows neither spaces nor colons in the name. With readable descriptions every
// line failed to match, so each suite was reported as SKIPPED with 0 test cases
// (failures still failed the run through the exit code, but the report was
// useless). The reported name is the description with those characters replaced.
inline const char* unityCaseReportName(const char* desc) {
  static char buf[160];
  size_t i = 0;
  for (; desc[i] != '\0' && i < sizeof(buf) - 1; ++i) {
    const char c = desc[i];
    buf[i] = (c == ' ' || c == '\t') ? '_' : (c == ':' ? '-' : c);
  }
  buf[i] = '\0';
  return buf;
}

int main(int, char**) {
  UNITY_BEGIN();
  for (size_t i = 0; i < unityCaseCount(); ++i) {
    UnityDefaultTestRun(unityCaseTable()[i].fn, unityCaseReportName(unityCaseTable()[i].name),
                        static_cast<int>(i) + 1);
  }
  return UNITY_END();
}
