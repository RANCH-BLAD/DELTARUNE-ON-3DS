#include "n3ds_audio_system.h"
#include "n3ds_debug_log.h"
#include "n3ds_platform_config.h"

#include "../data_win.h"
#include "../utils.h"

#include <3ds.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define N3DS_ENABLE_LOGGING 0

#if !N3DS_ENABLE_LOGGING
#define fprintf(...) ((int) 0)
#endif

#define N3DS_MAX_NDSP_CHANNELS 24
#define N3DS_MAX_SOUND_INSTANCES 24
#define N3DS_RESERVED_STREAM_INSTANCES 4
#define N3DS_SOUND_INSTANCE_ID_BASE 100000
#define N3DS_AUDIO_STREAM_INDEX_BASE 200000
#define N3DS_MAX_STREAMS 64
#define N3DS_STREAM_BUFFER_COUNT 3
#define N3DS_STREAM_CHUNK_SAMPLES (14u * 256u)
#define N3DS_STREAM_ADPCM_CACHE_FRAMES 256u
#define N3DS_STREAM_WORKER_STACK_SIZE (32u * 1024u)
#define N3DS_STREAM_WORKER_PRIORITY 0x31
#define N3DS_STREAM_WORKER_CORE_ID 1
#define N3DS_STREAM_WORKER_CPU_LIMIT 20u
#define N3DS_AUDIO_FILE_BUFFER_SIZE (64u * 1024u)
#define N3DS_ENABLE_STREAM_WORKER 0
#define N3DS_EAGER_SFX_PRELOAD 0
#define N3DS_MAX_PRELOADED_SFX_BYTES_NEW3DS (15u * 1024u * 1024u)
#define N3DS_MAX_PRELOADED_SFX_BYTES_OLD3DS (15u * 1024u * 1024u)
#define N3DS_ROOM_PREWARM_SOUND_COUNT_NEW3DS 2u
#define N3DS_ROOM_PREWARM_BYTE_BUDGET_NEW3DS (192u * 1024u)
#define N3DS_BACKGROUND_PREWARM_SOUND_COUNT_NEW3DS 0u
#define N3DS_BACKGROUND_PREWARM_BYTE_BUDGET_NEW3DS 0u
#define N3DS_ROOM_PREWARM_SOUND_COUNT_OLD3DS 1u
#define N3DS_ROOM_PREWARM_BYTE_BUDGET_OLD3DS (96u * 1024u)
#define N3DS_BACKGROUND_PREWARM_SOUND_COUNT_OLD3DS 0u
#define N3DS_BACKGROUND_PREWARM_BYTE_BUDGET_OLD3DS 0u
#define N3DS_FORCE_PCM_BCWAV_PLAYBACK 0
#define N3DS_ROMFS_AUDIO_BASE "romfs:/audio"
#define N3DS_ROMFS_MUSIC_BASE "romfs:/"
#define N3DS_SDMC_AUDIO_BASE "sdmc:/3ds/papyrus/audio"
#define N3DS_SDMC_MUSIC_BASE "sdmc:/3ds/papyrus"
#define N3DS_MAX_MISSING_AUDIO_PATHS 512
#define N3DS_SOUND_BANK_MAGIC 0x314B4253u /* SBK1 */
#define N3DS_SOUND_BANK_VERSION_BCWAV 1u
#define N3DS_SOUND_BANK_VERSION_PCM16 2u
#define N3DS_SOUND_BANK_ENTRY_CHANNEL_MASK 0x000000FFu
#define N3DS_SOUND_BANK_ENTRY_FLAG_LOOP 0x00000100u
#define N3DS_SOUND_BANK_ENTRY_FLAG_PCM16 0x00000200u
#define N3DS_PCM_END_FADE_FRAMES 256u

typedef struct {
    uint8_t predictorScale;
    int16_t yn1;
    int16_t yn2;
} N3DSDspContext;

typedef struct {
    uint16_t coefs[16];
    uint32_t dataOffset;
    uint32_t dataSize;
    N3DSDspContext startContext;
    N3DSDspContext loopContext;
} N3DSBcwavChannel;

typedef struct {
    uint32_t sampleRate;
    uint32_t sampleCount;
    uint32_t loopStart;
    uint32_t loopEnd;
    bool loop;
    uint8_t channelCount;
    N3DSBcwavChannel channels[2];
} N3DSBcwav;

typedef struct {
    bool active;
    char* path;
    uint8_t* blobData;
    uint32_t blobSize;
    N3DSBcwav bcwav;
    float gain;
    float pitch;
    uint32_t refCount;
} N3DSStreamEntry;

typedef struct {
    uint32_t nextFrameIndex;
    uint8_t predictorScale;
    int16_t hist1;
    int16_t hist2;
} N3DSStreamDecodeState;

typedef struct {
    uint32_t currentSample;
    uint32_t pendingSamples;
    uint32_t pendingOffset;
    int16_t pendingPcm[14 * 2];
    N3DSStreamDecodeState decode[2];
    uint32_t fileCacheFrameBase[2];
    uint32_t fileCacheFrameCount[2];
    uint8_t fileCache[2][8u * N3DS_STREAM_ADPCM_CACHE_FRAMES];
} N3DSStreamFillCursor;

typedef struct {
    bool requested;
    bool inProgress;
    bool ready;
    bool success;
    int bufferIndex;
    uint32_t generation;
    uint32_t bufferStartSample;
    uint32_t sampleCount;
    N3DSStreamFillCursor nextCursor;
    ndspAdpcmData adpcmStates[2];
} N3DSAsyncStreamFill;

typedef struct {
    bool active;
    bool paused;
    bool loop;
    bool isStream;
    bool streamFinished;
    int32_t soundIndex;
    int32_t instanceId;
    int channelId;
    int secondaryChannelId;
    float gain;
    float pitch;
    float baseRate;
    uint32_t sampleCount;
    uint32_t sampleRate;
    uint8_t channelCount;
    bool useNativeAdpcm;
    ndspWaveBuf waveBufs[N3DS_STREAM_BUFFER_COUNT];
    ndspWaveBuf secondaryWaveBufs[N3DS_STREAM_BUFFER_COUNT];
    ndspAdpcmData adpcmStates[N3DS_STREAM_BUFFER_COUNT * 2];
    uint32_t bufferStartSample[N3DS_STREAM_BUFFER_COUNT];
    uint8_t* streamAdpcm[2][N3DS_STREAM_BUFFER_COUNT];
    int16_t* streamPcm[N3DS_STREAM_BUFFER_COUNT];
    int16_t* pcmData;
    FILE* streamFile;
    const uint8_t* streamBlob;
    uint32_t streamBlobSize;
    bool ownsStreamBlob;
    bool streamBlobLinear;
    N3DSBcwav bcwav;
    N3DSStreamDecodeState decode[2];
    uint32_t fileCacheFrameBase[2];
    uint32_t fileCacheFrameCount[2];
    uint8_t fileCache[2][8u * N3DS_STREAM_ADPCM_CACHE_FRAMES];
    uint32_t currentSample;
    uint32_t pendingSamples;
    uint32_t pendingOffset;
    int16_t pendingPcm[14 * 2];
    uint32_t streamGeneration;
    N3DSAsyncStreamFill asyncFill;
    u64 playbackStartTick;
    u64 playbackReleaseTick;
} N3DSSoundInstance;

typedef struct {
    bool attempted;
    bool available;
    bool headerAttempted;
    bool headerAvailable;
    bool pathAttempted;
    bool blobOwned;
    char* path;
    uint8_t* blob;
    uint8_t* nativeBlob;
    uint32_t blobSize;
    N3DSBcwav bcwav;
} N3DSCachedSound;

typedef struct {
    uint32_t offset;
    uint32_t size;
    uint32_t sampleRate;
    uint32_t sampleCount;
    uint32_t flags;
} N3DSPackedSoundBankEntry;

struct N3DSAudioSystem {
    AudioSystem base;
    FileSystem* fileSystem;
    bool initialized;
    float masterGain;
    N3DSStreamEntry streams[N3DS_MAX_STREAMS];
    N3DSSoundInstance instances[N3DS_MAX_SOUND_INSTANCES];
    N3DSCachedSound* cachedSounds;
    uint32_t cachedSoundCount;
    uint32_t cachedSoundBytes;
    uint32_t cachedSoundPrewarmCursor;
    uint32_t maxCachedSoundBytes;
    uint32_t roomPrewarmSoundCount;
    uint32_t roomPrewarmByteBudget;
    uint32_t backgroundPrewarmSoundCount;
    uint32_t backgroundPrewarmByteBudget;
    uint32_t pendingPrewarmSoundCount;
    uint32_t pendingPrewarmByteBudget;
    bool isNew3DS;
    int32_t lastResolveFailureSound;
    bool ndspChannelInUse[N3DS_MAX_NDSP_CHANNELS];
    uint8_t nextNdspChannel;
    LightLock lock;
    LightLock missingPathLock;
    CondVar workerCond;
    LightEvent workerEvent;
    Thread workerThread;
    bool workerStop;
    bool workerEnabled;
    uint8_t* packedSoundBankData;
    uint32_t packedSoundBankSize;
    N3DSPackedSoundBankEntry* packedSoundBankEntries;
    uint32_t packedSoundBankEntryCount;
    uint32_t packedSoundBankVersion;
};

static char* gN3DSMissingAudioPaths[N3DS_MAX_MISSING_AUDIO_PATHS];
static uint32_t gN3DSMissingAudioPathCount = 0;

static void N3DSAudio_cancelAsyncFillLocked(N3DSAudioSystem* audio, N3DSSoundInstance* inst);
static bool N3DSAudio_hasActiveStreamPlaybackLocked(const N3DSAudioSystem* audio);
static char* N3DSAudio_tryResolveSfxCandidate(N3DSAudioSystem* audio, const char* candidate);

static void N3DSAudio_captureFillCursor(const N3DSSoundInstance* inst, N3DSStreamFillCursor* cursor) {
    if (inst == NULL || cursor == NULL) return;
    memset(cursor, 0, sizeof(*cursor));
    cursor->currentSample = inst->currentSample;
    cursor->pendingSamples = inst->pendingSamples;
    cursor->pendingOffset = inst->pendingOffset;
    memcpy(cursor->pendingPcm, inst->pendingPcm, sizeof(cursor->pendingPcm));
    memcpy(cursor->decode, inst->decode, sizeof(cursor->decode));
    memcpy(cursor->fileCacheFrameBase, inst->fileCacheFrameBase, sizeof(cursor->fileCacheFrameBase));
    memcpy(cursor->fileCacheFrameCount, inst->fileCacheFrameCount, sizeof(cursor->fileCacheFrameCount));
    memcpy(cursor->fileCache, inst->fileCache, sizeof(cursor->fileCache));
}

static void N3DSAudio_applyFillCursor(N3DSSoundInstance* inst, const N3DSStreamFillCursor* cursor) {
    if (inst == NULL || cursor == NULL) return;
    inst->currentSample = cursor->currentSample;
    inst->pendingSamples = cursor->pendingSamples;
    inst->pendingOffset = cursor->pendingOffset;
    memcpy(inst->pendingPcm, cursor->pendingPcm, sizeof(inst->pendingPcm));
    memcpy(inst->decode, cursor->decode, sizeof(inst->decode));
    memcpy(inst->fileCacheFrameBase, cursor->fileCacheFrameBase, sizeof(inst->fileCacheFrameBase));
    memcpy(inst->fileCacheFrameCount, cursor->fileCacheFrameCount, sizeof(inst->fileCacheFrameCount));
    memcpy(inst->fileCache, cursor->fileCache, sizeof(inst->fileCache));
}

static uint16_t N3DSAudio_readU16(const uint8_t* ptr) {
    return (uint16_t) (ptr[0] | (ptr[1] << 8));
}

static uint32_t N3DSAudio_readU32(const uint8_t* ptr) {
    return (uint32_t) ptr[0] |
        ((uint32_t) ptr[1] << 8) |
        ((uint32_t) ptr[2] << 16) |
        ((uint32_t) ptr[3] << 24);
}

static void N3DSAudio_softenPcmTail(int16_t* pcm, uint32_t sampleCount, uint8_t channelCount) {
    if (pcm == NULL || sampleCount == 0 || channelCount == 0) return;

    uint32_t fadeFrames = sampleCount;
    if (fadeFrames > N3DS_PCM_END_FADE_FRAMES) fadeFrames = N3DS_PCM_END_FADE_FRAMES;
    if (fadeFrames == 0) return;
    if (fadeFrames == 1) {
        for (uint8_t channel = 0; channel < channelCount; channel++) {
            pcm[channel] = 0;
        }
        return;
    }

    uint32_t startFrame = sampleCount - fadeFrames;
    for (uint32_t frame = 0; frame < fadeFrames; frame++) {
        uint32_t gainNumerator = fadeFrames - 1u - frame;
        uint32_t gainDenominator = fadeFrames - 1u;
        uint32_t frameOffset = (startFrame + frame) * channelCount;
        for (uint8_t channel = 0; channel < channelCount; channel++) {
            int32_t sample = pcm[frameOffset + channel];
            pcm[frameOffset + channel] = (int16_t) ((sample * (int32_t) gainNumerator) / (int32_t) gainDenominator);
        }
    }
}

static int16_t N3DSAudio_readS16(const uint8_t* ptr) {
    return (int16_t) N3DSAudio_readU16(ptr);
}

static int N3DSAudio_signExtend4(int nibble) {
    return (nibble & 0x8) ? (nibble - 16) : nibble;
}

static void N3DSAudio_configureFileBuffer(FILE* file) {
    if (file == NULL) return;
    setvbuf(file, NULL, _IOFBF, N3DS_AUDIO_FILE_BUFFER_SIZE);
}

static bool N3DSAudio_readFileFully(const char* path, uint8_t** outData, uint32_t* outSize) {
    *outData = NULL;
    *outSize = 0;

    FILE* file = fopen(path, "rb");
    if (file == NULL) return false;
    N3DSAudio_configureFileBuffer(file);

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size <= 0) {
        fclose(file);
        return false;
    }

    uint8_t* data = safeMalloc((size_t) size);
    if (fread(data, 1, (size_t) size, file) != (size_t) size) {
        fclose(file);
        free(data);
        return false;
    }

    fclose(file);
    *outData = data;
    *outSize = (uint32_t) size;
    return true;
}

static bool N3DSAudio_readFileFullyLinear(const char* path, uint8_t** outData, uint32_t* outSize) {
    *outData = NULL;
    *outSize = 0;

    FILE* file = fopen(path, "rb");
    if (file == NULL) return false;
    N3DSAudio_configureFileBuffer(file);

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size <= 0) {
        fclose(file);
        return false;
    }

    uint8_t* data = linearAlloc((size_t) size);
    if (data == NULL) {
        fclose(file);
        return false;
    }
    if (fread(data, 1, (size_t) size, file) != (size_t) size) {
        fclose(file);
        linearFree(data);
        return false;
    }

    fclose(file);
    DSP_FlushDataCache(data, (size_t) size);
    *outData = data;
    *outSize = (uint32_t) size;
    return true;
}

static bool N3DSAudio_getFileSize(FILE* file, uint32_t* outSize) {
    if (file == NULL || outSize == NULL) return false;

    if (fseek(file, 0, SEEK_END) != 0) return false;
    long size = ftell(file);
    if (size <= 0) {
        fseek(file, 0, SEEK_SET);
        return false;
    }
    if (fseek(file, 0, SEEK_SET) != 0) return false;

    *outSize = (uint32_t) size;
    return true;
}

static uint8_t* N3DSAudio_cloneBlobToLinear(const uint8_t* data, uint32_t size) {
    if (data == NULL || size == 0) return NULL;
    uint8_t* linearData = linearAlloc(size);
    if (linearData == NULL) return NULL;
    memcpy(linearData, data, size);
    DSP_FlushDataCache(linearData, size);
    return linearData;
}

static const N3DSPackedSoundBankEntry* N3DSAudio_getPackedSoundEntry(const N3DSAudioSystem* audio, int32_t soundIndex) {
    if (audio == NULL || soundIndex < 0 || (uint32_t) soundIndex >= audio->packedSoundBankEntryCount) return NULL;
    if (audio->packedSoundBankEntries == NULL) return NULL;
    return &audio->packedSoundBankEntries[soundIndex];
}

static bool N3DSAudio_packedSoundEntryIsPcm16(const N3DSPackedSoundBankEntry* entry) {
    return entry != NULL && (entry->flags & N3DS_SOUND_BANK_ENTRY_FLAG_PCM16) != 0;
}

static uint8_t N3DSAudio_packedSoundEntryChannelCount(const N3DSPackedSoundBankEntry* entry) {
    return entry != NULL ? (uint8_t) (entry->flags & N3DS_SOUND_BANK_ENTRY_CHANNEL_MASK) : 0;
}

static bool N3DSAudio_getPackedSoundBlob(
    const N3DSAudioSystem* audio,
    int32_t soundIndex,
    const uint8_t** outBlob,
    uint32_t* outSize
) {
    if (outBlob != NULL) *outBlob = NULL;
    if (outSize != NULL) *outSize = 0;
    if (audio == NULL || soundIndex < 0 || (uint32_t) soundIndex >= audio->packedSoundBankEntryCount) return false;
    if (audio->packedSoundBankData == NULL || audio->packedSoundBankEntries == NULL) return false;

    const N3DSPackedSoundBankEntry* entry = &audio->packedSoundBankEntries[soundIndex];
    if (entry->size == 0) return false;
    if (entry->offset >= audio->packedSoundBankSize) return false;
    if (entry->size > audio->packedSoundBankSize - entry->offset) return false;

    if (outBlob != NULL) *outBlob = audio->packedSoundBankData + entry->offset;
    if (outSize != NULL) *outSize = entry->size;
    return true;
}

static bool N3DSAudio_loadPackedSoundBank(N3DSAudioSystem* audio) {
    if (audio == NULL) return false;
    if (audio->packedSoundBankData != NULL) return true;

    char* path = N3DSAudio_tryResolveSfxCandidate(audio, "sound_bank.bin");
    if (path == NULL) return false;

    uint8_t* bankData = NULL;
    uint32_t bankSize = 0;
    N3DSPackedSoundBankEntry* entries = NULL;
    bool loaded = false;

    if (!N3DSAudio_readFileFully(path, &bankData, &bankSize)) goto cleanup;
    if (bankSize < 16u) goto cleanup;
    if (N3DSAudio_readU32(bankData + 0u) != N3DS_SOUND_BANK_MAGIC) goto cleanup;
    uint32_t version = N3DSAudio_readU32(bankData + 4u);
    if (version != N3DS_SOUND_BANK_VERSION_BCWAV && version != N3DS_SOUND_BANK_VERSION_PCM16) goto cleanup;

    uint32_t entryCount = N3DSAudio_readU32(bankData + 8u);
    uint32_t dataOffset = N3DSAudio_readU32(bankData + 12u);
    uint32_t entryStride = version >= N3DS_SOUND_BANK_VERSION_PCM16 ? 20u : 8u;
    uint32_t tableBytes = entryCount * entryStride;
    if (entryCount > 0 && tableBytes / entryStride != entryCount) goto cleanup;
    if (dataOffset < 16u || dataOffset > bankSize) goto cleanup;
    if (16u + tableBytes > dataOffset) goto cleanup;

    entries = safeCalloc(entryCount, sizeof(N3DSPackedSoundBankEntry));
    repeat(entryCount, i) {
        uint32_t tableOffset = 16u + (uint32_t) i * entryStride;
        uint32_t offset = N3DSAudio_readU32(bankData + tableOffset + 0u);
        uint32_t size = N3DSAudio_readU32(bankData + tableOffset + 4u);
        if (size != 0) {
            if (offset < dataOffset || offset >= bankSize) goto cleanup;
            if (size > bankSize - offset) goto cleanup;
        }
        entries[i].offset = offset;
        entries[i].size = size;
        if (version >= N3DS_SOUND_BANK_VERSION_PCM16) {
            entries[i].sampleRate = N3DSAudio_readU32(bankData + tableOffset + 8u);
            entries[i].sampleCount = N3DSAudio_readU32(bankData + tableOffset + 12u);
            entries[i].flags = N3DSAudio_readU32(bankData + tableOffset + 16u);
        }
    }

    audio->packedSoundBankData = bankData;
    audio->packedSoundBankSize = bankSize;
    audio->packedSoundBankEntries = entries;
    audio->packedSoundBankEntryCount = entryCount;
    audio->packedSoundBankVersion = version;
    bankData = NULL;
    entries = NULL;
    loaded = true;
    N3DSDebugLog_event(
        "audio_bank",
        "loaded version=%lu entries=%lu sizeKB=%lu path=%s",
        (unsigned long) version,
        (unsigned long) entryCount,
        (unsigned long) (bankSize / 1024u),
        path
    );

cleanup:
    free(bankData);
    free(entries);
    free(path);
    return loaded;
}

static bool N3DSAudio_fileExists(N3DSAudioSystem* audio, const char* path) {
    if (path == NULL) return false;

    if (audio != NULL) LightLock_Lock(&audio->missingPathLock);
    repeat(gN3DSMissingAudioPathCount, i) {
        if (strcmp(gN3DSMissingAudioPaths[i], path) == 0) {
            if (audio != NULL) LightLock_Unlock(&audio->missingPathLock);
            return false;
        }
    }
    if (audio != NULL) LightLock_Unlock(&audio->missingPathLock);

    struct stat st;
    if (stat(path, &st) != 0) {
        if (audio != NULL) LightLock_Lock(&audio->missingPathLock);
        if (gN3DSMissingAudioPathCount < N3DS_MAX_MISSING_AUDIO_PATHS) {
            gN3DSMissingAudioPaths[gN3DSMissingAudioPathCount++] = safeStrdup(path);
        }
        if (audio != NULL) LightLock_Unlock(&audio->missingPathLock);
        return false;
    }
    return true;
}

static bool N3DSAudio_hasScheme(const char* path) {
    return path != NULL && strstr(path, ":/") != NULL;
}

static bool N3DSAudio_pathStartsWith(const char* path, const char* prefix) {
    if (path == NULL || prefix == NULL) return false;
    size_t prefixLen = strlen(prefix);
    return strncmp(path, prefix, prefixLen) == 0;
}

