#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/protocols/types/SurfaceRole.hpp>
#include <hyprutils/math/Box.hpp>
#include <hyprutils/math/Vector2D.hpp>
#include <linux/input-event-codes.h>
#define WLR_USE_UNSTABLE

#include <any>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/helpers/MiscFunctions.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>

#include <algorithm>

// --- COLOR AND ROUNDING CONFIGURATION START ---
#define SELECTION_R 220 // 0-255 RGB for #DCD7BA
#define SELECTION_G 215 
#define SELECTION_B 186 
const bool SELECTION_NO_ROUNDING = false; // true = sharp, false = rounded
#define CHyprColorRGB(r, g, b, a) CHyprColor((float)r/255.f, (float)g/255.f, (float)b/255.f, a)
// --- COLOR AND ROUNDING CONFIGURATION END ---

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

void drawRect(CBox box, CHyprColor color, float round, float roundingPower, bool blur, float blurA);
void drawBorder(CBox box, CHyprColor color, float size, float round, float roundingPower, bool blur, float blurA);
void drawDropShadow(PHLMONITOR pMonitor, float const& a, CHyprColor b, float ROUNDINGBASE, float ROUNDINGPOWER, CBox fullBox, int range, float scale, bool sharp); 

// Always make box start from top left point
CBox rect(Vector2D start, Vector2D current) {
    auto x = start.x;
    auto y = start.y;
    auto xn = current.x;
    auto yn = current.y;
    if (x > xn) {
        auto t = xn;
        xn = x;
        x = t;
    }
    if (y > yn) {
        auto t = yn;
        yn = y;
        y = t;
    }
    auto w = xn - x;
    auto h = yn - y;
    return CBox(x, y, w, h);
}

