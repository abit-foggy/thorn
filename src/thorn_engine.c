/*
 * thorn_engine.c - the C99 host engine for thorn, a lean meta-build
 * generator that embeds The Pith Programming Language.
 *
 * A thorn is the sharp spine on the stem of a plant; it grows on the
 * pith. This tool is the sharp point of the pith toolchain: minimal,
 * zero-bloat, deterministic, in the QBE spirit.
 *
 * Architecture (the Lua-in-a-game-engine model):
 *
 *   thorn embeds pith's frontend (libtcc/libqbe/libruntime joined at
 *   link time, see the bootstrap Makefile). At runtime thorn reads
 *   build.thorn, registers its build API as a host namespace
 *   (thorn_engine.*) through pith's embed ABI, and evaluates the
 *   spec. The script configures the graph by calling the API
 *   directly through the C ABI; thorn appends an emission epilogue
 *   that calls thorn_engine.emit(), which writes a deterministic
 *   build.ninja (samurai/ninja) and a portable Makefile.
 *
 *   Because evaluation may run in-memory (tcc JIT) or through the
 *   temp-executable fallback on hardened kernels, the graph and the
 *   emission both live in this engine: on the fallback the child
 *   process builds the graph in its own copy of this object (linked
 *   from the registered link object) and writes the backends itself.
 *   thorn_engine.c therefore contains no main(); the CLI lives in
 *   src/thorn_main.c, and this file compiles under the
 *   -D<stem>=c_thorn_engine_<stem> renames so the fallback link
 *   resolves every registered symbol.
 *
 * Pith ABI notes (verified against pith 0.1.0 sources):
 *   - string parameters are borrowed PithValue* (NUL-terminated,
 *     copy with strdup when storing)
 *   - int parameters/returns are 32-bit ('w'); pith narrows its
 *     64-bit integers at the boundary
 *   - every API function returns int so pith can call it as a bare
 *     statement or test it with `if`
 *   - zero-parameter functions double as pseudo-constants in pith:
 *     `thorn_engine.exe` (bare member access emits the call)
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <pith.h>

#include "include/thorn_engine.h"

/* ------------------------------------------------------------------ */
/* Diagnostics                                                        */
/* ------------------------------------------------------------------ */

void thorn_diag(const char *fmt, ...)
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
/* Path helpers                                                       */
/* ------------------------------------------------------------------ */

void thorn_join_path(char *out, size_t n, const char *dir,
                     const char *name)
{
    if (strcmp(dir, ".") == 0)
        snprintf(out, n, "%s", name);
    else
        snprintf(out, n, "%s/%s", dir, name);
}

void thorn_dir_prefix(const char *path, char *prefix, size_t n)
{
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) {
        prefix[0] = '\0';
        return;
    }
    size_t len = (size_t)(slash - path);
    if (len >= n)
        len = n - 1;
    memcpy(prefix, path, len);
    prefix[len] = '\0';
    if (len + 1 < n) {
        prefix[len] = '/';
        prefix[len + 1] = '\0';
    }
}

