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

#include "include/engine.h"
#include "include/graph.h"
#include "include/ninja.h"
#include "include/make.h"
#include "include/spec.h"

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
    for (size_t i = 0; i < t->sources.count; i++) {
        if (strcmp(t->sources.items[i], s) == 0) {
            diag("warning: duplicate source `%s` on target `%s` skipped",
                 s, t->name);
            return 1;
        }
    }
    if (!strlist_push_force(&t->sources, s))
        return 0;
    SourceKind *nk = realloc(t->source_kinds, t->sources.cap * sizeof(SourceKind));
    if (!nk)
        return 0;
    t->source_kinds = nk;
    t->source_kinds[t->sources.count - 1] = classify_source(s);
    return 1;
}

int thorn_add_asflag(Target *t, const char *f)
{
    if (!t || !f)
        return 0;
    if (has_whitespace(f)) {
        diag("add_asflag(): flag `%s` contains whitespace", f);
        return 0;
    }
    if (!strlist_push(&t->asflags, f)) {
        diag("warning: duplicate asflag `%s` on target `%s` skipped",
             f, t->name);
        return 1;
    }
    return 1;
}

int add_asflag(PithValue *target, PithValue *flag)
{
    Target *t = want_target("add_asflag", target);
    if (!t)
        return 0;
    if (!flag) {
        diag("add_asflag() expects a flag string");
        return 0;
    }
    const char *f = pithStringData(flag);
    return thorn_add_asflag(t, f);
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

/* Include modular implementation units */
#include "graph.c"
#include "ninja.c"
#include "make.c"
#include "spec.c"
