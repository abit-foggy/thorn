#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pith.h>
#include "include/engine.h"

#ifndef THORN_CORE_BUILD
#include "engine.c"
#endif

/* Hook configuration and state */
typedef struct {
    char pat[128];
    char val[256];
} HookItem;

typedef struct {
    char proj[256];
    char cc[256];
    char ar[256];
    char dir_pfx[512];
    StrList ignore_targets;
    StrList keep_targets;
    StrList strip_cflags;
    HookItem inject_cflags[128];
    size_t ninject_cflags;
    HookItem inject_includes[128];
    size_t ninject_includes;
    StrList remap_from;
    StrList remap_to;
} DecompHooks;

static DecompHooks g_hooks;
static Graph g_hook_graph;
static int g_hook_ready = 0;
static char **g_hook_notes = NULL;
static size_t g_hook_nnotes = 0;

static int pat_match(const char *pat, const char *str)
{
    if (!pat || !str)
        return 0;
    if (strcmp(pat, "*") == 0)
        return 1;
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat)
                return 1;
            while (*str) {
                if (pat_match(pat, str))
                    return 1;
                str++;
            }
            return 0;
        }
        if (*pat != '?' && *pat != *str)
            return 0;
        pat++;
        str++;
    }
    return *str == '\0';
}

static int matches_any(const StrList *list, const char *str)
{
    if (!list || list->count == 0 || !str)
        return 0;
    for (size_t i = 0; i < list->count; i++) {
        if (pat_match(list->items[i], str))
            return 1;
    }
    return 0;
}

/* Notes & diagnostics */



static void note_add(char ***notes, size_t *n, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    char **ng = realloc(*notes, (*n + 1) * sizeof(char *));
    if (!ng)
        return;
    *notes = ng;
    char *d = strdup(buf);
    if (d)
        (*notes)[(*n)++] = d;
}

/* Push an owned copy onto a raw string-pointer array. */
static int tok_push(char ***arr, size_t *n, const char *tok)
{
    char **na = realloc(*arr, (*n + 1) * sizeof(char *));
    if (!na)
        return 0;
    *arr = na;
    char *d = strdup(tok);
    if (!d)
        return 0;
    (*arr)[(*n)++] = d;
    return 1;
}

/* Text helpers */

static char **split_owned(const char *text, size_t *count)
{
    char **out = NULL;
    size_t n = 0, cap = 0;
    const char *p = text;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (!*p)
            break;
        const char *st = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
            p++;
        char *tok = strndup(st, (size_t)(p - st));
        if (!tok)
            break;
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            char **ni = realloc(out, cap * sizeof(char *));
            if (!ni) {
                free(tok);
                break;
            }
            out = ni;
        }
        out[n++] = tok;
    }
    *count = n;
    return out;
}

static void free_tokens(char **toks, size_t n)
{
    for (size_t i = 0; i < n; i++)
        free(toks[i]);
    free(toks);
}

static char *trim_inplace(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    size_t l = strlen(s);
    while (l && (s[l - 1] == ' ' || s[l - 1] == '\t' ||
                 s[l - 1] == '\r' || s[l - 1] == '\n'))
        s[--l] = '\0';
    return s;
}

static int ends_with(const char *s, const char *suffix)
{
    size_t sl = strlen(s), fl = strlen(suffix);
    return sl >= fl && strcmp(s + sl - fl, suffix) == 0;
}

static char *read_all(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) {
        fclose(fp);
        return NULL;
    }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    buf[sz] = '\0';
    return buf;
}

static int is_cc_token(const char *tok)
{
    if (tok[0] == '$')
        tok++;
    static const char *names[] = {
        "cc", "gcc", "clang", "clang++", "g++", "c++", "ld", NULL
    };
    for (int i = 0; names[i]; i++) {
        if (strcmp(tok, names[i]) == 0)
            return 1;
        size_t nl = strlen(names[i]);
        size_t tl = strlen(tok);
        if (tl > nl + 1 && tok[tl - nl - 1] == '/' &&
            strcmp(tok + tl - nl, names[i]) == 0)
            return 1;
    }
    return 0;
}

/* ".o" -> ".c" style single-letter extension swap; malloc'd. */
static char *swap_ext_dup(const char *path, char newext)
{
    size_t l = strlen(path);
    char *s = strdup(path);
    if (!s)
        return NULL;
    if (l >= 2 && s[l - 2] == '.')
        s[l - 1] = newext;
    return s;
}

/* Strip leading directory prefix from target name */
static const char *strip_pfx(const char *s, const char *pfx)
{
    if (pfx && *pfx) {
        size_t l = strlen(pfx);
        if (strncmp(s, pfx, l) == 0)
            return s + l;
    }
    return s;
}

/* Clean target name: strip prefix and library extensions */
static const char *clean_target(const char *raw, const char *pfx,
                                char *buf, size_t n)
{
    const char *s = strip_pfx(raw, pfx);
    snprintf(buf, n, "%s", s);
    size_t l = strlen(buf);
    if (l > 3 && ends_with(buf, ".so"))
        buf[l - 3] = '\0';
    else if (l > 2 && ends_with(buf, ".a"))
        buf[l - 2] = '\0';
    return buf;
}

/* Rule classification */

enum {
    RC_PHONY = 0,
    RC_AR,
    RC_COMPILE,
    RC_SHARED,
    RC_LINK,
    RC_OTHER
};

static int classify_tokens(char **toks, size_t n)
{
    int has_c = 0, has_rcs = 0, has_shared = 0, ccish = 0;
    for (size_t i = 0; i < n; i++) {
        const char *t = toks[i];
        if (strcmp(t, "-c") == 0)
            has_c = 1;
        else if (strcmp(t, "rcs") == 0)
            has_rcs = 1;
        else if (strcmp(t, "-shared") == 0)
            has_shared = 1;
        else if (is_cc_token(t))
            ccish = 1;
    }
    if (has_rcs)
        return RC_AR;
    if (has_c && ccish)
        return RC_COMPILE;
    if (has_shared && ccish)
        return RC_SHARED;
    if (ccish)
        return RC_LINK;
    return RC_OTHER;
}

static int classify_text(const char *text)
{
    size_t nt = 0;
    char **toks = split_owned(text, &nt);
    int rc = classify_tokens(toks, nt);
    free_tokens(toks, nt);
    return rc;
}

/* Flag harvesting */

/* Parse -I includes and compiler flags */
static void harvest_flags_text(const char *text, Target *t)
{
    size_t nt;
    char **toks = split_owned(text, &nt);
    for (size_t i = 0; i < nt; i++) {
        const char *tok = toks[i];
        if (strncmp(tok, "-I", 2) == 0 && tok[2])
            strlist_push(&t->includes, tok + 2);
        else if (strcmp(tok, "-I") == 0 && i + 1 < nt)
            strlist_push(&t->includes, toks[++i]);
        else
            strlist_push(&t->cflags, tok);
    }
    free_tokens(toks, nt);
}

