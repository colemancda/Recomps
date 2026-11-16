/* The game's imports: one row per stub at the end of s3e_code (build/gen/imports.c). */
#ifndef CODZ_IMPORTS_H
#define CODZ_IMPORTS_H

#include <stdint.h>

enum { IMPORT_S3E, IMPORT_GL, IMPORT_DATA };
enum { RET_VOID, RET_I32, RET_I64, RET_DATA };

typedef struct ImportStub {
    uint32_t stub;      /* stub address in s3e_code */
    uint32_t impl;      /* implementation in the loader's __text (or data address) */
    uint8_t ret;        /* RET_*: from the loader trampoline */
    uint8_t stack_args; /* the trampoline copies stacked arguments */
    uint8_t kind;       /* IMPORT_* */
    const char *name;   /* GL function name, or the s3e subsystem guess */
} ImportStub;

extern const ImportStub g_import_stubs[];
extern const uint32_t g_import_stub_count;

#endif
