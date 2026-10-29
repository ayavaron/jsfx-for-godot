#include "audio_effect_jsfx.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "audio_effect_jsfx_instance.h"
#include "jsfx/processor.h"

using namespace godot;

namespace {

using SliderValues = std::array<
    double, static_cast<std::size_t>(jsfx::JsfxDocument::kMaxSliders)>;
using SliderSet = std::array<
    bool, static_cast<std::size_t>(jsfx::JsfxDocument::kMaxSliders)>;

double current_mix_rate() {
  if (AudioServer* server = AudioServer::get_singleton()) {
    const double rate = server->get_mix_rate();
    if (rate > 0.0) return rate;
  }
  return 48000.0;
}

bool on_main_thread() {
  OS* os = OS::get_singleton();
  return os != nullptr && os->get_thread_caller_id() == os->get_main_thread_id();
}

void apply_slider_values(jsfx::JsfxProcessor& processor,
                         const jsfx::JsfxDocument& document,
                         const SliderValues& values, const SliderSet& set) {
  for (const jsfx::JsfxSlider& slider : document.sliders()) {
    const auto slot = static_cast<std::size_t>(slider.index - 1);
    if (set[slot]) processor.set_slider(slider.index, values[slot]);
  }
}

void report_script_error(const char* kind, const String& path,
                         const jsfx::Error& error) {
  ERR_PRINT(String("JSFX ") + kind + " in '" + path + "' at line " +
            String::num_int64(error.line) + ": " + error.message.c_str());
}

/// Resolves `import` lines like REAPER: next to the importing script first,
/// then at the project root.
jsfx::JsfxImportResolver make_import_resolver(const String& script_path) {
  const String base_dir = script_path.get_base_dir();
  return [base_dir](std::string_view name) -> std::optional<std::string> {
    const String import_name =
        String::utf8(name.data(), static_cast<int64_t>(name.size()));
    for (const String& candidate :
         {base_dir.path_join(import_name), String("res://").path_join(import_name)}) {
      if (!FileAccess::file_exists(candidate)) continue;
      const CharString text = FileAccess::get_file_as_string(candidate).utf8();
      return std::string(text.get_data(), static_cast<std::size_t>(text.length()));
    }
    return std::nullopt;
  };
}

/// Enum sliders with integral values become an INT property with labels.
bool slider_is_enum(const jsfx::JsfxSlider& slider) {
  if (slider.enum_labels.size() < 2 || !slider.has_range) return false;
  return slider.step >= 1.0 && std::floor(slider.step) == slider.step &&
         std::floor(slider.min_value) == slider.min_value;
}

bool parse_slider_property_index(const StringName& property, int& index) {
  const String name = property;
  if (!name.begins_with("slider_")) return false;

  int value = 0;
  bool any_digit = false;
  for (int i = 7; i < name.length(); ++i) {
    const char32_t c = name[i];
    if (c < '0' || c > '9') break;
    any_digit = true;
    value = value * 10 + static_cast<int>(c - '0');
    if (value > jsfx::JsfxDocument::kMaxSliders) return false;
  }

  if (!any_digit || value < 1) return false;
  index = value;
  return true;
}

}  // namespace

