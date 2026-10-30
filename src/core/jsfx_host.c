/* JSFX host functions for EEL2.
 *
 * REAPER provides these to every JSFX. Godot has no MIDI routing, automation
 * lanes or plug-in UI, so those calls compile to harmless no-ops; the rest
 * behave as documented in the JSFX reference. Everything here is
 * allocation-free and safe to call from the audio thread. */

#include "jsfx_host.h"

#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/time.h>
#if !defined(__EMSCRIPTEN__)
#include <sys/mman.h>
#endif
#endif

/* ns-eel-int.h declares __NSEEL_RAMAlloc and nseel_ramalloc_onfail, which
 * eel_fft.h uses. Without the prototype, C would truncate the returned
 * pointer to int on 64-bit targets. */
#include "ns-eel.h"
#include "ns-eel-int.h"
#include "eel_fft.h"

static int index_of(EEL_F value) {
  /* JSFX rounds slider()/spl() indices toward zero (with a tiny bias). */
  return (int)(value + 0.0001);
}

/* slider(n): 1-based reference to a slider variable, usable as an lvalue. */
static EEL_F* NSEEL_CGEN_CALL host_slider(void* opaque, EEL_F* index) {
  JsfxHostState* state = (JsfxHostState*)opaque;
  const int i = index_of(*index);
  if (state && i >= 1 && i <= JSFX_HOST_MAX_SLIDERS && state->sliders[i - 1]) {
    return state->sliders[i - 1];
  }
  if (!state) return NULL;
  state->scratch = 0.0;
  return &state->scratch;
}

/* spl(n): 0-based reference to spl0..spl63, usable as an lvalue. */
static EEL_F* NSEEL_CGEN_CALL host_spl(void* opaque, EEL_F* index) {
  JsfxHostState* state = (JsfxHostState*)opaque;
  const int i = index_of(*index);
  if (state && i >= 0 && i < JSFX_HOST_MAX_SPL && state->spl[i]) {
    return state->spl[i];
  }
  if (!state) return NULL;
  state->scratch = 0.0;
  return &state->scratch;
}

/* Automation/UI notifications have no Godot counterpart: Godot reads slider
 * values through properties, so return the argument and do nothing else. */
static EEL_F NSEEL_CGEN_CALL host_passthrough(void* opaque, INT_PTR count,
                                              EEL_F** params) {
  (void)opaque;
  return count > 0 ? *params[0] : 0.0;
}

static EEL_F NSEEL_CGEN_CALL host_zero(void* opaque, INT_PTR count,
                                       EEL_F** params) {
  (void)opaque;
  (void)count;
  (void)params;
  return 0.0;
}

/* slider_next_chg(): there are never sample-accurate automation points. */
static EEL_F NSEEL_CGEN_CALL host_minus_one(void* opaque, INT_PTR count,
                                            EEL_F** params) {
  (void)opaque;
  (void)count;
  (void)params;
  return -1.0;
}

/* midirecv(offset, msg1, msg23) or midirecv(offset, msg1, msg2, msg3):
 * returns 1 and fills the arguments with the next event of this block, or 0
 * when none are left. msg23 packs data1 | data2 << 8. */
static EEL_F NSEEL_CGEN_CALL host_midirecv(void* opaque, INT_PTR count,
                                           EEL_F** params) {
  JsfxHostState* state = (JsfxHostState*)opaque;
  const JsfxMidiEvent* event;
  if (!state || count < 3 || state->midi_in_pos >= state->midi_in_count) {
    return 0.0;
  }
  event = &state->midi_in[state->midi_in_pos++];
  *params[0] = event->offset;
  *params[1] = event->status;
  if (count >= 4) {
    *params[2] = event->data1;
    *params[3] = event->data2;
  } else {
    *params[2] = event->data1 + event->data2 * 256.0;
  }
  return 1.0;
}

