#include "include/parser.h"

NRule *ndoc_add_rule(NDoc *d)
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

NEdge *ndoc_add_edge(NDoc *d)
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

int nedge_var_push(NEdge *e, const char *k, const char *v)
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

const char *nedge_var(const NEdge *e, const char *key)
{
    for (size_t i = 0; i < e->nvars; i++)
        if (strcmp(e->vk[i], key) == 0)
            return e->vv[i];
    return NULL;
}

int ninja_rule_index(const NDoc *doc, const char *name)
{
    for (size_t i = 0; i < doc->nrules; i++)
        if (strcmp(doc->rules[i].name, name) == 0)
            return (int)i;
    return -1;
}

void ninja_doc_free(NDoc *doc)
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

void parse_ninja(char *mem, NDoc *doc, char ***notes, size_t *nnotes)
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
        if (line[0] == '#') {
            const char *proj_tag = "project:";
            char *found = strstr(line, proj_tag);
            if (found && !doc->proj[0]) {
                found += strlen(proj_tag);
                while (*found == ' ')
                    found++;
                char *end = found;
                while (*end && *end != ' ' && *end != '(' && *end != '\r')
                    end++;
                size_t l = (size_t)(end - found);
                if (l >= sizeof(doc->proj))
                    l = sizeof(doc->proj) - 1;
                memcpy(doc->proj, found, l);
                doc->proj[l] = '\0';
            }
            continue;
        }

        if (line[0] == ' ' || line[0] == '\t') {
            char *ind = trim_inplace(line);
            if (cur_rule[0]) {
                if (strncmp(ind, "command", 7) == 0 &&
                    (ind[7] == ' ' || ind[7] == '=')) {
                    char *eq = strchr(ind, '=');
                    if (eq) {
                        char *cmd = trim_inplace(eq + 1);
                        int idx = ninja_rule_index(doc, cur_rule);
                        if (idx >= 0) {
                            free(doc->rules[idx].command);
                            doc->rules[idx].command = strdup(cmd);
                        }
                    }
                }
            } else if (cur_edge) {
                char *eq = strchr(ind, '=');
                if (eq) {
                    *eq = '\0';
                    char *k = trim_inplace(ind);
                    char *v = trim_inplace(eq + 1);
                    nedge_var_push(cur_edge, k, v);
                }
            }
            continue;
        }

        cur_rule[0] = '\0';
        cur_edge = NULL;
        char *s = trim_inplace(line);
        if (!*s)
            continue;

        if (strncmp(s, "rule ", 5) == 0) {
            char *rname = trim_inplace(s + 5);
            snprintf(cur_rule, sizeof(cur_rule), "%s", rname);
            NRule *r = ndoc_add_rule(doc);
            if (r)
                snprintf(r->name, sizeof(r->name), "%s", rname);
        } else if (strncmp(s, "build ", 6) == 0) {
            char *b = s + 6;
            char *colon = strchr(b, ':');
            if (!colon)
                continue;
            *colon = '\0';
            char *out_part = trim_inplace(b);
            char *rest = trim_inplace(colon + 1);

            NEdge *e = ndoc_add_edge(doc);
            if (!e)
                continue;
            cur_edge = e;

            char *op = out_part;
            while (*op) {
                while (*op == ' ' || *op == '\t')
                    op++;
                if (!*op)
                    break;
                char *st = op;
                while (*op && *op != ' ' && *op != '\t')
                    op++;
                char save = *op;
                *op = '\0';
                tok_push(&e->outs, &e->nout, st);
                if (save) {
                    *op = save;
                    op++;
                }
            }

            char *rname = rest;
            while (*rest && *rest != ' ' && *rest != '\t' &&
                   *rest != '|' && *rest != ':')
                rest++;
            char rsave = *rest;
            *rest = '\0';
            snprintf(e->rule, sizeof(e->rule), "%s", rname);
            if (rsave)
                *rest = rsave;

            char *in_start = rest;
            char *bar1 = strchr(in_start, '|');
            char *bar2 = bar1 && *(bar1 + 1) == '|' ? bar1 :
                         (bar1 ? strchr(bar1 + 1, '|') : NULL);

            char *exp_in = in_start;
            char *imp_in = NULL;
            char *ord_in = NULL;

            if (bar1 && bar1 == bar2) {
                *bar1 = '\0';
                ord_in = bar1 + 2;
            } else if (bar1 && bar2) {
                *bar1 = '\0';
                imp_in = bar1 + 1;
                *bar2 = '\0';
                ord_in = bar2 + 2;
            } else if (bar1) {
                *bar1 = '\0';
                imp_in = bar1 + 1;
            }

            size_t nt;
            char **toks = split_owned(exp_in, &nt);
            for (size_t i = 0; i < nt; i++)
                tok_push(&e->ins, &e->nin, toks[i]);
            free_tokens(toks, nt);

            if (imp_in) {
                toks = split_owned(imp_in, &nt);
                for (size_t i = 0; i < nt; i++)
                    tok_push(&e->imp, &e->nimp, toks[i]);
                free_tokens(toks, nt);
            }
            if (ord_in) {
                toks = split_owned(ord_in, &nt);
                for (size_t i = 0; i < nt; i++)
                    tok_push(&e->ord, &e->nord, toks[i]);
                free_tokens(toks, nt);
            }
        } else if (strncmp(s, "default ", 8) == 0) {
            if (!noted_default) {
                note_add(notes, nnotes,
                         "`default` declaration dropped; "
                         "all top-level targets build by default in thorn");
                noted_default = 1;
            }
        } else if (strncmp(s, "pool ", 5) == 0) {
            if (!noted_pool) {
                note_add(notes, nnotes,
                         "`pool` declaration dropped; "
                         "thorn does not model job concurrency pools");
                noted_pool = 1;
            }
        } else if (strncmp(s, "include ", 8) == 0 ||
                   strncmp(s, "subninja ", 9) == 0) {
            if (!noted_incl) {
                note_add(notes, nnotes,
                         "`include`/`subninja` statement skipped; "
                         "decompiler only parses the root graph file");
                noted_incl = 1;
            }
        } else if (strchr(s, '=')) {
            char *eq = strchr(s, '=');
            *eq = '\0';
            char *k = trim_inplace(s);
            char *v = trim_inplace(eq + 1);
            if (strcmp(k, "thorn_project") == 0) {
                snprintf(doc->proj, sizeof(doc->proj), "%s", v);
            } else if (strcmp(k, "cc") == 0 || strcmp(k, "ar") == 0) {
                if (!noted_cc) {
                    note_add(notes, nnotes,
                             "toolchain assignment (`%s = %s`) dropped; "
                             "configure via CC/AR environment in thorn",
                             k, v);
                    noted_cc = 1;
                }
            } else {
                if (!noted_dir) {
                    note_add(notes, nnotes,
                             "top-level ninja variable `%s = %s` skipped",
                             k, v);
                    noted_dir = 1;
                }
            }
        }
    }
}

