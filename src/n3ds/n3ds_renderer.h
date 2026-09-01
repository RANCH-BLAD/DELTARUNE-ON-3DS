#pragma once

#include "../renderer.h"

Renderer* N3DSRenderer_create(void);
bool N3DSRenderer_isReady(Renderer* renderer);
const char* N3DSRenderer_getStartupError(Renderer* renderer);
void N3DSRenderer_beginOverlay(Renderer* renderer);
void N3DSRenderer_beginBottomScreenGUIEx(Renderer* renderer, int32_t guiW, int32_t guiH, float scaleX, float scaleY, float offsetX, float offsetY);
void N3DSRenderer_beginBottomScreenGUI(Renderer* renderer, int32_t guiW, int32_t guiH);
void N3DSRenderer_endBottomScreenGUI(Renderer* renderer);
void N3DSRenderer_beginBottomScreenGUI2x(Renderer* renderer, int32_t guiW, int32_t guiH);
void N3DSRenderer_endBottomScreenGUI2x(Renderer* renderer);
void N3DSRenderer_beginTopScreenGUI(Renderer* renderer, int32_t guiW, int32_t guiH);
void N3DSRenderer_endTopScreenGUI(Renderer* renderer);
void N3DSRenderer_beginTopScreenGUI2x(Renderer* renderer, int32_t guiW, int32_t guiH);
void N3DSRenderer_endTopScreenGUI2x(Renderer* renderer);
bool N3DSRenderer_isTopScreenGUIActive(Renderer* renderer);
bool N3DSRenderer_isTopScreenBattleViewActive(Renderer* renderer);
void N3DSRenderer_setTopScreenBattleViewActive(Renderer* renderer, bool active);
uint32_t N3DSRenderer_getResidentAtlasVRAMBytes(Renderer* renderer);
uint32_t N3DSRenderer_getResidentAtlasVRAMLimitBytes(Renderer* renderer);
uint32_t N3DSRenderer_getResidentAtlasPageCount(Renderer* renderer);
uint32_t N3DSRenderer_getResidentAtlasPageLimit(Renderer* renderer);
uint32_t N3DSRenderer_getResidentDirectAssetVRAMBytes(Renderer* renderer);
uint32_t N3DSRenderer_getResidentDirectAssetVRAMLimitBytes(Renderer* renderer);
uint32_t N3DSRenderer_getFrameFragmentDraws(Renderer* renderer);
uint32_t N3DSRenderer_getFrameSpriteDrawCalls(Renderer* renderer);
uint32_t N3DSRenderer_getFrameSpritePartDrawCalls(Renderer* renderer);
uint32_t N3DSRenderer_getFrameDirectSpriteHits(Renderer* renderer);
uint32_t N3DSRenderer_getFrameDirectAssetLoads(Renderer* renderer);
uint32_t N3DSRenderer_getFrameTextureSwitches(Renderer* renderer);
uint32_t N3DSRenderer_getFrameTextGlyphDraws(Renderer* renderer);
uint32_t N3DSRenderer_getFrameTextTenthsMs(Renderer* renderer);
int32_t N3DSRenderer_findTileEntryIndex(Renderer* renderer, int32_t backgroundIndex, int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH);
bool N3DSRenderer_drawCachedTileEntry(Renderer* renderer, int32_t tileEntryIndex, float drawX, float drawY, float xscale, float yscale, uint32_t color, float alpha);
