#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/protocols/types/SurfaceRole.hpp>
#include <hyprlang.hpp>
#include <hyprutils/math/Box.hpp>
#include <hyprutils/math/Vector2D.hpp>
#include <linux/input-event-codes.h>
#define WLR_USE_UNSTABLE

#include <any>
#include <chrono>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/helpers/MiscFunctions.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/config/values/ConfigValues.hpp>

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

void drawRect(CBox box, CHyprColor color, float round, float roundingPower, bool blur, float blurA);
void drawBorder(CBox box, CHyprColor color, float size, float round, float roundingPower, bool blur, float blurA);
void drawDropShadow(PHLMONITOR pMonitor, float const& a, CHyprColor b, float ROUNDINGBASE, float ROUNDINGPOWER, CBox fullBox, int range, bool sharp);

static void *PHANDLE = nullptr;

static SP<Config::Values::CBoolValue>  g_pShouldRound;
static SP<Config::Values::CColorValue> g_pColMain;
static SP<Config::Values::CColorValue> g_pColBorder;
static SP<Config::Values::CBoolValue>  g_pShouldBlur;
static SP<Config::Values::CFloatValue> g_pBlurPower;
static SP<Config::Values::CFloatValue> g_pBorderSize;
static SP<Config::Values::CFloatValue> g_pRounding;
static SP<Config::Values::CFloatValue> g_pRoundingPower;
static SP<Config::Values::CFloatValue> g_pFadeTime;

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

static long get_current_time_in_ms() {
    using namespace std::chrono;
    milliseconds currentTime = duration_cast<milliseconds>(system_clock::now().time_since_epoch());
    return currentTime.count();
}

