#include "wiiu_renderer.h"

#include "textured_quad_gsh.h"

#include <gx2/clear.h>
#include <gx2/display.h>
#include <gx2/aperture.h>
#include <gx2/context.h>
#include <gx2/draw.h>
#include <gx2/enum.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/shaders.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <gx2/swap.h>
#include <gx2/texture.h>
#include <gx2/utils.h>
#include <gx2r/buffer.h>
#include <gx2r/draw.h>
#include <gx2r/surface.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/time.h>

#include <math.h>
#include <limits.h>
#include <malloc.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../matrix_math.h"
#include <stb/image/stb_image.h>
#include "../text_utils.h"
#include "../utils.h"

#define WIIU_MAX_QUADS 4096
#define WIIU_VERTICES_PER_QUAD 6

// Render to the full Wii U scan-buffer width and center the game's 4:3 image inside it.
#define WIIU_RENDER_WIDTH  854
#define WIIU_RENDER_HEIGHT 480

__attribute__((weak)) void WiiURenderer_platformBootLog(const char* message) {
    (void) message;
}

static void WiiURenderer_bootLog(const char* message) {
    WiiURenderer_platformBootLog(message);
}

static void WiiURenderer_releaseTextureBlob(DataWin* dataWin, uint32_t index);
static void WiiURenderer_emitVertex(WiiURenderer* renderer, WiiUVec2 clip, float u, float v, uint32_t color, float alpha);
static void WiiURenderer_computeOutputLayout(WiiUPresentLayout* layout, uint32_t outputWidth, uint32_t outputHeight, uint32_t sourceWidth, uint32_t sourceHeight, bool preferIntegerScale);
static bool WiiURenderer_initLinearTexture(GX2Texture* texture, uint32_t width, uint32_t height);
static void WiiURenderer_destroyRenderTarget(WiiURenderTarget* target);
static bool WiiURenderer_initRenderTarget(WiiURenderTarget* target, uint32_t width, uint32_t height);
static void WiiURenderer_ensureSceneTarget(WiiURenderer* renderer, uint32_t width, uint32_t height);
static void WiiURenderer_computeIntegerBlitRect(uint32_t sourceWidth, uint32_t sourceHeight, uint32_t targetWidth, uint32_t targetHeight, float* left, float* top, float* right, float* bottom);
static bool WiiURenderer_ensureVertexCapacity(WiiURenderer* renderer, uint32_t neededVertices);
static bool WiiURenderer_mapVertexBuffer(WiiURenderer* renderer);

static void* WiiURenderer_gpuAlloc(size_t alignment, size_t size) {
    return MEMAllocFromDefaultHeapEx((uint32_t) size, (int32_t) alignment);
}

static void WiiURenderer_gpuFree(void* ptr) {
    if (ptr != NULL) {
        MEMFreeToDefaultHeap(ptr);
    }
}

static double WiiURenderer_elapsedMs(OSTime start, OSTime end) {
    return (double) OSTicksToMicroseconds(end - start) / 1000.0;
}

static float WiiURenderer_snapPixel(float value) {
    return floorf(value + 0.5f);
}

static void WiiURenderer_updatePresentLayout(WiiURenderer* renderer) {
    uint32_t srcW = (renderer->frameWidth  > 0) ? (uint32_t) renderer->frameWidth  : 640u;
    uint32_t srcH = (renderer->frameHeight > 0) ? (uint32_t) renderer->frameHeight : 480u;
    WiiURenderer_computeOutputLayout(
        &renderer->presentLayout,
        WIIU_RENDER_WIDTH,
        WIIU_RENDER_HEIGHT,
        srcW,
        srcH,
        false
    );
}

static void WiiURenderer_computeOutputLayout(
    WiiUPresentLayout* layout,
    uint32_t outputWidth,
    uint32_t outputHeight,
    uint32_t sourceWidth,
    uint32_t sourceHeight,
    bool preferIntegerScale
) {
    if (layout == NULL || sourceWidth == 0 || sourceHeight == 0 || outputWidth == 0 || outputHeight == 0) {
        return;
    }

    float fitScale = fminf(
        (float) outputWidth / (float) sourceWidth,
        (float) outputHeight / (float) sourceHeight
    );
    float scale = fitScale;

    if (preferIntegerScale) {
        float integerScale = floorf(fitScale);
        if (integerScale >= 2.0f) {
            scale = integerScale;
        }
    }

    float targetWidth = (float) sourceWidth * scale;
    float targetHeight = (float) sourceHeight * scale;
    if (targetWidth > (float) outputWidth) targetWidth = (float) outputWidth;
    if (targetHeight > (float) outputHeight) targetHeight = (float) outputHeight;

    uint32_t snappedWidth = (uint32_t) lroundf(targetWidth);
    uint32_t snappedHeight = (uint32_t) lroundf(targetHeight);
    if (snappedWidth > outputWidth) snappedWidth = outputWidth;
    if (snappedHeight > outputHeight) snappedHeight = outputHeight;

    layout->targetWidth = (float) snappedWidth;
    layout->targetHeight = (float) snappedHeight;
    layout->xOffset = (outputWidth - snappedWidth) / 2u;
    layout->yOffset = (outputHeight - snappedHeight) / 2u;
}

void WiiURenderer_refreshOutputState(WiiURenderer* renderer) {
    if (renderer == NULL) return;

    WiiURenderer_updatePresentLayout(renderer);
}



static void WiiURenderer_destroyTexturePage(WiiUTexturePage* page) {
    if (page->texture.surface.image != NULL) {
        WiiURenderer_gpuFree(page->texture.surface.image);
    }
    memset(page, 0, sizeof(*page));
}

static void WiiURenderer_destroyRenderTarget(WiiURenderTarget* target) {
    if (target->colorBuffer.surface.image != NULL) {
        WiiURenderer_gpuFree(target->colorBuffer.surface.image);
    }
    memset(target, 0, sizeof(*target));
}

static bool WiiURenderer_initRenderTarget(WiiURenderTarget* target, uint32_t width, uint32_t height) {
    memset(target, 0, sizeof(*target));

    target->colorBuffer.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    target->colorBuffer.surface.width = width;
    target->colorBuffer.surface.height = height;
    target->colorBuffer.surface.depth = 1;
    target->colorBuffer.surface.mipLevels = 1;
    target->colorBuffer.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    target->colorBuffer.surface.aa = GX2_AA_MODE1X;
    target->colorBuffer.surface.use = GX2_SURFACE_USE_COLOR_BUFFER;
    target->colorBuffer.surface.tileMode = GX2_TILE_MODE_DEFAULT;
    GX2CalcSurfaceSizeAndAlignment(&target->colorBuffer.surface);
    target->colorBuffer.surface.image = WiiURenderer_gpuAlloc(target->colorBuffer.surface.alignment, target->colorBuffer.surface.imageSize);
    if (target->colorBuffer.surface.image == NULL) {
        WiiURenderer_destroyRenderTarget(target);
        return false;
    }
    memset(target->colorBuffer.surface.image, 0, target->colorBuffer.surface.imageSize);

    target->colorBuffer.viewMip = 0;
    target->colorBuffer.viewFirstSlice = 0;
    target->colorBuffer.viewNumSlices = 1;
    target->colorBuffer.aaBuffer = NULL;
    target->colorBuffer.aaSize = 0;
    GX2InitColorBufferRegs(&target->colorBuffer);
    GX2Invalidate(
        GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_COLOR_BUFFER,
        target->colorBuffer.surface.image,
        target->colorBuffer.surface.imageSize
    );

    memset(&target->presentTexture, 0, sizeof(target->presentTexture));
    target->presentTexture.surface = target->colorBuffer.surface;
    target->presentTexture.viewFirstMip = 0;
    target->presentTexture.viewNumMips = 1;
    target->presentTexture.viewFirstSlice = 0;
    target->presentTexture.viewNumSlices = 1;
    target->presentTexture.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);
    GX2InitTextureRegs(&target->presentTexture);

    target->ready = true;
    return true;
}

static void WiiURenderer_ensureSceneTarget(WiiURenderer* renderer, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;

    if (renderer->sceneTarget.ready &&
        renderer->sceneTarget.colorBuffer.surface.width == width &&
        renderer->sceneTarget.colorBuffer.surface.height == height) {
        return;
    }

    WiiURenderer_destroyRenderTarget(&renderer->sceneTarget);
    if (!WiiURenderer_initRenderTarget(&renderer->sceneTarget, width, height)) {
        WiiURenderer_bootLog("wiiu_renderer: scene target allocation failed");
    }
}

