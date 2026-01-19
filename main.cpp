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

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

void drawRect(CBox box, CHyprColor color, float round, float roundingPower, bool blur, float blurA);
void drawBorder(CBox box, CHyprColor color, float size, float round, float roundingPower, bool blur, float blurA);
void drawDropShadow(PHLMONITOR pMonitor, float const& a, CHyprColor b, float ROUNDINGBASE, float ROUNDINGPOWER, CBox fullBox, int range, bool sharp); 

static void *PHANDLE = nullptr;

class HyprlangUnspecifiedCustomType {};

// abandon hope all ye who enter here
template <typename T, typename V = HyprlangUnspecifiedCustomType>
class ConfigValue {
public:
	ConfigValue(const std::string& option) {
		this->static_data_ptr = HyprlandAPI::getConfigValue(PHANDLE, option)->getDataStaticPtr();
	}

	template <typename U = T>
	typename std::enable_if<std::is_same<U, Hyprlang::CUSTOMTYPE>::value, const V&>::type
	operator*() const {
		return *(V*) ((Hyprlang::CUSTOMTYPE*) *this->static_data_ptr)->getData();
	}

	template <typename U = T>
	typename std::enable_if<std::is_same<U, Hyprlang::CUSTOMTYPE>::value, const V*>::type
	operator->() const {
		return &**this;
	}

	// Bullshit microptimization case for strings
	template <typename U = T>
	typename std::enable_if<std::is_same<U, Hyprlang::STRING>::value, const char*>::type
	operator*() const {
		return *(const char**) this->static_data_ptr;
	}

