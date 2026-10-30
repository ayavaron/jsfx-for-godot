cmake_minimum_required(VERSION 3.24)

project(jsfx_for_godot VERSION 0.1.1 LANGUAGES C CXX)

if(EMSCRIPTEN)
  # Emscripten's platform module reports no shared-library support, but a
  # GDExtension is a wasm side module linked with -sSIDE_MODULE=1.
  set_property(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS TRUE)
endif()

# Link the MSVC runtime statically, as godot-cpp's SCons build does. A
# GDExtension DLL that imports MSVCP140.dll needs the VC++ redistributable on
# players' machines, and crashes (std::mutex) when the process has already
# loaded an older msvcp140.dll.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(PROJECT_IS_TOP_LEVEL)
  set(JSFX_IS_TOP_LEVEL ON)
else()
  set(JSFX_IS_TOP_LEVEL OFF)
endif()

option(JSFX_BUILD_TESTS "Build the Catch2 test suite" ${JSFX_IS_TOP_LEVEL})
option(JSFX_WARNINGS_AS_ERRORS "Treat warnings in project code as errors" OFF)
option(JSFX_BUILD_GODOT
       "Build the Godot GDExtension bridge. Fetches godot-cpp unless \
JSFX_GODOT_CPP_SOURCE_DIR is set. Use a single-config generator (Ninja)." OFF)
option(JSFX_WEB_THREADS
       "Build the Web extension with pthreads (SharedArrayBuffer). Ignored \
outside Emscripten." OFF)
set(JSFX_GODOT_CPP_SOURCE_DIR "" CACHE PATH
    "Optional local godot-cpp checkout; skips FetchContent")
set(JSFX_DEMO_BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/demo/bin" CACHE PATH
    "Directory the demo project uses for built GDExtension binaries")
# EEL2's JIT needs an x86-64 target and WDL's x64 assembly object. MSVC x64
# uses the prebuilt COFF object WDL ships; other toolchains assemble the
# vendored NASM source (ELF64 on Linux, Mach-O on macOS, COFF on Windows).
set(JSFX_EEL_JIT_TARGET_X64 OFF)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _jsfx_system_processor)
  if(_jsfx_system_processor MATCHES "^(x86_64|amd64|x64)$")
    set(JSFX_EEL_JIT_TARGET_X64 ON)
  endif()
endif()
if(APPLE)
  if(CMAKE_OSX_ARCHITECTURES)
    # A universal or arm64-only build cannot use the x64 JIT object yet.
    string(TOLOWER "${CMAKE_OSX_ARCHITECTURES}" _jsfx_apple_architectures)
    if(_jsfx_apple_architectures STREQUAL "x86_64")
      set(JSFX_EEL_JIT_TARGET_X64 ON)
    else()
      set(JSFX_EEL_JIT_TARGET_X64 OFF)
    endif()
  elseif(CMAKE_GENERATOR MATCHES "Xcode")
    # Xcode may otherwise build ARCHS_STANDARD (universal), which this object
    # cannot satisfy. Require an explicit x86_64 architecture for JIT.
    set(JSFX_EEL_JIT_TARGET_X64 OFF)
  endif()
endif()

set(JSFX_EEL_JIT_DEFAULT OFF)
if(JSFX_EEL_JIT_TARGET_X64 AND NOT EMSCRIPTEN AND NOT IOS)
  if(MSVC)
    set(JSFX_EEL_JIT_DEFAULT ON)
  else()
    find_program(JSFX_NASM_EXECUTABLE NAMES nasm)
    if(JSFX_NASM_EXECUTABLE)
      set(JSFX_EEL_JIT_DEFAULT ON)
    endif()
  endif()
endif()

option(JSFX_EEL_JIT
       "Enable EEL2's x64 JIT on supported x86-64 desktop builds. Web/iOS \
always use the interpreter."
       ${JSFX_EEL_JIT_DEFAULT})
if(EMSCRIPTEN OR IOS)
  set(JSFX_EEL_JIT OFF)
endif()

