#pragma once
#include <string>

// Per-frame glue for the features that are not drawn: camera table, instant roll context, pause,
// background mute.
namespace zz::features {

// Render thread, every frame before drawing.
void Tick(float dt, bool menu_open);
// Render thread, after the frame was submitted (frame cap while paused).
void AfterFrame();
// Starts the background thread (audio session watcher).
void StartBackground();

void TogglePause();
bool Paused();
std::string PauseStatus();

bool AudioMuted();
std::string AudioStatus();

void SetGameCursorVisible(bool visible);

}  // namespace zz::features
