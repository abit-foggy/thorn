#ifndef THORN_LEX_H
#define THORN_LEX_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"

int pat_match(const char *pat, const char *str);
int matches_any(const StrList *list, const char *str);
void note_add(char ***notes, size_t *n, const char *fmt, ...);
int tok_push(char ***arr, size_t *n, const char *tok);
char **split_owned(const char *text, size_t *count);
void free_tokens(char **toks, size_t n);
char *trim_inplace(char *s);
int ends_with(const char *s, const char *suffix);
char *read_all(const char *path);
int is_cc_token(const char *tok);
int is_as_token(const char *tok);
int is_source_ext(const char *path);
char *swap_ext_source(const char *path);
char *prefix_scoped_path(const char *path, const char *scope);
const char *strip_pfx(const char *s, const char *pfx);
const char *clean_target(const char *raw, const char *pfx, char *buf, size_t n);

enum {
    RC_PHONY = 0,
    RC_AR,
    RC_COMPILE,
    RC_SHARED,
    RC_LINK,
    RC_OTHER
};

int classify_tokens(char **toks, size_t n);
int classify_text(const char *text);
void harvest_flags_text(const char *text, Target *t);
void harvest_asflags_text(const char *text, Target *t);
void harvest_cmd_compile_text(const char *text, Target *t);
void harvest_cmd_link_text(const char *text, Target *t);

#endif /* THORN_LEX_H */