namespace jsfx_godot {

struct AudioEffectJSFX::LoadedScript {
  jsfx::JsfxDocument document;
  bool has_document = false;
  std::unique_ptr<jsfx::JsfxProcessor> prepared;
  double sample_rate = 0.0;
  SliderValues values{};
  SliderSet set{};
};

AudioEffectJSFX::AudioEffectJSFX() {
  slider_values_.fill(0.0);
  slider_set_.fill(false);
}

AudioEffectJSFX::~AudioEffectJSFX() = default;

void AudioEffectJSFX::_bind_methods() {
  ClassDB::bind_method(D_METHOD("set_script_path", "path"),
                       &AudioEffectJSFX::set_script_path);
  ClassDB::bind_method(D_METHOD("get_script_path"),
                       &AudioEffectJSFX::get_script_path);
  ADD_PROPERTY(PropertyInfo(Variant::STRING, "script_path",
                            PROPERTY_HINT_FILE, "*.jsfx"),
               "set_script_path", "get_script_path");

  ClassDB::bind_method(D_METHOD("set_downsample_factor", "factor"),
                       &AudioEffectJSFX::set_downsample_factor);
  ClassDB::bind_method(D_METHOD("get_downsample_factor"),
                       &AudioEffectJSFX::get_downsample_factor);
  ADD_PROPERTY(PropertyInfo(Variant::INT, "downsample_factor",
                            PROPERTY_HINT_RANGE, "1,8,1"),
               "set_downsample_factor", "get_downsample_factor");

  ClassDB::bind_method(D_METHOD("set_force_mono", "enabled"),
                       &AudioEffectJSFX::set_force_mono);
  ClassDB::bind_method(D_METHOD("get_force_mono"),
                       &AudioEffectJSFX::get_force_mono);
  ADD_PROPERTY(PropertyInfo(Variant::BOOL, "force_mono"), "set_force_mono",
               "get_force_mono");

  ClassDB::bind_method(D_METHOD("set_auto_reload", "enabled"),
                       &AudioEffectJSFX::set_auto_reload);
  ClassDB::bind_method(D_METHOD("get_auto_reload"),
                       &AudioEffectJSFX::get_auto_reload);
  ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_reload"), "set_auto_reload",
               "get_auto_reload");

  ClassDB::bind_method(D_METHOD("set_slider", "index", "value"),
                       &AudioEffectJSFX::set_slider);
  ClassDB::bind_method(D_METHOD("get_slider", "index"),
                       &AudioEffectJSFX::get_slider);
  ClassDB::bind_method(D_METHOD("get_processed_frames"),
                       &AudioEffectJSFX::get_processed_frames);
  ClassDB::bind_method(D_METHOD("reload_script"),
                       &AudioEffectJSFX::reload_script);

  // Editor file-watch target; internal, not intended for scripts.
  ClassDB::bind_method(D_METHOD("_poll_script_file"),
                       &AudioEffectJSFX::_poll_script_file);
}

AudioEffectJSFX::LoadedScript AudioEffectJSFX::build_script(
    const String& path, bool preserve_values) {
  LoadedScript loaded;
  loaded.values.fill(0.0);
  loaded.set.fill(false);
  if (path.is_empty()) return loaded;

  const String source = FileAccess::get_file_as_string(path);
  const Error open_error = FileAccess::get_open_error();
  if (open_error != OK) {
    ERR_PRINT(String("JSFX: cannot open '") + path + "': " +
              UtilityFunctions::error_string(
                  static_cast<int64_t>(open_error)));
    return loaded;
  }

  const CharString source_utf8 = source.utf8();
  const CharString path_utf8 = path.utf8();
  auto parsed = jsfx::JsfxDocument::parse(
      std::string_view(source_utf8.get_data()),
      std::string(path_utf8.get_data()), make_import_resolver(path));
  if (!parsed.ok()) {
    report_script_error("parse error", path, parsed.error());
    return loaded;
  }
  for (const jsfx::JsfxWarning& warning : parsed.value().warnings()) {
    WARN_PRINT(String("JSFX '") + path + "' line " +
               String::num_int64(warning.line) + ": " + warning.message.c_str());
  }

  loaded.document = std::move(parsed.value());
  loaded.has_document = true;

  for (const jsfx::JsfxSlider& slider : loaded.document.sliders()) {
    const auto slot = static_cast<std::size_t>(slider.index - 1);
    loaded.values[slot] = slider.default_value;
    if (preserve_values && slider_set_[slot]) {
      loaded.values[slot] = slider_values_[slot];
      loaded.set[slot] = true;
    }
  }

  loaded.sample_rate = effective_sample_rate();
  jsfx::JsfxEnvironment environment;
  environment.sample_rate = loaded.sample_rate;
  auto compiled = jsfx::JsfxProcessor::create(loaded.document, environment);
  if (!compiled.ok()) {
    report_script_error("compile error", path, compiled.error());
    return loaded;
  }

  loaded.prepared = std::move(compiled.value());
  apply_slider_values(*loaded.prepared, loaded.document, loaded.values,
                      loaded.set);
  return loaded;
}

