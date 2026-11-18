/**
 * @file        system/ppc_types.cpp
 * @brief       PPC thread-local state definitions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ppc/function.h>

// DS2 port: the export call counters register in this library's copy of ExportCallCounterHead() (an inline
// function's static: libmain.so has a copy of its own that stays empty). The app's watchdog reads the list here.
extern "C" __attribute__((visibility("default"))) bool rex_export_stats_on = false;
extern "C" __attribute__((visibility("default"))) void rex_export_stats_enable(bool on) { rex_export_stats_on = on; }

extern "C" __attribute__((visibility("default"))) rex::ppc::ExportCallCounter* rex_export_call_counter_head() {
  return rex::ppc::ExportCallCounterHead();
}