static void WiiURenderer_computeIntegerBlitRect(
    uint32_t sourceWidth,
    uint32_t sourceHeight,
    uint32_t targetWidth,
    uint32_t targetHeight,
    float* left,
    float* top,
    float* right,
    float* bottom
) {
    uint32_t scaleX = targetWidth / sourceWidth;
    uint32_t scaleY = targetHeight / sourceHeight;
    uint32_t scale = scaleX < scaleY ? scaleX : scaleY;
    if (scale < 1u) scale = 1u;

    uint32_t rectW = sourceWidth * scale;
    uint32_t rectH = sourceHeight * scale;
    uint32_t offX = (targetWidth - rectW) / 2u;
    uint32_t offY = (targetHeight - rectH) / 2u;

    *left = ((float) offX / (float) targetWidth) * 2.0f - 1.0f;
    *right = ((float) (offX + rectW) / (float) targetWidth) * 2.0f - 1.0f;
    *top = 1.0f - ((float) offY / (float) targetHeight) * 2.0f;
    *bottom = 1.0f - ((float) (offY + rectH) / (float) targetHeight) * 2.0f;
}


static bool WiiURenderer_initLinearTexture(GX2Texture* texture, uint32_t width, uint32_t height) {
    memset(texture, 0, sizeof(*texture));
    texture->surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    texture->surface.width = width;
    texture->surface.height = height;
    texture->surface.depth = 1;
    texture->surface.mipLevels = 1;
    texture->surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    texture->surface.aa = GX2_AA_MODE1X;
    texture->surface.use = GX2_SURFACE_USE_TEXTURE;
    texture->surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    GX2CalcSurfaceSizeAndAlignment(&texture->surface);
    texture->surface.image = WiiURenderer_gpuAlloc(texture->surface.alignment, texture->surface.imageSize);
    if (texture->surface.image == NULL) {
        memset(texture, 0, sizeof(*texture));
        return false;
    }
    memset(texture->surface.image, 0, texture->surface.imageSize);
    texture->viewFirstMip = 0;
    texture->viewNumMips = 1;
    texture->viewFirstSlice = 0;
    texture->viewNumSlices = 1;
    texture->compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);
    GX2InitTextureRegs(texture);
    return true;
}

static bool WiiURenderer_uploadTexturePage(WiiUTexturePage* page, const uint8_t* pixels, uint32_t width, uint32_t height) {
    WiiURenderer_destroyTexturePage(page);
    if (pixels == NULL || width == 0 || height == 0) return false;
    if (!WiiURenderer_initLinearTexture(&page->texture, width, height)) return false;

    uint8_t* dst = (uint8_t*) page->texture.surface.image;
    if (dst == NULL) {
        WiiURenderer_destroyTexturePage(page);
        return false;
    }
    memset(dst, 0, page->texture.surface.imageSize);
    uint32_t dstPitchBytes = page->texture.surface.pitch * 4u;
    for (uint32_t y = 0; y < height; ++y) {
        memcpy(
            dst + (size_t) y * (size_t) dstPitchBytes,
            pixels + (size_t) y * (size_t) width * 4u,
            (size_t) width * 4u
        );
    }
    page->ready = true;
    return true;
}

static void WiiURenderer_loadTexturePages(WiiURenderer* renderer, DataWin* dataWin) {
    renderer->texturePageCount = dataWin->txtr.count;
    if (renderer->texturePageCount > 0) {
        renderer->texturePages = calloc(renderer->texturePageCount, sizeof(WiiUTexturePage));
        if (renderer->texturePages == NULL) {
            renderer->texturePageCount = 0;
            WiiURenderer_bootLog("wiiu_renderer: texture page table allocation failed");
            return;
        }
    }

    repeat(renderer->texturePageCount, i) {
        Texture* tex = &dataWin->txtr.textures[i];
        if (tex->blobSize == 0) continue;
        if (tex->blobSize > 50u * 1024u * 1024u) continue;

        uint8_t* pngData = tex->blobData;
        if (pngData == NULL) continue;

        int width = 0, height = 0, channels = 0;
        uint8_t* pixels = stbi_load_from_memory(pngData, (int) tex->blobSize,
                                                 &width, &height, &channels, 4);

        if (pixels == NULL) continue;

        if (width > 0 && height > 0) {
            WiiURenderer_uploadTexturePage(&renderer->texturePages[i], pixels,
                                           (uint32_t) width, (uint32_t) height);
        }
        stbi_image_free(pixels);
    }

    repeat(renderer->texturePageCount, i) {
        if (!renderer->texturePages[i].ready) continue;
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE,
                      renderer->texturePages[i].texture.surface.image,
                      renderer->texturePages[i].texture.surface.imageSize);
    }

    {
        const uint8_t whitePixel[4] = { 255, 255, 255, 255 };
        WiiURenderer_uploadTexturePage(&renderer->whiteTexture, whitePixel, 1, 1);
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE,
                      renderer->whiteTexture.texture.surface.image,
                      renderer->whiteTexture.texture.surface.imageSize);
    }
}

static void WiiURenderer_freeTexturePages(WiiURenderer* renderer) {
    repeat(renderer->texturePageCount, i) {
        WiiURenderer_destroyTexturePage(&renderer->texturePages[i]);
    }
    free(renderer->texturePages);
    renderer->texturePages = NULL;
    renderer->texturePageCount = 0;
    WiiURenderer_destroyTexturePage(&renderer->whiteTexture);
}

static void WiiURenderer_destroyVertexBuffer(WiiURenderer* renderer) {
    if (renderer->batchVertices != NULL && GX2RBufferExists(&renderer->batchVertexBuffer)) {
        GX2RUnlockBufferEx(&renderer->batchVertexBuffer, 0);
    }
    if (GX2RBufferExists(&renderer->batchVertexBuffer)) {
        GX2RDestroyBufferEx(&renderer->batchVertexBuffer, 0);
    }
    renderer->batchVertices = NULL;
    renderer->batchVertexCapacity = 0;
}

static bool WiiURenderer_ensureVertexCapacity(WiiURenderer* renderer, uint32_t neededVertices) {
    uint32_t needed = renderer->batchVertexCount + neededVertices;
    if (needed <= renderer->batchVertexCapacity) return true;

    uint32_t newCapacity = renderer->batchVertexCapacity == 0 ? WIIU_MAX_QUADS * WIIU_VERTICES_PER_QUAD : renderer->batchVertexCapacity;
    while (newCapacity < needed) {
        newCapacity *= 2;
    }

    GX2RBuffer newVertexBuffer = { 0 };
    newVertexBuffer.flags =
        GX2R_RESOURCE_BIND_VERTEX_BUFFER |
        GX2R_RESOURCE_USAGE_CPU_WRITE |
        GX2R_RESOURCE_USAGE_GPU_READ |
        GX2R_RESOURCE_USAGE_FORCE_MEM2;
    newVertexBuffer.elemSize = sizeof(WiiUBatchVertex);
    newVertexBuffer.elemCount = newCapacity;
    if (!GX2RCreateBuffer(&newVertexBuffer)) {
        WiiURenderer_bootLog("wiiu_renderer: failed to create GX2R vertex buffer");
        return false;
    }

    WiiUBatchVertex* newVertices = (WiiUBatchVertex*) GX2RLockBufferEx(&newVertexBuffer, 0);
    if (newVertices == NULL) {
        WiiURenderer_bootLog("wiiu_renderer: failed to lock new GX2R vertex buffer");
        GX2RDestroyBufferEx(&newVertexBuffer, 0);
        return false;
    }

    if (renderer->batchVertices != NULL && renderer->batchVertexCount > 0) {
        memcpy(newVertices, renderer->batchVertices, (size_t) renderer->batchVertexCount * sizeof(WiiUBatchVertex));
    }

    WiiURenderer_destroyVertexBuffer(renderer);
    renderer->batchVertexBuffer = newVertexBuffer;
    renderer->batchVertices = newVertices;
    renderer->batchVertexCapacity = newCapacity;
    return true;
}