void AudioEffectJSFX::set_script_path(const String& path) {
  script_path_ = path;

  if (!on_main_thread()) {
    // Godot's threaded ResourceFormatLoader calls this on a worker. Remember
    // the path and parse/compile on the first main-thread access (PRD 4.3).
    pending_slider_set_.fill(false);
    pending_load_.store(true, std::memory_order_release);
    document_ = jsfx::JsfxDocument{};
    has_document_ = false;
    prepared_processor_.reset();
    prepared_sample_rate_ = 0.0;
    slider_values_.fill(0.0);
    slider_set_.fill(false);
    return;
  }

  pending_slider_set_.fill(false);
  load_script_now(false, true);

  // If the resource is already live on a bus, apply the new script through the
  // same safe swap path used by hot reload.
  bool has_instances = false;
  {
    std::lock_guard<std::mutex> lock(instances_mutex_);
    has_instances = !instances_.empty();
  }
  if (has_instances && has_document_ && prepared_processor_ != nullptr) {
    reload_script();
  }
}

void AudioEffectJSFX::load_script_now(bool preserve_values, bool notify) {
  pending_load_.store(false, std::memory_order_release);

  LoadedScript loaded = build_script(script_path_, preserve_values);

  // Values written to dynamic properties by a worker-side thread load arrive
  // before the document exists; merge them now that it is available.
  for (int index = 1; index <= jsfx::JsfxDocument::kMaxSliders; ++index) {
    const auto slot = static_cast<std::size_t>(index - 1);
    if (!pending_slider_set_[slot]) continue;
    pending_slider_set_[slot] = false;
    if (loaded.document.find_slider(index) == nullptr) continue;

    loaded.values[slot] = pending_slider_values_[slot];
    loaded.set[slot] = true;
    if (loaded.prepared != nullptr) {
      loaded.prepared->set_slider(index, loaded.values[slot]);
    }
  }

  document_ = std::move(loaded.document);
  has_document_ = loaded.has_document;
  slider_values_ = loaded.values;
  slider_set_ = loaded.set;
  prepared_processor_ = std::move(loaded.prepared);
  prepared_sample_rate_ = loaded.sample_rate;
  script_modified_time_ =
      script_path_.is_empty() ? 0
                              : FileAccess::get_modified_time(script_path_);

  if (notify) {
    emit_changed();
    notify_property_list_changed();
  }
  ensure_file_watch();
}

void AudioEffectJSFX::ensure_script_loaded() const {
  if (!pending_load_.load(std::memory_order_acquire)) return;
  if (!on_main_thread()) return;
  const_cast<AudioEffectJSFX*>(this)->load_script_now(false, false);
}

String AudioEffectJSFX::get_script_path() const { return script_path_; }

bool AudioEffectJSFX::reload_script() {
  if (!on_main_thread()) {
    ERR_PRINT("JSFX: reload_script must run on the main thread");
    return false;
  }
  ensure_script_loaded();
  if (script_path_.is_empty()) return false;

  LoadedScript loaded = build_script(script_path_, true);
  script_modified_time_ = FileAccess::get_modified_time(script_path_);
  if (!loaded.has_document || loaded.prepared == nullptr) {
    // Keep the currently running script; the error was logged above.
    return false;
  }

  struct Replacement {
    AudioEffectJSFXInstance* instance = nullptr;
    std::unique_ptr<jsfx::JsfxProcessor> processor;
  };

  std::vector<Replacement> replacements;
  {
    std::lock_guard<std::mutex> lock(instances_mutex_);
    replacements.reserve(instances_.size());

    jsfx::JsfxEnvironment environment;
    environment.sample_rate = loaded.sample_rate;
    for (AudioEffectJSFXInstance* instance : instances_) {
      auto compiled =
          jsfx::JsfxProcessor::create(loaded.document, environment);
      if (!compiled.ok()) {
        report_script_error("compile error", script_path_, compiled.error());
        return false;
      }
      apply_slider_values(*compiled.value(), loaded.document, loaded.values,
                          loaded.set);
      replacements.push_back(
          {instance, std::move(compiled.value())});
    }

    // All replacements compiled; commit the resource state and swap. Keep the
    // prepared processor only when no instance exists to receive it; each
    // one reserves the full EEL2 memory window.
    document_ = std::move(loaded.document);
    has_document_ = true;
    slider_values_ = loaded.values;
    slider_set_ = loaded.set;
    if (instances_.empty()) {
      prepared_processor_ = std::move(loaded.prepared);
      prepared_sample_rate_ = loaded.sample_rate;
    } else {
      prepared_processor_.reset();
      prepared_sample_rate_ = 0.0;
    }

    for (Replacement& replacement : replacements) {
      replacement.instance->swap_processor(std::move(replacement.processor));
    }
  }

  emit_changed();
  notify_property_list_changed();
  ensure_file_watch();
  return true;
}

