#include "jsfx/eel2_context.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "jsfx/ram_pool.h"
#include "jsfx_host.h"
#include "ns-eel.h"

namespace jsfx {
namespace {

std::string register_name(int index, const char* prefix) {
  char buffer[24];
  std::snprintf(buffer, sizeof(buffer), "%s%d", prefix, index);
  return buffer;
}

int line_from_eel_error(const char* message, int fallback) {
  if (message == nullptr) return fallback;

  int line = 0;
  const char* p = message;
  while (*p >= '0' && *p <= '9') {
    line = line * 10 + (*p - '0');
    ++p;
  }
  if (p == message || *p != ':' || line <= 0) return fallback;
  return line;
}

void register_host_functions_once() {
  // EEL2's function table is process-global; fill it before any compile.
  static std::once_flag once;
  std::call_once(once, [] { jsfx_host_register_functions(); });
}

bool compile_section(NSEEL_VMCTX vm, const JsfxSection& section,
                     JsfxSectionKind kind, int flags, NSEEL_CODEHANDLE* handle,
                     Error& error) {
  *handle = nullptr;
  if (!section.present || section.code.empty()) return true;

  NSEEL_CODEHANDLE compiled = NSEEL_code_compile_ex(
      vm, section.code.c_str(), section.eel_line_offset(), flags);
  if (compiled == nullptr) {
    const char* message = NSEEL_code_getcodeerror(vm);
    // EEL2 returns no handle and no error for code that is only whitespace or
    // comments; REAPER treats that as an empty section.
    if (message == nullptr || *message == '\0') return true;
    error.line = line_from_eel_error(message, section.first_code_line);
    error.message = std::string(to_string(kind)) + ": " +
                    (message != nullptr ? message : "syntax error");
    return false;
  }

  *handle = compiled;
  return true;
}

}  // namespace

struct Eel2Context::Impl {
  NSEEL_VMCTX vm = nullptr;
  void* ram_region = nullptr;  // OS-backed EEL2 memory, see jsfx_host_map_ram

  JsfxHostState host{};
  std::vector<NSEEL_CODEHANDLE> import_code;
  NSEEL_CODEHANDLE init_code = nullptr;
  NSEEL_CODEHANDLE slider_code = nullptr;
  NSEEL_CODEHANDLE block_code = nullptr;
  NSEEL_CODEHANDLE sample_code = nullptr;

  double* srate = nullptr;
  double* tempo = nullptr;
  double* play_state = nullptr;
  double* samplesblock = nullptr;
  double* beat_position = nullptr;
  double* play_position = nullptr;

  std::array<double*, 64> spl{};
  std::array<double*, JsfxDocument::kMaxSliders> sliders{};
  std::array<bool, static_cast<std::size_t>(JsfxSectionKind::Count)> present{};

