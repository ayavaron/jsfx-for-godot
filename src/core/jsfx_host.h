/* JSFX host API shared between the C builtins (jsfx_host.c) and the C++
 * EEL2 wrapper. C-compatible on purpose: WDL's function glue is C. */
#ifndef JSFX_HOST_H
#define JSFX_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

#define JSFX_HOST_MAX_SLIDERS 256
#define JSFX_HOST_MAX_SPL 64

/* Per-VM state handed to EEL2 through NSEEL_VM_SetCustomFuncThis. Slider and
 * spl pointers refer to EEL2 variable storage owned by the VM. */
typedef struct JsfxHostState {
  double* sliders[JSFX_HOST_MAX_SLIDERS];
  double* spl[JSFX_HOST_MAX_SPL];
  /* Target for slider(n)/spl(n) with an out-of-range index. */
  double scratch;
} JsfxHostState;

/* Backs the first `slots` EEL2 memory slots of `vm` with one virtual-memory
 * region from the OS. Pages are zero-filled by the OS on first touch, so an
 * effect only consumes RAM for the memory its script actually uses, and no
 * allocator runs later on the audio thread. Returns the region, or NULL where
 * this is unavailable (the caller then preallocates with calloc).
 * Must be called on a fresh VM before any code runs. */
void* jsfx_host_map_ram(void* vm, int slots);

/* Detaches and releases a region from jsfx_host_map_ram. Call before
 * NSEEL_VM_free, which would otherwise free() the block pointers. */
void jsfx_host_unmap_ram(void* vm, void* region, int slots);

/* Registers JSFX's host functions (slider(), spl(), sliderchange(), the MIDI
 * API as no-ops, time(), the FFT family, ...) in EEL2's global function table.
 * Not thread-safe; call once before the first compile. */
void jsfx_host_register_functions(void);

#ifdef __cplusplus
}
#endif

#endif /* JSFX_HOST_H */