static char* N3DSAudio_joinPath(const char* base, const char* relativePath) {
    if (base == NULL || relativePath == NULL) return NULL;
    size_t baseLen = strlen(base);
    size_t relLen = strlen(relativePath);
    bool needSlash = baseLen > 0 && base[baseLen - 1] != '/' && relLen > 0 && relativePath[0] != '/';
    char* result = safeMalloc(baseLen + relLen + (needSlash ? 2 : 1));
    memcpy(result, base, baseLen);
    size_t cursor = baseLen;
    if (needSlash) result[cursor++] = '/';
    memcpy(result + cursor, relativePath, relLen);
    result[cursor + relLen] = '\0';
    return result;
}

static char* N3DSAudio_tryResolveCandidateInBases(N3DSAudioSystem* audio, const char* candidate, const char* const* bases, size_t baseCount) {
    (void) audio;
    if (candidate == NULL || candidate[0] == '\0') return NULL;
    if (N3DSAudio_hasScheme(candidate)) {
        return N3DSAudio_fileExists(audio, candidate) ? safeStrdup(candidate) : NULL;
    }

    repeat(baseCount, i) {
        char* resolved = N3DSAudio_joinPath(bases[i], candidate);
        if (resolved != NULL && N3DSAudio_fileExists(audio, resolved)) {
            return resolved;
        }
        free(resolved);
    }
    return NULL;
}

static char* N3DSAudio_tryResolveCandidate(N3DSAudioSystem* audio, const char* candidate) {
    const char* bases[] = {
        N3DS_ROMFS_AUDIO_BASE,
        N3DS_ROMFS_MUSIC_BASE,
        N3DS_SDMC_AUDIO_BASE,
        N3DS_SDMC_MUSIC_BASE,
    };
    return N3DSAudio_tryResolveCandidateInBases(audio, candidate, bases, 4);
}

static char* N3DSAudio_tryResolveMusicCandidate(N3DSAudioSystem* audio, const char* candidate) {
    const char* bases[] = {
        N3DS_ROMFS_MUSIC_BASE,
        N3DS_ROMFS_AUDIO_BASE,
        N3DS_SDMC_MUSIC_BASE,
        N3DS_SDMC_AUDIO_BASE,
    };
    return N3DSAudio_tryResolveCandidateInBases(audio, candidate, bases, 4);
}

static char* N3DSAudio_tryResolveSfxCandidate(N3DSAudioSystem* audio, const char* candidate) {
    const char* bases[] = {
        N3DS_ROMFS_AUDIO_BASE,
        N3DS_SDMC_AUDIO_BASE,
    };
    return N3DSAudio_tryResolveCandidateInBases(audio, candidate, bases, 2);
}

static char* N3DSAudio_resolveStreamPath(N3DSAudioSystem* audio, const char* name) {
    if (name == NULL || name[0] == '\0') return NULL;

    char* resolved = N3DSAudio_tryResolveMusicCandidate(audio, name);
    if (resolved != NULL) return resolved;

    char candidate[512];
    if (strchr(name, '.') == NULL) {
        snprintf(candidate, sizeof(candidate), "%s.bcwav", name);
        return N3DSAudio_tryResolveMusicCandidate(audio, candidate);
    }

    const char* slash = strrchr(name, '/');
    const char* base = slash != NULL ? slash + 1 : name;
    const char* dot = strrchr(base, '.');
    if (dot == NULL || dot <= base) return NULL;

    size_t dirLen = slash != NULL ? (size_t) (slash - name + 1) : 0;
    size_t baseLen = (size_t) (dot - base);

    if (dirLen + baseLen + 7 < sizeof(candidate)) {
        memcpy(candidate, name, dirLen);
        memcpy(candidate + dirLen, base, baseLen);
        memcpy(candidate + dirLen + baseLen, ".bcwav", 7);
        resolved = N3DSAudio_tryResolveMusicCandidate(audio, candidate);
        if (resolved != NULL) return resolved;
    }

    if (baseLen + 7 < sizeof(candidate)) {
        memcpy(candidate, base, baseLen);
        memcpy(candidate + baseLen, ".bcwav", 7);
        resolved = N3DSAudio_tryResolveMusicCandidate(audio, candidate);
        if (resolved != NULL) return resolved;
    }

    return NULL;
}

static char* N3DSAudio_resolveSoundPath(N3DSAudioSystem* audio, const Sound* sound, int32_t soundIndex);
static N3DSCachedSound* N3DSAudio_getCachedSoundIfAvailable(N3DSAudioSystem* audio, int32_t soundIndex);
static const char* N3DSAudio_getResolvedCachedSoundPath(N3DSAudioSystem* audio, int32_t soundIndex, const Sound* sound);
static bool N3DSAudio_getCachedSoundHeader(N3DSAudioSystem* audio, int32_t soundIndex, const Sound* sound, N3DSBcwav* out);
static uint8_t* N3DSAudio_getCachedNativeBlob(N3DSCachedSound* cachedSound);
static bool N3DSAudio_primeNativeAdpcm(N3DSSoundInstance* inst);
static void N3DSAudio_prewarmSoundCache(N3DSAudioSystem* audio, uint32_t maxSounds, uint32_t byteBudget);
static void N3DSAudio_requestCachePrewarmLocked(N3DSAudioSystem* audio, uint32_t maxSounds, uint32_t byteBudget);
static bool N3DSAudio_loadPackedSoundBank(N3DSAudioSystem* audio);
static bool N3DSAudio_getPackedSoundBlob(
    const N3DSAudioSystem* audio,
    int32_t soundIndex,
    const uint8_t** outBlob,
    uint32_t* outSize
);

static bool N3DSAudio_stringContainsIgnoreCase(const char* haystack, const char* needle) {
    if (haystack == NULL || needle == NULL || needle[0] == '\0') return false;
    size_t needleLen = strlen(needle);
    for (const char* cursor = haystack; *cursor != '\0'; ++cursor) {
        size_t matched = 0;
        while (matched < needleLen) {
            char a = cursor[matched];
            if (a == '\0') return false;
            char b = needle[matched];
            if (a >= 'A' && a <= 'Z') a = (char) (a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char) (b - 'A' + 'a');
            if (a != b) break;
            matched++;
        }
        if (matched == needleLen) return true;
    }
    return false;
}

static bool N3DSAudio_nameLooksLikeSfx(const char* value) {
    if (value == NULL || value[0] == '\0') return false;

    const char* base = strrchr(value, '/');
    const char* backslash = strrchr(value, '\\');
    if (backslash != NULL && (base == NULL || backslash > base)) base = backslash;
    base = base != NULL ? base + 1 : value;

    return strncmp(base, "mus_sfx", 7) == 0 ||
        strncmp(base, "sfx_", 4) == 0 ||
        strncmp(base, "snd_", 4) == 0 ||
        N3DSAudio_stringContainsIgnoreCase(base, "_sfx") ||
        N3DSAudio_stringContainsIgnoreCase(base, "sfx_");
}

static bool N3DSAudio_nameLooksLikeMusic(const char* value) {
    if (value == NULL || value[0] == '\0') return false;

    const char* base = strrchr(value, '/');
    const char* backslash = strrchr(value, '\\');
    if (backslash != NULL && (base == NULL || backslash > base)) base = backslash;
    base = base != NULL ? base + 1 : value;
    if (N3DSAudio_nameLooksLikeSfx(base)) return false;
    if (strncmp(base, "mus_", 4) == 0 || strncmp(base, "bgm_", 4) == 0) return true;
    return N3DSAudio_stringContainsIgnoreCase(base, "music");
}

static bool N3DSAudio_soundLooksLikeSfx(const Sound* sound) {
    if (sound == NULL) return false;
    return N3DSAudio_nameLooksLikeSfx(sound->name) ||
        N3DSAudio_nameLooksLikeSfx(sound->file) ||
        N3DSAudio_stringContainsIgnoreCase(sound->type, "sfx") ||
        N3DSAudio_stringContainsIgnoreCase(sound->type, "effect");
}

static bool N3DSAudio_soundLooksLikeMusic(const Sound* sound) {
    if (sound == NULL) return false;
    if (N3DSAudio_soundLooksLikeSfx(sound)) return false;
    return N3DSAudio_nameLooksLikeMusic(sound->name) ||
        N3DSAudio_nameLooksLikeMusic(sound->file) ||
        N3DSAudio_stringContainsIgnoreCase(sound->type, "music") ||
        N3DSAudio_stringContainsIgnoreCase(sound->type, "stream");
}

static bool N3DSAudio_pathLooksLikeBundledMusic(const char* path) {
    if (path == NULL) return false;
    if (N3DSAudio_pathStartsWith(path, "romfs:/audio/")) return false;
    if (N3DSAudio_pathStartsWith(path, "sdmc:/3ds/papyrus/audio/")) return false;
    return N3DSAudio_pathStartsWith(path, "romfs:/") || N3DSAudio_pathStartsWith(path, "sdmc:/3ds/papyrus/");
}

static bool N3DSAudio_extractBaseNameNoExt(const char* value, char* out, size_t outSize) {
    if (out == NULL || outSize == 0) return false;
    out[0] = '\0';
    if (value == NULL || value[0] == '\0') return false;

    const char* base = strrchr(value, '/');
    const char* backslash = strrchr(value, '\\');
    if (backslash != NULL && (base == NULL || backslash > base)) base = backslash;
    base = (base != NULL) ? base + 1 : value;
    if (base[0] == '\0') return false;

    const char* dot = strrchr(base, '.');
    size_t len = (dot != NULL && dot > base) ? (size_t) (dot - base) : strlen(base);
    if (len == 0 || len + 1 > outSize) return false;
    memcpy(out, base, len);
    out[len] = '\0';
    return true;
}

static bool N3DSAudio_shouldPreloadSound(const Sound* sound) {
    return sound != NULL;
}

static bool N3DSAudio_shouldPrewarmCachedSound(const Sound* sound, const N3DSBcwav* bcwav) {
    if (sound == NULL || bcwav == NULL) return false;
    if (!N3DSAudio_soundLooksLikeMusic(sound) &&
        bcwav->sampleRate > 0 &&
        bcwav->sampleCount > (bcwav->sampleRate * 10u)) {
        return false;
    }
    return true;
}

static bool N3DSAudio_parseBcwavInfo(const uint8_t* info, uint32_t infoSize, uint32_t dataBlockOffset, uint32_t fileSize, N3DSBcwav* out) {
    memset(out, 0, sizeof(*out));
    if (infoSize < 0x20 || memcmp(info, "INFO", 4) != 0) return false;

    uint8_t encoding = info[0x08];
    if (encoding != 2) return false;

    out->loop = info[0x09] != 0;
    out->sampleRate = N3DSAudio_readU32(info + 0x0C);
    out->loopStart = N3DSAudio_readU32(info + 0x10);
    out->loopEnd = N3DSAudio_readU32(info + 0x14);
    out->sampleCount = out->loopEnd;

    uint32_t tableOffset = 0x1C;
    uint32_t channelCount = N3DSAudio_readU32(info + tableOffset);
    if (channelCount == 0 || channelCount > 2) return false;
    if (infoSize < tableOffset + 4 + channelCount * 8) return false;

    out->channelCount = (uint8_t) channelCount;
    uint32_t encodedBytes = ((out->sampleCount + 13u) / 14u) * 8u;

    repeat(channelCount, i) {
        const uint8_t* channelRef = info + tableOffset + 4 + i * 8;
        uint32_t channelInfoOffset = tableOffset + N3DSAudio_readU32(channelRef + 4);
        if (channelInfoOffset + 0x14 > infoSize) return false;

        const uint8_t* channelInfo = info + channelInfoOffset;
        uint32_t sampleOffset = N3DSAudio_readU32(channelInfo + 4);
        uint32_t adpcmInfoOffset = channelInfoOffset + N3DSAudio_readU32(channelInfo + 12);
        if (adpcmInfoOffset + 0x2E > infoSize) return false;

        N3DSBcwavChannel* channel = &out->channels[i];
        channel->dataOffset = dataBlockOffset + 8 + sampleOffset;
        channel->dataSize = encodedBytes;
        if (channel->dataOffset + channel->dataSize > fileSize) return false;

        const uint8_t* adpcmInfo = info + adpcmInfoOffset;
        repeat(16, coef) {
            channel->coefs[coef] = N3DSAudio_readU16(adpcmInfo + coef * 2);
        }
        channel->startContext.predictorScale = adpcmInfo[0x20];
        channel->startContext.yn1 = N3DSAudio_readS16(adpcmInfo + 0x22);
        channel->startContext.yn2 = N3DSAudio_readS16(adpcmInfo + 0x24);
        channel->loopContext.predictorScale = adpcmInfo[0x26];
        channel->loopContext.yn1 = N3DSAudio_readS16(adpcmInfo + 0x28);
        channel->loopContext.yn2 = N3DSAudio_readS16(adpcmInfo + 0x2A);
    }

    return true;
}

static bool N3DSAudio_parseBcwavBlob(const uint8_t* data, uint32_t size, N3DSBcwav* out) {
    if (size < 0x40 || memcmp(data, "CWAV", 4) != 0) return false;
    uint32_t infoOffset = N3DSAudio_readU32(data + 0x18);
    uint32_t infoSize = N3DSAudio_readU32(data + 0x1C);
    uint32_t dataOffset = N3DSAudio_readU32(data + 0x24);
    if (infoOffset + infoSize > size || dataOffset + 8 > size) return false;
    return N3DSAudio_parseBcwavInfo(data + infoOffset, infoSize, dataOffset, size, out);
}

static bool N3DSAudio_parseBcwavFile(const char* path, N3DSBcwav* out) {
    if (path == NULL || out == NULL) return false;

    bool success = false;
    FILE* file = fopen(path, "rb");
    if (file == NULL) return false;
    N3DSAudio_configureFileBuffer(file);

    uint32_t fileSize = 0;
    uint8_t header[0x28];
    uint8_t* infoData = NULL;

    if (!N3DSAudio_getFileSize(file, &fileSize)) goto cleanup;
    if (fileSize < 0x40) goto cleanup;
    if (fread(header, 1, sizeof(header), file) != sizeof(header)) goto cleanup;
    if (memcmp(header, "CWAV", 4) != 0) goto cleanup;

    uint32_t infoOffset = N3DSAudio_readU32(header + 0x18);
    uint32_t infoSize = N3DSAudio_readU32(header + 0x1C);
    uint32_t dataOffset = N3DSAudio_readU32(header + 0x24);
    if (infoSize == 0 || infoOffset + infoSize > fileSize || dataOffset + 8 > fileSize) goto cleanup;

    infoData = safeMalloc(infoSize);
    if (fseek(file, (long) infoOffset, SEEK_SET) != 0) goto cleanup;
    if (fread(infoData, 1, infoSize, file) != infoSize) goto cleanup;

    success = N3DSAudio_parseBcwavInfo(infoData, infoSize, dataOffset, fileSize, out);

cleanup:
    free(infoData);
    fclose(file);
    return success;
}

static void N3DSAudio_decodeFrame(const N3DSBcwavChannel* channel, const uint8_t* frame, int16_t* hist1, int16_t* hist2, int16_t outSamples[14]) {
    int predictor = frame[0] >> 4;
    int scale = 1 << (frame[0] & 0x0F);
    int coef1 = (int16_t) channel->coefs[predictor * 2 + 0];
    int coef2 = (int16_t) channel->coefs[predictor * 2 + 1];

    int sampleIndex = 0;
    for (int byteIndex = 1; byteIndex < 8; ++byteIndex) {
        int hi = N3DSAudio_signExtend4(frame[byteIndex] >> 4);
        int lo = N3DSAudio_signExtend4(frame[byteIndex] & 0x0F);
        int nibbles[2] = { hi, lo };
        repeat(2, nibbleIndex) {
            int sample = (nibbles[nibbleIndex] * scale) << 11;
            sample += 1024 + coef1 * (*hist1) + coef2 * (*hist2);
            sample >>= 11;
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            outSamples[sampleIndex++] = (int16_t) sample;
            *hist2 = *hist1;
            *hist1 = (int16_t) sample;
        }
    }
}

static N3DSDspContext N3DSAudio_contextForSample(const N3DSBcwavChannel* channel, const N3DSStreamDecodeState* state, uint32_t sampleIndex) {
    N3DSDspContext context;
    if (channel != NULL && sampleIndex == 0) {
        context = channel->startContext;
    } else {
        context.predictorScale = state->predictorScale;
        context.yn1 = state->hist1;
        context.yn2 = state->hist2;
    }
    return context;
}

static int16_t* N3DSAudio_decodeBcwavToPcm(const uint8_t* fileData, const N3DSBcwav* bcwav) {
    size_t totalSamples = (size_t) bcwav->sampleCount * bcwav->channelCount;
    int16_t* pcm = linearAlloc(totalSamples * sizeof(int16_t));
    if (pcm == NULL) return NULL;

    int16_t frameSamples[2][14];
    int16_t hist1[2] = {
        bcwav->channels[0].startContext.yn1,
        bcwav->channels[1].startContext.yn1,
    };
    int16_t hist2[2] = {
        bcwav->channels[0].startContext.yn2,
        bcwav->channels[1].startContext.yn2,
    };

    uint32_t frameCount = (bcwav->sampleCount + 13u) / 14u;
    uint32_t sampleCursor = 0;
    repeat(frameCount, frameIndex) {
        repeat(bcwav->channelCount, channelIndex) {
            const uint8_t* frame = fileData + bcwav->channels[channelIndex].dataOffset + frameIndex * 8u;
            N3DSAudio_decodeFrame(&bcwav->channels[channelIndex], frame, &hist1[channelIndex], &hist2[channelIndex], frameSamples[channelIndex]);
        }

        uint32_t samplesThisFrame = bcwav->sampleCount - sampleCursor;
        if (samplesThisFrame > 14u) samplesThisFrame = 14u;
        repeat(samplesThisFrame, sampleIndex) {
            if (bcwav->channelCount == 1) {
                pcm[sampleCursor + sampleIndex] = frameSamples[0][sampleIndex];
            } else {
                size_t outIndex = ((size_t) sampleCursor + sampleIndex) * 2u;
                pcm[outIndex + 0] = frameSamples[0][sampleIndex];
                pcm[outIndex + 1] = frameSamples[1][sampleIndex];
            }
        }
        sampleCursor += samplesThisFrame;
    }

    DSP_FlushDataCache(pcm, totalSamples * sizeof(int16_t));
    return pcm;
}

static char* N3DSAudio_tryResolveGeneratedSoundPath(N3DSAudioSystem* audio, const Sound* sound, int32_t soundIndex) {
    char candidate[512];
    bool isMusic = N3DSAudio_soundLooksLikeMusic(sound);
    char baseName[256];

    if (isMusic) {
        if (sound != NULL && N3DSAudio_extractBaseNameNoExt(sound->file, baseName, sizeof(baseName))) {
            snprintf(candidate, sizeof(candidate), "%s.bcwav", baseName);
            char* resolved = N3DSAudio_tryResolveMusicCandidate(audio, candidate);
            if (resolved != NULL) return resolved;
        }

        if (sound != NULL && N3DSAudio_extractBaseNameNoExt(sound->name, baseName, sizeof(baseName))) {
            snprintf(candidate, sizeof(candidate), "%s.bcwav", baseName);
            char* resolved = N3DSAudio_tryResolveMusicCandidate(audio, candidate);
            if (resolved != NULL) return resolved;
        }
    }

    if (sound != NULL && sound->name != NULL && sound->name[0] != '\0') {
        if (!isMusic) {
            snprintf(candidate, sizeof(candidate), "audio/%s.bcwav", sound->name);
            char* resolved = N3DSAudio_tryResolveSfxCandidate(audio, candidate);
            if (resolved != NULL) return resolved;

            snprintf(candidate, sizeof(candidate), "%s.bcwav", sound->name);
            resolved = N3DSAudio_tryResolveSfxCandidate(audio, candidate);
            if (resolved != NULL) return resolved;
        }
    }

    if (!isMusic && sound != NULL) {
        if (N3DSAudio_extractBaseNameNoExt(sound->file, baseName, sizeof(baseName))) {
            snprintf(candidate, sizeof(candidate), "%s.bcwav", baseName);
            char* resolved = N3DSAudio_tryResolveSfxCandidate(audio, candidate);
            if (resolved != NULL) return resolved;
        }
        if (N3DSAudio_extractBaseNameNoExt(sound->name, baseName, sizeof(baseName))) {
            snprintf(candidate, sizeof(candidate), "%s.bcwav", baseName);
            char* resolved = N3DSAudio_tryResolveSfxCandidate(audio, candidate);
            if (resolved != NULL) return resolved;
        }
    }

    if (!isMusic && soundIndex >= 0) {
        snprintf(candidate, sizeof(candidate), "audio/sound_%05ld.bcwav", (long) soundIndex);
        char* resolved = N3DSAudio_tryResolveSfxCandidate(audio, candidate);
        if (resolved != NULL) return resolved;

        snprintf(candidate, sizeof(candidate), "sound_%05ld.bcwav", (long) soundIndex);
        resolved = N3DSAudio_tryResolveSfxCandidate(audio, candidate);
        if (resolved != NULL) return resolved;

        snprintf(candidate, sizeof(candidate), "audio/sound_%ld.bcwav", (long) soundIndex);
        resolved = N3DSAudio_tryResolveSfxCandidate(audio, candidate);
        if (resolved != NULL) return resolved;

        snprintf(candidate, sizeof(candidate), "sound_%ld.bcwav", (long) soundIndex);
        resolved = N3DSAudio_tryResolveSfxCandidate(audio, candidate);
        if (resolved != NULL) return resolved;
    }

    if (!isMusic && sound != NULL && sound->audioFile >= 0) {
        snprintf(candidate, sizeof(candidate), "audio/audo_%05ld.bcwav", (long) sound->audioFile);
        char* resolved = N3DSAudio_tryResolveSfxCandidate(audio, candidate);
        if (resolved != NULL) return resolved;

        snprintf(candidate, sizeof(candidate), "audo_%05ld.bcwav", (long) sound->audioFile);
        resolved = N3DSAudio_tryResolveSfxCandidate(audio, candidate);
        if (resolved != NULL) return resolved;
    }

    if (sound != NULL && sound->name != NULL && sound->name[0] != '\0') {
        char baseName[256];
        if (N3DSAudio_extractBaseNameNoExt(sound->name, baseName, sizeof(baseName))) {
            snprintf(candidate, sizeof(candidate), "%s.bcwav", baseName);
            char* resolved = isMusic ? N3DSAudio_tryResolveMusicCandidate(audio, candidate) : N3DSAudio_tryResolveSfxCandidate(audio, candidate);
            if (resolved != NULL) return resolved;
        }
    }

    return NULL;
}