void AudioEffectJSFX::set_auto_reload(bool enabled) {
  if (enabled == auto_reload_) return;
  auto_reload_ = enabled;
  if (auto_reload_) ensure_file_watch();
  emit_changed();
}

bool AudioEffectJSFX::get_auto_reload() const { return auto_reload_; }

void AudioEffectJSFX::ensure_file_watch() {
  if (!on_main_thread()) return;
  if (file_watch_connected_ || !auto_reload_ || script_path_.is_empty()) {
    return;
  }

  Engine* engine = Engine::get_singleton();
  if (engine == nullptr || !engine->is_editor_hint()) return;

  SceneTree* tree = Object::cast_to<SceneTree>(engine->get_main_loop());
  if (tree == nullptr) return;

  const Callable callback(this, "_poll_script_file");
  if (tree->connect("process_frame", callback) == OK) {
    file_watch_connected_ = true;
  }
}

void AudioEffectJSFX::_poll_script_file() {
  if (!auto_reload_ || script_path_.is_empty()) return;
  if (++poll_tick_ < 15) return;
  poll_tick_ = 0;

  const std::uint64_t modified = FileAccess::get_modified_time(script_path_);
  if (modified == 0 || modified == script_modified_time_) return;
  reload_script();
}

double AudioEffectJSFX::effective_sample_rate() const {
  // A downsampled script runs @sample every `factor` frames, so it must see
  // the reduced rate or its filters, delays and LFOs are off by that factor.
  return current_mix_rate() / downsample_factor_;
}

void AudioEffectJSFX::set_downsample_factor(int factor) {
  factor = std::clamp(factor, 1, jsfx::JsfxProcessor::kMaxDownsampleFactor);
  if (factor == downsample_factor_) return;
  downsample_factor_ = factor;

  // Live instances need contexts initialized at the new rate; @init must run
  // on the main thread, so rebuild them through the hot-reload swap.
  bool has_instances = false;
  {
    std::lock_guard<std::mutex> lock(instances_mutex_);
    has_instances = !instances_.empty();
  }
  if (has_instances && has_document_ && on_main_thread()) reload_script();

  {
    std::lock_guard<std::mutex> lock(instances_mutex_);
    for (AudioEffectJSFXInstance* instance : instances_) {
      instance->set_downsample_factor(factor);
    }
  }
  emit_changed();
}

int AudioEffectJSFX::get_downsample_factor() const {
  return downsample_factor_;
}

void AudioEffectJSFX::set_force_mono(bool enabled) {
  if (enabled == force_mono_) return;
  force_mono_ = enabled;

  {
    std::lock_guard<std::mutex> lock(instances_mutex_);
    for (AudioEffectJSFXInstance* instance : instances_) {
      instance->set_force_mono(enabled);
    }
  }
  emit_changed();
}

bool AudioEffectJSFX::get_force_mono() const { return force_mono_; }

bool AudioEffectJSFX::set_slider(int index, double value) {
  ensure_script_loaded();
  if (!has_document_ || index < 1 ||
      index > jsfx::JsfxDocument::kMaxSliders) {
    return false;
  }
  if (!std::isfinite(value) || document_.find_slider(index) == nullptr) {
    return false;
  }

  const auto slot = static_cast<std::size_t>(index - 1);
  slider_values_[slot] = value;
  slider_set_[slot] = true;

  bool pushed = true;
  {
    std::lock_guard<std::mutex> lock(instances_mutex_);
    for (AudioEffectJSFXInstance* instance : instances_) {
      pushed = instance->push_slider(index, value) && pushed;
    }
  }
  return pushed;
}

