#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bl2::touch {
bool Bl2TouchReviewedUiMovie(std::string_view);
bool Bl2TouchNativeDialogMovie(std::string_view);

// A read-only bridge. The implementation never stores this view or guest pointers
// after capture and never calls guest code. Only use at the input-thread boundary.
struct UiModelMemory {
    const void* context = nullptr;
    bool (*copy)(const void*, std::uint32_t, void*, std::size_t) = nullptr;
};

struct UiPropertyWitness {
    std::string name;
    std::uint32_t property = 0, offset = 0, elementSize = 0, extra = 0;
};

struct UiDialogButtonModel {
    std::string tag;
    std::vector<std::string> keys;
};

struct UiNativeMethod { std::uint32_t function = 0, flags = 0; std::uint16_t bytes = 0; std::uint64_t token = 0; };
struct UiAgeGateModel {
    bool valid = false;
    std::string reason;
    std::uint64_t owner = 0, clip = 0, structure = 0;
    UiNativeMethod input;
};
// GearboxAccount's Android A handler checks the original ScreenStack top,
// then the focused month/day/year field. Other account screens stay generic.
UiAgeGateModel CaptureBl2AgeGate(const UiModelMemory&,std::uint32_t base,std::uint32_t owner);
struct UiTextChatModel {
    bool valid=false;
    std::string reason;
    std::uint64_t owner=0,chat=0;
    UiNativeMethod input;
};
UiTextChatModel CaptureBl2TextChat(const UiModelMemory&,std::uint32_t base,std::uint32_t owner);
// StopTextChatInternal retains ChatObj but clears the declaring GFxMoviePlayer
// capture flag. Observe that exact registered pair without touching its AS view.
bool Bl2TouchTextChatStopped(const UiModelMemory&,std::uint32_t base,
                            std::uint64_t owner,std::uint64_t chat);
struct UiTradingOfferModel {
    bool valid=false;
    std::string reason;
    std::uint64_t owner=0,panel=0;
    UiNativeMethod inventory,canStart;
};
// The four local offer cells have no unique AS item data. Resolve the original
// read-only GetOfferedCellInventory/CanStartOffer schema and selected source.
UiTradingOfferModel CaptureBl2TradingOffer(const UiModelMemory&,std::uint32_t base,
                                          std::uint32_t owner,std::int32_t index);
// Trading StartOffer intentionally freezes/resorts the original source list.
// Capture its genuine item getter before that transition, then revalidate the
// same OfferingThing/FrozenThing during our acknowledged original AS drag.
UiTradingOfferModel CaptureBl2TradingBackpack(const UiModelMemory&,std::uint32_t base,
                                             std::uint32_t owner,std::int32_t index);
std::uint64_t Bl2TouchTradingHeldItem(const UiModelMemory&,std::uint32_t base,std::uint32_t owner);
std::uint64_t Bl2TouchInventoryToken(const UiModelMemory&,std::uint32_t base,std::uint32_t inventory);
struct UiStatusInventoryModel {
    bool valid=false;
    std::string reason;
    std::uint64_t owner=0,top=0,backpack=0,equipped=0;
    UiNativeMethod backpackInventory,equippedInventory;
};
// Exact Inventory screen3 and its original typed panel links. The two pure
// GetThingByIndex queries pin the actual inventory item, not its thumbnail.
UiStatusInventoryModel CaptureBl2StatusInventory(const UiModelMemory&,std::uint32_t base,std::uint32_t owner);
struct UiStatusInventoryInputState {
    bool valid=false,doingEquip=false,setupFinished=false;
    std::string reason,delegateName,queuedName,triggerName;
    std::uint64_t owner=0,top=0,delegateObject=0,queuedObject=0;
    bool movieValid=false,captureInput=false,discardNonOwnerInput=false;
    std::int32_t localPlayerOwnerIndex=-1;
    std::string movieReason,movieDelegateName,movieDelegateClass;
    std::uint64_t movieDelegateObject=0;
};
// Diagnostic only: reflect the original Movie and Top input delegates/filters.
// No callbacks or guessed offsets; the caller gates all work on its log flag.
UiStatusInventoryInputState CaptureBl2StatusInventoryInputState(const UiModelMemory&,std::uint32_t base,std::uint32_t owner);
struct UiDirectModel {
    bool valid = false;
    bool trainingPrompt = false;
    std::string movieClass, reason;
    std::uint64_t owner = 0, structure = 0, clip = 0;
    std::int32_t screen = -1, selected = -1;
    std::uint32_t screenField = 0,screenOffset = 0,screenSize = 0,screenDimension = 1;
    std::uint32_t selectionField = 0,selectionOffset = 0,clipField = 0,clipOffset = 0,player = 0;
    std::uint32_t mask = 0;
    UiNativeMethod hover, click, select, menuClick, startDrag, drag, stopDrag, zoom, waypoint, legend, info;
};
struct UiMapState {
    bool valid = false;
    std::string reason;
    std::uint64_t map = 0, world = 0;
    float zoomSpeed = 0, deltaSeconds = 0, cursorX = 0, cursorY = 0, clipWidth = 0, clipHeight = 0;
    std::uint32_t cursorProperty = 0, cursorOffset = 0;
};
// Current Map only. Read the same original Float/Vector2D properties used by
// AnalogScale and PlaceCustomObjective; no guessed cursor or world offsets.
UiMapState CaptureBl2MapState(const UiModelMemory&,std::uint32_t base,const UiDirectModel&);
// Only original direct-input families with independently proved parameters.
// Character state/selection have two local-player slots; no pointer-sized or
// single-slot reinterpretation is admitted. Status Map is original enum value2.
UiDirectModel CaptureBl2DirectUi(const UiModelMemory&,std::uint32_t base,
                               std::uint32_t owner,std::uint32_t localPlayer = 0);
