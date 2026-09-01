#include "n3ds_debug_log.h"

#if N3DS_DEBUG_BREADCRUMB_LOGGING

#include <3ds.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define N3DS_DEBUG_HISTORY_LOG_PATH "sdmc:/3ds/papyrus/crash_history.log"
#define N3DS_DEBUG_MARKER_LOG_PATH "sdmc:/3ds/papyrus/last_breadcrumb.txt"
#define N3DS_DEBUG_MARKER_BUFFER_SIZE 256

// ===[ Forensic ring buffer ]===
// VM-call tracing in RAM, batched to SD. last_breadcrumb.txt is rewritten on
// EVERY ring write (unbuffered) so the final trace survives a hard crash.
#define N3DS_DEBUG_RING_CAPACITY 192
#define N3DS_DEBUG_RING_LINE_MAX 200
#define N3DS_DEBUG_RING_FLUSH_INTERVAL 16
#define N3DS_DEBUG_RING_FULL_CAPTURE_CALLS 1500

typedef struct {
    char lines[N3DS_DEBUG_RING_CAPACITY][N3DS_DEBUG_RING_LINE_MAX];
    uint32_t head;   // next write slot
    uint32_t count;  // entries since last flush
    uint32_t total;  // monotonically increasing sequence
} N3DSDebugRing;

static FILE* gN3DSDebugHistoryLog = NULL;
static FILE* gN3DSDebugMarkerLog = NULL;
static u64 gN3DSDebugStartTick = 0;
static uint32_t gN3DSDebugEventCounter = 0;
static char gN3DSDebugLastMarker[N3DS_DEBUG_MARKER_BUFFER_SIZE];
static N3DSDebugRing gN3DSDebugRing;

static double N3DSDebugLog_ticksToMs(u64 ticks) {
    return (double) ticks * 1000.0 / (double) SYSCLOCK_ARM11;
}

// Integer-milliseconds formatting: printf with %f/%g pulls in newlib's _dtoa_r
// bigint machinery, which dies on a poisoned heap (heap corruption upstream
// turns the LOGGER into the crash victim and hides the real culprit).
// Log lines carry integer ms instead — no double formatting anywhere in the
// forensic path.
static uint32_t N3DSDebugLog_msInt(void) {
    if (gN3DSDebugStartTick == 0) return 0;
    u64 ticks = svcGetSystemTick() - gN3DSDebugStartTick;
    return (uint32_t) (ticks / (SYSCLOCK_ARM11 / 1000u));
}

static void N3DSDebugLog_openIfNeeded(void) {
    if (gN3DSDebugStartTick == 0) gN3DSDebugStartTick = svcGetSystemTick();

    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/papyrus", 0777);

    if (gN3DSDebugHistoryLog == NULL) {
        gN3DSDebugHistoryLog = fopen(N3DS_DEBUG_HISTORY_LOG_PATH, "a");
        if (gN3DSDebugHistoryLog != NULL) {
            setvbuf(gN3DSDebugHistoryLog, NULL, _IOLBF, 0);
        }
    }

    if (gN3DSDebugMarkerLog == NULL) {
        gN3DSDebugMarkerLog = fopen(N3DS_DEBUG_MARKER_LOG_PATH, "w");
        if (gN3DSDebugMarkerLog != NULL) {
            setvbuf(gN3DSDebugMarkerLog, NULL, _IONBF, 0);
        }
    }
}

void N3DSDebugLog_init(void) {
    N3DSDebugLog_openIfNeeded();
    N3DSDebugLog_event("boot", "debug log initialized");
}

void N3DSDebugLog_close(void) {
    N3DSDebugLog_ringFlush();
    if (gN3DSDebugMarkerLog != NULL) {
        fclose(gN3DSDebugMarkerLog);
        gN3DSDebugMarkerLog = NULL;
    }
    if (gN3DSDebugHistoryLog != NULL) {
        fclose(gN3DSDebugHistoryLog);
        gN3DSDebugHistoryLog = NULL;
    }
}

void N3DSDebugLog_event(const char* tag, const char* fmt, ...) {
    N3DSDebugLog_openIfNeeded();
    if (gN3DSDebugHistoryLog == NULL) return;

    char message[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt != NULL ? fmt : "", args);
    va_end(args);

    fprintf(
        gN3DSDebugHistoryLog,
        "[%u ms] #%06lu %-10s %s\n",
        N3DSDebugLog_msInt(),
        (unsigned long) ++gN3DSDebugEventCounter,
        tag != NULL ? tag : "event",
        message
    );
    fflush(gN3DSDebugHistoryLog);
}

