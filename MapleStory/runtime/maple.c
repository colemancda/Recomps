/* MapleStory: DLLs that are not lifted but replaced by high-level stubs, and the
 * (currently empty) guest-function hook table. */
#include <stdio.h>

#include "rt.h"

/* No lifted-code hooks needed. GameGuard's anti-hack scanner (0x672c93) is a
 * DLL-planting check (system DLLs must not be files in the game folder); lifted
 * system DLLs live in a syslib/ subfolder the scan does not see (loader.c,
 * build.sh). See docs/SURVEY.md. */
const RtHook rt_hooks[] = {{0}};
const unsigned rt_nhooks = 0;

/* GameGuard / nProtect KeyCrypt (npkcrypt.dll). The anti-cheat driver is not
 * loaded; every entry point reports success so the client proceeds. The
 * KeyCrypt init (FUN_00867060) stores NPKOpenDriver's return and throws
 * CSecurityKeyCryptInitFailed if it is <= 0, so NPKOpenDriver must report a
 * positive handle; and it treats NPKGetAppCompatFlag == -1 as "compatible, no
 * action", so that returns -1 to skip the app-compat error path. */
H(npk_open) { UNUSED; return 1; }              /* NPKOpenDriver: a positive handle = ok */
H(npk_compat_ok) { UNUSED; return 0xFFFFFFFFu; } /* NPKGetAppCompatFlag: -1 = compatible */
H(npk_ok) { UNUSED; return 1; }
H(npk_zero) { UNUSED; return 0; }
H(npk_msg) { UNUSED; return 0x8000; }          /* NPKRegisterCryptWindowMsg: a message id */

/* WzMss.dll: the in-game bulletin board (SOAP over HTTP). Not reached at boot;
 * each call reports an error (nonzero HRESULT-like code). */
H(wz_soap_fail) { UNUSED; return 1; }

static const ImportDef defs[] = {
    {"npkcrypt.dll", "NPKOpenDriver", npk_open, 1, 0},
    {"npkcrypt.dll", "NPKCloseDriver", npk_zero, 0, 0},
    {"npkcrypt.dll", "NPKLoadAtStartup", npk_ok, 1, 0},
    {"npkcrypt.dll", "NPKSetDrvPath", npk_ok, 1, 0},
    {"npkcrypt.dll", "NPKGetAppCompatFlag", npk_compat_ok, 0, 0},
    {"npkcrypt.dll", "NPKSetAppCompatFlag", npk_zero, 1, 0},
    {"npkcrypt.dll", "NPKRegisterCryptWindowMsg", npk_msg, 1, 0},

    {"wzmss.dll", "?WzSoap_GetArticleContent@@YAJEH_NPBGPAG@Z", wz_soap_fail, 5, 0},
    {"wzmss.dll", "?WzSoap_CommentArticle@@YAJEHPBGEH00H_NPAG@Z", wz_soap_fail, 11, 0},
    {"wzmss.dll", "?WzSoap_WriteArticle@@YAJEHPBGE000HPAG1_N1@Z", wz_soap_fail, 12, 0},
    {"wzmss.dll", "?WzSoap_GetArticleOwner@@YAJEHPBGEH_NPAH@Z", wz_soap_fail, 7, 0},
    {"wzmss.dll", "?WzSoap_GetArticleTitle@@YAJEHPBGEH_NPAG@Z", wz_soap_fail, 7, 0},
    {"wzmss.dll", "?WzSoap_ModifyArticle@@YAJEHPBG0_N0PAG@Z", wz_soap_fail, 7, 0},
    {"wzmss.dll", "?WzSoap_DeleteArticle@@YAJEH_NPBGPAG@Z", wz_soap_fail, 5, 0},
};
RT_EXPORT_IMPORTS(rt_imports_maple, defs);
