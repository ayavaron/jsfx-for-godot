// JSFX features layered on WDL EEL2: strings (eel_strings.h), printf, the
// file_open() family, mdct (eel_mdct.h) and the `<? ?>` preprocessor
// (eel_pproc.h). Internal to the core; compiled with the vendored WDL code.
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "jsfx/file_data.h"

struct JsfxHostState;

namespace jsfx::ext {

/// A file slider's folder listing, fixed when the context is created.
struct FileSlider {
  int index = 0;            ///< 1-based slider index.
  std::string folder;       ///< e.g. "amp_models"
  std::vector<std::string> names;
};

struct State;

/// Registers strings, printf, the file API and mdct in EEL2's global function
/// table and pre-builds mdct's tables. Call once, on the main thread, before
/// the first compile.
void register_functions();

/// Per-VM state. `host` must outlive it; `cache` may be shared by contexts of
/// the same effect; `provider` may be null (only cached files then open).
State* create_state(void* vm, JsfxHostState* host,
                    std::shared_ptr<JsfxFileCache> cache,
                    std::shared_ptr<JsfxFileProvider> provider,
                    std::vector<FileSlider> file_sliders);
void destroy_state(State* state);

/// Call after compiling code, so printf's %{name} formats can see variables.
void after_compile(State* state, void* vm);

/// Main thread: appends text the script printed (printf) since the last call.
/// Returns true if anything was appended.
bool drain_log(State* state, std::string& out);

/// Main thread: the most recent file the audio thread could not open because
/// it was not preloaded. Returns false if there is none.
bool take_missed_file(State* state, std::string& out);

/// Runs JSFX's `<? ?>` preprocessor over a whole file. `config:` lines define
/// variables at their default values first, as REAPER does. `line_map`, if
/// given, receives the original line for each output line (1-based, index 0
/// is output line 1). Returns false and sets `error` on failure.
bool preprocess(std::string_view source, std::string& out,
                std::vector<int>* line_map, std::string& error);

}  // namespace jsfx::ext