  ~Impl() {
    // Code handles that may reference functions exported from @init must be
    // destroyed first (see NSEEL_CODE_COMPILE_FLAG_COMMONFUNCS).
    if (sample_code != nullptr) NSEEL_code_free(sample_code);
    if (block_code != nullptr) NSEEL_code_free(block_code);
    if (slider_code != nullptr) NSEEL_code_free(slider_code);
    if (init_code != nullptr) NSEEL_code_free(init_code);
    for (auto it = import_code.rbegin(); it != import_code.rend(); ++it) {
      NSEEL_code_free(*it);
    }
    if (vm != nullptr) {
      jsfx_host_unmap_ram(vm, ram_region, kEelRamSlots);
      NSEEL_VM_free(vm);
    }
  }
};

Result<std::unique_ptr<Eel2Context>> Eel2Context::create(
    const JsfxDocument& document, const JsfxEnvironment& environment) {
  if (!std::isfinite(environment.sample_rate) || environment.sample_rate <= 0.0) {
    return Result<std::unique_ptr<Eel2Context>>::failure(
        "sample_rate must be a positive finite value");
  }
  if (!std::isfinite(environment.tempo) || environment.tempo <= 0.0) {
    return Result<std::unique_ptr<Eel2Context>>::failure(
        "tempo must be a positive finite value");
  }

  register_host_functions_once();

  auto impl = std::make_unique<Impl>();
  impl->vm = NSEEL_VM_alloc();
  if (impl->vm == nullptr) {
    return Result<std::unique_ptr<Eel2Context>>::failure(
        "failed to allocate an EEL2 VM");
  }

  // The PRD requires the EEL2 memory to be sized and allocated on the main
  // thread, then never resized on the audio thread.
  const int actual_slots = NSEEL_VM_setramsize(impl->vm, kEelRamSlots);
  if (actual_slots < kEelRamSlots) {
    return Result<std::unique_ptr<Eel2Context>>::failure(
        "failed to reserve the EEL2 RAM window");
  }
  // Desktop: one lazily backed OS region, so RAM is only used for the slots a
  // script touches. Web: blocks are allocated when first touched, by calloc on
  // the main thread and from the pre-filled pool on the audio thread.
  static_assert(NSEEL_RAM_ITEMSPERBLOCK * sizeof(EEL_F) == ram_pool::kBlockBytes,
                "EEL2 block size changed; update ram_pool::kBlockBytes");
  impl->ram_region = jsfx_host_map_ram(impl->vm, kEelRamSlots);
  if (impl->ram_region == nullptr) {
    ram_pool::note_on_demand_vm();
  } else if (NSEEL_VM_getramptr_noalloc(impl->vm, kEelRamSlots - 1, nullptr) ==
             nullptr) {
    return Result<std::unique_ptr<Eel2Context>>::failure(
        "failed to map the EEL2 RAM window");
  }

  auto register_variable = [&](const char* name) -> double* {
    return NSEEL_VM_regvar(impl->vm, name);
  };

  impl->srate = register_variable("srate");
  impl->tempo = register_variable("tempo");
  impl->play_state = register_variable("play_state");
  impl->samplesblock = register_variable("samplesblock");
  impl->beat_position = register_variable("beat_position");
  impl->play_position = register_variable("play_position");
  if (impl->srate == nullptr || impl->tempo == nullptr ||
      impl->play_state == nullptr || impl->samplesblock == nullptr ||
      impl->beat_position == nullptr || impl->play_position == nullptr) {
    return Result<std::unique_ptr<Eel2Context>>::failure(
        "failed to register JSFX environment variables");
  }

  for (int i = 0; i < 64; ++i) {
    const std::string name = register_name(i, "spl");
    impl->spl[static_cast<std::size_t>(i)] = register_variable(name.c_str());
    if (impl->spl[static_cast<std::size_t>(i)] == nullptr) {
      return Result<std::unique_ptr<Eel2Context>>::failure(
          "failed to register JSFX output variable " + name);
    }
  }

  // Godot effect instances are stereo with a 4/4 transport.
  const std::pair<const char*, double> constants[] = {
      {"num_ch", 2.0}, {"ts_num", 4.0}, {"ts_denom", 4.0}};
  for (const auto& [name, value] : constants) {
    if (double* slot = register_variable(name)) *slot = value;
  }

  for (int i = 0; i < 64; ++i) {
    impl->host.spl[i] = impl->spl[static_cast<std::size_t>(i)];
  }

  for (const JsfxSlider& slider : document.sliders()) {
    // Named sliders (`slider1:gain=0<...>`) live in their own variable.
    const std::string name = slider.variable_name.empty()
                                 ? register_name(slider.index, "slider")
                                 : slider.variable_name;
    double* value = register_variable(name.c_str());
    if (value == nullptr) {
      return Result<std::unique_ptr<Eel2Context>>::failure(
          "failed to register " + name);
    }
    *value = slider.default_value;
    impl->sliders[static_cast<std::size_t>(slider.index - 1)] = value;
    impl->host.sliders[slider.index - 1] = value;
  }
  NSEEL_VM_SetCustomFuncThis(impl->vm, &impl->host);

  *impl->srate = environment.sample_rate;
  *impl->tempo = environment.tempo;
  *impl->play_state = environment.play_state;
  *impl->samplesblock = 0.0;

  for (JsfxSectionKind kind : {JsfxSectionKind::Init, JsfxSectionKind::Slider,
                               JsfxSectionKind::Block, JsfxSectionKind::Sample}) {
    impl->present[static_cast<std::size_t>(kind)] = document.has_section(kind);
  }

  Error error;
  // Imported libraries first, so their functions are visible to this file.
  for (const JsfxImport& imported : document.imports()) {
    if (imported.init_code.empty()) continue;
    NSEEL_CODEHANDLE handle = NSEEL_code_compile_ex(
        impl->vm, imported.init_code.c_str(), 0,
        NSEEL_CODE_COMPILE_FLAG_COMMONFUNCS);
    if (handle == nullptr) {
      const char* message = NSEEL_code_getcodeerror(impl->vm);
      return Result<std::unique_ptr<Eel2Context>>::failure(
          "in import '" + imported.name +
          "': " + (message != nullptr ? message : "syntax error"));
    }
    impl->import_code.push_back(handle);
    NSEEL_code_execute(handle);
  }

  // Functions declared in @init must be visible to the other sections.
  if (!compile_section(impl->vm, document.section(JsfxSectionKind::Init),
                       JsfxSectionKind::Init,
                       NSEEL_CODE_COMPILE_FLAG_COMMONFUNCS, &impl->init_code,
                       error)) {
    return Result<std::unique_ptr<Eel2Context>>::failure(std::move(error));
  }
  if (impl->init_code != nullptr) NSEEL_code_execute(impl->init_code);

  if (!compile_section(impl->vm, document.section(JsfxSectionKind::Slider),
                       JsfxSectionKind::Slider, 0, &impl->slider_code, error) ||
      !compile_section(impl->vm, document.section(JsfxSectionKind::Block),
                       JsfxSectionKind::Block, 0, &impl->block_code, error) ||
      !compile_section(impl->vm, document.section(JsfxSectionKind::Sample),
                       JsfxSectionKind::Sample, 0, &impl->sample_code, error)) {
    return Result<std::unique_ptr<Eel2Context>>::failure(std::move(error));
  }

  // JSFX runs @slider once after @init so that derived state is initialized.
  if (impl->slider_code != nullptr) NSEEL_code_execute(impl->slider_code);

  return Result<std::unique_ptr<Eel2Context>>::success(
      std::unique_ptr<Eel2Context>(new Eel2Context(std::move(impl))));
}

Eel2Context::Eel2Context(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Eel2Context::~Eel2Context() = default;

bool Eel2Context::set_slider(int index, double value) {
  if (impl_ == nullptr || index < 1 || index > JsfxDocument::kMaxSliders) {
    return false;
  }
  double* slot = impl_->sliders[static_cast<std::size_t>(index - 1)];
  if (slot == nullptr) return false;
  *slot = value;
  return true;
}

double Eel2Context::slider(int index) const {
  if (impl_ == nullptr || index < 1 || index > JsfxDocument::kMaxSliders) {
    return 0.0;
  }
  const double* slot = impl_->sliders[static_cast<std::size_t>(index - 1)];
  return slot != nullptr ? *slot : 0.0;
}

bool Eel2Context::set_tempo(double bpm) {
  if (impl_ == nullptr || impl_->tempo == nullptr) return false;
  if (!std::isfinite(bpm) || bpm <= 0.0) return false;
  *impl_->tempo = bpm;
  return true;
}

bool Eel2Context::set_play_state(int state) {
  if (impl_ == nullptr || impl_->play_state == nullptr) return false;
  *impl_->play_state = static_cast<double>(state);
  return true;
}

void Eel2Context::run_block(int frames) {
  if (impl_ == nullptr) return;
  frames = frames > 0 ? frames : 0;
  *impl_->samplesblock = static_cast<double>(frames);
  if (impl_->block_code != nullptr) NSEEL_code_execute(impl_->block_code);

  // Advance the transport after @block, which sees the block's start position.
  if (static_cast<int>(*impl_->play_state) & 1) {
    const double seconds = frames / *impl_->srate;
    *impl_->play_position += seconds;
    *impl_->beat_position += seconds * *impl_->tempo / 60.0;
  }
}

void Eel2Context::run_sample() {
  if (impl_ != nullptr && impl_->sample_code != nullptr) {
    NSEEL_code_execute(impl_->sample_code);
  }
}

void Eel2Context::run_slider() {
  if (impl_ != nullptr && impl_->slider_code != nullptr) {
    NSEEL_code_execute(impl_->slider_code);
  }
}

bool Eel2Context::has_section(JsfxSectionKind kind) const {
  if (impl_ == nullptr) return false;
  const auto index = static_cast<std::size_t>(kind);
  if (index >= impl_->present.size()) return false;
  return impl_->present[index];
}

double* Eel2Context::variable(std::string_view name) {
  if (impl_ == nullptr || impl_->vm == nullptr || name.empty()) return nullptr;
  const std::string terminated(name);
  return NSEEL_VM_getvar(impl_->vm, terminated.c_str());
}

}  // namespace jsfx
