#include "include/emit.h"
#include "include/parser.h"

void apply_target_hooks(Target *t)
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

void map_ninja(const NDoc *doc, Graph *g, const char *pfx,
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
            if (strcmp(e->rule, "as") == 0 || strcmp(e->rule, "as_cpp") == 0)
                cls[i] = RC_COMPILE;
            else {
                cls[i] = RC_OTHER;
                note_add(notes, nnotes,
                         "edge `%s` references undefined rule `%s`; "
                         "dropped",
                         e->nout ? e->outs[0] : "?", e->rule);
            }
            continue;
        }
        if (strcmp(e->rule, "as") == 0 || strcmp(e->rule, "as_cpp") == 0)
            cls[i] = RC_COMPILE;
        else
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
        const char *asf = nedge_var(ce, "asflags");
        if (asf)
            harvest_asflags_text(asf, consumer);

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

void map_make(const MDoc *doc, Graph *g, const char *pfx,
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

        /* the source: a .c/.s/.S prerequisite, else the token after -c */
        const char *src = NULL;
        for (size_t k = 0; k < r->nprq; k++) {
            if (is_source_ext(r->prereqs[k])) {
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
                        is_source_ext(toks[t + 1])) {
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
            oi->src = swap_ext_source(tgt);
            if (!noted_heuristic) {
                noted_heuristic = 1;
                note_add(notes, nnotes,
                         "some objects had no visible source; guessed "
                         "by swapping .o extension");
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
                    char *src = swap_ext_source(strip_pfx(pr, pfx));
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
            } else if (is_source_ext(pr)) {
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
                            char *src = swap_ext_source(strip_pfx(tok, pfx));
                            if (src) {
                                strlist_push(&t->sources, src);
                                free(src);
                            }
                        }
                    } else if (is_source_ext(tok)) {
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
                                char *src = swap_ext_source(strip_pfx(tok, pfx));
                                if (src) {
                                    strlist_push(&t->sources, src);
                                    free(src);
                                }
                            }
                        } else if (is_source_ext(tok)) {
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

    const char *asflags = mvar_get(&doc->vars, "asflags-y");
    if (!asflags) asflags = mvar_get(&doc->vars, "EXTRA_AFLAGS");
    if (asflags && *asflags) {
        for (size_t i = 0; i < g->count; i++) {
            harvest_asflags_text(asflags, &g->targets[i]);
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
