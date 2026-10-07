#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pith.h>
#include "include/engine.h"
#include "include/parser.h"
#include "include/emit.h"

#ifndef THORN_CORE_BUILD
#include "engine.c"
#endif

DecompHooks g_hooks;
static Graph g_hook_graph;
static int g_hook_ready = 0;
static char **g_hook_notes = NULL;
static size_t g_hook_nnotes = 0;

int decompile_file_scoped(const char *path, const char *dir_scope,
                          Graph *g, char ***notes, size_t *nnotes)
{
    *notes = NULL;
    *nnotes = 0;

    char *mem = read_all(path);
    if (!mem) {
        diag("cannot read %s", path);
        return 1;
    }

    size_t prev_target_count = g->count;
    size_t prev_cmd_count = g->cmd_count;

    int rc = 1;
    if (sniff_is_ninja(mem)) {
        char pfx[2048];
        if (dir_scope && *dir_scope)
            snprintf(pfx, sizeof(pfx), "%s", dir_scope);
        else if (g_hooks.dir_pfx[0])
            snprintf(pfx, sizeof(pfx), "%s", g_hooks.dir_pfx);
        else
            dir_prefix(path, pfx, sizeof(pfx));
        snprintf(g->backend, sizeof(g->backend), "ninja");
        NDoc doc;
        memset(&doc, 0, sizeof(doc));
        parse_ninja(mem, &doc, notes, nnotes);
        map_ninja(&doc, g, pfx, notes, nnotes);
        ninja_doc_free(&doc);
        rc = (g->count == prev_target_count && g->cmd_count == prev_cmd_count);
    } else {
        char pfx[2048];
        if (dir_scope && *dir_scope)
            snprintf(pfx, sizeof(pfx), "%s", dir_scope);
        else if (g_hooks.dir_pfx[0])
            snprintf(pfx, sizeof(pfx), "%s", g_hooks.dir_pfx);
        else
            dir_prefix(path, pfx, sizeof(pfx));
        snprintf(g->backend, sizeof(g->backend), "make");
        MDoc doc;
        memset(&doc, 0, sizeof(doc));
        parse_make(mem, &doc, notes, nnotes);
        map_make(&doc, g, pfx, notes, nnotes);
        mdoc_free(&doc);
        rc = (g->count == prev_target_count && g->cmd_count == prev_cmd_count);
    }

    /* Apply dir_scope path prefixing to relative sources and local includes */
    if (dir_scope && *dir_scope) {
        for (size_t i = prev_target_count; i < g->count; i++) {
            Target *t = &g->targets[i];
            for (size_t s = 0; s < t->sources.count; s++) {
                char *scoped = prefix_scoped_path(t->sources.items[s], dir_scope);
                free(t->sources.items[s]);
                t->sources.items[s] = scoped;
            }
            for (size_t inc = 0; inc < t->includes.count; inc++) {
                char *scoped = prefix_scoped_path(t->includes.items[inc], dir_scope);
                free(t->includes.items[inc]);
                t->includes.items[inc] = scoped;
            }
        }
    }

    free(mem);
    if (rc)
        diag("no buildable targets or commands found in %s", path);
    return rc;
}

int decompile_file(const char *path, Graph *g, char ***notes,
                   size_t *nnotes)
{
    return decompile_file_scoped(path, NULL, g, notes, nnotes);
}

/* Hookable decompiler API implementation */

static const char *pv_to_cstr(PithValue *val)
{
    if (!val)
        return "";
    return pithStringData(val);
}

static PithValue *strlist_to_pith(const StrList *list)
{
    if (!list || list->count == 0)
        return pithNewString("");
    size_t total = 0;
    for (size_t i = 0; i < list->count; i++)
        total += strlen(list->items[i]) + 1;
    char *buf = malloc(total + 1);
    if (!buf)
        return pithNewString("");
    buf[0] = '\0';
    for (size_t i = 0; i < list->count; i++) {
        if (i)
            strcat(buf, " ");
        strcat(buf, list->items[i]);
    }
    PithValue *res = pithNewString(buf);
    free(buf);
    return res;
}

