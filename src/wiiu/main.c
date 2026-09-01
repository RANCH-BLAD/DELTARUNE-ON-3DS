#include "../data_win.h"
#include "../vm.h"
#include "../runner.h"
#include "../runner_keyboard.h"

#include "wiiu_file_system.h"
#include "wiiu_renderer.h"
#include "wiiu_audio_system.h"

#include <gx2/clear.h>
#include <gx2/context.h>
#include <gx2/draw.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <gx2/swap.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/mutex.h>
#include <coreinit/systeminfo.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <gx2r/buffer.h>
#include <gx2r/draw.h>
#include <padscore/kpad.h>
#include <padscore/wpad.h>
#include <proc_ui/procui.h>
#include <sysapp/launch.h>
#include <sysapp/switch.h>
#include <vpad/input.h>
#include <whb/gfx.h>
#include <whb/proc.h>
#include <whb/sdcard.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int gBootLogFd = -1;
static OSTime gLoadingAnimationStartTime = 0;
static void bootLog(const char* message);

typedef struct {
    OSMutex mutex;
    float progress;
    int32_t lastChunkIndex;
    char* dataWinPath;
    DataWin* dataWin;
    VMContext* vm;
    WiiUFileSystem* fileSystem;
    bool completed;
    bool failed;
} WiiULoadingState;

typedef struct {
    bool keyHeld[GML_KEY_COUNT];
} WiiUInputState;

typedef struct {
    uint32_t vpadButton;
    int32_t gmlKey;
} WiiUKeyMap;

typedef struct {
    uint32_t wpadButton;
    int32_t gmlKey;
} WiiUWiimoteKeyMap;

static void dataWinProgressCallback(
    const char* chunkName,
    int chunkIndex,
    int totalChunks,
    DataWin* dataWin,
    void* userData
);

static const WiiUKeyMap WIIU_KEY_MAPS[] = {
    { VPAD_BUTTON_UP, VK_UP },
    { VPAD_BUTTON_DOWN, VK_DOWN },
    { VPAD_BUTTON_LEFT, VK_LEFT },
    { VPAD_BUTTON_RIGHT, VK_RIGHT },
    { VPAD_BUTTON_A, 'Z' },
    { VPAD_BUTTON_B, 'X' },
    { VPAD_BUTTON_X, 'C' },
    { VPAD_BUTTON_Y, 'C' },
    { VPAD_BUTTON_PLUS, VK_ENTER },
    { VPAD_BUTTON_MINUS, VK_BACKSPACE },
    { VPAD_BUTTON_L, VK_PAGEDOWN },
    { VPAD_BUTTON_R, VK_PAGEUP },
    { VPAD_BUTTON_ZL, VK_SHIFT },
};

static const WiiUWiimoteKeyMap WIIU_WIIMOTE_HORIZONTAL_KEY_MAPS[] = {
    { WPAD_BUTTON_LEFT, VK_DOWN },
    { WPAD_BUTTON_RIGHT, VK_UP },
    { WPAD_BUTTON_UP, VK_LEFT },
    { WPAD_BUTTON_DOWN, VK_RIGHT },
    { WPAD_BUTTON_PLUS, 'C' },
    { WPAD_BUTTON_MINUS, 'C' },
    { WPAD_BUTTON_2, 'Z' },
    { WPAD_BUTTON_1, 'X' },
};

static const WiiUWiimoteKeyMap WIIU_PRO_CONTROLLER_KEY_MAPS[] = {
    { WPAD_PRO_BUTTON_UP, VK_UP },
    { WPAD_PRO_BUTTON_DOWN, VK_DOWN },
    { WPAD_PRO_BUTTON_LEFT, VK_LEFT },
    { WPAD_PRO_BUTTON_RIGHT, VK_RIGHT },
    { WPAD_PRO_BUTTON_A, 'Z' },
    { WPAD_PRO_BUTTON_B, 'X' },
    { WPAD_PRO_BUTTON_X, 'C' },
    { WPAD_PRO_BUTTON_Y, 'C' },
    { WPAD_PRO_BUTTON_PLUS, VK_ENTER },
    { WPAD_PRO_BUTTON_MINUS, VK_BACKSPACE },
    { WPAD_PRO_BUTTON_L, VK_PAGEDOWN },
    { WPAD_PRO_BUTTON_R, VK_PAGEUP },
    { WPAD_PRO_BUTTON_ZL, VK_SHIFT },
};

static const WiiUWiimoteKeyMap WIIU_CLASSIC_CONTROLLER_KEY_MAPS[] = {
    { WPAD_CLASSIC_BUTTON_UP, VK_UP },
    { WPAD_CLASSIC_BUTTON_DOWN, VK_DOWN },
    { WPAD_CLASSIC_BUTTON_LEFT, VK_LEFT },
    { WPAD_CLASSIC_BUTTON_RIGHT, VK_RIGHT },
    { WPAD_CLASSIC_BUTTON_A, 'Z' },
    { WPAD_CLASSIC_BUTTON_B, 'X' },
    { WPAD_CLASSIC_BUTTON_X, 'C' },
    { WPAD_CLASSIC_BUTTON_Y, 'C' },
    { WPAD_CLASSIC_BUTTON_PLUS, VK_ENTER },
    { WPAD_CLASSIC_BUTTON_MINUS, VK_BACKSPACE },
    { WPAD_CLASSIC_BUTTON_L, VK_PAGEDOWN },
    { WPAD_CLASSIC_BUTTON_R, VK_PAGEUP },
    { WPAD_CLASSIC_BUTTON_ZL, VK_SHIFT },
};

static void bootLog(const char* message) {
    if (gBootLogFd < 0 || message == NULL) return;

    if (strncmp(message, "vm:", 3) == 0) return;
    if (strncmp(message, "perf:", 5) == 0) return;
    if (strncmp(message, "wiiu_audio: playSound begin", 27) == 0) return;
    if (strncmp(message, "wiiu_audio: playSound end", 25) == 0) return;
    if (strncmp(message, "wiiu_audio: decodeSound begin", 29) == 0) return;
    if (strncmp(message, "wiiu_audio: audo entry", 22) == 0) return;
    if (strncmp(message, "wiiu_audio: audo head", 21) == 0) return;
    if (strncmp(message, "wiiu_audio: try path=", 21) == 0) return;
    if (strncmp(message, "wiiu_audio: external decode ok", 30) == 0) return;

    write(gBootLogFd, message, strlen(message));
    write(gBootLogFd, "\n", 1);

    if (
        strncmp(message, "stage:", 6) == 0 ||
        strncmp(message, "frame:", 6) == 0 ||
        strncmp(message, "runner:", 7) == 0 ||
        strncmp(message, "shutdown:", 9) == 0 ||
        strncmp(message, "datawin:", 8) == 0 ||
        strncmp(message, "procui:", 7) == 0 ||
        strncmp(message, "wiiu_", 5) == 0 ||
        strncmp(message, "perf:", 5) == 0
    ) {
        fsync(gBootLogFd);
    }
}

