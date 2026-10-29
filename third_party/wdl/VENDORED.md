# Vendored WDL / EEL2

This directory contains an unmodified subset of Cockos WDL, used as the EEL2
engine for JSFX for Godot.

| | |
|---|---|
| Upstream | https://github.com/justinfrankel/WDL |
| Commit | `61c5553aad0d257775a4dd7bf36b8be617ec8281` (2026-10-21) |
| Vendored on | 2026-10-28 (x64 JIT object added 2026-10-29) |
| License | zlib-style; see below and the header of every file |

## What is vendored

`eel2/` contains the EEL2 compiler, bytecode interpreter, lexer/parser
generated sources, architecture glue, and assembly sources for JIT builds.
`wdltypes.h`, `wdlcstring.h`, `utf8_extended.h`, and `denormal.h` are the WDL
headers required to compile EEL2. `fft.c` / `fft.h` are WDL's FFT, kept because
EEL2's `eel_fft.h` depends on it (not used by the raw EEL2 core).

`eel2/asm-nseel-x64.obj` is the upstream prebuilt MSVC x64 JIT object, copied
verbatim. It is linked when `JSFX_EEL_JIT=ON` on MSVC x64. Other x64 toolchains
assemble `eel2/asm-nseel-x64-sse.asm` at build time with NASM, matching WDL's
own commands:

- Linux: `nasm -D AMD64ABI -f elf64 asm-nseel-x64-sse.asm`
- macOS: `nasm -D AMD64ABI -f macho64 --prefix _ asm-nseel-x64-sse.asm`
- non-MSVC Windows: `nasm -f win64 asm-nseel-x64-sse.asm` (no `AMD64ABI`)

Web, iOS, ARM, MSVC x86 and universal macOS builds use the portable
interpreter.

The following upstream files are intentionally **not** vendored:

- Other prebuilt assembly objects (`asm-nseel-aarch64-msvc.obj`,
  `asm-nseel-arm64ec-msvc.obj`, `asm-nseel-multi-macho.o`,
  `asm-nseel-x64-macho.o`) and IDE project files.
- The `eelscript.h` convenience layer, `eel_pproc.h` preprocessor, graphics,
  file, network, LICE, SWELL, and unrelated WDL libraries.
- Upstream build files; this project builds EEL2 from CMake.

EEL2 is compiled with `EEL_TARGET_PORTABLE` unless the x64 JIT is enabled.
JIT is the default on x86-64 desktop targets when the required object can be
provided: MSVC uses the prebuilt COFF object, and other toolchains require
NASM. Web, iOS, ARM and universal macOS force the interpreter.

## Updating

Re-copy the paths listed above from the desired upstream commit, update the
commit hash in this file, then run the test suite. Do not edit vendored files;
if a local patch becomes necessary, apply it at build time or isolate it in
`src/core/` and record it here.

## License

The following notice is reproduced from the EEL2 sources:

```
  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
```