static char* N3DSAudio_resolveAudioFilePath(N3DSAudioSystem* audio, const char* name) {
    if (name == NULL || name[0] == '\0') return NULL;

    char* resolved = N3DSAudio_resolveStreamPath(audio, name);
    if (resolved != NULL) return resolved;

    char candidate[512];
    if (strchr(name, '.') == NULL) {
        snprintf(candidate, sizeof(candidate), "%s.bcwav", name);
        return N3DSAudio_tryResolveMusicCandidate(audio, candidate);
    }

    char baseName[256];
    if (N3DSAudio_extractBaseNameNoExt(name, baseName, sizeof(baseName))) {
        snprintf(candidate, sizeof(candidate), "%s.bcwav", baseName);
        resolved = N3DSAudio_tryResolveMusicCandidate(audio, candidate);
        if (resolved != NULL) return resolved;
    }

    return NULL;
}

static char* N3DSAudio_resolveSoundPath(N3DSAudioSystem* audio, const Sound* sound, int32_t soundIndex) {
    const char* name = sound != NULL ? sound->file : NULL;
    if (name == NULL || name[0] == '\0') return NULL;
    bool isMusic = N3DSAudio_soundLooksLikeMusic(sound);

    char candidate[512];
    char* resolved = NULL;

    if (strchr(name, '.') == NULL) {
        if (isMusic) {
            snprintf(candidate, sizeof(candidate), "%s.bcwav", name);
            resolved = N3DSAudio_tryResolveMusicCandidate(audio, candidate);
            if (resolved != NULL) return resolved;
        } else {
            snprintf(candidate, sizeof(candidate), "audio/%s.bcwav", name);
            resolved = N3DSAudio_tryResolveSfxCandidate(audio, candidate);
            if (resolved != NULL) return resolved;

            snprintf(candidate, sizeof(candidate), "%s.bcwav", name);
            resolved = N3DSAudio_tryResolveSfxCandidate(audio, candidate);
            if (resolved != NULL) return resolved;

            resolved = N3DSAudio_tryResolveSfxCandidate(audio, candidate);
            if (resolved != NULL) return resolved;
        }
        return NULL;
    }

    resolved = isMusic ? N3DSAudio_tryResolveMusicCandidate(audio, name) : N3DSAudio_tryResolveSfxCandidate(audio, name);
    if (resolved != NULL) return resolved;

    const char* slash = strrchr(name, '/');
    const char* base = slash != NULL ? slash + 1 : name;
    const char* dot = strrchr(base, '.');
    if (dot != NULL && dot > base) {
        size_t dirLen = slash != NULL ? (size_t) (slash - name + 1) : 0;
        size_t baseLen = (size_t) (dot - base);

        if (dirLen + baseLen + 7 < sizeof(candidate)) {
            memcpy(candidate, name, dirLen);
            memcpy(candidate + dirLen, base, baseLen);
            memcpy(candidate + dirLen + baseLen, ".bcwav", 7);
            resolved = isMusic ? N3DSAudio_tryResolveMusicCandidate(audio, candidate) : N3DSAudio_tryResolveSfxCandidate(audio, candidate);
            if (resolved != NULL) return resolved;
        }

        if (!isMusic && baseLen + 13 < sizeof(candidate)) {
            memcpy(candidate, "audio/", 6);
            memcpy(candidate + 6, base, baseLen);
            memcpy(candidate + 6 + baseLen, ".bcwav", 7);
            resolved = N3DSAudio_tryResolveSfxCandidate(audio, candidate);
            if (resolved != NULL) return resolved;
        }

        if (baseLen + 7 < sizeof(candidate)) {
            memcpy(candidate, base, baseLen);
            memcpy(candidate + baseLen, ".bcwav", 7);
            resolved = isMusic ? N3DSAudio_tryResolveMusicCandidate(audio, candidate) : N3DSAudio_tryResolveSfxCandidate(audio, candidate);
            if (resolved != NULL) return resolved;
        }
    }

    if (sound != NULL && sound->name != NULL && sound->name[0] != '\0') {
        char baseName[256];
        if (N3DSAudio_extractBaseNameNoExt(sound->name, baseName, sizeof(baseName))) {
            snprintf(candidate, sizeof(candidate), "%s.bcwav", baseName);
            resolved = isMusic ? N3DSAudio_tryResolveMusicCandidate(audio, candidate) : N3DSAudio_tryResolveSfxCandidate(audio, candidate);
            if (resolved != NULL) return resolved;
        }
    }

    (void) soundIndex;
    return NULL;
}

static N3DSSoundInstance* N3DSAudio_findInstanceById(N3DSAudioSystem* audio, int32_t instanceId) {
    repeat(N3DS_MAX_SOUND_INSTANCES, i) {
        if (audio->instances[i].active && audio->instances[i].instanceId == instanceId) {
            return &audio->instances[i];
        }
    }
    return NULL;
}

static bool N3DSAudio_isInstanceId(int32_t soundOrInstance) {
    return soundOrInstance >= N3DS_SOUND_INSTANCE_ID_BASE &&
        soundOrInstance < N3DS_SOUND_INSTANCE_ID_BASE + N3DS_MAX_SOUND_INSTANCES;
}

static int32_t N3DSAudio_streamSlotFromSoundIndex(int32_t soundIndex) {
    int32_t slot = soundIndex - N3DS_AUDIO_STREAM_INDEX_BASE;
    return (slot >= 0 && slot < N3DS_MAX_STREAMS) ? slot : -1;
}

static int N3DSAudio_acquireChannel(N3DSAudioSystem* audio) {
    if (audio == NULL) return -1;

    uint32_t start = (uint32_t) audio->nextNdspChannel % N3DS_MAX_NDSP_CHANNELS;
    repeat(N3DS_MAX_NDSP_CHANNELS, offset) {
        uint32_t channel = (start + offset) % N3DS_MAX_NDSP_CHANNELS;
        if (!audio->ndspChannelInUse[channel]) {
            audio->ndspChannelInUse[channel] = true;
            audio->nextNdspChannel = (uint8_t) ((channel + 1u) % N3DS_MAX_NDSP_CHANNELS);
            return (int) channel;
        }
    }
    return -1;
}

static void N3DSAudio_resetChannelPlayback(int channelId) {
    if (channelId < 0 || channelId >= N3DS_MAX_NDSP_CHANNELS) return;
    ndspChnSetPaused(channelId, true);
    ndspChnWaveBufClear(channelId);
    ndspChnReset(channelId);
    ndspChnSetInterp(channelId, NDSP_INTERP_LINEAR);
}

static void N3DSAudio_releaseChannel(N3DSAudioSystem* audio, int channelId) {
    if (channelId < 0 || channelId >= N3DS_MAX_NDSP_CHANNELS) return;
    N3DSAudio_resetChannelPlayback(channelId);
    audio->ndspChannelInUse[channelId] = false;
}

static uint32_t N3DSAudio_adpcmByteOffsetForSample(uint32_t sampleIndex) {
    return (sampleIndex / 14u) * 8u;
}

static bool N3DSAudio_isFrameAlignedSample(uint32_t sampleIndex) {
    return (sampleIndex % 14u) == 0;
}

static float N3DSAudio_sanitizeGain(float gain) {
    if (!isfinite(gain)) return 1.0f;
    if (gain < 0.0f) return 0.0f;
    if (gain > 1.0f) return 1.0f;
    return gain;
}

static float N3DSAudio_sanitizePitch(float pitch) {
    if (!isfinite(pitch) || pitch <= 0.0f) return 1.0f;
    if (pitch < 0.0625f) return 0.0625f;
    if (pitch > 4.0f) return 4.0f;
    return pitch;
}

static void N3DSAudio_refreshNonStreamReleaseTick(N3DSSoundInstance* inst) {
    if (inst == NULL || inst->isStream || inst->loop || inst->sampleRate == 0 || inst->sampleCount == 0) {
        return;
    }

    float pitch = N3DSAudio_sanitizePitch(inst->pitch);
    double durationSeconds = ((double) inst->sampleCount / (double) inst->sampleRate) / (double) pitch;
    if (!(durationSeconds > 0.0)) durationSeconds = 0.001;

    u64 durationTicks = (u64) (durationSeconds * (double) SYSCLOCK_ARM11);
    if (durationTicks == 0) durationTicks = 1;
    inst->playbackReleaseTick = inst->playbackStartTick + durationTicks;
}

static bool N3DSAudio_nonStreamPlaybackFinished(const N3DSSoundInstance* inst, u64 nowTick) {
    if (inst == NULL || !inst->active || inst->isStream || inst->loop || inst->paused) return false;
    if (nowTick < inst->playbackReleaseTick) return false;
    if (inst->waveBufs[0].status != NDSP_WBUF_DONE) return false;
    if (ndspChnIsPlaying(inst->channelId)) return false;
    if (inst->useNativeAdpcm && inst->secondaryChannelId >= 0) {
        if (inst->secondaryWaveBufs[0].status != NDSP_WBUF_DONE) return false;
        if (ndspChnIsPlaying(inst->secondaryChannelId)) return false;
    }
    return true;
}

static void N3DSAudio_applyMix(N3DSAudioSystem* audio, N3DSSoundInstance* inst) {
    float mix[12];
    memset(mix, 0, sizeof(mix));
    inst->gain = N3DSAudio_sanitizeGain(inst->gain);
    if (inst->useNativeAdpcm && inst->secondaryChannelId >= 0) {
        mix[0] = inst->gain * audio->masterGain;
        ndspChnSetMix(inst->channelId, mix);

        memset(mix, 0, sizeof(mix));
        mix[1] = inst->gain * audio->masterGain;
        ndspChnSetMix(inst->secondaryChannelId, mix);
    } else {
        mix[0] = inst->gain * audio->masterGain;
        mix[1] = inst->gain * audio->masterGain;
        ndspChnSetMix(inst->channelId, mix);
    }

    float pitch = N3DSAudio_sanitizePitch(inst->pitch);
    float rate = inst->baseRate * pitch;
    if (!isfinite(rate) || rate < 4000.0f) rate = 4000.0f;
    if (rate > 96000.0f) rate = 96000.0f;
    inst->pitch = pitch;
    ndspChnSetRate(inst->channelId, rate);
    if (inst->useNativeAdpcm && inst->secondaryChannelId >= 0) {
        ndspChnSetRate(inst->secondaryChannelId, rate);
    }
}

static void N3DSAudio_resetInstanceWaveBufState(N3DSSoundInstance* inst) {
    if (inst == NULL) return;
    memset(inst->waveBufs, 0, sizeof(inst->waveBufs));
    memset(inst->secondaryWaveBufs, 0, sizeof(inst->secondaryWaveBufs));
    memset(inst->adpcmStates, 0, sizeof(inst->adpcmStates));
    memset(inst->bufferStartSample, 0, sizeof(inst->bufferStartSample));
}

