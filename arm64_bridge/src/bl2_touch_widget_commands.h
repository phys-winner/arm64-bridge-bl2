#pragma once
#include "bl2_touch_widget_gestures.h"
#include "bl2_touch_inventory_gesture.h"
#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace bl2::touch {
struct InventoryWidgetTarget {
    InventoryDragSource source{};
    GfxIdentity rendererId{}, dataId{}, managerId{};
    GfxHandle panel{}, cell{}, renderer{}, data{}, manager{};
    std::int32_t index = -1;
    std::uint64_t nativeItem = 0;
    std::string label;
    explicit operator bool() const { return source.content != 0; }
};
// Diagnostic copies of values already read by capture; no additional GFx calls.
struct InventoryWidgetObservation {
    const char* reason="not-captured";
    const char* panel="none";
    unsigned step=0,tested=0,hitMask=0,selectableMask=0;
    std::int32_t index=-1;
    std::array<std::uint32_t,6> types{};
    std::array<std::uint32_t,4> managerTypes{};
    GfxIdentity topId{},panelId{},cellId{};
    double stageX=0,stageY=0;
};
struct InventoryDragObservation {
    bool known=false,active=false,started=false;
    std::uint32_t relatedType=0,inDragType=0;
    GfxIdentity manager{},origin{},payload{};
};
// Resolve the genuine StoragePanel.list.GetHitCell(globalPoint), then retain
// its original renderer/data and the scoped DragManager. The query is repeated
// at the captured origin; a moving finger cannot switch the source item.
// StatusInventory additionally requires native-pinned top/backpack/equipped
// identities. Its secondary query is Equipped, rather than Trading Offers.
InventoryWidgetTarget CaptureBl2InventoryWidget(GfxSession&, InventoryDragFamily,
    std::uint32_t width, std::uint32_t height, float surfaceX, float surfaceY,
    const std::function<std::uint64_t(std::int32_t,bool secondary)>& nativeInventory = {},
    const std::array<GfxIdentity,3>& statusPanels = {},InventoryWidgetObservation* observation = nullptr);
bool SameBl2InventoryWidget(const InventoryWidgetTarget&, const InventoryWidgetTarget&);
bool Bl2InventoryDragStarted(GfxSession&, const InventoryWidgetTarget&, GfxIdentity* data = nullptr,
                            bool* activeOrUnknown = nullptr,InventoryDragObservation* observation = nullptr);
bool CancelBl2InventoryDrag(GfxSession&, const InventoryWidgetTarget&);

enum class CharacterWidgetKind { None, Portrait, Menu, Customization };
struct AgeGateWidgetTarget {
    std::int32_t index = -1;
    GfxIdentity contentId{}, gateId{};
    std::array<GfxIdentity,3> fieldIds{};
    GfxHandle content{}, gate{};
    std::array<GfxHandle,3> fields{};
    std::array<std::string,3> text{};
    GfxRect bounds{};
    explicit operator bool() const { return index >= 0 && index < 3; }
};
// Exact GearboxAccount frame4 TextInputs, measured through native projection.
// Public focus changes on DOWN; only an exclusively focused matching UP may
// reach the movie's original Android XboxA keyboard branch.
AgeGateWidgetTarget CaptureBl2AgeGateWidget(GfxSession&,GfxHandle content,
    std::uint32_t width,std::uint32_t height,float x,float y);
