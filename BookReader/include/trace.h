#pragma once
// trace.h — crash/hang-proof progress logging
//
// A hang on real hardware gives us nothing to work with: the screen freezes on
// whatever was last presented and the buttons stop responding, so there is no
// way to ask the program where it got to.
//
// TRACE() appends a line to /3DS/BookReader/trace.log and closes the file every
// time. That is deliberately slow — it means the log is complete and on disk
// even if the very next instruction wedges the console. Power-cycle, pull the
// SD card, and the last line tells you exactly how far execution got.
//
// Build with ENABLE_TRACE=0 to compile every call site out to nothing.

#ifndef ENABLE_TRACE
#define ENABLE_TRACE 1
#endif

#if ENABLE_TRACE

/// Append a printf-formatted line to the trace log.
void traceLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/// Truncate the log. Call once at startup so each run starts clean.
void traceReset();

#define TRACE(...)  traceLog(__VA_ARGS__)
#define TRACE_RESET() traceReset()

#else

#define TRACE(...)    ((void)0)
#define TRACE_RESET() ((void)0)

#endif
