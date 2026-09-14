/* SPDX-License-Identifier: GPL-2.0+ OR BSD-3-Clause */
#ifndef lstrwork_h
#define lstrwork_h

#include <linux/types.h>

/* Internal logical work units, not a Lua or kernel ABI. */
enum StrWorkKind {
  SW_OPERATION, SW_START, SW_ENTER, SW_DISPATCH, SW_RETRY,
  SW_SUBJECT, SW_PATTERN, SW_CLASS, SW_CAPTURE_SLOT, SW_COMPARE,
  SW_CLASSIFY, SW_PLAIN_SCAN, SW_PLAIN_CANDIDATE, SW_CAPTURE_EMIT,
  SW_REPLACEMENT, SW_REPLACEMENT_SCAN, SW_CALLBACK, SW_OUTPUT,
  SW_KINDS
};

enum StrWorkAPI { SW_FIND, SW_MATCH, SW_GMATCH, SW_GSUB };

/* Initial internal limit; representative workload/performance review follows. */
#define LUA_STRING_WORK_LIMIT 65536ULL

typedef struct StrWorkBudget {
  u64 remaining;
  bool exceeded;
} StrWorkBudget;

/* Exact exhaustion succeeds; only an unaffordable request sets exceeded. */
static inline bool strwork_debit (StrWorkBudget *b, u64 cost) {
  if (b->exceeded) return false;
  if (cost > b->remaining) {
    b->remaining = 0;
    b->exceeded = true;
    return false;
  }
  b->remaining -= cost;
  return true;
}

static inline bool strwork_accumulate (u64 *total, u64 cost) {
  if (cost > ~(u64)0 - *total) {
    *total = ~(u64)0;
    return true;
  }
  *total += cost;
  return false;
}

#endif
