#pragma once

namespace jsfx {

/// Stereo sample matching Godot's `AudioFrame` layout, without depending on
/// godot-cpp. This keeps the headless core buildable and testable on its own.
struct AudioFrame {
  float left = 0.0f;
  float right = 0.0f;
};

}  // namespace jsfx
