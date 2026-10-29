// Host hooks required by WDL's EEL2 core.
//
// ns-eel.h documents that these only need to serialize RAM allocation (and
// shared-GRAM use) across threads. Phase 1 creates and destroys each VM on the
// main thread, and every instance gets its own RAM window, so empty stubs are
// correct. These are never called from the audio thread; when the Godot bridge
// adds concurrent compilation, replace the stubs with a process-wide mutex.

#include "ns-eel.h"

void NSEEL_HOSTSTUB_EnterMutex(void) {}
void NSEEL_HOSTSTUB_LeaveMutex(void) {}

// EEL_STRING_DEBUGOUT sink (PRD Story 1.1). WDL's eelscript layer calls this
// with printf-style arguments; the core does not compile that layer, but
// keeping a real definition makes the macro contract explicit and avoids
// pulling in a logging dependency. Replace with a Godot logger if WDL-side
// diagnostics are needed.
void jsfx_eel_debugout(const char* format, ...) { (void)format; }