void reset(void)
{
    strlist_free(&g_hooks.ignore_targets);
    strlist_free(&g_hooks.keep_targets);
    strlist_free(&g_hooks.strip_cflags);
    strlist_free(&g_hooks.remap_from);
    strlist_free(&g_hooks.remap_to);
    memset(&g_hooks, 0, sizeof(g_hooks));

    if (g_hook_ready) {
        graph_free(&g_hook_graph);
        memset(&g_hook_graph, 0, sizeof(g_hook_graph));
        g_hook_ready = 0;
    }

    for (size_t i = 0; i < g_hook_nnotes; i++)
        free(g_hook_notes[i]);
    free(g_hook_notes);
    g_hook_notes = NULL;
    g_hook_nnotes = 0;
}

void set_project(PithValue *name)
{
    const char *s = pv_to_cstr(name);
    snprintf(g_hooks.proj, sizeof(g_hooks.proj), "%s", s);
    if (!g_hook_ready) {
        graph_init(&g_hook_graph);
        g_hook_ready = 1;
    }
    snprintf(g_hook_graph.proj, sizeof(g_hook_graph.proj), "%s", g_hooks.proj);
}

void set_compiler(PithValue *cc)
{
    const char *s = pv_to_cstr(cc);
    snprintf(g_hooks.cc, sizeof(g_hooks.cc), "%s", s);
}

void set_ar(PithValue *ar)
{
    const char *s = pv_to_cstr(ar);
    snprintf(g_hooks.ar, sizeof(g_hooks.ar), "%s", s);
}

void set_dir_prefix(PithValue *pfx)
{
    const char *s = pv_to_cstr(pfx);
    snprintf(g_hooks.dir_pfx, sizeof(g_hooks.dir_pfx), "%s", s);
}

void ignore_target(PithValue *pattern)
{
    const char *s = pv_to_cstr(pattern);
    if (*s)
        strlist_push(&g_hooks.ignore_targets, s);
}

void keep_target(PithValue *pattern)
{
    const char *s = pv_to_cstr(pattern);
    if (*s)
        strlist_push(&g_hooks.keep_targets, s);
}

void strip_cflag(PithValue *pattern)
{
    const char *s = pv_to_cstr(pattern);
    if (*s)
        strlist_push(&g_hooks.strip_cflags, s);
}

void inject_cflag(PithValue *target_pattern, PithValue *flag)
{
    const char *pat = pv_to_cstr(target_pattern);
    const char *f = pv_to_cstr(flag);
    if (!*f)
        return;
    if (g_hooks.ninject_cflags < 128) {
        snprintf(g_hooks.inject_cflags[g_hooks.ninject_cflags].pat,
                 sizeof(g_hooks.inject_cflags[0].pat), "%s", pat && *pat ? pat : "*");
        snprintf(g_hooks.inject_cflags[g_hooks.ninject_cflags].val,
                 sizeof(g_hooks.inject_cflags[0].val), "%s", f);
        g_hooks.ninject_cflags++;
    }
}

void inject_include(PithValue *target_pattern, PithValue *inc)
{
    const char *pat = pv_to_cstr(target_pattern);
    const char *d = pv_to_cstr(inc);
    if (!*d)
        return;
    if (g_hooks.ninject_includes < 128) {
        snprintf(g_hooks.inject_includes[g_hooks.ninject_includes].pat,
                 sizeof(g_hooks.inject_includes[0].pat), "%s", pat && *pat ? pat : "*");
        snprintf(g_hooks.inject_includes[g_hooks.ninject_includes].val,
                 sizeof(g_hooks.inject_includes[0].val), "%s", d);
        g_hooks.ninject_includes++;
    }
}

