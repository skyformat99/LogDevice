/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
/* can be included multiple times */

#ifndef STAT_DEFINE
#error STAT_DEFINE() macro not defined
#define STAT_DEFINE(...)
#endif

// The number of tasks posted
STAT_DEFINE(storage_tasks_posted, SUM)
// The number of tasks executed
STAT_DEFINE(storage_tasks_executed, SUM)
// Number of microseconds spent in storage threads on executing this type of
// storage tasks
STAT_DEFINE(storage_thread_usec, SUM)

#undef STAT_DEFINE
#undef RESETTING_STATS