static bool WiiURenderer_mapVertexBuffer(WiiURenderer* renderer) {
    if (renderer->batchVertices == NULL && GX2RBufferExists(&renderer->batchVertexBuffer)) {
        renderer->batchVertices = (WiiUBatchVertex*) GX2RLockBufferEx(&renderer->batchVertexBuffer, 0);
        if (renderer->batchVertices == NULL) {
            WiiURenderer_bootLog("wiiu_renderer: failed to lock GX2R vertex buffer");
            return false;
        }
    }
    return renderer->batchVertices != NULL;
}

static void WiiURenderer_releaseTextureBlob(DataWin* dataWin, uint32_t index) {
    if (dataWin == NULL || index >= dataWin->txtr.count) return;

    if (dataWin->txtr.textures[index].blobData != NULL) {
        free(dataWin->txtr.textures[index].blobData);
        dataWin->txtr.textures[index].blobData = NULL;
    }
}

static void WiiURenderer_pushCommand(WiiURenderer* renderer, const WiiUQuadCommand* command) {
    if (renderer->commandCount >= renderer->commandCapacity) {
        uint32_t newCapacity = renderer->commandCapacity == 0 ? 1024 : renderer->commandCapacity * 2;
        if (newCapacity > 8192) newCapacity = 8192;
        if (renderer->commandCount >= newCapacity) return; // drop rather than crash
        WiiUQuadCommand* newCommands = realloc(renderer->commands, (size_t) newCapacity * sizeof(WiiUQuadCommand));
        if (newCommands == NULL) {
            WiiURenderer_bootLog("wiiu_renderer: command buffer growth failed");
            return;
        }
        renderer->commands = newCommands;
        renderer->commandCapacity = newCapacity;
    }
    renderer->commands[renderer->commandCount++] = *command;
}

static void WiiURenderer_bindShader(WiiURenderer* renderer) {
    GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_BLOCK);
    GX2SetShaderModeEx(
        renderer->shaderGroup.vertexShader->mode,
        GX2GetVertexShaderGPRs(renderer->shaderGroup.vertexShader),
        GX2GetVertexShaderStackEntries(renderer->shaderGroup.vertexShader),
        0,
        0,
        GX2GetPixelShaderGPRs(renderer->shaderGroup.pixelShader),
        GX2GetPixelShaderStackEntries(renderer->shaderGroup.pixelShader)
    );
    GX2SetFetchShader(&renderer->shaderGroup.fetchShader);
    GX2SetVertexShader(renderer->shaderGroup.vertexShader);
    GX2SetPixelShader(renderer->shaderGroup.pixelShader);
    GX2SetPixelSampler(&renderer->sampler, renderer->textureUnit);
    GX2SetStreamOutEnable(FALSE);
}

static void WiiURenderer_setCommonState(bool blendEnabled) {
    GX2SetColorControl(GX2_LOGIC_OP_COPY, blendEnabled ? 0x1 : 0x0, FALSE, TRUE);
    GX2SetBlendControl(
        GX2_RENDER_TARGET_0,
        GX2_BLEND_MODE_SRC_ALPHA,
        GX2_BLEND_MODE_INV_SRC_ALPHA,
        GX2_BLEND_COMBINE_MODE_ADD,
        blendEnabled ? GX2_ENABLE : GX2_DISABLE,
        GX2_BLEND_MODE_ONE,
        GX2_BLEND_MODE_INV_SRC_ALPHA,
        GX2_BLEND_COMBINE_MODE_ADD
    );
    GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA, 0, 0, 0, 0, 0, 0, 0);
    GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
    GX2SetAlphaTest(TRUE, GX2_COMPARE_FUNC_GREATER, 0.0f);
}

static void WiiURenderer_emitClipVertex(
    WiiURenderer* renderer,
    float clipX,
    float clipY,
    float u,
    float v
) {
    uint32_t index = renderer->batchVertexCount++;
    WiiUBatchVertex* vertex = &renderer->batchVertices[index];

    vertex->x = clipX;
    vertex->y = clipY;
    vertex->z = 0.0f;
    vertex->w = 1.0f;
    vertex->u = u;
    vertex->v = v;
    vertex->r = 1.0f;
    vertex->g = 1.0f;
    vertex->b = 1.0f;
    vertex->a = 1.0f;
}

static void WiiURenderer_emitSolidClipVertex(
    WiiURenderer* renderer,
    float clipX,
    float clipY,
    uint32_t color,
    float alpha
) {
    WiiURenderer_emitVertex(
        renderer,
        (WiiUVec2) { clipX, clipY },
        0.0f,
        0.0f,
        color,
        alpha
    );
}

static void WiiURenderer_flushVerticesWithTexture(WiiURenderer* renderer, uint32_t vertexCount, GX2Texture* texture) {
    if (vertexCount == 0 || texture == NULL) return;

    GX2SetPixelTexture(texture, renderer->textureUnit);

    if (renderer->batchVertices != NULL) {
        GX2RUnlockBufferEx(&renderer->batchVertexBuffer, 0);
        renderer->batchVertices = NULL;
    }
    GX2RInvalidateBuffer(&renderer->batchVertexBuffer, 0);
    GX2RSetAttributeBuffer(&renderer->batchVertexBuffer, 0, sizeof(WiiUBatchVertex), offsetof(WiiUBatchVertex, x));
    GX2RSetAttributeBuffer(&renderer->batchVertexBuffer, 1, sizeof(WiiUBatchVertex), offsetof(WiiUBatchVertex, u));
    GX2RSetAttributeBuffer(&renderer->batchVertexBuffer, 2, sizeof(WiiUBatchVertex), offsetof(WiiUBatchVertex, r));
    GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, vertexCount, 0, 1);
    GX2DrawDone();

    renderer->perfFlushCount++;
}

static void WiiURenderer_flushVertices(WiiURenderer* renderer, uint32_t vertexCount, int32_t texturePageId) {
    if (vertexCount == 0 || texturePageId < -1) return;

    GX2Texture* texture = texturePageId < 0
        ? &renderer->whiteTexture.texture
        : &renderer->texturePages[texturePageId].texture;
    WiiURenderer_flushVerticesWithTexture(renderer, vertexCount, texture);
}

static void WiiURenderer_renderLetterboxMasksToTarget(
    WiiURenderer* renderer,
    uint32_t targetWidth,
    uint32_t targetHeight,
    const WiiUPresentLayout* layout
) {
    uint32_t presentWidth = (uint32_t) lroundf(layout->targetWidth);
    uint32_t presentHeight = (uint32_t) lroundf(layout->targetHeight);
    if (presentWidth >= targetWidth && presentHeight >= targetHeight) return;

    float left = ((float) layout->xOffset / (float) targetWidth) * 2.0f - 1.0f;
    float right = ((float) (layout->xOffset + presentWidth) / (float) targetWidth) * 2.0f - 1.0f;
    WiiURenderer_bindShader(renderer);
    WiiURenderer_setCommonState(false);
    GX2SetViewport(0.0f, 0.0f, (float) targetWidth, (float) targetHeight, 0.0f, 1.0f);
    GX2SetScissor(0, 0, targetWidth, targetHeight);
    if (!WiiURenderer_ensureVertexCapacity(renderer, 12)) return;
    if (!WiiURenderer_mapVertexBuffer(renderer)) return;
    renderer->batchVertexCount = 0;

    #define WIIU_EMIT_MASK_QUAD(x0, y0, x1, y1) \
        do { \
            WiiURenderer_emitSolidClipVertex(renderer, (x0), (y0), 0x000000, 1.0f); \
            WiiURenderer_emitSolidClipVertex(renderer, (x1), (y0), 0x000000, 1.0f); \
            WiiURenderer_emitSolidClipVertex(renderer, (x0), (y1), 0x000000, 1.0f); \
            WiiURenderer_emitSolidClipVertex(renderer, (x0), (y1), 0x000000, 1.0f); \
            WiiURenderer_emitSolidClipVertex(renderer, (x1), (y0), 0x000000, 1.0f); \
            WiiURenderer_emitSolidClipVertex(renderer, (x1), (y1), 0x000000, 1.0f); \
        } while (0)

    if (left > -1.0f) {
        WIIU_EMIT_MASK_QUAD(-1.0f, 1.0f, left, -1.0f);
    }
    if (right < 1.0f) {
        WIIU_EMIT_MASK_QUAD(right, 1.0f, 1.0f, -1.0f);
    }

    #undef WIIU_EMIT_MASK_QUAD

    if (renderer->batchVertexCount > 0) {
        WiiURenderer_flushVertices(renderer, renderer->batchVertexCount, -1);
        renderer->batchVertexCount = 0;
    }
}