void remap_target(PithValue *old_name, PithValue *new_name)
{
    const char *from = pv_to_cstr(old_name);
    const char *to = pv_to_cstr(new_name);
    if (*from && *to) {
        strlist_push(&g_hooks.remap_from, from);
        strlist_push(&g_hooks.remap_to, to);
    }
}

void add_command_edge(PithValue *out, PithValue *cmd, PithValue *in)
{
    if (!g_hook_ready) {
        graph_init(&g_hook_graph);
        g_hook_ready = 1;
    }
    const char *o = pv_to_cstr(out);
    const char *c = pv_to_cstr(cmd);
    const char *i = pv_to_cstr(in);
    if (*o && *c)
        graph_add_command(&g_hook_graph, o, c, i);
}

int parse_file(PithValue *path)
{
    const char *p = pv_to_cstr(path);
    if (!*p)
        return 0;
    if (!g_hook_ready) {
        graph_init(&g_hook_graph);
        g_hook_ready = 1;
    }
    int rc = decompile_file(p, &g_hook_graph, &g_hook_notes, &g_hook_nnotes);
    if (rc == 0) {
        if (g_hooks.proj[0])
            snprintf(g_hook_graph.proj, sizeof(g_hook_graph.proj), "%s", g_hooks.proj);
        return 1;
    }
    return 0;
}

int parse_scoped(PithValue *path, PithValue *dir_scope)
{
    const char *p = pv_to_cstr(path);
    const char *s = pv_to_cstr(dir_scope);
    if (!*p)
        return 0;
    if (!g_hook_ready) {
        graph_init(&g_hook_graph);
        g_hook_ready = 1;
    }
    int rc = decompile_file_scoped(p, s, &g_hook_graph, &g_hook_notes, &g_hook_nnotes);
    if (rc == 0) {
        if (g_hooks.proj[0])
            snprintf(g_hook_graph.proj, sizeof(g_hook_graph.proj), "%s", g_hooks.proj);
        return 1;
    }
    return 0;
}

int parse_string(PithValue *content)
{
    const char *s = pv_to_cstr(content);
    if (!*s)
        return 0;
    if (!g_hook_ready) {
        graph_init(&g_hook_graph);
        g_hook_ready = 1;
    }
    char *mem = strdup(s);
    if (!mem)
        return 0;
    int rc = 1;
    const char *pfx = g_hooks.dir_pfx;
    if (sniff_is_ninja(mem)) {
        snprintf(g_hook_graph.backend, sizeof(g_hook_graph.backend), "ninja");
        NDoc doc;
        memset(&doc, 0, sizeof(doc));
        parse_ninja(mem, &doc, &g_hook_notes, &g_hook_nnotes);
        map_ninja(&doc, &g_hook_graph, pfx, &g_hook_notes, &g_hook_nnotes);
        ninja_doc_free(&doc);
        rc = (g_hook_graph.count == 0 && g_hook_graph.cmd_count == 0);
    } else {
        snprintf(g_hook_graph.backend, sizeof(g_hook_graph.backend), "make");
        MDoc doc;
        memset(&doc, 0, sizeof(doc));
        parse_make(mem, &doc, &g_hook_notes, &g_hook_nnotes);
        map_make(&doc, &g_hook_graph, pfx, &g_hook_notes, &g_hook_nnotes);
        mdoc_free(&doc);
        rc = (g_hook_graph.count == 0 && g_hook_graph.cmd_count == 0);
    }
    free(mem);
    if (g_hooks.proj[0])
        snprintf(g_hook_graph.proj, sizeof(g_hook_graph.proj), "%s", g_hooks.proj);
    return rc == 0 ? 1 : 0;
}

long target_count(void)
{
    return (long)g_hook_graph.count;
}

long command_count(void)
{
    return (long)g_hook_graph.cmd_count;
}

PithValue *get_target_name(long index)
{
    if (index < 0 || (size_t)index >= g_hook_graph.count)
        return pithNewString("");
    return pithNewString(g_hook_graph.targets[index].name);
}