void N3DSDebugLog_ringWrite(const char* tag, const char* fmt, ...) {
    char message[N3DS_DEBUG_RING_LINE_MAX];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt != NULL ? fmt : "", args);
    va_end(args);

    N3DSDebugRing* ring = &gN3DSDebugRing;
    char* slot = ring->lines[ring->head];
    snprintf(slot, N3DS_DEBUG_RING_LINE_MAX, "[%u ms] %s %s", N3DSDebugLog_msInt(), tag != NULL ? tag : "vm", message);
    ring->head = (ring->head + 1) % N3DS_DEBUG_RING_CAPACITY;
    if (ring->count < N3DS_DEBUG_RING_CAPACITY) ring->count++;
    ring->total++;

    // last_breadcrumb.txt: unbuffered rewrite, but only when the line actually
    // changes — repeating calls (busy loops) cost one strcmp, not an SD write.
    // SPEED FIX: the marker rewrite itself was still an SD round-trip per
    // DISTINCT call (hundreds/sec while typing). Throttle: at most one marker
    // flush per ring flush batch. Crash forensics still get the full ring.
    N3DSDebugLog_openIfNeeded();
    if (gN3DSDebugMarkerLog != NULL && ring->count == 1) {
        char marker[N3DS_DEBUG_RING_LINE_MAX + 16];
        int mlen = snprintf(marker, sizeof(marker), "ring#%lu %s", (unsigned long) ring->total, slot);
        if (mlen > 0 && (size_t) mlen != strlen(marker)) marker[mlen] = '\0';
        if (strncmp(marker, gN3DSDebugLastMarker, sizeof(gN3DSDebugLastMarker)) != 0) {
            snprintf(gN3DSDebugLastMarker, sizeof(gN3DSDebugLastMarker), "%s", marker);
            rewind(gN3DSDebugMarkerLog);
            // Truncate before writing: rewind alone leaves stale-tail bytes from
            // a previous LONGER line (the 661s marker mixed 3 writers this way).
            ftruncate(fileno(gN3DSDebugMarkerLog), 0);
            fprintf(gN3DSDebugMarkerLog, "%s\n", marker);
            fflush(gN3DSDebugMarkerLog);
        }
    }

    // Full-capture mode: during the boot/init window (first N ring writes =
    // global init + ROOM_INITIALIZE, where the doorway-adjacent crash lives)
    // flush EVERY call to SD. Cost is fine — room 0 issues only a few hundred
    // calls; after that we fall back to batched flushing.
    if (ring->count >= N3DS_DEBUG_RING_FLUSH_INTERVAL || ring->total <= N3DS_DEBUG_RING_FULL_CAPTURE_CALLS) {
        N3DSDebugLog_ringFlush();
    }
}

void N3DSDebugLog_ringFlush(void) {
    N3DSDebugRing* ring = &gN3DSDebugRing;
    if (ring->count == 0) return;
    N3DSDebugLog_openIfNeeded();
    if (gN3DSDebugHistoryLog == NULL) {
        ring->count = 0;
        return;
    }
    // Flush oldest-first starting at head - count
    uint32_t start = (ring->head + N3DS_DEBUG_RING_CAPACITY - ring->count) % N3DS_DEBUG_RING_CAPACITY;
    for (uint32_t i = 0; i < ring->count; i++) {
        fprintf(gN3DSDebugHistoryLog, "%s\n", ring->lines[(start + i) % N3DS_DEBUG_RING_CAPACITY]);
    }
    fflush(gN3DSDebugHistoryLog);
    ring->count = 0;
}

// ===[ Native crash hook ]===
// libctru's user exception handler needs UNITINFO set (dev units only), so on
// retail the ring + last_breadcrumb marker are the forensics: the marker is
// rewritten on every VM call (unbuffered SD write) and the ring flushes every
// N calls. The final line in last_breadcrumb.txt = the exact call in flight.
void N3DSDebugLog_installCrashHandler(void) {
    // Reserved: enable via threadOnException when running under UNITINFO.
}

void N3DSDebugLog_setMarker(
    const char* stage,
    uint32_t frame,
    const char* roomName,
    int32_t detailA,
    int32_t detailB
) {
    N3DSDebugLog_openIfNeeded();
    if (gN3DSDebugMarkerLog == NULL) return;

    char line[N3DS_DEBUG_MARKER_BUFFER_SIZE];
    snprintf(
        line,
        sizeof(line),
        "t=%ums frame=%lu stage=%s room=%s a=%ld b=%ld",
        N3DSDebugLog_msInt(),
        (unsigned long) frame,
        stage != NULL ? stage : "<null>",
        roomName != NULL ? roomName : "(none)",
        (long) detailA,
        (long) detailB
    );

    if (strncmp(line, gN3DSDebugLastMarker, sizeof(gN3DSDebugLastMarker)) == 0) {
        return;
    }
    snprintf(gN3DSDebugLastMarker, sizeof(gN3DSDebugLastMarker), "%s", line);

    rewind(gN3DSDebugMarkerLog);
    fprintf(gN3DSDebugMarkerLog, "%-240s\n", line);
    fflush(gN3DSDebugMarkerLog);
}

#endif