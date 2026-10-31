# JSFX for Godot

Run REAPER's **JSFX** audio effects inside **Godot 4**, and drive their
parameters from gameplay.

JSFX is the text-based DSP scripting language REAPER uses for its built-in
and community effects: compressors, filters, delays, distortion, spectral
tools and more, written in Cockos' EEL2 language. This GDExtension compiles
`.jsfx` files with the real EEL2 engine (the x64 JIT on desktop) and runs them
as ordinary Godot `AudioEffect`s on your audio buses, or plays JSFX synths as
instruments you send MIDI notes to from GDScript. Each JSFX slider becomes a
Godot property, so you can automate it with `Tween`, `AnimationPlayer` or one
line of GDScript.

**[Play the live demo](https://ayavaron.github.io/jsfx-for-godot/)**: three
JSFX instruments layered over three JSFX effects in a Godot Web export, with
Rossini's *Barber of Seville* overture as a demo track to tweak while it
plays. It is about a 54 MB download, almost all of it Godot's engine, and has
been tested in Chrome.

```gdscript
var lowpass := load("res://effects/lowpass.jsfx") as AudioEffectJSFX
AudioServer.add_bus_effect(AudioServer.get_bus_index("Music"), lowpass)

# Muffle the music as the player's health drops.
lowpass.slider_1_cutoff_hz = lerpf(400.0, 18000.0, health / max_health)
```

## Who it's for

- **Game audio designers** who already build sounds in REAPER and want the
  same effects reacting to gameplay in Godot, without porting DSP to C++.
- **DSP tinkerers** who want a fast scripting loop: edit a `.jsfx` file, save,
  and hear the change in the running editor (hot reload).
- **Godot developers** who need effects Godot doesn't ship (bit crushers,
  waveshapers, resonant filters, custom dynamics) and would rather write 20
  lines of EEL2 than a native plug-in.
- **Procedural and interactive music**: play a JSFX synth from gameplay code,
  positioned in 2D or 3D space like any other sound.