/* Extract compile flags from command line */
static void harvest_cmd_compile_text(const char *text, Target *t)
{
    size_t nt;
    char **toks = split_owned(text, &nt);
    for (size_t i = 0; i < nt; i++) {
        const char *tok = toks[i];
        if (tok[0] == '$')
            continue;
        if (strcmp(tok, "-c") == 0 || strcmp(tok, "-MD") == 0 ||
            strcmp(tok, "-MMD") == 0 || strcmp(tok, "-MP") == 0)
            continue;
        if (strcmp(tok, "-MF") == 0 || strcmp(tok, "-o") == 0) {
            i++;
            continue;
        }
        if (ends_with(tok, ".c") || ends_with(tok, ".s") ||
            ends_with(tok, ".o") || ends_with(tok, ".d"))
            continue;
        if (strncmp(tok, "-I", 2) == 0 && tok[2]) {
            strlist_push(&t->includes, tok + 2);
            continue;
        }
        if (strcmp(tok, "-I") == 0 && i + 1 < nt) {
            strlist_push(&t->includes, toks[++i]);
            continue;
        }
        if (tok[0] == '-')
            strlist_push(&t->cflags, tok);
    }
    free_tokens(toks, nt);
}

/* Extract link flags and archives from command line */
static void harvest_cmd_link_text(const char *text, Target *t)
{
    size_t nt;
    char **toks = split_owned(text, &nt);
    for (size_t i = 0; i < nt; i++) {
        const char *tok = toks[i];
        if (tok[0] == '$')
            continue;
        if (strcmp(tok, "-o") == 0) {
            i++;
            continue;
        }
        if (strcmp(tok, "-shared") == 0 || strcmp(tok, "-c") == 0)
            continue;
        if (ends_with(tok, ".o") || ends_with(tok, ".d"))
            continue;
        if (tok[0] == '-' || ends_with(tok, ".a") ||
            ends_with(tok, ".so"))
            strlist_push(&t->ldflags, tok);
    }
    free_tokens(toks, nt);
}

/* Ninja ingestion */

typedef struct {
    char name[128];
    char *command;                 /* NULL when the rule has none */
} NRule;

typedef struct {
    char **outs;
    size_t nout;
    char rule[128];
    char **ins;
    size_t nin;
    char **imp;
    size_t nimp;
    char **ord;
    size_t nord;
    char **vk;
    char **vv;
    size_t nvars;
} NEdge;

typedef struct {
    NRule *rules;
    size_t nrules, capr;
    NEdge *edges;
    size_t nedges, cape;
    char proj[256];
} NDoc;

static NRule *ndoc_add_rule(NDoc *d)
{
    if (d->nrules == d->capr) {
        size_t nc = d->capr ? d->capr * 2 : 8;
        NRule *nr = realloc(d->rules, nc * sizeof(NRule));
        if (!nr)
            return NULL;
        d->rules = nr;
        d->capr = nc;
    }
    NRule *r = &d->rules[d->nrules++];
    memset(r, 0, sizeof(*r));
    return r;
}

static NEdge *ndoc_add_edge(NDoc *d)
{
    if (d->nedges == d->cape) {
        size_t nc = d->cape ? d->cape * 2 : 8;
        NEdge *ne = realloc(d->edges, nc * sizeof(NEdge));
        if (!ne)
            return NULL;
        d->edges = ne;
        d->cape = nc;
    }
    NEdge *e = &d->edges[d->nedges++];
    memset(e, 0, sizeof(*e));
    return e;
}

static int nedge_var_push(NEdge *e, const char *k, const char *v)
{
    char *kc = strdup(k), *vc = strdup(v);
    if (!kc || !vc) {
        free(kc);
        free(vc);
        return 0;
    }
    char **nk = realloc(e->vk, (e->nvars + 1) * sizeof(char *));
    if (!nk) {
        free(kc);
        free(vc);
        return 0;
    }
    e->vk = nk;
    char **nv = realloc(e->vv, (e->nvars + 1) * sizeof(char *));
    if (!nv) {
        free(kc);
        free(vc);
        return 0;
    }
    e->vv = nv;
    e->vk[e->nvars] = kc;
    e->vv[e->nvars] = vc;
    e->nvars++;
    return 1;
}

static const char *nedge_var(const NEdge *e, const char *key)
{
    for (size_t i = 0; i < e->nvars; i++)
        if (strcmp(e->vk[i], key) == 0)
            return e->vv[i];
    return NULL;
}

static int ninja_rule_index(const NDoc *doc, const char *name)
{
    for (size_t i = 0; i < doc->nrules; i++)
        if (strcmp(doc->rules[i].name, name) == 0)
            return (int)i;
    return -1;
}

static void ninja_doc_free(NDoc *doc)
{
    for (size_t i = 0; i < doc->nrules; i++)
        free(doc->rules[i].command);
    free(doc->rules);
    for (size_t i = 0; i < doc->nedges; i++) {
        NEdge *e = &doc->edges[i];
        free_tokens(e->outs, e->nout);
        free_tokens(e->ins, e->nin);
        free_tokens(e->imp, e->nimp);
        free_tokens(e->ord, e->nord);
        free_tokens(e->vk, e->nvars);
        free_tokens(e->vv, e->nvars);
    }
    free(doc->edges);
    memset(doc, 0, sizeof(*doc));
}

