/* Age of Mythology: the DLLs that are not lifted but replaced (high-level stubs). */
#include <stdio.h>

#include "rt.h"

/* EBUEulax.dll!EBUEula(a, b, c, d), cdecl: shows the licence dialog on the first
 * start and returns nonzero once the licence has been accepted. The player
 * accepted it when installing; the recompilation never shows it. Declared with
 * no arguments, so the dispatcher leaves the stack to the caller as cdecl does. */
H(eula_EBUEula) {
    UNUSED;
    if (g_opt.verbose) rt_log("[aom] EBUEula: licence accepted\n");
    return 1;
}

static const ImportDef defs[] = {
    {"ebueulax.dll", "EBUEula", eula_EBUEula, 0, 0},
};
RT_EXPORT_IMPORTS(rt_imports_aom, defs);
