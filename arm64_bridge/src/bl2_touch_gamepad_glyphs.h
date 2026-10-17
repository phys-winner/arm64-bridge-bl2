#pragma once
#include <string>

class DynarmicHost;

namespace bl2::touch {

// How HIDE GAMEPAD BUTTONS rewrites one gamepad button image of the game's
// StringAliasMap markup, <IMG src='android_A' vspace='-3'>:
//   Remove      drops the tag; the footer text closes up.
//   Shrink      keeps the image source at 1x1 px, so native footers still name
//               the button of every caption (Generic footer taps keep working).
//   Unresolved  renames the source to bl2hidden_<source>: Scaleform finds no
//               image, the footer parser maps the name back.
enum class GamepadGlyphHiding : unsigned { Remove = 0, Shrink = 1, Unresolved = 2 };

// True when text holds at least one gamepad button image (android_, xbox360_
// or ps3_ source); hidden then receives the rewritten markup, other text and
// tags unchanged.
bool Bl2HideGamepadMarkup(const std::string& text, GamepadGlyphHiding mode, std::string& hidden);

// The original image source of a footer glyph: "bl2hidden_android_A" -> "android_A".
std::string Bl2VisibleGlyphSource(const std::string& source);

} // namespace bl2::touch

// Game thread, at the touch input boundary: applies or restores the setting
// on the live UIDataStore_StringAliasMap objects (instance and class default).
void Bl2TouchGamepadGlyphsTick(DynarmicHost& host);