if(JSFX_EEL_JIT AND NOT JSFX_EEL_JIT_TARGET_X64)
  message(FATAL_ERROR
    "JSFX_EEL_JIT requires an x86-64-only target. Disable it to use the EEL2 \
interpreter; Web, iOS, ARM and universal macOS always use the interpreter.")
endif()
if(JSFX_EEL_JIT AND NOT MSVC AND NOT JSFX_NASM_EXECUTABLE)
  message(FATAL_ERROR
    "JSFX_EEL_JIT on a non-MSVC x64 build requires NASM to assemble WDL's \
asm-nseel-x64-sse.asm. Install NASM or disable JSFX_EEL_JIT.")
endif()
if(NOT JSFX_EEL_JIT AND JSFX_EEL_JIT_TARGET_X64 AND NOT MSVC
   AND NOT EMSCRIPTEN AND NOT IOS AND NOT JSFX_NASM_EXECUTABLE)
  message(STATUS
    "JSFX for Godot: NASM not found; EEL2 JIT disabled, using the interpreter")
endif()

function(jsfx_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    if(JSFX_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    if(JSFX_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Vendored WDL EEL2 (x64 JIT where available, interpreter elsewhere)
# ---------------------------------------------------------------------------

add_library(jsfx_wdl STATIC
  third_party/wdl/eel2/nseel-caltab.c
  third_party/wdl/eel2/nseel-cfunc.c
  third_party/wdl/eel2/nseel-compiler.c
  third_party/wdl/eel2/nseel-eval.c
  third_party/wdl/eel2/nseel-lextab.c
  third_party/wdl/eel2/nseel-ram.c
  third_party/wdl/eel2/nseel-yylex.c
  third_party/wdl/fft.c
  # Host callbacks WDL requires. They live in this archive so GNU linkers can
  # resolve WDL's back-references without a --start-group link group.
  src/core/wdl_host_stubs.c
  # JSFX host functions (slider(), spl(), FFT, MIDI no-ops, ...).
  src/core/jsfx_host.c
  # Real-time block pool that nseel-ram.c's calloc is redirected to.
  src/core/ram_pool.cpp
)
target_include_directories(jsfx_wdl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Redirect calloc in nseel-ram.c (only) to the block pool, at build time, so
# the vendored file stays unmodified.
set(JSFX_RAM_REDIRECT "${CMAKE_CURRENT_SOURCE_DIR}/src/core/jsfx_ram_redirect.h")
if(MSVC)
  set_source_files_properties(third_party/wdl/eel2/nseel-ram.c PROPERTIES
    COMPILE_OPTIONS "/FI${JSFX_RAM_REDIRECT}")
else()
  set_source_files_properties(third_party/wdl/eel2/nseel-ram.c PROPERTIES
    COMPILE_OPTIONS "-include;${JSFX_RAM_REDIRECT}")
endif()

# jsfx_host.c is ours, not vendored: an implicit declaration there truncates
# 64-bit pointers, so make it an error even though WDL builds with warnings off.
if(MSVC)
  set_source_files_properties(src/core/jsfx_host.c PROPERTIES
    COMPILE_OPTIONS "/we4013")
else()
  set_source_files_properties(src/core/jsfx_host.c PROPERTIES
    COMPILE_OPTIONS "-Werror=implicit-function-declaration")
endif()

target_include_directories(jsfx_wdl SYSTEM PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wdl/eel2
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wdl
)

# EEL_TARGET_PORTABLE must be visible to our wrapper, which includes ns-eel.h.
target_compile_definitions(jsfx_wdl PUBLIC
  WDL_FFT_REALSIZE=8
)

if(JSFX_EEL_JIT)
  if(MSVC)
    # WDL ships this prebuilt NASM object for the MSVC x64 JIT.
    set(JSFX_EEL_JIT_OBJECT
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wdl/eel2/asm-nseel-x64.obj")
  else()
    # Match WDL's own assemble commands. Win64 omits AMD64ABI; the SysV
    # ELF/Mach-O ABIs define it, and Mach-O symbols need the C underscore.
    if(WIN32)
      set(_jsfx_nasm_format win64)
      set(_jsfx_nasm_options)
      set(_jsfx_nasm_abi_defines)
      set(_jsfx_jit_object_suffix ".obj")
    elseif(APPLE)
      set(_jsfx_nasm_format macho64)
      set(_jsfx_nasm_options --prefix _)
      set(_jsfx_nasm_abi_defines -D AMD64ABI)
      set(_jsfx_jit_object_suffix ".o")
    else()
      set(_jsfx_nasm_format elf64)
      set(_jsfx_nasm_options)
      set(_jsfx_nasm_abi_defines -D AMD64ABI)
      set(_jsfx_jit_object_suffix ".o")
    endif()

    set(JSFX_EEL_JIT_ASM
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wdl/eel2/asm-nseel-x64-sse.asm")
    set(JSFX_EEL_JIT_OBJECT
        "${CMAKE_CURRENT_BINARY_DIR}/asm-nseel-x64${_jsfx_jit_object_suffix}")
    add_custom_command(
      OUTPUT "${JSFX_EEL_JIT_OBJECT}"
      COMMAND "${JSFX_NASM_EXECUTABLE}"
              ${_jsfx_nasm_abi_defines}
              -f "${_jsfx_nasm_format}"
              ${_jsfx_nasm_options}
              "${JSFX_EEL_JIT_ASM}"
              -o "${JSFX_EEL_JIT_OBJECT}"
      DEPENDS "${JSFX_EEL_JIT_ASM}"
      COMMENT "Assembling WDL EEL2 x64 JIT object (${_jsfx_nasm_format})"
      VERBATIM
    )
  endif()
  set_source_files_properties("${JSFX_EEL_JIT_OBJECT}" PROPERTIES
    EXTERNAL_OBJECT TRUE
  )
  target_sources(jsfx_wdl PRIVATE "${JSFX_EEL_JIT_OBJECT}")
else()
  target_compile_definitions(jsfx_wdl PUBLIC EEL_TARGET_PORTABLE)
endif()
target_compile_definitions(jsfx_wdl PRIVATE
  NSEEL_LOOPFUNC_SUPPORT_MAXLEN=0
  # Story 1.1: route EEL diagnostics to the host's null sink. eelscript.h is
  # not part of the compiled core yet, so this is inert until that layer lands.
  EEL_STRING_DEBUGOUT=jsfx_eel_debugout
)

if(MSVC)
  target_compile_options(jsfx_wdl PRIVATE /W0)
  target_compile_definitions(jsfx_wdl PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(jsfx_wdl PRIVATE -w)
endif()

set_target_properties(jsfx_wdl PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Headless JSFX core
# ---------------------------------------------------------------------------

add_library(jsfx_core STATIC
  src/core/document.cpp
  src/core/eel2_context.cpp
  src/core/processor.cpp
)
add_library(jsfx::core ALIAS jsfx_core)

target_include_directories(jsfx_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(jsfx_core PRIVATE jsfx_wdl)
if(EMSCRIPTEN)
  # Match Godot's own no-exceptions Web build.
  target_compile_options(jsfx_core PRIVATE -fno-exceptions)
endif()
if(MSVC)
  # WDL headers use older CRT spellings; the headers are inspected by the core
  # translation units even though their implementation is compiled separately.
  target_compile_definitions(jsfx_core PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
set_target_properties(jsfx_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
jsfx_set_warnings(jsfx_core)

# PRD Story 4.2 (Phase 2): the Wasm heap must fit EEL2's 64 MiB RAM window.
# These propagate to an Emscripten final link when the core is consumed by a
# CMake target. The Godot GDExtension link configuration is wired up in Phase 2.
if(EMSCRIPTEN)
  target_link_options(jsfx_core INTERFACE
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=134217728
  )
endif()

# ---------------------------------------------------------------------------
# Godot GDExtension bridge (Phase 2)
# ---------------------------------------------------------------------------

if(JSFX_BUILD_GODOT)
  if(EMSCRIPTEN)
    set(JSFX_GODOT_PLATFORM web)
  elseif(WIN32)
    set(JSFX_GODOT_PLATFORM windows)
  elseif(APPLE)
    set(JSFX_GODOT_PLATFORM macos)
  else()
    set(JSFX_GODOT_PLATFORM linux)
  endif()

  if(JSFX_GODOT_CPP_SOURCE_DIR)
    add_subdirectory("${JSFX_GODOT_CPP_SOURCE_DIR}"
                     "${CMAKE_CURRENT_BINARY_DIR}/godot-cpp")
  else()
    include(FetchContent)
    FetchContent_Declare(godot-cpp
      GIT_REPOSITORY https://github.com/godotengine/godot-cpp.git
      GIT_TAG godot-4.3-stable
      GIT_SHALLOW TRUE
      SYSTEM
    )
    FetchContent_MakeAvailable(godot-cpp)
  endif()

  if(MSVC)
    # godot-cpp 4.3's CMake hard-codes /MD; switch it to the static runtime
    # selected above so the whole DLL links one CRT.
    get_target_property(JSFX_GODOT_CPP_FLAGS godot-cpp COMPILE_FLAGS)
    string(REGEX REPLACE "/MD(d?)" "/MT\\1" JSFX_GODOT_CPP_FLAGS
           "${JSFX_GODOT_CPP_FLAGS}")
    set_target_properties(godot-cpp PROPERTIES COMPILE_FLAGS
                                               "${JSFX_GODOT_CPP_FLAGS}")
  endif()

  if(EMSCRIPTEN)
    # Emscripten's CMake platform file does not advertise shared libraries.
    # A GDExtension is a WebAssembly side module, which is a shared object built
    # with -sSIDE_MODULE=1.
    set(CMAKE_SHARED_LIBRARY_CREATE_C_FLAGS "-sSIDE_MODULE=1")
    set(CMAKE_SHARED_LIBRARY_CREATE_CXX_FLAGS "-sSIDE_MODULE=1")
    set(CMAKE_SHARED_LIBRARY_SUFFIX ".wasm")
    set(CMAKE_SHARED_LIBRARY_PREFIX "")
  endif()

  set(JSFX_GODOT_SOURCES
    src/godot/register_types.cpp
    src/godot/audio_effect_jsfx.cpp
    src/godot/audio_effect_jsfx_instance.cpp
    src/godot/audio_effect_jsfx_loader.cpp
    src/godot/audio_stream_jsfx.cpp
    src/godot/jsfx_runtime.cpp
  )
  add_library(jsfx_gdextension SHARED ${JSFX_GODOT_SOURCES})
  target_include_directories(jsfx_gdextension PRIVATE src)
  target_link_libraries(jsfx_gdextension PRIVATE jsfx_core godot-cpp)
  jsfx_set_warnings(jsfx_gdextension)

  if(EMSCRIPTEN)
    # Flags synchronized with godot-cpp's tools/web.py. The extension is a
    # WebAssembly side module loaded by Godot's AudioWorklet/engine runtime.
    set(JSFX_WEB_COMPILE_OPTIONS
      -sSIDE_MODULE=1
      -sSUPPORT_LONGJMP=wasm
    )
    set(JSFX_WEB_LINK_OPTIONS
      -sSIDE_MODULE=1
      -sSUPPORT_LONGJMP=wasm
      # Godot 4.3 official Web templates are built with Emscripten >= 3.1.41
      # and therefore use BigInt for 64-bit values.
      -sWASM_BIGINT
      # PRD Story 4.2: EEL2 reserves a 64 MiB RAM window.
      -sALLOW_MEMORY_GROWTH=1
      -sINITIAL_MEMORY=134217728
    )
    if(JSFX_WEB_THREADS)
      list(APPEND JSFX_WEB_COMPILE_OPTIONS -sUSE_PTHREADS=1)
      list(APPEND JSFX_WEB_LINK_OPTIONS -sUSE_PTHREADS=1)
    endif()

    target_compile_options(godot-cpp PRIVATE ${JSFX_WEB_COMPILE_OPTIONS})
    # The core uses atomics (std::call_once, the parameter queue). Every
    # object in a shared-memory side module must be built with the same
    # pthread features or wasm-ld refuses to link.
    target_compile_options(jsfx_core PRIVATE ${JSFX_WEB_COMPILE_OPTIONS})
    target_compile_options(jsfx_wdl PRIVATE ${JSFX_WEB_COMPILE_OPTIONS})
    target_compile_definitions(godot-cpp PRIVATE WEB_ENABLED UNIX_ENABLED)

    target_compile_options(jsfx_gdextension PRIVATE ${JSFX_WEB_COMPILE_OPTIONS}
                                                  -fno-exceptions)
    target_compile_definitions(jsfx_gdextension PRIVATE WEB_ENABLED UNIX_ENABLED)
    target_link_options(jsfx_gdextension PRIVATE ${JSFX_WEB_LINK_OPTIONS})
    set_target_properties(jsfx_gdextension PROPERTIES
      PREFIX ""
      SUFFIX ".wasm"
    )
  endif()

  set(JSFX_GODOT_VARIANT "")
  if(EMSCRIPTEN AND JSFX_WEB_THREADS)
    set(JSFX_GODOT_VARIANT "-threads")
  endif()
  set(JSFX_GODOT_OUTPUT_DIR
      "${JSFX_DEMO_BIN_DIR}/${JSFX_GODOT_PLATFORM}-$<LOWER_CASE:$<CONFIG>>${JSFX_GODOT_VARIANT}")
  set_target_properties(jsfx_gdextension PROPERTIES
    OUTPUT_NAME "jsfx_for_godot"
    RUNTIME_OUTPUT_DIRECTORY "${JSFX_GODOT_OUTPUT_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${JSFX_GODOT_OUTPUT_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib"
    POSITION_INDEPENDENT_CODE ON
  )

  message(STATUS "JSFX for Godot: GDExtension -> ${JSFX_GODOT_OUTPUT_DIR}")
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(JSFX_BUILD_TESTS)
  include(CTest)
  if(BUILD_TESTING)
    include(FetchContent)
    FetchContent_Declare(Catch2
      GIT_REPOSITORY https://github.com/catchorg/Catch2.git
      GIT_TAG v3.9.1
      GIT_SHALLOW TRUE
      SYSTEM
    )
    FetchContent_MakeAvailable(Catch2)

    add_executable(jsfx_tests
      tests/document_test.cpp
      tests/eel2_context_test.cpp
      tests/processor_test.cpp
      tests/slider_queue_test.cpp
    )
    target_link_libraries(jsfx_tests PRIVATE jsfx_core Catch2::Catch2WithMain)
    # Lets tests load the demo project's .jsfx files.
    target_compile_definitions(jsfx_tests PRIVATE
      JSFX_DEMO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/demo")
    jsfx_set_warnings(jsfx_tests)
    find_package(Threads REQUIRED)
    target_link_libraries(jsfx_tests PRIVATE Threads::Threads)

    include(Catch)
    catch_discover_tests(jsfx_tests PROPERTIES TIMEOUT 60)

    # Compatibility/performance probe over real .jsfx files (not a CTest).
    add_executable(jsfx_bench tools/jsfx_bench.cpp)
    target_link_libraries(jsfx_bench PRIVATE jsfx_core)
    if(WIN32)
      target_link_libraries(jsfx_bench PRIVATE psapi)
    endif()
    jsfx_set_warnings(jsfx_bench)
  endif()
endif()

if(JSFX_EEL_JIT)
  set(JSFX_EEL_MODE "JIT")
else()
  set(JSFX_EEL_MODE "interpreter")
endif()

message(STATUS "JSFX for Godot ${PROJECT_VERSION}: EEL2 ${JSFX_EEL_MODE}, "
               "tests ${JSFX_BUILD_TESTS}")
