/*
# ______       ____   ___
#   |     \/   ____| |___|    
#   |     |   |   \  |   |       
#-----------------------------------------------------------------------
# Copyright 2020, tyra - https://github.com/h4570/tyra
# Licenced under Apache License 2.0
# Sandro Sobczyński <sandro.sobczynski@gmail.com>
*/

#include "../include/modules/renderer.hpp"

#include <dma.h>
#include <graph.h>
#include <packet.h>
#include <draw.h>
#include <gs_psm.h>
#include "../include/utils/debug.hpp"
#include "../include/utils/math.hpp"

// ----
// Constructors/Destructors
// ----

/** Initialize DMA<->GIF channel
 * Allocate buffers
 * Initialize screen
 * Initialize drawing environment
 * Load/setup textures
 * @param screenW Half of screen width
 * @param screenH Half of screen height
 */
Renderer::Renderer(u32 t_packetSize, ScreenSettings *t_screen)
{
    PRINT_LOG("Initializing renderer");
    dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0); // Initialize DMA to enable data transfer
    dma_channel_fast_waits(DMA_CHANNEL_GIF);
    context = 0;
    isTextureVRAMAllocated = false;
    isVSyncEnabled = true;
    isFrameEmpty = false;
    lastTextureId = 0;
    flipPacket = packet2_create(4, P2_TYPE_UNCACHED_ACCL, P2_MODE_NORMAL, 0);
    allocateBuffers(t_screen->width, t_screen->height);
    initDrawingEnv(t_screen->width, t_screen->height);
    setPrim();
    screen = t_screen;
    gifSender = new GifSender(t_packetSize, t_screen);
    vifSender = new VifSender();
    perspective.setPerspective(*t_screen);
    renderData.perspective = &perspective;
    PRINT_LOG("Renderer initialized!");
}

Renderer::~Renderer() {}

// ----
// Methods
// ----

/** Configure and allocate vRAM for texture buffer */
void Renderer::allocateTextureBuffer(u16 t_width, u16 t_height)
{
    textureBuffer.width = t_width;
    textureBuffer.psm = GS_PSM_24;
    textureBuffer.address = graph_vram_allocate(t_width, t_height, GS_PSM_24, GRAPH_ALIGN_BLOCK);
    if (textureBuffer.address <= 1)
        PRINT_ERR("Texture buffer allocation error. No memory!");
    textureBuffer.info.width = draw_log2(t_width);
    textureBuffer.info.height = draw_log2(t_height);
    textureBuffer.info.components = TEXTURE_COMPONENTS_RGB;
    textureBuffer.info.function = TEXTURE_FUNCTION_MODULATE;
    isTextureVRAMAllocated = true;
}

/** Configure and allocate vRAM for texture buffer */
void Renderer::deallocateTextureBuffer()
{
    if (isTextureVRAMAllocated)
    {
        graph_vram_free(textureBuffer.address);
        isTextureVRAMAllocated = false;
    }
}

void Renderer::changeTexture(const Mesh &t_mesh, u32 t_materialId)
{

    MeshTexture *tex = textureRepo.getByMesh(t_mesh.getId(), t_materialId);
    if (tex != NULL)
    {
        if (tex->getId() != lastTextureId)
        {
            lastTextureId = tex->getId();
            deallocateTextureBuffer();
            allocateTextureBuffer(tex->getWidth(), tex->getHeight());
            GifSender::sendTexture(*tex, &textureBuffer);
        }
    }
    else
        PRINT_ERR("Texture was not found in texture repository!");
}

void Renderer::drawRectangle()
{
    beginFrameIfNeeded();
    packet2_t *packet2 = packet2_create(20, P2_TYPE_NORMAL, P2_MODE_NORMAL, 0);
    packet2_update(packet2, draw_primitive_xyoffset(packet2->next, 0, 2048, 2048));

    int loop0 = 10;

    packet2_add_s64(packet2, GIF_SET_TAG(4, 0, 0, 0, GIF_FLG_PACKED, 1));
    packet2_add_s64(packet2, GIF_REG_AD);

    packet2_add_s64(packet2, GIF_SET_PRIM(6, 0, 0, 0, 0, 0, 0, 0, 0));
    packet2_add_s64(packet2, GIF_REG_PRIM);

    packet2_add_s64(packet2, GIF_SET_RGBAQ((loop0 * 10), 0, 255 - (loop0 * 10), 0x80, 0x3F800000));
    packet2_add_s64(packet2, GIF_REG_RGBAQ);

    packet2_add_s64(packet2, GIF_SET_XYZ(((loop0 * 20) << 4) + (2048 << 4), ((loop0 * 10) << 4) + (2048 << 4), -128));
    packet2_add_s64(packet2, GIF_REG_XYZ2);

    packet2_add_s64(packet2, GIF_SET_XYZ((((loop0 * 20) + 100) << 4) + (2048 << 4), (((loop0 * 10) + 100) << 4) + (2048 << 4), -128));
    packet2_add_s64(packet2, GIF_REG_XYZ2);

    packet2_update(packet2,
                   draw_primitive_xyoffset(
                       packet2->next,
                       0,
                       (2048 - (screen->width / 2)), (2048 - (screen->height / 2))));
    packet2_update(packet2, draw_finish(packet2->next));
    dma_channel_wait(DMA_CHANNEL_GIF, 0);
    dma_channel_send_packet2(packet2, DMA_CHANNEL_GIF, true);
    packet2_free(packet2);
}