	template <typename U = T>
	typename std::enable_if<
	    !std::is_same<U, Hyprlang::CUSTOMTYPE>::value && !std::is_same<U, Hyprlang::STRING>::value,
	    const T&>::type
	operator*() const {
		return *(T*) *this->static_data_ptr;
	}

private:
	void* const* static_data_ptr;
};

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
	static const auto c_should_round = ConfigValue<Hyprlang::INT>("plugin:hyprselect:should_round");
	static const auto c_col_main = ConfigValue<Hyprlang::INT>("plugin:hyprselect:col.main");
	static const auto c_col_border = ConfigValue<Hyprlang::INT>("plugin:hyprselect:col.border");
	static const auto c_should_blur = ConfigValue<Hyprlang::INT>("plugin:hyprselect:should_blur");
	static const auto c_blur_power = ConfigValue<Hyprlang::FLOAT>("plugin:hyprselect:blur_power");
	static const auto c_rounding = ConfigValue<Hyprlang::FLOAT>("plugin:hyprselect:rounding");
	static const auto c_rounding_power = ConfigValue<Hyprlang::FLOAT>("plugin:hyprselect:rounding_power");
	static const auto c_border_size = ConfigValue<Hyprlang::FLOAT>("plugin:hyprselect:border_size");

    // rendering requires the raw coords to be relative to the monitor and scaled by the monitor scale
    auto m = g_pHyprOpenGL->m_renderData.pMonitor.lock();

    float rounding = *c_rounding * m->m_scale;
    float roundingPower = *c_rounding_power;
    if (!*c_should_round) {
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
    drawDropShadow(m, 1.0, {0, 0, 0, 0.12f * supressDropShadow * alpha}, rounding, roundingPower, bbb, 7 * m->m_scale, false);

    CHyprColor mainCol = *c_col_main;
    mainCol.a *= alpha;
    drawRect(selectionBox, mainCol, rounding, roundingPower, *c_should_blur, sqrt(*c_blur_power * alpha));

    auto borderBox = selectionBox;
    auto borderSize = std::floor(1.1f * m->m_scale);
    if (borderSize < 1.0)
        borderSize = 1.0;
    // If we don't apply m_scale to rounding here, it'll not match drawRect, even though drawRect shouldn't be applying m_scale, somewhere in the pipeline, it clearly does (annoying inconsistancy)
    if (*c_border_size >= 0.0)  { 
        borderSize = *c_border_size;
        borderBox = selectionBox;
    }
    borderBox.expand(-borderSize);
    borderBox.round();
    if (*c_border_size != 0.0) {
        CHyprColor borderCol = *c_col_border;
        borderCol.a *= alpha;
        drawBorder(borderBox, borderCol, borderSize, rounding, roundingPower, false, 1.0f);
    }
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprselect:should_round", Hyprlang::CConfigValue((Hyprlang::INT) 0));
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprselect:col.main", Hyprlang::CConfigValue((Hyprlang::INT) CHyprColor(0, .52, .9, 0.25).getAsHex()));
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprselect:col.border", Hyprlang::CConfigValue((Hyprlang::INT) CHyprColor(0, .52, .9, 1.0).getAsHex()));
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprselect:should_blur", Hyprlang::CConfigValue((Hyprlang::INT) 0.0));
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprselect:blur_power", Hyprlang::CConfigValue((Hyprlang::FLOAT) 1.0));
    
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprselect:border_size", Hyprlang::CConfigValue((Hyprlang::FLOAT) -1.0));
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprselect:rounding", Hyprlang::CConfigValue((Hyprlang::FLOAT) 6.0));
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprselect:rounding_power", Hyprlang::CConfigValue((Hyprlang::FLOAT) 2.0));
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprselect:fade_time_ms", Hyprlang::CConfigValue((Hyprlang::FLOAT) 65.0));

    static bool drawSelection = false;
    static Vector2D mouseAtStart;

    struct FadingBox {
        long creation_time = 0;
        CBox box;
        bool done = false;
    };

    static std::vector<FadingBox> fadingBoxes;

    static auto mouseButton = HyprlandAPI::registerCallbackDynamic(PHANDLE, "mouseButton", [](void* self, SCallbackInfo& info, std::any data) {
        auto e = std::any_cast<IPointer::SButtonEvent>(data);
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
                auto m = g_pHyprOpenGL->m_renderData.pMonitor.lock();
                drawSelectionBox(fixForRender(m, selectionBox), 1.0);
            }

            if (!fadingBoxes.empty()) {
            	static const auto c_fade_length = ConfigValue<Hyprlang::FLOAT>("plugin:hyprselect:fade_time_ms");
                
                for (auto &fbox : fadingBoxes) {
                    float dt = (float) (get_current_time_in_ms() - fbox.creation_time);  
                    float scalar = dt / *c_fade_length;
                    if (scalar > 1.0) {
                        fbox.done = true;
                        scalar = 1.0;
                    }
                    auto m = g_pHyprOpenGL->m_renderData.pMonitor.lock();
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
        CBox box = {};
    };
    AnyData* m_data = nullptr;

    AnyPass(const AnyData& data) {
        m_data       = new AnyData;
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

    g_pHyprOpenGL->blend(true);

    color.a *= a;

    if (PSHADOWSHARP)
        g_pHyprOpenGL->renderRect(box, color, {.round = round, .roundingPower = roundingPower});
    else
        g_pHyprOpenGL->renderRoundedShadow(box, round, roundingPower, range, color, 1.F);
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

        g_pHyprOpenGL->scissor(nullptr);
        auto before_window = g_pHyprOpenGL->m_renderData.currentWindow;
        g_pHyprOpenGL->m_renderData.currentWindow = fake_window;

        // we'll take the liberty of using this as it should not be used rn
        CFramebuffer& alphaFB = g_pHyprOpenGL->m_renderData.pCurrentMonData->mirrorFB;
        CFramebuffer& alphaSwapFB = g_pHyprOpenGL->m_renderData.pCurrentMonData->mirrorSwapFB;
        auto* LASTFB = g_pHyprOpenGL->m_renderData.currentFB;

        CRegion saveDamage = g_pHyprOpenGL->m_renderData.damage;

        g_pHyprOpenGL->m_renderData.damage = allBox;
        g_pHyprOpenGL->m_renderData.damage.subtract(fullBox.copy().expand(-ROUNDING)).intersect(saveDamage);
        g_pHyprOpenGL->m_renderData.renderModif.applyToRegion(g_pHyprOpenGL->m_renderData.damage);

        alphaFB.bind();

        // build the matte
        // 10-bit formats have dogshit alpha channels, so we have to use the matte to its fullest.
        // first, clear region of interest with black (fully transparent)
        g_pHyprOpenGL->renderRect(allBox, CHyprColor(0, 0, 0, 1), {.round = 0});

        // render white shadow with the alpha of the shadow color (otherwise we clear with alpha later and shit it to 2 bit)
        drawShadowInternal(allBox, ROUNDING, ROUNDINGPOWER, PSHADOWSIZE, CHyprColor(1, 1, 1, m_realShadowColor.a), a, sharp);

        // render black window box ("clip")
        int some = (ROUNDING + 1 /* This fixes small pixel gaps. */);
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


