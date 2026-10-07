#ifndef THORN_PARSER_H
#define THORN_PARSER_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "lex.h"

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

extern DecompHooks g_hooks;

/* Ninja AST */
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

NRule *ndoc_add_rule(NDoc *d);
NEdge *ndoc_add_edge(NDoc *d);
int nedge_var_push(NEdge *e, const char *k, const char *v);
const char *nedge_var(const NEdge *e, const char *key);
int ninja_rule_index(const NDoc *doc, const char *name);
void ninja_doc_free(NDoc *doc);
void parse_ninja(char *mem, NDoc *doc, char ***notes, size_t *nnotes);

/* Make AST */
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
    char proj[256];
} MDoc;

typedef struct {
    char *obj;
    char *src;
    StrList incs, cfl, ords;
    int consumed;
} ObjInfo;

const char *mvar_get(const MVars *v, const char *k);
void mvar_set(MVars *v, const char *k, const char *val);
void mvar_append(MVars *v, const char *k, const char *val);
MRule *mdoc_add_rule(MDoc *d);
void mrule_push_recipe(MRule *r, const char *line);
void mdoc_free(MDoc *d);
char *str_expand(const char *in, const MVars *v);
char *rule_recipe_text(const MRule *r, const MVars *v);
void preprocess_make_lines(char *str);
void parse_make(char *mem, MDoc *doc, char ***notes, size_t *nnotes);
ObjInfo *objmap_find(ObjInfo *m, size_t n, const char *obj);
void subst_make_to_cmd(const char *in, char *out, size_t n);

int sniff_is_ninja(const char *mem);

#endif /* THORN_PARSER_H */
