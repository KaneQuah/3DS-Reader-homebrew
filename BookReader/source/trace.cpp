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
    FILE* f = openTrace("w");
    if (f) {
        fprintf(f, "--- BookReader trace ---\n");
        fflush(f);
        fclose(f);
    }
}

void traceLog(const char* fmt, ...) {
    // Open/append/close every call. Slow, but it guarantees the line is on the
    // card before we return — which is the entire point when the next thing the
    // program does might be to hang.
    FILE* f = openTrace("a");
    if (!f) return;

    fprintf(f, "[%8.1f ms] ", elapsedMs());

    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);

    fputc('\n', f);
    fflush(f);
    fclose(f);
}

#endif // ENABLE_TRACE