void drawSelectionBox(CBox selectionBox, float alpha) {

    // rendering requires the raw coords to be relative to the monitor and scaled by the monitor scale
    auto m = g_pHyprRenderer->m_renderData.pMonitor.lock();

    float rounding = g_pRounding->value() * m->m_scale;
    float roundingPower = g_pRoundingPower->value();
    if (!g_pShouldRound->value()) {
        rounding = 0.0;
        roundingPower = 2.0f;
    }

    float supressDropShadow = 1.0f;
    float thresholdForShowingDropShadow = 40.0f * m->m_scale;
    
    if (selectionBox.h < thresholdForShowingDropShadow)
        supressDropShadow = ((float) (selectionBox.h)) / thresholdForShowingDropShadow;
    if (selectionBox.w < thresholdForShowingDropShadow) {
        auto val = ((float) (selectionBox.w)) / thresholdForShowingDropShadow;
        if (val < supressDropShadow)
            supressDropShadow = val;
    }
    auto bbb = selectionBox;
    bbb.expand(1.0); 
    drawDropShadow(m, 1.0, {0, 0, 0, 0.03f * supressDropShadow * alpha}, rounding, roundingPower, bbb, 7 * m->m_scale, false);

    CHyprColor mainCol = (uint64_t) g_pColMain->value();
    mainCol.a *= alpha;
    drawRect(selectionBox, mainCol, rounding, roundingPower, g_pShouldBlur->value(), sqrt(g_pBlurPower->value() * alpha));

    auto borderBox = selectionBox;
    auto borderSize = std::floor(1.1f * m->m_scale);
    if (borderSize < 1.0)
        borderSize = 1.0;
    // If we don't apply m_scale to rounding here, it'll not match drawRect, even though drawRect shouldn't be applying m_scale, somewhere in the pipeline, it clearly does (annoying inconsistancy)
    if (g_pBorderSize->value() >= 0.0)  {
        borderSize = g_pBorderSize->value();
        borderBox = selectionBox;
    }
    borderBox.expand(-borderSize);
    borderBox.round();
    if (g_pBorderSize->value() != 0.0) {
        CHyprColor borderCol = (uint64_t) g_pColBorder->value();
        borderCol.a *= alpha;
        drawBorder(borderBox, borderCol, borderSize, rounding, roundingPower, false, 1.0f);
    }
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    using namespace Config::Values;

    g_pShouldRound   = makeShared<CBoolValue>("plugin:hyprselect:should_round", "Whether the selection box has rounded corners", (Config::BOOL) false);
    g_pColMain       = makeShared<CColorValue>("plugin:hyprselect:col.main", "Fill color of the selection box", (Config::INTEGER) CHyprColor(0, .52, .9, 0.25).getAsHex());
    g_pColBorder     = makeShared<CColorValue>("plugin:hyprselect:col.border", "Border color of the selection box", (Config::INTEGER) CHyprColor(0, .52, .9, 1.0).getAsHex());
    g_pShouldBlur    = makeShared<CBoolValue>("plugin:hyprselect:should_blur", "Whether the selection box fill is blurred", (Config::BOOL) false);
    g_pBlurPower     = makeShared<CFloatValue>("plugin:hyprselect:blur_power", "Strength of the selection box blur", (Config::FLOAT) 1.0);
    g_pBorderSize    = makeShared<CFloatValue>("plugin:hyprselect:border_size", "Border thickness (-1 = auto, 0 = no border)", (Config::FLOAT) -1.0);
    g_pRounding      = makeShared<CFloatValue>("plugin:hyprselect:rounding", "Corner rounding radius", (Config::FLOAT) 6.0);
    g_pRoundingPower = makeShared<CFloatValue>("plugin:hyprselect:rounding_power", "Corner rounding power (squircle factor)", (Config::FLOAT) 2.0);
    g_pFadeTime      = makeShared<CFloatValue>("plugin:hyprselect:fade_time_ms", "Fade-out duration in milliseconds", (Config::FLOAT) 65.0);

    HyprlandAPI::addConfigValueV2(PHANDLE, g_pShouldRound);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pColMain);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pColBorder);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pShouldBlur);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pBlurPower);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pBorderSize);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pRounding);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pRoundingPower);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pFadeTime);

    static bool drawSelection = false;
    static Vector2D mouseAtStart;

    struct FadingBox {
        long creation_time = 0;
        CBox box;
        bool done = false;
    };

    static std::vector<FadingBox> fadingBoxes;

    static auto mouseButton = Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent e, Event::SCallbackInfo &info) {
        auto mouse = g_pInputManager->getMouseCoordsInternal();
        auto m = g_pCompositor->getMonitorFromCursor();
        
        if (e.button == BTN_LEFT) {
            if (e.state) { 
                bool intersected = false; 
                { // against clients
                    auto intersectedWindow = g_pCompositor->vectorToWindowUnified(mouse, Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);
                    if (intersectedWindow)
                        intersected = true;
                    }
                { // against layers
                    for (auto& lsl : m->m_layerSurfaceLayers | std::views::reverse) {
                        Vector2D surfaceCoords;
                        PHLLS pFoundLayerSurface;
                        auto foundSurface = g_pCompositor->vectorToLayerSurface(mouse, &lsl, &surfaceCoords, &pFoundLayerSurface, false);
                        if (foundSurface) {
                            {
                                // Guesstimate by the size of the layer if it's wallpaper
                                CBox box = foundSurface->extends();
                                if (!(std::abs(box.w - m->m_size.x) < 30 && std::abs(box.h - m->m_size.y) < 30)) {
                                    intersected = true;
                                }
                            }
                            // Ignore wallpaper layers so desktop drag works with hyprpaper.
                            //if (!pFoundLayerSurface || pFoundLayerSurface->m_namespace != "hyprpaper")
                                //intersected = true;
                        }
                    }
                }
                { // against popups
                    Vector2D surfaceCoords;
                    PHLLS pFoundLayerSurface;
                    auto foundSurface =
                        g_pCompositor->vectorToLayerPopupSurface(
                            mouse, m, &surfaceCoords, &pFoundLayerSurface);
                    if (foundSurface)
                        intersected = true;
                }
                        
                if (!intersected) {
                    drawSelection = true;
                    mouseAtStart = mouse;
                }
            } else { // released
                if (drawSelection) {
                    drawSelection = false;
                    FadingBox fbox;
                    fbox.box = rect(mouseAtStart, mouse);
                    fbox.creation_time = get_current_time_in_ms();
                    fadingBoxes.push_back(fbox);
                        
                    for (auto m : g_pCompositor->m_monitors)
                        g_pHyprRenderer->damageMonitor(m);
                }
            }
        }
    });

    static auto mouseMove = Event::bus()->m_events.input.mouse.move.listen([](Vector2D event, Event::SCallbackInfo &info) {
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

    static auto render = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) {
        if (stage == eRenderStage::RENDER_POST_WALLPAPER) {
            if (drawSelection) {
                auto mouse = g_pInputManager->getMouseCoordsInternal();
                auto selectionBox = rect(mouseAtStart, mouse);
                auto m = g_pHyprRenderer->m_renderData.pMonitor.lock();
                drawSelectionBox(fixForRender(m, selectionBox), 1.0);
            }

            if (!fadingBoxes.empty()) {
                for (auto &fbox : fadingBoxes) {
                    float dt = (float) (get_current_time_in_ms() - fbox.creation_time);
                    float scalar = dt / g_pFadeTime->value();
                    if (scalar > 1.0) {
                        fbox.done = true;
                        scalar = 1.0;
                    }
                    auto m = g_pHyprRenderer->m_renderData.pMonitor.lock();
                    drawSelectionBox(fixForRender(m, fbox.box), 1.0 - scalar);
                    
                    auto area = fbox.box;
                    area.expand(10 * m->m_scale);
                    g_pHyprRenderer->damageBox(area);
                }

                for (int i = fadingBoxes.size() - 1; i >= 0; i--) {
                    if (fadingBoxes[i].done) {
                        fadingBoxes.erase(fadingBoxes.begin() + i);
                    }
                }
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
    };
    AnyData* m_data = nullptr;

    AnyPass(const AnyData& data) {
        m_data       = new AnyData;
        m_data->draw = data.draw;
    }
    virtual ~AnyPass() {
        delete m_data;
    }

    std::vector<UP<IPassElement>> draw() {
        if (m_data->draw) {
            m_data->draw(this);
        }
        return {};
    }

    bool needsLiveBlur() {
        return false;
    }
    bool needsPrecomputeBlur() {
        return false;
    }

    ePassElementType type() {
        return EK_CUSTOM;
    };

    const char* passName() {
        return "CAnyPassElement";
    }
};

void drawRect(CBox box, CHyprColor color, float round, float roundingPower, bool blur, float blurA) {
    if (box.h <= 0 || box.w <= 0)
        return;
    AnyPass::AnyData anydata([box, color, blur, blurA, round, roundingPower](AnyPass* pass) {
        Render::GL::CHyprOpenGLImpl::SRectRenderData rectdata;
        rectdata.blur = blur;
        rectdata.blurA = blurA;
        rectdata.round = round;
        rectdata.roundingPower = roundingPower;
        rectdata.xray = false;
        Render::GL::g_pHyprOpenGL->renderRect(box, CHyprColor(color.r, color.g, color.b, color.a), rectdata);
    });
    g_pHyprRenderer->m_renderPass.add(makeUnique<AnyPass>(anydata));
}

void drawBorder(CBox box, CHyprColor color, float size, float round, float roundingPower, bool blur, float blurA) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    if (box.h <= 0 || box.w <= 0)
        return;
    CBorderPassElement::SBorderData rectdata;
    rectdata.grad1         = CHyprColor(color.r, color.g, color.b, color.a);
    rectdata.grad2         = CHyprColor(color.r, color.g, color.b, color.a);
    rectdata.box           = box;
    rectdata.round         = round;
    rectdata.outerRound    = round;
    rectdata.borderSize    = size;
    rectdata.roundingPower = roundingPower;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(rectdata));
}