static void N3DSAudio_rebuildInstanceChannels(N3DSAudioSystem* audio, N3DSSoundInstance* inst) {
    if (audio == NULL || inst == NULL || inst->channelId < 0) return;

    N3DSAudio_resetChannelPlayback(inst->channelId);
    if (inst->secondaryChannelId >= 0) {
        N3DSAudio_resetChannelPlayback(inst->secondaryChannelId);
    }

    if (inst->useNativeAdpcm) {
        ndspChnSetFormat(inst->channelId, NDSP_FORMAT_MONO_ADPCM);
        ndspChnSetAdpcmCoefs(inst->channelId, inst->bcwav.channels[0].coefs);
        if (inst->secondaryChannelId >= 0) {
            ndspChnSetFormat(inst->secondaryChannelId, NDSP_FORMAT_MONO_ADPCM);
            ndspChnSetAdpcmCoefs(inst->secondaryChannelId, inst->bcwav.channels[1].coefs);
        }
    } else {
        ndspChnSetFormat(inst->channelId, inst->channelCount == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
    }

    N3DSAudio_applyMix(audio, inst);
    ndspChnSetPaused(inst->channelId, inst->paused);
    if (inst->secondaryChannelId >= 0) {
        ndspChnSetPaused(inst->secondaryChannelId, inst->paused);
    }
}

static void N3DSAudio_releaseCachedSound(N3DSCachedSound* cachedSound) {
    if (cachedSound == NULL) return;
    free(cachedSound->path);
    if (cachedSound->blobOwned) free(cachedSound->blob);
    if (cachedSound->nativeBlob != NULL) linearFree(cachedSound->nativeBlob);
    memset(cachedSound, 0, sizeof(*cachedSound));
}

static uint8_t* N3DSAudio_getCachedNativeBlob(N3DSCachedSound* cachedSound) {
    if (cachedSound == NULL || !cachedSound->available || cachedSound->blob == NULL || cachedSound->blobSize == 0) return NULL;
    if (cachedSound->nativeBlob != NULL) return cachedSound->nativeBlob;

    cachedSound->nativeBlob = N3DSAudio_cloneBlobToLinear(cachedSound->blob, cachedSound->blobSize);
    return cachedSound->nativeBlob;
}

static bool N3DSAudio_tryCacheSoundBlob(N3DSAudioSystem* audio, int32_t soundIndex) {
    if (audio == NULL || soundIndex < 0 || (uint32_t) soundIndex >= audio->cachedSoundCount) return false;
    N3DSCachedSound* cachedSound = &audio->cachedSounds[soundIndex];
    if (cachedSound->attempted) return cachedSound->available;

    cachedSound->attempted = true;

    DataWin* dw = audio->base.audioGroups[0];
    Sound* sound = &dw->sond.sounds[soundIndex];
    // sndcensus is a one-shot-per-sound boot probe: cheap, keep it.
    N3DSDebugLog_event("sndcensus", "try sound=%ld name=%s file=%s",
        (long) soundIndex,
        sound->name != NULL ? sound->name : "?",
        sound->file != NULL ? sound->file : "?");
    if (!N3DSAudio_shouldPreloadSound(sound)) return false;

    const uint8_t* packedBlob = NULL;
    uint32_t packedBlobSize = 0;
    const N3DSPackedSoundBankEntry* packedEntry = N3DSAudio_getPackedSoundEntry(audio, soundIndex);
    if (!N3DSAudio_soundLooksLikeMusic(sound) &&
        packedEntry != NULL &&
        !N3DSAudio_packedSoundEntryIsPcm16(packedEntry) &&
        N3DSAudio_getPackedSoundBlob(audio, soundIndex, &packedBlob, &packedBlobSize)) {
        N3DSBcwav bcwav;
        if (!N3DSAudio_parseBcwavBlob(packedBlob, packedBlobSize, &bcwav)) return false;
        if (!N3DSAudio_shouldPrewarmCachedSound(sound, &bcwav)) return false;

        cachedSound->available = true;
        cachedSound->headerAttempted = true;
        cachedSound->headerAvailable = true;
        cachedSound->blobOwned = false;
        cachedSound->blob = (uint8_t*) packedBlob;
        cachedSound->blobSize = packedBlobSize;
        cachedSound->bcwav = bcwav;
        return true;
    }

    const char* resolvedPath = N3DSAudio_getResolvedCachedSoundPath(audio, soundIndex, sound);
    if (resolvedPath == NULL) return false;
    char* path = safeStrdup(resolvedPath);

    uint8_t* blob = NULL;
    uint32_t blobSize = 0;
    if (!N3DSAudio_readFileFully(path, &blob, &blobSize)) {
        free(path);
        return false;
    }
    if (audio->cachedSoundBytes + blobSize > audio->maxCachedSoundBytes) {
        free(blob);
        free(path);
        return false;
    }

    N3DSBcwav bcwav;
    if (!N3DSAudio_parseBcwavBlob(blob, blobSize, &bcwav)) {
        free(blob);
        free(path);
        return false;
    }
    if (!N3DSAudio_shouldPrewarmCachedSound(sound, &bcwav)) {
        free(blob);
        free(path);
        return false;
    }

    cachedSound->available = true;
    cachedSound->headerAttempted = true;
    cachedSound->headerAvailable = true;
    cachedSound->blobOwned = true;
    cachedSound->path = path;
    cachedSound->blob = blob;
    cachedSound->blobSize = blobSize;
    cachedSound->bcwav = bcwav;
    audio->cachedSoundBytes += blobSize;
    return true;
}

static N3DSCachedSound* N3DSAudio_getCachedSoundIfAvailable(N3DSAudioSystem* audio, int32_t soundIndex) {
    if (audio == NULL || soundIndex < 0 || (uint32_t) soundIndex >= audio->cachedSoundCount) return NULL;
    return audio->cachedSounds[soundIndex].available ? &audio->cachedSounds[soundIndex] : NULL;
}

static const char* N3DSAudio_getResolvedCachedSoundPath(N3DSAudioSystem* audio, int32_t soundIndex, const Sound* sound) {
    if (audio == NULL || soundIndex < 0 || (uint32_t) soundIndex >= audio->cachedSoundCount) return NULL;

    N3DSCachedSound* cachedSound = &audio->cachedSounds[soundIndex];
    if (cachedSound->path != NULL) return cachedSound->path;
    if (cachedSound->pathAttempted) return NULL;

    cachedSound->pathAttempted = true;
    cachedSound->path = N3DSAudio_resolveSoundPath(audio, sound, soundIndex);
    if (cachedSound->path == NULL) {
        cachedSound->path = N3DSAudio_tryResolveGeneratedSoundPath(audio, sound, soundIndex);
    }
    return cachedSound->path;
}

static bool N3DSAudio_getCachedSoundHeader(N3DSAudioSystem* audio, int32_t soundIndex, const Sound* sound, N3DSBcwav* out) {
    if (audio == NULL || soundIndex < 0 || (uint32_t) soundIndex >= audio->cachedSoundCount) return false;

    N3DSCachedSound* cachedSound = &audio->cachedSounds[soundIndex];
    if (cachedSound->available || cachedSound->headerAvailable) {
        if (out != NULL) *out = cachedSound->bcwav;
        return true;
    }
    if (cachedSound->headerAttempted) return false;

    cachedSound->headerAttempted = true;
    const uint8_t* packedBlob = NULL;
    uint32_t packedBlobSize = 0;
    const N3DSPackedSoundBankEntry* packedEntry = N3DSAudio_getPackedSoundEntry(audio, soundIndex);
    if (!N3DSAudio_soundLooksLikeMusic(sound) &&
        packedEntry != NULL &&
        !N3DSAudio_packedSoundEntryIsPcm16(packedEntry) &&
        N3DSAudio_getPackedSoundBlob(audio, soundIndex, &packedBlob, &packedBlobSize)) {
        if (!N3DSAudio_parseBcwavBlob(packedBlob, packedBlobSize, &cachedSound->bcwav)) return false;
    } else {
        const char* path = N3DSAudio_getResolvedCachedSoundPath(audio, soundIndex, sound);
        if (path == NULL) return false;
        if (!N3DSAudio_parseBcwavFile(path, &cachedSound->bcwav)) return false;
    }

    cachedSound->headerAvailable = true;
    if (out != NULL) *out = cachedSound->bcwav;
    return true;
}

static void N3DSAudio_prewarmSoundCache(N3DSAudioSystem* audio, uint32_t maxSounds, uint32_t byteBudget) {
    if (audio == NULL || audio->cachedSounds == NULL || audio->cachedSoundCount == 0) return;
    if (maxSounds == 0 || byteBudget == 0) return;

    uint32_t loadedSounds = 0;
    uint32_t consumedBytes = 0;
    uint32_t scanned = 0;

    while (scanned < audio->cachedSoundCount && loadedSounds < maxSounds && consumedBytes < byteBudget) {
        uint32_t soundIndex = audio->cachedSoundPrewarmCursor;
        audio->cachedSoundPrewarmCursor++;
        if (audio->cachedSoundPrewarmCursor >= audio->cachedSoundCount) {
            audio->cachedSoundPrewarmCursor = 0;
        }
        scanned++;

        N3DSCachedSound* cachedSound = &audio->cachedSounds[soundIndex];
        if (cachedSound->attempted) continue;

        uint32_t bytesBefore = audio->cachedSoundBytes;
        if (N3DSAudio_tryCacheSoundBlob(audio, (int32_t) soundIndex)) {
            loadedSounds++;
            consumedBytes += audio->cachedSoundBytes - bytesBefore;
        }
    }
}

static void N3DSAudio_requestCachePrewarmLocked(N3DSAudioSystem* audio, uint32_t maxSounds, uint32_t byteBudget) {
    if (audio == NULL || audio->cachedSounds == NULL || audio->cachedSoundCount == 0) return;
    if (maxSounds == 0 || byteBudget == 0) return;
    if (!audio->workerEnabled) {
        N3DSAudio_prewarmSoundCache(audio, maxSounds, byteBudget);
        return;
    }

    if (audio->pendingPrewarmSoundCount < maxSounds) {
        audio->pendingPrewarmSoundCount = maxSounds;
    }
    if (audio->pendingPrewarmByteBudget < byteBudget) {
        audio->pendingPrewarmByteBudget = byteBudget;
    }
    if (N3DSAudio_hasActiveStreamPlaybackLocked(audio)) return;
    LightEvent_Signal(&audio->workerEvent);
}

#if N3DS_EAGER_SFX_PRELOAD
static void N3DSAudio_preloadSounds(N3DSAudioSystem* audio, DataWin* dataWin) {
    if (audio == NULL || dataWin == NULL || dataWin->sond.count == 0) return;

    audio->cachedSounds = safeCalloc(dataWin->sond.count, sizeof(N3DSCachedSound));
    audio->cachedSoundCount = dataWin->sond.count;

    uint32_t loadedCount = 0;
    repeat(dataWin->sond.count, i) {
        if (N3DSAudio_tryCacheSoundBlob(audio, (int32_t) i)) loadedCount++;
    }

    fprintf(
        stderr,
        "N3DSAudio: preloaded %lu/%lu likely-SFX blobs (%lu bytes, cap=%lu)\n",
        (unsigned long) loadedCount,
        (unsigned long) dataWin->sond.count,
        (unsigned long) audio->cachedSoundBytes,
        (unsigned long) audio->maxCachedSoundBytes
    );
}
#endif

static N3DSStreamEntry* N3DSAudio_getActiveStreamEntry(N3DSAudioSystem* audio, int32_t soundIndex) {
    int32_t slot = N3DSAudio_streamSlotFromSoundIndex(soundIndex);
    if (slot < 0 || !audio->streams[slot].active) return NULL;
    return &audio->streams[slot];
}

static bool N3DSAudio_canUseNativeAdpcmPlayback(const N3DSBcwav* bcwav, bool loop) {
#if N3DS_FORCE_PCM_BCWAV_PLAYBACK
    (void) bcwav;
    (void) loop;
    return false;
#else
    if (bcwav == NULL || bcwav->sampleRate == 0 || bcwav->channelCount == 0 || bcwav->channelCount > 2) return false;
    if (!loop) return true;
    return !bcwav->loop ||
        bcwav->loopEnd <= bcwav->loopStart ||
        N3DSAudio_isFrameAlignedSample(bcwav->loopStart);
#endif
}

static void N3DSAudio_cleanupPartialStreamResources(N3DSAudioSystem* audio, N3DSSoundInstance* inst) {
    if (inst == NULL) return;

    if (inst->streamFile != NULL) {
        fclose(inst->streamFile);
        inst->streamFile = NULL;
    }
    repeat(inst->channelCount, channelIndex) {
        repeat(N3DS_STREAM_BUFFER_COUNT, bufferIndex) {
            if (inst->streamAdpcm[channelIndex][bufferIndex] != NULL) {
                linearFree(inst->streamAdpcm[channelIndex][bufferIndex]);
                inst->streamAdpcm[channelIndex][bufferIndex] = NULL;
            }
        }
    }
    repeat(N3DS_STREAM_BUFFER_COUNT, i) {
        if (inst->streamPcm[i] != NULL) {
            linearFree(inst->streamPcm[i]);
            inst->streamPcm[i] = NULL;
        }
    }
    if (inst->ownsStreamBlob && inst->streamBlob != NULL) {
        if (inst->streamBlobLinear) linearFree((void*) inst->streamBlob);
        else free((void*) inst->streamBlob);
    }
    inst->streamBlob = NULL;
    inst->streamBlobSize = 0;
    inst->ownsStreamBlob = false;
    inst->streamBlobLinear = false;

    N3DSAudio_releaseChannel(audio, inst->channelId);
    N3DSAudio_releaseChannel(audio, inst->secondaryChannelId);
    inst->channelId = -1;
    inst->secondaryChannelId = -1;

    inst->isStream = false;
    inst->useNativeAdpcm = false;
    inst->streamFinished = false;
    inst->sampleRate = 0;
    inst->sampleCount = 0;
    inst->channelCount = 0;
    inst->baseRate = 0.0f;
    memset(&inst->bcwav, 0, sizeof(inst->bcwav));
    N3DSAudio_resetInstanceWaveBufState(inst);
}

static bool N3DSAudio_startNativeAdpcmPlayback(
    N3DSAudioSystem* audio,
    N3DSSoundInstance* inst,
    const uint8_t* blob,
    uint32_t blobSize,
    const N3DSBcwav* bcwav
) {
    if (audio == NULL || inst == NULL || blob == NULL || bcwav == NULL) return false;

    inst->channelId = N3DSAudio_acquireChannel(audio);
    if (inst->channelId < 0) return false;
    if (bcwav->channelCount == 2) {
        inst->secondaryChannelId = N3DSAudio_acquireChannel(audio);
        if (inst->secondaryChannelId < 0) return false;
    }

    uint8_t* nativeBlob = N3DSAudio_cloneBlobToLinear(blob, blobSize);
    if (nativeBlob == NULL) return false;

    inst->useNativeAdpcm = true;
    inst->streamBlob = nativeBlob;
    inst->streamBlobSize = blobSize;
    inst->ownsStreamBlob = true;
    inst->streamBlobLinear = true;
    inst->bcwav = *bcwav;
    inst->sampleRate = bcwav->sampleRate;
    inst->sampleCount = bcwav->sampleCount;
    inst->channelCount = bcwav->channelCount;
    inst->baseRate = (float) bcwav->sampleRate;
    inst->playbackStartTick = svcGetSystemTick();
    N3DSAudio_refreshNonStreamReleaseTick(inst);

    N3DSAudio_rebuildInstanceChannels(audio, inst);
    return N3DSAudio_primeNativeAdpcm(inst);
}

static bool N3DSAudio_startOwnedLinearNativeAdpcmPlayback(
    N3DSAudioSystem* audio,
    N3DSSoundInstance* inst,
    const uint8_t* linearBlob,
    uint32_t blobSize,
    const N3DSBcwav* bcwav
) {
    if (audio == NULL || inst == NULL || linearBlob == NULL || bcwav == NULL) return false;

    inst->channelId = N3DSAudio_acquireChannel(audio);
    if (inst->channelId < 0) return false;
    if (bcwav->channelCount == 2) {
        inst->secondaryChannelId = N3DSAudio_acquireChannel(audio);
        if (inst->secondaryChannelId < 0) return false;
    }

    inst->useNativeAdpcm = true;
    inst->streamBlob = linearBlob;
    inst->streamBlobSize = blobSize;
    inst->ownsStreamBlob = true;
    inst->streamBlobLinear = true;
    inst->bcwav = *bcwav;
    inst->sampleRate = bcwav->sampleRate;
    inst->sampleCount = bcwav->sampleCount;
    inst->channelCount = bcwav->channelCount;
    inst->baseRate = (float) bcwav->sampleRate;
    inst->playbackStartTick = svcGetSystemTick();
    N3DSAudio_refreshNonStreamReleaseTick(inst);

    N3DSAudio_rebuildInstanceChannels(audio, inst);
    return N3DSAudio_primeNativeAdpcm(inst);
}

static bool N3DSAudio_startOwnedPcmPlayback(
    N3DSAudioSystem* audio,
    N3DSSoundInstance* inst,
    int16_t* pcm,
    uint32_t sampleRate,
    uint32_t sampleCount,
    uint8_t channelCount,
    bool loop
) {
    if (audio == NULL || inst == NULL || pcm == NULL) return false;
    if (sampleRate == 0 || sampleCount == 0 || channelCount == 0 || channelCount > 2) return false;
    if (!loop) N3DSAudio_softenPcmTail(pcm, sampleCount, channelCount);

    inst->channelId = N3DSAudio_acquireChannel(audio);
    if (inst->channelId < 0) return false;
    ndspChnSetInterp(inst->channelId, NDSP_INTERP_LINEAR);

    inst->pcmData = pcm;
    inst->sampleRate = sampleRate;
    inst->sampleCount = sampleCount;
    inst->channelCount = channelCount;
    inst->baseRate = (float) sampleRate;
    inst->playbackStartTick = svcGetSystemTick();
    N3DSAudio_refreshNonStreamReleaseTick(inst);
    N3DSAudio_resetInstanceWaveBufState(inst);
    inst->waveBufs[0].data_pcm16 = pcm;
    inst->waveBufs[0].nsamples = sampleCount;
    inst->waveBufs[0].looping = loop;
    N3DSAudio_rebuildInstanceChannels(audio, inst);
    ndspChnWaveBufAdd(inst->channelId, &inst->waveBufs[0]);
    return true;
}

static bool N3DSAudio_startSharedNativeAdpcmPlayback(
    N3DSAudioSystem* audio,
    N3DSSoundInstance* inst,
    const uint8_t* linearBlob,
    uint32_t blobSize,
    const N3DSBcwav* bcwav
) {
    if (audio == NULL || inst == NULL || linearBlob == NULL || bcwav == NULL) return false;

    inst->channelId = N3DSAudio_acquireChannel(audio);
    if (inst->channelId < 0) return false;
    if (bcwav->channelCount == 2) {
        inst->secondaryChannelId = N3DSAudio_acquireChannel(audio);
        if (inst->secondaryChannelId < 0) return false;
    }

    inst->useNativeAdpcm = true;
    inst->streamBlob = linearBlob;
    inst->streamBlobSize = blobSize;
    inst->ownsStreamBlob = false;
    inst->streamBlobLinear = true;
    inst->bcwav = *bcwav;
    inst->sampleRate = bcwav->sampleRate;
    inst->sampleCount = bcwav->sampleCount;
    inst->channelCount = bcwav->channelCount;
    inst->baseRate = (float) bcwav->sampleRate;
    inst->playbackStartTick = svcGetSystemTick();
    N3DSAudio_refreshNonStreamReleaseTick(inst);

    N3DSAudio_rebuildInstanceChannels(audio, inst);
    return N3DSAudio_primeNativeAdpcm(inst);
}

static bool N3DSAudio_primeNativeAdpcm(N3DSSoundInstance* inst) {
    if (!inst->useNativeAdpcm || inst->streamBlob == NULL) return false;

    uint32_t loopStart = 0;
    uint32_t loopEnd = inst->bcwav.sampleCount;
    bool useLoopRegion = inst->loop &&
        inst->bcwav.loop &&
        inst->bcwav.loopStart > 0 &&
        inst->bcwav.loopEnd > inst->bcwav.loopStart;
    if (useLoopRegion) {
        loopStart = inst->bcwav.loopStart;
        loopEnd = inst->bcwav.loopEnd;
    }

    N3DSAudio_resetInstanceWaveBufState(inst);
    inst->bufferStartSample[0] = 0;
    inst->bufferStartSample[1] = loopStart;

    inst->adpcmStates[0].index = inst->bcwav.channels[0].startContext.predictorScale;
    inst->adpcmStates[0].history0 = inst->bcwav.channels[0].startContext.yn1;
    inst->adpcmStates[0].history1 = inst->bcwav.channels[0].startContext.yn2;

    inst->waveBufs[0].data_adpcm = (uint8_t*) inst->streamBlob + inst->bcwav.channels[0].dataOffset;
    inst->waveBufs[0].nsamples = useLoopRegion ? loopStart : inst->bcwav.sampleCount;
    inst->waveBufs[0].adpcm_data = &inst->adpcmStates[0];
    inst->waveBufs[0].looping = inst->loop && !useLoopRegion;

    if (inst->secondaryChannelId >= 0) {
        inst->adpcmStates[1].index = inst->bcwav.channels[1].startContext.predictorScale;
        inst->adpcmStates[1].history0 = inst->bcwav.channels[1].startContext.yn1;
        inst->adpcmStates[1].history1 = inst->bcwav.channels[1].startContext.yn2;

        inst->secondaryWaveBufs[0].data_adpcm = (uint8_t*) inst->streamBlob + inst->bcwav.channels[1].dataOffset;
        inst->secondaryWaveBufs[0].nsamples = inst->waveBufs[0].nsamples;
        inst->secondaryWaveBufs[0].adpcm_data = &inst->adpcmStates[1];
        inst->secondaryWaveBufs[0].looping = inst->waveBufs[0].looping;
    }

    if (useLoopRegion) {
        inst->adpcmStates[2].index = inst->bcwav.channels[0].loopContext.predictorScale;
        inst->adpcmStates[2].history0 = inst->bcwav.channels[0].loopContext.yn1;
        inst->adpcmStates[2].history1 = inst->bcwav.channels[0].loopContext.yn2;

        inst->waveBufs[1].data_adpcm = (uint8_t*) inst->streamBlob + inst->bcwav.channels[0].dataOffset + N3DSAudio_adpcmByteOffsetForSample(loopStart);
        inst->waveBufs[1].nsamples = loopEnd - loopStart;
        inst->waveBufs[1].adpcm_data = &inst->adpcmStates[2];
        inst->waveBufs[1].looping = true;

        if (inst->secondaryChannelId >= 0) {
            inst->adpcmStates[3].index = inst->bcwav.channels[1].loopContext.predictorScale;
            inst->adpcmStates[3].history0 = inst->bcwav.channels[1].loopContext.yn1;
            inst->adpcmStates[3].history1 = inst->bcwav.channels[1].loopContext.yn2;

            inst->secondaryWaveBufs[1].data_adpcm = (uint8_t*) inst->streamBlob + inst->bcwav.channels[1].dataOffset + N3DSAudio_adpcmByteOffsetForSample(loopStart);
            inst->secondaryWaveBufs[1].nsamples = inst->waveBufs[1].nsamples;
            inst->secondaryWaveBufs[1].adpcm_data = &inst->adpcmStates[3];
            inst->secondaryWaveBufs[1].looping = true;
        }
    }

    DSP_FlushDataCache((void*) inst->streamBlob, inst->streamBlobSize);
    ndspChnWaveBufAdd(inst->channelId, &inst->waveBufs[0]);
    if (inst->secondaryChannelId >= 0) {
        ndspChnWaveBufAdd(inst->secondaryChannelId, &inst->secondaryWaveBufs[0]);
    }

    if (useLoopRegion) {
        ndspChnWaveBufAdd(inst->channelId, &inst->waveBufs[1]);
        if (inst->secondaryChannelId >= 0) {
            ndspChnWaveBufAdd(inst->secondaryChannelId, &inst->secondaryWaveBufs[1]);
        }
    }
    return true;
}

static void N3DSAudio_releaseInstance(N3DSAudioSystem* audio, N3DSSoundInstance* inst) {
    if (!inst->active) return;
    if (inst->isStream) {
        N3DSDebugLog_event(
            "audio_stop",
            "instance=%ld sound=%ld native=%d sample=%lu/%lu ch=%ld/%ld",
            (long) inst->instanceId,
            (long) inst->soundIndex,
            inst->useNativeAdpcm ? 1 : 0,
            (unsigned long) inst->currentSample,
            (unsigned long) inst->sampleCount,
            (long) inst->channelId,
            (long) inst->secondaryChannelId
        );
    }
    if (audio != NULL && inst->isStream) {
        N3DSAudio_cancelAsyncFillLocked(audio, inst);
    }

    // ===[ Teardown breadcrumbs: the 661s crash died silently somewhere in
    // here (audio_stop logged, then nothing — no crash banner, no VM trace).
    // Suspects: ndsp IPC on a wedged DSP, linearFree on a poisoned heap, or
    // fclose on an SD handle gone bad. Name every step. ]===
    if (inst->isStream) {
        N3DSDebugLog_event("tdn", "0 begin free inst=%ld", (long) inst->instanceId);
        N3DSDebugLog_event("tdn", "1 async-cancel done inst=%ld", (long) inst->instanceId);
    }

    N3DSAudio_releaseChannel(audio, inst->channelId);
    N3DSAudio_releaseChannel(audio, inst->secondaryChannelId);
    if (inst->isStream) {
        N3DSDebugLog_event("tdn", "2 ndsp channels released inst=%ld", (long) inst->instanceId);
    }

    if (inst->pcmData != NULL) linearFree(inst->pcmData);
    if (inst->isStream) {
        N3DSDebugLog_event("tdn", "3 pcmData freed inst=%ld", (long) inst->instanceId);
    }

    repeat(inst->channelCount, channelIndex) {
        repeat(N3DS_STREAM_BUFFER_COUNT, bufferIndex) {
            if (inst->streamAdpcm[channelIndex][bufferIndex] != NULL) {
                linearFree(inst->streamAdpcm[channelIndex][bufferIndex]);
            }
        }
    }
    if (inst->isStream) {
        N3DSDebugLog_event("tdn", "4 adpcm buffers freed inst=%ld", (long) inst->instanceId);
    }

    repeat(N3DS_STREAM_BUFFER_COUNT, i) {
        if (inst->streamPcm[i] != NULL) linearFree(inst->streamPcm[i]);
    }
    if (inst->isStream) {
        N3DSDebugLog_event("tdn", "5 pcm buffers freed inst=%ld", (long) inst->instanceId);
    }

    if (inst->streamFile != NULL) fclose(inst->streamFile);
    if (inst->isStream) {
        N3DSDebugLog_event("tdn", "6 streamFile closed inst=%ld", (long) inst->instanceId);
    }

    if (inst->ownsStreamBlob) {
        if (inst->streamBlobLinear) linearFree((void*) inst->streamBlob);
        else free((void*) inst->streamBlob);
    }
    if (inst->isStream) {
        N3DSDebugLog_event("tdn", "7 streamBlob freed — teardown complete inst=%ld", (long) inst->instanceId);
    }

    memset(inst, 0, sizeof(*inst));
}

static N3DSSoundInstance* N3DSAudio_findFreeInstanceInRange(N3DSAudioSystem* audio, int32_t start, int32_t endExclusive) {
    u64 nowTick = svcGetSystemTick();
    for (int32_t i = start; i < endExclusive; ++i) {
        if (!audio->instances[i].active) return &audio->instances[i];
    }
    for (int32_t i = start; i < endExclusive; ++i) {
        bool finishedPcm = N3DSAudio_nonStreamPlaybackFinished(&audio->instances[i], nowTick);
        if (finishedPcm) {
            N3DSAudio_releaseInstance(audio, &audio->instances[i]);
            return &audio->instances[i];
        }
    }
    return NULL;
}

static N3DSSoundInstance* N3DSAudio_findFreeInstance(N3DSAudioSystem* audio, bool preferStreamSlots) {
    int32_t split = N3DS_MAX_SOUND_INSTANCES - N3DS_RESERVED_STREAM_INSTANCES;
    if (split < 0) split = 0;
    if (split > N3DS_MAX_SOUND_INSTANCES) split = N3DS_MAX_SOUND_INSTANCES;

    if (preferStreamSlots) {
        N3DSSoundInstance* inst = N3DSAudio_findFreeInstanceInRange(audio, split, N3DS_MAX_SOUND_INSTANCES);
        if (inst != NULL) return inst;
        return N3DSAudio_findFreeInstanceInRange(audio, 0, split);
    }

    N3DSSoundInstance* inst = N3DSAudio_findFreeInstanceInRange(audio, 0, split);
    if (inst != NULL) return inst;
    return N3DSAudio_findFreeInstanceInRange(audio, split, N3DS_MAX_SOUND_INSTANCES);
}

static void N3DSAudio_streamResetDecoder(N3DSSoundInstance* inst) {
    inst->currentSample = 0;
    inst->pendingSamples = 0;
    inst->pendingOffset = 0;
    repeat(inst->bcwav.channelCount, i) {
        inst->decode[i].nextFrameIndex = 0;
        inst->decode[i].predictorScale = inst->bcwav.channels[i].startContext.predictorScale;
        inst->decode[i].hist1 = inst->bcwav.channels[i].startContext.yn1;
        inst->decode[i].hist2 = inst->bcwav.channels[i].startContext.yn2;
        inst->fileCacheFrameBase[i] = 0;
        inst->fileCacheFrameCount[i] = 0;
    }
}

static bool N3DSAudio_streamEnsureFileCache(N3DSSoundInstance* inst, uint32_t channelIndex) {
    if (inst == NULL || inst->streamFile == NULL || channelIndex >= inst->bcwav.channelCount) return false;

    N3DSStreamDecodeState* state = &inst->decode[channelIndex];
    uint32_t frameIndex = state->nextFrameIndex;
    uint32_t cacheBase = inst->fileCacheFrameBase[channelIndex];
    uint32_t cacheCount = inst->fileCacheFrameCount[channelIndex];
    if (frameIndex >= cacheBase && frameIndex < cacheBase + cacheCount) return true;

    uint32_t totalFrames = (inst->sampleCount + 13u) / 14u;
    if (frameIndex >= totalFrames) return false;

    uint32_t framesToRead = totalFrames - frameIndex;
    if (framesToRead > N3DS_STREAM_ADPCM_CACHE_FRAMES) framesToRead = N3DS_STREAM_ADPCM_CACHE_FRAMES;

    uint32_t byteOffset = inst->bcwav.channels[channelIndex].dataOffset + frameIndex * 8u;
    uint32_t byteCount = framesToRead * 8u;
    if (fseek(inst->streamFile, (long) byteOffset, SEEK_SET) != 0) return false;
    if (fread(inst->fileCache[channelIndex], 1, byteCount, inst->streamFile) != byteCount) return false;

    inst->fileCacheFrameBase[channelIndex] = frameIndex;
    inst->fileCacheFrameCount[channelIndex] = framesToRead;
    return true;
}

static bool N3DSAudio_streamDecodeNextFrame(N3DSSoundInstance* inst, int16_t outPcm[14 * 2], uint32_t* outSamples) {
    if (inst->currentSample >= inst->sampleCount) {
        *outSamples = 0;
        return true;
    }

    int16_t monoSamples[2][14];
    repeat(inst->bcwav.channelCount, channelIndex) {
        N3DSStreamDecodeState* state = &inst->decode[channelIndex];
        uint32_t byteOffset = inst->bcwav.channels[channelIndex].dataOffset + state->nextFrameIndex * 8u;
        const uint8_t* frame = NULL;
        uint8_t frameBuffer[8];
        if (inst->streamBlob != NULL) {
            if (byteOffset + 8u > inst->streamBlobSize) return false;
            frame = inst->streamBlob + byteOffset;
        } else {
            if (!N3DSAudio_streamEnsureFileCache(inst, channelIndex)) return false;
            uint32_t cacheIndex = state->nextFrameIndex - inst->fileCacheFrameBase[channelIndex];
            if (cacheIndex >= inst->fileCacheFrameCount[channelIndex]) return false;
            memcpy(frameBuffer, inst->fileCache[channelIndex] + cacheIndex * 8u, sizeof(frameBuffer));
            frame = frameBuffer;
        }
        state->predictorScale = frame[0];
        N3DSAudio_decodeFrame(&inst->bcwav.channels[channelIndex], frame, &state->hist1, &state->hist2, monoSamples[channelIndex]);
        state->nextFrameIndex++;
    }

    uint32_t samplesThisFrame = inst->sampleCount - inst->currentSample;
    if (samplesThisFrame > 14u) samplesThisFrame = 14u;
    repeat(samplesThisFrame, sampleIndex) {
        if (inst->bcwav.channelCount == 1) {
            outPcm[sampleIndex] = monoSamples[0][sampleIndex];
        } else {
            outPcm[sampleIndex * 2 + 0] = monoSamples[0][sampleIndex];
            outPcm[sampleIndex * 2 + 1] = monoSamples[1][sampleIndex];
        }
    }
    inst->currentSample += samplesThisFrame;
    *outSamples = samplesThisFrame;
    return true;
}

static bool N3DSAudio_streamSeekSamples(N3DSSoundInstance* inst, uint32_t targetSample) {
    if (targetSample > inst->sampleCount) targetSample = inst->sampleCount;

    N3DSAudio_streamResetDecoder(inst);
    if (targetSample == 0) return true;
    if (inst->bcwav.loop &&
        targetSample == inst->bcwav.loopStart &&
        N3DSAudio_isFrameAlignedSample(targetSample)) {
        uint32_t frameIndex = targetSample / 14u;
        repeat(inst->bcwav.channelCount, i) {
            inst->decode[i].nextFrameIndex = frameIndex;
            inst->decode[i].predictorScale = inst->bcwav.channels[i].loopContext.predictorScale;
            inst->decode[i].hist1 = inst->bcwav.channels[i].loopContext.yn1;
            inst->decode[i].hist2 = inst->bcwav.channels[i].loopContext.yn2;
        }
        inst->currentSample = targetSample;
        return true;
    }

    int16_t framePcm[14 * 2];
    uint32_t frameSamples = 0;
    while (inst->currentSample + 14u <= targetSample) {
        if (!N3DSAudio_streamDecodeNextFrame(inst, framePcm, &frameSamples)) return false;
        if (frameSamples == 0) return true;
    }

    if (inst->currentSample < targetSample) {
        uint32_t baseSample = inst->currentSample;
        if (!N3DSAudio_streamDecodeNextFrame(inst, framePcm, &frameSamples)) return false;
        uint32_t skip = targetSample - baseSample;
        if (skip < frameSamples) {
            uint32_t remaining = frameSamples - skip;
            size_t stride = inst->bcwav.channelCount;
            memcpy(inst->pendingPcm, framePcm + skip * stride, remaining * stride * sizeof(int16_t));
            inst->pendingSamples = remaining;
            inst->pendingOffset = 0;
        }
        inst->currentSample = targetSample;
    }

    return true;
}

static bool N3DSAudio_streamEnsureFileCacheAsync(N3DSSoundInstance* inst, N3DSStreamFillCursor* cursor, uint32_t channelIndex) {
    if (inst == NULL || cursor == NULL || inst->streamFile == NULL || channelIndex >= inst->bcwav.channelCount) return false;

    N3DSStreamDecodeState* state = &cursor->decode[channelIndex];
    uint32_t frameIndex = state->nextFrameIndex;
    uint32_t cacheBase = cursor->fileCacheFrameBase[channelIndex];
    uint32_t cacheCount = cursor->fileCacheFrameCount[channelIndex];
    if (frameIndex >= cacheBase && frameIndex < cacheBase + cacheCount) return true;

    uint32_t totalFrames = (inst->sampleCount + 13u) / 14u;
    if (frameIndex >= totalFrames) return false;

    uint32_t framesToRead = totalFrames - frameIndex;
    if (framesToRead > N3DS_STREAM_ADPCM_CACHE_FRAMES) framesToRead = N3DS_STREAM_ADPCM_CACHE_FRAMES;

    uint32_t byteOffset = inst->bcwav.channels[channelIndex].dataOffset + frameIndex * 8u;
    uint32_t byteCount = framesToRead * 8u;
    if (fseek(inst->streamFile, (long) byteOffset, SEEK_SET) != 0) return false;
    if (fread(cursor->fileCache[channelIndex], 1, byteCount, inst->streamFile) != byteCount) return false;

    cursor->fileCacheFrameBase[channelIndex] = frameIndex;
    cursor->fileCacheFrameCount[channelIndex] = framesToRead;
    return true;
}

static bool N3DSAudio_streamDecodeNextFrameAsync(
    N3DSSoundInstance* inst,
    N3DSStreamFillCursor* cursor,
    int16_t outPcm[14 * 2],
    uint32_t* outSamples
) {
    if (inst == NULL || cursor == NULL || outPcm == NULL || outSamples == NULL) return false;
    if (cursor->currentSample >= inst->sampleCount) {
        *outSamples = 0;
        return true;
    }

    int16_t monoSamples[2][14];
    repeat(inst->bcwav.channelCount, channelIndex) {
        N3DSStreamDecodeState* state = &cursor->decode[channelIndex];
        uint32_t byteOffset = inst->bcwav.channels[channelIndex].dataOffset + state->nextFrameIndex * 8u;
        const uint8_t* frame = NULL;
        uint8_t frameBuffer[8];
        if (inst->streamBlob != NULL) {
            if (byteOffset + 8u > inst->streamBlobSize) return false;
            frame = inst->streamBlob + byteOffset;
        } else {
            if (!N3DSAudio_streamEnsureFileCacheAsync(inst, cursor, channelIndex)) return false;
            uint32_t cacheIndex = state->nextFrameIndex - cursor->fileCacheFrameBase[channelIndex];
            if (cacheIndex >= cursor->fileCacheFrameCount[channelIndex]) return false;
            memcpy(frameBuffer, cursor->fileCache[channelIndex] + cacheIndex * 8u, sizeof(frameBuffer));
            frame = frameBuffer;
        }
        state->predictorScale = frame[0];
        N3DSAudio_decodeFrame(&inst->bcwav.channels[channelIndex], frame, &state->hist1, &state->hist2, monoSamples[channelIndex]);
        state->nextFrameIndex++;
    }

    uint32_t samplesThisFrame = inst->sampleCount - cursor->currentSample;
    if (samplesThisFrame > 14u) samplesThisFrame = 14u;
    repeat(samplesThisFrame, sampleIndex) {
        if (inst->bcwav.channelCount == 1) {
            outPcm[sampleIndex] = monoSamples[0][sampleIndex];
        } else {
            outPcm[sampleIndex * 2 + 0] = monoSamples[0][sampleIndex];
            outPcm[sampleIndex * 2 + 1] = monoSamples[1][sampleIndex];
        }
    }
    cursor->currentSample += samplesThisFrame;
    *outSamples = samplesThisFrame;
    return true;
}

static bool N3DSAudio_streamSeekSamplesAsync(N3DSSoundInstance* inst, N3DSStreamFillCursor* cursor, uint32_t targetSample) {
    if (inst == NULL || cursor == NULL) return false;
    if (targetSample > inst->sampleCount) targetSample = inst->sampleCount;

    cursor->currentSample = 0;
    cursor->pendingSamples = 0;
    cursor->pendingOffset = 0;
    repeat(inst->bcwav.channelCount, i) {
        cursor->decode[i].nextFrameIndex = 0;
        cursor->decode[i].predictorScale = inst->bcwav.channels[i].startContext.predictorScale;
        cursor->decode[i].hist1 = inst->bcwav.channels[i].startContext.yn1;
        cursor->decode[i].hist2 = inst->bcwav.channels[i].startContext.yn2;
        cursor->fileCacheFrameBase[i] = 0;
        cursor->fileCacheFrameCount[i] = 0;
    }
    if (targetSample == 0) return true;
    if (inst->bcwav.loop &&
        targetSample == inst->bcwav.loopStart &&
        N3DSAudio_isFrameAlignedSample(targetSample)) {
        uint32_t frameIndex = targetSample / 14u;
        repeat(inst->bcwav.channelCount, i) {
            cursor->decode[i].nextFrameIndex = frameIndex;
            cursor->decode[i].predictorScale = inst->bcwav.channels[i].loopContext.predictorScale;
            cursor->decode[i].hist1 = inst->bcwav.channels[i].loopContext.yn1;
            cursor->decode[i].hist2 = inst->bcwav.channels[i].loopContext.yn2;
        }
        cursor->currentSample = targetSample;
        return true;
    }

    int16_t framePcm[14 * 2];
    uint32_t frameSamples = 0;
    while (cursor->currentSample + 14u <= targetSample) {
        if (!N3DSAudio_streamDecodeNextFrameAsync(inst, cursor, framePcm, &frameSamples)) return false;
        if (frameSamples == 0) return true;
    }

    if (cursor->currentSample < targetSample) {
        uint32_t baseSample = cursor->currentSample;
        if (!N3DSAudio_streamDecodeNextFrameAsync(inst, cursor, framePcm, &frameSamples)) return false;
        uint32_t skip = targetSample - baseSample;
        if (skip < frameSamples) {
            uint32_t remaining = frameSamples - skip;
            size_t stride = inst->bcwav.channelCount;
            memcpy(cursor->pendingPcm, framePcm + skip * stride, remaining * stride * sizeof(int16_t));
            cursor->pendingSamples = remaining;
            cursor->pendingOffset = 0;
        }
        cursor->currentSample = targetSample;
    }

    return true;
}

static bool N3DSAudio_fillStreamWaveBufAsync(
    N3DSSoundInstance* inst,
    int bufferIndex,
    N3DSStreamFillCursor* cursor,
    uint32_t* outStartSample,
    uint32_t* outProducedSamples
) {
    if (inst == NULL || cursor == NULL || outStartSample == NULL || outProducedSamples == NULL) return false;

    size_t stride = inst->bcwav.channelCount;
    int16_t* out = inst->streamPcm[bufferIndex];
    uint32_t produced = 0;
    uint32_t startSample = cursor->currentSample;

    while (produced < N3DS_STREAM_CHUNK_SAMPLES) {
        if (cursor->pendingOffset < cursor->pendingSamples) {
            uint32_t take = cursor->pendingSamples - cursor->pendingOffset;
            if (take > N3DS_STREAM_CHUNK_SAMPLES - produced) take = N3DS_STREAM_CHUNK_SAMPLES - produced;
            memcpy(
                out + produced * stride,
                cursor->pendingPcm + cursor->pendingOffset * stride,
                take * stride * sizeof(int16_t)
            );
            cursor->pendingOffset += take;
            produced += take;
            cursor->currentSample += take;
            if (cursor->pendingOffset >= cursor->pendingSamples) {
                cursor->pendingSamples = 0;
                cursor->pendingOffset = 0;
            }
            continue;
        }

        if (cursor->currentSample >= inst->sampleCount) {
            if (inst->loop) {
                if (produced > 0) break;
                if (!N3DSAudio_streamSeekSamplesAsync(inst, cursor, inst->bcwav.loopStart)) return false;
                startSample = cursor->currentSample;
                continue;
            }
            break;
        }

        int16_t framePcm[14 * 2];
        uint32_t frameSamples = 0;
        if (!N3DSAudio_streamDecodeNextFrameAsync(inst, cursor, framePcm, &frameSamples)) return false;
        if (frameSamples == 0) break;

        uint32_t take = frameSamples;
        if (take > N3DS_STREAM_CHUNK_SAMPLES - produced) take = N3DS_STREAM_CHUNK_SAMPLES - produced;
        memcpy(out + produced * stride, framePcm, take * stride * sizeof(int16_t));
        produced += take;
    }

    if (produced == 0) return false;

    DSP_FlushDataCache(out, produced * stride * sizeof(int16_t));
    *outStartSample = startSample;
    *outProducedSamples = produced;
    return true;
}

static bool N3DSAudio_fillNativeStreamWaveBufAsync(
    N3DSSoundInstance* inst,
    int bufferIndex,
    N3DSStreamFillCursor* cursor,
    uint32_t* outStartSample,
    uint32_t* outProducedSamples,
    ndspAdpcmData outAdpcmStates[2]
) {
    if (inst == NULL || cursor == NULL || outStartSample == NULL || outProducedSamples == NULL || outAdpcmStates == NULL) return false;
    if (inst->streamFile == NULL || inst->channelCount == 0) return false;

    uint32_t startSample = cursor->currentSample;
    if (startSample >= inst->sampleCount) {
        if (!inst->loop) return false;
        if (!N3DSAudio_isFrameAlignedSample(inst->bcwav.loopStart)) return false;
        if (!N3DSAudio_streamSeekSamplesAsync(inst, cursor, inst->bcwav.loopStart)) return false;
        startSample = cursor->currentSample;
    }

    uint32_t endSample = startSample + N3DS_STREAM_CHUNK_SAMPLES;
    if (endSample > inst->sampleCount) endSample = inst->sampleCount;
    if (inst->loop && inst->bcwav.loop && inst->bcwav.loopEnd > inst->bcwav.loopStart && endSample > inst->bcwav.loopEnd) {
        endSample = inst->bcwav.loopEnd;
    }
    uint32_t sampleCount = endSample - startSample;
    if (sampleCount == 0) return false;

    repeat(inst->channelCount, channelIndex) {
        uint32_t startFrame = startSample / 14u;
        uint32_t frameCount = (sampleCount + 13u) / 14u;
        uint32_t byteOffset = inst->bcwav.channels[channelIndex].dataOffset + startFrame * 8u;
        uint32_t byteCount = frameCount * 8u;
        if (fseek(inst->streamFile, (long) byteOffset, SEEK_SET) != 0) return false;
        if (fread(inst->streamAdpcm[channelIndex][bufferIndex], 1, byteCount, inst->streamFile) != byteCount) return false;
        DSP_FlushDataCache(inst->streamAdpcm[channelIndex][bufferIndex], byteCount);
    }

    N3DSDspContext primaryContext = N3DSAudio_contextForSample(&inst->bcwav.channels[0], &cursor->decode[0], startSample);
    outAdpcmStates[0].index = primaryContext.predictorScale;
    outAdpcmStates[0].history0 = primaryContext.yn1;
    outAdpcmStates[0].history1 = primaryContext.yn2;

    if (inst->secondaryChannelId >= 0) {
        N3DSDspContext secondaryContext = N3DSAudio_contextForSample(&inst->bcwav.channels[1], &cursor->decode[1], startSample);
        outAdpcmStates[1].index = secondaryContext.predictorScale;
        outAdpcmStates[1].history0 = secondaryContext.yn1;
        outAdpcmStates[1].history1 = secondaryContext.yn2;
    } else {
        memset(&outAdpcmStates[1], 0, sizeof(outAdpcmStates[1]));
    }

    bool atLoopBoundary = inst->loop &&
        inst->bcwav.loop &&
        inst->bcwav.loopEnd > inst->bcwav.loopStart &&
        endSample >= inst->bcwav.loopEnd;

    {
        N3DSStreamDecodeState nextDecode[2];
        memcpy(nextDecode, cursor->decode, sizeof(nextDecode));

        uint32_t framesToAdvance = (sampleCount + 13u) / 14u;
        repeat(inst->channelCount, channelIndex) {
            uint32_t channelSample = startSample;
            repeat(framesToAdvance, frameIndex) {
                const uint8_t* frame = inst->streamAdpcm[channelIndex][bufferIndex] + frameIndex * 8u;
                int16_t scratch[14];
                nextDecode[channelIndex].predictorScale = frame[0];
                N3DSAudio_decodeFrame(
                    &inst->bcwav.channels[channelIndex],
                    frame,
                    &nextDecode[channelIndex].hist1,
                    &nextDecode[channelIndex].hist2,
                    scratch
                );
                nextDecode[channelIndex].nextFrameIndex++;

                uint32_t remaining = endSample - channelSample;
                uint32_t frameSamples = remaining > 14u ? 14u : remaining;
                channelSample += frameSamples;
            }
        }

        if (atLoopBoundary) {
            uint32_t loopFrame = inst->bcwav.loopStart / 14u;
            repeat(inst->channelCount, channelIndex) {
                cursor->decode[channelIndex].nextFrameIndex = loopFrame;
                cursor->decode[channelIndex].predictorScale = inst->bcwav.channels[channelIndex].loopContext.predictorScale;
                cursor->decode[channelIndex].hist1 = inst->bcwav.channels[channelIndex].loopContext.yn1;
                cursor->decode[channelIndex].hist2 = inst->bcwav.channels[channelIndex].loopContext.yn2;
            }
            cursor->currentSample = inst->bcwav.loopStart;
        } else {
            memcpy(cursor->decode, nextDecode, sizeof(nextDecode));
            cursor->currentSample = endSample;
        }
    }

    *outStartSample = startSample;
    *outProducedSamples = sampleCount;
    return true;
}

static bool N3DSAudio_fillStreamWaveBuf(N3DSSoundInstance* inst, int bufferIndex) {
    size_t stride = inst->bcwav.channelCount;
    int16_t* out = inst->streamPcm[bufferIndex];
    uint32_t produced = 0;
    uint32_t startSample = inst->currentSample;

    while (produced < N3DS_STREAM_CHUNK_SAMPLES) {
        if (inst->pendingOffset < inst->pendingSamples) {
            uint32_t take = inst->pendingSamples - inst->pendingOffset;
            if (take > N3DS_STREAM_CHUNK_SAMPLES - produced) take = N3DS_STREAM_CHUNK_SAMPLES - produced;
            memcpy(
                out + produced * stride,
                inst->pendingPcm + inst->pendingOffset * stride,
                take * stride * sizeof(int16_t)
            );
            inst->pendingOffset += take;
            produced += take;
            inst->currentSample += take;
            if (inst->pendingOffset >= inst->pendingSamples) {
                inst->pendingSamples = 0;
                inst->pendingOffset = 0;
            }
            continue;
        }

        if (inst->currentSample >= inst->sampleCount) {
            if (inst->loop) {
                if (produced > 0) break;
                if (!N3DSAudio_streamSeekSamples(inst, inst->bcwav.loopStart)) return false;
                startSample = inst->currentSample;
                continue;
            }
            break;
        }

        int16_t framePcm[14 * 2];
        uint32_t frameSamples = 0;
        if (!N3DSAudio_streamDecodeNextFrame(inst, framePcm, &frameSamples)) return false;
        if (frameSamples == 0) break;

        uint32_t take = frameSamples;
        if (take > N3DS_STREAM_CHUNK_SAMPLES - produced) take = N3DS_STREAM_CHUNK_SAMPLES - produced;
        memcpy(out + produced * stride, framePcm, take * stride * sizeof(int16_t));
        produced += take;
    }

    if (produced == 0) return false;

    memset(&inst->waveBufs[bufferIndex], 0, sizeof(inst->waveBufs[bufferIndex]));
    inst->waveBufs[bufferIndex].data_pcm16 = out;
    inst->waveBufs[bufferIndex].nsamples = produced;
    inst->waveBufs[bufferIndex].looping = false;
    inst->bufferStartSample[bufferIndex] = startSample;
    DSP_FlushDataCache(out, produced * stride * sizeof(int16_t));
    return true;
}

static bool N3DSAudio_fillNativeStreamWaveBuf(N3DSSoundInstance* inst, int bufferIndex) {
    if (inst == NULL || inst->streamFile == NULL || inst->channelCount == 0) return false;

    uint32_t startSample = inst->currentSample;
    if (startSample >= inst->sampleCount) {
        if (!inst->loop) return false;
        if (!N3DSAudio_isFrameAlignedSample(inst->bcwav.loopStart)) return false;
        if (!N3DSAudio_streamSeekSamples(inst, inst->bcwav.loopStart)) return false;
        startSample = inst->currentSample;
    }

    uint32_t endSample = startSample + N3DS_STREAM_CHUNK_SAMPLES;
    if (endSample > inst->sampleCount) endSample = inst->sampleCount;
    if (inst->loop && inst->bcwav.loop && inst->bcwav.loopEnd > inst->bcwav.loopStart && endSample > inst->bcwav.loopEnd) {
        endSample = inst->bcwav.loopEnd;
    }
    uint32_t sampleCount = endSample - startSample;
    if (sampleCount == 0) return false;

    repeat(inst->channelCount, channelIndex) {
        uint32_t startFrame = startSample / 14u;
        uint32_t frameCount = (sampleCount + 13u) / 14u;
        uint32_t byteOffset = inst->bcwav.channels[channelIndex].dataOffset + startFrame * 8u;
        uint32_t byteCount = frameCount * 8u;
        if (fseek(inst->streamFile, (long) byteOffset, SEEK_SET) != 0) return false;
        if (fread(inst->streamAdpcm[channelIndex][bufferIndex], 1, byteCount, inst->streamFile) != byteCount) return false;
        DSP_FlushDataCache(inst->streamAdpcm[channelIndex][bufferIndex], byteCount);
    }

    ndspAdpcmData* primaryState = &inst->adpcmStates[bufferIndex * 2];
    N3DSDspContext primaryContext = N3DSAudio_contextForSample(&inst->bcwav.channels[0], &inst->decode[0], startSample);
    primaryState->index = primaryContext.predictorScale;
    primaryState->history0 = primaryContext.yn1;
    primaryState->history1 = primaryContext.yn2;

    memset(&inst->waveBufs[bufferIndex], 0, sizeof(inst->waveBufs[bufferIndex]));
    inst->waveBufs[bufferIndex].data_adpcm = inst->streamAdpcm[0][bufferIndex];
    inst->waveBufs[bufferIndex].nsamples = sampleCount;
    inst->waveBufs[bufferIndex].adpcm_data = primaryState;
    inst->waveBufs[bufferIndex].looping = false;
    inst->bufferStartSample[bufferIndex] = startSample;

    if (inst->secondaryChannelId >= 0) {
        ndspAdpcmData* secondaryState = &inst->adpcmStates[bufferIndex * 2 + 1];
        N3DSDspContext secondaryContext = N3DSAudio_contextForSample(&inst->bcwav.channels[1], &inst->decode[1], startSample);
        secondaryState->index = secondaryContext.predictorScale;
        secondaryState->history0 = secondaryContext.yn1;
        secondaryState->history1 = secondaryContext.yn2;

        memset(&inst->secondaryWaveBufs[bufferIndex], 0, sizeof(inst->secondaryWaveBufs[bufferIndex]));
        inst->secondaryWaveBufs[bufferIndex].data_adpcm = inst->streamAdpcm[1][bufferIndex];
        inst->secondaryWaveBufs[bufferIndex].nsamples = sampleCount;
        inst->secondaryWaveBufs[bufferIndex].adpcm_data = secondaryState;
        inst->secondaryWaveBufs[bufferIndex].looping = false;
    }

    bool atLoopBoundary = inst->loop &&
        inst->bcwav.loop &&
        inst->bcwav.loopEnd > inst->bcwav.loopStart &&
        endSample >= inst->bcwav.loopEnd;

    {
        N3DSStreamDecodeState nextDecode[2];
        memcpy(nextDecode, inst->decode, sizeof(nextDecode));

        uint32_t framesToAdvance = (sampleCount + 13u) / 14u;
        repeat(inst->channelCount, channelIndex) {
            uint32_t channelSample = startSample;
            repeat(framesToAdvance, frameIndex) {
                const uint8_t* frame = inst->streamAdpcm[channelIndex][bufferIndex] + frameIndex * 8u;
                int16_t scratch[14];
                nextDecode[channelIndex].predictorScale = frame[0];
                N3DSAudio_decodeFrame(
                    &inst->bcwav.channels[channelIndex],
                    frame,
                    &nextDecode[channelIndex].hist1,
                    &nextDecode[channelIndex].hist2,
                    scratch
                );
                nextDecode[channelIndex].nextFrameIndex++;

                uint32_t remaining = endSample - channelSample;
                uint32_t frameSamples = remaining > 14u ? 14u : remaining;
                channelSample += frameSamples;
            }
        }

        if (atLoopBoundary) {
            uint32_t loopFrame = inst->bcwav.loopStart / 14u;
            repeat(inst->channelCount, channelIndex) {
                inst->decode[channelIndex].nextFrameIndex = loopFrame;
                inst->decode[channelIndex].predictorScale = inst->bcwav.channels[channelIndex].loopContext.predictorScale;
                inst->decode[channelIndex].hist1 = inst->bcwav.channels[channelIndex].loopContext.yn1;
                inst->decode[channelIndex].hist2 = inst->bcwav.channels[channelIndex].loopContext.yn2;
            }
            inst->currentSample = inst->bcwav.loopStart;
        } else {
            memcpy(inst->decode, nextDecode, sizeof(nextDecode));
            inst->currentSample = endSample;
        }
    }

    return true;
}

static bool N3DSAudio_primeStream(N3DSSoundInstance* inst) {
    inst->streamFinished = false;
    N3DSAudio_resetInstanceWaveBufState(inst);
    repeat(N3DS_STREAM_BUFFER_COUNT, i) {
        bool queued = inst->useNativeAdpcm ? N3DSAudio_fillNativeStreamWaveBuf(inst, i) : N3DSAudio_fillStreamWaveBuf(inst, i);
        if (!queued) break;
        ndspChnWaveBufAdd(inst->channelId, &inst->waveBufs[i]);
        if (inst->useNativeAdpcm && inst->secondaryChannelId >= 0) {
            ndspChnWaveBufAdd(inst->secondaryChannelId, &inst->secondaryWaveBufs[i]);
        }
    }

    repeat(N3DS_STREAM_BUFFER_COUNT, i) {
        if (inst->waveBufs[i].status == NDSP_WBUF_QUEUED || inst->waveBufs[i].status == NDSP_WBUF_PLAYING) {
            return true;
        }
    }
    return false;
}

static void N3DSAudio_waitForAsyncFillIdleLocked(N3DSAudioSystem* audio, N3DSSoundInstance* inst) {
    if (audio == NULL || inst == NULL || !audio->workerEnabled) return;
    while (inst->asyncFill.inProgress) {
        CondVar_Wait(&audio->workerCond, &audio->lock);
    }
}

static void N3DSAudio_cancelAsyncFillLocked(N3DSAudioSystem* audio, N3DSSoundInstance* inst) {
    if (audio == NULL || inst == NULL || !inst->isStream) return;
    N3DSAudio_waitForAsyncFillIdleLocked(audio, inst);
    inst->streamGeneration += 1u;
    memset(&inst->asyncFill, 0, sizeof(inst->asyncFill));
}

static void N3DSAudio_requestAsyncFillLocked(N3DSAudioSystem* audio, N3DSSoundInstance* inst, int bufferIndex) {
    if (audio == NULL || inst == NULL || !audio->workerEnabled || !inst->isStream) return;
    if (inst->asyncFill.requested || inst->asyncFill.inProgress || inst->asyncFill.ready) return;

    inst->asyncFill.requested = true;
    inst->asyncFill.inProgress = false;
    inst->asyncFill.ready = false;
    inst->asyncFill.success = false;
    inst->asyncFill.bufferIndex = bufferIndex;
    inst->asyncFill.generation = inst->streamGeneration;
    LightEvent_Signal(&audio->workerEvent);
}

static bool N3DSAudio_applyAsyncFillLocked(N3DSAudioSystem* audio, N3DSSoundInstance* inst) {
    if (audio == NULL || inst == NULL || !inst->asyncFill.ready) return false;

    int bufferIndex = inst->asyncFill.bufferIndex;
    bool success = inst->asyncFill.success;
    bool useNativeAdpcm = inst->useNativeAdpcm;

    if (success) {
        N3DSAudio_applyFillCursor(inst, &inst->asyncFill.nextCursor);

        memset(&inst->waveBufs[bufferIndex], 0, sizeof(inst->waveBufs[bufferIndex]));
        inst->bufferStartSample[bufferIndex] = inst->asyncFill.bufferStartSample;
        if (useNativeAdpcm) {
            inst->adpcmStates[bufferIndex * 2] = inst->asyncFill.adpcmStates[0];
            inst->waveBufs[bufferIndex].data_adpcm = inst->streamAdpcm[0][bufferIndex];
            inst->waveBufs[bufferIndex].nsamples = inst->asyncFill.sampleCount;
            inst->waveBufs[bufferIndex].adpcm_data = &inst->adpcmStates[bufferIndex * 2];
            inst->waveBufs[bufferIndex].looping = false;

            if (inst->secondaryChannelId >= 0) {
                memset(&inst->secondaryWaveBufs[bufferIndex], 0, sizeof(inst->secondaryWaveBufs[bufferIndex]));
                inst->adpcmStates[bufferIndex * 2 + 1] = inst->asyncFill.adpcmStates[1];
                inst->secondaryWaveBufs[bufferIndex].data_adpcm = inst->streamAdpcm[1][bufferIndex];
                inst->secondaryWaveBufs[bufferIndex].nsamples = inst->asyncFill.sampleCount;
                inst->secondaryWaveBufs[bufferIndex].adpcm_data = &inst->adpcmStates[bufferIndex * 2 + 1];
                inst->secondaryWaveBufs[bufferIndex].looping = false;
            }
        } else {
            inst->waveBufs[bufferIndex].data_pcm16 = inst->streamPcm[bufferIndex];
            inst->waveBufs[bufferIndex].nsamples = inst->asyncFill.sampleCount;
            inst->waveBufs[bufferIndex].looping = false;
        }

        ndspChnWaveBufAdd(inst->channelId, &inst->waveBufs[bufferIndex]);
        if (useNativeAdpcm && inst->secondaryChannelId >= 0) {
            ndspChnWaveBufAdd(inst->secondaryChannelId, &inst->secondaryWaveBufs[bufferIndex]);
        }
    } else {
        inst->streamFinished = true;
    }

    memset(&inst->asyncFill, 0, sizeof(inst->asyncFill));
    CondVar_Broadcast(&audio->workerCond);
    return success;
}

static bool N3DSAudio_hasActiveStreamPlaybackLocked(const N3DSAudioSystem* audio) {
    if (audio == NULL) return false;
    repeat(N3DS_MAX_SOUND_INSTANCES, i) {
        const N3DSSoundInstance* inst = &audio->instances[i];
        if (!inst->active || !inst->isStream) continue;
        if (inst->streamFinished) continue;
        return true;
    }
    return false;
}

static bool N3DSAudio_runCachePrewarmJob(N3DSAudioSystem* audio) {
    if (audio == NULL) return false;

    int32_t soundIndex = -1;

    LightLock_Lock(&audio->lock);
    if (audio->workerStop) {
        LightLock_Unlock(&audio->lock);
        return false;
    }
    if (audio->pendingPrewarmSoundCount == 0 || audio->pendingPrewarmByteBudget == 0 || audio->cachedSounds == NULL) {
        LightLock_Unlock(&audio->lock);
        return false;
    }
    if (N3DSAudio_hasActiveStreamPlaybackLocked(audio)) {
        LightLock_Unlock(&audio->lock);
        return false;
    }

    uint32_t scanned = 0;
    while (scanned < audio->cachedSoundCount) {
        uint32_t candidateIndex = audio->cachedSoundPrewarmCursor;
        audio->cachedSoundPrewarmCursor++;
        if (audio->cachedSoundPrewarmCursor >= audio->cachedSoundCount) {
            audio->cachedSoundPrewarmCursor = 0;
        }
        scanned++;

        N3DSCachedSound* cachedSound = &audio->cachedSounds[candidateIndex];
        if (cachedSound->attempted) continue;

        Sound* sound = &audio->base.audioGroups[0]->sond.sounds[candidateIndex];
        if (!N3DSAudio_shouldPreloadSound(sound)) {
            cachedSound->attempted = true;
            continue;
        }

        cachedSound->attempted = true;
        soundIndex = (int32_t) candidateIndex;
        break;
    }

    if (soundIndex < 0) {
        audio->pendingPrewarmSoundCount = 0;
        audio->pendingPrewarmByteBudget = 0;
        LightLock_Unlock(&audio->lock);
        return false;
    }
    LightLock_Unlock(&audio->lock);

    DataWin* dw = audio->base.audioGroups[0];
    Sound* sound = &dw->sond.sounds[soundIndex];
    char* path = NULL;
    uint8_t* blob = NULL;
    uint32_t blobSize = 0;
    N3DSBcwav bcwav;
    bool loaded = false;
    bool blobOwned = false;
    memset(&bcwav, 0, sizeof(bcwav));

    const uint8_t* packedBlob = NULL;
    uint32_t packedBlobSize = 0;
    const N3DSPackedSoundBankEntry* packedEntry = N3DSAudio_getPackedSoundEntry(audio, soundIndex);
    if (!N3DSAudio_soundLooksLikeMusic(sound) &&
        packedEntry != NULL &&
        !N3DSAudio_packedSoundEntryIsPcm16(packedEntry) &&
        N3DSAudio_getPackedSoundBlob(audio, soundIndex, &packedBlob, &packedBlobSize) &&
        N3DSAudio_parseBcwavBlob(packedBlob, packedBlobSize, &bcwav) &&
        N3DSAudio_shouldPrewarmCachedSound(sound, &bcwav)) {
        blob = (uint8_t*) packedBlob;
        blobSize = packedBlobSize;
        loaded = true;
    } else {
        path = N3DSAudio_resolveSoundPath(audio, sound, soundIndex);
        if (path == NULL) {
            path = N3DSAudio_tryResolveGeneratedSoundPath(audio, sound, soundIndex);
        }

        if (path != NULL &&
            N3DSAudio_readFileFully(path, &blob, &blobSize) &&
            N3DSAudio_parseBcwavBlob(blob, blobSize, &bcwav) &&
            N3DSAudio_shouldPrewarmCachedSound(sound, &bcwav)) {
            loaded = true;
            blobOwned = true;
        }
    }

    LightLock_Lock(&audio->lock);
    N3DSCachedSound* cachedSound = &audio->cachedSounds[soundIndex];
    if (loaded && (!blobOwned || audio->cachedSoundBytes + blobSize <= audio->maxCachedSoundBytes)) {
        cachedSound->available = true;
        cachedSound->path = path;
        cachedSound->blob = blob;
        cachedSound->blobOwned = blobOwned;
        cachedSound->blobSize = blobSize;
        cachedSound->bcwav = bcwav;
        if (blobOwned) audio->cachedSoundBytes += blobSize;
        if (audio->pendingPrewarmSoundCount > 0) audio->pendingPrewarmSoundCount--;
        if (audio->pendingPrewarmByteBudget > blobSize) audio->pendingPrewarmByteBudget -= blobSize;
        else audio->pendingPrewarmByteBudget = 0;
        path = NULL;
        blob = NULL;
    }
    LightLock_Unlock(&audio->lock);

    free(path);
    if (blobOwned) free(blob);
    return true;
}

static bool N3DSAudio_hasPendingAsyncFillLocked(const N3DSAudioSystem* audio) {
    if (audio == NULL || !audio->workerEnabled) return false;
    repeat(N3DS_MAX_SOUND_INSTANCES, i) {
        const N3DSSoundInstance* inst = &audio->instances[i];
        if (!inst->active || !inst->isStream) continue;
        if (inst->asyncFill.requested || inst->asyncFill.inProgress) return true;
    }
    return false;
}

static void N3DSAudio_streamWorkerMain(void* arg) {
    N3DSAudioSystem* audio = (N3DSAudioSystem*) arg;
    if (audio == NULL) threadExit(0);

    while (true) {
        LightEvent_Wait(&audio->workerEvent);

        while (true) {
            int32_t jobSlot = -1;
            int bufferIndex = -1;
            uint32_t generation = 0;
            bool useNativeAdpcm = false;
            N3DSStreamFillCursor cursor;
            memset(&cursor, 0, sizeof(cursor));

            LightLock_Lock(&audio->lock);
            if (audio->workerStop) {
                LightLock_Unlock(&audio->lock);
                threadExit(0);
            }

            repeat(N3DS_MAX_SOUND_INSTANCES, i) {
                N3DSSoundInstance* inst = &audio->instances[i];
                if (!inst->active || !inst->isStream) continue;
                if (!inst->asyncFill.requested || inst->asyncFill.inProgress || inst->asyncFill.ready) continue;
                inst->asyncFill.inProgress = true;
                jobSlot = (int32_t) i;
                bufferIndex = inst->asyncFill.bufferIndex;
                generation = inst->asyncFill.generation;
                useNativeAdpcm = inst->useNativeAdpcm;
                N3DSAudio_captureFillCursor(inst, &cursor);
                break;
            }
            LightLock_Unlock(&audio->lock);

            if (jobSlot < 0) {
                if (!N3DSAudio_runCachePrewarmJob(audio)) break;
                continue;
            }

            N3DSSoundInstance* inst = &audio->instances[jobSlot];
            uint32_t startSample = 0;
            uint32_t producedSamples = 0;
            ndspAdpcmData adpcmStates[2];
            memset(adpcmStates, 0, sizeof(adpcmStates));
            bool success = useNativeAdpcm
                ? N3DSAudio_fillNativeStreamWaveBufAsync(inst, bufferIndex, &cursor, &startSample, &producedSamples, adpcmStates)
                : N3DSAudio_fillStreamWaveBufAsync(inst, bufferIndex, &cursor, &startSample, &producedSamples);

            LightLock_Lock(&audio->lock);
            inst = &audio->instances[jobSlot];
            if (inst->active &&
                inst->isStream &&
                inst->asyncFill.inProgress &&
                inst->asyncFill.requested &&
                inst->asyncFill.generation == generation &&
                inst->asyncFill.bufferIndex == bufferIndex) {
                inst->asyncFill.inProgress = false;
                inst->asyncFill.ready = true;
                inst->asyncFill.success = success;
                inst->asyncFill.bufferStartSample = startSample;
                inst->asyncFill.sampleCount = producedSamples;
                inst->asyncFill.nextCursor = cursor;
                inst->asyncFill.adpcmStates[0] = adpcmStates[0];
                inst->asyncFill.adpcmStates[1] = adpcmStates[1];
            }
            CondVar_Broadcast(&audio->workerCond);
            bool morePending = N3DSAudio_hasPendingAsyncFillLocked(audio);
            bool shouldStop = audio->workerStop;
            LightLock_Unlock(&audio->lock);

            if (shouldStop) threadExit(0);
            if (!morePending) break;
        }
    }
}

static bool N3DSAudio_startFileNativeAdpcmStreamPlayback(
    N3DSAudioSystem* audio,
    N3DSSoundInstance* inst,
    const char* path,
    const N3DSBcwav* bcwav
) {
    if (audio == NULL || inst == NULL || path == NULL || bcwav == NULL) return false;
    if (!N3DSAudio_canUseNativeAdpcmPlayback(bcwav, inst->loop)) return false;

    FILE* file = fopen(path, "rb");
    if (file == NULL) return false;
    N3DSAudio_configureFileBuffer(file);

    inst->streamFile = file;
    inst->channelId = N3DSAudio_acquireChannel(audio);
    if (inst->channelId < 0) {
        N3DSAudio_cleanupPartialStreamResources(audio, inst);
        return false;
    }
    if (bcwav->channelCount == 2) {
        inst->secondaryChannelId = N3DSAudio_acquireChannel(audio);
        if (inst->secondaryChannelId < 0) {
            N3DSAudio_cleanupPartialStreamResources(audio, inst);
            return false;
        }
    }

    inst->isStream = true;
    inst->useNativeAdpcm = true;
    inst->sampleRate = bcwav->sampleRate;
    inst->sampleCount = bcwav->sampleCount;
    inst->channelCount = bcwav->channelCount;
    inst->baseRate = (float) bcwav->sampleRate;
    inst->bcwav = *bcwav;

    uint32_t maxFrames = (N3DS_STREAM_CHUNK_SAMPLES + 13u) / 14u;
    uint32_t bytesPerBuffer = maxFrames * 8u;
    repeat(inst->channelCount, channelIndex) {
        repeat(N3DS_STREAM_BUFFER_COUNT, bufferIndex) {
            inst->streamAdpcm[channelIndex][bufferIndex] = linearAlloc(bytesPerBuffer);
            if (inst->streamAdpcm[channelIndex][bufferIndex] == NULL) {
                N3DSAudio_cleanupPartialStreamResources(audio, inst);
                return false;
            }
        }
    }

    N3DSAudio_streamResetDecoder(inst);
    N3DSAudio_rebuildInstanceChannels(audio, inst);
    if (!N3DSAudio_primeStream(inst)) {
        N3DSAudio_cleanupPartialStreamResources(audio, inst);
        return false;
    }
    return true;
}

static bool N3DSAudio_startFileStreamPlayback(
    N3DSAudioSystem* audio,
    N3DSSoundInstance* inst,
    const char* path,
    const N3DSBcwav* bcwav
) {
    if (audio == NULL || inst == NULL || path == NULL || bcwav == NULL) return false;
    if (bcwav->sampleRate == 0 || bcwav->channelCount == 0 || bcwav->channelCount > 2) return false;

    FILE* file = fopen(path, "rb");
    if (file == NULL) return false;
    N3DSAudio_configureFileBuffer(file);

    inst->channelId = N3DSAudio_acquireChannel(audio);
    if (inst->channelId < 0) {
        fclose(file);
        return false;
    }

    inst->streamFile = file;
    inst->isStream = true;
    inst->sampleRate = bcwav->sampleRate;
    inst->sampleCount = bcwav->sampleCount;
    inst->channelCount = bcwav->channelCount;
    inst->baseRate = (float) bcwav->sampleRate;
    inst->bcwav = *bcwav;

    size_t stride = bcwav->channelCount;
    size_t samplesPerBuffer = (size_t) N3DS_STREAM_CHUNK_SAMPLES * stride;
    repeat(N3DS_STREAM_BUFFER_COUNT, i) {
        inst->streamPcm[i] = linearAlloc(samplesPerBuffer * sizeof(int16_t));
        if (inst->streamPcm[i] == NULL) {
            return false;
        }
    }

    N3DSAudio_streamResetDecoder(inst);
    N3DSAudio_rebuildInstanceChannels(audio, inst);
    return N3DSAudio_primeStream(inst);
}

static void N3DSAudio_init(AudioSystem* base, DataWin* dataWin, FileSystem* fileSystem) {
    N3DSAudioSystem* audio = (N3DSAudioSystem*) base;
    if (audio == NULL || audio->initialized) return;

    bool isNew3DS = false;
    LightLock_Init(&audio->lock);
    LightLock_Init(&audio->missingPathLock);
    CondVar_Init(&audio->workerCond);
    LightEvent_Init(&audio->workerEvent, RESET_STICKY);
    audio->fileSystem = fileSystem;
    audio->masterGain = 1.0f;
    audio->base.audioGroups = safeCalloc(1, sizeof(DataWin*));
    audio->base.audioGroups[0] = dataWin;
    if (R_SUCCEEDED(APT_CheckNew3DS(&isNew3DS))) {
        audio->isNew3DS = isNew3DS;
    }
#if N3DS_FORCE_OLD3DS_MODE
    audio->isNew3DS = false;
#endif
    audio->maxCachedSoundBytes = audio->isNew3DS ? N3DS_MAX_PRELOADED_SFX_BYTES_NEW3DS : N3DS_MAX_PRELOADED_SFX_BYTES_OLD3DS;
    audio->roomPrewarmSoundCount = audio->isNew3DS ? N3DS_ROOM_PREWARM_SOUND_COUNT_NEW3DS : N3DS_ROOM_PREWARM_SOUND_COUNT_OLD3DS;
    audio->roomPrewarmByteBudget = audio->isNew3DS ? N3DS_ROOM_PREWARM_BYTE_BUDGET_NEW3DS : N3DS_ROOM_PREWARM_BYTE_BUDGET_OLD3DS;
    audio->backgroundPrewarmSoundCount = audio->isNew3DS ? N3DS_BACKGROUND_PREWARM_SOUND_COUNT_NEW3DS : N3DS_BACKGROUND_PREWARM_SOUND_COUNT_OLD3DS;
    audio->backgroundPrewarmByteBudget = audio->isNew3DS ? N3DS_BACKGROUND_PREWARM_BYTE_BUDGET_NEW3DS : N3DS_BACKGROUND_PREWARM_BYTE_BUDGET_OLD3DS;
    N3DSAudio_loadPackedSoundBank(audio);

    ndspInit();
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    N3DSDebugLog_event(
        "audio",
        "init model=%s cacheCapKB=%lu packedBankKB=%lu",
        audio->isNew3DS ? "new3ds" : "old3ds",
        (unsigned long) (audio->maxCachedSoundBytes / 1024u),
        (unsigned long) (audio->packedSoundBankSize / 1024u)
    );
    fprintf(
        stderr,
        "N3DSAudio: init complete, sounds=%lu, model=%s, cacheCap=%luKB, packedBank=%luKB, roomPrewarm=%lu/%luKB, bgPrewarm=%lu/%luKB\n",
        (unsigned long) dataWin->sond.count,
        audio->isNew3DS ? "new3ds" : "old3ds",
        (unsigned long) (audio->maxCachedSoundBytes / 1024u),
        (unsigned long) (audio->packedSoundBankSize / 1024u),
        (unsigned long) audio->roomPrewarmSoundCount,
        (unsigned long) (audio->roomPrewarmByteBudget / 1024u),
        (unsigned long) audio->backgroundPrewarmSoundCount,
        (unsigned long) (audio->backgroundPrewarmByteBudget / 1024u)
    );
    repeat(N3DS_MAX_SOUND_INSTANCES, i) {
        ndspChnReset((int) i);
        ndspChnSetInterp((int) i, NDSP_INTERP_LINEAR);
    }
#if N3DS_EAGER_SFX_PRELOAD
    N3DSAudio_preloadSounds(audio, dataWin);
#else
    audio->cachedSounds = safeCalloc(dataWin->sond.count, sizeof(N3DSCachedSound));
    audio->cachedSoundCount = dataWin->sond.count;
    fprintf(stderr, "N3DSAudio: eager preload disabled; sounds will cache on first use or the worker thread\n");
#endif

    audio->workerEnabled = false;
#if N3DS_ENABLE_STREAM_WORKER
    if (R_SUCCEEDED(APT_SetAppCpuTimeLimit(N3DS_STREAM_WORKER_CPU_LIMIT))) {
        audio->workerThread = threadCreate(
            N3DSAudio_streamWorkerMain,
            audio,
            N3DS_STREAM_WORKER_STACK_SIZE,
            N3DS_STREAM_WORKER_PRIORITY,
            N3DS_STREAM_WORKER_CORE_ID,
            false
        );
        audio->workerEnabled = audio->workerThread != NULL;
    }
#endif
    N3DSDebugLog_event(
        "audio",
        "worker enabled=%d cpuLimit=%lu nativeAdpcm=%d",
        audio->workerEnabled ? 1 : 0,
        (unsigned long) N3DS_STREAM_WORKER_CPU_LIMIT,
        N3DS_FORCE_PCM_BCWAV_PLAYBACK ? 0 : 1
    );
    audio->initialized = true;
}

static void N3DSAudio_destroy(AudioSystem* base) {
    N3DSAudioSystem* audio = (N3DSAudioSystem*) base;
    if (audio->workerEnabled) {
        LightLock_Lock(&audio->lock);
        audio->workerStop = true;
        LightLock_Unlock(&audio->lock);
        LightEvent_Signal(&audio->workerEvent);
        threadJoin(audio->workerThread, U64_MAX);
        threadFree(audio->workerThread);
        audio->workerThread = NULL;
        audio->workerEnabled = false;
    }
    repeat(N3DS_MAX_SOUND_INSTANCES, i) {
        N3DSAudio_releaseInstance(audio, &audio->instances[i]);
    }
    repeat(N3DS_MAX_STREAMS, i) {
        free(audio->streams[i].path);
    }
    repeat(audio->cachedSoundCount, i) {
        N3DSAudio_releaseCachedSound(&audio->cachedSounds[i]);
    }
    free(audio->cachedSounds);
    free(audio->packedSoundBankEntries);
    free(audio->packedSoundBankData);
    ndspExit();
    free(audio->base.audioGroups);
    free(audio);

    repeat(gN3DSMissingAudioPathCount, i) {
        free(gN3DSMissingAudioPaths[i]);
        gN3DSMissingAudioPaths[i] = NULL;
    }
    gN3DSMissingAudioPathCount = 0;
}

static void N3DSAudio_update(AudioSystem* base, MAYBE_UNUSED float deltaTime) {
    N3DSAudioSystem* audio = (N3DSAudioSystem*) base;
    u64 nowTick = svcGetSystemTick();
    LightLock_Lock(&audio->lock);
#if !N3DS_EAGER_SFX_PRELOAD
    N3DSAudio_requestCachePrewarmLocked(audio, audio->backgroundPrewarmSoundCount, audio->backgroundPrewarmByteBudget);
#endif
    repeat(N3DS_MAX_SOUND_INSTANCES, i) {
        N3DSSoundInstance* inst = &audio->instances[i];
        if (!inst->active) continue;

        if (!inst->isStream) {
            bool finished = N3DSAudio_nonStreamPlaybackFinished(inst, nowTick);
            if (finished && !inst->loop) {
                N3DSAudio_releaseInstance(audio, inst);
            }
            continue;
        }

        bool anyQueued = false;
        repeat(N3DS_STREAM_BUFFER_COUNT, bufferIndex) {
            ndspWaveBuf* waveBuf = &inst->waveBufs[bufferIndex];
            bool secondaryReadyToReuse = true;
            if (inst->useNativeAdpcm && inst->secondaryChannelId >= 0) {
                secondaryReadyToReuse = inst->secondaryWaveBufs[bufferIndex].status == NDSP_WBUF_DONE;
            }
            if (waveBuf->status == NDSP_WBUF_DONE && secondaryReadyToReuse) {
                waveBuf->status = NDSP_WBUF_FREE;
                if (inst->useNativeAdpcm && inst->secondaryChannelId >= 0) {
                    inst->secondaryWaveBufs[bufferIndex].status = NDSP_WBUF_FREE;
                }
            }
            if (waveBuf->status == NDSP_WBUF_FREE &&
                inst->asyncFill.ready &&
                inst->asyncFill.bufferIndex == (int) bufferIndex) {
                N3DSAudio_applyAsyncFillLocked(audio, inst);
            }
            if (waveBuf->status == NDSP_WBUF_FREE &&
                !inst->streamFinished &&
                !inst->asyncFill.requested &&
                !inst->asyncFill.inProgress &&
                !inst->asyncFill.ready) {
                if (audio->workerEnabled) N3DSAudio_requestAsyncFillLocked(audio, inst, (int) bufferIndex);
                else {
                    bool refilled = inst->useNativeAdpcm ?
                        N3DSAudio_fillNativeStreamWaveBuf(inst, (int) bufferIndex) :
                        N3DSAudio_fillStreamWaveBuf(inst, (int) bufferIndex);
                    if (!inst->streamFinished && refilled) {
                        ndspChnWaveBufAdd(inst->channelId, waveBuf);
                        if (inst->useNativeAdpcm && inst->secondaryChannelId >= 0) {
                            ndspChnWaveBufAdd(inst->secondaryChannelId, &inst->secondaryWaveBufs[bufferIndex]);
                        }
                    }
                }
            }
            bool primaryBusy = waveBuf->status != NDSP_WBUF_FREE;
            bool secondaryBusy = inst->useNativeAdpcm &&
                inst->secondaryChannelId >= 0 &&
                inst->secondaryWaveBufs[bufferIndex].status != NDSP_WBUF_FREE;
            if (primaryBusy || secondaryBusy) {
                anyQueued = true;
            }
        }

        if (!anyQueued &&
            !inst->asyncFill.requested &&
            !inst->asyncFill.inProgress &&
            !inst->asyncFill.ready) {
            inst->streamFinished = true;
            N3DSAudio_releaseInstance(audio, inst);
        }
    }
    LightLock_Unlock(&audio->lock);
}

static int32_t N3DSAudio_playSound(AudioSystem* base, int32_t soundIndex, MAYBE_UNUSED int32_t priority, bool loop) {
    N3DSAudioSystem* audio = (N3DSAudioSystem*) base;
    LightLock_Lock(&audio->lock);
    char* path = NULL;
    bool useStreamingPath = soundIndex >= N3DS_AUDIO_STREAM_INDEX_BASE;
    bool useMusicStreamPath = false;
    N3DSBcwav streamBcwav;
    memset(&streamBcwav, 0, sizeof(streamBcwav));
    Sound* sound = NULL;
    N3DSCachedSound* cachedSound = NULL;
    const uint8_t* packedBlob = NULL;
    uint32_t packedBlobSize = 0;
    bool hasPackedBlob = false;

    if (useStreamingPath) {
        N3DSStreamEntry* stream = N3DSAudio_getActiveStreamEntry(audio, soundIndex);
        if (stream == NULL) {
            LightLock_Unlock(&audio->lock);
            return -1;
        }
        path = safeStrdup(stream->path);
        streamBcwav = stream->bcwav;
    } else {
        DataWin* dw = audio->base.audioGroups[0];
        if (soundIndex < 0 || (uint32_t) soundIndex >= dw->sond.count) {
            LightLock_Unlock(&audio->lock);
            return -1;
        }
        sound = &dw->sond.sounds[soundIndex];
        cachedSound = N3DSAudio_getCachedSoundIfAvailable(audio, soundIndex);
        if (!N3DSAudio_soundLooksLikeMusic(sound)) {
            hasPackedBlob = N3DSAudio_getPackedSoundBlob(audio, soundIndex, &packedBlob, &packedBlobSize);
        }
        useMusicStreamPath = sound != NULL && loop && N3DSAudio_soundLooksLikeMusic(sound);
        if (useMusicStreamPath || (!hasPackedBlob && cachedSound == NULL)) {
            const char* resolvedPath = N3DSAudio_getResolvedCachedSoundPath(audio, soundIndex, sound);
            if (resolvedPath != NULL) path = safeStrdup(resolvedPath);
        }
    }

    if (!useStreamingPath && !useMusicStreamPath && loop && N3DSAudio_pathLooksLikeBundledMusic(path)) {
        useMusicStreamPath = true;
    }

    if (path == NULL && !hasPackedBlob && cachedSound == NULL) {
        if (audio->lastResolveFailureSound != soundIndex) {
            audio->lastResolveFailureSound = soundIndex;
            N3DSDebugLog_event("audio_fail", "resolve failed sound=%ld", (long) soundIndex);
            fprintf(
                stderr,
                "N3DSAudio: could not resolve sound %ld name=%s file=%s audioFile=%ld group=%ld\n",
                (long) soundIndex,
                sound != NULL && sound->name != NULL ? sound->name : "<null>",
                sound != NULL && sound->file != NULL ? sound->file : "<null>",
                sound != NULL ? (long) sound->audioFile : -1L,
                sound != NULL ? (long) sound->audioGroup : -1L
            );
        }
        LightLock_Unlock(&audio->lock);
        return -1;
    }

    bool shouldReplaceExistingInstance = useStreamingPath || (useMusicStreamPath && loop);
    if (shouldReplaceExistingInstance) {
        repeat(N3DS_MAX_SOUND_INSTANCES, i) {
            if (audio->instances[i].active && audio->instances[i].soundIndex == soundIndex) {
                N3DSAudio_releaseInstance(audio, &audio->instances[i]);
            }
        }
    }

    N3DSSoundInstance* inst = N3DSAudio_findFreeInstance(audio, useStreamingPath || useMusicStreamPath);
    if (inst == NULL) {
        free(path);
        LightLock_Unlock(&audio->lock);
        return -1;
    }

    int32_t slot = (int32_t) (inst - audio->instances);
    memset(inst, 0, sizeof(*inst));
    inst->active = true;
    inst->loop = loop;
    inst->soundIndex = soundIndex;
    inst->instanceId = N3DS_SOUND_INSTANCE_ID_BASE + slot;
    inst->channelId = -1;
    inst->secondaryChannelId = -1;
    inst->gain = N3DSAudio_sanitizeGain(sound != NULL ? sound->volume : 1.0f);
    inst->pitch = N3DSAudio_sanitizePitch((sound != NULL && sound->pitch > 0.0f) ? sound->pitch : 1.0f);
    inst->streamGeneration = 1u;

    if (useStreamingPath || useMusicStreamPath) {
        N3DSBcwav musicBcwav;
        memset(&musicBcwav, 0, sizeof(musicBcwav));

        N3DSStreamEntry* stream = N3DSAudio_getActiveStreamEntry(audio, soundIndex);
        if (useStreamingPath) {
            if (stream == NULL) {
                free(path);
                N3DSAudio_releaseInstance(audio, inst);
                LightLock_Unlock(&audio->lock);
                return -1;
            }
            inst->gain = stream->gain;
            inst->pitch = stream->pitch;
            musicBcwav = stream->bcwav;
        } else {
            if (!N3DSAudio_parseBcwavFile(path, &musicBcwav)) {
                fprintf(stderr, "N3DSAudio: failed to parse music header %s\n", path);
                free(path);
                N3DSAudio_releaseInstance(audio, inst);
                LightLock_Unlock(&audio->lock);
                return -1;
            }
        }

        bool started = N3DSAudio_startFileNativeAdpcmStreamPlayback(audio, inst, path, &musicBcwav);
        if (!started) {
            N3DSDebugLog_event("audio_fail", "stream start failed sound=%ld path=%s", (long) soundIndex, path);
            free(path);
            N3DSAudio_releaseInstance(audio, inst);
            LightLock_Unlock(&audio->lock);
            return -1;
        }
        N3DSDebugLog_event(
            "audio_start",
            "stream sound=%ld instance=%ld native=%d loop=%d rate=%lu ch=%lu path=%s",
            (long) soundIndex,
            (long) inst->instanceId,
            inst->useNativeAdpcm ? 1 : 0,
            loop ? 1 : 0,
            (unsigned long) inst->sampleRate,
            (unsigned long) inst->channelCount,
            path
        );
        free(path);
        int32_t instanceId = inst->instanceId;
        LightLock_Unlock(&audio->lock);
        return instanceId;
    }

    const uint8_t* blob = NULL;
    uint32_t blobSize = 0;
    N3DSBcwav bcwav;
    const N3DSPackedSoundBankEntry* packedEntry = hasPackedBlob ? N3DSAudio_getPackedSoundEntry(audio, soundIndex) : NULL;
    bool uncachedBlobLinear = false;
    memset(&bcwav, 0, sizeof(bcwav));

    if (cachedSound != NULL) {
        blob = cachedSound->blob;
        blobSize = cachedSound->blobSize;
        bcwav = cachedSound->bcwav;
    } else if (hasPackedBlob && packedEntry != NULL && N3DSAudio_packedSoundEntryIsPcm16(packedEntry)) {
        uint8_t channelCount = N3DSAudio_packedSoundEntryChannelCount(packedEntry);
        size_t expectedBytes = (size_t) packedEntry->sampleCount * channelCount * sizeof(int16_t);
        if (packedEntry->sampleRate == 0 ||
            packedEntry->sampleCount == 0 ||
            channelCount == 0 ||
            channelCount > 2 ||
            expectedBytes != packedBlobSize) {
            N3DSDebugLog_event("audio_fail", "bank pcm metadata invalid sound=%ld", (long) soundIndex);
            N3DSAudio_releaseInstance(audio, inst);
            LightLock_Unlock(&audio->lock);
            return -1;
        }

        int16_t* pcm = (int16_t*) N3DSAudio_cloneBlobToLinear(packedBlob, packedBlobSize);
        if (pcm == NULL || !N3DSAudio_startOwnedPcmPlayback(audio, inst, pcm, packedEntry->sampleRate, packedEntry->sampleCount, channelCount, loop)) {
            if (pcm != NULL) linearFree(pcm);
            N3DSDebugLog_event("audio_fail", "bank pcm start failed sound=%ld", (long) soundIndex);
            N3DSAudio_releaseInstance(audio, inst);
            LightLock_Unlock(&audio->lock);
            return -1;
        }

        int32_t instanceId = inst->instanceId;
        LightLock_Unlock(&audio->lock);
        return instanceId;
    } else if (hasPackedBlob) {
        blob = packedBlob;
        blobSize = packedBlobSize;
        if (!N3DSAudio_getCachedSoundHeader(audio, soundIndex, sound, &bcwav) &&
            !N3DSAudio_parseBcwavBlob(blob, blobSize, &bcwav)) {
            N3DSDebugLog_event("audio_fail", "bank parse failed sound=%ld", (long) soundIndex);
            N3DSAudio_releaseInstance(audio, inst);
            LightLock_Unlock(&audio->lock);
            return -1;
        }
    } else {
        uint8_t* uncachedBlob = NULL;
        if (!N3DSAudio_readFileFullyLinear(path, &uncachedBlob, &blobSize)) {
            N3DSDebugLog_event("audio_fail", "read failed path=%s sound=%ld", path, (long) soundIndex);
            fprintf(stderr, "N3DSAudio: failed to read %s\n", path);
            free(path);
            N3DSAudio_releaseInstance(audio, inst);
            LightLock_Unlock(&audio->lock);
            return -1;
        }
        blob = uncachedBlob;
        uncachedBlobLinear = true;
        if (!N3DSAudio_getCachedSoundHeader(audio, soundIndex, sound, &bcwav) &&
            !N3DSAudio_parseBcwavBlob(blob, blobSize, &bcwav)) {
            N3DSDebugLog_event("audio_fail", "blob parse failed sound=%ld path=%s", (long) soundIndex, path);
            fprintf(stderr, "N3DSAudio: failed to parse in-memory BCWAV\n");
            linearFree((void*) blob);
            free(path);
            N3DSAudio_releaseInstance(audio, inst);
            LightLock_Unlock(&audio->lock);
            return -1;
        }
    }
    free(path);
    if (cachedSound == NULL && soundIndex >= 0 && (uint32_t) soundIndex < audio->cachedSoundCount) {
        N3DSCachedSound* headerCache = &audio->cachedSounds[soundIndex];
        headerCache->headerAttempted = true;
        headerCache->headerAvailable = true;
        headerCache->bcwav = bcwav;
    }

    if (N3DSAudio_canUseNativeAdpcmPlayback(&bcwav, loop)) {
        bool started = false;
        if (cachedSound != NULL) {
            started = N3DSAudio_startNativeAdpcmPlayback(audio, inst, blob, blobSize, &bcwav);
        } else if (hasPackedBlob) {
            started = N3DSAudio_startNativeAdpcmPlayback(audio, inst, blob, blobSize, &bcwav);
        } else {
            started = uncachedBlobLinear
                ? N3DSAudio_startOwnedLinearNativeAdpcmPlayback(audio, inst, blob, blobSize, &bcwav)
                : N3DSAudio_startNativeAdpcmPlayback(audio, inst, blob, blobSize, &bcwav);
        }
        if (!started) {
            N3DSDebugLog_event("audio_fail", "native start failed sound=%ld", (long) soundIndex);
            if (cachedSound == NULL && !hasPackedBlob) {
                if (uncachedBlobLinear) linearFree((void*) blob);
                else free((void*) blob);
            }
            N3DSAudio_releaseInstance(audio, inst);
            LightLock_Unlock(&audio->lock);
            return -1;
        }
        int32_t instanceId = inst->instanceId;
        LightLock_Unlock(&audio->lock);
        return instanceId;
    }

    int16_t* pcm = N3DSAudio_decodeBcwavToPcm(blob, &bcwav);
    if (cachedSound == NULL && !hasPackedBlob) {
        if (uncachedBlobLinear) linearFree((void*) blob);
        else free((void*) blob);
    }
    if (pcm == NULL) {
        N3DSDebugLog_event("audio_fail", "pcm decode failed sound=%ld", (long) soundIndex);
        N3DSAudio_releaseInstance(audio, inst);
        LightLock_Unlock(&audio->lock);
        return -1;
    }
    if (!N3DSAudio_startOwnedPcmPlayback(audio, inst, pcm, bcwav.sampleRate, bcwav.sampleCount, bcwav.channelCount, loop)) {
        linearFree(pcm);
        N3DSAudio_releaseInstance(audio, inst);
        LightLock_Unlock(&audio->lock);
        return -1;
    }

    int32_t instanceId = inst->instanceId;
    LightLock_Unlock(&audio->lock);
    return instanceId;
}

static void N3DSAudio_prewarmRoom(AudioSystem* base, MAYBE_UNUSED Runner* runner) {
#if !N3DS_EAGER_SFX_PRELOAD
    N3DSAudioSystem* audio = (N3DSAudioSystem*) base;
    LightLock_Lock(&audio->lock);
    N3DSAudio_requestCachePrewarmLocked(audio, audio->roomPrewarmSoundCount, audio->roomPrewarmByteBudget);
    LightLock_Unlock(&audio->lock);
#else
    (void) base;
    (void) runner;
#endif
}

static void N3DSAudio_stopSound(AudioSystem* base, int32_t soundOrInstance) {
    N3DSAudioSystem* audio = (N3DSAudioSystem*) base;
    LightLock_Lock(&audio->lock);
    if (N3DSAudio_isInstanceId(soundOrInstance)) {
        N3DSSoundInstance* inst = N3DSAudio_findInstanceById(audio, soundOrInstance);
        if (inst != NULL) N3DSAudio_releaseInstance(audio, inst);
        LightLock_Unlock(&audio->lock);
        return;
    }
    repeat(N3DS_MAX_SOUND_INSTANCES, i) {
        if (audio->instances[i].active && audio->instances[i].soundIndex == soundOrInstance) {
            N3DSAudio_releaseInstance(audio, &audio->instances[i]);
        }
    }
    LightLock_Unlock(&audio->lock);
}

static void N3DSAudio_stopAll(AudioSystem* base) {
    N3DSAudioSystem* audio = (N3DSAudioSystem*) base;
    LightLock_Lock(&audio->lock);
    repeat(N3DS_MAX_SOUND_INSTANCES, i) {
        N3DSAudio_releaseInstance(audio, &audio->instances[i]);
    }
    LightLock_Unlock(&audio->lock);
}

static bool N3DSAudio_isPlaying(AudioSystem* base, int32_t soundOrInstance) {
    N3DSAudioSystem* audio = (N3DSAudioSystem*) base;
    LightLock_Lock(&audio->lock);
    if (N3DSAudio_isInstanceId(soundOrInstance)) {
        N3DSSoundInstance* inst = N3DSAudio_findInstanceById(audio, soundOrInstance);
        bool isPlaying = inst != NULL && ndspChnIsPlaying(inst->channelId) && !inst->paused;
        LightLock_Unlock(&audio->lock);
        return isPlaying;
    }
    repeat(N3DS_MAX_SOUND_INSTANCES, i) {
        if (audio->instances[i].active && audio->instances[i].soundIndex == soundOrInstance) {
            bool isPlaying = ndspChnIsPlaying(audio->instances[i].channelId) && !audio->instances[i].paused;
            LightLock_Unlock(&audio->lock);
            return isPlaying;
        }
    }
    LightLock_Unlock(&audio->lock);
    return false;
}

static void N3DSAudio_pauseSound(AudioSystem* base, int32_t soundOrInstance) {
    N3DSAudioSystem* audio = (N3DSAudioSystem*) base;
    LightLock_Lock(&audio->lock);
    if (N3DSAudio_isInstanceId(soundOrInstance)) {
        N3DSSoundInstance* inst = N3DSAudio_findInstanceById(audio, soundOrInstance);
        if (inst != NULL) {
            inst->paused = true;
            ndspChnSetPaused(inst->channelId, true);
            if (inst->secondaryChannelId >= 0) ndspChnSetPaused(inst->secondaryChannelId, true);
        }
        LightLock_Unlock(&audio->lock);
        return;
    }
    repeat(N3DS_MAX_SOUND_INSTANCES, i) {
        N3DSSoundInstance* inst = &audio->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance) {
            inst->paused = true;
            ndspChnSetPaused(inst->channelId, true);
            if (inst->secondaryChannelId >= 0) ndspChnSetPaused(inst->secondaryChannelId, true);
        }
    }
    LightLock_Unlock(&audio->lock);
}

