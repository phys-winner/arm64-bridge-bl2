// HIDE GAMEPAD BUTTONS: the markup rewrite of StringAliasMap MappedText.
#include "bl2_touch_gamepad_glyphs.h"
#include <cassert>
#include <cstdio>
#include <string>

using namespace bl2::touch;

int main() {
    std::string hidden;
    const std::string accept = "<IMG src='android_A' vspace='-3'>";
    assert(Bl2HideGamepadMarkup(accept, GamepadGlyphHiding::Remove, hidden) && hidden.empty());
    assert(Bl2HideGamepadMarkup(accept, GamepadGlyphHiding::Shrink, hidden) &&
        hidden == "<IMG src='android_A' width='1' height='1'>");
    assert(Bl2HideGamepadMarkup(accept, GamepadGlyphHiding::Unresolved, hidden) && hidden == "<IMG src='bl2hidden_android_A'>");
    // ShiftBothTrigger mixes two platforms in the shipped ANDROID set; both go.
    assert(Bl2HideGamepadMarkup("<IMG src='android_LT' vspace='-3'><IMG src='xbox360_RT' vspace='-3'>",
        GamepadGlyphHiding::Shrink, hidden) &&
        hidden == "<IMG src='android_LT' width='1' height='1'><IMG src='xbox360_RT' width='1' height='1'>");
    // Text and other tags around the image stay byte for byte.
    assert(Bl2HideGamepadMarkup("Press <img SRC=\"ps3_Triangle\" vspace='-3'> to <b>customize</b>",
        GamepadGlyphHiding::Remove, hidden) && hidden == "Press  to <b>customize</b>");
    // Non-gamepad markup is not a gamepad image: nothing to hide, output untouched.
    hidden = "unchanged";
    for (const char* text : {"<Strings:WillowGame.Terms.Controller_PC>", "", "[E]", "<IMG src='img://Icons.Skull'>",
                             "<IMG src='android_'>", "<IMG vspace='-3'>", "<IMG src='android_A'"})
        assert(!Bl2HideGamepadMarkup(text, GamepadGlyphHiding::Shrink, hidden) && hidden == "unchanged");
    // The footer parser maps a renamed source back to its button.
    assert(Bl2VisibleGlyphSource("bl2hidden_android_RB") == "android_RB");
    assert(Bl2VisibleGlyphSource("android_RB") == "android_RB");
    std::puts("gamepad glyph markup checks passed");
    return 0;
}
