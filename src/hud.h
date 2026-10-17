#pragma once

// Everything drawn over the game while the menu is closed: stance (poise) bars, HP numbers,
// buff timers, FPS counter, pause banner.
namespace zz::hud {

// Inside the ImGui frame, every frame.
void Draw();
// Draws example bars at fixed positions (menu preview / test harness).
void DrawPreview();

}  // namespace zz::hud