bool SameBl2AgeGateWidget(const AgeGateWidgetTarget&,const AgeGateWidgetTarget&);
bool FocusBl2AgeGateWidget(GfxSession&,const AgeGateWidgetTarget&);
bool Bl2AgeGateExclusivelyFocused(GfxSession&,const AgeGateWidgetTarget&);
struct CharacterWidgetTarget {
    CharacterWidgetKind kind = CharacterWidgetKind::None;
    std::int32_t index = -1, player = -1, screen = -1;
    GfxIdentity contentId{}, containerId{}, widgetId{}, providerId{};
    GfxHandle content{}, container{}, widget{}, provider{};
    std::string label;
    GfxRect bounds{};
    explicit operator bool() const { return kind != CharacterWidgetKind::None; }
};
// Optional diagnostics copy only values already queried by the capture. They
// must not add guest calls or retain any additional GFx handles.
struct CharacterWidgetObservation {
    struct Row {
        const char* reason = "unobserved";
        std::uint32_t slot = 0, type = 0, itemType = 0, dataType = 0;
        double index = -1;
        bool shown = false, owner = false, disabledKnown = false, disabled = true;
        bool projected = false, normalized = false, hit = false;
        std::string label, displayed;
        GfxRect stage{}, bounds{};
    };
    const char* reason = "unobserved";
    std::uint32_t contentType = 0, panelType = 0, containerType = 0, providerType = 0, renderersType = 0;
    double count = 0, rendered = 0, scroll = 0;
    bool committed = false, disabledKnown = false, disabled = true;
    std::array<Row,8> rows{};
    std::uint32_t rowCount = 0;
};
// Original CharacterSelectRedux's cells/menuEntries/scrollingList renderers.
// Returned handles remain pinned in this session through DOWN/UP. A second
// bounded capture must match identities and semantic row before activation.
CharacterWidgetTarget CaptureBl2CharacterWidget(GfxSession&, GfxHandle content,
    std::int32_t screen, std::int32_t player, std::uint32_t width, std::uint32_t height,
    float surfaceX, float surfaceY, CharacterWidgetObservation* observation = nullptr);
bool SameBl2CharacterWidget(const CharacterWidgetTarget&,const CharacterWidgetTarget&);
bool ConfirmBl2CharacterPortrait(const CharacterWidgetTarget& down,const CharacterWidgetTarget& up,
                                bool wasSelected,std::int32_t selected);
// Portrait selection/confirmation use the original reflected native handlers.
// Menu/List highlighting uses their shipped public AS focus/selection methods;
// list click uses its original native-bound HandleCustomizationSelected.
bool SelectBl2CharacterWidget(GfxSession&,const CharacterWidgetTarget&);
bool ClickBl2CharacterCustomization(GfxSession&,const CharacterWidgetTarget&);

enum class NativeFooterFamily { Map, StatusInventory, TwoPanel, Vending, Trading, Training, Generic };
enum class NativeFooterCommand {
    None, Legend, Inspect, Mark, Sort, Select, Drop, Close, Accept, SellTrash, TrainingAccept,
    ButtonA, ButtonB, ButtonX, ButtonY, ButtonBack, ButtonStart,
    ButtonLSB, ButtonRSB, ButtonLB, ButtonRB, ButtonLT, ButtonRT
};
struct MapLegendTarget {
    NativeFooterCommand command = NativeFooterCommand::None;
    GfxIdentity parentId{}, textId{};
    GfxHandle parent{}, text{};
    std::string caption;
    std::string html;
    std::uint32_t first = 0, count = 0;
    GfxRect bounds{};
    double scaleX = 0, scaleY = 0;
    explicit operator bool() const { return count != 0 && command != NativeFooterCommand::None; }
};
struct MapLegendObservation {
    const char* reason="unobserved";
    std::uint32_t rootType=0,parentType=0,textType=0,rectType=0,first=0,count=0,measured=0;
    std::string caption;
    double x=0,y=0,scaleX=0,scaleY=0,rotation=0;
    GfxRect character{},local{},stage{},frame{},bounds{};
    bool projected=false,hit=false;
};
// The shipped Map footer is one HTML TextField, with no click handler. Measure
// only the actual Legend word through its native UTF-16 character rectangles;
// never split the footer by guessed percentages or activate its other hints.
MapLegendTarget CaptureBl2MapLegend(GfxSession&,std::uint32_t width,std::uint32_t height,float x,float y,
                                  MapLegendObservation* observation=nullptr);
// Native inventory hints are also one TextField. A visible, measured command
// routes to the original key handler; no footer-wide or guessed button areas.
MapLegendTarget CaptureBl2NativeFooter(GfxSession&,NativeFooterFamily,std::uint32_t width,std::uint32_t height,
                                     float x,float y,MapLegendObservation* observation=nullptr);
