#pragma once
// perf.h — cheap wall-clock timing for on-hardware measurement
//
// The handoff's working agreement is "prefer measuring over reasoning from
// symptoms". TRACE() is the existing tool for that, but it writes to the SD
// card and fflush()es every line, which on this hardware costs more than most
// of the things worth measuring — so timing anything with TRACE() alone
// perturbs the number it reports, and ENABLE_TRACE=0 (the release default)
// removes it entirely.
//
// These helpers cost a single system-tick read, so they can wrap a chapter
// load or a layout pass without changing what is being measured, and they
// stay compiled in regardless of ENABLE_TRACE. ReaderView uses them to report
// load timings through the on-screen toast (Settings ▸ Show Timings), which
// is the only measurement channel that survives a release build.

#include <3ds.h>

/// Current ARM11 system tick. Monotonic; wraps only after ~140 years.
inline u64 perfNow() { return svcGetSystemTick(); }

/// Milliseconds elapsed since `startTick`, as returned by perfNow().
inline float perfMsSince(u64 startTick) {
    return (float)(svcGetSystemTick() - startTick) /
           (float)(SYSCLOCK_ARM11 / 1000);
}
