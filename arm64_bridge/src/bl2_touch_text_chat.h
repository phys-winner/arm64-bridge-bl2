#pragma once
#include "bl2_touch_gfx_values.h"
#include <optional>

namespace bl2::touch {

struct TextChatInput {
    GfxIdentity chat{},input{};
    GfxRect bounds{};
    std::string text;
};
// Original UI_TextChat pcChat/chatInput, visible and focused after native
// StartTextChatInternal. The original TextInput maximum is 128 UTF16 units.
bool CaptureBl2TextChatInput(GfxSession&,GfxHandle chat,TextChatInput&);
bool AssignBl2TextChatInput(GfxSession&,GfxHandle chat,const TextChatInput&,const std::string& utf8);

struct TextChatOwner {
    std::uint64_t epoch=0,scene=0,generation=0,owner=0,controller=0,world=0;
    std::uint32_t movie=0,interface=0,viewport=0,width=0,height=0,activity=0;
    GfxIdentity chat{},input{};
    explicit operator bool() const;
    bool operator==(const TextChatOwner&) const = default;
};
struct TextChatResult { TextChatOwner owner{};std::string text;bool canceled=false; };

// Java's existing GetStringUTFChars returns modified UTF8. Preserve BMP text
// and convert a valid surrogate pair to genuine UTF8 for the original SF field.
// NUL, isolated surrogates, invalid/overlong input and >128 units are rejected.
std::optional<std::string> Bl2TextChatUtf8(const std::string&);

struct TextChatKeyboardState {
    std::uint32_t activity=0;
    bool known=false,active=false,open=false,done=false;
};
// Original Activity bAppActive/bKeyboardOpen and the original completion flag.
// Unknown JNI/schema is a rejection, never an inferred active keyboard.
TextChatKeyboardState ObserveBl2TextChatKeyboard(GfxTransport&,std::uint32_t base);

// The native ARM32 original service/Java keyboard is reused. No second Android
// view or KeyboardFinished delegate is introduced. Scratch must belong to a
// separate GfxHostTransport, with no live GfxSession occupying that block.
bool ShowBl2TextChatKeyboard(GfxTransport&,std::uint32_t base,std::uint32_t scratch,
                            const std::string& title,const std::string& initial);

class UiTextChatKeyboard {
public:
    // Call only after the above original service accepted this exact owner.
    bool opened(const TextChatOwner&,std::uint64_t now);
    // A keyboard's own focus/IME reset is explicitly acknowledged while its
    // exact original service is open, or while consuming its observed completion.
    // Only ManualReset/HeartbeatLost are accepted; lifecycle/scene resets retire.
    bool acknowledgeModalEpoch(const TextChatOwner&,const TextChatKeyboardState&,
                               std::uint32_t resetReasons,std::uint64_t now);
    std::optional<TextChatResult> poll(GfxTransport&,std::uint32_t base,const TextChatOwner& current,
                                       const TextChatKeyboardState&,bool inputAllowed,bool reset,std::uint64_t now);
    // Keep the outstanding service until completion; never relabel a late old
    // result as a new TextChat/Character request. Retired results are discarded.
    void retire() { retired_=true; }
    bool pending() const { return pending_; }
private:
    TextChatOwner owner_{};
    std::uint64_t time_=0;
    bool pending_=false,retired_=false,observedModal_=false;
};
} // namespace bl2::touch
