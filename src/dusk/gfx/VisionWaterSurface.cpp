#include "VisionWaterSurface.hpp"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_VISION

#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/J3DGraphBase/J3DShapeMtx.h"
#include "JSystem/J3DGraphBase/J3DSys.h"
#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "dusk/gfx/VisionStereoRenderer.hpp"
#include "dusk/logging.h"

#include <algorithm>
#include <cstring>

namespace dusk::gfx {
namespace {

thread_local bool sSurfaceStateTouched = false;

struct Surface {
    u16 texture;
    GXTexGenSrc source;
    J3DTexMtx* matrix;
};

bool FindSurface(J3DMatPacket* packet, Surface& surface) {
    auto* material = packet->getMaterial();
    auto* shapePacket = packet->getShapePacket();
    if (!material || !shapePacket || !shapePacket->getModel() || !packet->mpTexture) {
        return false;
    }
    // J3DTevBlock1 physically has only one texture slot, even in release builds.
    if (!material->getTevBlock() || material->getTevBlock()->getType() == 'TVB1' ||
        material->getTexGenNum() < 2) {
        return false;
    }
    auto* data = shapePacket->getModel()->getModelData();
    if (!data) {
        return false;
    }
    auto* names = data->getMaterialName();
    auto* textures = data->getTextureName();
    if (!names || !textures || !data->getTexture() || material->getIndex() >= data->getMaterialNum()) {
        return false;
    }
    const char* name = names->getName(material->getIndex());
    if (!name || std::strlen(name) < 7 || std::memcmp(name + 3, "MA02", 4) != 0) {
        return false;
    }

    // This verified pair covers Lake Hylia BG_OBJ and Castle Sewer GRDWATER.
    // Do not infer support from a water-like name or a framebuffer alias alone.
    const u16 framebuffer = material->getTexNo(0);
    const u16 ripple = material->getTexNo(1);
    if (framebuffer >= packet->mpTexture->getNum() || ripple >= packet->mpTexture->getNum() ||
        framebuffer >= data->getTexture()->getNum() || ripple >= data->getTexture()->getNum()) {
        return false;
    }
    const char* framebufferName = textures->getName(framebuffer);
    const char* rippleName = textures->getName(ripple);
    if (!framebufferName || !rippleName || std::strcmp(framebufferName, "fbtex_dummy") != 0 ||
        std::strcmp(rippleName, "M_WaterIndirect_Fix") != 0) {
        return false;
    }
    auto* coord = material->getTexCoord(1);
    auto* matrix = material->getTexMtx(1);
    if (!coord || !matrix || (matrix->getTexMtxInfo().mInfo & 0x3f) != 0 ||
        coord->getTexGenSrc() < GX_TG_TEX0 || coord->getTexGenSrc() > GX_TG_TEX7) {
        return false;
    }
    surface = {ripple, static_cast<GXTexGenSrc>(coord->getTexGenSrc()), matrix};
    return true;
}

bool SupportsSurfaceCoordinates(J3DShape* shape, GXTexGenSrc source) {
    if (shape->getTexMtxLoadType() != 0) {
        return false;
    }
    bool hasSource = false;
    const int uvIndex = static_cast<int>(source) - static_cast<int>(GX_TG_TEX0);
    const auto attribute = static_cast<GXAttr>(static_cast<int>(GX_VA_TEX0) + uvIndex);
    for (const auto* desc = shape->getVtxDesc(); desc && desc->attr != GX_VA_NULL; ++desc) {
        if (desc->attr == GX_VA_TEX0MTXIDX && desc->type != GX_NONE) {
            return false;
        }
        hasSource |= desc->attr == attribute && desc->type != GX_NONE;
    }
    return hasSource;
}

void DrawSurfacePacket(J3DMatPacket* packet) {
    Surface surface{};
    if (!FindSurface(packet, surface)) {
        return;
    }
    sSurfaceStateTouched = true;

    // One texture lookup, no framebuffer feedback, no image uploads. The native
    // indirect map's red channel modulates opacity, not the underwater image.
    const auto& ambient = g_env_light.bg_amb_col[1];
    const float shine = std::clamp(g_env_light.mWaterSurfaceShineRate, 0.0f, 1.0f);
    const auto tint = [](int value) { return static_cast<u8>(std::clamp(value + 28, 24, 220)); };
    const GXColor color = {tint(ambient.r), tint(ambient.g), tint(ambient.b),
                          static_cast<u8>(24.0f + shine * 20.0f)};
    GXSetNumChans(0);
    GXSetNumTexGens(1);
    GXSetNumIndStages(0);
    GXSetNumTevStages(1);
    GXSetTevDirect(GX_TEVSTAGE0);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    GXSetTevKColor(GX_KCOLOR0, color);
    GXSetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
    GXSetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_K0_A);
    GXSetTevSwapModeTable(GX_TEV_SWAP3, GX_CH_RED, GX_CH_RED, GX_CH_RED, GX_CH_RED);
    GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP3);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_KONST);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_KONST, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_COPY);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
    GXSetZCompLoc(GX_TRUE);
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_FALSE);
    GXSetCullMode(GX_CULL_NONE);
    GXSetCoPlanar(GX_FALSE);
    GXSetFogRangeAdj(GX_FALSE, 0, nullptr);
    if (auto* fog = packet->getMaterial()->getFog()) {
        GXSetFog(static_cast<GXFogType>(fog->mType), fog->mStartZ, fog->mEndZ,
                 fog->mNearZ, fog->mFarZ, fog->mColor);
    } else {
        GXSetFog(GX_FOG_NONE, 0.0f, 1.0f, 0.0f, 1.0f, GXColor{0, 0, 0, 0});
    }
    packet->mpTexture->loadGX(surface.texture, GX_TEXMAP0);

    for (auto* shapePacket = packet->getShapePacket(); shapePacket;
         shapePacket = static_cast<J3DShapePacket*>(shapePacket->getNextPacket())) {
        auto* shape = shapePacket->getShape();
        if (!shape || shapePacket->checkFlag(J3DShpFlag_Hidden) ||
            !SupportsSurfaceCoordinates(shape, surface.source)) {
            continue;
        }
        shapePacket->prepareDraw();
        J3DDifferedTexMtx::sTexGenBlock = nullptr;
        J3DDifferedTexMtx::sTexMtxObj = nullptr;
        shape->loadPreDrawSetting();
        // Pre-draw already loaded the position selector. Avoid drawFast reloading
        // the original texture selector when the preceding shape was enveloped.
        J3DShape::sEnvelopeFlag = false;
        GXLoadTexMtxImm(surface.matrix->getMtx(), GX_TEXMTX9, GX_MTX2x4);
        GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, surface.source,
                         GX_TEXMTX9, GX_FALSE, GX_PTIDENTITY);
        shape->drawFast();
        static bool logged = false;
        if (!logged) {
            logged = true;
            DuskLog.info("[DuskStereo] Native-UV water surface active (no framebuffer capture)");
        }
    }
    J3DShape::resetVcdVatCache();
}

} // namespace

void DrawVisionWaterSurfaces(bool opaqueList) {
    if (!IsVisionStereoDrawing()) {
        return;
    }
    struct OverrideGuard {
        J3DMatPacket::DrawOverride previous = J3DMatPacket::sDrawOverride;
        bool previousTouched = sSurfaceStateTouched;
        OverrideGuard() {
            J3DMatPacket::sDrawOverride = DrawSurfacePacket;
            sSurfaceStateTouched = false;
        }
        ~OverrideGuard() {
            J3DMatPacket::sDrawOverride = previous;
            if (sSurfaceStateTouched) {
                J3DDifferedTexMtx::sTexGenBlock = nullptr;
                J3DDifferedTexMtx::sTexMtxObj = nullptr;
                J3DShape::resetVcdVatCache();
                j3dSys.reinitGX();
            }
            sSurfaceStateTouched = previousTouched;
        }
    } guard;
    if (opaqueList) {
        g_dComIfG_gameInfo.drawlist.drawOpaListInvisible();
    } else {
        g_dComIfG_gameInfo.drawlist.drawXluListInvisible();
    }
}

} // namespace dusk::gfx

#else

namespace dusk::gfx {
void DrawVisionWaterSurfaces(bool) {}
}

#endif
