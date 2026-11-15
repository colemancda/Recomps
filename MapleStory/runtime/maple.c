/* MapleStory: DLLs that are not lifted but replaced by high-level stubs, and the
 * (currently empty) guest-function hook table. */
#include <stdio.h>

#include "rt.h"

/* No lifted-code hooks yet. MapleStory's unpacked client (GMSv62.exe) statically
 * links its CRT and has no MSLU layer, so nothing like Age of Mythology's
 * _GetProcAddressInternal hook is needed. */
const RtHook rt_hooks[] = {{0}};
const unsigned rt_nhooks = 0;

/* GameGuard / nProtect KeyCrypt (npkcrypt.dll). The anti-cheat driver is not
 * loaded; every entry point reports success so the client proceeds. */
H(npk_open) { UNUSED; return 0; }              /* NPKOpenDriver: 0 = ok */
H(npk_ok) { UNUSED; return 1; }
H(npk_zero) { UNUSED; return 0; }
H(npk_msg) { UNUSED; return 0x8000; }          /* NPKRegisterCryptWindowMsg: a message id */

/* ijl15.dll (Intel JPEG Library): decodes the JPEG bitmaps in the .wz files.
 * Stubbed until lifted or reimplemented; ijlInit/ijlFree succeed, ijlRead/Write
 * report failure so a caller that checks will notice rather than read garbage. */
H(ijl_init) { UNUSED; return 0; }              /* IJL_OK */
H(ijl_free) { UNUSED; return 0; }
H(ijl_fail) { UNUSED; return -1; }             /* IJL_ERR_* */

/* WzMss.dll: the in-game bulletin board (SOAP over HTTP). Not reached at boot;
 * each call reports an error (nonzero HRESULT-like code). */
H(wz_soap_fail) { UNUSED; return 1; }

static const ImportDef defs[] = {
    {"npkcrypt.dll", "NPKOpenDriver", npk_open, 1, 0},
    {"npkcrypt.dll", "NPKCloseDriver", npk_zero, 0, 0},
    {"npkcrypt.dll", "NPKLoadAtStartup", npk_ok, 1, 0},
    {"npkcrypt.dll", "NPKSetDrvPath", npk_ok, 1, 0},
    {"npkcrypt.dll", "NPKGetAppCompatFlag", npk_zero, 0, 0},
    {"npkcrypt.dll", "NPKSetAppCompatFlag", npk_zero, 1, 0},
    {"npkcrypt.dll", "NPKRegisterCryptWindowMsg", npk_msg, 1, 0},

    {"ijl15.dll", "ijlInit", ijl_init, 1, 0},
    {"ijl15.dll", "ijlFree", ijl_free, 1, 0},
    {"ijl15.dll", "ijlRead", ijl_fail, 2, 0},
    {"ijl15.dll", "ijlWrite", ijl_fail, 2, 0},

    {"wzmss.dll", "?WzSoap_GetArticleContent@@YAJEH_NPBGPAG@Z", wz_soap_fail, 5, 0},
    {"wzmss.dll", "?WzSoap_CommentArticle@@YAJEHPBGEH00H_NPAG@Z", wz_soap_fail, 11, 0},
    {"wzmss.dll", "?WzSoap_WriteArticle@@YAJEHPBGE000HPAG1_N1@Z", wz_soap_fail, 12, 0},
    {"wzmss.dll", "?WzSoap_GetArticleOwner@@YAJEHPBGEH_NPAH@Z", wz_soap_fail, 7, 0},
    {"wzmss.dll", "?WzSoap_GetArticleTitle@@YAJEHPBGEH_NPAG@Z", wz_soap_fail, 7, 0},
    {"wzmss.dll", "?WzSoap_ModifyArticle@@YAJEHPBG0_N0PAG@Z", wz_soap_fail, 7, 0},
    {"wzmss.dll", "?WzSoap_DeleteArticle@@YAJEH_NPBGPAG@Z", wz_soap_fail, 5, 0},
};
RT_EXPORT_IMPORTS(rt_imports_maple, defs);