static void N3DSAudio_resumeSound(AudioSystem* base, int32_t soundOrInstance) {
    N3DSAudioSystem* audio = (N3DSAudioSystem*) base;
    LightLock_Lock(&audio->lock);
    if (N3DSAudio_isInstanceId(soundOrInstance)) {
        N3DSSoundInstance* inst = N3DSAudio_findInstanceById(audio, soundOrInstance);
        if (inst != NULL) {
            inst->paused = false;
            ndspChnSetPaused(inst->channelId, false);
            if (inst->secondaryChannelId >= 0) ndspChnSetPaused(inst->secondaryChannelId, false);
        }
        LightLock_Unlock(&audio->lock);
        return;
    }
    repeat(N3DS_MAX_SOUND_INSTANCES, i) {
        N3DSSoundInstance* inst = &audio->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance) {
            inst->paused = false;
            ndspChnSetPaused(inst->channelId, false);
            if (inst->secondaryChannelId >= 0) ndspChnSetPaused(inst->secondaryChannelId, false);
        }
    }
    LightLock_Unlock(&audio->lock);
}

static void N3DSAudio_pauseAll(AudioSystem* base) {
    N3DSAudioSystem* audio = (N3DSAudioSystem*) base;
    LightLock_Lock(&audio->lock);
    repeat(N3DS_MAX_SOUND_INSTANCES, i) {
        if (audio->instances[i].active) {
            audio->instances[i].paused = true;
            ndspChnSetPaused(audio->instances[i].channelId, true);
            if (audio->instances[i].secondaryChannelId >= 0) ndspChnSetPaused(audio->instances[i].secondaryChannelId, true);
        }
    }
    LightLock_Unlock(&audio->lock);
}