long get_target_type(long index)
{
    if (index < 0 || (size_t)index >= g_hook_graph.count)
        return -1;
    return (long)g_hook_graph.targets[index].type;
}

PithValue *get_target_sources(long index)
{
    if (index < 0 || (size_t)index >= g_hook_graph.count)
        return pithNewString("");
    return strlist_to_pith(&g_hook_graph.targets[index].sources);
}

PithValue *get_target_cflags(long index)
{
    if (index < 0 || (size_t)index >= g_hook_graph.count)
        return pithNewString("");
    return strlist_to_pith(&g_hook_graph.targets[index].cflags);
}

PithValue *get_target_ldflags(long index)
{
    if (index < 0 || (size_t)index >= g_hook_graph.count)
        return pithNewString("");
    return strlist_to_pith(&g_hook_graph.targets[index].ldflags);
}

PithValue *get_target_includes(long index)
{
    if (index < 0 || (size_t)index >= g_hook_graph.count)
        return pithNewString("");
    return strlist_to_pith(&g_hook_graph.targets[index].includes);
}

PithValue *get_target_order_deps(long index)
{
    if (index < 0 || (size_t)index >= g_hook_graph.count)
        return pithNewString("");
    return strlist_to_pith(&g_hook_graph.targets[index].order_deps);
}

PithValue *get_command_output(long index)
{
    if (index < 0 || (size_t)index >= g_hook_graph.cmd_count)
        return pithNewString("");
    return pithNewString(g_hook_graph.commands[index].output);
}

PithValue *get_command_line(long index)
{
    if (index < 0 || (size_t)index >= g_hook_graph.cmd_count)
        return pithNewString("");
    return pithNewString(g_hook_graph.commands[index].command);
}

PithValue *get_command_input(long index)
{
    if (index < 0 || (size_t)index >= g_hook_graph.cmd_count)
        return pithNewString("");
    return pithNewString(g_hook_graph.commands[index].input);
}

void set_target_type(PithValue *target_name, long type)
{
    const char *n = pv_to_cstr(target_name);
    if (!*n)
        return;
    if (matches_any(&g_hooks.ignore_targets, n))
        return;
    if (!g_hook_ready) {
        graph_init(&g_hook_graph);
        g_hook_ready = 1;
    }
    Target *t = graph_find(&g_hook_graph, n);
    if (!t)
        t = graph_add(&g_hook_graph, n, (int)type);
    else
        t->type = (int)type;
    apply_target_hooks(t);
}

void add_target_source(PithValue *target_name, PithValue *src)
{
    const char *n = pv_to_cstr(target_name);
    const char *s = pv_to_cstr(src);
    if (!*n || !*s)
        return;
    if (matches_any(&g_hooks.ignore_targets, n) || matches_any(&g_hooks.ignore_targets, s))
        return;
    if (!g_hook_ready) {
        graph_init(&g_hook_graph);
        g_hook_ready = 1;
    }
    Target *t = graph_find(&g_hook_graph, n);
    if (!t)
        t = graph_add(&g_hook_graph, n, THORN_STATIC_LIB);
    if (t) {
        strlist_push(&t->sources, s);
        apply_target_hooks(t);
    }
}

void remove_target(PithValue *target_name)
{
    const char *n = pv_to_cstr(target_name);
    for (size_t i = 0; i < g_hook_graph.count; i++) {
        if (strcmp(g_hook_graph.targets[i].name, n) == 0) {
            strlist_free(&g_hook_graph.targets[i].sources);
            strlist_free(&g_hook_graph.targets[i].cflags);
            strlist_free(&g_hook_graph.targets[i].ldflags);
            strlist_free(&g_hook_graph.targets[i].includes);
            strlist_free(&g_hook_graph.targets[i].order_deps);
            for (size_t j = i + 1; j < g_hook_graph.count; j++)
                g_hook_graph.targets[j - 1] = g_hook_graph.targets[j];
            g_hook_graph.count--;
            break;
        }
    }
}