static void parse_ninja(char *mem, NDoc *doc, char ***notes, size_t *nnotes)
{
    char cur_rule[128] = "";
    NEdge *cur_edge = NULL;
    int noted_default = 0, noted_pool = 0, noted_incl = 0, noted_cc = 0,
        noted_dir = 0;

    char *p = mem;
    while (p && *p) {
        char *line = p;
        char *nl = strchr(p, '\n');
        if (nl) {
            *nl = '\0';
            p = nl + 1;
        } else {
            p = NULL;
        }

        /* strip comments: '#' at start or preceded by whitespace */
        for (char *c = line; *c; c++) {
            if (*c == '#' && (c == line || c[-1] == ' ' ||
                              c[-1] == '\t')) {
                *c = '\0';
                break;
            }
        }

        int indented = (*line == ' ' || *line == '\t');
        char *s = trim_inplace(line);
        if (!*s)
            continue;

        if (indented) {
            char *eq = strchr(s, '=');
            if (!eq)
                continue;
            *eq = '\0';
            char *key = trim_inplace(s);
            char *val = trim_inplace(eq + 1);
            if (cur_edge) {
                nedge_var_push(cur_edge, key, val);
            } else if (cur_rule[0] && strcmp(key, "command") == 0) {
                for (size_t i = doc->nrules; i > 0; i--) {
                    if (strcmp(doc->rules[i - 1].name, cur_rule) == 0) {
                        free(doc->rules[i - 1].command);
                        doc->rules[i - 1].command = strdup(val);
                        break;
                    }
                }
            }
            continue;
        }

        /* a non-indented statement closes any open block */
        cur_edge = NULL;
        cur_rule[0] = '\0';

        if (strncmp(s, "rule ", 5) == 0) {
            NRule *r = ndoc_add_rule(doc);
            if (!r)
                return;
            snprintf(r->name, sizeof(r->name), "%s",
                     trim_inplace(s + 5));
            snprintf(cur_rule, sizeof(cur_rule), "%s", r->name);
        } else if (strncmp(s, "build ", 6) == 0) {
            char *body = s + 6;
            char *colon = strchr(body, ':');
            if (!colon) {
                note_add(notes, nnotes,
                         "malformed build statement skipped");
                continue;
            }
            *colon = '\0';
            NEdge *e = ndoc_add_edge(doc);
            if (!e)
                return;
            e->outs = split_owned(trim_inplace(body), &e->nout);
            size_t nt = 0;
            char **toks = split_owned(colon + 1, &nt);
            size_t ti = 0;
            if (nt > 0)
                snprintf(e->rule, sizeof(e->rule), "%s", toks[ti++]);
            int phase = 0;
            for (; ti < nt; ti++) {
                if (strcmp(toks[ti], "|") == 0)
                    phase = 1;
                else if (strcmp(toks[ti], "||") == 0)
                    phase = 2;
                else if (phase == 0)
                    tok_push(&e->ins, &e->nin, toks[ti]);
                else if (phase == 1)
                    tok_push(&e->imp, &e->nimp, toks[ti]);
                else
                    tok_push(&e->ord, &e->nord, toks[ti]);
            }
            free_tokens(toks, nt);
            cur_edge = e;
        } else if (strncmp(s, "default ", 8) == 0) {
            if (!noted_default) {
                noted_default = 1;
                note_add(notes, nnotes,
                         "the `default`/phony aggregate is regenerated "
                         "by thorn, not modeled");
            }
        } else if (strncmp(s, "pool ", 5) == 0) {
            if (!noted_pool) {
                noted_pool = 1;
                note_add(notes, nnotes, "ninja pools are not modeled");
            }
        } else if (strncmp(s, "subninja ", 9) == 0 ||
                   strncmp(s, "include ", 8) == 0) {
            if (!noted_incl) {
                noted_incl = 1;
                note_add(notes, nnotes,
                         "ninja include/subninja fragments are not "
                         "modeled");
            }
        } else {
            char *eq = strchr(s, '=');
            char *co = strchr(s, ':');
            if (eq && (!co || eq < co)) {
                *eq = '\0';
                char *key = trim_inplace(s);
                char *val = trim_inplace(eq + 1);
                if (strcmp(key, "thorn_project") == 0)
                    snprintf(doc->proj, sizeof(doc->proj), "%s", val);
                else if (strcmp(key, "cc") == 0 ||
                         strcmp(key, "ar") == 0) {
                    if (!noted_cc) {
                        noted_cc = 1;
                        note_add(notes, nnotes,
                                 "cc/ar toolchain variables are not "
                                 "modeled; thorn bakes $CC/$AR at "
                                 "configure time");
                    }
                } else if (strcmp(key, "builddir") == 0 && !noted_dir) {
                    noted_dir = 1;
                    note_add(notes, nnotes,
                             "ninja builddir is not modeled");
                }
                /* other global variables are ignored */
            }
        }
    }
}

static void apply_target_hooks(Target *t)
{
    if (!t)
        return;
    if (g_hooks.strip_cflags.count > 0) {
        StrList orig = t->cflags;
        memset(&t->cflags, 0, sizeof(t->cflags));
        for (size_t i = 0; i < orig.count; i++) {
            if (!matches_any(&g_hooks.strip_cflags, orig.items[i]))
                strlist_push(&t->cflags, orig.items[i]);
            free(orig.items[i]);
        }
        free(orig.items);
    }
    for (size_t i = 0; i < g_hooks.ninject_cflags; i++) {
        if (pat_match(g_hooks.inject_cflags[i].pat, t->name))
            strlist_push(&t->cflags, g_hooks.inject_cflags[i].val);
    }
    for (size_t i = 0; i < g_hooks.ninject_includes; i++) {
        if (pat_match(g_hooks.inject_includes[i].pat, t->name))
            strlist_push(&t->includes, g_hooks.inject_includes[i].val);
    }
}

