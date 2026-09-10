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

typedef struct StrWorkBudget {
  u64 remaining;
  bool exceeded;
} StrWorkBudget;

/* Pure arithmetic only: no production operation enforces this budget yet. */
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

#ifdef LUA_STRING_WORK_TEST
typedef struct StrWork { void *record; } StrWork;
static void strwork_begin (lua_State *L, StrWork *work, enum StrWorkAPI api);
static void strwork_charge (StrWork *work, enum StrWorkKind kind, u64 cost);
static void strwork_end (StrWork *work);
#define STRWORK_FIELD StrWork *work;
#define strwork_bind(ms, w) ((ms)->work = (w))
#define strwork_ms(ms, kind, cost) strwork_charge((ms)->work, kind, cost)
#else
/* No observer storage or counter operations in the ordinary/depth libraries. */
typedef struct StrWork {} StrWork;
#define strwork_begin(L, work, api) ((void)(work))
#define strwork_charge(work, kind, cost) ((void)0)
#define strwork_end(work) ((void)(work))
#define STRWORK_FIELD
#define strwork_bind(ms, w) ((void)0)
#define strwork_ms(ms, kind, cost) ((void)0)
#endif

#endif
