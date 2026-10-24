#pragma once

// Sideways camera offset (over-the-shoulder): every frame the render camera (CS::CSPersCam) copies
// itself into the data the renderer uses; in that copy the position is moved along the camera's
// right vector. The camera itself is not touched, so nothing adds up and aiming / lock-on are
// unchanged. The moved part has no wall collision.
namespace zz::camera_offset {

// Finds and hooks the camera copy (once, after game::Init).
bool Install();
bool Installed();

}  // namespace zz::camera_offset