// Rendering function require coordinates to be relative to the xy of the monitor,
// and to be scaled by the monitor scaling factor
CBox fixForRender(PHLMONITOR m, CBox box) {
    box.x -= m->m_position.x;
    box.y -= m->m_position.y;
    box.scale(m->m_scale); // CBox scale is particular and if not used, can cause jittering
    box.round();
    
    return box;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    static void *PHANDLE = handle;

    static bool drawSelection = false;
    static Vector2D mouseAtStart;

    static auto mouseButton = HyprlandAPI::registerCallbackDynamic(PHANDLE, "mouseButton", [](void* self, SCallbackInfo& info, std::any data) {
        auto e = std::any_cast<IPointer::SButtonEvent>(data);
        auto mouse = g_pInputManager->getMouseCoordsInternal();
        if (e.button == BTN_LEFT) {
            if (e.state) { // clicked
                auto intersectedWindow = g_pCompositor->vectorToWindowUnified(mouse, RESERVED_EXTENTS | INPUT_EXTENTS | ALLOW_FLOATING);
                if (!intersectedWindow) {
                    drawSelection = true;
                    mouseAtStart = mouse;
                }
            } else { // released
                drawSelection = false;
                for (auto m : g_pCompositor->m_monitors)
                    g_pHyprRenderer->damageMonitor(m);
            }
        }
    });

    static auto mouseMove = HyprlandAPI::registerCallbackDynamic(PHANDLE, "mouseMove", [](void* self, SCallbackInfo& info, std::any data) {
        if (drawSelection) {
            auto mouse = g_pInputManager->getMouseCoordsInternal();
            auto selectionBox = rect(mouseAtStart, mouse);
            // Raw coords are already correct for damage 'debug:damage_blink = true' to verify
            selectionBox.expand(10);

            static auto previousBox = selectionBox;
            g_pHyprRenderer->damageBox(selectionBox);
            g_pHyprRenderer->damageBox(previousBox);
            previousBox = selectionBox;

            info.cancelled = true; // consume mouse if select box being draw
        }
    });

    static auto render = HyprlandAPI::registerCallbackDynamic(PHANDLE, "render", [](void* self, SCallbackInfo& info, std::any data) {
        auto stage = std::any_cast<eRenderStage>(data);
        if (stage == eRenderStage::RENDER_POST_WALLPAPER) {
            if (drawSelection) {
                auto mouse = g_pInputManager->getMouseCoordsInternal();
                auto selectionBox = rect(mouseAtStart, mouse);

                // rendering requires the raw coords to be relative to the monitor and scaled by the monitor scale
                auto m = g_pHyprOpenGL->m_renderData.pMonitor.lock();
                selectionBox = fixForRender(m, selectionBox);
                
                float rounding = 6.0f * m->m_scale;
                float roundingPower = 2.0f;

                float supressDropShadow = 1.0f;
                float thresholdForShowingDropShadow = 40.0f * m->m_scale;
                
                if (selectionBox.h < thresholdForShowingDropShadow)
                    supressDropShadow = ((float) (selectionBox.h)) / thresholdForShowingDropShadow;
                if (selectionBox.w < thresholdForShowingDropShadow) {
                    auto val = ((float) (selectionBox.w)) / thresholdForShowingDropShadow;
                    if (val < supressDropShadow)
                        supressDropShadow = val;
                }
                
                // Use rounding for shadow unless SELECTION_NO_ROUNDING is true
                float shadowRounding = SELECTION_NO_ROUNDING ? 0.0f : rounding;
                
                drawDropShadow(m, 1.0, {0, 0, 0, 0.15f * supressDropShadow}, shadowRounding, roundingPower, selectionBox, 7 * m->m_scale, 1.0, false);
                
                // Use new color macro for fill. Alpha set to 0.25
                float rectRounding = SELECTION_NO_ROUNDING ? 0.0f : rounding;
                drawRect(selectionBox, CHyprColorRGB(SELECTION_R, SELECTION_G, SELECTION_B, 0.25f), rectRounding, roundingPower, false, 1.0f);
                //drawRect(selectionBox, **PMAINCOL, rounding, roundingPower, false, 1.0f);

                auto borderBox = selectionBox;
                auto borderSize = std::floor(1.1f * m->m_scale);
                if (borderSize < 1.0)
                    borderSize = 1.0;
                // If we don't apply m_scale to rounding here, it'll not match drawRect, even though drawRect shouldn't be applying m_scale, somewhere in the pipeline, it clearly does (annoying inconsistancy)
                borderBox.expand(-borderSize * .7);
                borderBox.round();
                
                // Use new color macro for border. Alpha set to 1.0
                float borderRounding = SELECTION_NO_ROUNDING ? 0.0f : rounding * m->m_scale;

                // drawBorder(borderBox, {0, .47, .84, 1.0}, borderSize, rounding * m->m_scale, 2.0f, false, 1.0f);
                // No border radius use the above code for border radius
                drawBorder(borderBox, CHyprColorRGB(SELECTION_R, SELECTION_G, SELECTION_B, 1.0f), borderSize, borderRounding, 2.0f, false, 1.0f);
                //drawBorder(borderBox, **PBORDERCOL, borderSize, rounding * m->m_scale, 2.0f, false, 1.0f);
            }
        }
    });

    HyprlandAPI::reloadConfig();

    return {"hyprselect", "A plugin that adds a completely useless desktop selection box to Hyprland.", "jmanc3", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_pHyprRenderer->m_renderPass.removeAllOfType("CRectPassElement");
    g_pHyprRenderer->m_renderPass.removeAllOfType("CBorderPassElement");
    g_pHyprRenderer->m_renderPass.removeAllOfType("CAnyPassElement");
}

class AnyPass : public IPassElement {
public:
    struct AnyData {
        std::function<void(AnyPass*)> draw = nullptr;
        CBox box = {};
    };
    AnyData* m_data = nullptr;

    AnyPass(const AnyData& data) {
        m_data        = new AnyData;
        m_data->draw = data.draw;
    }
    virtual ~AnyPass() {
        delete m_data;
    }

    virtual void draw(const CRegion& damage) {
        // here we can draw anything
        if (m_data->draw) {
            m_data->draw(this);
        }
    }
    virtual bool needsLiveBlur() {
        return false;
    }
    virtual bool needsPrecomputeBlur() {
        return false;
    }
    //virtual std::optional<CBox> boundingBox() {
        //return {};
    //}
    
    virtual const char* passName() {
        return "CAnyPassElement";
    }
};

void drawRect(CBox box, CHyprColor color, float round, float roundingPower, bool blur, float blurA) {
    if (box.h <= 0 || box.w <= 0)
        return;
    AnyPass::AnyData anydata([box, color, round, roundingPower, blur, blurA](AnyPass* pass) {
        CHyprOpenGLImpl::SRectRenderData rectdata;
        auto region = new CRegion(box);
        rectdata.damage = region;
        rectdata.blur = blur;
        rectdata.blurA = blurA;
        rectdata.round = round;
        rectdata.roundingPower = roundingPower;
        rectdata.xray = false;
        g_pHyprOpenGL->renderRect(box, CHyprColor(color.r, color.g, color.b, color.a), rectdata);
    });
    anydata.box = box;
    g_pHyprRenderer->m_renderPass.add(makeUnique<AnyPass>(std::move(anydata)));
}

void drawBorder(CBox box, CHyprColor color, float size, float round, float roundingPower, bool blur, float blurA) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    if (box.h <= 0 || box.w <= 0)
        return;
    CBorderPassElement::SBorderData rectdata;
    rectdata.grad1           = CHyprColor(color.r, color.g, color.b, color.a);
    rectdata.grad2           = CHyprColor(color.r, color.g, color.b, color.a);
    rectdata.box             = box;
    rectdata.round           = round;
    rectdata.outerRound      = round;
    rectdata.borderSize      = size;
    rectdata.roundingPower = roundingPower;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(rectdata));
}

