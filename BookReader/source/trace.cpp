#include "trace.h"

#if ENABLE_TRACE

#include "common.h"
#include <3ds.h>
#include <cstdio>
#include <cstdarg>

namespace {
// Primary location, next to the rest of the app's data.
const char* kTracePath     = "sdmc:/3DS/BookReader/trace.log";

// Fallback at the root of the SD card. The primary path depends on
// /3DS/BookReader already existing and on the default device still being the
// SD card; the root is writable from the moment the filesystem is up, so if
// the primary open fails we still get a log rather than silently nothing.
const char* kTraceFallback = "sdmc:/BookReader_trace.log";

// Which path actually worked. Resolved on first use.
const char* g_path = nullptr;

// Kept open for the process's lifetime — see the comment on traceLog().
FILE* g_file = nullptr;

FILE* openTrace(const char* mode) {
    if (g_path) return fopen(g_path, mode);

    FILE* f = fopen(kTracePath, mode);
    if (f) { g_path = kTracePath; return f; }

    f = fopen(kTraceFallback, mode);
    if (f) { g_path = kTraceFallback; return f; }

    return nullptr;
}

// System ticks at startup, so timings are relative to the run rather than to
// some arbitrary epoch.
u64 g_startTick = 0;

float elapsedMs() {
    if (g_startTick == 0) return 0.0f;
    return (float)(svcGetSystemTick() - g_startTick) / (float)(SYSCLOCK_ARM11 / 1000);
}
} // namespace

void traceReset() {
    g_startTick = svcGetSystemTick();
    g_path      = nullptr;
    if (g_file) { fclose(g_file); g_file = nullptr; }

    FILE* f = openTrace("w");   // truncate for a clean run
    if (f) {
        fprintf(f, "--- BookReader trace ---\n");
        fflush(f);
        g_file = f;   // kept open — see traceLog()
    }
}

void traceLog(const char* fmt, ...) {
    // Previously opened, wrote, flushed, and *closed* the file on every
    // single call. fflush() alone already forces the write through to the
    // filesystem — closing added nothing to the "on disk before we return"
    // guarantee this exists for, but on this hardware's SD card each open
    // is a real FAT directory-entry/allocation-table operation, not just a
    // buffer op. With render tracing re-arming for 12 frames after *every*
    // page turn (see RENDER_TRACE_FRAMES in main.cpp), that was a dozen-plus
    // full open/close cycles on every single page turn — measured as
    // multi-second stalls on real hardware. Keeping the handle open for the
    // process's lifetime and just flushing removes that cost while keeping
    // the same durability guarantee.
    if (!g_file) g_file = openTrace("a");
    if (!g_file) return;

    fprintf(g_file, "[%8.1f ms] ", elapsedMs());

    va_list args;
    va_start(args, fmt);
    vfprintf(g_file, fmt, args);
    va_end(args);

    fputc('\n', g_file);
    fflush(g_file);
}

#endif // ENABLE_TRACE