static void map_ninja(const NDoc *doc, Graph *g, const char *pfx,
                      char ***notes, size_t *nnotes)
{
    if (g_hooks.proj[0])
        snprintf(g->proj, sizeof(g->proj), "%s", g_hooks.proj);
    else if (doc->proj[0])
        snprintf(g->proj, sizeof(g->proj), "%s", doc->proj);

    /* classify each defined rule once */
    int *rcls = calloc(doc->nrules ? doc->nrules : 1, sizeof(int));
    if (!rcls)
        return;
    for (size_t i = 0; i < doc->nrules; i++) {
        NRule *r = &doc->rules[i];
        rcls[i] = r->command ? classify_text(r->command) : RC_OTHER;
    }

    size_t ne = doc->nedges;
    int *cls = calloc(ne ? ne : 1, sizeof(int));
    Target **assoc = calloc(ne ? ne : 1, sizeof(Target *));
    char *consumed = calloc(ne ? ne : 1, 1);
    if (!cls || !assoc || !consumed) {
        free(rcls);
        free(cls);
        free(assoc);
        free(consumed);
        return;
    }

    for (size_t i = 0; i < ne; i++) {
        NEdge *e = &doc->edges[i];
        if (strcmp(e->rule, "phony") == 0) {
            cls[i] = RC_PHONY;
            continue;
        }
        int ri = ninja_rule_index(doc, e->rule);
        if (ri < 0) {
            cls[i] = RC_OTHER;
            note_add(notes, nnotes,
                     "edge `%s` references undefined rule `%s`; "
                     "dropped",
                     e->nout ? e->outs[0] : "?", e->rule);
            continue;
        }
        cls[i] = rcls[ri];
        if (cls[i] == RC_OTHER && doc->rules[ri].command && e->nout > 0) {
            const char *out = strip_pfx(e->outs[0], pfx);
            if (!matches_any(&g_hooks.ignore_targets, out)) {
                if (g_hooks.keep_targets.count == 0 || matches_any(&g_hooks.keep_targets, out)) {
                    const char *in = e->nin > 0 ? strip_pfx(e->ins[0], pfx) : "";
                    graph_add_command(g, out, doc->rules[ri].command, in);
                }
            }
            consumed[i] = 1;
        }
    }

    /* pass 1: create targets in file order */
    for (size_t i = 0; i < ne; i++) {
        int c = cls[i];
        if (c == RC_PHONY || c == RC_COMPILE || c == RC_OTHER)
            continue;
        NEdge *e = &doc->edges[i];
        if (e->nout == 0)
            continue;
        char nbuf[512];
        const char *name = clean_target(e->outs[0], pfx, nbuf,
                                        sizeof(nbuf));
        for (size_t rm = 0; rm < g_hooks.remap_from.count; rm++) {
            if (strcmp(g_hooks.remap_from.items[rm], name) == 0) {
                name = g_hooks.remap_to.items[rm];
                break;
            }
        }
        if (matches_any(&g_hooks.ignore_targets, name))
            continue;
        if (g_hooks.keep_targets.count > 0 && !matches_any(&g_hooks.keep_targets, name))
            continue;

        int type = c == RC_AR ? THORN_STATIC_LIB
                : c == RC_SHARED ? THORN_SHARED_LIB
                                 : THORN_EXE;
        Target *t = graph_add(g, name, type);
        if (!t) {
            note_add(notes, nnotes,
                     "duplicate target edge `%s` skipped", name);
            continue;
        }
        assoc[i] = t;
    }

    /* pass 2: attach compile edges through their consumers */
    int noted_hdr = 0;
    for (size_t i = 0; i < ne; i++) {
        if (cls[i] != RC_COMPILE)
            continue;
        NEdge *ce = &doc->edges[i];
        if (ce->nout == 0)
            continue;
        const char *obj = ce->outs[0];

        Target *consumer = NULL;
        for (size_t j = 0; j < ne && !consumer; j++) {
            if (!assoc[j])
                continue;
            NEdge *le = &doc->edges[j];
            for (size_t k = 0; k < le->nin; k++) {
                if (strcmp(le->ins[k], obj) == 0) {
                    consumer = assoc[j];
                    break;
                }
            }
        }
        if (!consumer) {
            note_add(notes, nnotes,
                     "compile edge `%s` has no consumer; dropped", obj);
            continue;
        }
        consumed[i] = 1;

        if (ce->nin > 0)
            strlist_push(&consumer->sources, ce->ins[0]);
        else
            note_add(notes, nnotes,
                     "compile edge `%s` has no input; dropped", obj);

        int ri = ninja_rule_index(doc, ce->rule);
        if (ri >= 0 && doc->rules[ri].command)
            harvest_cmd_compile_text(doc->rules[ri].command, consumer);
        const char *cfl = nedge_var(ce, "cflags");
        if (cfl)
            harvest_flags_text(cfl, consumer);

        for (size_t k = 0; k < ce->nord; k++)
            strlist_push(&consumer->order_deps, ce->ord[k]);
        if (ce->nimp > 0 && !noted_hdr) {
            noted_hdr = 1;
            note_add(notes, nnotes,
                     "implicit header dependencies on compile edges "
                     "are not modeled (depfiles discover them)");
        }
    }

    /* pass 3: link-edge raw inputs, ldflags, order deps */
    for (size_t i = 0; i < ne; i++) {
        if (!assoc[i])
            continue;
        NEdge *e = &doc->edges[i];
        Target *t = assoc[i];

        for (size_t k = 0; k < e->nin; k++) {
            const char *in = e->ins[k];
            if (!ends_with(in, ".o"))
                strlist_push(&t->sources, in);
            /* .o inputs were resolved through compile edges */
        }
        const char *ldf = nedge_var(e, "ldflags");
        if (ldf) {
            size_t nt;
            char **toks = split_owned(ldf, &nt);
            for (size_t k = 0; k < nt; k++)
                strlist_push(&t->ldflags, toks[k]);
            free_tokens(toks, nt);
        }
        for (size_t k = 0; k < e->nimp; k++)
            strlist_push(&t->order_deps, e->imp[k]);
        for (size_t k = 0; k < e->nord; k++)
            strlist_push(&t->order_deps, e->ord[k]);
    }

    /* objects with no compile edge: keep as raw inputs */
    for (size_t i = 0; i < ne; i++) {
        if (!assoc[i])
            continue;
        NEdge *e = &doc->edges[i];
        Target *t = assoc[i];
        for (size_t k = 0; k < e->nin; k++) {
            const char *in = e->ins[k];
            if (!ends_with(in, ".o"))
                continue;
            int modeled = 0;
            for (size_t j = 0; j < ne; j++) {
                if (cls[j] != RC_COMPILE || !consumed[j])
                    continue;
                if (strcmp(doc->edges[j].outs[0], in) == 0) {
                    modeled = 1;
                    break;
                }
            }
            if (!modeled) {
                strlist_push(&t->sources, in);
                note_add(notes, nnotes,
                         "object `%s` has no compile edge; kept as a "
                         "raw link input", in);
            }
        }
    }

    for (size_t i = 0; i < g->count; i++)
        apply_target_hooks(&g->targets[i]);

    free(consumed);
    free(cls);
    free(assoc);
    free(rcls);
}

/* Makefile ingestion */

typedef struct {
    char **vk;
    char **vv;
    size_t nvars, cap;
} MVars;

typedef struct {
    char **targets;
    size_t ntgt;
    char **prereqs;
    size_t nprq;
    char **recipe;
    size_t nrec, caprec;
} MRule;

typedef struct {
    MVars vars;
    MRule *rules;
    size_t nrules, cap;
} MDoc;

static const char *mvar_get(const MVars *v, const char *k)
{
    for (size_t i = 0; i < v->nvars; i++)
        if (strcmp(v->vk[i], k) == 0)
            return v->vv[i];
    return NULL;
}

static void mvar_set(MVars *v, const char *k, const char *val)
{
    for (size_t i = 0; i < v->nvars; i++) {
        if (strcmp(v->vk[i], k) == 0) {
            free(v->vv[i]);
            v->vv[i] = strdup(val);
            return;
        }
    }
    char **nk = realloc(v->vk, (v->nvars + 1) * sizeof(char *));
    if (!nk)
        return;
    v->vk = nk;
    char **nv = realloc(v->vv, (v->nvars + 1) * sizeof(char *));
    if (!nv)
        return;
    v->vv = nv;
    v->vk[v->nvars] = strdup(k);
    v->vv[v->nvars] = strdup(val);
    v->nvars++;
}

