/*
 * thorn_engine.c - the C99 host engine for thorn, a native meta-build
 * generator that dogfoods The Pith Programming Language.
 *
 * A thorn is the sharp spine on the stem of a plant; it grows on the
 * pith. This tool is the sharp point of the pith toolchain: lean,
 * minimal, zero-bloat, in the QBE spirit.
 *
 * Pipeline (see the bootstrap Makefile for the exact commands):
 *
 *   build.thorn --(pith build --plugin)--> build_thorn.o
 *       exports c_thorn_<author>_<name>_thorn_configure (zero params,
 *       64-bit return), calls back into this engine through the
 *       c_thorn_engine_* C ABI
 *   thorn_engine.c --(cc, -D<stem>=c_thorn_engine_<stem> renames)--> thorn_engine.o
 *   thorn_decompile.c --> thorn_decompile.o
 *   $CC links engine + decompiler + plugin object + pith's
 *   runtime/libruntime.a --> the project's thorn binary
 *
 * thorn's main() parses the CLI, invokes the compiled-in Pith entry
 * point (thorn_configure()), collects the build graph, and emits a
 * deterministic build.ninja (samurai/ninja) and a portable Makefile.
 *
 * Pith ABI notes (verified against pith 0.1.0 sources):
 *   - string parameters are borrowed PithValue* (NUL-terminated data,
 *     do not free; copy with strdup when storing)
 *   - int parameters/returns are 32-bit (w class); Pith narrows its
 *     64-bit integers automatically
 *   - every ABI function returns int so Pith can call it as a bare
 *     statement or test it with `if`
 *   - zero-parameter ABI functions double as pseudo-constants in
 *     Pith: `thorn_engine.exe` (bare member access emits the call)
 *
 * Identifier discipline: this file is compiled with
 *   -Dproject=c_thorn_engine_project -Dexe=c_thorn_engine_exe ...
 * so no other identifier here may reuse an ABI stem.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pith.h>

#include "thorn_engine.h"

/* The Pith entry point baked into this binary. Each project
 * overrides it at engine compile time; the symbol contract is
 * c_thorn_<pith.toml author>_<pith.toml name>_thorn_configure. */
#ifndef THORN_CONFIGURE_SYMBOL
#define THORN_CONFIGURE_SYMBOL c_thorn_build_thorn_configure
#endif

/* ------------------------------------------------------------------ */
/* Diagnostics                                                        */
/* ------------------------------------------------------------------ */