void drawShadowInternal(const CBox& box, int round, float roundingPower, int range, CHyprColor color, float a, bool sharp) {
    static auto PSHADOWSHARP = sharp;

    if (box.w < 1 || box.h < 1)
        return;

    Render::GL::g_pHyprOpenGL->blend(true);

    color.a *= a;

    if (PSHADOWSHARP)
        Render::GL::g_pHyprOpenGL->renderRect(box, color, {.round = round, .roundingPower = roundingPower});
    else
        Render::GL::g_pHyprOpenGL->renderRoundedShadow(box, round, roundingPower, range, color, 1.F);
}

void drawDropShadow(PHLMONITOR pMonitor, float const& a, CHyprColor b, float ROUNDINGBASE, float ROUNDINGPOWER, CBox fullBox, int range, bool sharp) {
    AnyPass::AnyData anydata([pMonitor, a, b, ROUNDINGBASE, ROUNDINGPOWER, fullBox, range, sharp](AnyPass* pass) {
        CHyprColor m_realShadowColor = CHyprColor(b.r, b.g, b.b, b.a);
        if (g_pCompositor->m_windows.empty())
            return;
        PHLWINDOW fake_window = g_pCompositor->m_windows[0]; // there is a faulty assert that exists that would otherwise be hit without a fake window target
        static auto PSHADOWSIZE = range;
        const auto ROUNDING = ROUNDINGBASE;
        auto allBox = fullBox;
        allBox.expand(PSHADOWSIZE);
        allBox.round();

        if (fullBox.width < 1 || fullBox.height < 1)
            return; // don't draw invisible shadows

        Render::GL::g_pHyprOpenGL->scissor(nullptr);
        auto before_window = g_pHyprRenderer->m_renderData.currentWindow;
        g_pHyprRenderer->m_renderData.currentWindow = fake_window;

        // we'll take the liberty of using this as it should not be used rn
        static auto alphaFB = g_pHyprRenderer->createFB();
        static auto alphaSwapFB = g_pHyprRenderer->createFB();
        alphaFB->alloc(pMonitor->m_pixelSize.x, pMonitor->m_pixelSize.y);
        alphaSwapFB->alloc(pMonitor->m_pixelSize.x, pMonitor->m_pixelSize.y);
        // auto alphaFB = g_pHyprRenderer->m_renderData.pMonitor->mirrorFB();
        // auto alphaSwapFB = g_pHyprRenderer->m_renderData.pMonitor->mirrorSwapFB();
        auto LASTFB = g_pHyprRenderer->m_renderData.currentFB;

        CRegion saveDamage = g_pHyprRenderer->m_renderData.damage;

        g_pHyprRenderer->m_renderData.damage = allBox;
        g_pHyprRenderer->m_renderData.damage.subtract(fullBox.copy().expand(-ROUNDING)).intersect(saveDamage);
        g_pHyprRenderer->m_renderData.renderModif.applyToRegion(g_pHyprRenderer->m_renderData.damage);

        alphaFB->bind();

        // build the matte
        // 10-bit formats have dogshit alpha channels, so we have to use the matte to its fullest.
        // first, clear region of interest with black (fully transparent)
        Render::GL::g_pHyprOpenGL->renderRect(allBox, CHyprColor(0, 0, 0, 1), {.round = 0});

        // render white shadow with the alpha of the shadow color (otherwise we clear with alpha later and shit it to 2 bit)
        // Render::GL::g_pHyprOpenGL->renderRoundedShadow(allBox, ROUNDING, ROUNDINGPOWER, PSHADOWSIZE, CHyprColor(1, 1, 1, m_realShadowColor.a), a);
        drawShadowInternal(allBox, ROUNDING, ROUNDINGPOWER, PSHADOWSIZE, CHyprColor(1, 1, 1, m_realShadowColor.a), a, sharp);

        // render black window box ("clip")
        Render::GL::g_pHyprOpenGL->renderRect(fullBox, CHyprColor(0, 0, 0, 1.0), {.round = (int) (ROUNDING), .roundingPower = ROUNDINGPOWER});

        alphaSwapFB->bind();

        // alpha swap just has the shadow color. It will be the "texture" to render.
        Render::GL::g_pHyprOpenGL->renderRect(allBox, m_realShadowColor.stripA(), {.round = 0});

        LASTFB->bind();

        CBox monbox = {0, 0, pMonitor->m_transformedSize.x, pMonitor->m_transformedSize.y};

        g_pHyprRenderer->pushMonitorTransformEnabled(true);
        g_pHyprRenderer->m_renderData.renderModif.enabled = false;

        Render::GL::g_pHyprOpenGL->renderTextureMatte(alphaSwapFB->getTexture(), monbox, alphaFB);

        g_pHyprRenderer->m_renderData.renderModif.enabled = true;
        g_pHyprRenderer->popMonitorTransformEnabled();

        g_pHyprRenderer->m_renderData.damage = saveDamage;

        g_pHyprRenderer->m_renderData.currentWindow = before_window;
    });
    g_pHyprRenderer->m_renderPass.add(makeUnique<AnyPass>(std::move(anydata)));
}