/** Initializes drawing environment (1st app packet) */
void Renderer::initDrawingEnv(float t_screenW, float t_screenH)
{
    PRINT_LOG("Initializing drawing environment");
    u16 halfW = (u16)t_screenW / 2;
    u16 halfH = (u16)t_screenH / 2;
    packet2_t *packet2 = packet2_create(20, P2_TYPE_NORMAL, P2_MODE_NORMAL, 0);
    packet2_update(packet2, draw_setup_environment(packet2->base, 0, frameBuffers, &(zBuffer)));
    packet2_update(packet2, draw_primitive_xyoffset(packet2->next, 0, (2048 - halfW), (2048 - halfH)));
    packet2_update(packet2, draw_finish(packet2->next));
    dma_channel_send_packet2(packet2, DMA_CHANNEL_GIF, true);
    dma_channel_wait(DMA_CHANNEL_GIF, 0);
    packet2_free(packet2);
    PRINT_LOG("Drawing environment initialized!");
}

/** Sets drawing prim for all 3D objects */
void Renderer::setPrim()
{
    prim.type = PRIM_TRIANGLE;
    prim.shading = PRIM_SHADE_FLAT;
    prim.mapping = DRAW_ENABLE;
    prim.fogging = DRAW_DISABLE;
    prim.blending = DRAW_ENABLE;
    prim.antialiasing = DRAW_DISABLE;
    prim.mapping_type = PRIM_MAP_ST;
    prim.colorfix = PRIM_UNFIXED;
    renderData.prim = &prim;
    PRINT_LOG("Prim set!");
}

/** Defines and allocates framebuffers and zbuffer */
void Renderer::allocateBuffers(float t_screenW, float t_screenH)
{
    frameBuffers[0].width = (u16)t_screenW;
    frameBuffers[0].height = (u16)t_screenH;
    frameBuffers[0].mask = 0;
    frameBuffers[0].psm = GS_PSM_32;
    frameBuffers[0].address = graph_vram_allocate((u16)t_screenW, (u16)t_screenH, frameBuffers[0].psm, GRAPH_ALIGN_PAGE);

    frameBuffers[1].width = (u16)t_screenW;
    frameBuffers[1].height = (u16)t_screenH;
    frameBuffers[1].mask = 0;
    frameBuffers[1].psm = GS_PSM_32;
    frameBuffers[1].address = graph_vram_allocate((u16)t_screenW, (u16)t_screenH, frameBuffers[1].psm, GRAPH_ALIGN_PAGE);

    zBuffer.enable = DRAW_ENABLE;
    zBuffer.mask = 0;
    zBuffer.method = ZTEST_METHOD_GREATER_EQUAL;
    zBuffer.zsm = GS_ZBUF_32;
    zBuffer.address = graph_vram_allocate((u16)t_screenW, (u16)t_screenH, zBuffer.zsm, GRAPH_ALIGN_PAGE);
    PRINT_LOG("Framebuffers, zBuffer set and allocated!");

    // Initialize the screen and tie the first framebuffer to the read circuits.
    graph_initialize(frameBuffers[1].address, frameBuffers[1].width, frameBuffers[1].height, frameBuffers[1].psm, 0, 0);
}

/// --- Draw: PATH3

void Renderer::drawByPath3(Mesh *t_meshes, u16 t_amount, LightBulb *t_bulbs, u16 t_bulbsCount)
{
    for (u16 i = 0; i < t_amount; i++)
        drawByPath3(t_meshes[i], t_bulbs, t_bulbsCount);
}

