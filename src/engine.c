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
#include <compiler.h>

#include "include/engine.h"

/* Diagnostics */
__attribute__((weak))
void pith_emit_diagnostic(const char *severity, const char *message,
                          const char *filepath, const char *source,
                          size_t line, size_t col, size_t span);

static int g_errors = 0;

void diag(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    const char *sev = "error";
    const char *msg = buf;
    if (strncmp(buf, "warning: ", 9) == 0) {
        sev = "warning";
        msg = buf + 9;
    } else if (strncmp(buf, "error: ", 7) == 0) {
        sev = "error";
        msg = buf + 7;
    }

    if (strcmp(sev, "error") == 0)
        g_errors++;

    if (pith_emit_diagnostic)
        pith_emit_diagnostic(sev, msg, NULL, NULL, 0, 0, 0);
    else
        fprintf(stderr, "%s: %s\n", sev, msg);
    fflush(stderr);
}

/* Growable string buffer */

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

/* Model implementation */

int strlist_push(StrList *l, const char *s)
{
    for (size_t i = 0; i < l->count; i++)
        if (strcmp(l->items[i], s) == 0)
            return 0;
    return strlist_push_force(l, s);
}

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
    char *c = strdup(s);
    if (!c)
        return 0;
    l->items[l->count++] = c;
    return 1;
}

void strlist_free(StrList *l)
{
    for (size_t i = 0; i < l->count; i++)
        free(l->items[i]);
    free(l->items);
    l->items = NULL;
    l->count = l->cap = 0;
}

void graph_init(Graph *g)
{
    memset(g, 0, sizeof(*g));
    g_errors = 0;
}