// Call at a live input boundary in a mutation-enabled session, with no captured
// touch. The exact painted cells keep their centre/bottom anchor, include their
// inline icons, and restores its original transform at interface scale 100%.
bool ScaleBl2NativeFooter(GfxSession&,NativeFooterFamily);
bool SameBl2MapLegend(const MapLegendTarget&,const MapLegendTarget&);
struct MapCursorTarget {
    bool valid=false, mousePending=false;
    WidgetPoint local{},surface{};
    const char* reason="map-cursor-unobserved";
};
// _xmouse/_ymouse use the shipped inherited3D ScreenToWorld path. Their
// localToGlobal round trip must match this exact touch before CursorPos is
// changed. A queued MouseEvent may not have advanced yet; never use its old
// mouse position as the new waypoint.
MapCursorTarget CaptureBl2MapCursor(GfxSession&,GfxHandle map,GfxHandle mask,
    std::uint32_t width,std::uint32_t height,float x,float y);

// Original menu descriptions are fixed EditTexts that scroll by whole lines
// (PageUp/PageDown, LT/RT, the wheel and the more-arrows). A vertical drag
// that starts on one moves the same TextField.scroll the originals do.
struct TextScrollTarget {
    const char* field;     // AS path of the TextField from _root
    bool parentOwnsSwipe;  // the whole parent panel owns the swipe
    const char* arrows;    // _root function redrawing the original arrows, or nullptr
    GfxRect box;           // authored EditText box, pixels at 100% scale
};
// Mission log (StatusMenuExGFxMovie screen1), challenge log (screen5) and the
// mission accept/turn-in dialog. Other screens have no scrollable description.
std::vector<const TextScrollTarget*> Bl2TextScrollTargets(std::string_view movieClass,std::int32_t screen);
// [_x,_y,_width,_height,_xscale,_yscale,_rotation] of the authored field. The
// field must still have its authored size (up to one uniform depth factor for
// a field with its own _z); Scaleform reports _x/_y at the box's own top-left,
// so the authored nonzero origin is never added again.
bool Bl2TextScrollLocalRect(const TextScrollTarget&,const std::array<double,7>& metrics,GfxRect& local);
// Dragging up by one line height reveals one later line.
std::int32_t Bl2TextScrollLine(std::int32_t start,std::int32_t maximum,double dragPixels,double linePixels);
struct TextScrollCapture {
    const TextScrollTarget* target=nullptr;
    GfxIdentity field{};
    GfxRect bounds{};        // surface-normalized region owning the swipe
    std::int32_t scroll=1,maximum=1;
    double linePixels=0;     // surface pixels per visible text line
    const char* reason="text-unobserved"; // diagnostics: last rejected step
    std::array<double,7> metrics{};       // diagnostics: last measured field
    explicit operator bool() const { return target!=nullptr; }
};
// Observation only: a visible, actually overflowing description under the point.
TextScrollCapture CaptureBl2TextScroll(GfxSession&,std::string_view movieClass,std::int32_t screen,
    std::uint32_t width,std::uint32_t height,float x,float y);
// Mutation session: the same field takes the line, clamped to its live
// maxscroll, then the original arrow function redraws.
bool ScrollBl2Text(GfxSession&,const TextScrollCapture&,std::int32_t line);

// Keep one of these on a separate, read-only GfxSession from DOWN until UP or
// cancellation. Pinned list/data/renderer/row/slider values prevent address reuse
// while the input capture survives across engine frames. The retention session
// is not reused for per-frame queries; each operation has a fresh bounded session
// and exclusive scratch. Both sessions pin the same Movie independently.
class WidgetBinding final {
public:
    WidgetBinding(GfxSession& retention, const UiModelSnapshot&, const WidgetRegistry&,
                  const WidgetTarget* target);
    ~WidgetBinding();
    WidgetBinding(const WidgetBinding&) = delete;
    WidgetBinding& operator=(const WidgetBinding&) = delete;
    bool valid() const;
    const char* reason() const { return reason_; }
    bool matches(const UiModelSnapshot&, const WidgetRegistry&, bool requireRow) const;
    const WidgetTarget& target() const { return target_; }
    GfxHandle list() const { return list_; }
    GfxHandle renderer() const { return renderer_; }
    GfxHandle rowData() const { return rowData_; }
    GfxHandle inner() const { return inner_; }
    GfxHandle slider() const { return slider_; }
private:
    GfxSession& s_;
    std::vector<GfxHandle> retained_;
    std::uint64_t model_ = 0, owner_ = 0, nativeList_ = 0, provider_ = 0, route_ = 0;
    WidgetTarget target_;
    GfxHandle list_, data_, renderer_, rowData_, inner_, slider_, choices_;
    const char* reason_ = "not-bound";
    GfxHandle keep(GfxHandle);
};

