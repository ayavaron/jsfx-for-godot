# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

JSFX for Godot: a GDExtension for Godot 4.x that loads and runs Reaper JSFX (WDL/EEL2) inside Godot's `AudioServer`, exposing JSFX sliders as native Godot properties for gameplay automation.

All three PRD phases are implemented and public as v0.1.0. `jsfx_to_godot_prd.md` is the original spec; where the code deliberately departs from it (lazy OS-backed EEL2 memory, `force_mono` not saving CPU, downsampled scripts seeing the reduced `srate`), README.md documents the shipped behavior.

## Build and test status

- `jsfx_core` is the Godot-free static library: a REAPER-lenient parser (named sliders, up to 256 sliders, `:log` shapes, file sliders, repeated sections, `import` through a resolver callback; `@gfx`, `@serialize` and unknown sections stripped), vendored WDL EEL2, the JSFX host API in `src/core/jsfx_host.c` (`slider()`, `spl()`, FFT family, MIDI/automation no-ops, `time_precise`), float-buffer processing with FTZ/DAZ, the lock-free parameter queue, lo-fi modes, and 26 Catch2 tests.
- `tools/jsfx_bench.cpp` (`jsfx_bench`) runs any set of `.jsfx` files for compatibility, CPU per block and (`--memory N`) per-instance memory. A stock REAPER install's `InstallData/Effects` is the compatibility corpus: 234 of 263 load (string functions, `file_open`, `<? ?>` and `mdct` are the remaining gaps).
- Phase 2 bridge is implemented in `src/godot/`: `AudioEffectJSFX`, `AudioEffectJSFXInstance`, per-instance EEL2 contexts, slider broadcast, and passthrough on failure. godot-cpp is pinned to `godot-4.3-stable` via FetchContent (or `JSFX_GODOT_CPP_SOURCE_DIR`).
- Phase 3 is implemented: dynamic slider properties (`_get_property_list`/`_get`/`_set`, range hints, `.tres` serialization), `AudioEffectJSFXLoader` (`.jsfx` ResourceFormatLoader), and hot reload (`reload_script`, editor mtime poll behind `auto_reload`, atomic per-instance processor swaps with deferred reclamation).
- Core tests: `cmake --preset windows-debug`, `cmake --build --preset windows-debug`, `ctest --preset windows-debug` (or `windows-release`). No Godot needed.
- GDExtension: `cmake --preset windows-godot-debug`, `cmake --build --preset windows-godot-debug` from a Developer PowerShell (Ninja is single-config so godot-cpp and the bridge share the CRT). Output goes to `demo/bin/windows-debug/`.
- Windows builds link the MSVC runtime statically (`CMAKE_MSVC_RUNTIME_LIBRARY`, and godot-cpp's hard-coded `/MD` is rewritten), so the DLL imports only KERNEL32.
- Godot editor binaries load the **debug** library (`demo/bin/windows-debug/`). Rebuild `windows-godot-debug` before testing in the editor, or you test stale code.
- Godot verification requires a 4.3+ editor: `godot --headless --path demo --editor --quit-after 300` (one-time import; plain `--quit` can crash Godot 4.7 while it is still generating docs for a newly discovered extension), then `godot --headless --path demo --script res://tests/extension_smoke.gd` must print `JSFX_SMOKE_OK`. This covers registration, the loader, dynamic properties/serialization, instantiation, hot reload and teardown; actual audio output needs a non-headless run.
- Real audio: `godot --headless --audio-driver WASAPI --path demo --script res://tests/audio_probe.gd` must print `AUDIO_PROBE_OK` (accuracy via AudioEffectCapture, hot reload under load, downsampled `srate`, 17 live instances).
- GUT: run `tools/fetch_gut.ps1` (v9.7.1 by default; `-Tag v9.3.0` for Godot 4.3/4.4), re-import the demo (`--editor --quit-after 300`) so GUT's global classes register, then `godot --headless --path demo -s addons/gut/gut_cmdln.gd -gdir=res://tests/unit -gexit`. GUT is gitignored.
- Keep the native core decoupled from godot-cpp so Catch2 tests run headless. GUT/runtime tests require Godot 4.3+ and are not part of the CTest suite.
- C++20; godot-cpp targeting Godot 4.3+. x86-64 desktop builds use the WDL EEL2 x64 JIT by default: MSVC x64 links the vendored prebuilt COFF object, while other x64 toolchains assemble `asm-nseel-x64-sse.asm` with NASM (ELF64, Mach-O, or Win64 COFF). Non-MSVC builds without NASM fall back to the interpreter. Web, iOS, ARM and universal macOS builds use `EEL_TARGET_PORTABLE`.
- Web/Wasm is implemented with CMake presets `web-godot-debug`, `web-godot-release`, `web-godot-debug-threads`, `web-godot-release-threads`. They require Emscripten 3.1.64 (matching Godot 4.3's official templates) and a shell where `emsdk_env.bat` has run (the presets read `$env{EMSDK}` for the toolchain file). Outputs go to `demo/bin/web-<config>[-threads]/jsfx_for_godot.wasm`; the non-threaded build uses the plain-ring queue, pthreads uses the lock-free queue. Web link flags include `-sWASM_BIGINT`.
- Browser verification: export with `demo/export_presets.cfg` (`Web` and `WebThreads`), serve with `tools/serve_web.py` (COOP/COEP headers), and run `tools/browser_smoke.py` (Playwright with installed Chrome/Edge). Both modes must print `JSFX_WEB_READY` and `JSFX_WEB_PROCESSED` with a non-silent `peak_db`; threaded mode must also report `crossOriginIsolated=True`. Both modes have been verified in Chrome.
- `_instantiate` and `reload_script` are main-thread guarded; an off-main `set_script_path` (Godot's threaded ResourceFormatLoader) stores the path and defers parse/compile to the first main-thread access. Dynamic slider values from off-main `.tres` loads are queued and merged when the document loads. `_process` never parses or compiles (PRD Story 4.3).
- The NASM Win64 JIT path is covered by the Catch2 suite with GCC/MinGW and the MSVC prebuilt path is also covered; Linux/macOS JIT builds are wired but have not been run on those operating systems from this Windows-only development machine.

## Hard audio constraints

- `AudioEffectInstance._process` runs on the audio thread. No locks, mutexes, allocation, `std::vector::push_back`, or file I/O there.
- Main thread to audio thread parameter changes travel through the lock-free multiple-producer/single-consumer queue; hot reload is an atomic context-pointer swap with deferred deletion of the old context.
- The EEL2 8-million-slot memory is reserved once on the main thread and never resized on the audio thread. On desktop it is one VirtualAlloc/mmap region wired into the VM's block table (`jsfx_host_map_ram`), so pages become resident only when touched; Web falls back to `NSEEL_VM_preallocram`.
- Hot reload reclaims the old processor with an epoch handshake (`process_epoch_` is odd while `_process` runs), never with a timer.
- In Web builds, all parsing, compilation, and `@init` execution finish on the main thread before the context reaches the `AudioWorklet`; the audio thread never parses.
- Sliders are exposed through `_get_property_list` / `_get` / `_set` with `PROPERTY_HINT_RANGE` and are serialized into the scene/resource.
- `downsample_factor` uses zero-order hold and rebuilds contexts at `mix_rate / factor` so scripts see the rate they actually run at. `force_mono` feeds only the left input and copies `spl0` to both outputs (it does not save CPU). Keep 1x stereo behavior exact.

## Conventions

- Keep upstream WDL license headers on vendored code and isolate it from Godot-facing code.
- Put construction, parsing and compilation work on the main thread; audio-thread code stays allocation-free.
- The PRD specifies the product behavior; do not invent alternate architecture. Flag ambiguities before choosing.