int thorn_makedirs(const char *dir)
{
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    size_t l = strlen(tmp);
    for (size_t i = 1; i <= l; i++) {
        if (tmp[i] == '/' || tmp[i] == '\0') {
            char c = tmp[i];
            tmp[i] = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                thorn_diag("cannot create directory %s", tmp);
                return -1;
            }
            tmp[i] = c;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* The host-facing API (thorn_engine.* in pith)                       */
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

/* int returns cross to pith as 32-bit words, widened via extsw. */

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

/* Zero-argument pseudo-constants: pith reads these through bare
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
 * (pith has no string-splitting primitives, so the split lives here.)
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
    snprintf(cmd, sizeof(cmd), "pkg-config --cflags --libs '%s'", pn);
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

/*
 * thorn_engine.emit() - the emission trigger thorn appends to every
 * evaluated spec (and which specs may also call explicitly). Reads
 * the backend selection from the environment the CLI exported, so
 * evaluation behaves identically in-process (in-memory tcc) and in
 * the temp-executable fallback child.
 */
int emit(void)
{
    ensure_graph();

    const char *dir = getenv("THORN_OUT_DIR");
    if (!dir || !*dir)
        dir = ".";
    int want_nin = 1, want_mk = 1;
    const char *v;
    if ((v = getenv("THORN_EMIT_NINJA")) && strcmp(v, "0") == 0)
        want_nin = 0;
    if ((v = getenv("THORN_EMIT_MAKE")) && strcmp(v, "0") == 0)
        want_mk = 0;
    if (!want_nin && !want_mk) {
        thorn_diag("no backend selected");
        return 0;
    }
    if (g_graph.count == 0) {
        thorn_diag("the specification declared no targets");
        return 0;
    }
    if (strcmp(dir, ".") != 0 && thorn_makedirs(dir) != 0)
        return 0;

    const char *cc = getenv("CC");
    if (!cc || !*cc)
        cc = "cc";
    const char *ar = getenv("AR");
    if (!ar || !*ar)
        ar = "ar";

    char np[4096], mp[4096];
    thorn_join_path(np, sizeof(np), dir, "build.ninja");
    thorn_join_path(mp, sizeof(mp), dir, "Makefile");

    int fail = 0;
    if (want_nin)
        fail |= thorn_emit_ninja(&g_graph, cc, ar, np);
    if (want_mk)
        fail |= thorn_emit_makefile(&g_graph, cc, ar, mp);
    if (fail)
        return 0;

    size_t total_sources = 0;
    for (size_t i = 0; i < g_graph.count; i++)
        total_sources += g_graph.targets[i].sources.count;

    printf("thorn: configured project `%s` (%zu target%s, %zu "
           "source%s)\n", g_graph.proj, g_graph.count,
           g_graph.count == 1 ? "" : "s", total_sources,
           total_sources == 1 ? "" : "s");
    if (want_nin)
        printf("thorn: wrote %s (build with: samu -f %s or "
               "ninja -f %s)\n", np, np, np);
    if (want_mk)
        printf("thorn: wrote %s (build with: make -f %s)\n", mp, mp);
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

/* An emitted OUTPUT name, prefixed with the backend file's directory
 * ("out/artifacts/build.ninja" puts its objects under out/artifacts).
 * Inputs stay project-root relative; the backends run from the root. */
static void out_name(char *out, size_t n, const char *pfx,
                      const char *name)
{
    snprintf(out, n, "%s%s", pfx, name);
}

/* The artifact name of a target: executables keep the declared name,
 * libraries gain their conventional extension. */
static void artifact_name(const Target *t, char *out, size_t n)
{
    if (t->type == THORN_STATIC_LIB)
        snprintf(out, n, "%s.a", t->name);
    else if (t->type == THORN_SHARED_LIB)
        snprintf(out, n, "%s.so", t->name);
    else
        snprintf(out, n, "%s", t->name);
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

    char pfx[2048];
    thorn_dir_prefix(path, pfx, sizeof(pfx));

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

    {
        char all[2048], tgt[2048], aname[512];
        out_name(all, sizeof(all), pfx, "thorn_all");
        fprintf(f, "build %s_%s: phony", all, g->proj);
        for (size_t i = 0; i < g->count; i++) {
            artifact_name(&g->targets[i], aname, sizeof(aname));
            out_name(tgt, sizeof(tgt), pfx, aname);
            fprintf(f, " %s", tgt);
        }
        fprintf(f, "\ndefault %s_%s\n\n", all, g->proj);
    }

    for (size_t i = 0; i < g->count; i++) {
        Target *t = &g->targets[i];
        SBuf ctext;
        flags_text(t, &ctext);
        char tname[2048], aname[512], obj[1024];
        artifact_name(t, aname, sizeof(aname));
        out_name(tname, sizeof(tname), pfx, aname);

        fprintf(f, "# target %s (%s)\n", t->name, type_name(t->type));

        for (size_t s = 0; s < t->sources.count; s++) {
            const char *src = t->sources.items[s];
            if (!is_c_source(src))
                continue;   /* raw link input, appears on the edge */
            obj_name(t, src, obj, sizeof(obj));
            fprintf(f, "build %s%s: cc %s", pfx, obj, src);
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
        fprintf(f, "build %s: %s", tname, rule);
        for (size_t s = 0; s < t->sources.count; s++) {
            const char *src = t->sources.items[s];
            if (is_c_source(src)) {
                obj_name(t, src, obj, sizeof(obj));
                fprintf(f, " %s%s", pfx, obj);
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
    char pfx[2048];
    thorn_dir_prefix(path, pfx, sizeof(pfx));

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
        char obj[1024];
        fprintf(f, "%s_OBJS =", t->name);
        for (size_t s = 0; s < t->sources.count; s++) {
            const char *src = t->sources.items[s];
            if (!is_c_source(src))
                continue;
            if (pattern) {
                snprintf(obj, sizeof(obj), "%s", src);
                obj[strlen(obj) - 1] = 'o';
            } else {
                obj_name(t, src, obj, sizeof(obj));
            }
            fprintf(f, " %s%s", pfx, obj);
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
    for (size_t i = 0; i < g->count; i++) {
        char aname[512];
        artifact_name(&g->targets[i], aname, sizeof(aname));
        fprintf(f, " %s%s", pfx, aname);
    }
    fprintf(f, "\n\n");

    /* link/archive rules */
    for (size_t i = 0; i < g->count; i++) {
        Target *t = &g->targets[i];
        char tname[2048], aname[512];
        artifact_name(t, aname, sizeof(aname));
        out_name(tname, sizeof(tname), pfx, aname);
        fprintf(f, "%s: $(%s_OBJS)", tname, t->name);
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
        fprintf(f, "%s%%.o: %%.c\n", pfx);
        if (t->cflags.count > 0 || t->includes.count > 0)
            fprintf(f, "\t$(CC) $(%s_CFLAGS) -MMD -MP -c $< -o $@\n\n",
                    t->name);
        else
            fprintf(f, "\t$(CC) -MMD -MP -c $< -o $@\n\n");
    } else {
        for (size_t i = 0; i < g->count; i++) {
            Target *t = &g->targets[i];
            char obj[1024];
            for (size_t s = 0; s < t->sources.count; s++) {
                const char *src = t->sources.items[s];
                if (!is_c_source(src))
                    continue;
                obj_name(t, src, obj, sizeof(obj));
                fprintf(f, "%s%s: %s", pfx, obj, src);
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
        char obj[1024];
        for (size_t s = 0; s < t->sources.count; s++) {
            const char *src = t->sources.items[s];
            if (!is_c_source(src))
                continue;
            if (pattern) {
                snprintf(obj, sizeof(obj), "%s", src);
                obj[strlen(obj) - 1] = 'o';
                fprintf(f, " %s%s.d %s%s", pfx, obj, pfx, obj);
            } else {
                obj_name(t, src, obj, sizeof(obj));
                char dep[1024];
                snprintf(dep, sizeof(dep), "%s", obj);
                dep[strlen(dep) - 1] = 'd';
                fprintf(f, " %s%s %s%s", pfx, obj, pfx, dep);
            }
        }
        char tname[2048], aname[512];
        artifact_name(t, aname, sizeof(aname));
        out_name(tname, sizeof(tname), pfx, aname);
        fprintf(f, " %s", tname);
    }
    fprintf(f, "\n\n");

    fprintf(f, ".PHONY: all clean\n\n");

    /* dependency fragments emitted by -MMD */
    fprintf(f, "-include");
    for (size_t i = 0; i < g->count; i++) {
        Target *t = &g->targets[i];
        char dep[1024];
        for (size_t s = 0; s < t->sources.count; s++) {
            const char *src = t->sources.items[s];
            if (!is_c_source(src))
                continue;
            if (pattern) {
                snprintf(dep, sizeof(dep), "%s", src);
                dep[strlen(dep) - 1] = 'd';
            } else {
                obj_name(t, src, dep, sizeof(dep));
                dep[strlen(dep) - 1] = 'd';
            }
            fprintf(f, " %s%s", pfx, dep);
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
    fprintf(out, "thorn_engine.project(\"%s\")\n\n", g->proj);

    for (size_t i = 0; i < g->count; i++) {
        Target *t = &g->targets[i];
        fprintf(out, "thorn_engine.add_target(\"%s\", "
                     "thorn_engine.%s)\n", t->name,
                type_const(t->type));
        for (size_t s = 0; s < t->sources.count; s++)
            fprintf(out, "thorn_engine.add_source(\"%s\", \"%s\")\n",
                    t->name, t->sources.items[s]);
        for (size_t s = 0; s < t->includes.count; s++)
            fprintf(out, "thorn_engine.add_include(\"%s\", \"%s\")\n",
                    t->name, t->includes.items[s]);
        for (size_t s = 0; s < t->cflags.count; s++)
            fprintf(out, "thorn_engine.add_cflag(\"%s\", \"%s\")\n",
                    t->name, t->cflags.items[s]);
        for (size_t s = 0; s < t->ldflags.count; s++)
            fprintf(out, "thorn_engine.add_ldflag(\"%s\", \"%s\")\n",
                    t->name, t->ldflags.items[s]);
        for (size_t s = 0; s < t->order_deps.count; s++)
            fprintf(out, "thorn_engine.add_order_dep(\"%s\", \"%s\")"
                         "\n", t->name, t->order_deps.items[s]);
        fprintf(out, "\n");
    }

    for (size_t i = 0; i < nnotes; i++)
        fprintf(out, "\n# note: %s\n", notes[i]);
    return 0;
}
