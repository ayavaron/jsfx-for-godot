#pragma once

#include <string>
#include <vector>

namespace jsfx {

/// One `sliderN:default<min,max,step>description` declaration from the JSFX
/// header. Slider indices are 1-based and capped at 256, matching REAPER 7.
struct JsfxSlider {
  int index = 0;
  double default_value = 0.0;

  /// Named-slider syntax `slider1:gain_db=0<...>`: the script reads and writes
  /// `gain_db` instead of `slider1`. Empty for classic sliders.
  std::string variable_name;

  /// `<min,max,step:log>` (or `:sqr`): the slider is shaped in REAPER's UI.
  /// Godot shows it with the `exp` range hint; the value itself is unchanged.
  bool logarithmic = false;

  /// File-chooser slider (`slider1:/folder:default.wav:desc`). Godot exposes
  /// it as a plain index; the file list is not available outside REAPER.
  bool is_file = false;

  /// Description started with `-`: REAPER hides the slider from its UI.
  bool hidden = false;

  bool has_range = false;
  double min_value = 0.0;
  double max_value = 0.0;
  double step = 0.0;

  /// Labels from `<min,max,step{Low,Medium,High}>`, if present.
  std::vector<std::string> enum_labels;

  /// Display text after the range, e.g. "Trim (dB)". Empty when omitted.
  std::string description;

  /// Godot-facing property name: `slider_<N>` plus a sanitized description,
  /// e.g. `slider_1_volume`. Generated once on the main thread.
  std::string property_name;
};

}  // namespace jsfx