static void openBootLog(void) {
    if (!WHBMountSdCard()) return;

    const char* mountPath = WHBGetSdCardMountPath();
    if (mountPath == NULL) return;

    char logPath[512];
    snprintf(logPath, sizeof(logPath), "%s/wiiu/apps/cinnamon/bootlog.txt", mountPath);
    gBootLogFd = open(logPath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (gBootLogFd < 0) return;

    bootLog("stage: sd mounted");
}

static char* duplicateDirname(const char* path) {
    const char* lastSlash = strrchr(path, '/');
    if (lastSlash == NULL) return strdup(".");
    size_t length = (size_t) (lastSlash - path);
    char* dir = malloc(length + 1);
    memcpy(dir, path, length);
    dir[length] = '\0';
    return dir;
}

static bool fileExistsAtPath(const char* path) {
    if (path == NULL) return false;
    struct stat st;
    return stat(path, &st) == 0;
}

static char* buildDefaultDataWinPath(const char* argv0) {
    const char* mountPath = WHBGetSdCardMountPath();
    if (mountPath != NULL) {
        char sdAppPath[512];
        snprintf(sdAppPath, sizeof(sdAppPath), "%s/wiiu/apps/cinnamon/data.win", mountPath);
        if (fileExistsAtPath(sdAppPath)) return strdup(sdAppPath);
    }

    if (fileExistsAtPath("/vol/content/data.win")) {
        return strdup("/vol/content/data.win");
    }

    char* dir = duplicateDirname(argv0);
    size_t dirLen = strlen(dir);
    const char suffix[] = "/data.win";
    char* result = malloc(dirLen + sizeof(suffix));
    memcpy(result, dir, dirLen);
    memcpy(result + dirLen, suffix, sizeof(suffix));
    free(dir);
    return result;
}

static int32_t clampRenderDimension(int32_t value, int32_t fallback, int32_t maxValue) {
    if (value <= 0) value = fallback;
    if (value > maxValue) value = maxValue;
    return value;
}

static double elapsedMs(OSTime start, OSTime end) {
    return (double) OSTicksToMicroseconds(end - start) / 1000.0;
}

static void clearAllInputState(WiiUInputState* inputState, RunnerKeyboardState* keyboard) {
    if (inputState != NULL) {
        memset(inputState->keyHeld, 0, sizeof(inputState->keyHeld));
    }
    if (keyboard == NULL) return;

    repeat(GML_KEY_COUNT, i) {
        if (keyboard->keyDown[i]) {
            RunnerKeyboard_onKeyUp(keyboard, i);
        }
    }
    RunnerKeyboard_beginFrame(keyboard);
}

static bool waitForForegroundRestore(void) {
    bool loggedWait = false;
    while (WHBProcIsRunning()) {
        if (!loggedWait) {
            bootLog("procui: waiting for foreground");
            loggedWait = true;
        }

        ProcUIStatus status = ProcUIProcessMessages(TRUE);
        switch (status) {
            case PROCUI_STATUS_IN_FOREGROUND:
                bootLog("procui: foreground restored");
                return true;
            case PROCUI_STATUS_RELEASE_FOREGROUND:
                bootLog("procui: release foreground");
                ProcUIDrawDoneRelease();
                break;
            case PROCUI_STATUS_IN_BACKGROUND:
                OSSleepTicks(OSMicrosecondsToTicks(50000));
                break;
            case PROCUI_STATUS_EXITING:
                bootLog("procui: exiting while backgrounded");
                WHBProcStopRunning();
                return false;
        }
    }

    if (loggedWait) bootLog("procui: stopped while backgrounded");
    return WHBProcIsRunning();
}

static void pumpProcUIState(void) {
    if (!ProcUIIsRunning()) return;
    ProcUIProcessMessages(FALSE);
}

static void setLoadingProgress(WiiULoadingState* state, float progress) {
    if (state == NULL) return;
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    OSLockMutex(&state->mutex);
    state->progress = progress;
    OSUnlockMutex(&state->mutex);
}

static float getLoadingProgress(WiiULoadingState* state) {
    float progress = 0.0f;
    if (state == NULL) return progress;

    OSLockMutex(&state->mutex);
    progress = state->progress;
    OSUnlockMutex(&state->mutex);
    return progress;
}

static int loadingWorkerThreadMain(int argc, const char** argv) {
    (void) argc;
    WiiULoadingState* state = (WiiULoadingState*) argv;
    if (state == NULL) return 1;

    bootLog("stage: worker before DataWin_parse");
    DataWin* dataWin = DataWin_parse(
        state->dataWinPath,
        (DataWinParserOptions) {
            .parseGen8 = true,
            .parseOptn = true,
            .parseLang = true,
            .parseExtn = true,
            .parseSond = true,
            .parseAgrp = true,
            .parseSprt = true,
            .parseBgnd = true,
            .parsePath = true,
            .parseScpt = true,
            .parseGlob = true,
            .parseShdr = true,
            .parseFont = true,
            .parseTmln = true,
            .parseObjt = true,
            .parseRoom = true,
            .parseTpag = true,
            .parseCode = true,
            .parseVari = true,
            .parseFunc = true,
            .parseStrg = true,
            .parseTxtr = true,
            .parseAudo = true,
            .skipLoadingPreciseMasksForNonPreciseSprites = true,
            .progressCallback = dataWinProgressCallback,
            .progressCallbackUserData = state,
        }
    );
    bootLog("stage: worker after DataWin_parse");
    if (dataWin == NULL) {
        OSLockMutex(&state->mutex);
        state->failed = true;
        state->completed = true;
        OSUnlockMutex(&state->mutex);
        return 1;
    }
    setLoadingProgress(state, 0.78f);

    bootLog("stage: worker before VM_create");
    VMContext* vm = VM_create(dataWin);
    bootLog("stage: worker after VM_create");
    setLoadingProgress(state, 0.82f);

    WiiUFileSystem* fileSystem = WiiUFileSystem_create(state->dataWinPath);
    bootLog("stage: worker after WiiUFileSystem_create");
    setLoadingProgress(state, 0.86f);

    OSLockMutex(&state->mutex);
    state->dataWin = dataWin;
    state->vm = vm;
    state->fileSystem = fileSystem;
    state->completed = true;
    OSUnlockMutex(&state->mutex);
    return 0;
}

static int32_t resolveMappedKey(const RunnerKeyboardState* keyboard, int32_t gmlKey) {
    (void) keyboard;
    return gmlKey;
}

static void setDesiredKeyState(bool* desiredKeys, const RunnerKeyboardState* keyboard, int32_t gmlKey, bool isHeld) {
    int32_t mappedKey = resolveMappedKey(keyboard, gmlKey);
    if (mappedKey >= 0 && mappedKey < GML_KEY_COUNT && isHeld) {
        desiredKeys[mappedKey] = true;
    }
}

static void syncKeyState(WiiUInputState* inputState, RunnerKeyboardState* keyboard, int32_t gmlKey, bool isHeld) {
    int32_t mappedKey = resolveMappedKey(keyboard, gmlKey);
    bool wasHeld = (mappedKey >= 0 && mappedKey < GML_KEY_COUNT)
        ? inputState->keyHeld[mappedKey]
        : false;
    if (isHeld && !wasHeld) {
        RunnerKeyboard_onKeyDown(keyboard, gmlKey);
    } else if (!isHeld && wasHeld) {
        RunnerKeyboard_onKeyUp(keyboard, gmlKey);
    }
    if (mappedKey >= 0 && mappedKey < GML_KEY_COUNT) {
        inputState->keyHeld[mappedKey] = isHeld;
    }
}

static void accumulateButtonsToDesiredKeys(bool* desiredKeys, RunnerKeyboardState* keyboard, uint32_t held) {
    repeat(sizeof(WIIU_KEY_MAPS) / sizeof(WIIU_KEY_MAPS[0]), i) {
        int32_t gmlKey = WIIU_KEY_MAPS[i].gmlKey;
        if (gmlKey == VK_LEFT || gmlKey == VK_RIGHT || gmlKey == VK_UP || gmlKey == VK_DOWN) {
            continue;
        }
        setDesiredKeyState(desiredKeys, keyboard, gmlKey, (held & WIIU_KEY_MAPS[i].vpadButton) != 0);
    }
}

static void accumulateWiimoteButtonsToDesiredKeys(bool* desiredKeys, RunnerKeyboardState* keyboard, uint32_t held) {
    repeat(sizeof(WIIU_WIIMOTE_HORIZONTAL_KEY_MAPS) / sizeof(WIIU_WIIMOTE_HORIZONTAL_KEY_MAPS[0]), i) {
        setDesiredKeyState(
            desiredKeys,
            keyboard,
            WIIU_WIIMOTE_HORIZONTAL_KEY_MAPS[i].gmlKey,
            (held & WIIU_WIIMOTE_HORIZONTAL_KEY_MAPS[i].wpadButton) != 0
        );
    }
}

static void accumulateProButtonsToDesiredKeys(bool* desiredKeys, RunnerKeyboardState* keyboard, uint32_t held) {
    repeat(sizeof(WIIU_PRO_CONTROLLER_KEY_MAPS) / sizeof(WIIU_PRO_CONTROLLER_KEY_MAPS[0]), i) {
        setDesiredKeyState(
            desiredKeys,
            keyboard,
            WIIU_PRO_CONTROLLER_KEY_MAPS[i].gmlKey,
            (held & WIIU_PRO_CONTROLLER_KEY_MAPS[i].wpadButton) != 0
        );
    }
}

static void accumulateClassicButtonsToDesiredKeys(bool* desiredKeys, RunnerKeyboardState* keyboard, uint32_t held) {
    repeat(sizeof(WIIU_CLASSIC_CONTROLLER_KEY_MAPS) / sizeof(WIIU_CLASSIC_CONTROLLER_KEY_MAPS[0]), i) {
        setDesiredKeyState(
            desiredKeys,
            keyboard,
            WIIU_CLASSIC_CONTROLLER_KEY_MAPS[i].gmlKey,
            (held & WIIU_CLASSIC_CONTROLLER_KEY_MAPS[i].wpadButton) != 0
        );
    }
}

static bool axisPressedNegative(float value, bool wasHeld) {
    const float pressDeadzone = 0.40f;
    const float releaseDeadzone = 0.24f;
    return value <= -(wasHeld ? releaseDeadzone : pressDeadzone);
}

static bool axisPressedPositive(float value, bool wasHeld) {
    const float pressDeadzone = 0.40f;
    const float releaseDeadzone = 0.24f;
    return value >= (wasHeld ? releaseDeadzone : pressDeadzone);
}

static void accumulateDirectionalInputToDesiredKeys(bool* desiredKeys, WiiUInputState* inputState, RunnerKeyboardState* keyboard, uint32_t held, const VPADStatus* status) {
    bool leftWasHeld = inputState->keyHeld[resolveMappedKey(keyboard, VK_LEFT)];
    bool rightWasHeld = inputState->keyHeld[resolveMappedKey(keyboard, VK_RIGHT)];
    bool upWasHeld = inputState->keyHeld[resolveMappedKey(keyboard, VK_UP)];
    bool downWasHeld = inputState->keyHeld[resolveMappedKey(keyboard, VK_DOWN)];

    bool leftHeld = (held & VPAD_BUTTON_LEFT) != 0 ||
        axisPressedNegative(status->leftStick.x, leftWasHeld);
    bool rightHeld = (held & VPAD_BUTTON_RIGHT) != 0 ||
        axisPressedPositive(status->leftStick.x, rightWasHeld);
    bool upHeld = (held & VPAD_BUTTON_UP) != 0 ||
        axisPressedPositive(status->leftStick.y, upWasHeld);
    bool downHeld = (held & VPAD_BUTTON_DOWN) != 0 ||
        axisPressedNegative(status->leftStick.y, downWasHeld);

    setDesiredKeyState(desiredKeys, keyboard, VK_LEFT, leftHeld);
    setDesiredKeyState(desiredKeys, keyboard, VK_RIGHT, rightHeld);
    setDesiredKeyState(desiredKeys, keyboard, VK_UP, upHeld);
    setDesiredKeyState(desiredKeys, keyboard, VK_DOWN, downHeld);
}

static void accumulateProDirectionalInputToDesiredKeys(
    bool* desiredKeys,
    WiiUInputState* inputState,
    RunnerKeyboardState* keyboard,
    uint32_t held,
    const KPADStatus* status
) {
    bool leftWasHeld = inputState->keyHeld[resolveMappedKey(keyboard, VK_LEFT)];
    bool rightWasHeld = inputState->keyHeld[resolveMappedKey(keyboard, VK_RIGHT)];
    bool upWasHeld = inputState->keyHeld[resolveMappedKey(keyboard, VK_UP)];
    bool downWasHeld = inputState->keyHeld[resolveMappedKey(keyboard, VK_DOWN)];

    bool leftHeld = (held & WPAD_PRO_BUTTON_LEFT) != 0 ||
        axisPressedNegative(status->pro.leftStick.x, leftWasHeld);
    bool rightHeld = (held & WPAD_PRO_BUTTON_RIGHT) != 0 ||
        axisPressedPositive(status->pro.leftStick.x, rightWasHeld);
    bool upHeld = (held & WPAD_PRO_BUTTON_UP) != 0 ||
        axisPressedPositive(status->pro.leftStick.y, upWasHeld);
    bool downHeld = (held & WPAD_PRO_BUTTON_DOWN) != 0 ||
        axisPressedNegative(status->pro.leftStick.y, downWasHeld);

    setDesiredKeyState(desiredKeys, keyboard, VK_LEFT, leftHeld);
    setDesiredKeyState(desiredKeys, keyboard, VK_RIGHT, rightHeld);
    setDesiredKeyState(desiredKeys, keyboard, VK_UP, upHeld);
    setDesiredKeyState(desiredKeys, keyboard, VK_DOWN, downHeld);
}

static void accumulateClassicDirectionalInputToDesiredKeys(
    bool* desiredKeys,
    WiiUInputState* inputState,
    RunnerKeyboardState* keyboard,
    uint32_t held,
    const KPADStatus* status
) {
    bool leftWasHeld = inputState->keyHeld[resolveMappedKey(keyboard, VK_LEFT)];
    bool rightWasHeld = inputState->keyHeld[resolveMappedKey(keyboard, VK_RIGHT)];
    bool upWasHeld = inputState->keyHeld[resolveMappedKey(keyboard, VK_UP)];
    bool downWasHeld = inputState->keyHeld[resolveMappedKey(keyboard, VK_DOWN)];

    bool leftHeld = (held & WPAD_CLASSIC_BUTTON_LEFT) != 0 ||
        axisPressedNegative(status->classic.leftStick.x, leftWasHeld);
    bool rightHeld = (held & WPAD_CLASSIC_BUTTON_RIGHT) != 0 ||
        axisPressedPositive(status->classic.leftStick.x, rightWasHeld);
    bool upHeld = (held & WPAD_CLASSIC_BUTTON_UP) != 0 ||
        axisPressedPositive(status->classic.leftStick.y, upWasHeld);
    bool downHeld = (held & WPAD_CLASSIC_BUTTON_DOWN) != 0 ||
        axisPressedNegative(status->classic.leftStick.y, downWasHeld);

    setDesiredKeyState(desiredKeys, keyboard, VK_LEFT, leftHeld);
    setDesiredKeyState(desiredKeys, keyboard, VK_RIGHT, rightHeld);
    setDesiredKeyState(desiredKeys, keyboard, VK_UP, upHeld);
    setDesiredKeyState(desiredKeys, keyboard, VK_DOWN, downHeld);
}

static void syncDesiredKeysToKeyboard(WiiUInputState* inputState, RunnerKeyboardState* keyboard, const bool* desiredKeys) {
    repeat(GML_KEY_COUNT, key) {
        bool isHeld = desiredKeys[key];
        bool wasHeld = inputState->keyHeld[key];
        if (isHeld && !wasHeld) {
            RunnerKeyboard_onKeyDown(keyboard, key);
        } else if (!isHeld && wasHeld) {
            RunnerKeyboard_onKeyUp(keyboard, key);
        }
        inputState->keyHeld[key] = isHeld;
    }
}

void Runner_platformBootLog(const char* message) { bootLog(message); }
void VM_platformBootLog(const char* message) { bootLog(message); }
void DataWin_platformBootLog(const char* message) { bootLog(message); }
void WiiUFileSystem_platformBootLog(const char* message) { bootLog(message); }
void WiiUAudio_platformBootLog(const char* message) { bootLog(message); }
void WiiURenderer_platformBootLog(const char* message) { bootLog(message); }

#include <stb/image/stb_image.h>

#define DOG_FRAME_W     21
#define DOG_FRAME_H     83
#define DOG_FRAME_COUNT  9
#define DOG_FRAME_STRIDE 21
#define DOG_FRAME_COPY_W 21
#define DOG_SCALE        12
#define DOG_REF_WIDTH    1920
#define DOG_REF_HEIGHT   1080
#define DOG_Y_OFFSET     -200

typedef struct {
    uint8_t* pixels; // RGBA8, row-major, DOG_FRAME_COPY_W * DOG_FRAME_H * 4 bytes per frame
    int      frameCount;
    bool     loaded;
} DogSprite;

static DogSprite gDogSprite = { NULL, 0, false };

static void dogSprite_load(void) {
    if (gDogSprite.loaded) return;
    const char* paths[] = {
        "/vol/content/loadingDog.png",
        "/vol/content/resources/wiiu/loadingDog.png",
    };

    FILE* f = NULL;
    repeat(sizeof(paths) / sizeof(paths[0]), i) {
        f = fopen(paths[i], "rb");
        if (f != NULL) break;
    }
    if (f == NULL) {
        return;
    }
    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    rewind(f);
    uint8_t* fileData = (uint8_t*) malloc((size_t) fileSize);
    if (fileData == NULL) { fclose(f); return; }
    fread(fileData, 1, (size_t) fileSize, f);
    fclose(f);

    int w, h, channels;
    uint8_t* sheet = stbi_load_from_memory(fileData, (int) fileSize, &w, &h, &channels, 4);
    free(fileData);
    if (sheet == NULL) {
        bootLog("wiiu_loading: failed to decode loadingDog.png");
        return;
    }

    int sheetH = (h < DOG_FRAME_H) ? h : DOG_FRAME_H;
    int frameCount = w / DOG_FRAME_STRIDE;
    if (frameCount > DOG_FRAME_COUNT) frameCount = DOG_FRAME_COUNT;
    if (frameCount == 0) { stbi_image_free(sheet); return; }

    size_t frameBytes = (size_t)(DOG_FRAME_COPY_W * sheetH * 4);
    gDogSprite.pixels = (uint8_t*) malloc(frameBytes * (size_t) frameCount);
    if (gDogSprite.pixels == NULL) { stbi_image_free(sheet); return; }

    for (int fi = 0; fi < frameCount; fi++) {
        uint8_t* dst = gDogSprite.pixels + fi * frameBytes;
        for (int row = 0; row < sheetH; row++) {
            const uint8_t* src = sheet + (row * w + fi * DOG_FRAME_STRIDE) * 4;
            memcpy(dst + row * DOG_FRAME_COPY_W * 4, src, (size_t)(DOG_FRAME_COPY_W * 4));
        }
    }

    stbi_image_free(sheet);
    gDogSprite.frameCount = frameCount;
    gDogSprite.loaded = true;
    bootLog("wiiu_loading: loadingDog.png loaded");
}

static void dogSprite_free(void) {
    if (gDogSprite.pixels) { free(gDogSprite.pixels); gDogSprite.pixels = NULL; }
    gDogSprite.loaded = false;
}

static int resolveDogScale(uint32_t bufW, uint32_t bufH) {
    float modifier = 1.0f;

    if (bufW < DOG_REF_WIDTH || bufH < DOG_REF_HEIGHT) {
        float widthModifier = (float) bufW / (float) DOG_REF_WIDTH;
        float heightModifier = (float) bufH / (float) DOG_REF_HEIGHT;
        modifier = fminf(widthModifier, heightModifier);
    }

    int scale = (int) lroundf((float) DOG_SCALE * modifier);
    if (scale < 1) scale = 1;
    if (scale > DOG_SCALE) scale = DOG_SCALE;
    return scale;
}

static void blitDogFrame(uint32_t* pixels, uint32_t pitch,
                         uint32_t bufW, uint32_t bufH,
                         float progress) {
    if (!gDogSprite.loaded || gDogSprite.frameCount == 0) return;

    OSTime now = OSGetTime();
    OSTime startTime = gLoadingAnimationStartTime != 0 ? gLoadingAnimationStartTime : now;
    uint64_t elapsedUs = OSTicksToMicroseconds(now - startTime);
    uint64_t frameTicks = elapsedUs / 200000ull;
    int frameIndex = (int) (frameTicks % (uint64_t) gDogSprite.frameCount);

    int spriteW = DOG_FRAME_COPY_W;
    int spriteH = DOG_FRAME_H;

    int scale = resolveDogScale(bufW, bufH);
    int scaledW = spriteW * scale;
    int scaledH = spriteH * scale;

    int dogX = (int)((int32_t)bufW / 2 - scaledW / 2);

    int targetY = (int)((int32_t)bufH / 2 - scaledH / 2) + DOG_Y_OFFSET;
    int maxY = (int) bufH - scaledH;
    if (maxY < 0) maxY = 0;
    if (targetY < 0) targetY = 0;
    if (targetY > maxY) targetY = maxY;

    int startY = -scaledH;
    int dogY = (int) lroundf((float) startY + ((float) (targetY - startY) * progress * 0.9));
    if (dogY < startY) dogY = startY;
    if (dogY > targetY) dogY = targetY;

    const uint8_t* framePixels = gDogSprite.pixels +
        (size_t)(frameIndex * spriteW * spriteH * 4);

    for (int row = 0; row < spriteH; row++) {
        for (int col = 0; col < spriteW; col++) {
            const uint8_t* sp = framePixels + (row * spriteW + col) * 4;
            uint8_t sr = sp[0], sg = sp[1], sb = sp[2], sa = sp[3];
            if (sa == 0 || (sr == 0 && sg == 0 && sb == 0)) continue;
            uint32_t packed = ((uint32_t) sr << 24) | ((uint32_t) sg << 16) |
                              ((uint32_t) sb <<  8) | 0xFFu;
            for (int sy = 0; sy < scale; sy++) {
                int dstY = dogY + row * scale + sy;
                if (dstY < 0 || (uint32_t) dstY >= bufH) continue;
                for (int sx = 0; sx < scale; sx++) {
                    int dstX = dogX + col * scale + sx;
                    if (dstX < 0 || (uint32_t) dstX >= bufW) continue;
                    pixels[(uint32_t) dstY * pitch + (uint32_t) dstX] = packed;
                }
            }
        }
    }
}

static void fillRectLinear(uint32_t* pixels, uint32_t pitch,
                           uint32_t x, uint32_t y,
                           uint32_t w, uint32_t h,
                           uint8_t r, uint8_t g, uint8_t b) {
    uint32_t color = ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | 0xFFu;
    for (uint32_t row = y; row < y + h; row++) {
        for (uint32_t col = x; col < x + w; col++) {
            pixels[row * pitch + col] = color;
        }
    }
}

static void drawLoadingBarToBuffer(GX2ColorBuffer* buffer, float progress) {
    if (buffer == NULL) return;

    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    uint32_t bw = buffer->surface.width;
    uint32_t bh = buffer->surface.height;

    GX2Surface linear;
    memset(&linear, 0, sizeof(linear));
    linear.dim       = GX2_SURFACE_DIM_TEXTURE_2D;
    linear.width     = bw;
    linear.height    = bh;
    linear.depth     = 1;
    linear.mipLevels = 1;
    linear.format    = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    linear.tileMode  = GX2_TILE_MODE_LINEAR_ALIGNED;
    linear.use       = GX2_SURFACE_USE_COLOR_BUFFER;
    GX2CalcSurfaceSizeAndAlignment(&linear);

    void* mem = MEMAllocFromDefaultHeapEx(linear.imageSize, linear.alignment);
    if (mem == NULL) return;
    linear.image = mem;

    uint32_t pitch = linear.pitch; // in pixels, aligned
    uint32_t* pixels = (uint32_t*) mem;

    // Background
    fillRectLinear(pixels, pitch, 0, 0, bw, bh, 0, 0, 0);

    // Bar geometry
    uint32_t barW = bw * 2u / 3u;
    uint32_t barH = bh / 18u;
    if (barW < 64u) barW = bw > 64u ? bw - 16u : bw;
    if (barH < 8u)  barH = 8u;
    uint32_t barX = (bw > barW) ? (bw - barW) / 2u : 0u;
    uint32_t barY = (bh * 4u) / 5u;
    if (barY + barH >= bh) barY = bh > barH + 8u ? bh - barH - 8u : 0u;

    uint32_t border = 3u;
    uint32_t trackX = barX + border;
    uint32_t trackY = barY + border;
    uint32_t trackW = barW > border * 2u ? barW - border * 2u : barW;
    uint32_t trackH = barH > border * 2u ? barH - border * 2u : barH;
    uint32_t fillW  = (uint32_t)((float)trackW * progress);
    if (progress > 0.0f && fillW == 0u) fillW = 1u;
    if (fillW > trackW) fillW = trackW;

    fillRectLinear(pixels, pitch, barX,   barY,   barW,  barH,  204, 179,  61); // gold border
    fillRectLinear(pixels, pitch, trackX, trackY, trackW, trackH, 26,  26,  36); // dark trough
    if (fillW > 0u)
        fillRectLinear(pixels, pitch, trackX, trackY, fillW, trackH, 250, 194,  56); // bright fill

    // draw the annoying dog rolling down from top to center as progress increases.
    blitDogFrame(pixels, pitch, bw, bh, progress);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU, mem, linear.imageSize);
    GX2CopySurface(&linear, 0, 0, &buffer->surface, 0, 0);
    GX2DrawDone();

    MEMFreeToDefaultHeap(mem);
}