static WiiUVec2 WiiURenderer_worldToGame(WiiURenderer* renderer, float worldX, float worldY) {
    WiiUVec2 result;
    result.x = (float) renderer->portX + (worldX - (float) renderer->viewX) * renderer->viewScaleX;
    result.y = (float) renderer->portY + (worldY - (float) renderer->viewY) * renderer->viewScaleY;
    return result;
}

static WiiUVec2 WiiURenderer_gameToClip(
    const WiiURenderer* renderer,
    WiiUVec2 point,
    uint32_t targetWidth,
    uint32_t targetHeight,
    const WiiUPresentLayout* layout
) {
    WiiUVec2 result;
    float targetX = (float) layout->xOffset +
        ((point.x / (float) renderer->frameWidth) * layout->targetWidth);
    float targetY = (float) layout->yOffset +
        ((point.y / (float) renderer->frameHeight) * layout->targetHeight);
    result.x = (targetX / (float) targetWidth) * 2.0f - 1.0f;
    result.y = 1.0f - (targetY / (float) targetHeight) * 2.0f;
    return result;
}

static void WiiURenderer_emitVertex(WiiURenderer* renderer, WiiUVec2 clip, float u, float v, uint32_t color, float alpha) {
    uint32_t index = renderer->batchVertexCount++;
    WiiUBatchVertex* vertex = &renderer->batchVertices[index];

    vertex->x = clip.x;
    vertex->y = clip.y;
    vertex->z = 0.0f;
    vertex->w = 1.0f;
    vertex->u = u;
    vertex->v = v;
    vertex->r = (float) BGR_R(color) / 255.0f;
    vertex->g = (float) BGR_G(color) / 255.0f;
    vertex->b = (float) BGR_B(color) / 255.0f;
    vertex->a = alpha;
}

static bool WiiURenderer_quadIntersectsView(
    const WiiURenderer* renderer,
    WiiUVec2 p00,
    WiiUVec2 p10,
    WiiUVec2 p01
) {
    WiiUVec2 p11 = {
        p10.x + (p01.x - p00.x),
        p10.y + (p01.y - p00.y)
    };

    float minX = fminf(fminf(p00.x, p10.x), fminf(p01.x, p11.x));
    float maxX = fmaxf(fmaxf(p00.x, p10.x), fmaxf(p01.x, p11.x));
    float minY = fminf(fminf(p00.y, p10.y), fminf(p01.y, p11.y));
    float maxY = fmaxf(fmaxf(p00.y, p10.y), fmaxf(p01.y, p11.y));

    float viewMinX = (float) renderer->portX;
    float viewMinY = (float) renderer->portY;
    float viewMaxX = viewMinX + (float) renderer->portW;
    float viewMaxY = viewMinY + (float) renderer->portH;

    return maxX >= viewMinX && minX <= viewMaxX && maxY >= viewMinY && minY <= viewMaxY;
}

static void WiiURenderer_appendQuad(
    WiiURenderer* renderer,
    int32_t texturePageId,
    WiiUVec2 p00,
    WiiUVec2 p10,
    WiiUVec2 p01,
    float u0,
    float v0,
    float u1,
    float v1,
    uint32_t color,
    float alpha
) {
    if (renderer->frameWidth <= 0 || renderer->frameHeight <= 0) return;
    if (!WiiURenderer_quadIntersectsView(renderer, p00, p10, p01)) return;

    WiiUQuadCommand command;
    memset(&command, 0, sizeof(command));
    command.texturePageId = texturePageId;
    command.gradient = false;
    command.p00 = p00;
    command.p10 = p10;
    command.p01 = p01;
    command.u0 = u0;
    command.v0 = v0;
    command.u1 = u1;
    command.v1 = v1;
    command.color0 = color;
    command.color1 = color;
    command.alpha = alpha;
    WiiURenderer_pushCommand(renderer, &command);
    renderer->queuedQuadCount++;
}

static void WiiURenderer_appendGradientQuad(
    WiiURenderer* renderer,
    int32_t texturePageId,
    WiiUVec2 p00,
    WiiUVec2 p10,
    WiiUVec2 p01,
    float u0,
    float v0,
    float u1,
    float v1,
    uint32_t color0,
    uint32_t color1,
    float alpha
) {
    if (renderer->frameWidth <= 0 || renderer->frameHeight <= 0) return;
    if (!WiiURenderer_quadIntersectsView(renderer, p00, p10, p01)) return;

    WiiUQuadCommand command;
    memset(&command, 0, sizeof(command));
    command.texturePageId = texturePageId;
    command.gradient = true;
    command.p00 = p00;
    command.p10 = p10;
    command.p01 = p01;
    command.u0 = u0;
    command.v0 = v0;
    command.u1 = u1;
    command.v1 = v1;
    command.color0 = color0;
    command.color1 = color1;
    command.alpha = alpha;
    WiiURenderer_pushCommand(renderer, &command);
    renderer->queuedQuadCount++;
}

static void WiiURenderer_buildQuadVertices(
    WiiURenderer* renderer,
    const WiiUQuadCommand* command,
    uint32_t targetWidth,
    uint32_t targetHeight,
    const WiiUPresentLayout* layout
) {
    if (!WiiURenderer_ensureVertexCapacity(renderer, WIIU_VERTICES_PER_QUAD)) return;
    if (!WiiURenderer_mapVertexBuffer(renderer)) return;
    WiiUVec2 c00 = WiiURenderer_gameToClip(renderer, command->p00, targetWidth, targetHeight, layout);
    WiiUVec2 c10 = WiiURenderer_gameToClip(renderer, command->p10, targetWidth, targetHeight, layout);
    WiiUVec2 c01 = WiiURenderer_gameToClip(renderer, command->p01, targetWidth, targetHeight, layout);
    WiiUVec2 p11 = { command->p10.x + (command->p01.x - command->p00.x), command->p10.y + (command->p01.y - command->p00.y) };
    WiiUVec2 c11 = WiiURenderer_gameToClip(renderer, p11, targetWidth, targetHeight, layout);

    if (command->gradient) {
        WiiURenderer_emitVertex(renderer, c00, command->u0, command->v0, command->color0, command->alpha);
        WiiURenderer_emitVertex(renderer, c10, command->u1, command->v0, command->color1, command->alpha);
        WiiURenderer_emitVertex(renderer, c01, command->u0, command->v1, command->color0, command->alpha);
        WiiURenderer_emitVertex(renderer, c01, command->u0, command->v1, command->color0, command->alpha);
        WiiURenderer_emitVertex(renderer, c10, command->u1, command->v0, command->color1, command->alpha);
        WiiURenderer_emitVertex(renderer, c11, command->u1, command->v1, command->color1, command->alpha);
    } else {
        WiiURenderer_emitVertex(renderer, c00, command->u0, command->v0, command->color0, command->alpha);
        WiiURenderer_emitVertex(renderer, c10, command->u1, command->v0, command->color0, command->alpha);
        WiiURenderer_emitVertex(renderer, c01, command->u0, command->v1, command->color0, command->alpha);
        WiiURenderer_emitVertex(renderer, c01, command->u0, command->v1, command->color0, command->alpha);
        WiiURenderer_emitVertex(renderer, c10, command->u1, command->v0, command->color0, command->alpha);
        WiiURenderer_emitVertex(renderer, c11, command->u1, command->v1, command->color0, command->alpha);
    }
}

static void WiiURenderer_renderCommandsToTarget(
    WiiURenderer* renderer,
    uint32_t targetWidth,
    uint32_t targetHeight,
    const WiiUPresentLayout* layout
) {
    uint32_t presentWidth = (uint32_t) lroundf(layout->targetWidth);
    uint32_t presentHeight = (uint32_t) lroundf(layout->targetHeight);

    GX2SetViewport(0.0f, 0.0f, (float) targetWidth, (float) targetHeight, 0.0f, 1.0f);
    GX2SetScissor(
        layout->xOffset,
        layout->yOffset,
        presentWidth,
        presentHeight
    );
    renderer->batchVertexCount = 0;

    if (renderer->commandCount == 0) return;

    WiiURenderer_bindShader(renderer);
    WiiURenderer_setCommonState(true);
    int32_t currentTexturePageId = INT32_MIN;

    repeat(renderer->commandCount, i) {
        WiiUQuadCommand* command = &renderer->commands[i];
        if (renderer->batchVertexCount > 0 &&
            (command->texturePageId != currentTexturePageId ||
             renderer->batchVertexCount + WIIU_VERTICES_PER_QUAD > renderer->batchVertexCapacity)) {
            WiiURenderer_flushVertices(renderer, renderer->batchVertexCount, currentTexturePageId);
            renderer->batchVertexCount = 0;
        }

        currentTexturePageId = command->texturePageId;
        WiiURenderer_buildQuadVertices(renderer, command, targetWidth, targetHeight, layout);
    }

    if (renderer->batchVertexCount > 0) {
        WiiURenderer_flushVertices(renderer, renderer->batchVertexCount, currentTexturePageId);
        renderer->batchVertexCount = 0;
    }
}