static void N3DSAudio_resumeAll(AudioSystem* base) {
    N3DSAudioSystem* audio = (N3DSAudioSystem*) base;
    LightLock_Lock(&audio->lock);
    repeat(N3DS_MAX_SOUND_INSTANCES, i) {
        if (audio->instances[i].active) {
            audio->instances[i].paused = false;
            ndspChnSetPaused(audio->instances[i].channelId, false);
            if (audio->instances[i].secondaryChannelId >= 0) ndspChnSetPaused(audio->instances[i].secondaryChannelId, false);
        }
    }
    LightLock_Unlock(&audio->lock);
}

static void N3DSAudio_setSoundGain(AudioSystem* base, int32_t soundOrInstance, float gain, MAYBE_UNUSED uint32_t timeMs) {
    N3DSAudioSystem* audio = (N3DSAudioSystem*) base;
    LightLock_Lock(&audio->lock);
    gain = N3DSAudio_sanitizeGain(gain);
    if (N3DSAudio_isInstanceId(soundOrInstance)) {
        N3DSSoundInstance* inst = N3DSAudio_findInstanceById(audio, soundOrInstance);
        if (inst == NULL) {
            LightLock_Unlock(&audio->lock);
            return;
        }
        inst->gain = gain;
        if (inst->soundIndex >= N3DS_AUDIO_STREAM_INDEX_BASE) {
            N3DSStreamEntry* stream = N3DSAudio_getActiveStreamEntry(audio, inst->soundIndex);
            if (stream != NULL) stream->gain = gain;
        }
        N3DSAudio_applyMix(audio, inst);
        LightLock_Unlock(&audio->lock);
        return;
    }
    N3DSStreamEntry* stream = N3DSAudio_getActiveStreamEntry(audio, soundOrInstance);
    if (stream != NULL) stream->gain = gain;
    repeat(N3DS_MAX_SOUND_INSTANCES, i) {
        N3DSSoundInstance* inst = &audio->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance) {
            inst->gain = gain;
            N3DSAudio_applyMix(audio, inst);
        }
    }
    LightLock_Unlock(&audio->lock);
}

