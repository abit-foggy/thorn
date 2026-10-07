#ifndef THORN_GRAPH_H
#define THORN_GRAPH_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "engine.h"

extern int g_errors;

/* Growable string buffer */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} SBuf;

int sb_init(SBuf *b);
int sb_add(SBuf *b, const char *s);
void sb_free(SBuf *b);

int has_whitespace(const char *s);
SourceKind classify_source(const char *path);
int is_c_source(const char *s);
int is_asm_source(const char *s);
int is_buildable_source(const char *s);
const char *type_name(int type);
const char *type_const(int type);
void obj_name(const Target *t, const char *src, char *out, size_t n);
int flags_text(const Target *t, SBuf *b);
int asflags_text(const Target *t, SBuf *b);
int ldflags_text(const Target *t, SBuf *b);
void out_name(char *out, size_t n, const char *pfx, const char *name);
void artifact_name(const Target *t, char *out, size_t n);

#endif /* THORN_GRAPH_H */
