#pragma once

#include <stdint.h>

#ifndef N3DS_DEBUG_BREADCRUMB_LOGGING
#define N3DS_DEBUG_BREADCRUMB_LOGGING 1
#endif

#if N3DS_DEBUG_BREADCRUMB_LOGGING
void N3DSDebugLog_init(void);
void N3DSDebugLog_close(void);
void N3DSDebugLog_event(const char* tag, const char* fmt, ...);
void N3DSDebugLog_setMarker(
    const char* stage,
    uint32_t frame,
    const char* roomName,
    int32_t detailA,
    int32_t detailB
);

// Forensic ring buffer: captures high-frequency VM breadcrumbs in RAM.
// Flushed to SD in batches; flushed to last_breadcrumb.txt on EVERY write
// so the exact final trace survives any crash/hang.
void N3DSDebugLog_ringWrite(const char* tag, const char* fmt, ...);
void N3DSDebugLog_ringFlush(void);
void N3DSDebugLog_installCrashHandler(void);
#else
#define N3DSDebugLog_init() ((void) 0)
#define N3DSDebugLog_close() ((void) 0)
#define N3DSDebugLog_event(...) ((void) 0)
#define N3DSDebugLog_setMarker(...) ((void) 0)
#define N3DSDebugLog_ringWrite(...) ((void) 0)
#define N3DSDebugLog_ringFlush() ((void) 0)
#define N3DSDebugLog_installCrashHandler() ((void) 0)
#endif