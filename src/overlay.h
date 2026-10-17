#pragma once
#include <windows.h>

// DirectX 12 overlay: hooks the game's swap chain to draw the HUD and the ImGui menu on top of
// the game, and the window procedure / DirectInput to feed and isolate input while the menu is
// open.
namespace zz::overlay {

bool Install();
bool MenuOpen();
void SetMenuOpen(bool open);
HWND GameWindow();
bool GameFocused();

}  // namespace zz::overlay