static void mvar_append(MVars *v, const char *k, const char *val)
{
    const char *old = mvar_get(v, k);
    if (!old || !*old) {
        mvar_set(v, k, val);
        return;
    }
    size_t len = strlen(old) + 1 + strlen(val) + 1;
    char *buf = malloc(len);
    if (!buf)
        return;
    snprintf(buf, len, "%s %s", old, val);
    mvar_set(v, k, buf);
    free(buf);
}

static MRule *mdoc_add_rule(MDoc *d)
{
    if (d->nrules == d->cap) {
        size_t nc = d->cap ? d->cap * 2 : 8;
        MRule *nr = realloc(d->rules, nc * sizeof(MRule));
        if (!nr)
            return NULL;
        d->rules = nr;
        d->cap = nc;
    }
    MRule *r = &d->rules[d->nrules++];
    memset(r, 0, sizeof(*r));
    return r;
}

static void mrule_push_recipe(MRule *r, const char *line)
{
    char **nr = realloc(r->recipe, (r->nrec + 1) * sizeof(char *));
    if (!nr)
        return;
    r->recipe = nr;
    r->recipe[r->nrec++] = strdup(line);
}

static void mdoc_free(MDoc *d)
{
    for (size_t i = 0; i < d->vars.nvars; i++) {
        free(d->vars.vk[i]);
        free(d->vars.vv[i]);
    }
    free(d->vars.vk);
    free(d->vars.vv);
    for (size_t i = 0; i < d->nrules; i++) {
        MRule *r = &d->rules[i];
        free_tokens(r->targets, r->ntgt);
        free_tokens(r->prereqs, r->nprq);
        free_tokens(r->recipe, r->nrec);
    }
    free(d->rules);
    memset(d, 0, sizeof(*d));
}

/* Expand $(VAR) references using parsed variables */
static char *str_expand(const char *in, const MVars *v)
{
    size_t cap = strlen(in) * 2 + 64, len = 0;
    char *out = malloc(cap);
    if (!out)
        return NULL;
    out[0] = '\0';

    const char *p = in;
    while (*p) {
        if (p[0] == '$' && p[1] == '(') {
            const char *close = strchr(p + 2, ')');
            if (close) {
                char name[128];
                size_t nl = (size_t)(close - (p + 2));
                if (nl >= sizeof(name))
                    nl = sizeof(name) - 1;
                memcpy(name, p + 2, nl);
                name[nl] = '\0';
                const char *val = mvar_get(v, name);
                const char *src = val ? val : p;
                size_t vl = val ? strlen(val)
                                : (size_t)(close - p) + 1;
                while (len + vl + 1 > cap) {
                    cap *= 2;
                    char *no = realloc(out, cap);
                    if (!no) {
                        free(out);
                        return NULL;
                    }
                    out = no;
                }
                memcpy(out + len, src, vl);
                len += vl;
                out[len] = '\0';
                p = close + 1;
                continue;
            }
        }
        if (len + 2 > cap) {
            cap *= 2;
            char *no = realloc(out, cap);
            if (!no) {
                free(out);
                return NULL;
            }
            out = no;
        }
        out[len++] = *p++;
        out[len] = '\0';
    }
    return out;
}

/* Join recipe lines of a rule into one expanded malloc'd string. */
static char *rule_recipe_text(const MRule *r, const MVars *v)
{
    size_t jcap = 64, jlen = 0;
    char *joined = malloc(jcap);
    if (!joined)
        return NULL;
    joined[0] = '\0';
    for (size_t k = 0; k < r->nrec; k++) {
        size_t rl = strlen(r->recipe[k]);
        while (jlen + rl + 2 > jcap) {
            jcap *= 2;
            char *nj = realloc(joined, jcap);
            if (!nj) {
                free(joined);
                return NULL;
            }
            joined = nj;
        }
        memcpy(joined + jlen, r->recipe[k], rl);
        jlen += rl;
        joined[jlen++] = ' ';
        joined[jlen] = '\0';
    }
    char *exp = str_expand(joined, v);
    free(joined);
    return exp;
}

static void preprocess_make_lines(char *str)
{
    char *r = str;
    char *w = str;
    while (*r) {
        if (*r == '\\') {
            char *p = r + 1;
            while (*p == ' ' || *p == '\t')
                p++;
            if (*p == '\r' && *(p + 1) == '\n') {
                *w++ = ' ';
                r = p + 2;
                continue;
            } else if (*p == '\n') {
                *w++ = ' ';
                r = p + 1;
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
}

static void parse_make(char *mem, MDoc *doc, char ***notes, size_t *nnotes)
{
    preprocess_make_lines(mem);
    MRule *cur = NULL;
    int noted_directive = 0;

    char *p = mem;
    while (p && *p) {
        char *line = p;
        char *nl = strchr(p, '\n');
        if (nl) {
            *nl = '\0';
            p = nl + 1;
        } else {
            p = NULL;
        }

        if (*line == '\t') {
            if (cur) {
                char *rc = line;
                while (*rc == '\t')
                    rc++;
                mrule_push_recipe(cur, trim_inplace(rc));
            }
            continue;
        }

        char *s = trim_inplace(line);
        if (!*s || *s == '#')
            continue;
        if (strncmp(s, "-include", 8) == 0 ||
            strncmp(s, "sinclude", 8) == 0 ||
            strncmp(s, "include", 7) == 0) {
            /* generated dependency fragments: thorn regenerates these */
            continue;
        }
        if (strncmp(s, "export", 6) == 0 ||
            strncmp(s, "unexport", 8) == 0 ||
            strncmp(s, "ifeq", 4) == 0 || strncmp(s, "ifneq", 5) == 0 ||
            strncmp(s, "ifdef", 5) == 0 || strncmp(s, "ifndef", 6) == 0 ||
            strncmp(s, "else", 4) == 0 || strncmp(s, "endif", 5) == 0) {
            if (!noted_directive) {
                noted_directive = 1;
                note_add(notes, nnotes,
                         "make conditionals/exports are not modeled");
            }
            continue;
        }

        char *eq = strchr(s, '=');
        char *co = strchr(s, ':');
        int is_assign = 0;
        if (eq) {
            if (!co || eq < co)
                is_assign = 1;
            else if (co + 1 == eq || (co > s && *(co - 1) == ':' && co == eq - 1))
                is_assign = 1;
        }
        if (is_assign) {
            *eq = '\0';
            char *key = trim_inplace(s);
            size_t kl = strlen(key);
            int is_append = 0;
            while (kl && (key[kl - 1] == '?' || key[kl - 1] == ':' ||
                          key[kl - 1] == '+' || key[kl - 1] == '!')) {
                if (key[kl - 1] == '+')
                    is_append = 1;
                key[--kl] = '\0';
                key = trim_inplace(key);
            }
            char *val = trim_inplace(eq + 1);
            if (is_append)
                mvar_append(&doc->vars, key, val);
            else
                mvar_set(&doc->vars, key, val);
            cur = NULL;
            continue;
        }
        if (co) {
            *co = '\0';
            char *tgt_text = trim_inplace(s);
            char *exp = str_expand(co + 1, &doc->vars);
            MRule *r = mdoc_add_rule(doc);
            if (!r) {
                free(exp);
                return;
            }
            r->targets = split_owned(tgt_text, &r->ntgt);
            r->prereqs = split_owned(exp ? exp : "", &r->nprq);
            free(exp);
            cur = r;
            continue;
        }
        cur = NULL;
    }
}

/* Per-object record rebuilt from explicit compile rules. */
typedef struct {
    char *obj;
    char *src;
    StrList incs, cfl, ords;
    int consumed;
} ObjInfo;

static ObjInfo *objmap_find(ObjInfo *m, size_t n, const char *obj)
{
    for (size_t i = 0; i < n; i++)
        if (strcmp(m[i].obj, obj) == 0)
            return &m[i];
    return NULL;
}


static void subst_make_to_cmd(const char *in, char *out, size_t n)
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
            if (in[i + 1] == '@') {
                if (o + 4 < n) {
                    out[o++] = '$';
                    out[o++] = 'o';
                    out[o++] = 'u';
                    out[o++] = 't';
                }
                i += 2;
                continue;
            }
            if (in[i + 1] == '<') {
                if (o + 3 < n) {
                    out[o++] = '$';
                    out[o++] = 'i';
                    out[o++] = 'n';
                }
                i += 2;
                continue;
            }
        }
        out[o++] = in[i++];
    }
    out[o] = '\0';
}