void Renderer::drawByPath3(Mesh &t_mesh, LightBulb *t_bulbs, u16 t_bulbsCount)
{
    beginFrameIfNeeded();
    if (!t_mesh.isDataLoaded())
        PRINT_ERR("Can't draw, because no mesh data was loaded!");
    if (t_mesh.getCurrentAnimationFrame() != t_mesh.getNextAnimationFrame())
        t_mesh.animate();
    for (u32 i = 0; i < t_mesh.getMaterialsCount(); i++)
    {
        if (t_mesh.shouldBeFrustumCulled && !t_mesh.getMaterial(i).isInFrustum(renderData.frustumPlanes, t_mesh.position))
            return;
        changeTexture(t_mesh, t_mesh.getMaterial(i).getId());
        gifSender->initPacket(context);
        u32 vertCount = t_mesh.getMaterial(i).getFacesCount();
        VECTOR *vertices = new VECTOR[vertCount];
        VECTOR *normals = new VECTOR[vertCount];
        VECTOR *coordinates = new VECTOR[vertCount];
        vertCount = t_mesh.getDrawData(i, vertices, normals, coordinates, *renderData.cameraPosition);
        gifSender->addObject(&renderData, t_mesh, vertCount, vertices, normals, coordinates, t_bulbs, t_bulbsCount, &textureBuffer);
        gifSender->sendPacket();
        delete[] vertices;
        delete[] normals;
        delete[] coordinates;
    }
}

void Renderer::drawByPath3(Mesh *t_meshes, u16 t_amount) { drawByPath3(t_meshes, t_amount, NULL, 0); }

void Renderer::drawByPath3(Mesh &t_mesh) { drawByPath3(t_mesh, NULL, 0); }

/// --- Draw: PATH1

void Renderer::draw(Mesh *t_meshes, u16 t_amount, LightBulb *t_bulbs, u16 t_bulbsCount)
{
    for (u16 i = 0; i < t_amount; i++)
        draw(t_meshes[i], t_bulbs, t_bulbsCount);
}

void Renderer::draw(Mesh &t_mesh, LightBulb *t_bulbs, u16 t_bulbsCount)
{
    beginFrameIfNeeded();
    vifSender->sendMatrices(renderData, t_mesh.position, t_mesh.rotation);
    if (!t_mesh.isDataLoaded())
        PRINT_ERR("Can't draw, because no mesh data was loaded!");

    Vector3 rotatedCamera = Vector3(*renderData.cameraPosition);
    rotatedCamera.rotate(t_mesh.rotation, true);

    if (t_mesh.getCurrentAnimationFrame() != t_mesh.getNextAnimationFrame())
        t_mesh.animate();
    for (u32 i = 0; i < t_mesh.getMaterialsCount(); i++)
    {
        if (t_mesh.shouldBeFrustumCulled && !t_mesh.getMaterial(i).isInFrustum(renderData.frustumPlanes, t_mesh.position))
            return;
        u32 vertCount = t_mesh.getMaterial(i).getFacesCount();
        VECTOR __attribute__((aligned(16))) vertices[vertCount];
        VECTOR __attribute__((aligned(16))) normals[vertCount];
        VECTOR __attribute__((aligned(16))) coordinates[vertCount];
        changeTexture(t_mesh, t_mesh.getMaterial(i).getId());
        vertCount = t_mesh.getDrawData(i, vertices, normals, coordinates, rotatedCamera);
        vifSender->drawMesh(&renderData, perspective, vertCount, vertices, normals, coordinates, t_mesh, t_bulbs, t_bulbsCount, &textureBuffer);
    }
}

void Renderer::draw(Mesh *t_meshes, u16 t_amount) { draw(t_meshes, t_amount, NULL, 0); }

void Renderer::draw(Mesh &t_mesh) { draw(t_mesh, NULL, 0); }

/// ---

void Renderer::setCameraDefinitions(Matrix *t_worldView, Vector3 *t_cameraPos, Plane *t_planes)
{
    renderData.worldView = t_worldView;
    renderData.cameraPosition = t_cameraPos;
    renderData.frustumPlanes = t_planes;
}

void Renderer::beginFrameIfNeeded()
{
    if (isFrameEmpty)
    {
        isFrameEmpty = false;
        gifSender->sendClear(&zBuffer);
    }
}

void Renderer::endFrame(float fps)
{
    if (!isFrameEmpty)
    {
        if (fps > 49.0F && isVSyncEnabled)
            graph_wait_vsync();
        flipBuffers();
    }
}

/** We need to flip buffers outside of the chain, for some reason,
 * so we use a separate small packet
 * Do not use this method. This is called via packetManager
 */
void Renderer::flipBuffers()
{
    graph_set_framebuffer_filtered(
        frameBuffers[context].address,
        frameBuffers[context].width,
        frameBuffers[context].psm,
        0,
        0);
    context ^= 1;
    isFrameEmpty = 1;
    packet2_update(flipPacket, draw_framebuffer(flipPacket->base, 0, &frameBuffers[context]));
    packet2_update(flipPacket, draw_finish(flipPacket->next));
    dma_channel_send_packet2(flipPacket, DMA_CHANNEL_GIF, true);
    draw_wait_finish();
}