void graph_free(Graph *g)
{
    for (size_t i = 0; i < g->count; i++) {
        Target *t = &g->targets[i];
        strlist_free(&t->sources);
        strlist_free(&t->cflags);
        strlist_free(&t->ldflags);
        strlist_free(&t->includes);
        strlist_free(&t->order_deps);
    }
    free(g->targets);
    g->targets = NULL;
    g->count = g->cap = 0;
    free(g->commands);
    g->commands = NULL;
    g->cmd_count = g->cmd_cap = 0;
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
    if (g->count == g->cap) {
        size_t nc = g->cap ? g->cap * 2 : 4;
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

int graph_add_command(Graph *g, const char *out, const char *cmd,
                      const char *in)
{
    if (g->cmd_count == g->cmd_cap) {
        size_t nc = g->cmd_cap ? g->cmd_cap * 2 : 4;
        Command *nc_arr = realloc(g->commands, nc * sizeof(Command));
        if (!nc_arr)
            return 0;
        g->commands = nc_arr;
        g->cmd_cap = nc;
    }
    Command *c = &g->commands[g->cmd_count++];
    memset(c, 0, sizeof(*c));
    snprintf(c->output, sizeof(c->output), "%s", out);
    snprintf(c->command, sizeof(c->command), "%s", cmd);
    if (in)
        snprintf(c->input, sizeof(c->input), "%s", in);
    return 1;
}

/* Path helpers */

void join_path(char *out, size_t n, const char *dir,
               const char *name)
{
    if (!dir || !*dir || strcmp(dir, ".") == 0)
        snprintf(out, n, "%s", name);
    else if (dir[strlen(dir) - 1] == '/')
        snprintf(out, n, "%s%s", dir, name);
    else
        snprintf(out, n, "%s/%s", dir, name);
}

void dir_prefix(const char *path, char *prefix, size_t n)
{
    const char *slash = strrchr(path, '/');
    if (!slash) {
        prefix[0] = '\0';
        return;
    }
    size_t len = (size_t)(slash - path + 1);
    if (len >= n)
        len = n - 1;
    memcpy(prefix, path, len);
    prefix[len] = '\0';
}

int makedirs(const char *dir)
{
    char tmp[4096];
    size_t l = strlen(dir);
    if (l >= sizeof(tmp)) {
        diag("directory path too long: %s", dir);
        return -1;
    }
    memcpy(tmp, dir, l + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                diag("cannot create directory %s: %s", tmp,
                     strerror(errno));
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        diag("cannot create directory %s: %s", tmp, strerror(errno));
        return -1;
    }
    return 0;
}

/* Whitespace validator */

static int has_whitespace(const char *s)
{
    if (!s)
        return 0;
    for (const char *p = s; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            return 1;
    }
    return 0;
}

/* Host API implementation */

static Graph g_graph;
static int g_ready;

static void ensure_graph(void)
{
    if (!g_ready) {
        graph_init(&g_graph);
        g_ready = 1;
    }
}

static Target *want_target(const char *api, PithValue *name)
{
    ensure_graph();
    if (!name) {
        diag("%s() expects a target name string", api);
        return NULL;
    }
    const char *n = pithStringData(name);
    Target *t = graph_find(&g_graph, n);
    if (!t)
        diag("%s(): unknown target `%s` (declare it with add_target first)",
             api, n);
    return t;
}

static const char *find_contradicting_cflag(const StrList *list, const char *flag)
{
    if (strncmp(flag, "-std=", 5) == 0) {
        for (size_t i = 0; i < list->count; i++) {
            if (strncmp(list->items[i], "-std=", 5) == 0 &&
                strcmp(list->items[i], flag) != 0)
                return list->items[i];
        }
    } else if (strncmp(flag, "-O", 2) == 0 && strlen(flag) <= 7) {
        for (size_t i = 0; i < list->count; i++) {
            if (strncmp(list->items[i], "-O", 2) == 0 &&
                strlen(list->items[i]) <= 7 &&
                strcmp(list->items[i], flag) != 0)
                return list->items[i];
        }
    }
    return NULL;
}

int project(PithValue *name)
{
    ensure_graph();
    if (!name) {
        diag("project() expects a project name string");
        return 0;
    }
    const char *n = pithStringData(name);
    if (has_whitespace(n)) {
        diag("project(): name `%s` contains whitespace", n);
        return 0;
    }
    if (g_graph.proj[0]) {
        if (strcmp(g_graph.proj, n) != 0) {
            diag("project(): contradicting project name `%s` (already set to `%s`)",
                 n, g_graph.proj);
            return 0;
        }
        diag("warning: duplicate project `%s` skipped", n);
        return 1;
    }
    snprintf(g_graph.proj, sizeof(g_graph.proj), "%s", n);
    return 1;
}

int backend(PithValue *name)
{
    ensure_graph();
    if (!name) {
        diag("backend() expects a backend name (\"ninja\", \"make\", or \"both\")");
        return 0;
    }
    const char *b = pithStringData(name);
    if (strcmp(b, "ninja") != 0 && strcmp(b, "make") != 0 && strcmp(b, "both") != 0) {
        diag("backend(): unknown backend `%s` (expected \"ninja\", \"make\", or \"both\")", b);
        return 0;
    }
    if (g_graph.backend[0]) {
        if (strcmp(g_graph.backend, b) != 0) {
            diag("backend(): contradicting backend `%s` (already set to `%s`)",
                 b, g_graph.backend);
            return 0;
        }
        diag("warning: duplicate backend `%s` skipped", b);
        return 1;
    }
    snprintf(g_graph.backend, sizeof(g_graph.backend), "%s", b);
    return 1;
}

int exe(void)         { return THORN_EXE; }
int static_lib(void)  { return THORN_STATIC_LIB; }
int shared_lib(void)  { return THORN_SHARED_LIB; }

int add_target(PithValue *name, int type)
{
    ensure_graph();
    if (!name) {
        diag("add_target() expects a target name string");
        return 0;
    }
    if (type < THORN_EXE || type > THORN_SHARED_LIB) {
        diag("add_target(): the type must be engine.exe, "
             "engine.static_lib or engine.shared_lib");
        return 0;
    }
    const char *n = pithStringData(name);
    if (!n[0]) {
        diag("add_target(): the name must not be empty");
        return 0;
    }
    if (has_whitespace(n)) {
        diag("add_target(): target name `%s` contains whitespace", n);
        return 0;
    }
    Target *existing = graph_find(&g_graph, n);
    if (existing) {
        if (existing->type != type) {
            diag("add_target(): contradicting type for target `%s`", n);
            return 0;
        }
        diag("add_target(): duplicate target `%s`", n);
        return 0;
    }
    for (size_t i = 0; i < g_graph.cmd_count; i++) {
        if (strcmp(g_graph.commands[i].output, n) == 0) {
            diag("add_target(): target `%s` conflicts with custom command output", n);
            return 0;
        }
    }
    if (!graph_add(&g_graph, n, type)) {
        diag("add_target(): out of memory for `%s`", n);
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
        diag("add_source() expects a source path string");
        return 0;
    }
    const char *s = pithStringData(src);
    if (has_whitespace(s)) {
        diag("add_source(): source path `%s` contains whitespace", s);
        return 0;
    }
    if (!strlist_push(&t->sources, s)) {
        diag("warning: duplicate source `%s` on target `%s` skipped",
             s, t->name);
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
        diag("add_cflag() expects a flag string");
        return 0;
    }
    const char *f = pithStringData(flag);
    if (has_whitespace(f)) {
        diag("add_cflag(): flag `%s` contains whitespace", f);
        return 0;
    }
    const char *conflict = find_contradicting_cflag(&t->cflags, f);
    if (conflict) {
        diag("add_cflag(): contradicting flag `%s` on target `%s` (already has `%s`)",
             f, t->name, conflict);
        return 0;
    }
    if (!strlist_push(&t->cflags, f)) {
        diag("warning: duplicate cflag `%s` on target `%s` skipped",
             f, t->name);
        return 1;
    }
    return 1;
}

int add_ldflag(PithValue *target, PithValue *flag)
{
    Target *t = want_target("add_ldflag", target);
    if (!t)
        return 0;
    if (!flag) {
        diag("add_ldflag() expects a flag string");
        return 0;
    }
    const char *f = pithStringData(flag);
    if (has_whitespace(f)) {
        diag("add_ldflag(): flag `%s` contains whitespace", f);
        return 0;
    }
    if (!strlist_push(&t->ldflags, f)) {
        diag("warning: duplicate ldflag `%s` on target `%s` skipped",
             f, t->name);
        return 1;
    }
    return 1;
}

int add_include(PithValue *target, PithValue *dir)
{
    Target *t = want_target("add_include", target);
    if (!t)
        return 0;
    if (!dir) {
        diag("add_include() expects a directory string");
        return 0;
    }
    const char *d = pithStringData(dir);
    if (has_whitespace(d)) {
        diag("add_include(): directory `%s` contains whitespace", d);
        return 0;
    }
    if (!strlist_push(&t->includes, d)) {
        diag("warning: duplicate include `%s` on target `%s` skipped",
             d, t->name);
        return 1;
    }
    return 1;
}

int add_order_dep(PithValue *target, PithValue *prereq)
{
    Target *t = want_target("add_order_dep", target);
    if (!t)
        return 0;
    if (!prereq) {
        diag("add_order_dep() expects a prerequisite string");
        return 0;
    }
    const char *p = pithStringData(prereq);
    if (has_whitespace(p)) {
        diag("add_order_dep(): prerequisite `%s` contains whitespace", p);
        return 0;
    }
    if (!strlist_push(&t->order_deps, p)) {
        diag("warning: duplicate order dependency `%s` on target `%s` skipped",
             p, t->name);
        return 1;
    }
    return 1;
}

int add_command(PithValue *output, PithValue *command, PithValue *input)
{
    ensure_graph();
    if (!output || !command) {
        diag("add_command() expects output and command strings");
        return 0;
    }
    const char *out = pithStringData(output);
    const char *cmd = pithStringData(command);
    const char *in = input ? pithStringData(input) : "";
    if (!out[0] || !cmd[0]) {
        diag("add_command(): output and command must not be empty");
        return 0;
    }
    if (has_whitespace(out)) {
        diag("add_command(): output `%s` contains whitespace", out);
        return 0;
    }
    if (in[0] && has_whitespace(in)) {
        diag("add_command(): input `%s` contains whitespace", in);
        return 0;
    }
    if (graph_find(&g_graph, out)) {
        diag("add_command(): output `%s` conflicts with existing target", out);
        return 0;
    }
    for (size_t i = 0; i < g_graph.cmd_count; i++) {
        if (strcmp(g_graph.commands[i].output, out) == 0) {
            if (strcmp(g_graph.commands[i].command, cmd) == 0 &&
                strcmp(g_graph.commands[i].input, in) == 0) {
                diag("warning: duplicate command for output `%s` skipped", out);
                return 1;
            } else {
                diag("add_command(): contradicting command for output `%s`", out);
                return 0;
            }
        }
    }
    if (!graph_add_command(&g_graph, out, cmd, in)) {
        diag("add_command(): out of memory");
        return 0;
    }
    return 1;
}

int pkg_config(PithValue *target, PithValue *pkg)
{
    Target *t = want_target("pkg_config", target);
    if (!t)
        return 0;
    if (!pkg) {
        diag("pkg_config() expects a package name string");
        return 0;
    }
    const char *pn = pithStringData(pkg);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "pkg-config --cflags --libs '%s'", pn);
    FILE *p = popen(cmd, "r");
    if (!p) {
        diag("pkg_config(): cannot run pkg-config");
        return 0;
    }
    char line[8192];
    if (!fgets(line, sizeof(line), p))
        line[0] = '\0';
    int st = pclose(p);
    if (st != 0) {
        diag("pkg_config(): `pkg-config %s` failed (is the package installed?)",
             pn);
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

int emit(void)
{
    ensure_graph();
    if (g_errors > 0)
        return 0;

    if (!g_graph.proj[0])
        snprintf(g_graph.proj, sizeof(g_graph.proj), "project");

    const char *dir = getenv("THORN_OUT_DIR");
    if (!dir || !*dir)
        dir = ".";

    /* Determine backend: CLI env THORN_ENGINE overrides spec */
    const char *sel = getenv("THORN_ENGINE");
    if (!sel || !*sel) {
        if (g_graph.backend[0])
            sel = g_graph.backend;
    }

    if (!sel || !*sel) {
        diag("no engine selected (specify engine.backend(\"ninja\") in spec or pass --engine <ninja|make|both>)");
        return 0;
    }

    int want_nin = 0, want_mk = 0;
    if (strcmp(sel, "ninja") == 0) {
        want_nin = 1;
    } else if (strcmp(sel, "make") == 0) {
        want_mk = 1;
    } else if (strcmp(sel, "both") == 0) {
        want_nin = 1;
        want_mk = 1;
    } else {
        diag("unknown backend `%s` (expected ninja, make, or both)", sel);
        return 0;
    }

    if (g_graph.count == 0 && g_graph.cmd_count == 0) {
        diag("the specification declared no targets");
        return 0;
    }
    if (strcmp(dir, ".") != 0 && makedirs(dir) != 0)
        return 0;

    const char *cc = getenv("CC");
    if (!cc || !*cc)
        cc = "cc";
    const char *ar = getenv("AR");
    if (!ar || !*ar)
        ar = "ar";

    char np[4096], mp[4096];
    join_path(np, sizeof(np), dir, "build.ninja");
    join_path(mp, sizeof(mp), dir, "Makefile");

    int fail = 0;
    if (want_nin)
        fail |= emit_ninja(&g_graph, cc, ar, np);
    if (want_mk)
        fail |= emit_makefile(&g_graph, cc, ar, mp);
    if (fail)
        return 0;

    size_t total_sources = 0;
    for (size_t i = 0; i < g_graph.count; i++)
        total_sources += g_graph.targets[i].sources.count;

    if (g_graph.cmd_count > 0) {
        printf("thorn: configured project `%s` (%zu target%s, %zu source%s, %zu command%s)\n",
               g_graph.proj, g_graph.count,
               g_graph.count == 1 ? "" : "s", total_sources,
               total_sources == 1 ? "" : "s",
               g_graph.cmd_count,
               g_graph.cmd_count == 1 ? "" : "s");
    } else {
        printf("thorn: configured project `%s` (%zu target%s, %zu source%s)\n",
               g_graph.proj, g_graph.count,
               g_graph.count == 1 ? "" : "s", total_sources,
               total_sources == 1 ? "" : "s");
    }
    if (want_nin)
        printf("thorn: wrote %s (build with: samu -f %s or ninja -f %s)\n",
               np, np, np);
    if (want_mk)
        printf("thorn: wrote %s (build with: make -f %s)\n", mp, mp);
    return 1;
}

/* Naming helpers */

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

static void out_name(char *out, size_t n, const char *pfx,
                     const char *name)
{
    snprintf(out, n, "%s%s", pfx, name);
}

static void artifact_name(const Target *t, char *out, size_t n)
{
    if (t->type == THORN_STATIC_LIB)
        snprintf(out, n, "%s.a", t->name);
    else if (t->type == THORN_SHARED_LIB)
        snprintf(out, n, "%s.so", t->name);
    else
        snprintf(out, n, "%s", t->name);
}

/* Ninja emitter */

int emit_ninja(const Graph *g, const char *cc, const char *ar,
               const char *path)
{
    if (g->count == 0 && g->cmd_count == 0) {
        diag("no targets declared; nothing to emit");
        return 1;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        diag("cannot write %s", path);
        return 1;
    }

    char pfx[2048];
    dir_prefix(path, pfx, sizeof(pfx));

    fprintf(f, "# build.ninja - generated by thorn " THORN_VERSION
               ", do not edit\n");
    fprintf(f, "# project: %s (deterministic: same graph and "
               "environment, same bytes)\n\n", g->proj);
    fprintf(f, "ninja_required_version = 1.3\n\n");
    fprintf(f, "thorn_project = %s\n", g->proj);
    fprintf(f, "cc = %s\n", cc);
    fprintf(f, "ar = %s\n\n", ar);

    /* toolchain rules */
    fprintf(f, "rule cc\n");
    fprintf(f, "  command = $cc $cflags -MMD -MF $out.d -c $in -o $out\n");
    fprintf(f, "  depfile = $out.d\n");
    fprintf(f, "  deps = gcc\n");
    fprintf(f, "  description = cc $in\n\n");

    fprintf(f, "rule link\n");
    fprintf(f, "  command = $cc $in $ldflags -o $out\n");
    fprintf(f, "  description = link $out\n\n");

    fprintf(f, "rule solink\n");
    fprintf(f, "  command = $cc -shared $in $ldflags -o $out\n");
    fprintf(f, "  description = shared $out\n\n");

    fprintf(f, "rule ar\n");
    fprintf(f, "  command = rm -f $out && $ar rcs $out $in\n");
    fprintf(f, "  description = ar $out\n\n");

    /* custom command rules and edges */
    for (size_t i = 0; i < g->cmd_count; i++) {
        const Command *c = &g->commands[i];
        char cout[2048];
        out_name(cout, sizeof(cout), pfx, c->output);
        fprintf(f, "# custom command %zu (%s)\n", i + 1, c->output);
        fprintf(f, "rule cmd_%zu\n", i + 1);
        fprintf(f, "  command = %s\n", c->command);
        fprintf(f, "  description = %s\n\n", c->command);
        if (c->input[0])
            fprintf(f, "build %s: cmd_%zu %s\n\n", cout, i + 1, c->input);
        else
            fprintf(f, "build %s: cmd_%zu\n\n", cout, i + 1);
    }

    /* default target aggregation */
    if (g->count > 0) {
        char all[2048];
        out_name(all, sizeof(all), pfx, "thorn_all");
        fprintf(f, "build %s: phony", all);
        for (size_t i = 0; i < g->count; i++) {
            char tgt[2048], aname[512];
            artifact_name(&g->targets[i], aname, sizeof(aname));
            out_name(tgt, sizeof(tgt), pfx, aname);
            fprintf(f, " %s", tgt);
        }
        fprintf(f, "\n");
        fprintf(f, "default %s\n\n", all);
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
                continue;
            obj_name(t, src, obj, sizeof(obj));
            fprintf(f, "build %s%s: cc %s", pfx, obj, src);
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

/* Makefile emitter */

static void subst_cmd_for_make(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < n; ) {
        if (in[i] == '$') {
            if (in[i + 1] == '$') {
                if (o + 2 < n) {
                    out[o++] = '$';
                    out[o++] = '$';
                }
                i += 2;
                continue;
            }
            if (strncmp(in + i, "$out", 4) == 0) {
                if (o + 2 < n) {
                    out[o++] = '$';
                    out[o++] = '@';
                }
                i += 4;
                continue;
            }
            if (strncmp(in + i, "$in", 3) == 0) {
                if (o + 2 < n) {
                    out[o++] = '$';
                    out[o++] = '<';
                }
                i += 3;
                continue;
            }
        }
        out[o++] = in[i++];
    }
    out[o] = '\0';
}

static int make_uses_pattern(const Graph *g)
{
    if (g->count != 1 || g->cmd_count > 0)
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

int emit_makefile(const Graph *g, const char *cc, const char *ar,
                  const char *path)
{
    if (g->count == 0 && g->cmd_count == 0) {
        diag("no targets declared; nothing to emit");
        return 1;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        diag("cannot write %s", path);
        return 1;
    }
    int pattern = make_uses_pattern(g);
    char pfx[2048];
    dir_prefix(path, pfx, sizeof(pfx));

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
        fprintf(f, "%s_CFLAGS = %s\n", t->name, ctext.buf);
        sb_free(&ctext);

        SBuf lt;
        ldflags_text(t, &lt);
        fprintf(f, "%s_LDFLAGS = %s\n\n", t->name, lt.buf);
        sb_free(&lt);
    }

    /* all (default rule) */
    if (g->count > 0) {
        fprintf(f, "all:");
        for (size_t i = 0; i < g->count; i++) {
            char tname[2048], aname[512];
            artifact_name(&g->targets[i], aname, sizeof(aname));
            out_name(tname, sizeof(tname), pfx, aname);
            fprintf(f, " %s", tname);
        }
        fprintf(f, "\n\n");
    }

    /* custom command rules */
    for (size_t i = 0; i < g->cmd_count; i++) {
        const Command *c = &g->commands[i];
        char cout[2048];
        out_name(cout, sizeof(cout), pfx, c->output);
        char make_cmd[2048];
        subst_cmd_for_make(c->command, make_cmd, sizeof(make_cmd));
        if (c->input[0])
            fprintf(f, "%s: %s\n", cout, c->input);
        else
            fprintf(f, "%s:\n", cout);
        fprintf(f, "\t%s\n\n", make_cmd);
    }

    /* link rules */
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
                fprintf(f, "\t$(CC) -shared $^ $(%s_LDFLAGS) -o $@\n\n",
                        t->name);
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
                    fprintf(f, "\t$(CC) $(%s_CFLAGS) -MMD -MP -c %s -o $@\n\n",
                            t->name, src);
                else
                    fprintf(f, "\t$(CC) -MMD -MP -c %s -o $@\n\n", src);
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
    for (size_t i = 0; i < g->cmd_count; i++) {
        char cout[2048];
        out_name(cout, sizeof(cout), pfx, g->commands[i].output);
        fprintf(f, " %s", cout);
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

/* build.thorn printer */

int print_spec(const Graph *g, FILE *out, const char **notes,
               size_t nnotes)
{
    fprintf(out, "# build.thorn - generated by thorn " THORN_VERSION
                 "\n");
    fprintf(out, "# decompiled from an existing build graph; review "
                 "before use\n\n");
    fprintf(out, "engine.project(\"%s\")\n", g->proj);
    if (g->backend[0])
        fprintf(out, "engine.backend(\"%s\")\n", g->backend);
    fprintf(out, "\n");

    for (size_t i = 0; i < g->count; i++) {
        Target *t = &g->targets[i];
        fprintf(out, "engine.add_target(\"%s\", engine.%s)\n",
                t->name, type_const(t->type));
        for (size_t s = 0; s < t->sources.count; s++)
            fprintf(out, "engine.add_source(\"%s\", \"%s\")\n",
                    t->name, t->sources.items[s]);
        for (size_t s = 0; s < t->includes.count; s++)
            fprintf(out, "engine.add_include(\"%s\", \"%s\")\n",
                    t->name, t->includes.items[s]);
        for (size_t s = 0; s < t->cflags.count; s++)
            fprintf(out, "engine.add_cflag(\"%s\", \"%s\")\n",
                    t->name, t->cflags.items[s]);
        for (size_t s = 0; s < t->ldflags.count; s++)
            fprintf(out, "engine.add_ldflag(\"%s\", \"%s\")\n",
                    t->name, t->ldflags.items[s]);
        for (size_t s = 0; s < t->order_deps.count; s++)
            fprintf(out, "engine.add_order_dep(\"%s\", \"%s\")\n",
                    t->name, t->order_deps.items[s]);
        fprintf(out, "\n");
    }

    for (size_t i = 0; i < g->cmd_count; i++) {
        Command *c = &g->commands[i];
        fprintf(out, "engine.add_command(\"%s\", \"%s\", \"%s\")\n",
                c->output, c->command, c->input);
    }
    if (g->cmd_count > 0)
        fprintf(out, "\n");

    for (size_t i = 0; i < nnotes; i++)
        fprintf(out, "\n# note: %s\n", notes[i]);
    return 0;
}