static void WiiURenderer_renderCommandsToSceneTarget(WiiURenderer* renderer) {
    if (!renderer->sceneTarget.ready) return;

    uint32_t targetWidth = renderer->sceneTarget.colorBuffer.surface.width;
    uint32_t targetHeight = renderer->sceneTarget.colorBuffer.surface.height;
    WiiUPresentLayout layout = {
        .targetWidth = (float) targetWidth,
        .targetHeight = (float) targetHeight,
        .xOffset = 0,
        .yOffset = 0,
    };

    WiiURenderer_renderCommandsToTarget(renderer, targetWidth, targetHeight, &layout);
    GX2DrawDone();
    GX2Invalidate(
        GX2_INVALIDATE_MODE_COLOR_BUFFER | GX2_INVALIDATE_MODE_TEXTURE,
        renderer->sceneTarget.colorBuffer.surface.image,
        renderer->sceneTarget.colorBuffer.surface.imageSize
    );
}

static void WiiURenderer_renderPresentTextureToTarget(
    WiiURenderer* renderer,
    GX2ColorBuffer* targetBuffer,
    uint32_t targetWidth,
    uint32_t targetHeight,
    const WiiUPresentLayout* layout,
    GX2Texture* texture
) {
    if (targetBuffer == NULL || layout == NULL || texture == NULL) return;

    uint32_t presentWidth = (uint32_t) lroundf(layout->targetWidth);
    uint32_t presentHeight = (uint32_t) lroundf(layout->targetHeight);
    if (presentWidth == 0 || presentHeight == 0) return;

    float x0 = ((float) layout->xOffset / (float) targetWidth) * 2.0f - 1.0f;
    float y0 = 1.0f - ((float) (layout->yOffset + presentHeight) / (float) targetHeight) * 2.0f;
    float x1 = ((float) (layout->xOffset + presentWidth) / (float) targetWidth) * 2.0f - 1.0f;
    float y1 = 1.0f - ((float) layout->yOffset / (float) targetHeight) * 2.0f;

    GX2SetColorBuffer(targetBuffer, GX2_RENDER_TARGET_0);
    WiiURenderer_bindShader(renderer);
    WiiURenderer_setCommonState(false);
    GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
    GX2SetViewport(0.0f, 0.0f, (float) targetWidth, (float) targetHeight, 0.0f, 1.0f);
    GX2SetScissor(layout->xOffset, layout->yOffset, presentWidth, presentHeight);

    if (!WiiURenderer_ensureVertexCapacity(renderer, WIIU_VERTICES_PER_QUAD)) return;
    if (!WiiURenderer_mapVertexBuffer(renderer)) return;
    renderer->batchVertexCount = 0;

    WiiURenderer_emitClipVertex(renderer, x0, y1, 0.0f, 0.0f);
    WiiURenderer_emitClipVertex(renderer, x1, y1, 1.0f, 0.0f);
    WiiURenderer_emitClipVertex(renderer, x0, y0, 0.0f, 1.0f);
    WiiURenderer_emitClipVertex(renderer, x0, y0, 0.0f, 1.0f);
    WiiURenderer_emitClipVertex(renderer, x1, y1, 1.0f, 0.0f);
    WiiURenderer_emitClipVertex(renderer, x1, y0, 1.0f, 1.0f);

    WiiURenderer_flushVerticesWithTexture(renderer, renderer->batchVertexCount, texture);
    renderer->batchVertexCount = 0;
}

static bool WiiURenderer_initShaderPipeline(WiiURenderer* renderer) {
    memset(&renderer->shaderGroup, 0, sizeof(renderer->shaderGroup));
    if (!WHBGfxLoadGFDShaderGroup(&renderer->shaderGroup, 0, resources_wiiu_shaders_textured_quad_gsh)) {
        WiiURenderer_bootLog("wiiu_renderer: failed to load shader group");
        return false;
    }
    if (!WHBGfxInitShaderAttribute(&renderer->shaderGroup, "aPosition", 0, offsetof(WiiUBatchVertex, x), GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32)) {
        WiiURenderer_bootLog("wiiu_renderer: failed to init aPosition");
        WHBGfxFreeShaderGroup(&renderer->shaderGroup);
        memset(&renderer->shaderGroup, 0, sizeof(renderer->shaderGroup));
        return false;
    }
    if (!WHBGfxInitShaderAttribute(&renderer->shaderGroup, "aTexCoord", 0, offsetof(WiiUBatchVertex, u), GX2_ATTRIB_FORMAT_FLOAT_32_32)) {
        WiiURenderer_bootLog("wiiu_renderer: failed to init aTexCoord");
        WHBGfxFreeShaderGroup(&renderer->shaderGroup);
        memset(&renderer->shaderGroup, 0, sizeof(renderer->shaderGroup));
        return false;
    }
    if (!WHBGfxInitShaderAttribute(&renderer->shaderGroup, "aColour", 0, offsetof(WiiUBatchVertex, r), GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32)) {
        WiiURenderer_bootLog("wiiu_renderer: failed to init aColour");
        WHBGfxFreeShaderGroup(&renderer->shaderGroup);
        memset(&renderer->shaderGroup, 0, sizeof(renderer->shaderGroup));
        return false;
    }
    if (!WHBGfxInitFetchShader(&renderer->shaderGroup)) {
        WiiURenderer_bootLog("wiiu_renderer: failed to init fetch shader");
        WHBGfxFreeShaderGroup(&renderer->shaderGroup);
        memset(&renderer->shaderGroup, 0, sizeof(renderer->shaderGroup));
        return false;
    }

    renderer->textureUnit = 0;
    GX2InitSampler(&renderer->sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_POINT);
    GX2InitSamplerZMFilter(&renderer->sampler, GX2_TEX_Z_FILTER_MODE_NONE, GX2_TEX_MIP_FILTER_MODE_NONE);
    renderer->shaderReady = true;
    return true;
}

static void WiiURenderer_init(Renderer* base, DataWin* dataWin) {
    WiiURenderer_bootLog("wiiu_renderer: init begin");
    WiiURenderer* renderer = (WiiURenderer*) base;
    base->dataWin = dataWin;

    if (!WiiURenderer_initShaderPipeline(renderer)) {
        return;
    }

    WiiURenderer_refreshOutputState(renderer);
    WiiURenderer_bootLog("wiiu_renderer: manual present layout ready");

    renderer->clearR = 0;
    renderer->clearG = 0;
    renderer->clearB = 0;

    WiiURenderer_loadTexturePages(renderer, dataWin);
    WiiURenderer_bootLog("wiiu_renderer: gpu path ready");
    WiiURenderer_bootLog("wiiu_renderer: init end");
}

static void WiiURenderer_destroy(Renderer* base) {
    WiiURenderer* renderer = (WiiURenderer*) base;

    WiiURenderer_freeTexturePages(renderer);
    WiiURenderer_destroyRenderTarget(&renderer->sceneTarget);
    WiiURenderer_destroyVertexBuffer(renderer);
    if (renderer->shaderReady) {
        WHBGfxFreeShaderGroup(&renderer->shaderGroup);
    }
    free(renderer->commands);
    free(renderer);
}