static void map_make(const MDoc *doc, Graph *g, const char *pfx,
                     char ***notes, size_t *nnotes)
{
    if (g_hooks.proj[0])
        snprintf(g->proj, sizeof(g->proj), "%s", g_hooks.proj);
    else {
        const char *pv = mvar_get(&doc->vars, "thorn_project");
        if (pv)
            snprintf(g->proj, sizeof(g->proj), "%s", pv);
    }

    size_t nr = doc->nrules;
    int *cls = calloc(nr ? nr : 1, sizeof(int));
    if (!cls)
        return;

    /* classify every rule once from its (expanded) recipe */
    for (size_t i = 0; i < nr; i++) {
        char *rec = rule_recipe_text(&doc->rules[i], &doc->vars);
        cls[i] = rec ? classify_text(rec) : RC_OTHER;
        free(rec);
    }

    int noted_clean = 0, noted_multi = 0, noted_unmodeled = 0,
        noted_pattern = 0, noted_heuristic = 0, noted_global_c = 0,
        noted_global_l = 0, noted_direct_src = 0;

    /* pass 1: explicit compile rules -> objmap; pattern rules */
    ObjInfo *objmap = NULL;
    size_t nobj = 0, cobj = 0;
    Target pat_holder;
    memset(&pat_holder, 0, sizeof(pat_holder));
    int has_pattern = 0;

    for (size_t i = 0; i < nr; i++) {
        MRule *r = &doc->rules[i];
        if (r->ntgt == 0 || cls[i] != RC_COMPILE)
            continue;
        const char *tgt = r->targets[0];

        if (strchr(tgt, '%')) {
            has_pattern = 1;
            char *rec = rule_recipe_text(r, &doc->vars);
            if (rec) {
                harvest_cmd_compile_text(rec, &pat_holder);
                free(rec);
            }
            continue;
        }

        if (nobj == cobj) {
            cobj = cobj ? cobj * 2 : 8;
            ObjInfo *nm = realloc(objmap, cobj * sizeof(ObjInfo));
            if (!nm)
                continue;
            objmap = nm;
        }
        ObjInfo *oi = &objmap[nobj++];
        memset(oi, 0, sizeof(*oi));
        oi->obj = strdup(tgt);

        /* the source: a .c prerequisite, else the token after -c */
        const char *src = NULL;
        for (size_t k = 0; k < r->nprq; k++) {
            if (ends_with(r->prereqs[k], ".c")) {
                src = r->prereqs[k];
                break;
            }
        }
        if (!src) {
            char *rec = rule_recipe_text(r, &doc->vars);
            if (rec) {
                size_t nt;
                char **toks = split_owned(rec, &nt);
                for (size_t t = 0; t + 1 < nt; t++) {
                    if (strcmp(toks[t], "-c") == 0 &&
                        ends_with(toks[t + 1], ".c")) {
                        src = toks[t + 1];
                        break;
                    }
                }
                if (src)
                    oi->src = strdup(src);
                free_tokens(toks, nt);
                free(rec);
            }
        } else {
            oi->src = strdup(src);
        }
        if (!oi->src) {
            oi->src = swap_ext_dup(tgt, 'c');
            if (!noted_heuristic) {
                noted_heuristic = 1;
                note_add(notes, nnotes,
                         "some objects had no visible source; guessed "
                         "by swapping .o to .c");
            }
        }

        /* flags from the recipe */
        char *rec = rule_recipe_text(r, &doc->vars);
        if (rec) {
            Target tmp;
            memset(&tmp, 0, sizeof(tmp));
            harvest_cmd_compile_text(rec, &tmp);
            free(rec);
            for (size_t x = 0; x < tmp.includes.count; x++)
                strlist_push(&oi->incs, tmp.includes.items[x]);
            for (size_t x = 0; x < tmp.cflags.count; x++)
                strlist_push(&oi->cfl, tmp.cflags.items[x]);
            strlist_free(&tmp.includes);
            strlist_free(&tmp.cflags);
        }

        /* extra prereqs beyond the source become order deps */
        for (size_t k = 0; k < r->nprq; k++) {
            const char *pr = r->prereqs[k];
            if (strcmp(pr, oi->src) == 0 || ends_with(pr, ".o"))
                continue;
            strlist_push(&oi->ords, pr);
        }
    }

    /* pass 2: link/archive rules -> targets, consuming the objmap */
    for (size_t i = 0; i < nr; i++) {
        MRule *r = &doc->rules[i];
        if (r->ntgt == 0)
            continue;
        char nbuf[512];
        const char *tgt = clean_target(r->targets[0], pfx, nbuf,
                                       sizeof(nbuf));
        if (strcmp(tgt, ".PHONY") == 0 || strcmp(tgt, "all") == 0)
            continue;
        if (strcmp(tgt, "clean") == 0) {
            if (!noted_clean) {
                noted_clean = 1;
                note_add(notes, nnotes,
                         "the clean rule is regenerated by thorn, not "
                         "modeled");
            }
            continue;
        }
        int c = cls[i];
        if (c != RC_AR && c != RC_SHARED && c != RC_LINK)
            continue;
        if (strchr(tgt, '%')) {
            if (!noted_pattern) {
                noted_pattern = 1;
                note_add(notes, nnotes,
                         "pattern link rules are not modeled");
            }
            continue;
        }
        if (r->ntgt > 1 && !noted_multi) {
            noted_multi = 1;
            note_add(notes, nnotes,
                     "multi-target rules: only the first target is "
                     "modeled");
        }

        for (size_t rm = 0; rm < g_hooks.remap_from.count; rm++) {
            if (strcmp(g_hooks.remap_from.items[rm], tgt) == 0) {
                tgt = g_hooks.remap_to.items[rm];
                break;
            }
        }
        if (matches_any(&g_hooks.ignore_targets, tgt))
            continue;
        if (g_hooks.keep_targets.count > 0 && !matches_any(&g_hooks.keep_targets, tgt))
            continue;

        int type = c == RC_AR || ends_with(tgt, ".a") ? THORN_STATIC_LIB
                : c == RC_SHARED || ends_with(tgt, ".so")
                    ? THORN_SHARED_LIB
                    : THORN_EXE;
        Target *t = graph_add(g, tgt, type);
        if (!t) {
            note_add(notes, nnotes,
                     "duplicate target rule `%s` skipped", tgt);
            continue;
        }

        int pat_attached = 0;
        for (size_t k = 0; k < r->nprq; k++) {
            const char *pr = r->prereqs[k];
            if (ends_with(pr, ".o")) {
                ObjInfo *oi = objmap_find(objmap, nobj, pr);
                if (oi) {
                    oi->consumed = 1;
                    strlist_push(&t->sources, oi->src);
                    for (size_t x = 0; x < oi->incs.count; x++)
                        strlist_push(&t->includes,
                                     oi->incs.items[x]);
                    for (size_t x = 0; x < oi->cfl.count; x++)
                        strlist_push(&t->cflags, oi->cfl.items[x]);
                    for (size_t x = 0; x < oi->ords.count; x++)
                        strlist_push(&t->order_deps,
                                     oi->ords.items[x]);
                } else if (has_pattern) {
                    char *src = swap_ext_dup(strip_pfx(pr, pfx), 'c');
                    strlist_push(&t->sources, src);
                    if (!pat_attached) {
                        pat_attached = 1;
                        for (size_t x = 0;
                             x < pat_holder.includes.count; x++)
                            strlist_push(&t->includes,
                                         pat_holder.includes.items[x]);
                        for (size_t x = 0; x < pat_holder.cflags.count;
                             x++)
                            strlist_push(&t->cflags,
                                         pat_holder.cflags.items[x]);
                    }
                    note_add(notes, nnotes,
                             "object `%s` follows the pattern rule; "
                             "assuming source `%s`", pr, src);
                    free(src);
                } else {
                    strlist_push(&t->sources, pr);
                    note_add(notes, nnotes,
                             "object `%s` has no compile rule; kept "
                             "as a raw link input", pr);
                }
            } else if (ends_with(pr, ".a") || ends_with(pr, ".so")) {
                strlist_push(&t->ldflags, pr);
            } else if (ends_with(pr, ".c")) {
                strlist_push(&t->sources, pr);
                if (!noted_direct_src) {
                    noted_direct_src = 1;
                    note_add(notes, nnotes,
                             "sources linked directly without an "
                             "object rule are kept as sources");
                }
            } else {
                strlist_push(&t->order_deps, pr);
            }
        }

        /* link recipe residue -> ldflags */
        char *rec = rule_recipe_text(r, &doc->vars);
        if (rec) {
            harvest_cmd_link_text(rec, t);
            free(rec);
        }

        /* global CFLAGS/LDFLAGS fallbacks */
        if (t->cflags.count == 0 && t->includes.count == 0) {
            const char *cf = mvar_get(&doc->vars, "CFLAGS");
            if (cf && *cf) {
                harvest_flags_text(cf, t);
                if (!noted_global_c) {
                    noted_global_c = 1;
                    note_add(notes, nnotes,
                             "global CFLAGS applied where per-object "
                             "flags were absent");
                }
            }
        }
        {
            const char *lf = mvar_get(&doc->vars, "LDFLAGS");
            if (lf && *lf) {
                harvest_cmd_link_text(lf, t);
                if (!noted_global_l) {
                    noted_global_l = 1;
                    note_add(notes, nnotes,
                             "global LDFLAGS applied to every target");
                }
            }
        }
    }

    /* Kbuild composite objects or obj-y/obj-m fallback */
    if (g->count == 0) {
        for (size_t i = 0; i < doc->vars.nvars; i++) {
            const char *vk = doc->vars.vk[i];
            const char *vv = doc->vars.vv[i];
            if (!vk || !vv || !*vv)
                continue;
            const char *suffix = NULL;
            if (ends_with(vk, "-objs"))
                suffix = "-objs";
            else if (ends_with(vk, "-y") && strcmp(vk, "obj-y") != 0 &&
                     strcmp(vk, "ccflags-y") != 0 && strcmp(vk, "asflags-y") != 0)
                suffix = "-y";

            if (suffix) {
                char tname[256];
                size_t nlen = strlen(vk) - strlen(suffix);
                if (nlen >= sizeof(tname))
                    nlen = sizeof(tname) - 1;
                memcpy(tname, vk, nlen);
                tname[nlen] = '\0';

                const char *tgt = tname;
                for (size_t rm = 0; rm < g_hooks.remap_from.count; rm++) {
                    if (strcmp(g_hooks.remap_from.items[rm], tgt) == 0) {
                        tgt = g_hooks.remap_to.items[rm];
                        break;
                    }
                }
                if (matches_any(&g_hooks.ignore_targets, tgt))
                    continue;
                if (g_hooks.keep_targets.count > 0 && !matches_any(&g_hooks.keep_targets, tgt))
                    continue;

                Target *t = graph_add(g, tgt, THORN_STATIC_LIB);
                if (!t)
                    continue;

                size_t nt = 0;
                char **toks = split_owned(vv, &nt);
                for (size_t k = 0; k < nt; k++) {
                    const char *tok = toks[k];
                    if (ends_with(tok, ".o")) {
                        ObjInfo *oi = objmap_find(objmap, nobj, tok);
                        if (oi) {
                            oi->consumed = 1;
                            strlist_push(&t->sources, oi->src);
                        } else {
                            char *src = swap_ext_dup(strip_pfx(tok, pfx), 'c');
                            if (src) {
                                strlist_push(&t->sources, src);
                                free(src);
                            }
                        }
                    } else if (ends_with(tok, ".c")) {
                        strlist_push(&t->sources, strip_pfx(tok, pfx));
                    }
                }
                free_tokens(toks, nt);
            }
        }

        const char *obj_y = mvar_get(&doc->vars, "obj-y");
        const char *obj_m = mvar_get(&doc->vars, "obj-m");
        if ((obj_y || obj_m) && g->count == 0) {
            const char *def_tgt = g_hooks.proj[0] ? g_hooks.proj : (g->proj[0] ? g->proj : "kbuild_target");
            Target *t = graph_add(g, def_tgt, THORN_STATIC_LIB);
            if (t) {
                const char *lists[2] = { obj_y, obj_m };
                for (int l = 0; l < 2; l++) {
                    if (!lists[l]) continue;
                    size_t nt = 0;
                    char **toks = split_owned(lists[l], &nt);
                    for (size_t k = 0; k < nt; k++) {
                        const char *tok = toks[k];
                        if (ends_with(tok, ".o")) {
                            ObjInfo *oi = objmap_find(objmap, nobj, tok);
                            if (oi) {
                                oi->consumed = 1;
                                strlist_push(&t->sources, oi->src);
                            } else {
                                char *src = swap_ext_dup(strip_pfx(tok, pfx), 'c');
                                if (src) {
                                    strlist_push(&t->sources, src);
                                    free(src);
                                }
                            }
                        } else if (ends_with(tok, ".c")) {
                            strlist_push(&t->sources, strip_pfx(tok, pfx));
                        }
                    }
                    free_tokens(toks, nt);
                }
            }
        }
    }

    const char *ccflags = mvar_get(&doc->vars, "ccflags-y");
    if (!ccflags) ccflags = mvar_get(&doc->vars, "EXTRA_CFLAGS");
    if (ccflags && *ccflags) {
        for (size_t i = 0; i < g->count; i++) {
            harvest_flags_text(ccflags, &g->targets[i]);
        }
    }

    /* orphans and unmodeled rules */
    for (size_t i = 0; i < nobj; i++) {
        if (!objmap[i].consumed)
            note_add(notes, nnotes,
                     "object `%s` (source `%s`) has no consumer; "
                     "dropped", objmap[i].obj, objmap[i].src);
    }
    for (size_t i = 0; i < nr; i++) {
        MRule *r = &doc->rules[i];
        if (r->ntgt == 0 || cls[i] != RC_OTHER || r->nrec == 0)
            continue;
        const char *tgt = r->targets[0];
        if (strcmp(tgt, ".PHONY") == 0 || strcmp(tgt, "all") == 0 ||
            strcmp(tgt, "clean") == 0)
            continue;
        const char *out = strip_pfx(tgt, pfx);
        if (matches_any(&g_hooks.ignore_targets, out))
            continue;
        if (g_hooks.keep_targets.count > 0 && !matches_any(&g_hooks.keep_targets, out))
            continue;
        char *rec = rule_recipe_text(r, &doc->vars);
        if (rec) {
            char cmd_buf[2048];
            subst_make_to_cmd(rec, cmd_buf, sizeof(cmd_buf));
            const char *in = r->nprq > 0 ? strip_pfx(r->prereqs[0], pfx) : "";
            graph_add_command(g, out, cmd_buf, in);
            free(rec);
            continue;
        }
        if (!noted_unmodeled) {
            noted_unmodeled = 1;
            note_add(notes, nnotes,
                     "unrecognized recipe rules (e.g. `%s`) are "
                     "dropped", tgt);
        }
    }

    for (size_t i = 0; i < g->count; i++)
        apply_target_hooks(&g->targets[i]);

    /* cleanup */
    for (size_t i = 0; i < nobj; i++) {
        free(objmap[i].obj);
        free(objmap[i].src);
        strlist_free(&objmap[i].incs);
        strlist_free(&objmap[i].cfl);
        strlist_free(&objmap[i].ords);
    }
    free(objmap);
    free(cls);
    strlist_free(&pat_holder.includes);
    strlist_free(&pat_holder.cflags);
}

