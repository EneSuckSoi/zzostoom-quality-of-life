#pragma once
#include <string>

namespace zz::ui {

// Called once after the ImGui context exists (fonts, style).
void Init(float scale);
// Called inside the ImGui frame: HUD always, the menu window when open.
void Draw(bool menu_open);

void Notify(const std::string& text);

// Controller: LB/RB move between tabs; the window takes focus when the menu opens.
void SwitchTab(int delta);
void OnMenuToggled(bool open);

}  // namespace zz::ui