double AudioEffectJSFX::get_slider(int index) const {
  ensure_script_loaded();
  if (!has_document_ || index < 1 ||
      index > jsfx::JsfxDocument::kMaxSliders ||
      document_.find_slider(index) == nullptr) {
    return 0.0;
  }
  return slider_values_[static_cast<std::size_t>(index - 1)];
}

int64_t AudioEffectJSFX::get_processed_frames() const {
  int64_t total = 0;
  std::lock_guard<std::mutex> lock(instances_mutex_);
  for (const AudioEffectJSFXInstance* instance : instances_) {
    total += static_cast<int64_t>(instance->processed_frames());
  }
  return total;
}

Ref<AudioEffectInstance> AudioEffectJSFX::_instantiate() {
  Ref<AudioEffectJSFX> self(this);

  // PRD Story 4.3: parsing/compilation must never happen on the audio thread
  // (or inside the Web AudioWorklet). Godot calls _instantiate on the main
  // thread; if a host violates that, fail closed with an unregistered
  // passthrough instance instead of compiling here.
  if (!on_main_thread()) {
    ERR_PRINT("JSFX: effect instantiation must run on the main thread");
    return Ref<AudioEffectInstance>(memnew(AudioEffectJSFXInstance(
        Ref<AudioEffectJSFX>(), nullptr, downsample_factor_, force_mono_)));
  }

  ensure_script_loaded();
  if (!has_document_) {
    return Ref<AudioEffectInstance>(memnew(AudioEffectJSFXInstance(
        self, nullptr, downsample_factor_, force_mono_)));
  }

  jsfx::JsfxEnvironment environment;
  const double mix_rate = effective_sample_rate();
  environment.sample_rate = mix_rate;

  std::unique_ptr<jsfx::JsfxProcessor> processor;
  if (prepared_processor_ != nullptr && prepared_sample_rate_ == mix_rate) {
    processor = std::move(prepared_processor_);
    prepared_sample_rate_ = 0.0;
  } else {
    prepared_processor_.reset();
    prepared_sample_rate_ = 0.0;

    auto compiled = jsfx::JsfxProcessor::create(document_, environment);
    if (!compiled.ok()) {
      report_script_error("compile error", script_path_, compiled.error());
      return Ref<AudioEffectInstance>(memnew(AudioEffectJSFXInstance(
          self, nullptr, downsample_factor_, force_mono_)));
    }
    processor = std::move(compiled.value());
    apply_slider_values(*processor, document_, slider_values_, slider_set_);
  }

  AudioEffectJSFXInstance* raw_instance = memnew(AudioEffectJSFXInstance(
      self, std::move(processor), downsample_factor_, force_mono_));
  Ref<AudioEffectInstance> instance(raw_instance);

  // A resource may have been modified before it was added to a bus; make sure
  // the first processing block sees the current slider values.
  for (int index = 1; index <= jsfx::JsfxDocument::kMaxSliders; ++index) {
    const auto slot = static_cast<std::size_t>(index - 1);
    if (slider_set_[slot]) raw_instance->push_slider(index, slider_values_[slot]);
  }

  return instance;
}

void AudioEffectJSFX::register_instance(AudioEffectJSFXInstance* instance) {
  std::lock_guard<std::mutex> lock(instances_mutex_);
  instances_.push_back(instance);
}

void AudioEffectJSFX::unregister_instance(AudioEffectJSFXInstance* instance) {
  std::lock_guard<std::mutex> lock(instances_mutex_);
  instances_.erase(std::remove(instances_.begin(), instances_.end(), instance),
                   instances_.end());
}