static void WiiURenderer_beginFrame(Renderer* base, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    (void) windowW;
    (void) windowH;

    WiiURenderer* renderer = (WiiURenderer*) base;
    renderer->frameWidth = gameW;
    renderer->frameHeight = gameH;
    WiiURenderer_ensureSceneTarget(renderer, (uint32_t) gameW, (uint32_t) gameH);
    WiiURenderer_updatePresentLayout(renderer);
    renderer->viewX = 0;
    renderer->viewY = 0;
    renderer->viewW = gameW;
    renderer->viewH = gameH;
    renderer->portX = 0;
    renderer->portY = 0;
    renderer->portW = gameW;
    renderer->portH = gameH;
    renderer->viewScaleX = 1.0f;
    renderer->viewScaleY = 1.0f;
    renderer->commandCount = 0;
    renderer->batchVertexCount = 0;
    renderer->queuedQuadCount = 0;
    if (renderer->commandCapacity > 2048 && renderer->commands != NULL) {
        WiiUQuadCommand* resizedCommands = realloc(renderer->commands, (size_t) 2048 * sizeof(WiiUQuadCommand));
        if (resizedCommands != NULL) {
            renderer->commands = resizedCommands;
            renderer->commandCapacity = 2048;
        }
    }
}

static void WiiURenderer_beginView(Renderer* base, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH, int32_t portX, int32_t portY, int32_t portW, int32_t portH, float viewAngle) {
    (void) viewAngle;

    WiiURenderer* renderer = (WiiURenderer*) base;
    renderer->viewX = viewX;
    renderer->viewY = viewY;
    renderer->viewW = viewW != 0 ? viewW : 1;
    renderer->viewH = viewH != 0 ? viewH : 1;
    renderer->portX = portX;
    renderer->portY = portY;
    renderer->portW = portW;
    renderer->portH = portH;
    renderer->viewScaleX = (float) portW / (float) renderer->viewW;
    renderer->viewScaleY = (float) portH / (float) renderer->viewH;
}

static void WiiURenderer_endView(Renderer* base) {
    (void) base;
}

static void WiiURenderer_beginGUI(Renderer* base, int32_t guiW, int32_t guiH, int32_t portX, int32_t portY, int32_t portW, int32_t portH) {
    WiiURenderer_beginView(base, 0, 0, guiW, guiH, portX, portY, portW, portH, 0.0f);
}

static void WiiURenderer_endGUI(Renderer* base) {
    WiiURenderer_endView(base);
}

static void WiiURenderer_endFrame(Renderer* base) {
    WiiURenderer* renderer = (WiiURenderer*) base;
    GX2ColorBuffer* tvScan = WHBGfxGetTVColourBuffer();
    GX2ColorBuffer* drcScan = WHBGfxGetDRCColourBuffer();
    bool traceFrame = renderer->debugFrameIndex < 2;
    if (!renderer->shaderReady || tvScan == NULL || drcScan == NULL) {
        WiiURenderer_bootLog("wiiu_renderer: missing scan buffers");
        return;
    }

    if (renderer->debugFrameIndex < 12) {
        char summary[160];
        snprintf(
            summary,
            sizeof(summary),
            "wiiu_renderer: frame=%u cmds=%u quads=%u frame=%dx%d view=%d,%d %dx%d port=%d,%d %dx%d",
            renderer->debugFrameIndex,
            renderer->commandCount,
            renderer->queuedQuadCount,
            renderer->frameWidth,
            renderer->frameHeight,
            renderer->viewX,
            renderer->viewY,
            renderer->viewW,
            renderer->viewH,
            renderer->portX,
            renderer->portY,
            renderer->portW,
            renderer->portH
        );
        WiiURenderer_bootLog(summary);

        uint32_t sampleCount = renderer->commandCount < 8 ? renderer->commandCount : 8;
        for (uint32_t i = 0; i < sampleCount; ++i) {
            WiiUQuadCommand* command = &renderer->commands[i];
            char detail[256];
            snprintf(
                detail,
                sizeof(detail),
                "wiiu_renderer: cmd[%u] tex=%d uv=(%.4f,%.4f)-(%.4f,%.4f) p00=(%.1f,%.1f) p10=(%.1f,%.1f) p01=(%.1f,%.1f) alpha=%.3f color=%06X grad=%d",
                i,
                command->texturePageId,
                command->u0,
                command->v0,
                command->u1,
                command->v1,
                command->p00.x,
                command->p00.y,
                command->p10.x,
                command->p10.y,
                command->p01.x,
                command->p01.y,
                command->alpha,
                command->color0 & 0xFFFFFFu,
                command->gradient ? 1 : 0
            );
            WiiURenderer_bootLog(detail);
        }
    }

    OSTime tvStart = 0;
    OSTime tvEnd = 0;
    OSTime drcStart = 0;
    OSTime drcEnd = 0;
    WiiUPresentLayout tvLayout = { 0 };
    WiiUPresentLayout drcLayout = { 0 };
    GX2ContextState* tvContext = WHBGfxGetTVContextState();

    if (traceFrame) WiiURenderer_bootLog("wiiu_renderer: before WHBGfxBeginRender");
    WHBGfxBeginRender();
    if (traceFrame) WiiURenderer_bootLog("wiiu_renderer: after WHBGfxBeginRender");
    WiiURenderer_refreshOutputState(renderer);
    WiiURenderer_computeOutputLayout(
        &drcLayout,
        drcScan->surface.width,
        drcScan->surface.height,
        (uint32_t) renderer->frameWidth,
        (uint32_t) renderer->frameHeight,
        false
    );
    WiiURenderer_computeOutputLayout(
        &tvLayout,
        tvScan->surface.width,
        tvScan->surface.height,
        (uint32_t) renderer->frameWidth,
        (uint32_t) renderer->frameHeight,
        true
    );

    tvStart = OSGetTime();
    if (traceFrame) WiiURenderer_bootLog("wiiu_renderer: before WHBGfxBeginRenderTV");
    WHBGfxBeginRenderTV();
    if (traceFrame) WiiURenderer_bootLog("wiiu_renderer: after WHBGfxBeginRenderTV");

    GX2SetColorBuffer(&renderer->sceneTarget.colorBuffer, GX2_RENDER_TARGET_0);
    GX2SetViewport(
        0.0f,
        0.0f,
        (float) renderer->sceneTarget.colorBuffer.surface.width,
        (float) renderer->sceneTarget.colorBuffer.surface.height,
        0.0f,
        1.0f
    );
    GX2SetScissor(0, 0, renderer->sceneTarget.colorBuffer.surface.width, renderer->sceneTarget.colorBuffer.surface.height);
    GX2ClearColor(
        &renderer->sceneTarget.colorBuffer,
        (float) renderer->clearR / 255.0f,
        (float) renderer->clearG / 255.0f,
        (float) renderer->clearB / 255.0f,
        1.0f
    );
    if (tvContext != NULL) {
        GX2SetContextState(tvContext);
    }
    GX2SetColorBuffer(&renderer->sceneTarget.colorBuffer, GX2_RENDER_TARGET_0);
    WiiURenderer_renderCommandsToSceneTarget(renderer);
    GX2SetColorBuffer(tvScan, GX2_RENDER_TARGET_0);
    GX2SetViewport(0.0f, 0.0f, (float) tvScan->surface.width, (float) tvScan->surface.height, 0.0f, 1.0f);
    GX2SetScissor(0, 0, tvScan->surface.width, tvScan->surface.height);
    WHBGfxClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    WiiURenderer_renderPresentTextureToTarget(
        renderer,
        tvScan,
        tvScan->surface.width,
        tvScan->surface.height,
        &tvLayout,
        &renderer->sceneTarget.presentTexture
    );
    if (traceFrame) WiiURenderer_bootLog("wiiu_renderer: before WHBGfxFinishRenderTV");
    WHBGfxFinishRenderTV();
    if (traceFrame) WiiURenderer_bootLog("wiiu_renderer: after WHBGfxFinishRenderTV");
    tvEnd = OSGetTime();

    drcStart = OSGetTime();
    if (traceFrame) WiiURenderer_bootLog("wiiu_renderer: before WHBGfxBeginRenderDRC");
    WHBGfxBeginRenderDRC();
    if (traceFrame) WiiURenderer_bootLog("wiiu_renderer: after WHBGfxBeginRenderDRC");
    GX2SetScissor(0, 0, drcScan->surface.width, drcScan->surface.height);
    WHBGfxClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    WiiURenderer_renderPresentTextureToTarget(
        renderer,
        drcScan,
        drcScan->surface.width,
        drcScan->surface.height,
        &drcLayout,
        &renderer->sceneTarget.presentTexture
    );
    if (traceFrame) WiiURenderer_bootLog("wiiu_renderer: before WHBGfxFinishRenderDRC");
    WHBGfxFinishRenderDRC();
    if (traceFrame) WiiURenderer_bootLog("wiiu_renderer: after WHBGfxFinishRenderDRC");
    if (traceFrame) WiiURenderer_bootLog("wiiu_renderer: before GX2DrawDone");
    GX2DrawDone();
    if (traceFrame) WiiURenderer_bootLog("wiiu_renderer: after GX2DrawDone");
    drcEnd = OSGetTime();
    if (traceFrame) WiiURenderer_bootLog("wiiu_renderer: before WHBGfxFinishRender");
    WHBGfxFinishRender();
    if (traceFrame) WiiURenderer_bootLog("wiiu_renderer: after WHBGfxFinishRender");

    renderer->perfSceneMs += 0.0;
    renderer->perfRenderTvMs += WiiURenderer_elapsedMs(tvStart, tvEnd);
    renderer->perfRenderDrcMs += WiiURenderer_elapsedMs(drcStart, drcEnd);
    renderer->perfQuadCount += renderer->queuedQuadCount;
    renderer->perfFrameCount++;

    if (renderer->perfFrameCount >= 60) {
        char perfBuffer[256];
        snprintf(
            perfBuffer,
            sizeof(perfBuffer),
            "wiiu_renderer: avg over %u frames scene=%.2fms tv=%.2fms drc=%.2fms total=%.2fms quads=%u flush=%u",
            renderer->perfFrameCount,
            renderer->perfSceneMs / (double) renderer->perfFrameCount,
            renderer->perfRenderTvMs / (double) renderer->perfFrameCount,
            renderer->perfRenderDrcMs / (double) renderer->perfFrameCount,
            (renderer->perfSceneMs + renderer->perfRenderTvMs + renderer->perfRenderDrcMs) / (double) renderer->perfFrameCount,
            renderer->perfQuadCount / renderer->perfFrameCount,
            renderer->perfFlushCount / renderer->perfFrameCount
        );
        WiiURenderer_bootLog(perfBuffer);
        renderer->perfFrameCount = 0;
        renderer->perfSceneMs = 0.0;
        renderer->perfRenderTvMs = 0.0;
        renderer->perfRenderDrcMs = 0.0;
        renderer->perfFlushCount = 0;
        renderer->perfQuadCount = 0;
    }

    renderer->debugFrameIndex++;
}