static float N3DSAudio_getSoundGain(AudioSystem* base, int32_t soundOrInstance) {
    N3DSAudioSystem* audio = (N3DSAudioSystem*) base;
    LightLock_Lock(&audio->lock);
    if (N3DSAudio_isInstanceId(soundOrInstance)) {
        N3DSSoundInstance* inst = N3DSAudio_findInstanceById(audio, soundOrInstance);
        float gain = inst != NULL ? inst->gain : 0.0f;
        LightLock_Unlock(&audio->lock);
        return gain;
    }
    repeat(N3DS_MAX_SOUND_INSTANCES, i) {
        if (audio->instances[i].active && audio->instances[i].soundIndex == soundOrInstance) {
            float gain = audio->instances[i].gain;
            LightLock_Unlock(&audio->lock);
            return gain;
        }
    }
    N3DSStreamEntry* stream = N3DSAudio_getActiveStreamEntry(audio, soundOrInstance);
    if (stream != NULL) {
        float gain = stream->gain;
        LightLock_Unlock(&audio->lock);
        return gain;
    }
    LightLock_Unlock(&audio->lock);
    return 0.0f;
}

static void N3DSAudio_setSoundPitch(AudioSystem* base, int32_t soundOrInstance, float pitch) {
    N3DSAudioSystem* audio = (N3DSAudioSystem*) base;
    LightLock_Lock(&audio->lock);
    pitch = N3DSAudio_sanitizePitch(pitch);
    if (N3DSAudio_isInstanceId(soundOrInstance)) {
        N3DSSoundInstance* inst = N3DSAudio_findInstanceById(audio, soundOrInstance);
        if (inst == NULL) {
            LightLock_Unlock(&audio->lock);
            return;
        }
        inst->pitch = pitch;
        if (inst->soundIndex >= N3DS_AUDIO_STREAM_INDEX_BASE) {
            N3DSStreamEntry* stream = N3DSAudio_getActiveStreamEntry(audio, inst->soundIndex);
            if (stream != NULL) stream->pitch = pitch;
        }
        N3DSAudio_applyMix(audio, inst);
        N3DSAudio_refreshNonStreamReleaseTick(inst);
        LightLock_Unlock(&audio->lock);
        return;
    }
    N3DSStreamEntry* stream = N3DSAudio_getActiveStreamEntry(audio, soundOrInstance);
    if (stream != NULL) stream->pitch = pitch;
    repeat(N3DS_MAX_SOUND_INSTANCES, i) {
        N3DSSoundInstance* inst = &audio->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance) {
            inst->pitch = pitch;
            N3DSAudio_applyMix(audio, inst);
            N3DSAudio_refreshNonStreamReleaseTick(inst);
        }
    }
    LightLock_Unlock(&audio->lock);
}

