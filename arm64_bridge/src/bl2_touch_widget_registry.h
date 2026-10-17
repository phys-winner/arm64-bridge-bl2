#pragma once
#include "bl2_touch_gfx_values.h"
#include "bl2_touch_ui_model.h"
#include <string>
#include <vector>

namespace bl2::touch {
struct WidgetPoint { double x = 0, y = 0; };
enum class WidgetKind { Button, Slider, Spinner, Unsupported };
struct WidgetTarget {
    std::uint64_t id = 0;
    std::int32_t index = -1, eventId = 0;
    WidgetKind kind = WidgetKind::Unsupported;
    bool enabled = false, visible = false;
    std::string caption, rendererType;
    GfxIdentity renderer, data, inner, slider, choices;
    GfxRect stageBounds, bounds, sliderBounds, valueBounds;
    std::vector<std::string> choiceLabels;
    WidgetPoint origin, sliderStart, sliderEnd;
    double minimum = 0, maximum = 0, increment = 0, value = 0;
};
struct WidgetRegistry {
    bool valid = false;
    std::string reason;
    std::uint64_t model = 0, route = 0, content = 0;
    GfxIdentity list, data, root;
    GfxRect frame, listStageBounds, listBounds;
    GfxViewport viewport;
    std::uint32_t surfaceWidth = 0, surfaceHeight = 0;
    std::int32_t scrollPosition = 0, totalRenderers = 0, rowCount = 0, selectedIndex = -1;
    std::vector<WidgetTarget> rows;
};

// Observation only. This invokes getters/getBounds/localToGlobal and creates
// short-lived private AS point objects; it does not allocate UObjects or change
// selection/settings/input. Caller must re-capture the native model afterwards
// and reject changed structure. Captured identities are comparison-only; never
// dereference one or treat the content hash as a lifetime reference.
WidgetRegistry CaptureBl2Widgets(GfxSession&, const UiModelSnapshot&,
                                std::uint32_t surfaceWidth, std::uint32_t surfaceHeight);

bool NormalizeWidgetPoint(const GfxRect& visibleFrame, const GfxViewport&,
                          std::uint32_t surfaceWidth, std::uint32_t surfaceHeight,
                          WidgetPoint stage, WidgetPoint& normalized);
bool WidgetContains(const GfxRect&, WidgetPoint);
// Slider projection uses pixel-space geometry, so 20:9 and landscape/portrait
// surfaces do not change the meaning of a diagonal/rotated slider axis.
bool ProjectWidgetSlider(const WidgetTarget&, WidgetPoint normalized,
                         std::uint32_t surfaceWidth, std::uint32_t surfaceHeight,
                         double& normalizedValue);
} // namespace bl2::touch