static void WiiURenderer_drawSprite(Renderer* base, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    WiiURenderer* renderer = (WiiURenderer*) base;
    DataWin* dataWin = base->dataWin;
    if (tpagIndex < 0 || (uint32_t) tpagIndex >= dataWin->tpag.count) return;

    TexturePageItem* tpag = &dataWin->tpag.items[tpagIndex];
    if (tpag->texturePageId < 0 || (uint32_t) tpag->texturePageId >= renderer->texturePageCount) return;
    if (!renderer->texturePages[tpag->texturePageId].ready) return;

    GX2Texture* page = &renderer->texturePages[tpag->texturePageId].texture;
    x = WiiURenderer_snapPixel(x);
    y = WiiURenderer_snapPixel(y);
    float localX0 = (float) tpag->targetX - originX;
    float localY0 = (float) tpag->targetY - originY;
    float localX1 = localX0 + (float) tpag->sourceWidth;
    float localY1 = localY0 + (float) tpag->sourceHeight;

    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale, yscale, angleRad);

    float w0x, w0y, w1x, w1y, w2x, w2y;
    Matrix4f_transformPoint(&transform, localX0, localY0, &w0x, &w0y);
    Matrix4f_transformPoint(&transform, localX1, localY0, &w1x, &w1y);
    Matrix4f_transformPoint(&transform, localX0, localY1, &w2x, &w2y);

    WiiURenderer_appendQuad(
        renderer,
        tpag->texturePageId,
        WiiURenderer_worldToGame(renderer, w0x, w0y),
        WiiURenderer_worldToGame(renderer, w1x, w1y),
        WiiURenderer_worldToGame(renderer, w2x, w2y),
        (float) tpag->sourceX / (float) page->surface.width,
        (float) tpag->sourceY / (float) page->surface.height,
        (float) (tpag->sourceX + tpag->sourceWidth) / (float) page->surface.width,
        (float) (tpag->sourceY + tpag->sourceHeight) / (float) page->surface.height,
        color,
        alpha
    );
}

static void WiiURenderer_drawSpritePart(Renderer* base, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH, float x, float y, float xscale, float yscale, float angleDeg, float pivotX, float pivotY, uint32_t color, float alpha) {
    WiiURenderer* renderer = (WiiURenderer*) base;
    DataWin* dataWin = base->dataWin;
    (void) angleDeg;
    (void) pivotX;
    (void) pivotY;
    if (tpagIndex < 0 || (uint32_t) tpagIndex >= dataWin->tpag.count) return;

    TexturePageItem* tpag = &dataWin->tpag.items[tpagIndex];
    if (tpag->texturePageId < 0 || (uint32_t) tpag->texturePageId >= renderer->texturePageCount) return;
    if (!renderer->texturePages[tpag->texturePageId].ready) return;

    GX2Texture* page = &renderer->texturePages[tpag->texturePageId].texture;
    x = WiiURenderer_snapPixel(x);
    y = WiiURenderer_snapPixel(y);
    float x1 = x + (float) srcW * xscale;
    float y1 = y + (float) srcH * yscale;
    WiiURenderer_appendQuad(
        renderer,
        tpag->texturePageId,
        WiiURenderer_worldToGame(renderer, x, y),
        WiiURenderer_worldToGame(renderer, x1, y),
        WiiURenderer_worldToGame(renderer, x, y1),
        (float) (tpag->sourceX + srcOffX) / (float) page->surface.width,
        (float) (tpag->sourceY + srcOffY) / (float) page->surface.height,
        (float) (tpag->sourceX + srcOffX + srcW) / (float) page->surface.width,
        (float) (tpag->sourceY + srcOffY + srcH) / (float) page->surface.height,
        color,
        alpha
    );
}

static void WiiURenderer_drawRectangle(Renderer* base, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline) {
    WiiURenderer* renderer = (WiiURenderer*) base;

    if (outline) {
        renderer->base.vtable->drawLine(base, x1, y1, x2, y1, 1.0f, color, alpha);
        renderer->base.vtable->drawLine(base, x2, y1, x2, y2, 1.0f, color, alpha);
        renderer->base.vtable->drawLine(base, x2, y2, x1, y2, 1.0f, color, alpha);
        renderer->base.vtable->drawLine(base, x1, y2, x1, y1, 1.0f, color, alpha);
        return;
    }

    WiiURenderer_appendQuad(
        renderer,
        -1,
        WiiURenderer_worldToGame(renderer, x1, y1),
        WiiURenderer_worldToGame(renderer, x2, y1),
        WiiURenderer_worldToGame(renderer, x1, y2),
        0.0f,
        0.0f,
        1.0f,
        1.0f,
        color,
        alpha
    );
}

static void WiiURenderer_drawLine(Renderer* base, float x1, float y1, float x2, float y2, float width, uint32_t color, float alpha) {
    WiiURenderer* renderer = (WiiURenderer*) base;

    WiiUVec2 p0 = WiiURenderer_worldToGame(renderer, x1, y1);
    WiiUVec2 p1 = WiiURenderer_worldToGame(renderer, x2, y2);
    x1 = p0.x;
    y1 = p0.y;
    x2 = p1.x;
    y2 = p1.y;
    width *= fmaxf(renderer->viewScaleX, renderer->viewScaleY);

    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len <= 0.0001f) {
        WiiURenderer_appendQuad(renderer, -1, (WiiUVec2) { x1, y1 }, (WiiUVec2) { x1 + 1.0f, y1 }, (WiiUVec2) { x1, y1 + 1.0f }, 0.0f, 0.0f, 1.0f, 1.0f, color, alpha);
        return;
    }

    float half = fmaxf(width, 1.0f) * 0.5f;
    float nx = -dy / len * half;
    float ny = dx / len * half;
    WiiURenderer_appendQuad(
        renderer,
        -1,
        (WiiUVec2) { x1 - nx, y1 - ny },
        (WiiUVec2) { x2 - nx, y2 - ny },
        (WiiUVec2) { x1 + nx, y1 + ny },
        0.0f,
        0.0f,
        1.0f,
        1.0f,
        color,
        alpha
    );
}