static float N3DSAudio_getSoundPitch(AudioSystem* base, int32_t soundOrInstance) {
    N3DSAudioSystem* audio = (N3DSAudioSystem*) base;
    LightLock_Lock(&audio->lock);
    if (N3DSAudio_isInstanceId(soundOrInstance)) {
        N3DSSoundInstance* inst = N3DSAudio_findInstanceById(audio, soundOrInstance);
        float pitch = inst != NULL ? inst->pitch : 1.0f;
        LightLock_Unlock(&audio->lock);
        return pitch;
    }
    repeat(N3DS_MAX_SOUND_INSTANCES, i) {
        if (audio->instances[i].active && audio->instances[i].soundIndex == soundOrInstance) {
            float pitch = audio->instances[i].pitch;
            LightLock_Unlock(&audio->lock);
            return pitch;
        }
    }
    N3DSStreamEntry* stream = N3DSAudio_getActiveStreamEntry(audio, soundOrInstance);
    if (stream != NULL) {
        float pitch = stream->pitch;
        LightLock_Unlock(&audio->lock);
        return pitch;
    }
    LightLock_Unlock(&audio->lock);
    return 1.0f;
}

static float N3DSAudio_getTrackPosition(AudioSystem* base, int32_t soundOrInstance) {
    N3DSAudioSystem* audio = (N3DSAudioSystem*) base;
    LightLock_Lock(&audio->lock);
    N3DSSoundInstance* inst = NULL;
    if (N3DSAudio_isInstanceId(soundOrInstance)) {
        inst = N3DSAudio_findInstanceById(audio, soundOrInstance);
    } else {
        repeat(N3DS_MAX_SOUND_INSTANCES, i) {
            if (audio->instances[i].active && audio->instances[i].soundIndex == soundOrInstance) {
                inst = &audio->instances[i];
                break;
            }
        }
    }
    if (inst == NULL || inst->sampleRate == 0) {
        LightLock_Unlock(&audio->lock);
        return 0.0f;
    }

    if (inst->useNativeAdpcm) {
        uint16_t currentSeq = ndspChnGetWaveBufSeq(inst->channelId);
        repeat(N3DS_STREAM_BUFFER_COUNT, i) {
            if (inst->waveBufs[i].sequence_id == currentSeq) {
                uint32_t samplePos = ndspChnGetSamplePos(inst->channelId);
                float position = (float) (inst->bufferStartSample[i] + samplePos) / (float) inst->sampleRate;
                LightLock_Unlock(&audio->lock);
                return position;
            }
        }
        float position = (float) ndspChnGetSamplePos(inst->channelId) / (float) inst->sampleRate;
        LightLock_Unlock(&audio->lock);
        return position;
    }

    if (!inst->isStream) {
        float position = (float) ndspChnGetSamplePos(inst->channelId) / (float) inst->sampleRate;
        LightLock_Unlock(&audio->lock);
        return position;
    }

    uint16_t currentSeq = ndspChnGetWaveBufSeq(inst->channelId);
    repeat(N3DS_STREAM_BUFFER_COUNT, i) {
        if (inst->waveBufs[i].sequence_id == currentSeq) {
            uint32_t samplePos = ndspChnGetSamplePos(inst->channelId);
            float position = (float) (inst->bufferStartSample[i] + samplePos) / (float) inst->sampleRate;
            LightLock_Unlock(&audio->lock);
            return position;
        }
    }

    float position = (float) inst->currentSample / (float) inst->sampleRate;
    LightLock_Unlock(&audio->lock);
    return position;
}

static void N3DSAudio_setTrackPosition(AudioSystem* base, int32_t soundOrInstance, float positionSeconds) {
    N3DSAudioSystem* audio = (N3DSAudioSystem*) base;
    LightLock_Lock(&audio->lock);
    N3DSSoundInstance* inst = NULL;
    if (N3DSAudio_isInstanceId(soundOrInstance)) {
        inst = N3DSAudio_findInstanceById(audio, soundOrInstance);
    } else {
        repeat(N3DS_MAX_SOUND_INSTANCES, i) {
            if (audio->instances[i].active && audio->instances[i].soundIndex == soundOrInstance) {
                inst = &audio->instances[i];
                break;
            }
        }
    }
    if (inst == NULL || inst->sampleRate == 0) {
        LightLock_Unlock(&audio->lock);
        return;
    }

    uint32_t targetSample = (uint32_t) (positionSeconds * (float) inst->sampleRate);
    if (inst->useNativeAdpcm && !inst->isStream) {
        if (targetSample != 0) {
            N3DSDebugLog_event("audio_seek", "reject native instance=%ld target=%lu", (long) inst->instanceId, (unsigned long) targetSample);
            LightLock_Unlock(&audio->lock);
            return;
        }
        N3DSDebugLog_event("audio_seek", "native instance=%ld target=%lu", (long) inst->instanceId, (unsigned long) targetSample);
        N3DSAudio_rebuildInstanceChannels(audio, inst);
        if (!N3DSAudio_primeNativeAdpcm(inst)) {
            LightLock_Unlock(&audio->lock);
            return;
        }
        if (inst->paused) {
            ndspChnSetPaused(inst->channelId, true);
            if (inst->secondaryChannelId >= 0) ndspChnSetPaused(inst->secondaryChannelId, true);
        }
        LightLock_Unlock(&audio->lock);
        return;
    }

    if (inst->useNativeAdpcm && inst->isStream) {
        N3DSAudio_cancelAsyncFillLocked(audio, inst);
        if (!N3DSAudio_isFrameAlignedSample(targetSample)) {
            targetSample = (targetSample / 14u) * 14u;
        }
        N3DSDebugLog_event("audio_seek", "native-stream instance=%ld target=%lu", (long) inst->instanceId, (unsigned long) targetSample);
        N3DSAudio_rebuildInstanceChannels(audio, inst);
        if (!N3DSAudio_streamSeekSamples(inst, targetSample)) {
            LightLock_Unlock(&audio->lock);
            return;
        }
        if (!N3DSAudio_primeStream(inst)) {
            LightLock_Unlock(&audio->lock);
            return;
        }
        if (inst->paused) {
            ndspChnSetPaused(inst->channelId, true);
            if (inst->secondaryChannelId >= 0) ndspChnSetPaused(inst->secondaryChannelId, true);
        }
        LightLock_Unlock(&audio->lock);
        return;
    }

    if (!inst->isStream) {
        N3DSDebugLog_event("audio_seek", "pcm instance=%ld target=%lu", (long) inst->instanceId, (unsigned long) targetSample);
        N3DSAudio_rebuildInstanceChannels(audio, inst);
        N3DSAudio_resetInstanceWaveBufState(inst);
        if (targetSample >= inst->sampleCount) targetSample = inst->sampleCount > 0 ? inst->sampleCount - 1 : 0;
        inst->waveBufs[0].data_pcm16 = inst->pcmData + (size_t) targetSample * inst->channelCount;
        inst->waveBufs[0].nsamples = inst->sampleCount - targetSample;
        inst->waveBufs[0].looping = inst->loop;
        ndspChnWaveBufAdd(inst->channelId, &inst->waveBufs[0]);
        LightLock_Unlock(&audio->lock);
        return;
    }

    N3DSAudio_cancelAsyncFillLocked(audio, inst);
    N3DSDebugLog_event("audio_seek", "stream instance=%ld target=%lu", (long) inst->instanceId, (unsigned long) targetSample);
    N3DSAudio_rebuildInstanceChannels(audio, inst);
    if (!N3DSAudio_streamSeekSamples(inst, targetSample)) {
        LightLock_Unlock(&audio->lock);
        return;
    }
    N3DSAudio_primeStream(inst);
    if (inst->paused) ndspChnSetPaused(inst->channelId, true);
    LightLock_Unlock(&audio->lock);
}

static float N3DSAudio_getSoundLength(AudioSystem* base, int32_t soundOrInstance) {
    N3DSAudioSystem* audio = (N3DSAudioSystem*) base;
    LightLock_Lock(&audio->lock);
    N3DSSoundInstance* inst = NULL;
    if (N3DSAudio_isInstanceId(soundOrInstance)) {
        inst = N3DSAudio_findInstanceById(audio, soundOrInstance);
    } else {
        repeat(N3DS_MAX_SOUND_INSTANCES, i) {
            if (audio->instances[i].active && audio->instances[i].soundIndex == soundOrInstance) {
                inst = &audio->instances[i];
                break;
            }
        }
    }
    if (inst != NULL && inst->sampleRate > 0) {
        float length = (float) inst->sampleCount / (float) inst->sampleRate;
        LightLock_Unlock(&audio->lock);
        return length;
    }

    if (soundOrInstance >= N3DS_AUDIO_STREAM_INDEX_BASE) {
        int32_t slot = soundOrInstance - N3DS_AUDIO_STREAM_INDEX_BASE;
        if (slot >= 0 && slot < N3DS_MAX_STREAMS && audio->streams[slot].active && audio->streams[slot].bcwav.sampleRate > 0) {
            float length = (float) audio->streams[slot].bcwav.sampleCount / (float) audio->streams[slot].bcwav.sampleRate;
            LightLock_Unlock(&audio->lock);
            return length;
        }
    }

    LightLock_Unlock(&audio->lock);
    return 0.0f;
}

static void N3DSAudio_setMasterGain(AudioSystem* base, float gain) {
    N3DSAudioSystem* audio = (N3DSAudioSystem*) base;
    LightLock_Lock(&audio->lock);
    audio->masterGain = gain;
    repeat(N3DS_MAX_SOUND_INSTANCES, i) {
        if (audio->instances[i].active) N3DSAudio_applyMix(audio, &audio->instances[i]);
    }
    LightLock_Unlock(&audio->lock);
}

static void N3DSAudio_setChannelCount(MAYBE_UNUSED AudioSystem* base, MAYBE_UNUSED int32_t count) {}
static void N3DSAudio_groupLoad(MAYBE_UNUSED AudioSystem* base, MAYBE_UNUSED int32_t groupIndex) {}
static bool N3DSAudio_groupIsLoaded(MAYBE_UNUSED AudioSystem* base, MAYBE_UNUSED int32_t groupIndex) { return true; }

static int32_t N3DSAudio_createStream(AudioSystem* base, const char* filename) {
    N3DSAudioSystem* audio = (N3DSAudioSystem*) base;
    LightLock_Lock(&audio->lock);
    if (filename == NULL || filename[0] == '\0') {
        LightLock_Unlock(&audio->lock);
        return -1;
    }

    char* path = N3DSAudio_resolveAudioFilePath(audio, filename);
    if (path == NULL) {
        fprintf(stderr, "N3DSAudio: createStream failed to resolve %s\n", filename);
        LightLock_Unlock(&audio->lock);
        return -1;
    }

    repeat(N3DS_MAX_STREAMS, i) {
        if (audio->streams[i].active && audio->streams[i].path != NULL && strcmp(audio->streams[i].path, path) == 0) {
            audio->streams[i].refCount += 1u;
            free(path);
            LightLock_Unlock(&audio->lock);
            return N3DS_AUDIO_STREAM_INDEX_BASE + (int32_t) i;
        }
    }

    repeat(N3DS_MAX_STREAMS, i) {
        if (!audio->streams[i].active) {
            N3DSBcwav bcwav;
            if (!N3DSAudio_parseBcwavFile(path, &bcwav)) {
                free(path);
                LightLock_Unlock(&audio->lock);
                return -1;
            }

            audio->streams[i].active = true;
            audio->streams[i].path = path;
            audio->streams[i].blobData = NULL;
            audio->streams[i].blobSize = 0;
            audio->streams[i].bcwav = bcwav;
            audio->streams[i].gain = 1.0f;
            audio->streams[i].pitch = 1.0f;
            audio->streams[i].refCount = 1u;
            LightLock_Unlock(&audio->lock);
            return N3DS_AUDIO_STREAM_INDEX_BASE + (int32_t) i;
        }
    }
    fprintf(stderr, "N3DSAudio: createStream exhausted slots for %s\n", path);
    free(path);
    LightLock_Unlock(&audio->lock);
    return -1;
}

static bool N3DSAudio_destroyStream(AudioSystem* base, int32_t streamIndex) {
    N3DSAudioSystem* audio = (N3DSAudioSystem*) base;
    LightLock_Lock(&audio->lock);
    int32_t slot = streamIndex - N3DS_AUDIO_STREAM_INDEX_BASE;
    if (slot < 0 || slot >= N3DS_MAX_STREAMS || !audio->streams[slot].active) {
        LightLock_Unlock(&audio->lock);
        return false;
    }

    if (audio->streams[slot].refCount > 1u) {
        audio->streams[slot].refCount -= 1u;
        LightLock_Unlock(&audio->lock);
        return true;
    }

    repeat(N3DS_MAX_SOUND_INSTANCES, i) {
        if (audio->instances[i].active && audio->instances[i].soundIndex == streamIndex) {
            N3DSAudio_releaseInstance(audio, &audio->instances[i]);
        }
    }

    free(audio->streams[slot].blobData);
    free(audio->streams[slot].path);
    memset(&audio->streams[slot], 0, sizeof(audio->streams[slot]));
    LightLock_Unlock(&audio->lock);
    return true;
}

static AudioSystemVtable N3DSAudio_vtable = {
    .init = N3DSAudio_init,
    .destroy = N3DSAudio_destroy,
    .update = N3DSAudio_update,
    .playSound = N3DSAudio_playSound,
    .stopSound = N3DSAudio_stopSound,
    .stopAll = N3DSAudio_stopAll,
    .isPlaying = N3DSAudio_isPlaying,
    .pauseSound = N3DSAudio_pauseSound,
    .resumeSound = N3DSAudio_resumeSound,
    .pauseAll = N3DSAudio_pauseAll,
    .resumeAll = N3DSAudio_resumeAll,
    .setSoundGain = N3DSAudio_setSoundGain,
    .getSoundGain = N3DSAudio_getSoundGain,
    .setSoundPitch = N3DSAudio_setSoundPitch,
    .getSoundPitch = N3DSAudio_getSoundPitch,
    .getTrackPosition = N3DSAudio_getTrackPosition,
    .setTrackPosition = N3DSAudio_setTrackPosition,
    .getSoundLength = N3DSAudio_getSoundLength,
    .setMasterGain = N3DSAudio_setMasterGain,
    .setChannelCount = N3DSAudio_setChannelCount,
    .groupLoad = N3DSAudio_groupLoad,
    .groupIsLoaded = N3DSAudio_groupIsLoaded,
    .createStream = N3DSAudio_createStream,
    .destroyStream = N3DSAudio_destroyStream,
    .prewarmRoom = N3DSAudio_prewarmRoom,
};

N3DSAudioSystem* N3DSAudioSystem_create(void) {
    N3DSAudioSystem* audio = safeCalloc(1, sizeof(N3DSAudioSystem));
    audio->base.vtable = &N3DSAudio_vtable;
    audio->masterGain = 1.0f;
    audio->lastResolveFailureSound = INT32_MIN;
    return audio;
}

void N3DSAudioSystem_getCacheStats(
    AudioSystem* base,
    uint32_t* outCachedSounds,
    uint32_t* outTotalSounds,
    uint32_t* outCachedBytes,
    uint32_t* outCacheLimitBytes
) {
    N3DSAudioSystem* audio = (N3DSAudioSystem*) base;
    if (outCachedSounds != NULL) *outCachedSounds = 0;
    if (outTotalSounds != NULL) *outTotalSounds = 0;
    if (outCachedBytes != NULL) *outCachedBytes = 0;
    if (outCacheLimitBytes != NULL) *outCacheLimitBytes = 0;
    if (audio == NULL) return;

    uint32_t cachedSounds = 0;
    LightLock_Lock(&audio->lock);
    if (audio->cachedSounds != NULL) {
        repeat(audio->cachedSoundCount, i) {
            if (audio->cachedSounds[i].available) cachedSounds++;
        }
    }
    if (outCachedSounds != NULL) *outCachedSounds = cachedSounds;
    if (outTotalSounds != NULL) *outTotalSounds = audio->cachedSoundCount;
    if (outCachedBytes != NULL) *outCachedBytes = audio->cachedSoundBytes;
    if (outCacheLimitBytes != NULL) *outCacheLimitBytes = audio->maxCachedSoundBytes;
    LightLock_Unlock(&audio->lock);
}
