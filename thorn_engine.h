/*
 * thorn_engine.h - internal shared model for the thorn build engine.
 *
 * thorn is a native meta-build generator that dogfoods The Pith
 * Programming Language (~/pith): the project specification (build.thorn)
 * is a Pith script compiled AOT to an object file and linked directly
 * against this C99 engine through Pith's C ABI.
 *
 * This header is shared between thorn_engine.c (graph, emitters, CLI)
 * and thorn_decompile.c (build.ninja / Makefile ingestion). It is NOT
 * the Pith-facing ABI; the engine functions Pith calls are the
 * non-static definitions in thorn_engine.c, renamed at compile time to
 * their c_thorn_engine_* link symbols (see the bootstrap Makefile).
 *
 * Identifier discipline: no identifier in these sources may collide
 * with a Pith ABI stem (project, exe, static_lib, shared_lib,
 * add_target, add_source, add_cflag, add_ldflag, add_include,
 * add_order_dep, pkg_config) because the bootstrap compiles the
 * engine with -D<stem>=c_thorn_engine_<stem> preprocessor renames,
 * mirroring exactly what pith does when it compiles imported C units.
 */
#ifndef THORN_ENGINE_H
#define THORN_ENGINE_H

#include <stddef.h>
#include <stdio.h>

#define THORN_VERSION "0.1.0"

/* Target types (the values thorn_engine.exe / .static_lib / .shared_lib
 * return to Pith as zero-argument pseudo-constants). */
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
    char proj[256];                 /* project name (ninja/make thorn_project var) */
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
/* Emitters (thorn_engine.c)                                          */
/* ------------------------------------------------------------------ */

/* Both emitters are deterministic: identical graph + identical
 * environment (CC/AR) produce byte-identical output. Returns 0 on
 * success. */
int thorn_emit_ninja(const Graph *g, const char *cc, const char *ar,
                     const char *path);
int thorn_emit_makefile(const Graph *g, const char *cc, const char *ar,
                        const char *path);

/* ------------------------------------------------------------------ */
/* build.thorn printer (thorn_engine.c, used by the decompiler)       */
/* ------------------------------------------------------------------ */

/* Print an idiomatic build.thorn for `g`. `notes` are appended as
 * trailing comments (unmodeled edges, caveats). Returns 0 on success. */
int thorn_print_spec(const Graph *g, FILE *out, const char **notes,
                     size_t nnotes);

/* ------------------------------------------------------------------ */
/* Reverse decompilation (thorn_decompile.c)                          */
/* ------------------------------------------------------------------ */

/*
 * Parse `path` (a build.ninja or a Makefile; detected by content) into
 * `g`. On success returns 0, fills `g`, and hands the caller a
 * malloc'd array of malloc'd note strings through `notes` (free each
 * element and the array itself).
 */
int thorn_decompile_file(const char *path, Graph *g, char ***notes,
                         size_t *nnotes);

#endif /* THORN_ENGINE_H */
