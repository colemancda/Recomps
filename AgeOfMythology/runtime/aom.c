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

/* ------------------------------------------------------------------ hooks */

/* aomx.exe 0x4cd815, MSLU's _GetProcAddressInternal@8(hModule, name). The Unicode
 * layer finds the system W functions by walking each DLL's export directory
 * itself; the runtime's DLL handles have no PE image behind them. Answered from
 * the import handlers instead: a function without a handler gets a stub that
 * stops with its name if it is ever called. */
H(hook_mslu_GetProcAddress) {
    UNUSED;
    if (!a[0] || !a[1]) return 0;
    RtImage *im = rt_image_at(a[0]);
    if (im) return rt_image_export(im, rt_gstr(a[1]), 0);
    const char *dll = rt_module_name(a[0]);
    if (!dll) {
        rt_log("[aom] MSLU lookup of \"%s\" in unknown module %#x\n", rt_gstr(a[1]), a[0]);
        return 0;
    }
    const char *name = rt_gstr(a[1]);
    const ImportDef *def = rt_find_import_def(dll, name);
    if (g_opt.verbose && !def) rt_log("[aom] MSLU: %s!%s has no handler\n", dll, name);
    return rt_host_fn(dll, name, def ? def->fn : NULL, def ? def->nargs : 0);
}

const RtHook rt_hooks[] = {
    {0x004cd815, "mslu_GetProcAddressInternal", hook_mslu_GetProcAddress, 2},
};
const unsigned rt_nhooks = RT_COUNTOF(rt_hooks);

static const ImportDef defs[] = {
    {"ebueulax.dll", "EBUEula", eula_EBUEula, 0, 0},
};
RT_EXPORT_IMPORTS(rt_imports_aom, defs);