static void presentStartupFrame(uint8_t r, uint8_t g, uint8_t b) {
    GX2ColorBuffer* tv = WHBGfxGetTVColourBuffer();
    GX2ColorBuffer* drc = WHBGfxGetDRCColourBuffer();
    GX2ContextState* tvContext = WHBGfxGetTVContextState();
    GX2ContextState* drcContext = WHBGfxGetDRCContextState();
    if (tv == NULL || drc == NULL || tvContext == NULL || drcContext == NULL) {
        return;
    }

    float rf = (float) r / 255.0f;
    float gf = (float) g / 255.0f;
    float bf = (float) b / 255.0f;

    GX2SetContextState(tvContext);
    GX2ClearColor(tv, rf, gf, bf, 1.0f);
    GX2CopyColorBufferToScanBuffer(tv, GX2_SCAN_TARGET_TV);

    GX2SetContextState(drcContext);
    GX2ClearColor(drc, rf, gf, bf, 1.0f);
    GX2CopyColorBufferToScanBuffer(drc, GX2_SCAN_TARGET_DRC);

    GX2Flush();
    GX2SwapScanBuffers();
    GX2DrawDone();
}

static void presentLoadingProgress(float progress) {
    GX2ColorBuffer* tv = WHBGfxGetTVColourBuffer();
    GX2ColorBuffer* drc = WHBGfxGetDRCColourBuffer();
    GX2ContextState* tvContext = WHBGfxGetTVContextState();
    GX2ContextState* drcContext = WHBGfxGetDRCContextState();
    if (tv == NULL || drc == NULL || tvContext == NULL || drcContext == NULL) {
        return;
    }

    drawLoadingBarToBuffer(tv, progress);
    GX2SetContextState(tvContext);
    GX2CopyColorBufferToScanBuffer(tv, GX2_SCAN_TARGET_TV);

    drawLoadingBarToBuffer(drc, progress);
    GX2SetContextState(drcContext);
    GX2CopyColorBufferToScanBuffer(drc, GX2_SCAN_TARGET_DRC);

    GX2Flush();
    GX2SwapScanBuffers();
    GX2DrawDone();
}

