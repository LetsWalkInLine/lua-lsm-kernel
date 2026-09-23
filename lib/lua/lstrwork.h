/* SPDX-License-Identifier: GPL-2.0+ OR BSD-3-Clause */
#ifndef lstrwork_h
#define lstrwork_h

#include <linux/types.h>

/* Internal operation identifiers, not a Lua or kernel ABI. */
enum StrWorkAPI { SW_FIND, SW_MATCH, SW_GMATCH, SW_GSUB };

/* Initial internal limit; representative workload/performance review follows. */
#define LUA_STRING_WORK_LIMIT 65536ULL

typedef struct StrWorkBudget {
  u64 remaining;
} StrWorkBudget;

/* Exact exhaustion succeeds. The caller raises a Lua error on rejection. */
static inline bool strwork_debit (StrWorkBudget *b, u64 cost) {
  if (cost > b->remaining) {
    b->remaining = 0;
    return false;
  }
  b->remaining -= cost;
  return true;
}

#endif
