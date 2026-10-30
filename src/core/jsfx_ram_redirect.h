/* Force-included into WDL's nseel-ram.c only (see CMakeLists.txt), so EEL2's
 * memory blocks come from jsfx_ram_calloc without editing vendored code.
 * stdlib.h is included first so its own calloc declaration is unaffected; the
 * macro then rewrites only the calls in nseel-ram.c. */
#ifndef JSFX_RAM_REDIRECT_H
#define JSFX_RAM_REDIRECT_H

#include <stddef.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif
void* jsfx_ram_calloc(size_t count, size_t size);
#ifdef __cplusplus
}
#endif

#define calloc jsfx_ram_calloc

#endif /* JSFX_RAM_REDIRECT_H */