const char *mvar_get(const MVars *v, const char *k)
{
    for (size_t i = 0; i < v->nvars; i++)
        if (strcmp(v->vk[i], k) == 0)
            return v->vv[i];
    return NULL;
}

void mvar_set(MVars *v, const char *k, const char *val)
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

void mvar_append(MVars *v, const char *k, const char *val)
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

MRule *mdoc_add_rule(MDoc *d)
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

void mrule_push_recipe(MRule *r, const char *line)
{
    char **nr = realloc(r->recipe, (r->nrec + 1) * sizeof(char *));
    if (!nr)
        return;
    r->recipe = nr;
    r->recipe[r->nrec++] = strdup(line);
}

void mdoc_free(MDoc *d)
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

char *str_expand(const char *in, const MVars *v)
{
    size_t cap = strlen(in) * 2 + 64;
    char *out = malloc(cap);
    if (!out)
        return strdup(in);
    size_t o = 0;
    for (size_t i = 0; in[i]; ) {
        if (in[i] == '$' && (in[i + 1] == '(' || in[i + 1] == '{')) {
            char close = in[i + 1] == '(' ? ')' : '}';
            const char *st = in + i + 2;
            const char *end = strchr(st, close);
            if (end) {
                char var[128];
                size_t vl = (size_t)(end - st);
                if (vl >= sizeof(var))
                    vl = sizeof(var) - 1;
                memcpy(var, st, vl);
                var[vl] = '\0';
                const char *val = mvar_get(v, var);
                if (val) {
                    size_t vall = strlen(val);
                    while (o + vall + 1 > cap) {
                        cap *= 2;
                        char *no = realloc(out, cap);
                        if (!no) {
                            free(out);
                            return strdup(in);
                        }
                        out = no;
                    }
                    memcpy(out + o, val, vall);
                    o += vall;
                }
                i = (size_t)(end - in + 1);
                continue;
            }
        }
        if (o + 2 > cap) {
            cap *= 2;
            char *no = realloc(out, cap);
            if (!no) {
                free(out);
                return strdup(in);
            }
            out = no;
        }
        out[o++] = in[i++];
    }
    out[o] = '\0';
    return out;
}

char *rule_recipe_text(const MRule *r, const MVars *v)
{
    size_t total = 0;
    for (size_t i = 0; i < r->nrec; i++)
        total += strlen(r->recipe[i]) + 1;
    char *buf = malloc(total + 1);
    if (!buf)
        return NULL;
    buf[0] = '\0';
    for (size_t i = 0; i < r->nrec; i++) {
        if (i)
            strcat(buf, " ");
        strcat(buf, r->recipe[i]);
    }
    char *exp = str_expand(buf, v);
    free(buf);
    return exp;
}

void preprocess_make_lines(char *str)
{
    char *r = str, *w = str;
    while (*r) {
        if (*r == '\\' && (*(r + 1) == '\n' || *(r + 1) == '\r')) {
            r++;
            if (*r == '\r' && *(r + 1) == '\n')
                r++;
            r++;
            while (*r == ' ' || *r == '\t')
                r++;
            *w++ = ' ';
            continue;
        }
        if (*r == '\\' && *(r + 1) == '#') {
            *w++ = '#';
            r += 2;
            continue;
        }
        *w++ = *r++;
    }
    *w = '\0';
}

void parse_make(char *mem, MDoc *doc, char ***notes, size_t *nnotes)
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

ObjInfo *objmap_find(ObjInfo *m, size_t n, const char *obj)
{
    for (size_t i = 0; i < n; i++)
        if (strcmp(m[i].obj, obj) == 0)
            return &m[i];
    return NULL;
}

void subst_make_to_cmd(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < n; ) {
        if (in[i] == '$' && in[i + 1] == '$') {
            out[o++] = '$';
            i += 2;
            continue;
        }
        if (in[i] == '$' && in[i + 1] == '@') {
            if (o + 4 < n) {
                memcpy(out + o, "$out", 4);
                o += 4;
            }
            i += 2;
            continue;
        }
        if (in[i] == '$' && in[i + 1] == '<') {
            if (o + 3 < n) {
                memcpy(out + o, "$in", 3);
                o += 3;
            }
            i += 2;
            continue;
        }
        if (in[i] == '$' && in[i + 1] == '^') {
            if (o + 3 < n) {
                memcpy(out + o, "$in", 3);
                o += 3;
            }
            i += 2;
            continue;
        }
        out[o++] = in[i++];
    }
    out[o] = '\0';
}

int sniff_is_ninja(const char *mem)
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