static void WiiURenderer_drawLineColor(Renderer* base, float x1, float y1, float x2, float y2, float width, uint32_t color1, uint32_t color2, float alpha) {
    WiiURenderer* renderer = (WiiURenderer*) base;

    WiiUVec2 p0 = WiiURenderer_worldToGame(renderer, x1, y1);
    WiiUVec2 p1 = WiiURenderer_worldToGame(renderer, x2, y2);
    x1 = p0.x;
    y1 = p0.y;
    x2 = p1.x;
    y2 = p1.y;
    width *= fmaxf(renderer->viewScaleX, renderer->viewScaleY);

    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len <= 0.0001f) {
        renderer->base.vtable->drawRectangle(base, x1, y1, x1 + 1.0f, y1 + 1.0f, color1, alpha, false);
        return;
    }

    float half = fmaxf(width, 1.0f) * 0.5f;
    float nx = -dy / len * half;
    float ny = dx / len * half;
    WiiURenderer_appendGradientQuad(
        renderer,
        -1,
        (WiiUVec2) { x1 - nx, y1 - ny },
        (WiiUVec2) { x2 - nx, y2 - ny },
        (WiiUVec2) { x1 + nx, y1 + ny },
        0.0f,
        0.0f,
        1.0f,
        1.0f,
        color1,
        color2,
        alpha
    );
}

static void WiiURenderer_drawText(Renderer* base, const char* text, float x, float y, float xscale, float yscale, float angleDeg) {
    WiiURenderer* renderer = (WiiURenderer*) base;
    DataWin* dataWin = base->dataWin;

    int32_t fontIndex = base->drawFont;
    if (fontIndex < 0 || (uint32_t) fontIndex >= dataWin->font.count) return;

    Font* font = &dataWin->font.fonts[fontIndex];
    int32_t fontTpagIndex = font->tpagIndex;
    if (fontTpagIndex < 0 || (uint32_t) fontTpagIndex >= dataWin->tpag.count) return;

    TexturePageItem* fontTpag = &dataWin->tpag.items[fontTpagIndex];
    if (fontTpag->texturePageId < 0 || (uint32_t) fontTpag->texturePageId >= renderer->texturePageCount) return;
    if (!renderer->texturePages[fontTpag->texturePageId].ready) return;

    GX2Texture* page = &renderer->texturePages[fontTpag->texturePageId].texture;
    PreprocessedText processed = TextUtils_preprocessGmlText(text);
    const char* processedText = processed.text;
    int32_t textLen = (int32_t) strlen(processedText);
    int32_t lineCount = TextUtils_countLines(processedText, textLen);
    float totalHeight = (float) lineCount * (float) font->emSize;
    float valignOffset = 0.0f;
    if (base->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (base->drawValign == 2) valignOffset = -totalHeight;

    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;
    x = WiiURenderer_snapPixel(x);
    y = WiiURenderer_snapPixel(y);
    Matrix4f_setTransform2D(&transform, x, y, xscale * font->scaleX, yscale * font->scaleY, angleRad);

    float cursorY = valignOffset;
    int32_t lineStart = 0;
    repeat(lineCount, lineIdx) {
        int32_t lineEnd = lineStart;
        while (lineEnd < textLen && !TextUtils_isNewlineChar(processedText[lineEnd])) lineEnd++;
        int32_t lineLen = lineEnd - lineStart;

        float lineWidth = TextUtils_measureLineWidth(font, processedText + lineStart, lineLen);
        float halignOffset = 0.0f;
        if (base->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (base->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;
        int32_t pos = 0;
        while (pos < lineLen) {
            uint16_t ch = TextUtils_decodeUtf8(processedText + lineStart, lineLen, &pos);
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            if (glyph == NULL) continue;
            if (glyph->sourceWidth == 0 || glyph->sourceHeight == 0) {
                cursorX += glyph->shift;
                continue;
            }

            float localX0 = cursorX + glyph->offset;
            float localY0 = cursorY;
            float localX1 = localX0 + (float) glyph->sourceWidth;
            float localY1 = localY0 + (float) glyph->sourceHeight;

            float w0x, w0y, w1x, w1y, w2x, w2y;
            Matrix4f_transformPoint(&transform, localX0, localY0, &w0x, &w0y);
            Matrix4f_transformPoint(&transform, localX1, localY0, &w1x, &w1y);
            Matrix4f_transformPoint(&transform, localX0, localY1, &w2x, &w2y);

            WiiURenderer_appendQuad(
                renderer,
                fontTpag->texturePageId,
                WiiURenderer_worldToGame(renderer, w0x, w0y),
                WiiURenderer_worldToGame(renderer, w1x, w1y),
                WiiURenderer_worldToGame(renderer, w2x, w2y),
                (float) (fontTpag->sourceX + glyph->sourceX) / (float) page->surface.width,
                (float) (fontTpag->sourceY + glyph->sourceY) / (float) page->surface.height,
                (float) (fontTpag->sourceX + glyph->sourceX + glyph->sourceWidth) / (float) page->surface.width,
                (float) (fontTpag->sourceY + glyph->sourceY + glyph->sourceHeight) / (float) page->surface.height,
                base->drawColor,
                base->drawAlpha
            );

            cursorX += glyph->shift;
            if (pos < lineLen) {
                int32_t savedPos = pos;
                uint16_t nextCh = TextUtils_decodeUtf8(processedText + lineStart, lineLen, &pos);
                pos = savedPos;
                cursorX += TextUtils_getKerningOffset(glyph, nextCh);
            }
        }

        cursorY += (float) font->emSize;
        lineStart = TextUtils_skipNewline(processedText, lineEnd, textLen);
    }

    PreprocessedText_free(processed);
}

static void WiiURenderer_flush(Renderer* base) {
    (void) base;
}

static int32_t WiiURenderer_createSpriteFromSurface(Renderer* base, int32_t surfaceID, int32_t x, int32_t y, int32_t w, int32_t h, bool removeback, bool smooth, int32_t xorig, int32_t yorig) {
    (void) removeback;
    (void) surfaceID;
    (void) x;
    (void) y;
    (void) w;
    (void) h;
    (void) smooth;
    (void) xorig;
    (void) yorig;
    (void) base;
    WiiURenderer_bootLog("wiiu_renderer: createSpriteFromSurface unsupported on gpu path");
    return -1;
}

static void WiiURenderer_deleteSprite(Renderer* base, int32_t spriteIndex) {
    (void) base;
    (void) spriteIndex;
}

static RendererVtable WiiURendererVtable = {
    .init = WiiURenderer_init,
    .destroy = WiiURenderer_destroy,
    .beginFrame = WiiURenderer_beginFrame,
    .endFrame = WiiURenderer_endFrame,
    .beginView = WiiURenderer_beginView,
    .endView = WiiURenderer_endView,
    .beginGUI = WiiURenderer_beginGUI,
    .endGUI = WiiURenderer_endGUI,
    .drawSprite = WiiURenderer_drawSprite,
    .drawSpritePart = WiiURenderer_drawSpritePart,
    .drawRectangle = WiiURenderer_drawRectangle,
    .drawLine = WiiURenderer_drawLine,
    .drawLineColor = WiiURenderer_drawLineColor,
    .drawText = WiiURenderer_drawText,
    .flush = WiiURenderer_flush,
    .createSpriteFromSurface = WiiURenderer_createSpriteFromSurface,
    .deleteSprite = WiiURenderer_deleteSprite,
    .drawTile = NULL,
    .drawTiled = NULL,
};

Renderer* WiiURenderer_create(void) {
    WiiURenderer_bootLog("wiiu_renderer: create begin");
    WiiURenderer* renderer = safeCalloc(1, sizeof(WiiURenderer));
    renderer->base.vtable = &WiiURendererVtable;
    renderer->base.drawColor = 0xFFFFFF;
    renderer->base.drawAlpha = 1.0f;
    renderer->base.drawFont = -1;
    WiiURenderer_bootLog("wiiu_renderer: create end");
    return (Renderer*) renderer;
}

void WiiURenderer_setClearColor(WiiURenderer* renderer, uint32_t color) {
    renderer->clearR = (uint8_t) BGR_R(color);
    renderer->clearG = (uint8_t) BGR_G(color);
    renderer->clearB = (uint8_t) BGR_B(color);
}

void WiiURenderer_runStartupSmokeTest(uint32_t frameCount) {
    (void) frameCount;
}
