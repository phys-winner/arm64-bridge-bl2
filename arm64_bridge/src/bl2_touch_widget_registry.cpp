#include "bl2_touch_widget_registry.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace bl2::touch {
namespace {
constexpr std::uint64_t kHashStart = 1469598103934665603ull;
void hash(std::uint64_t& h, std::uint64_t n) {
    for (unsigned i = 0; i < 8; ++i) { h ^= n & 255; h *= 1099511628211ull; n >>= 8; }
}
void hash(std::uint64_t& h, const GfxIdentity& v) { hash(h,v.interface); hash(h,v.data); hash(h,v.extra); hash(h,v.type); }
void hash(std::uint64_t& h, const std::string& v) {
    hash(h,v.size()); for (unsigned char c : v) { h ^= c; h *= 1099511628211ull; }
}
void hashDouble(std::uint64_t& h, double n) { std::uint64_t bits = 0; std::memcpy(&bits,&n,8); hash(h,bits); }
void hash(std::uint64_t& h, const GfxRect& r) { hashDouble(h,r.left); hashDouble(h,r.top); hashDouble(h,r.right); hashDouble(h,r.bottom); }
bool validRect(const GfxRect& r) {
    return std::isfinite(r.left) && std::isfinite(r.right) && std::isfinite(r.top) &&
        std::isfinite(r.bottom) && r.left < r.right && r.top < r.bottom;
}
GfxRect intersect(GfxRect a, const GfxRect& b) {
    return {std::max(a.left,b.left),std::max(a.top,b.top),std::min(a.right,b.right),std::min(a.bottom,b.bottom)};
}
class Value {
public:
    Value(GfxSession& session, GfxHandle h) : session_(&session), handle_(h) {}
    ~Value() { if (session_) session_->release(handle_); }
    Value(const Value&) = delete; Value& operator=(const Value&) = delete;
    Value(Value&& other) noexcept : session_(other.session_), handle_(other.handle_) { other.session_ = nullptr; }
    operator GfxHandle() const { return handle_; }
    explicit operator bool() const { return bool(handle_); }
private:
    GfxSession* session_; GfxHandle handle_;
};
struct Reader {
    GfxSession& s;
    WidgetRegistry out;
    bool stop(const std::string& reason) { out.reason = reason; return false; }
    Value member(GfxHandle v, const char* n) { return {s,s.member(v,n)}; }
    bool number(GfxHandle v, const char* n, double& value) {
        auto x = member(v,n); return x && s.number(x,value);
    }
    bool integer(GfxHandle v, const char* n, std::int32_t& value, std::int32_t min, std::int32_t max) {
        double result = 0;
        if (!number(v,n,result) || result < min || result > max || std::floor(result) != result) return false;
        value = static_cast<std::int32_t>(result); return true;
    }
    bool boolean(GfxHandle v, const char* n, bool& value) {
        auto x = member(v,n); return x && s.boolean(x,value,std::strcmp(n,"disabled")==0);
    }
    bool string(GfxHandle v, const char* n, std::string& value) {
        auto x = member(v,n); return x && s.string(x,value);
    }
    bool same(GfxHandle a, const GfxIdentity& b) {
        GfxIdentity id; return s.identity(a,id) && id == b;
    }
    bool bounds(GfxHandle v, GfxHandle root, GfxRect& r) {
        Value b(s,s.invoke(v,"getBounds",{root}));
        return b && s.object(b) && number(b,"xMin",r.left) && number(b,"yMin",r.top) &&
            number(b,"xMax",r.right) && number(b,"yMax",r.bottom) && validRect(r);
    }
    bool normalized(const GfxRect& r, GfxRect& n) {
        WidgetPoint a,b;
        if (!NormalizeWidgetPoint(out.frame,out.viewport,out.surfaceWidth,out.surfaceHeight,{r.left,r.top},a) ||
            !NormalizeWidgetPoint(out.frame,out.viewport,out.surfaceWidth,out.surfaceHeight,{r.right,r.bottom},b)) return false;
        n = {a.x,a.y,b.x,b.y}; return validRect(n);
    }
    bool visible(GfxHandle v) {
        bool shown = false; double alpha = 0;
        return boolean(v,"_visible",shown) && shown && number(v,"_alpha",alpha) && alpha > 0 && alpha <= 1000;
    }
    bool visibleTo(GfxHandle v, const GfxIdentity& ancestor, unsigned limit) {
        // Keep only one parent reference at a time. Child stays pinned by caller.
        GfxHandle current = v, owned{};
        bool okay = false;
        for (unsigned i = 0; i < limit; ++i) {
            if (!visible(current)) break;
            if (same(current,ancestor)) { okay = true; break; }
            const auto parent = s.member(current,"_parent");
            s.release(owned); owned = parent; current = parent;
            if (!parent || !s.object(parent)) break;
        }
        s.release(owned); return okay;
    }
    bool toGlobal(GfxHandle object, double x, double y, WidgetPoint& point) {
        Value p(s,s.point(x,y)); if (!p) return false;
        Value ignored(s,s.invoke(object,"localToGlobal",{p}));
        return ignored && number(p,"x",point.x) && number(p,"y",point.y);
    }
    void valueArea(GfxHandle inner, WidgetTarget& row) {
        auto text=member(inner,"currentValue"), parent=member(text,"_parent");
        double x=0,y=0,w=0,h=0,rotation=0,xscale=0,yscale=0;
        if(!text || !same(parent,row.inner) || !visible(text) ||
            !number(text,"_x",x) || !number(text,"_y",y) || !number(text,"_width",w) || !number(text,"_height",h) ||
            !number(text,"_rotation",rotation) || !number(text,"_xscale",xscale) || !number(text,"_yscale",yscale) ||
            w<=0 || h<=0 || rotation!=0 || xscale!=100 || yscale!=100) return;
        WidgetPoint a,b,na,nb;
        if(toGlobal(inner,x,y,a) && toGlobal(inner,x+w,y+h,b) &&
            NormalizeWidgetPoint(out.frame,out.viewport,out.surfaceWidth,out.surfaceHeight,a,na) &&
            NormalizeWidgetPoint(out.frame,out.viewport,out.surfaceWidth,out.surfaceHeight,b,nb))
            row.valueBounds=intersect({na.x,na.y,nb.x,nb.y},row.bounds);
    }
    bool spinner(GfxHandle renderer, GfxHandle rowData, WidgetTarget& row) {
        auto inner=member(renderer,"inner"), data=member(inner,"data"), parent=member(inner,"_parent");
        auto choices=member(rowData,"spinnerChoices");
        std::int32_t count=0,index=0;
        if(!s.identity(inner,row.inner) || !same(data,row.data) || !same(parent,row.renderer) ||
            !visibleTo(inner,row.renderer,6) || !s.identity(choices,row.choices) || s.type(choices)!=9 ||
            !integer(choices,"length",count,1,128) || !integer(rowData,"currentValue",index,0,count-1))
            return stop("spinner-binding-or-range");
        for(std::int32_t i=0;i<count;++i) {
            Value choice(s,s.element(choices,static_cast<std::uint32_t>(i)));
            std::string label;
            if(!choice || !s.string(choice,label)) return stop("spinner-choice-type");
            row.choiceLabels.push_back(std::move(label));
        }
        row.minimum=0;row.maximum=count-1;row.increment=1;row.value=index;
        valueArea(inner,row);
        return true;
    }
    bool slider(GfxHandle renderer, GfxHandle rowData, GfxHandle root, WidgetTarget& row) {
        auto inner = member(renderer,"inner");
        auto slider = member(inner,"slider");
        auto data = member(inner,"data");
        if (!s.identity(inner,row.inner) || !s.identity(slider,row.slider) || !same(data,row.data) ||
            !visibleTo(slider,row.renderer,6)) return stop("slider-identity-visibility");
        valueArea(inner,row);
        bool disabled = false;
        if (!boolean(slider,"disabled",disabled)) return stop("slider-disabled-type");
        row.enabled = row.enabled && !disabled;
        if (!number(rowData,"minValue",row.minimum) || !number(rowData,"maxValue",row.maximum) ||
            !number(rowData,"sliderIncrement",row.increment) || !number(slider,"value",row.value) ||
            row.maximum <= row.minimum || row.increment <= 0 || row.increment > row.maximum - row.minimum ||
            row.value < row.minimum || row.value > row.maximum) return stop("slider-range");
        double actualMin = 0, actualMax = 0, actualIncrement = 0;
        if (!number(slider,"minimum",actualMin) || !number(slider,"maximum",actualMax) ||
            !number(slider,"snapInterval",actualIncrement) || actualMin != row.minimum ||
            actualMax != row.maximum || actualIncrement != row.increment) return stop("slider-model-renderer-mismatch");
        GfxRect sliderStage, trackLocal;
        auto track = member(slider,"track");
        if (!bounds(slider,root,sliderStage) || !normalized(sliderStage,row.sliderBounds) ||
            !bounds(track,slider,trackLocal)) return stop("slider-bounds");
        double width = 0, left = 0, right = 0;
        if (!number(slider,"__width",width) || !number(slider,"offsetLeft",left) ||
            !number(slider,"offsetRight",right) || left < 0 || right < 0 || width <= left + right) return stop("slider-axis");
        const auto y = (trackLocal.top + trackLocal.bottom) * 0.5;
        WidgetPoint a,b;
        if (!toGlobal(slider,left,y,a) || !toGlobal(slider,width - right,y,b) ||
            !NormalizeWidgetPoint(out.frame,out.viewport,out.surfaceWidth,out.surfaceHeight,a,row.sliderStart) ||
            !NormalizeWidgetPoint(out.frame,out.viewport,out.surfaceWidth,out.surfaceHeight,b,row.sliderEnd)) return stop("slider-transform");
        // The real track axis still controls the value; the finger target uses
        // the row height and a small horizontal margin, clipped to that row.
        const double padding = .025 * std::min(out.surfaceWidth,out.surfaceHeight) / out.surfaceWidth;
        row.sliderBounds = intersect({row.sliderBounds.left - padding,row.bounds.top,
            row.sliderBounds.right + padding,row.bounds.bottom},row.bounds);
        double projected = 0;
        return ProjectWidgetSlider(row,row.sliderStart,out.surfaceWidth,out.surfaceHeight,projected) || stop("slider-degenerate-axis");
    }
    bool build(const UiModelSnapshot& model) {
        if (!s.good() || !model.valid || !model.list || !model.provider || model.eventIds.empty() ||
            model.eventIds.size() > 128 || (model.movieClass != "FrontendGFxMovie" &&
            model.movieClass != "PauseGFxMovie" && model.movieClass != "OptionsGFxMovie")) return stop("model-or-family");
        out.model = model.structure;
        Value list(s,s.pin(static_cast<std::uint32_t>(model.list >> 32) + 0x3C));
        auto root = member(list,"_root"), data = member(list,"data"), renderers = member(list,"renderers");
        if (!s.identity(list,out.list) || !s.identity(root,out.root) || !s.identity(data,out.data) ||
            s.type(list) != 10 || s.type(root) != 10 || s.type(data) != 9 || s.type(renderers) != 9)
            return stop("list-root-data-renderers");
        double x = 0,y = 0,xscale = 0,yscale = 0,rotation = 0;
        if (!number(root,"_x",x) || !number(root,"_y",y) || !number(root,"_xscale",xscale) ||
            !number(root,"_yscale",yscale) || !number(root,"_rotation",rotation) ||
            std::abs(x) > 0.001 || std::abs(y) > 0.001 || std::abs(xscale - 100) > 0.001 ||
            std::abs(yscale - 100) > 0.001 || std::abs(rotation) > 0.001) return stop("root-stage-transform-unreviewed");
        if (!s.frameRect(out.frame) || !s.viewport(out.viewport)) return stop("movie-frame-viewport");
        if (!bounds(list,root,out.listStageBounds) || !normalized(out.listStageBounds,out.listBounds)) return stop("list-bounds-or-surface-map");
        out.listBounds = intersect(out.listBounds,{0,0,1,1});
        if (!validRect(out.listBounds) || !visibleTo(list,out.root,10)) return stop("list-not-visible");
        bool disabled = false;
        std::int32_t rendererArrayCount = 0;
        if (!boolean(list,"disabled",disabled) || disabled) return stop("list-disabled");
        if (!integer(data,"length",out.rowCount,1,128)) return stop("list-row-count-range");
        if (out.rowCount != static_cast<std::int32_t>(model.eventIds.size()))
            return stop("list-event-count-AS-" + std::to_string(out.rowCount) + "-model-" + std::to_string(model.eventIds.size()));
        // Shipped CLIK draw derives this count from the current list height
        // and row height. It is not limited to the original short menu's rows.
        // Keep it bounded by the same finite limit as the data provider.
        if (!integer(list,"totalRenderers",out.totalRenderers,1,128)) return stop("list-renderer-count-range");
        if (!integer(renderers,"length",rendererArrayCount,1,128) || rendererArrayCount != out.totalRenderers)
            return stop("list-renderer-array-count-" + std::to_string(rendererArrayCount) + "-total-" + std::to_string(out.totalRenderers));
        if (!integer(list,"scrollPosition",out.scrollPosition,0,std::max(0,out.rowCount - out.totalRenderers)))
            return stop("list-scroll-range");
        auto provider = member(list,"dataProvider");
        if (!same(provider,out.data)) return stop("uncommitted-data-provider");
        if (!integer(list,"selectedIndex",out.selectedIndex,-1,out.rowCount - 1)) return stop("list-selection");
        for (std::int32_t i = 0; i < out.totalRenderers && i + out.scrollPosition < out.rowCount; ++i) {
            WidgetTarget row; row.index = i + out.scrollPosition; row.eventId = model.eventIds[row.index];
            Value renderer(s,s.element(renderers,static_cast<std::uint32_t>(i)));
            Value item(s,s.element(data,static_cast<std::uint32_t>(row.index)));
            auto rendererData = member(renderer,"data"), owner = member(renderer,"owner");
            std::int32_t actualIndex = -1;
            if (!s.identity(renderer,row.renderer) || !s.identity(item,row.data) ||
                !same(rendererData,row.data) || !same(owner,out.list) ||
                !integer(renderer,"index",actualIndex,0,127) || actualIndex != row.index) return stop("renderer-binding-pending");
            bool unselectable = true, rendererDisabled = true;
            auto selectable=member(item,"unselectable"), disabledValue=member(renderer,"disabled");
            auto caption=member(item,"label"), rendererType=member(item,"rendererType");
            if(!s.boolean(selectable,unselectable)) return stop("row-unselectable-type-"+std::to_string(s.type(selectable)));
            // CLIK's disabled getter reads an initially undefined _disabled;
            // ActionScript and the stock GetBool treat that value as false.
            if(!s.boolean(disabledValue,rendererDisabled,true)) return stop("row-disabled-type-"+std::to_string(s.type(disabledValue)));
            if(!s.string(caption,row.caption)) return stop("row-caption-type-"+std::to_string(s.type(caption)));
            if(!s.string(rendererType,row.rendererType)) return stop("row-renderer-type-"+std::to_string(s.type(rendererType)));
            row.enabled = !unselectable && !rendererDisabled;
            row.visible = visibleTo(renderer,out.list,6);
            if (!bounds(renderer,root,row.stageBounds) || !normalized(row.stageBounds,row.bounds)) return stop("row-bounds");
            WidgetPoint origin;
            if(!toGlobal(renderer,0,0,origin) || !NormalizeWidgetPoint(out.frame,out.viewport,
                out.surfaceWidth,out.surfaceHeight,origin,row.origin)) return stop("row-origin");
            row.bounds = intersect(row.bounds,out.listBounds);
            row.visible = row.visible && validRect(row.bounds);
            if (row.rendererType == "default") row.kind = WidgetKind::Button;
            else if (row.rendererType == "slider") {
                row.kind = WidgetKind::Slider;
                if (row.visible && !slider(renderer,item,root,row)) return false;
            } else if (row.rendererType == "spinner") {
                row.kind = WidgetKind::Spinner;
                if(row.visible && !spinner(renderer,item,row)) return false;
            }
            std::uint64_t id = kHashStart;
            hash(id,out.list); hash(id,out.data); hash(id,row.renderer); hash(id,row.data);
            hash(id,static_cast<std::uint32_t>(row.index)); hash(id,static_cast<std::uint32_t>(row.eventId));
            hash(id,row.rendererType); row.id = id ? id : 1;
            out.rows.push_back(std::move(row));
        }
        // Detect a getter/callback refresh during capture before publishing any
        // rows. Caller additionally compares fresh UE3 model structure/tokens.
        auto finalData = member(list,"data"), finalProvider = member(list,"dataProvider");
        std::int32_t scroll = -1;
        if (!same(finalData,out.data) || !same(finalProvider,out.data) ||
            !integer(list,"scrollPosition",scroll,0,128) || scroll != out.scrollPosition) return stop("content-changed-during-capture");
        out.route = kHashStart;
        hash(out.route,out.model); hash(out.route,out.list); hash(out.route,out.data); hash(out.route,out.root);
        // getBounds includes animated selection artwork. Geometry is used for
        // the initial hit, while the movie/list/data identities own the gesture.
        hash(out.route,out.frame);
        hash(out.route,out.surfaceWidth); hash(out.route,out.surfaceHeight);
        for (auto w : out.viewport.words) hash(out.route,w);
        out.content = out.route;
        for (const auto& row : out.rows) {
            hash(out.content,row.id); hash(out.content,row.caption); hash(out.content,row.enabled); hash(out.content,row.visible);
            hash(out.content,row.bounds); hash(out.content,row.slider); hash(out.content,row.sliderBounds);
            hashDouble(out.content,row.origin.x);hashDouble(out.content,row.origin.y);
            hash(out.content,row.valueBounds);hash(out.content,row.choices);
            for(const auto& label:row.choiceLabels) hash(out.content,label);
            hashDouble(out.content,row.minimum); hashDouble(out.content,row.maximum); hashDouble(out.content,row.increment);
            hashDouble(out.content,row.sliderStart.x); hashDouble(out.content,row.sliderStart.y);
            hashDouble(out.content,row.sliderEnd.x); hashDouble(out.content,row.sliderEnd.y);
        }
        if (!s.good()) return stop(s.reason() ? s.reason() : "scene-changed");
        out.valid = true; out.reason = "observed"; return true;
    }
};
}
bool NormalizeWidgetPoint(const GfxRect& frame, const GfxViewport& viewport,
                          std::uint32_t width, std::uint32_t height, WidgetPoint point, WidgetPoint& out) {
    const auto& v = viewport.words;
    if (!validRect(frame) || !width || !height || width > 16384 || height > 16384 ||
        v[0] != width || v[1] != height || !v[4] || !v[5] ||
        std::uint64_t(v[2]) + v[4] > width || std::uint64_t(v[3]) + v[5] > height ||
        !std::isfinite(point.x) || !std::isfinite(point.y)) return false;
    out.x = (v[2] + (point.x - frame.left) * v[4] / (frame.right - frame.left)) / width;
    out.y = (v[3] + (point.y - frame.top) * v[5] / (frame.bottom - frame.top)) / height;
    return std::isfinite(out.x) && std::isfinite(out.y);
}
bool WidgetContains(const GfxRect& r, WidgetPoint p) {
    return validRect(r) && std::isfinite(p.x) && std::isfinite(p.y) &&
        p.x >= r.left && p.x <= r.right && p.y >= r.top && p.y <= r.bottom;
}
bool ProjectWidgetSlider(const WidgetTarget& row, WidgetPoint p, std::uint32_t w, std::uint32_t h, double& value) {
    if (!w || !h || !std::isfinite(p.x) || !std::isfinite(p.y)) return false;
    const auto dx = (row.sliderEnd.x - row.sliderStart.x) * w;
    const auto dy = (row.sliderEnd.y - row.sliderStart.y) * h;
    const auto square = dx * dx + dy * dy;
    if (!std::isfinite(square) || square < 1) return false;
    const auto dot = (p.x - row.sliderStart.x) * w * dx + (p.y - row.sliderStart.y) * h * dy;
    value = std::clamp(dot / square,0.0,1.0); return std::isfinite(value);
}
WidgetRegistry CaptureBl2Widgets(GfxSession& session, const UiModelSnapshot& model, std::uint32_t width, std::uint32_t height) {
    Reader reader{session,{}};
    reader.out.surfaceWidth = width; reader.out.surfaceHeight = height;
    reader.build(model);
    if(!session.good() && session.reason()) reader.out.reason=session.reason();
    return std::move(reader.out);
}
} // namespace bl2::touch