static void thorn_diag(const char *fmt, ...)
{
    va_list ap;
    fputs("thorn: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ------------------------------------------------------------------ */
/* Growable string buffer                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} SBuf;

static int sb_init(SBuf *b)
{
    b->cap = 128;
    b->len = 0;
    b->buf = malloc(b->cap);
    if (!b->buf)
        return -1;
    b->buf[0] = '\0';
    return 0;
}

static int sb_add(SBuf *b, const char *s)
{
    size_t sl = strlen(s);
    if (b->len + sl + 1 > b->cap) {
        while (b->len + sl + 1 > b->cap)
            b->cap *= 2;
        char *nb = realloc(b->buf, b->cap);
        if (!nb)
            return -1;
        b->buf = nb;
    }
    memcpy(b->buf + b->len, s, sl);
    b->len += sl;
    b->buf[b->len] = '\0';
    return 0;
}

static void sb_free(SBuf *b)
{
    free(b->buf);
    b->buf = NULL;
    b->len = b->cap = 0;
}

/* ------------------------------------------------------------------ */
/* Graph model                                                        */
/* ------------------------------------------------------------------ */

int strlist_push_force(StrList *l, const char *s)
{
    if (l->count == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 8;
        char **ni = realloc(l->items, nc * sizeof(char *));
        if (!ni)
            return 0;
        l->items = ni;
        l->cap = nc;
    }
    char *d = strdup(s);
    if (!d)
        return 0;
    l->items[l->count++] = d;
    return 1;
}

int strlist_push(StrList *l, const char *s)
{
    for (size_t i = 0; i < l->count; i++)
        if (strcmp(l->items[i], s) == 0)
            return 0;
    return strlist_push_force(l, s);
}

void strlist_free(StrList *l)
{
    for (size_t i = 0; i < l->count; i++)
        free(l->items[i]);
    free(l->items);
    memset(l, 0, sizeof(*l));
}

void graph_init(Graph *g)
{
    memset(g, 0, sizeof(*g));
    snprintf(g->proj, sizeof(g->proj), "project");
}

void graph_free(Graph *g)
{
    for (size_t i = 0; i < g->count; i++) {
        strlist_free(&g->targets[i].sources);
        strlist_free(&g->targets[i].cflags);
        strlist_free(&g->targets[i].ldflags);
        strlist_free(&g->targets[i].includes);
        strlist_free(&g->targets[i].order_deps);
    }
    free(g->targets);
    memset(g, 0, sizeof(*g));
}

Target *graph_find(Graph *g, const char *name)
{
    for (size_t i = 0; i < g->count; i++)
        if (strcmp(g->targets[i].name, name) == 0)
            return &g->targets[i];
    return NULL;
}

Target *graph_add(Graph *g, const char *name, int type)
{
    if (graph_find(g, name))
        return NULL;
    if (g->count == g->cap) {
        size_t nc = g->cap ? g->cap * 2 : 8;
        Target *nt = realloc(g->targets, nc * sizeof(Target));
        if (!nt)
            return NULL;
        g->targets = nt;
        g->cap = nc;
    }
    Target *t = &g->targets[g->count++];
    memset(t, 0, sizeof(*t));
    snprintf(t->name, sizeof(t->name), "%s", name);
    t->type = type;
    return t;
}

/* ------------------------------------------------------------------ */
/* The Pith-facing C ABI                                              */
/* ------------------------------------------------------------------ */

static Graph g_graph;
static int g_ready;

static void ensure_graph(void)
{
    if (!g_ready) {
        graph_init(&g_graph);
        g_ready = 1;
    }
}

/* Shared precondition: resolve the target named by the first string
 * argument of an add_* call. */
static Target *want_target(const char *api, PithValue *name)
{
    ensure_graph();
    if (!name) {
        thorn_diag("%s() expects a target name string", api);
        return NULL;
    }
    const char *n = pithStringData(name);
    Target *t = graph_find(&g_graph, n);
    if (!t)
        thorn_diag("%s(): unknown target `%s` (declare it with "
                   "add_target first)", api, n);
    return t;
}

/* int returns cross to Pith as 32-bit words, widened via extsw. */

int project(PithValue *name)
{
    ensure_graph();
    if (!name) {
        thorn_diag("project() expects a project name string");
        return 0;
    }
    snprintf(g_graph.proj, sizeof(g_graph.proj), "%s",
             pithStringData(name));
    return 1;
}

/* Zero-argument pseudo-constants: Pith reads these through bare
 * member access (`thorn_engine.exe`). */
int exe(void)          { return THORN_EXE; }
int static_lib(void)   { return THORN_STATIC_LIB; }
int shared_lib(void)  { return THORN_SHARED_LIB; }

int add_target(PithValue *name, int type)
{
    ensure_graph();
    if (!name) {
        thorn_diag("add_target() expects a target name string");
        return 0;
    }
    if (type < THORN_EXE || type > THORN_SHARED_LIB) {
        thorn_diag("add_target(): the type must be thorn_engine.exe, "
                   "thorn_engine.static_lib or thorn_engine.shared_lib");
        return 0;
    }
    const char *n = pithStringData(name);
    if (!n[0]) {
        thorn_diag("add_target(): the name must not be empty");
        return 0;
    }
    if (graph_find(&g_graph, n)) {
        thorn_diag("add_target(): duplicate target `%s`", n);
        return 0;
    }
    if (!graph_add(&g_graph, n, type)) {
        thorn_diag("add_target(): out of memory for `%s`", n);
        return 0;
    }
    return 1;
}

int add_source(PithValue *target, PithValue *src)
{
    Target *t = want_target("add_source", target);
    if (!t)
        return 0;
    if (!src) {
        thorn_diag("add_source() expects a source path string");
        return 0;
    }
    const char *s = pithStringData(src);
    if (!strlist_push(&t->sources, s)) {
        thorn_diag("warning: duplicate source `%s` on target `%s` "
                   "skipped", s, t->name);
        return 1;
    }
    return 1;
}

int add_cflag(PithValue *target, PithValue *flag)
{
    Target *t = want_target("add_cflag", target);
    if (!t)
        return 0;
    if (!flag) {
        thorn_diag("add_cflag() expects a flag string");
        return 0;
    }
    strlist_push(&t->cflags, pithStringData(flag));
    return 1;
}

int add_ldflag(PithValue *target, PithValue *flag)
{
    Target *t = want_target("add_ldflag", target);
    if (!t)
        return 0;
    if (!flag) {
        thorn_diag("add_ldflag() expects a flag string");
        return 0;
    }
    strlist_push(&t->ldflags, pithStringData(flag));
    return 1;
}

int add_include(PithValue *target, PithValue *dir)
{
    Target *t = want_target("add_include", target);
    if (!t)
        return 0;
    if (!dir) {
        thorn_diag("add_include() expects a directory string");
        return 0;
    }
    strlist_push(&t->includes, pithStringData(dir));
    return 1;
}

int add_order_dep(PithValue *target, PithValue *prereq)
{
    Target *t = want_target("add_order_dep", target);
    if (!t)
        return 0;
    if (!prereq) {
        thorn_diag("add_order_dep() expects a prerequisite string");
        return 0;
    }
    strlist_push(&t->order_deps, pithStringData(prereq));
    return 1;
}

/*
 * thorn_engine.pkg_config(target, pkg) runs `pkg-config --cflags
 * --libs <pkg>` and folds the result into the target: -I dirs become
 * includes, -l/-L become ldflags, everything else becomes cflags.
 * (Pith has no string-splitting primitives, so the split lives here.)
 */
int pkg_config(PithValue *target, PithValue *pkg)
{
    Target *t = want_target("pkg_config", target);
    if (!t)
        return 0;
    if (!pkg) {
        thorn_diag("pkg_config() expects a package name string");
        return 0;
    }
    const char *pn = pithStringData(pkg);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "pkg-config --cflags --libs '%s'",
             pn);
    FILE *p = popen(cmd, "r");
    if (!p) {
        thorn_diag("pkg_config(): cannot run pkg-config");
        return 0;
    }
    char line[8192];
    if (!fgets(line, sizeof(line), p))
        line[0] = '\0';
    int st = pclose(p);
    if (st != 0) {
        thorn_diag("pkg_config(): `pkg-config %s` failed (is the "
                   "package installed?)", pn);
        return 0;
    }

    char *w = line;
    while (*w) {
        while (*w == ' ' || *w == '\t' || *w == '\n' || *w == '\r')
            w++;
        if (!*w)
            break;
        char *tok = w;
        while (*w && *w != ' ' && *w != '\t' && *w != '\n' && *w != '\r')
            w++;
        if (*w)
            *w++ = '\0';
        if (strncmp(tok, "-I", 2) == 0 && tok[2])
            strlist_push(&t->includes, tok + 2);
        else if (strncmp(tok, "-L", 2) == 0 ||
                 strncmp(tok, "-l", 2) == 0)
            strlist_push(&t->ldflags, tok);
        else
            strlist_push(&t->cflags, tok);
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Naming helpers                                                     */
/* ------------------------------------------------------------------ */

static int is_c_source(const char *s)
{
    size_t l = strlen(s);
    return l >= 2 && s[l - 2] == '.' && s[l - 1] == 'c';
}

static const char *type_name(int type)
{
    switch (type) {
    case THORN_STATIC_LIB:  return "static library";
    case THORN_SHARED_LIB: return "shared library";
    default:                return "executable";
    }
}

static const char *type_const(int type)
{
    switch (type) {
    case THORN_STATIC_LIB:  return "static_lib";
    case THORN_SHARED_LIB: return "shared_lib";
    default:                return "exe";
    }
}

/*
 * Flat object path for BOTH backends:
 * <target>__<source with '/' folded to '_' and .c swapped to .o>.
 *
 * Flat naming avoids mkdir (portable make) and avoids the dir/file
 * collision of prefixing objects with the target name ("thorn/" as a
 * directory cannot coexist with the "thorn" executable). The
 * decompiler never needs to invert this name: compile edges and
 * recipes carry the true source paths.
 */
static void obj_name(const Target *t, const char *src, char *out,
                      size_t n)
{
    char tmp[512];
    size_t o = 0;
    for (const char *p = src; *p && o + 1 < sizeof(tmp); p++)
        tmp[o++] = (*p == '/') ? '_' : *p;
    tmp[o] = '\0';
    if (o >= 2 && tmp[o - 2] == '.' && tmp[o - 1] == 'c')
        tmp[o - 1] = 'o';
    snprintf(out, n, "%s__%s", t->name, tmp);
}

/* "-Iinc -Iinc2 -O2" text: includes first, then bare cflags.
 * Deterministic order that round-trips through the decompiler. */
static int flags_text(const Target *t, SBuf *b)
{
    if (sb_init(b) != 0)
        return -1;
    for (size_t i = 0; i < t->includes.count; i++) {
        sb_add(b, "-I");
        sb_add(b, t->includes.items[i]);
        if (i + 1 < t->includes.count || t->cflags.count)
            sb_add(b, " ");
    }
    for (size_t i = 0; i < t->cflags.count; i++) {
        sb_add(b, t->cflags.items[i]);
        if (i + 1 < t->cflags.count)
            sb_add(b, " ");
    }
    return 0;
}

static int ldflags_text(const Target *t, SBuf *b)
{
    if (sb_init(b) != 0)
        return -1;
    for (size_t i = 0; i < t->ldflags.count; i++) {
        sb_add(b, t->ldflags.items[i]);
        if (i + 1 < t->ldflags.count)
            sb_add(b, " ");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Ninja emitter                                                      */
/* ------------------------------------------------------------------ */

int thorn_emit_ninja(const Graph *g, const char *cc, const char *ar,
                     const char *path)
{
    if (g->count == 0) {
        thorn_diag("no targets declared; nothing to emit");
        return 1;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        thorn_diag("cannot write %s", path);
        return 1;
    }

    fprintf(f, "# build.ninja - generated by thorn " THORN_VERSION
               ", do not edit\n");
    fprintf(f, "# project: %s (deterministic: same graph and "
               "environment, same bytes)\n\n", g->proj);
    fprintf(f, "ninja_required_version = 1.3\n\n");
    fprintf(f, "thorn_project = %s\n", g->proj);
    fprintf(f, "cc = %s\n", cc);
    fprintf(f, "ar = %s\n\n", ar);

    fprintf(f, "rule cc\n");
    fprintf(f, "  command = $cc -MD -MF $out.d $cflags -c $in -o "
               "$out\n");
    fprintf(f, "  depfile = $out.d\n");
    fprintf(f, "  deps = gcc\n");
    fprintf(f, "  description = CC $out\n");
    fprintf(f, "rule ar\n");
    fprintf(f, "  command = $ar rcs $out $in\n");
    fprintf(f, "  description = AR $out\n");
    fprintf(f, "rule link\n");
    fprintf(f, "  command = $cc $in $ldflags -o $out\n");
    fprintf(f, "  description = LINK $out\n");
    fprintf(f, "rule solink\n");
    fprintf(f, "  command = $cc -shared $in $ldflags -o $out\n");
    fprintf(f, "  description = SOLINK $out\n\n");

    fprintf(f, "build %s_all: phony", g->proj);
    for (size_t i = 0; i < g->count; i++)
        fprintf(f, " %s", g->targets[i].name);
    fprintf(f, "\ndefault %s_all\n\n", g->proj);

    for (size_t i = 0; i < g->count; i++) {
        Target *t = &g->targets[i];
        SBuf ctext;
        flags_text(t, &ctext);

        fprintf(f, "# target %s (%s)\n", t->name, type_name(t->type));

        for (size_t s = 0; s < t->sources.count; s++) {
            const char *src = t->sources.items[s];
            if (!is_c_source(src))
                continue;   /* raw link input, appears on the edge */
            char obj[1024];
            obj_name(t, src, obj, sizeof(obj));
            fprintf(f, "build %s: cc %s", obj, src);
            /* order-only deps on compile edges: build the generated
             * prerequisite first, but header regeneration must not
             * recompile the world (ninja order-only semantics) */
            for (size_t d = 0; d < t->order_deps.count; d++)
                fprintf(f, " || %s", t->order_deps.items[d]);
            fprintf(f, "\n");
            if (ctext.len > 0)
                fprintf(f, "  cflags = %s\n", ctext.buf);
        }

        const char *rule = t->type == THORN_STATIC_LIB ? "ar"
                        : t->type == THORN_SHARED_LIB ? "solink"
                                                      : "link";
        fprintf(f, "build %s: %s", t->name, rule);
        for (size_t s = 0; s < t->sources.count; s++) {
            const char *src = t->sources.items[s];
            char obj[1024];
            if (is_c_source(src)) {
                obj_name(t, src, obj, sizeof(obj));
                fprintf(f, " %s", obj);
            } else {
                fprintf(f, " %s", src);
            }
        }
        /* order deps on link edges are implicit inputs: a regenerated
         * prereq (or a refreshed prebuilt object) must relink */
        for (size_t d = 0; d < t->order_deps.count; d++)
            fprintf(f, " | %s", t->order_deps.items[d]);
        fprintf(f, "\n");

        if (t->type != THORN_STATIC_LIB && t->ldflags.count > 0) {
            SBuf lt;
            ldflags_text(t, &lt);
            fprintf(f, "  ldflags = %s\n", lt.buf);
            sb_free(&lt);
        }
        fprintf(f, "\n");
        sb_free(&ctext);
    }

    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Makefile emitter                                                   */
/* ------------------------------------------------------------------ */

/* The lean case: one target, every source a flat .c file, no order
 * deps. Objects then share source names, so a single portable
 * pattern rule compiles them. Otherwise explicit per-object rules
 * are emitted (correct everywhere, including strict POSIX make). */
static int make_uses_pattern(const Graph *g)
{
    if (g->count != 1)
        return 0;
    Target *t = &g->targets[0];
    if (t->order_deps.count > 0)
        return 0;
    for (size_t i = 0; i < t->sources.count; i++)
        if (!is_c_source(t->sources.items[i]) ||
            strchr(t->sources.items[i], '/'))
            return 0;
    return 1;
}

int thorn_emit_makefile(const Graph *g, const char *cc, const char *ar,
                        const char *path)
{
    if (g->count == 0) {
        thorn_diag("no targets declared; nothing to emit");
        return 1;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        thorn_diag("cannot write %s", path);
        return 1;
    }
    int pattern = make_uses_pattern(g);

    fprintf(f, "# Makefile - generated by thorn " THORN_VERSION
               ", do not edit\n");
    fprintf(f, "# project: %s (deterministic: same graph and "
               "environment, same bytes)\n\n", g->proj);
    fprintf(f, "thorn_project = %s\n", g->proj);
    fprintf(f, "CC = %s\n", cc);
    fprintf(f, "AR = %s\n\n", ar);

    /* per-target variables */
    for (size_t i = 0; i < g->count; i++) {
        Target *t = &g->targets[i];
        fprintf(f, "%s_OBJS =", t->name);
        for (size_t s = 0; s < t->sources.count; s++) {
            const char *src = t->sources.items[s];
            if (!is_c_source(src))
                continue;
            char obj[1024];
            if (pattern) {
                snprintf(obj, sizeof(obj), "%s", src);
                obj[strlen(obj) - 1] = 'o';
            } else {
                obj_name(t, src, obj, sizeof(obj));
            }
            fprintf(f, " %s", obj);
        }
        fprintf(f, "\n");
        SBuf ctext;
        flags_text(t, &ctext);
        if (ctext.len > 0)
            fprintf(f, "%s_CFLAGS = %s\n", t->name, ctext.buf);
        sb_free(&ctext);
        if (t->type != THORN_STATIC_LIB && t->ldflags.count > 0) {
            SBuf lt;
            ldflags_text(t, &lt);
            fprintf(f, "%s_LDFLAGS = %s\n", t->name, lt.buf);
            sb_free(&lt);
        }
        fprintf(f, "\n");
    }

    /* all: the aggregate */
    fprintf(f, "all:");
    for (size_t i = 0; i < g->count; i++)
        fprintf(f, " %s", g->targets[i].name);
    fprintf(f, "\n\n");

    /* link/archive rules */
    for (size_t i = 0; i < g->count; i++) {
        Target *t = &g->targets[i];
        fprintf(f, "%s: $(%s_OBJS)", t->name, t->name);
        for (size_t s = 0; s < t->sources.count; s++)
            if (!is_c_source(t->sources.items[s]))
                fprintf(f, " %s", t->sources.items[s]);
        for (size_t d = 0; d < t->order_deps.count; d++)
            fprintf(f, " %s", t->order_deps.items[d]);
        fprintf(f, "\n");
        if (t->type == THORN_STATIC_LIB) {
            fprintf(f, "\t$(AR) rcs $@ $^\n\n");
        } else if (t->type == THORN_SHARED_LIB) {
            if (t->ldflags.count > 0)
                fprintf(f, "\t$(CC) -shared $^ $(%s_LDFLAGS) -o "
                           "$@\n\n", t->name);
            else
                fprintf(f, "\t$(CC) -shared $^ -o $@\n\n");
        } else {
            if (t->ldflags.count > 0)
                fprintf(f, "\t$(CC) $^ $(%s_LDFLAGS) -o $@\n\n",
                        t->name);
            else
                fprintf(f, "\t$(CC) $^ -o $@\n\n");
        }
    }

    /* compile rules */
    if (pattern) {
        Target *t = &g->targets[0];
        fprintf(f, "%%.o: %%.c\n");
        if (t->cflags.count > 0 || t->includes.count > 0)
            fprintf(f, "\t$(CC) $(%s_CFLAGS) -MMD -MP -c $< -o $@\n\n",
                    t->name);
        else
            fprintf(f, "\t$(CC) -MMD -MP -c $< -o $@\n\n");
    } else {
        for (size_t i = 0; i < g->count; i++) {
            Target *t = &g->targets[i];
            for (size_t s = 0; s < t->sources.count; s++) {
                const char *src = t->sources.items[s];
                if (!is_c_source(src))
                    continue;
                char obj[1024];
                obj_name(t, src, obj, sizeof(obj));
                fprintf(f, "%s: %s", obj, src);
                for (size_t d = 0; d < t->order_deps.count; d++)
                    fprintf(f, " %s", t->order_deps.items[d]);
                fprintf(f, "\n");
                if (t->cflags.count > 0 || t->includes.count > 0)
                    fprintf(f, "\t$(CC) $(%s_CFLAGS) -MMD -MP -c %s "
                               "-o $@\n\n", t->name, src);
                else
                    fprintf(f, "\t$(CC) -MMD -MP -c %s -o $@\n\n",
                            src);
            }
        }
    }

    /* clean */
    fprintf(f, "clean:\n\trm -f");
    for (size_t i = 0; i < g->count; i++) {
        Target *t = &g->targets[i];
        for (size_t s = 0; s < t->sources.count; s++) {
            const char *src = t->sources.items[s];
            if (!is_c_source(src))
                continue;
            char obj[1024];
            if (pattern) {
                snprintf(obj, sizeof(obj), "%s", src);
                obj[strlen(obj) - 1] = 'o';
                fprintf(f, " %s", obj);
            } else {
                obj_name(t, src, obj, sizeof(obj));
                fprintf(f, " %s", obj);
            }
        }
        fprintf(f, " %s", t->name);
    }
    fprintf(f, "\n\n");

    fprintf(f, ".PHONY: all clean\n\n");

    /* dependency fragments emitted by -MMD */
    fprintf(f, "-include");
    for (size_t i = 0; i < g->count; i++) {
        Target *t = &g->targets[i];
        for (size_t s = 0; s < t->sources.count; s++) {
            const char *src = t->sources.items[s];
            if (!is_c_source(src))
                continue;
            char dep[1024];
            if (pattern) {
                snprintf(dep, sizeof(dep), "%s", src);
                dep[strlen(dep) - 1] = 'd';
            } else {
                obj_name(t, src, dep, sizeof(dep));
                size_t dl = strlen(dep);
                dep[dl - 1] = 'd';
            }
            fprintf(f, " %s", dep);
        }
    }
    fprintf(f, "\n");

    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* build.thorn printer (used by thorn decompile)                      */
/* ------------------------------------------------------------------ */

int thorn_print_spec(const Graph *g, FILE *out, const char **notes,
                     size_t nnotes)
{
    fprintf(out, "# build.thorn - generated by thorn " THORN_VERSION
                 "\n");
    fprintf(out, "# decompiled from an existing build graph; review "
                 "before use\n\n");
    fprintf(out, "import \"thorn_engine.c\"\n\n");
    fprintf(out, "fn thorn_configure()\n");
    fprintf(out, "    thorn_engine.project(\"%s\")\n\n", g->proj);

    for (size_t i = 0; i < g->count; i++) {
        Target *t = &g->targets[i];
        fprintf(out, "    thorn_engine.add_target(\"%s\", "
                     "thorn_engine.%s)\n", t->name,
                type_const(t->type));
        for (size_t s = 0; s < t->sources.count; s++)
            fprintf(out, "    thorn_engine.add_source(\"%s\", \"%s\")"
                         "\n", t->name, t->sources.items[s]);
        for (size_t s = 0; s < t->includes.count; s++)
            fprintf(out, "    thorn_engine.add_include(\"%s\", \"%s\")"
                         "\n", t->name, t->includes.items[s]);
        for (size_t s = 0; s < t->cflags.count; s++)
            fprintf(out, "    thorn_engine.add_cflag(\"%s\", \"%s\")\n",
                    t->name, t->cflags.items[s]);
        for (size_t s = 0; s < t->ldflags.count; s++)
            fprintf(out, "    thorn_engine.add_ldflag(\"%s\", \"%s\")\n",
                    t->name, t->ldflags.items[s]);
        for (size_t s = 0; s < t->order_deps.count; s++)
            fprintf(out, "    thorn_engine.add_order_dep(\"%s\", "
                         "\"%s\")\n", t->name, t->order_deps.items[s]);
        fprintf(out, "\n");
    }
    fprintf(out, "end\n");

    for (size_t i = 0; i < nnotes; i++)
        fprintf(out, "\n# note: %s\n", notes[i]);
    return 0;
}

/* ------------------------------------------------------------------ */
/* CLI                                                                */
/* ------------------------------------------------------------------ */

static void usage(void)
{
    printf("thorn " THORN_VERSION " - a lean meta-build generator "
           "that dogfoods pith\n\n"
           "USAGE:\n"
           "    thorn                       emit build.ninja and "
           "Makefile in cwd\n"
           "    thorn build [--ninja|--make]\n"
           "    thorn decompile <build.ninja|Makefile> [-o out.thorn]\n"
           "    thorn help\n"
           "    thorn version\n\n"
           "The project specification (build.thorn) is compiled into "
           "this binary\n"
           "at link time; `thorn` invokes it, collects the graph, and "
           "writes the\n"
           "backends. CC/AR environment variables override the baked "
           "toolchain.\n");
}

static int cmd_build(int want_ninja, int want_make)
{
    graph_init(&g_graph);
    g_ready = 1;

    extern long THORN_CONFIGURE_SYMBOL(void);
    long rc = THORN_CONFIGURE_SYMBOL();
    if (rc != 0) {
        thorn_diag("thorn_configure() returned %ld", rc);
        graph_free(&g_graph);
        return 1;
    }
    if (g_graph.count == 0) {
        thorn_diag("the specification declared no targets");
        graph_free(&g_graph);
        return 1;
    }

    const char *cc = getenv("CC");
    if (!cc || !*cc)
        cc = "cc";
    const char *ar = getenv("AR");
    if (!ar || !*ar)
        ar = "ar";

    size_t total_sources = 0;
    for (size_t i = 0; i < g_graph.count; i++)
        total_sources += g_graph.targets[i].sources.count;

    int fail = 0;
    if (want_ninja)
        fail |= thorn_emit_ninja(&g_graph, cc, ar, "build.ninja");
    if (want_make)
        fail |= thorn_emit_makefile(&g_graph, cc, ar, "Makefile");

    if (!fail) {
        printf("thorn: configured project `%s` (%zu target%s, %zu "
               "source%s)\n", g_graph.proj, g_graph.count,
               g_graph.count == 1 ? "" : "s", total_sources,
               total_sources == 1 ? "" : "s");
        if (want_ninja)
            printf("thorn: wrote build.ninja (build with: samu or "
                   "ninja)\n");
        if (want_make)
            printf("thorn: wrote Makefile (build with: make)\n");
    }
    graph_free(&g_graph);
    g_ready = 0;
    return fail ? 1 : 0;
}

static int cmd_decompile(const char *in, const char *out)
{
    Graph g;
    graph_init(&g);
    char **notes = NULL;
    size_t nnotes = 0;

    if (thorn_decompile_file(in, &g, &notes, &nnotes) != 0) {
        graph_free(&g);
        return 1;
    }
    if (g.count == 0) {
        thorn_diag("no buildable targets found in %s", in);
        graph_free(&g);
        return 1;
    }

    FILE *f = fopen(out, "w");
    if (!f) {
        thorn_diag("cannot write %s", out);
        graph_free(&g);
        return 1;
    }
    thorn_print_spec(&g, f, (const char **)notes, nnotes);
    fclose(f);

    printf("thorn: decompiled %s into %s (%zu target%s, %zu note%s)\n",
           in, out, g.count, g.count == 1 ? "" : "s", nnotes,
           nnotes == 1 ? "" : "s");

    for (size_t i = 0; i < nnotes; i++)
        free(notes[i]);
    free(notes);
    graph_free(&g);
    return 0;
}

int main(int argc, char **argv)
{
    /* a leading flag implies the build verb: thorn --ninja */
    int argbase = 1;
    const char *verb = "build";
    if (argc >= 2 && argv[1][0] != '-') {
        verb = argv[1];
        argbase = 2;
    }

    if (strcmp(verb, "build") == 0) {
        int want_ninja = 1, want_make = 1;
        for (int i = argbase; i < argc; i++) {
            if (strcmp(argv[i], "--ninja") == 0)
                want_make = 0;
            else if (strcmp(argv[i], "--make") == 0)
                want_ninja = 0;
            else {
                thorn_diag("unknown flag `%s`", argv[i]);
                usage();
                return 2;
            }
        }
        return cmd_build(want_ninja, want_make);
    }

    if (strcmp(verb, "decompile") == 0) {
        const char *in = NULL;
        const char *out = "build.thorn";
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
                out = argv[++i];
            } else if (!in) {
                in = argv[i];
            } else {
                thorn_diag("unexpected argument `%s`", argv[i]);
                usage();
                return 2;
            }
        }
        if (!in) {
            thorn_diag("decompile expects an input file");
            usage();
            return 2;
        }
        return cmd_decompile(in, out);
    }

    if (strcmp(verb, "help") == 0 || strcmp(verb, "--help") == 0 ||
        strcmp(verb, "-h") == 0) {
        usage();
        return 0;
    }
    if (strcmp(verb, "version") == 0 || strcmp(verb, "--version") == 0) {
        printf("thorn " THORN_VERSION "\n");
        return 0;
    }

    thorn_diag("unknown command `%s`", verb);
    usage();
    return 2;
}