static void dataWinProgressCallback(
    const char* chunkName,
    int chunkIndex,
    int totalChunks,
    DataWin* dataWin,
    void* userData
) {
    (void) chunkName;
    (void) dataWin;
    WiiULoadingState* state = (WiiULoadingState*) userData;
    if (state == NULL) return;
    OSLockMutex(&state->mutex);
    if (chunkIndex == state->lastChunkIndex) {
        OSUnlockMutex(&state->mutex);
        return;
    }
    state->lastChunkIndex = chunkIndex;
    OSUnlockMutex(&state->mutex);

    float parseProgress = totalChunks > 0 ? (float) (chunkIndex + 1) / (float) totalChunks : 0.0f;
    if (parseProgress < 0.0f) parseProgress = 0.0f;
    if (parseProgress > 1.0f) parseProgress = 1.0f;

    setLoadingProgress(state, 0.10f + parseProgress * 0.65f);
}

int main(int argc, char* argv[]) {
    bool loadingThreadJoined = false;
    bool loadingThreadStarted = false;
    bool loadingThreadDetached = false;
    DataWin* dataWin = NULL;
    WiiUFileSystem* fileSystem = NULL;
    Runner* runner = NULL;
    Renderer* renderer = NULL;
    WiiUAudioSystem* audio = NULL;
    char* dataWinPath = NULL;
    void* loadingThreadStack = NULL;
    WiiULoadingState loadingState;
    memset(&loadingState, 0, sizeof(loadingState));
    OSThread loadingThread;

    WHBProcInit();
    openBootLog();
    bootLog("stage: after WHBProcInit");

    if (!WHBGfxInit()) {
        bootLog("stage: WHBGfxInit failed");
        WHBProcShutdown();
        return 1;
    }
    bootLog("stage: after WHBGfxInit");
    presentStartupFrame(0, 0, 0);
    presentLoadingProgress(0.02f);

    dogSprite_load();

    VPADInit();
    WPADInit();
    WPADEnableURCC(true);
    KPADInit();
    bootLog("stage: input init complete");
    presentLoadingProgress(0.05f);

    dataWinPath = argc > 1 ? strdup(argv[1]) : buildDefaultDataWinPath(argv[0]);
    if (gBootLogFd >= 0) {
        char pathBuffer[768];
        snprintf(pathBuffer, sizeof(pathBuffer), "data.win path: %s", dataWinPath);
        bootLog(pathBuffer);
    }

    OSInitMutexEx(&loadingState.mutex, "cinnamon-loading");
    loadingState.progress = 0.05f;
    loadingState.lastChunkIndex = -1;
    loadingState.dataWinPath = dataWinPath;

    enum { WIIU_LOADING_THREAD_STACK_SIZE = 256 * 1024 };
    int loadingThreadResult = 0;
    loadingThreadStack = MEMAllocFromDefaultHeapEx(WIIU_LOADING_THREAD_STACK_SIZE, 16);

    gLoadingAnimationStartTime = OSGetTime();
    bootLog("stage: before loading worker create");
    if (loadingThreadStack != NULL &&
        OSCreateThread(
            &loadingThread,
            loadingWorkerThreadMain,
            0,
            (char*) &loadingState,
            (uint8_t*) loadingThreadStack + WIIU_LOADING_THREAD_STACK_SIZE,
            WIIU_LOADING_THREAD_STACK_SIZE,
            16,
            OS_THREAD_ATTRIB_AFFINITY_ANY
        )) {
        OSSetThreadName(&loadingThread, "cinnamon-loader");
        OSResumeThread(&loadingThread);
        loadingThreadStarted = true;
        bootLog("stage: loading worker started");
    } else {
        bootLog("stage: loading worker create failed");
        if (loadingThreadStack != NULL) {
            MEMFreeToDefaultHeap(loadingThreadStack);
            loadingThreadStack = NULL;
        }
        loadingThreadResult = loadingWorkerThreadMain(0, (const char**) &loadingState);
    }

    while (WHBProcIsRunning()) {
        float progress = getLoadingProgress(&loadingState);
        bool completed = false;
        bool failed = false;

        OSLockMutex(&loadingState.mutex);
        completed = loadingState.completed;
        failed = loadingState.failed;
        OSUnlockMutex(&loadingState.mutex);

        presentLoadingProgress(progress);
        if (completed || failed) break;
        OSSleepTicks(OSMicrosecondsToTicks(83333));
    }

    if (!WHBProcIsRunning()) {
        goto shutdown;
    }

    if (loadingThreadStarted) {
        OSJoinThread(&loadingThread, &loadingThreadResult);
        loadingThreadJoined = true;
    }
    if (loadingThreadStack != NULL) {
        MEMFreeToDefaultHeap(loadingThreadStack);
        loadingThreadStack = NULL;
    }

    if (loadingState.failed || loadingThreadResult != 0 ||
        loadingState.dataWin == NULL || loadingState.vm == NULL ||
        loadingState.fileSystem == NULL) {
        bootLog("stage: loading worker failed");
        goto shutdown;
    }

    dataWin = loadingState.dataWin;
    fileSystem = loadingState.fileSystem;
    loadingState.dataWin = NULL;
    loadingState.fileSystem = NULL;
    VMContext* vm = loadingState.vm;
    loadingState.vm = NULL;

    renderer = WiiURenderer_create();
    bootLog("stage: after WiiURenderer_create");
    renderer->vtable->init(renderer, dataWin);
    bootLog("stage: after renderer init");

    audio = WiiUAudioSystem_create();
    audio->base.vtable->init((AudioSystem*) audio, dataWin, (FileSystem*) fileSystem);
    bootLog("stage: after audio init");

    runner = Runner_create(dataWin, vm, renderer, (FileSystem*) fileSystem, (AudioSystem*) audio);
    bootLog("stage: after Runner_create");

    bootLog("stage: before Runner_initFirstRoom");
    Runner_initFirstRoom(runner);
    bootLog("stage: after Runner_initFirstRoom");

    OSTime lastFrameTime = OSGetTime();
    uint32_t perfFrameCount = 0;
    double perfVmMs = 0.0;
    double perfRenderMs = 0.0;
    bool firstFrameTracePending = true;
    WiiUInputState inputState;
    memset(&inputState, 0, sizeof(inputState));
    bootLog("stage: before main loop");

    while (WHBProcIsRunning()) {
        if (runner == NULL) {
            bootLog("stage: runner null before main loop");
            break;
        }
        if (runner->shouldExit) {
            bootLog("stage: runner requested exit before frame");
            break;
        }

        OSTime frameStartTime = OSGetTime();
        bool desiredKeys[GML_KEY_COUNT];
        memset(desiredKeys, 0, sizeof(desiredKeys));

        VPADStatus vpadStatus;
        VPADReadError error;
        memset(&vpadStatus, 0, sizeof(vpadStatus));
        if (VPADRead(VPAD_CHAN_0, &vpadStatus, 1, &error) > 0 && error == VPAD_READ_SUCCESS) {
            accumulateButtonsToDesiredKeys(desiredKeys, runner->keyboard, vpadStatus.hold);
            accumulateDirectionalInputToDesiredKeys(desiredKeys, &inputState, runner->keyboard, vpadStatus.hold, &vpadStatus);
        }

        repeat(7, channel) {
            KPADStatus kpadStatus;
            KPADError kpadError = KPAD_ERROR_OK;
            memset(&kpadStatus, 0, sizeof(kpadStatus));

            if (KPADReadEx((KPADChan) channel, &kpadStatus, 1, &kpadError) <= 0 || kpadError != KPAD_ERROR_OK) {
                continue;
            }

            switch (kpadStatus.extensionType) {
                case WPAD_EXT_CORE:
                case WPAD_EXT_MPLUS:
                    accumulateWiimoteButtonsToDesiredKeys(desiredKeys, runner->keyboard, kpadStatus.hold);
                    break;
                case WPAD_EXT_PRO_CONTROLLER:
                    accumulateProButtonsToDesiredKeys(desiredKeys, runner->keyboard, kpadStatus.pro.hold);
                    accumulateProDirectionalInputToDesiredKeys(
                        desiredKeys,
                        &inputState,
                        runner->keyboard,
                        kpadStatus.pro.hold,
                        &kpadStatus
                    );
                    break;
                case WPAD_EXT_CLASSIC:
                case WPAD_EXT_MPLUS_CLASSIC:
                    accumulateClassicButtonsToDesiredKeys(desiredKeys, runner->keyboard, kpadStatus.classic.hold);
                    accumulateClassicDirectionalInputToDesiredKeys(
                        desiredKeys,
                        &inputState,
                        runner->keyboard,
                        kpadStatus.classic.hold,
                        &kpadStatus
                    );
                    break;
            }
        }

        syncDesiredKeysToKeyboard(&inputState, runner->keyboard, desiredKeys);

        OSTime vmStart = OSGetTime();
        if (firstFrameTracePending) bootLog("frame: before Runner_step");
        Runner_step(runner);
        if (firstFrameTracePending) bootLog("frame: after Runner_step");
        OSTime vmEnd = OSGetTime();
        perfVmMs += elapsedMs(vmStart, vmEnd);

        float deltaTime = (float) OSTicksToMicroseconds(frameStartTime - lastFrameTime) / 1000000.0f;
        if (deltaTime < 0.0f) deltaTime = 0.0f;
        if (deltaTime > 0.1f) deltaTime = 0.1f;

        if (runner->audioSystem != NULL) {
            if (firstFrameTracePending) bootLog("frame: before audio update");
            runner->audioSystem->vtable->update(runner->audioSystem, deltaTime);
            if (firstFrameTracePending) bootLog("frame: after audio update");
        }

        Gen8* gen8 = &dataWin->gen8;
        int32_t nativeGameW = (int32_t) gen8->defaultWindowWidth;
        int32_t nativeGameH = (int32_t) gen8->defaultWindowHeight;
        int32_t gameW = clampRenderDimension(nativeGameW, 640, nativeGameW > 0 ? nativeGameW : 640);
        int32_t gameH = clampRenderDimension(nativeGameH, 480, nativeGameH > 0 ? nativeGameH : 480);
        float portScaleX = nativeGameW > 0 ? (float) gameW / (float) nativeGameW : 1.0f;
        float portScaleY = nativeGameH > 0 ? (float) gameH / (float) nativeGameH : 1.0f;

        OSTime renderStart = OSGetTime();

        if (!WHBProcIsRunning()) {
            perfRenderMs += elapsedMs(renderStart, OSGetTime());
            RunnerKeyboard_beginFrame(runner->keyboard);
            break;
        }

        if (firstFrameTracePending) bootLog("frame: before beginFrame");
        WiiURenderer_setClearColor((WiiURenderer*) renderer, runner->drawBackgroundColor ? runner->backgroundColor : 0x000000);
        renderer->vtable->beginFrame(renderer, gameW, gameH, gameW, gameH);
        if (firstFrameTracePending) bootLog("frame: after beginFrame");

        Room* activeRoom = runner->currentRoom;
        bool viewsEnabled = (activeRoom->flags & 1) != 0;
        bool anyViewRendered = false;

        if (viewsEnabled) {
            repeat(8, vi) {
                if (!activeRoom->views[vi].enabled) continue;

                runner->viewCurrent = vi;
                renderer->vtable->beginView(
                    renderer,
                    activeRoom->views[vi].viewX,
                    activeRoom->views[vi].viewY,
                    activeRoom->views[vi].viewWidth,
                    activeRoom->views[vi].viewHeight,
                    (int32_t) lroundf((float) activeRoom->views[vi].portX * portScaleX),
                    (int32_t) lroundf((float) activeRoom->views[vi].portY * portScaleY),
                    (int32_t) lroundf((float) activeRoom->views[vi].portWidth * portScaleX),
                    (int32_t) lroundf((float) activeRoom->views[vi].portHeight * portScaleY),
                    runner->views[vi].viewAngle
                );
                Runner_draw(runner);
                renderer->vtable->endView(renderer);
                anyViewRendered = true;
            }
        }

        if (!anyViewRendered) {
            runner->viewCurrent = 0;
            if (firstFrameTracePending) bootLog("frame: before Runner_draw");
            renderer->vtable->beginView(renderer, 0, 0, gameW, gameH, 0, 0, gameW, gameH, 0.0f);
            Runner_draw(runner);
            renderer->vtable->endView(renderer);
            if (firstFrameTracePending) bootLog("frame: after Runner_draw");
        }

        runner->viewCurrent = 0;
        if (firstFrameTracePending) bootLog("frame: before endFrame");
        renderer->vtable->endFrame(renderer);
        if (firstFrameTracePending) bootLog("frame: after endFrame");
        OSTime renderEnd = OSGetTime();
        perfRenderMs += elapsedMs(renderStart, renderEnd);

        RunnerKeyboard_beginFrame(runner->keyboard);
        if (firstFrameTracePending) {
            bootLog("frame: first frame complete");
            firstFrameTracePending = false;
        }

        perfFrameCount++;
        if (perfFrameCount >= 60) {
            perfFrameCount = 0;
            perfVmMs = 0.0;
            perfRenderMs = 0.0;
        }

        OSTime frameEndTime = OSGetTime();
        double frameElapsedMs = elapsedMs(frameStartTime, frameEndTime);
        double targetFrameMs = 1000.0 / ((runner->currentRoom != NULL && runner->currentRoom->speed > 0)
            ? (double) runner->currentRoom->speed
            : 30.0);
        if (frameElapsedMs < targetFrameMs) {
            useconds_t remainingUs = (useconds_t)((targetFrameMs - frameElapsedMs) * 1000.0);
            if (remainingUs > 0) usleep(remainingUs);
            lastFrameTime = OSGetTime();
        } else {
            lastFrameTime = frameEndTime;
        }
    }


shutdown:
    bootLog("shutdown: begin");

    if (audio != NULL) {
        bootLog("shutdown: before audio destroy");
        audio->base.vtable->destroy((AudioSystem*) audio);
        audio = NULL;
        bootLog("shutdown: after audio destroy");
    }

    if (loadingThreadStarted && !loadingThreadJoined) {
        bootLog("shutdown: before loading thread detach");
        OSDetachThread(&loadingThread);
        bootLog("shutdown: after loading thread detach");
    }

    if (ProcUIInForeground()) {
        bootLog("shutdown: before GX2DrawDone [fg]");
        GX2DrawDone();
        bootLog("shutdown: after GX2DrawDone");
        WHBGfxShutdown();
        bootLog("shutdown: after WHBGfxShutdown");
    } else {
        bootLog("shutdown: GX2Shutdown [bg]");
        GX2Shutdown();
        bootLog("shutdown: after GX2Shutdown");
    }

    bootLog("shutdown: before WHBProcShutdown");
    if (gBootLogFd >= 0) {
        close(gBootLogFd);
        gBootLogFd = -1;
    }
    WHBUnmountSdCard();
    WHBProcShutdown();
    return 0;
}
