# Product Requirements Document: JSFX Integration for Godot 4 (GDExtension)

## 1. Product Overview
**Goal:** Create a robust, production-ready GDExtension plugin for Godot 4.x that allows users to write, import, and execute JSFX (Reaper's WDL/EEL2 DSP scripting language) directly within Godot's AudioServer, with all script parameters exposed natively for on-the-fly, in-game automation. 

**Target Audience:** Godot technical audio designers, DSP engineers, and game developers wanting to port Reaper effects into interactive games and drive them dynamically via gameplay logic.
**Agent Prompting Target:** This PRD is structured for an AI agent (DeepSeek Flash) to generate the C++ backend and GDScript frontend with zero architectural ambiguity.

## 2. Core Architectural Constraints (STRICT)
*   **Audio Thread Safety:** The `_process()` function in `AudioEffectInstance` runs on the Godot Audio Thread. **NO** mutexes, **NO** locks, **NO** dynamic memory allocation (`new`, `malloc`, `std::vector::push_back`), and **NO** File I/O are permitted in this loop.
*   **Parameter Passing:** Communication between the Godot Main Thread (Inspector/GDScript) and the Audio Thread MUST use a Lock-Free Ring Buffer (Multiple-Producer, Single-Consumer).
*   **Language & Standard:** C++20 for the core logic. GDExtension (`godot-cpp`) targeting Godot 4.3+.
*   **Memory Model:** The WDL EEL2 engine utilizes a massive global floating-point array (8 million slots). This must be pre-allocated during `_init()` on the main thread, never resized dynamically on the audio thread.

---

## 3. Epics & Explicit Technical Stories

### Epic 1: EEL2 Core & Parser (Headless C++)
*Objective: Build a decoupled, standalone C++ wrapper around Cockos WDL EEL2.*

*   **Story 1.1: EEL2 Compilation & Platform Macros**
    *   **Explicit:** Vendor the WDL EEL2 source. Set macros `EEL_STRING_DEBUGOUT` to route to a null-sink or custom logger.
    *   **Robustness:** Implement dynamic detection for JIT (Just-In-Time) compilation. If the platform is WebAssembly (HTML5) or iOS, force `EEL_TARGET_PORTABLE` (Interpreter mode) because dynamic executable memory is forbidden on these platforms.
*   **Story 1.2: Strict Section Parser**
    *   **Explicit:** Parse `.jsfx` files via Regex or line-by-line state machine.
    *   **Extraction Targets:** `@init`, `@slider`, `@block`, `@sample`.
    *   **Robustness:** Ignore/strip `@gfx` and `@serialize` blocks entirely to prevent compilation errors, as Godot handles UI and state serialization natively.
*   **Story 1.3: DSP Environment Emulation**
    *   **Explicit:** Expose required JSFX built-in variables to the EEL2 context before execution: `srate` (from Godot AudioServer), `tempo` (from Godot Engine/AudioServer), `play_state`, `spl0`, `spl1` (up to `spl63`).

### Epic 2: GDExtension Audio Bridge (Godot <-> JSFX)
*Objective: Map Godot's audio processing pipeline to the EEL2 engine.*

*   **Story 2.1: `AudioEffectJSFX` Resource**
    *   **Explicit:** Inherit from `AudioEffect`. Expose `script_path` (String). Upon setting the script, load the file, parse it, and generate the EEL2 execution context.
*   **Story 2.2: The Lock-Free Parameter Queue**
    *   **Explicit:** Implement `moodycamel::ReaderWriterQueue` or a custom `std::atomic`-based circular buffer to pass `SliderUpdate(index, value)` structs from `_set()` to `_process()`.
*   **Story 2.3: `AudioEffectInstance` Block & Sample Loop**
    *   **Explicit Routine:**
        1. Dequeue all pending slider updates and apply to JSFX memory.
        2. If sliders changed, call the `@slider` execution context.
        3. Set `samplesblock` variable. Call the `@block` execution context.
        4. Loop through Godot's `AudioFrame* src` and `AudioFrame* dst`:
            * Extract `src[i].left` to `spl0`, `src[i].right` to `spl1`.
            * Call the `@sample` execution context.
            * Write `spl0` to `dst[i].left`, `spl1` to `dst[i].right`.

### Epic 3: Godot Editor UX, Automation, & Asset Pipeline
*Objective: Make JSFX files feel like native Godot resources that are deeply integrated into gameplay logic.*

*   **Story 3.1: `.jsfx` Resource Importer**
    *   **Explicit:** Create a C++ `ResourceFormatLoader` or GDScript `EditorImportPlugin` that identifies `.jsfx`. 
    *   **Robustness:** If EEL2 compilation fails during import, the importer MUST fail gracefully, print the specific EEL2 line error to `ERR_PRINT`, and return a dummy pass-through audio effect to prevent engine crashes.
*   **Story 3.2: Dynamic Property Exposure for GDScript & Gameplay Automation**
    *   **Explicit Need:** A game developer must be able to change DSP sounds on the fly exactly like DAW automation, driven by the game's UI, physics, logic, or animations.
    *   **Implementation:** Override `_get_property_list()`, `_get()`, and `_set()`. Parse JSFX headers (e.g., `slider1:0.5<0,1,0.01>Volume`) and map them directly to standard Godot properties (e.g., `slider_1_volume`). Push a `PropertyInfo` struct with `PROPERTY_HINT_RANGE`.
    *   **Engine Integration:** By exposing these as standard properties, they are automatically interpolatable by the `AnimationPlayer`, drivable via `Tween`, and fully accessible from GDScript (e.g., `effect.set("slider_1_volume", player_health / 100.0)`).
    *   **Serialization:** Ensure `_set()` and `_get()` handle these dynamically generated names so Godot saves the tweaked slider values into the `.tres` or `.tscn` file.
*   **Story 3.3: Safe Hot-Reloading**
    *   **Explicit Mechanism (Atomic Pointer Swap):** When a `.jsfx` file is saved externally:
        1. Main thread compiles a *new* EEL2 context.
        2. Main thread sets an `std::atomic<EEL2Context*>` pointer.
        3. Audio thread reads the new pointer at the start of the next audio block.
        4. Main thread safely deletes the old context after a deferred frame delay (garbage collection).

### Epic 4: WebAssembly (Wasm) Survival & Fallbacks
*Objective: Ensure the plugin compiles and runs on HTML5 exports despite extreme memory and threading limitations in web browsers.*

*   **Story 4.1: Single-Threaded Fallback (No SharedArrayBuffer)**
    *   **Explicit Need:** Web hosts without strict COOP/COEP headers disable `SharedArrayBuffer`, breaking `std::atomic` and standard multi-threading. Godot will run in single-threaded mode where the audio mixes synchronously with the main loop.
    *   **Implementation:** Wrap the Lock-Free Queue (Epic 2.2) and Atomic Pointer Swap (Epic 3.3) in `#ifdef __EMSCRIPTEN_PTHREADS__` checks. If compiling for single-threaded Wasm, fall back to a simple `std::queue` or direct pointer reassignment, as race conditions are impossible on a single thread. 
*   **Story 4.2: Emscripten Memory Growth & Heap Limits**
    *   **Explicit Need:** JSFX's 8-million-slot array is ~64MB alone. Standard Wasm exports often crash with "OOM (Out of Memory)" upon initialization of the plugin.
    *   **Implementation:** In the `SCons` or `CMake` build files targeting Emscripten, inject the linker flags: `-s ALLOW_MEMORY_GROWTH=1` and `-s INITIAL_MEMORY=134217728` (128MB) to guarantee enough heap space for the engine to initialize the JSFX virtual memory without crashing the browser tab.
*   **Story 4.3: AudioWorklet Processor Adherence**
    *   **Explicit Need:** Ensure Godot's web audio backend (`AudioWorklet`) doesn't get blocked. 
    *   **Implementation:** No heavy JSFX parsing or `@init` blocks should ever fire on the audio thread in Web builds. The C++ code MUST strictly enforce that *all* parsing happens on the main thread and is fully completed before handing the context pointer to the AudioWorklet.

### Epic 5: Performance Scaling & Lo-Fi Execution Modes
*Objective: Provide drastic CPU optimizations for low-end hardware (Mobile/Web) or background game elements by intentionally reducing DSP fidelity.*

*   **Story 5.1: Processing Rate Decimation (Downsampling)**
    *   **Explicit Need:** Running `@sample` 48,000 times per second per voice is expensive.
    *   **Implementation:** Expose a `downsample_factor` property (e.g., 1x, 2x, 4x) on the `AudioEffectJSFX`. In the `_process` loop, if set to 2x, only call the EEL2 `@sample` execution context on even frames. On odd frames, duplicate the output from the previous frame (Zero-Order Hold). This halves the CPU usage immediately at the cost of high-frequency fidelity.
*   **Story 5.2: Forced Mono Processing**
    *   **Explicit Need:** Many game sound effects (explosions, dialogue) are spatialized in 3D space, meaning stereo processing inside the DSP is wasted before it hits the panner.
    *   **Implementation:** Expose a `force_mono` boolean property. When true, bypass feeding `src[i].right` into `spl1`. Run the `@sample` context once for `spl0`, and then assign `dst[i].right = spl0`. This reduces computational load by ~50% for stereo scripts.

### Epic 6: Automated Testing Strategy
*Objective: Guarantee mathematical accuracy and prevent regressions.*

*   **Story 6.1: Catch2 Core DSP Tests**
    *   **Explicit:** Mock an audio buffer with a 1kHz sine wave. Pass it through a simple Gain JSFX script. Assert that the output RMS exactly matches the input RMS multiplied by the gain factor ($\epsilon = 10^{-6}$).
    *   **Explicit:** Test memory boundary conditions (accessing array index 8,000,001) to ensure the EEL2 VM does not segfault.
*   **Story 6.2: Godot Unit Tests (GUT) & Automation Stress Testing**
    *   **Explicit:** Write GDScript tests that:
        1. Apply an `AudioEffectJSFX` to a bus.
        2. Automate a slider from 0.0 to 1.0 rapidly via `Tween` to simulate intense gameplay parameter modulation.
        3. Verify the engine does not stall, lock, or deadlock during rapid parameter spew from the main thread to the audio thread.

## 7. Development Phases for AI Assistant
1.  **Phase 1 (Headless):** Implement Epic 1 + Epic 6.1 (Catch2). The deliverable is a static C++ library that compiles JSFX text and processes a dummy float array. **Target: Desktop + Web (Interpreter).**
2.  **Phase 2 (Bridge):** Implement Epic 2, Epic 4, and Epic 5 (Performance). Connect the static library to `godot-cpp`. Build the fallback paths for single-threaded Wasm. Ensure audio plays in Godot.
3.  **Phase 3 (Editor):** Implement Epic 3. Map properties to `AnimationPlayer`/GDScript, build the importer, and handle hot-reloading state swaps safely.