/* Decompile entry point */

static int sniff_is_ninja(const char *mem)
{
    const char *p = mem;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        const char *s = p;
        while (*s == ' ' || *s == '\t')
            s++;
        size_t sl = 0;
        while (s[sl] && s[sl] != '\n' && s[sl] != '\r')
            sl++;
        if (sl > 0) {
            if (strncmp(s, "rule ", 5) == 0 ||
                strncmp(s, "build ", 6) == 0 ||
                strncmp(s, "ninja_required_version", 22) == 0)
                return 1;
        }
        p = nl ? nl + 1 : NULL;
    }
    return 0;
}

int decompile_file(const char *path, Graph *g, char ***notes,
                         size_t *nnotes)
{
    *notes = NULL;
    *nnotes = 0;

    char *mem = read_all(path);
    if (!mem) {
        diag("cannot read %s", path);
        return 1;
    }

    int rc = 1;
    if (sniff_is_ninja(mem)) {
        char pfx[2048];
        if (g_hooks.dir_pfx[0])
            snprintf(pfx, sizeof(pfx), "%s", g_hooks.dir_pfx);
        else
            dir_prefix(path, pfx, sizeof(pfx));
        snprintf(g->backend, sizeof(g->backend), "ninja");
        NDoc doc;
        memset(&doc, 0, sizeof(doc));
        parse_ninja(mem, &doc, notes, nnotes);
        map_ninja(&doc, g, pfx, notes, nnotes);
        ninja_doc_free(&doc);
        rc = (g->count == 0 && g->cmd_count == 0);
    } else {
        char pfx[2048];
        if (g_hooks.dir_pfx[0])
            snprintf(pfx, sizeof(pfx), "%s", g_hooks.dir_pfx);
        else
            dir_prefix(path, pfx, sizeof(pfx));
        snprintf(g->backend, sizeof(g->backend), "make");
        MDoc doc;
        memset(&doc, 0, sizeof(doc));
        parse_make(mem, &doc, notes, nnotes);
        map_make(&doc, g, pfx, notes, nnotes);
        mdoc_free(&doc);
        rc = (g->count == 0 && g->cmd_count == 0);
    }

    free(mem);
    if (rc)
        diag("no buildable targets or commands found in %s", path);
    return rc;
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
    if (g_hook_ready && g_hooks.proj[0])
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
    Target *t = graph_find(&g_hook_graph, n);
    if (t)
        t->type = (int)type;
}

void add_target_source(PithValue *target_name, PithValue *src)
{
    const char *n = pv_to_cstr(target_name);
    const char *s = pv_to_cstr(src);
    Target *t = graph_find(&g_hook_graph, n);
    if (t && *s)
        strlist_push(&t->sources, s);
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