static unsigned char midi_byte(EEL_F value) {
  const int v = (int)value;
  return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* midisend(offset, msg1, msg23) or midisend(offset, msg1, msg2, msg3). The
 * host forwards these to Godot (AudioEffectJSFX.midi_sent). */
static EEL_F NSEEL_CGEN_CALL host_midisend(void* opaque, INT_PTR count,
                                           EEL_F** params) {
  JsfxHostState* state = (JsfxHostState*)opaque;
  JsfxMidiEvent* event;
  if (!state || count < 3 || state->midi_out_count >= JSFX_HOST_MAX_MIDI) {
    return 0.0;
  }
  event = &state->midi_out[state->midi_out_count++];
  event->offset = (int)(*params[0] < 0 ? 0 : *params[0]);
  event->status = midi_byte(*params[1]);
  if (count >= 4) {
    event->data1 = midi_byte(*params[2]);
    event->data2 = midi_byte(*params[3]);
  } else {
    const int packed = (int)*params[2];
    event->data1 = (unsigned char)(packed & 0xFF);
    event->data2 = (unsigned char)((packed >> 8) & 0xFF);
  }
  return event->status ? (EEL_F)event->status : 1.0;
}

/* get_host_numchan(): Godot effect instances are always stereo. */
static EEL_F NSEEL_CGEN_CALL host_two(void* opaque, INT_PTR count,
                                      EEL_F** params) {
  (void)opaque;
  (void)count;
  (void)params;
  return 2.0;
}

static double wall_seconds(void) {
#if defined(_WIN32)
  FILETIME ft;
  ULARGE_INTEGER t;
  GetSystemTimeAsFileTime(&ft);
  t.LowPart = ft.dwLowDateTime;
  t.HighPart = ft.dwHighDateTime;
  /* 100 ns ticks since 1601 -> seconds since 1970. */
  return (double)(t.QuadPart - 116444736000000000ULL) * 1e-7;
#else
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (double)tv.tv_sec + (double)tv.tv_usec * 1e-6;
#endif
}

static EEL_F NSEEL_CGEN_CALL host_time(void* opaque, INT_PTR count,
                                       EEL_F** params) {
  const EEL_F now = (EEL_F)time(NULL);
  (void)opaque;
  if (count > 0) *params[0] = now;
  return now;
}

static EEL_F NSEEL_CGEN_CALL host_time_precise(void* opaque, INT_PTR count,
                                               EEL_F** params) {
  const EEL_F now = wall_seconds();
  (void)opaque;
  if (count > 0) *params[0] = now;
  return now;
}

static int g_os_mapping_enabled = 1;

void jsfx_host_set_os_mapping_enabled(int enabled) {
  g_os_mapping_enabled = enabled;
}

int jsfx_host_os_mapping_enabled(void) { return g_os_mapping_enabled; }

void* jsfx_host_map_ram(void* vm, int slots) {
  compileContext* ctx = (compileContext*)vm;
  const size_t bytes = (size_t)slots * sizeof(EEL_F);
  const int blocks = slots / NSEEL_RAM_ITEMSPERBLOCK;
  char* region = NULL;
  int i;
  if (!g_os_mapping_enabled || !ctx || !ctx->ram_state || blocks <= 0 ||
      blocks > NSEEL_RAM_BLOCKS) {
    return NULL;
  }
#if defined(_WIN32)
  region = (char*)VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT,
                               PAGE_READWRITE);
#elif !defined(__EMSCRIPTEN__)
  /* Wasm has no lazily backed pages; Web builds use the block pool. */
  region = (char*)mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (region == (char*)MAP_FAILED) region = NULL;
#endif
  if (!region) return NULL;
  for (i = 0; i < blocks; ++i) {
    ctx->ram_state->blocks[i] =
        (EEL_F*)(region + (size_t)i * NSEEL_RAM_ITEMSPERBLOCK * sizeof(EEL_F));
  }
  return region;
}

void jsfx_host_unmap_ram(void* vm, void* region, int slots) {
  compileContext* ctx = (compileContext*)vm;
  const int blocks = slots / NSEEL_RAM_ITEMSPERBLOCK;
  int i;
  if (!region) return;
  if (ctx && ctx->ram_state) {
    for (i = 0; i < blocks && i < NSEEL_RAM_BLOCKS; ++i) {
      ctx->ram_state->blocks[i] = NULL;
    }
  }
#if defined(_WIN32)
  VirtualFree(region, 0, MEM_RELEASE);
#elif !defined(__EMSCRIPTEN__)
  munmap(region, (size_t)slots * sizeof(EEL_F));
#else
  (void)slots;
#endif
}

void jsfx_host_register_functions(void) {
  NSEEL_addfunc_retptr("slider", 1, NSEEL_PProc_THIS, &host_slider);
  NSEEL_addfunc_retptr("spl", 1, NSEEL_PProc_THIS, &host_spl);

  NSEEL_addfunc_varparm("sliderchange", 1, NSEEL_PProc_THIS, &host_passthrough);
  NSEEL_addfunc_varparm("slider_automate", 1, NSEEL_PProc_THIS, &host_passthrough);
  NSEEL_addfunc_varparm("slider_show", 1, NSEEL_PProc_THIS, &host_passthrough);
  NSEEL_addfunc_varparm("slider_next_chg", 1, NSEEL_PProc_THIS, &host_minus_one);

  /* Short MIDI messages in and out. SysEx and buffer variants are no-ops. */
  NSEEL_addfunc_varparm("midirecv", 3, NSEEL_PProc_THIS, &host_midirecv);
  NSEEL_addfunc_varparm("midisend", 3, NSEEL_PProc_THIS, &host_midisend);
  NSEEL_addfunc_varparm("midirecv_buf", 1, NSEEL_PProc_THIS, &host_zero);
  NSEEL_addfunc_varparm("midirecv_str", 1, NSEEL_PProc_THIS, &host_zero);
  NSEEL_addfunc_varparm("midisend_buf", 1, NSEEL_PProc_THIS, &host_zero);
  NSEEL_addfunc_varparm("midisend_str", 1, NSEEL_PProc_THIS, &host_zero);
  NSEEL_addfunc_varparm("midisyx", 1, NSEEL_PProc_THIS, &host_zero);

  /* Channel/pin configuration: a fixed stereo host. */
  NSEEL_addfunc_varparm("get_host_numchan", 0, NSEEL_PProc_THIS, &host_two);
  NSEEL_addfunc_varparm("set_host_numchan", 1, NSEEL_PProc_THIS, &host_zero);
  NSEEL_addfunc_varparm("get_pin_mapping", 1, NSEEL_PProc_THIS, &host_zero);
  NSEEL_addfunc_varparm("set_pin_mapping", 1, NSEEL_PProc_THIS, &host_zero);
  NSEEL_addfunc_varparm("get_pinmapper_flags", 0, NSEEL_PProc_THIS, &host_zero);
  NSEEL_addfunc_varparm("set_pinmapper_flags", 1, NSEEL_PProc_THIS, &host_zero);

  NSEEL_addfunc_varparm("time", 0, NSEEL_PProc_THIS, &host_time);
  NSEEL_addfunc_varparm("time_precise", 0, NSEEL_PProc_THIS, &host_time_precise);

  /* fft, ifft, fft_real, ifft_real, fft_permute, fft_ipermute, convolve_c. */
  EEL_fft_register();
}