enum class WidgetDeliveryState { Rejected, Unchanged, Dispatched, SceneChanged, Failed };
struct WidgetDelivery {
    WidgetDeliveryState state = WidgetDeliveryState::Rejected;
    const char* reason = "unvalidated";
    double actualValue = 0;
};

// The authored slider value is inner.currentValue (a TextField), separate from
// numeric data.currentValue. Presentation never rounds the stored preference.
bool FormatBl2TouchSettingValue(GfxSession&, GfxHandle inner, std::size_t setting, double value);
// Caller retains the current native Options owner/provider and committed AS
// data identity. Recycled/pending renderers must agree with that data before a
// value is formatted; private CLIK typography fields are deliberately optional.
void FormatBl2TouchSettingsList(GfxSession&, GfxHandle list, const GfxIdentity& data,
                              const std::vector<std::int32_t>& eventIds);

// Preserve the native Options baseline in this exact live AS list. A bounded
// session failure can leave its augmented/custom data attached; a same-owner,
// same-provider reattach must recover the proven native rows and callbacks.
// Returned handle is session-owned and must be released by the caller.
GfxHandle CaptureBl2OptionsOriginal(GfxSession&, GfxHandle list, GfxHandle headerText,
                                   GfxHandle description, const UiModelSnapshot& original);

//125 is the shipped default; directional edits join the original10% grid.
double StepBl2InterfacePercent(double value, bool increase);

struct WidgetTypography {
    double originalRowHeight = 0, rowHeight = 0;
    std::uint32_t renderers = 0, fields = 0, fittedFields = 0;
};
// Options' automatic rowHeight can use its attached renderer height. Increase
// that row through the original CLIK setter, keeping clipping/scrolling intact.
// Main/Pause keep all native rows and selection animation; actual adjacent row
// positions prove pitch independently of animated selection-art bounds.
// Text follows the live global UI scale in its authored column; long labels
// grow as far as that column permits. Saved baselines allow100% restoration and
// prevent drift on recycled renderers/repeated pages/dynamic scale changes.
// Actual inner offset and track/thumb bounds reserve a separate control slot;
// vertical glyph fitting keeps selected captions above the following row.
// Frontend/Pause use captured native model rows; Options uses the committed
// current events, which may include synthetic settings in its original list.
WidgetTypography ScaleBl2WidgetTypography(GfxSession&, GfxHandle list,
                                         const GfxIdentity& data, const UiModelSnapshot& original,
                                         const std::vector<std::int32_t>& currentEvents,
                                         bool allowLayoutChange = true);

// Caller proves a live, screen-rendered PauseGFxMovie. Fit only its authored
// black scrim to the Movie's actual visible stage; retain original overscan,
// color/alpha and every menu/input transform. Inactive or replaced clips reject.
bool FitBl2PauseBackdrop(GfxSession&, GfxHandle nativeList,
                        GfxRect& visibleStage, GfxRect& before, GfxRect& after);

// exactModelStillCurrent must re-read the native list/provider structure and
// compare the owner/list/provider tokens/model hash from the supplied model,
// plus the sink's controller/focus/epoch/source guards. It is called immediately
// before each UI mutation, outside the mailbox lock, on the input thread.
// Result says a native semantic callback was dispatched, not hardware acceptance.
// Never retry an operation automatically after SceneChanged/Failed.
WidgetDelivery ApplyBl2WidgetCommand(GfxSession& operation, const WidgetBinding& retained,
    const UiModelSnapshot& freshModel, const WidgetRegistry& freshWidgets,
    const WidgetCommand&, std::uint32_t actualControllerId,
    const std::function<bool()>& exactModelStillCurrent);
}
