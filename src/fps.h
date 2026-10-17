#pragma once
#include <string>

// Frame rate unlock. Two code constants are changed in memory:
//  - CSFlipper's target frame time (1/60 s, rewritten from an immediate every frame), so the limit
//    can change while playing;
//  - the 60 Hz refresh rate the game asks for in exclusive fullscreen (read when the game switches
//    to fullscreen, i.e. at start-up or after changing the display mode).
namespace zz::fps {

// Called as early as possible (before the game creates its swap chain).
void EarlyInit();
// Re-applies the configured limit (cheap, call when settings change).
void Apply();
// Frame counter, called from the Present hook.
void OnPresent();
float CurrentFps();
float FrameTimeMs();
int MonitorMaxHz();
// True when another FPS mod (UnlockTheFps) is loaded: ours stays out of the way.
bool OtherModActive();
std::string Status();  // in the menu language
bool Active();          // the unlocked limit is in effect

}  // namespace zz::fps
