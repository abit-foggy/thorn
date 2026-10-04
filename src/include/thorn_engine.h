/*
 * thorn_engine.h - internal shared model for the thorn build engine.
 *
 * thorn is a lean meta-build generator in the QBE spirit that embeds
 * The Pith Programming Language (~/pith) the way a game engine embeds
 * a scripting language: the thorn binary carries pith's frontend and
 * evaluates build.thorn at runtime, with the engine's build API
 * registered as a host namespace (thorn_engine.*).
 *
 * This header is shared between src/thorn_engine.c (graph, host API,
 * emitters), src/thorn_main.c (CLI), and src/thorn_decompile.c
 * (build.ninja / Makefile ingestion). It is NOT the host-facing ABI;
 * the functions pith calls are the non-static definitions in
 * src/thorn_engine.c, compiled under -D<stem>=c_thorn_engine_<stem>
 * symbol renames (the same author-aware mangling pith applies to
 * imported C units) so the temp-executable fallback can link them.
 *
 * Identifier discipline: no identifier in these sources may collide
 * with a host API stem (project, exe, static_lib, shared_lib,
 * add_target, add_source, add_cflag, add_ldflag, add_include,
 * add_order_dep, pkg_config, emit) because the engine is compiled
 * with those -D renames.
 */
#ifndef THORN_ENGINE_H
#define THORN_ENGINE_H

#include <stddef.h>
#include <stdio.h>

#include <pith_embed.h>

#define THORN_VERSION "0.2.0"

/* Target types (the values thorn_engine.exe / .static_lib /
 * .shared_lib return to pith as zero-argument pseudo-constants). */
enum {
    THORN_EXE = 1,
    THORN_STATIC_LIB = 2,
    THORN_SHARED_LIB = 3
};

/* ------------------------------------------------------------------ */
/* Model                                                              */
/* ------------------------------------------------------------------ */

/* An append-only, de-duplicating list of owned strings. */
typedef struct StrList {
    char **items;
    size_t count;
    size_t cap;
} StrList;

typedef struct Target {
    char name[256];
    int type;                       /* THORN_EXE / _STATIC_LIB / _SHARED_LIB */
    StrList sources;                /* .c compiled per-target; others link raw */
    StrList cflags;
    StrList ldflags;
    StrList includes;               /* -I directories */
    StrList order_deps;             /* regeneration-order prerequisites */
} Target;

typedef struct Graph {
    char proj[256];                 /* project name (thorn_project var) */
    Target *targets;
    size_t count;
    size_t cap;
} Graph;

void graph_init(Graph *g);
void graph_free(Graph *g);
Target *graph_find(Graph *g, const char *name);
Target *graph_add(Graph *g, const char *name, int type);

/* Append a copy of `s`; returns 1 when newly added, 0 when a duplicate. */
int strlist_push(StrList *l, const char *s);
/* Append without de-duplication. */
int strlist_push_force(StrList *l, const char *s);
/* Free every item, the array, and reset the list. */
void strlist_free(StrList *l);

/* ------------------------------------------------------------------ */
/* Diagnostics & path helpers                                         */
/* ------------------------------------------------------------------ */

/* "thorn: <msg>" on stderr. */
void thorn_diag(const char *fmt, ...);

/* "<dir>/<name>", or the bare name for ".". */
void thorn_join_path(char *out, size_t n, const char *dir,
                     const char *name);

/* The output-name prefix for a backend file: the file's directory
 * with a trailing slash ("" when the file sits in the cwd). */
void thorn_dir_prefix(const char *path, char *prefix, size_t n);

/* mkdir -p (POSIX); 0 on success. */
int thorn_makedirs(const char *dir);

/* ------------------------------------------------------------------ */
/* Emitters (src/thorn_engine.c)                                      */
/* ------------------------------------------------------------------ */

/*
 * Both emitters are deterministic (identical graph and environment
 * produce identical bytes) and prefix every OUTPUT name with the
 * directory of `path` (build.ninja / Makefile generated into
 * out/artifacts place their objects and binaries there), while
 * source inputs stay project-root relative: the backends are invoked
 * from the project root (samu -f out/artifacts/build.ninja,
 * make -f out/artifacts/Makefile). Returns 0 on success.
 */
int thorn_emit_ninja(const Graph *g, const char *cc, const char *ar,
                     const char *path);
int thorn_emit_makefile(const Graph *g, const char *cc, const char *ar,
                        const char *path);

/* ------------------------------------------------------------------ */
/* build.thorn printer (used by the decompiler)                       */
/* ------------------------------------------------------------------ */

/* Print an idiomatic top-level build.thorn for `g`. `notes` are
 * appended as trailing comments. Returns 0 on success. */
int thorn_print_spec(const Graph *g, FILE *out, const char **notes,
                     size_t nnotes);

/* ------------------------------------------------------------------ */
/* Host registration (src/thorn_main.c)                               */
/* ------------------------------------------------------------------ */

/*
 * Register the full thorn_engine.* host namespace on `ctx`: the
 * configuration API plus the emit() trigger. Every function returns
 * an int ('w') so pith can use calls as statements or test them.
 * Lives in the CLI (never linked into the temp-executable fallback
 * child) and references the engine through its c_thorn_engine_*
 * symbols. Returns 0 on success.
 */
int thorn_host_register(PithContext *ctx);

/* ------------------------------------------------------------------ */
/* Reverse decompilation (src/thorn_decompile.c)                      */
/* ------------------------------------------------------------------ */

/*
 * Parse `path` (a build.ninja or a Makefile; detected by content)
 * into `g`. Target names carry the backend file's directory prefix
 * when it was generated outside the project root (out/artifacts/);
 * the prefix is stripped so decompiled specs use clean names. On
 * success returns 0, fills `g`, and hands the caller a malloc'd
 * array of malloc'd note strings through `notes` (free each element
 * and the array itself).
 */
int thorn_decompile_file(const char *path, Graph *g, char ***notes,
                         size_t *nnotes);

#endif /* THORN_ENGINE_H */