It is not a REAPER host: there's no JSFX graphics (`@gfx`), MIDI routing
between effects, or file browser. See [Limitations](#limitations).

## Features

- **`AudioEffectJSFX` resource.** Put it on any bus like a built-in effect, in
  the inspector or from code. `.jsfx` files load directly as resources
  (`load("res://foo.jsfx")`).
- **Sliders become properties.** `slider1:gain_db=0<-60,12,0.1>Gain (dB)`
  becomes `slider_1_gain_db`, a `float` with a range hint. Enum sliders
  (`{Sine,Triangle,Square}`) become dropdowns, `:log` sliders get an
  exponential editor curve, and every slider has revert-to-default. Values
  save into `.tres`/`.tscn` files and animate with `Tween` and
  `AnimationPlayer`.
- **Real EEL2.** The vendored Cockos WDL engine with the x64 JIT on desktop and
  the portable interpreter on Web, ARM and iOS. Supported: `@init`, `@slider`,
  `@block`, `@sample`, user functions, the 8M-slot memory model, `import` of
  `.jsfx-inc` libraries, and the JSFX host API: `slider()`, `spl()`, `fft`/
  `ifft`/`fft_real`/`convolve_c`, `time_precise()`, `srate`, `tempo`,
  `beat_position`, `num_ch` and more.
- **Hot reload.** Save a `.jsfx` file and the editor recompiles it within a
  quarter second, swapping the running effect and keeping your slider
  values. A script that fails to compile logs the EEL2 error with
  its line number and the previous version keeps playing. `reload_script()` does
  the same at runtime.
- **Instruments and MIDI.** `AudioStreamJSFX` plays a JSFX synth through
  `AudioStreamPlayer`, `AudioStreamPlayer2D` or `AudioStreamPlayer3D`.
  `note_on`, `note_off`, `control_change`, `pitch_bend`, `all_notes_off` and
  raw `send_midi` feed the script's `midirecv`, and anything it sends with
  `midisend` arrives as the `midi_sent` signal. Bus effects receive MIDI the
  same way.
- **Real-time safe.** The audio thread never locks, allocates, parses or
  compiles. Parameter changes travel through a lock-free queue, and hot reload
  is an atomic pointer swap with deferred reclamation.
- **Lo-fi modes for cheap voices.** `downsample_factor` (1–8) runs the script
  at a fraction of the mix rate for a proportional CPU saving.
- **Web export.** Works in both single-threaded and SharedArrayBuffer
  (threaded) Web exports.

## Install

1. Download `jsfx_for_godot-<version>.zip` from
   [Releases](https://github.com/ayavaron/jsfx-for-godot/releases).
2. Unzip it into your project root, so you have
   `res://addons/jsfx_for_godot/jsfx_for_godot.gdextension`.
3. Restart the editor. `AudioEffectJSFX` now appears in the Audio bus
   "Add Effect" menu.

Prebuilt binaries: **Windows x86-64** and **Web** (single-threaded and
threaded). Linux and macOS build from source with the same CMake project (see
[Building](#building-from-source)). Those builds are wired up but haven't been
tested yet.

Requires Godot **4.3 or newer**. Tested on Godot 4.7.2.

## Usage

### In the editor

Open the Audio panel, choose **Add Effect → AudioEffectJSFX** on a bus, and
pick a `.jsfx` file for `script_path`. The slider properties appear under it.

### From GDScript

```gdscript
var fx := AudioEffectJSFX.new()
fx.script_path = "res://effects/tremolo.jsfx"
AudioServer.add_bus_effect(AudioServer.get_bus_index("SFX"), fx)

fx.set("slider_2_depth", 0.8)                # by property name
fx.set_slider(1, 7.5)                        # by JSFX slider number
create_tween().tween_property(fx, "slider_1_rate_hz", 12.0, 2.0)

fx.downsample_factor = 2                     # half the CPU, lower fidelity
fx.reload_script()                           # recompile after editing the file
```

### Instruments

```gdscript
var synth := AudioStreamJSFX.new()
synth.jsfx = load("res://instruments/simple_synth.jsfx")
$Piano3D.stream = synth                   # AudioStreamPlayer, 2D or 3D
$Piano3D.play()

var keys := synth.jsfx                    # the instrument's AudioEffectJSFX
keys.note_on(60, 100)                     # note, velocity, channel = 0
keys.note_off(60)
keys.control_change(1, 64)                # mod wheel
keys.pitch_bend(0.5)                      # -1.0 ... 1.0
keys.all_notes_off()
keys.slider_5_cutoff_hz = 800.0           # sliders work as for effects

keys.midi_sent.connect(func(status, data1, data2): print("script sent ", data1))
```

Each `play()` runs its own copy of the script with silent input; MIDI sent to
`synth.jsfx` reaches every playing copy at the start of its next audio block.
Delivery is therefore quantized to Godot's mix block (about 5–10 ms), which
is fine for gameplay and generated music but too coarse for playing back a
recorded MIDI performance with exact timing. Polyphony is the script's job:
JSFX synths manage their own voices. The demo project's main scene is a
playable example.

On Web, Godot 4.3 and later play streams that can be sampled (WAV, Ogg, MP3)
directly through Web Audio by default, which skips the bus effects. For JSFX
bus effects to hear those sounds, set the player's `playback_type` to
`Stream`, or set the project setting `audio/general/default_playback_type.web`.
`AudioStreamJSFX` always mixes through Godot, so its warning about streams
that "cannot be sampled" is harmless; setting `Stream` silences it.

A synth has to be an `AudioStreamJSFX`, not a bus effect. Godot discards the
output of effects on a bus that nothing is playing into, so a synth on an
empty bus would never be heard.

Property names are `slider_<N>_<description>`, where the description is
lowercased with spaces and punctuation turned into `_`. `Cutoff (Hz)` becomes
`slider_1_cutoff_hz`. Check the inspector or `get_property_list()` if unsure.

The [demo project](demo/) contains three example effects and an instrument.
`gain.jsfx` is the minimal case. `effects/lowpass.jsfx` is a resonant filter
with named and log sliders. `effects/tremolo.jsfx` uses an enum slider and
imports a shared `lib/lfo.jsfx-inc`. `instruments/simple_synth.jsfx` is an
8-voice synth with an envelope and filter that responds to notes, the mod
wheel and pitch bend.

The demo's main scene, `playground/playground.tscn`, layers three instruments
(the simple synth and [Aya's](#demo-instruments-by-aya) Unreal Piano and Cicada
Swarm), each on its own bus with a volume and a lo-fi (`downsample_factor`)
selector. They feed a mix bus with the tremolo, the lowpass and Aya's
Neighborhood. Every control is generated from the script's
`get_property_list()`, one tab per script. Other ways to play:

- the mouse, touch or the computer keyboard (A W S E D F T G Y H U J K O L P ;,
  Z/X for octave);
- the arpeggiator;
- a demo track: Rossini's *Barber of Seville* overture as a MIDI file, read by
  a small GDScript MIDI parser (`playground/midi_file.gd`). Every part plays on
  every enabled layer, except the bass drum and cymbals, which only Cicada
  Swarm plays.

A boid flies around wherever the cursor has been. Click it and it changes a
random parameter on the visible tab, with a burst of colour and a caption
naming the change. It's a small argument for treating sound parameters as
part of game feel. The page shows its FPS, and it runs on desktop and as a
Web export.

### Using REAPER's effects

Copy a `.jsfx` file (and any `.jsfx-inc` files it imports) into your project.
Imports resolve next to the importing file first, then from `res://`. Check
the license of each effect before shipping it in a game: REAPER's stock and
community JSFX have various licenses, and many are GPL/LGPL.

## JSFX compatibility

All 263 effects in a stock REAPER install (`InstallData/Effects`, including
the bundled community packs) went through the engine headlessly with 2 seconds
of noise each.

| Result | Effects |
|---|---|
| Load and run, finite output, no crashes | **234 (89%)** |
| Need `file_open` (load samples or impulse responses from disk) | 10 |
| Use the `<? ?>` EEL preprocessor | 9 |
| Need EEL2 string functions (`sprintf`, `strcpy`, `strlen`) | 6 |
| Need `mdct`/`imdct` | 4 |

Most of the 234 are audio effects, which work fully. MIDI effects receive the
messages you send from GDScript, and their `midisend` output arrives as the
`midi_sent` signal. Only one of REAPER's 8 bundled synths loads (Tolerable
Tines, which plays correctly as an `AudioStreamJSFX`): the others load their
samples with `file_open` or need string functions.

Supported beyond the basics: named sliders (`slider1:name=0<...>`), up to 256
sliders, `:log`/`:sqr` slider shapes, hidden sliders (`-description`), file
sliders (exposed as an index), enum labels, repeated sections, sloppy headers
that REAPER tolerates, and `import`.

## Performance impact

Each JSFX instance adds three costs: CPU on Godot's audio thread, memory, and
a one-time compile on the main thread. Everything below was measured on an
Intel i7-11700KF with `tools/jsfx_bench.cpp` (engine alone) and
`demo/tests/audio_probe.gd` (inside Godot 4.7.2 with a real audio driver).
Run them on your own hardware to compare.

### CPU (audio thread)

Per effect instance at 48 kHz stereo, 512-frame blocks, x64 JIT, across the
234 working REAPER effects:

| | Time per 512-frame block | Share of one CPU core |
|---|---|---|
| Median effect | 21 µs | **0.20%** |
| 90th percentile | 52 µs | 0.49% |
| 99th percentile | 509 µs | 4.8% |
| Heaviest (multiband correlation meter) | 775 µs | 7.3% |

Typical game effects sit at the low end. The demo `gain.jsfx` uses 0.09% of a
core, `lowpass.jsfx` 0.17% and `tremolo.jsfx` 0.19%. Godot mixes on its own
audio thread, so this cost doesn't come out of your frame time unless the
machine is CPU-bound. A typical effect leaves room for hundreds of instances.
The Godot probe runs 17 JSFX buses at once and keeps up in real time.

**Interpreter builds** (Web, ARM, iOS, universal macOS) run EEL2 without the
JIT. That costs about 1.3–4× the CPU per effect: `lowpass.jsfx` goes from
0.17% to 0.56% natively, and the heaviest spectral effects reach about 4×.
Browsers add WebAssembly overhead on top (not yet measured), so budget fewer,
simpler effects for Web exports.

**Lo-fi mode:** `downsample_factor = 2` halves the cost and `4` quarters it.
For example, lowpass drops from 18.0 µs to 9.9 µs and then 5.0 µs. The script
sees the reduced `srate`, so filters and delays keep their tuning, but
everything above the reduced Nyquist frequency is lost (zero-order hold). It
works well for background voices and radios.

**Instruments** cost CPU while playing whether or not notes are sounding,
since the script runs every block. The demo synth uses 0.25% of a core idle
and 0.86% with all 8 voices sounding (x64 JIT); in the interpreter used on Web
that is 0.96% and 6.1%. Stop players you aren't using.

`force_mono` makes the output mono (left channel into both outputs). It does
**not** reduce CPU, because a stereo script still runs all its per-channel
code.

### Memory

| | Desktop, per instance | Web, per instance |
|---|---|---|
| Memory actually used | median **0.24 MB**, 90th percentile 0.31 MB, max 6.2 MB (resident) | median **0.37 MB**, 90th percentile 1.3 MB, max 7.9 MB (heap) |
| Address space reserved | 64 MB (virtual) | none |

Every JSFX can address EEL2's full 8M slots, as in REAPER, but only pays for
the memory its script touches.

- **Desktop** reserves one 64 MB virtual-memory region per instance, and the
  OS supplies pages only when a script first touches them. On Windows the
  64 MB counts toward the system commit limit (pagefile-backed) but not toward
  RAM in use. Each first touch of a 4 KB page costs one page fault (around a
  microsecond) on the audio thread.
- **Web** has no lazily backed pages, so EEL2 allocates its 512 KB blocks as
  they are first touched. Inside `@init` that's a normal allocation on the
  main thread. During playback, blocks come from a shared pool of pre-zeroed
  blocks (8 by default, 4 MB in total) that the main thread tops up every
  frame, so the audio thread still never allocates. In Chrome, a demo export
  with nine JSFX effects uses a 46 MB WebAssembly heap in total; v0.1.0 needed
  688 MB.

If a script touches more new memory between two frames than the pool holds,
the excess reads return 0 and writes are dropped until the next refill. Godot
prints a warning when that happens. No effect in the REAPER corpus came close.
Raise the reserve with `AudioEffectJSFX.set_memory_pool_blocks(16)` if you
see the warning. The pool is unused on desktop.

By comparison, stock EEL2 allocates 512 KB blocks with `calloc` on the audio
thread the first time a script touches them.

### Load time (main thread)

Parsing and compiling a script takes a median of **2.3 ms** (max 7.4 ms
across the REAPER corpus). This happens when `script_path` is set or a `.jsfx`
resource loads, and again for each extra bus instance or hot reload. In Godot,
creating a lowpass effect and adding it to a bus took about 1 ms. Create
effects during loading screens rather than mid-gameplay if you need perfectly
flat frame times.

### Binary size

The Windows release DLL is 692 KB with the C++ runtime linked statically, so
players don't need the VC++ redistributable. The Web side module is about
7 MB.

## Limitations

- No `@gfx` (JSFX UI) or `@serialize`. Godot's inspector and resource
  serialization replace them.
- No EEL2 strings, `file_*` functions, `<? ?>` preprocessor or `mdct`. Effects
  that need them fail to compile and pass audio through unchanged, with the
  error logged.
- MIDI is short messages only (no SysEx: the `_buf`/`_str` variants are
  no-ops), arrives at the start of the next mix block rather than at an exact
  sample, and is not routed between effects.
- Stereo only (`spl0`/`spl1`). `num_ch` is 2.
- `tempo` defaults to 120 BPM and `play_state` to "playing", with
  `beat_position`/`play_position` advancing in real time. Godot has no
  transport.
- Imported files contribute their `@init` code (function libraries).
  `@sample`/`@block` code inside an import is ignored with a warning.
- Linux and macOS builds are not yet tested.

## Building from source

Requirements: CMake 3.24+, a C++20 compiler (MSVC 2022, GCC 13+, Clang 16+),
and network access on first configure (Catch2 and godot-cpp are fetched).
Non-MSVC x86-64 builds also need NASM for the EEL2 JIT. Without it they fall
back to the interpreter.

```powershell
# Engine + unit tests (no Godot needed)
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release

# GDExtension (from a VS Developer PowerShell; Ninja)
cmake --preset windows-godot-release
cmake --build --preset windows-godot-release   # -> demo/bin/windows-release/
cmake --preset windows-godot-debug
cmake --build --preset windows-godot-debug     # editor builds load this one

# Web (Emscripten 3.1.64, after emsdk_env)
cmake --preset web-godot-release
cmake --build --preset web-godot-release
cmake --preset web-godot-release-threads
cmake --build --preset web-godot-release-threads

# Package the addon zip from whatever is built
python tools/package_release.py 0.1.0
```

Linux/macOS: `cmake -S . -B build/godot -G Ninja -DCMAKE_BUILD_TYPE=Release
-DJSFX_BUILD_GODOT=ON` followed by `cmake --build build/godot`. Pass
`-DJSFX_EEL_JIT=OFF` to force the interpreter anywhere.

## Testing

| Suite | What it covers | Command |
|---|---|---|
| Catch2 (29 cases) | Parser leniency, imports, host functions, MIDI in/out, the demo synth playing and releasing notes, FFT, gain accuracy (RMS within 1e-6), memory bounds, the Web memory path (forced on desktop), denormal flushing, lock-free queue under contention, lo-fi modes | `ctest --preset windows-release` |
| Smoke | Registration, loader, properties, serialization, hot reload, teardown | `godot --headless --path demo --script res://tests/extension_smoke.gd` |
| GUT (9 tests) | Property hints and serialization, imports, threaded loading, Tween automation, parameter-burst stress, the instrument MIDI API | `tools/fetch_gut.ps1`, then `godot --headless --path demo -s addons/gut/gut_cmdln.gd -gdir=res://tests/unit -gexit` |
| Audio probe | Real audio: output accuracy, 400+ hot reloads while playing, downsampled `srate`, a synth playing and releasing notes, `midisend` reaching `midi_sent`, 17 live instances | `godot --headless --audio-driver WASAPI --path demo --script res://tests/audio_probe.gd` |
| Browser | Web exports, both threading modes, in Chrome; `--playground` clicks start and holds a key in the playable demo until the synth sounds, and with `?autoplay=1&from=SECONDS` plus `--wait-for JSFX_TRACK_PLAYING` it plays the demo track and reports level and FPS; `?smoke=1` runs the test harness, where `?instances=N` plus `--settle-ms` reports the WebAssembly heap and `?synth=1` plays a chord and reports its level | `tools/serve_web.py` + `tools/browser_smoke.py` |
| Corpus bench | Every `.jsfx` you point it at: compatibility, CPU, memory (`--no-os-mapping` measures the Web memory path) | `jsfx_bench --import-dir <Effects> <files...>` |

Import the demo once before running the Godot suites:
`godot --headless --path demo --editor --quit-after 300`. Use
`--quit-after` rather than `--quit`: Godot 4.7 can crash if it quits while
still generating docs for a newly discovered extension.

## How it works

```
 main thread                                   audio thread
 ───────────                                   ────────────
 .jsfx ─► JsfxDocument::parse  (sections, sliders, imports)
        ─► Eel2Context::create (compile, map memory, run @init)
        ─► JsfxProcessor ──── atomic pointer ───► AudioEffectJSFXInstance::_process
 property set, MIDI ─► lock-free queues ────────►   drain queues → @slider, midirecv
 hot reload  ─► new processor, atomic swap ─────►   @block, @sample per frame
               (old one freed once the audio thread has left it)
 midi_sent signal ◄── lock-free queue ◄─────────────  midisend
```

Bus effects (`AudioEffectJSFXInstance`) and instruments
(`AudioStreamPlaybackJSFX`) share the same runtime (`src/godot/jsfx_runtime`).

`src/core` and `include/jsfx` hold a Godot-free C++20 library: parser, EEL2
wrapper, processor and queue. `src/godot` holds the GDExtension bridge.
`third_party/wdl` is an unmodified subset of Cockos WDL.

## Demo instruments by Aya

The demo's Unreal Piano, Cicada Swarm and Neighborhood
([`demo/instruments/aya/`](demo/instruments/aya/)) were vibe-coded by Aya, a
musician and REAPER enthusiast, and are MIT licensed. Her non-AI music is on
Bandcamp and Spotify: **GWID** for vocal music and **OK I DUNNO** for
instrumental music.

- **Gemini Unreal Piano**: 16 voices, each mixing a morphing oscillator, FM,
  Karplus-Strong and additive engines. Its round robin is on by default
  (Sequential), so successive notes cycle through the four engines.
- **Cicada Swarm**: 12 voices of noise bursts through resonant "membranes".
  Voices are not stolen: past 12 held notes, new notes are dropped. Its *Sway
  Rate* slider reads `play_pos`, which is not a JSFX variable
  (`play_position` is), so it has no effect, in REAPER too.
- **Neighborhood**: not an instrument but an effect. It has no MIDI input and
  rattles in response to the audio passing through it, so the demo puts it
  on the mix bus.

## License and credits

MIT. See [LICENSE](LICENSE). The bundled EEL2 engine is
[Cockos WDL](https://github.com/justinfrankel/WDL) under its zlib-style
license ([details](third_party/wdl/VENDORED.md)).

JSFX, EEL2 and REAPER are by [Cockos](https://www.cockos.com/). This project
is unofficial and not affiliated with or endorsed by Cockos.