bool AudioEffectJSFX::_set(const StringName& p_name, const Variant& p_value) {
  const Variant::Type type = p_value.get_type();
  if (type != Variant::FLOAT && type != Variant::INT) return false;

  const double value = static_cast<double>(p_value);
  if (!std::isfinite(value)) return false;

  if (!on_main_thread()) {
    // Dynamic property written by a worker-side .tres/.tscn load. Keep it until
    // the document is available on the main thread.
    int index = 0;
    if (!parse_slider_property_index(p_name, index)) return false;
    const auto slot = static_cast<std::size_t>(index - 1);
    pending_slider_values_[slot] = value;
    pending_slider_set_[slot] = true;
    return true;
  }

  ensure_script_loaded();
  const jsfx::JsfxSlider* slider = find_slider_by_property(p_name);
  if (slider == nullptr) return false;
  return set_slider(slider->index, value);
}

bool AudioEffectJSFX::_get(const StringName& p_name, Variant& r_value) const {
  ensure_script_loaded();
  const jsfx::JsfxSlider* slider = find_slider_by_property(p_name);
  if (slider == nullptr) return false;
  const double value = get_slider(slider->index);
  if (slider_is_enum(*slider) || slider->is_file) {
    r_value = static_cast<int64_t>(std::llround(value));
  } else {
    r_value = value;
  }
  return true;
}

bool AudioEffectJSFX::_property_can_revert(const StringName& p_name) const {
  return find_slider_by_property(p_name) != nullptr;
}

bool AudioEffectJSFX::_property_get_revert(const StringName& p_name,
                                           Variant& r_property) const {
  const jsfx::JsfxSlider* slider = find_slider_by_property(p_name);
  if (slider == nullptr) return false;
  if (slider_is_enum(*slider) || slider->is_file) {
    r_property = static_cast<int64_t>(std::llround(slider->default_value));
  } else {
    r_property = slider->default_value;
  }
  return true;
}

void AudioEffectJSFX::_get_property_list(List<PropertyInfo>* p_list) const {
  ensure_script_loaded();
  if (!has_document_) return;

  for (const jsfx::JsfxSlider& slider : document_.sliders()) {
    Variant::Type type = Variant::FLOAT;
    PropertyHint hint = PROPERTY_HINT_NONE;
    String hint_string;
    if (slider.is_file) {
      // File chooser sliders select by index; REAPER's file list is not here.
      type = Variant::INT;
    } else if (slider_is_enum(slider)) {
      type = Variant::INT;
      hint = PROPERTY_HINT_ENUM;
      for (std::size_t i = 0; i < slider.enum_labels.size(); ++i) {
        // ',' and ':' delimit Godot enum hints.
        const String label = String::utf8(slider.enum_labels[i].c_str())
                                 .replace(",", " ")
                                 .replace(":", " ");
        const auto value = static_cast<int64_t>(
            slider.min_value + slider.step * static_cast<double>(i));
        if (i > 0) hint_string += ",";
        hint_string += label + String(":") + String::num_int64(value);
      }
    } else if (slider.has_range && slider.min_value != slider.max_value) {
      // Godot needs min < max; REAPER also allows inverted sliders.
      hint = PROPERTY_HINT_RANGE;
      hint_string = String::num(std::min(slider.min_value, slider.max_value)) +
                    "," +
                    String::num(std::max(slider.min_value, slider.max_value));
      hint_string += String(",") +
                     (slider.step > 0.0 ? String::num(slider.step) : String("0.0001"));
      // Log-shaped sliders (`:log`) get an exponential editor curve, which
      // Godot only accepts for positive ranges.
      if (slider.logarithmic &&
          std::min(slider.min_value, slider.max_value) > 0.0) {
        hint_string += ",exp";
      }
    }

    p_list->push_back(PropertyInfo(type, StringName(slider.property_name.c_str()),
                                   hint, hint_string, PROPERTY_USAGE_DEFAULT));
  }
}

const jsfx::JsfxSlider* AudioEffectJSFX::find_slider_by_property(
    const StringName& property) const {
  if (!has_document_) return nullptr;

  for (const jsfx::JsfxSlider& slider : document_.sliders()) {
    if (StringName(slider.property_name.c_str()) == property) return &slider;
  }
  return nullptr;
}

}  // namespace jsfx_godot
