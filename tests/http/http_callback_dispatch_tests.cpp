/**
 * @file http_callback_dispatch_tests.cpp
 * @brief Regression guard for the HTTP callback dispatch sites that UBSan's
 *        function checker flagged.
 *
 * The handler chain (h_handler.c) and the dictionary walk (dictionary.c) both
 * call a function through a pointer.  Those calls were reported as calling a
 * function through a pointer to an incompatible type because the registered
 * callbacks were declared with different return/parameter types and the
 * mismatch was hidden behind a cast.  The casts are gone and the callbacks now
 * match the typedefs exactly, so these tests drive the real dispatch paths
 * under ASan+UBSan: a reintroduced mismatch trips -fsanitize=function here.
 *
 * The C++ side only drives the harness because the session headers use Blocks
 * and gtest needs C++.
 */

#include <gtest/gtest.h>

#include "http_callback_harness.h"

/* ── Dictionary preorder dispatch ───────────────────────────────────────── */

TEST(DictionaryPreorder, VisitsEveryEntryInKeyOrder) {
  struct visit_result result = {};
  EXPECT_EQ(HttpHarnessDictionaryPreorder(&result), 3);
  EXPECT_STREQ(result.first_key, "a");
  EXPECT_STREQ(result.first_value, "1");
}

TEST(DictionaryPreorder, MergeReachesTheVisitorThroughTheSameType) {
  char x[32] = {};
  char y[32] = {};
  EXPECT_EQ(HttpHarnessDictionaryMerge(x, sizeof(x), y, sizeof(y)), 0);
  EXPECT_STREQ(x, "10");
  EXPECT_STREQ(y, "20");
}

TEST(DictionaryPreorder, HardDupReachesTheVisitorThroughTheSameType) {
  char value[32] = {};
  EXPECT_EQ(HttpHarnessDictionaryHardDup(value, sizeof(value)), 0);
  EXPECT_STREQ(value, "v");
}

/* ── Handler chain dispatch ─────────────────────────────────────────────── */

TEST(HandlerDispatch, CallsHandlerThroughTypedPointer) {
  int calls = 0;
  EXPECT_EQ(HttpHarnessHandlerDispatch(&calls), 0);  // OCS_NOT_PROCESSED
  EXPECT_EQ(calls, 1);
}

TEST(HandlerDispatch, ReleasesPrivDataThroughTypedPointer) {
  /* onion_handler_free() calls handler->priv_data_free through the
   * onion_handler_private_data_free typedef; a mismatched destructor was the
   * other half of the reported handler problem. */
  int calls = 0;
  EXPECT_EQ(HttpHarnessPrivDataFree(&calls), 0);
  EXPECT_EQ(calls, 1);
}