int emit_ninja_file(PithValue *out_path)
{
    const char *p = pv_to_cstr(out_path);
    if (!*p)
        return 0;
    const char *cc = g_hooks.cc[0] ? g_hooks.cc : (getenv("CC") ? getenv("CC") : "cc");
    const char *ar = g_hooks.ar[0] ? g_hooks.ar : (getenv("AR") ? getenv("AR") : "ar");
    return emit_ninja(&g_hook_graph, cc, ar, p) == 0 ? 1 : 0;
}

int emit_posix_make_file(PithValue *out_path)
{
    const char *p = pv_to_cstr(out_path);
    if (!*p)
        return 0;
    const char *cc = g_hooks.cc[0] ? g_hooks.cc : (getenv("CC") ? getenv("CC") : "cc");
    const char *ar = g_hooks.ar[0] ? g_hooks.ar : (getenv("AR") ? getenv("AR") : "ar");
    return emit_makefile(&g_hook_graph, cc, ar, p) == 0 ? 1 : 0;
}

int emit_thorn_file(PithValue *out_path)
{
    const char *p = pv_to_cstr(out_path);
    if (!*p)
        return 0;
    FILE *f = fopen(p, "w");
    if (!f)
        return 0;
    int rc = print_spec(&g_hook_graph, f, (const char **)g_hook_notes, g_hook_nnotes);
    fclose(f);
    return rc == 0 ? 1 : 0;
}

PithValue *to_ninja(void)
{
    char tmp_path[] = "/tmp/thorn_ninja_XXXXXX";
    int fd = mkstemp(tmp_path);
    if (fd < 0)
        return pithNewString("");
    close(fd);
    const char *cc = g_hooks.cc[0] ? g_hooks.cc : (getenv("CC") ? getenv("CC") : "cc");
    const char *ar = g_hooks.ar[0] ? g_hooks.ar : (getenv("AR") ? getenv("AR") : "ar");
    emit_ninja(&g_hook_graph, cc, ar, tmp_path);
    char *data = read_all(tmp_path);
    unlink(tmp_path);
    PithValue *res = pithNewString(data ? data : "");
    free(data);
    return res;
}

PithValue *to_posix_make(void)
{
    char tmp_path[] = "/tmp/thorn_make_XXXXXX";
    int fd = mkstemp(tmp_path);
    if (fd < 0)
        return pithNewString("");
    close(fd);
    const char *cc = g_hooks.cc[0] ? g_hooks.cc : (getenv("CC") ? getenv("CC") : "cc");
    const char *ar = g_hooks.ar[0] ? g_hooks.ar : (getenv("AR") ? getenv("AR") : "ar");
    emit_makefile(&g_hook_graph, cc, ar, tmp_path);
    char *data = read_all(tmp_path);
    unlink(tmp_path);
    PithValue *res = pithNewString(data ? data : "");
    free(data);
    return res;
}

PithValue *to_thorn(void)
{
    char *buf = NULL;
    size_t sz = 0;
    FILE *mf = open_memstream(&buf, &sz);
    if (!mf)
        return pithNewString("");
    print_spec(&g_hook_graph, mf, (const char **)g_hook_notes, g_hook_nnotes);
    fclose(mf);
    PithValue *res = pithNewString(buf ? buf : "");
    free(buf);
    return res;
}

int convert(PithValue *in_path, PithValue *out_path, PithValue *format)
{
    if (!parse_file(in_path))
        return 0;
    const char *fmt = pv_to_cstr(format);
    if (strcmp(fmt, "ninja") == 0)
        return emit_ninja_file(out_path);
    if (strcmp(fmt, "make") == 0 || strcmp(fmt, "posix") == 0 || strcmp(fmt, "posix_make") == 0)
        return emit_posix_make_file(out_path);
    if (strcmp(fmt, "thorn") == 0)
        return emit_thorn_file(out_path);
    return 0;
}

#include "lex.c"
#include "parser.c"
#include "emit.c"