bool Bl2TouchDirectUiCurrent(const UiModelMemory&,std::uint32_t base,const UiDirectModel&,
                            std::int32_t* selected = nullptr);
// Empty Training dialogs expose their timed, original single prompt rather
// than DialogBox Buttons[]. Only a genuine negative-delay state closes through
// the original 20-byte HandleInputKey; its OnClose owns status-tab transitions.
bool Bl2TouchTrainingPromptReady(const UiModelMemory&,std::uint32_t base,const UiDirectModel&);
// Bounded read-only diagnostic of the original SwfMovie asset; no guessed path.
std::string Bl2TouchMovieSource(const UiModelMemory&,std::uint32_t base,std::uint32_t owner);

struct UiModelSnapshot {
    bool valid = false;
    std::string reason, movieClass, providerClass;
    std::uint64_t owner = 0, list = 0, provider = 0, options = 0;
    // A diagnostic structural fingerprint, NOT a complete widget/content token.
    // Labels, enabled state, GFx renderers and asynchronous refresh are not covered.
    std::uint64_t structure = 0;
    std::int32_t selectedIndex = -1;
    std::uint32_t providerDepth = 0;
    bool noCancel = false;
    bool nativeMenuOpen = false;
    std::vector<std::int32_t> eventIds;
    std::vector<UiDialogButtonModel> buttons;
    std::vector<UiPropertyWitness> properties;
    std::size_t bytesRead = 0;
};

// base and owner belong to the exact SHA-bound ELF guarded by stage1 Install.
// Runtime layout comes from live UProperty metadata, not guessed game offsets.
UiModelSnapshot CaptureBl2UiModel(const UiModelMemory&, std::uint32_t base,
                                 std::uint32_t owner, bool decorateSettings = true);
// Engine.GameViewport is an ObjectProperty. The adjacent GamePlayers TArray's
// Num and the native FViewportClient interface are not UObject pointers.
std::uint32_t Bl2TouchGameViewportClient(const UiModelMemory&, std::uint32_t base);

// A live pawn plus the original engine Pause flag, World Pauser or owned Pause
// movie distinguishes a game menu from front-end Options. LAN need not stop World.
struct UiPauseWorldIdentity {
    std::uint64_t controller = 0, pawn = 0, world = 0;
};
bool Bl2TouchPausedWorld(const UiModelMemory&, std::uint32_t base,
                         std::uint32_t controller, std::uint32_t pauseMovieOwner = 0, std::string* reason = nullptr,
                         UiPauseWorldIdentity* identity = nullptr);
bool Bl2TouchAtStartTitle(const UiModelMemory&, std::uint32_t base,
                          std::uint32_t owner);

} // namespace bl2::touch