void drawShadowInternal(const CBox& box, int round, float roundingPower, int range, CHyprColor color, float a, bool sharp) {
    static auto PSHADOWSHARP = sharp;

    if (box.w < 1 || box.h < 1)
        return;

    g_pHyprOpenGL->blend(true);

    color.a *= a;

    if (PSHADOWSHARP)
        g_pHyprOpenGL->renderRect(box, color, {.round = round, .roundingPower = roundingPower});
    else
        g_pHyprOpenGL->renderRoundedShadow(box, round, roundingPower, range, color, 1.F);
}

void drawDropShadow(PHLMONITOR pMonitor, float const& a, CHyprColor b, float ROUNDINGBASE, float ROUNDINGPOWER, CBox fullBox, int range, float scale, bool sharp) {
    AnyPass::AnyData anydata([pMonitor, a, b, ROUNDINGBASE, ROUNDINGPOWER, fullBox, range, scale, sharp](AnyPass* pass) {
        CHyprColor m_realShadowColor = CHyprColor(b.r, b.g, b.b, b.a);
        if (g_pCompositor->m_windows.empty())
            return;
        PHLWINDOW fake_window = g_pCompositor->m_windows[0]; // there is a faulty assert that exists that would otherwise be hit without a fake window target
        static auto PSHADOWSIZE = range;
        const auto ROUNDING = ROUNDINGBASE;
        auto allBox = fullBox;
        allBox.expand(PSHADOWSIZE * pMonitor->m_scale);
        allBox.round();
        
        if (fullBox.width < 1 || fullBox.height < 1)
            return; // don't draw invisible shadows

        g_pHyprOpenGL->scissor(nullptr);
        auto before_window = g_pHyprOpenGL->m_renderData.currentWindow;
        g_pHyprOpenGL->m_renderData.currentWindow = fake_window;

        // we'll take the liberty of using this as it should not be used rn
        CFramebuffer& alphaFB = g_pHyprOpenGL->m_renderData.pCurrentMonData->mirrorFB;
        CFramebuffer& alphaSwapFB = g_pHyprOpenGL->m_renderData.pCurrentMonData->mirrorSwapFB;
        auto* LASTFB = g_pHyprOpenGL->m_renderData.currentFB;

        CRegion saveDamage = g_pHyprOpenGL->m_renderData.damage;

        g_pHyprOpenGL->m_renderData.damage = allBox;
        g_pHyprOpenGL->m_renderData.damage.subtract(fullBox.copy().expand(-ROUNDING * pMonitor->m_scale)).intersect(saveDamage);
        g_pHyprOpenGL->m_renderData.renderModif.applyToRegion(g_pHyprOpenGL->m_renderData.damage);

        alphaFB.bind();

        // build the matte
        // 10-bit formats have dogshit alpha channels, so we have to use the matte to its fullest.
        // first, clear region of interest with black (fully transparent)
        g_pHyprOpenGL->renderRect(allBox, CHyprColor(0, 0, 0, 1), {.round = 0});

        // render white shadow with the alpha of the shadow color (otherwise we clear with alpha later and shit it to 2 bit)
        drawShadowInternal(allBox, ROUNDING * pMonitor->m_scale, ROUNDINGPOWER, PSHADOWSIZE * pMonitor->m_scale, CHyprColor(1, 1, 1, m_realShadowColor.a), a, sharp);

        // render black window box ("clip")
        int some = (ROUNDING + 1 /* This fixes small pixel gaps. */) * pMonitor->m_scale;
        g_pHyprOpenGL->renderRect(fullBox, CHyprColor(0, 0, 0, 1.0), {.round = some, .roundingPower = ROUNDINGPOWER});

        alphaSwapFB.bind();

        // alpha swap just has the shadow color. It will be the "texture" to render.
        g_pHyprOpenGL->renderRect(allBox, m_realShadowColor.stripA(), {.round = 0});

        LASTFB->bind();

        CBox monbox = {0, 0, pMonitor->m_transformedSize.x, pMonitor->m_transformedSize.y};

        g_pHyprOpenGL->pushMonitorTransformEnabled(true);
        g_pHyprOpenGL->setRenderModifEnabled(false);
        g_pHyprOpenGL->renderTextureMatte(alphaSwapFB.getTexture(), monbox, alphaFB);
        g_pHyprOpenGL->setRenderModifEnabled(true);
        g_pHyprOpenGL->popMonitorTransformEnabled();

        g_pHyprOpenGL->m_renderData.damage = saveDamage;

        g_pHyprOpenGL->m_renderData.currentWindow = before_window;
    });
    g_pHyprRenderer->m_renderPass.add(makeUnique<AnyPass>(std::move(anydata)));
}