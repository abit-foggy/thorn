/*
 * thorn_main.c - the thorn CLI.
 *
 * thorn embeds pith the way a game engine embeds a scripting
 * language: build.thorn is read and evaluated at runtime (no
 * per-project binaries, no AOT plugin objects), with the engine's
 * build API registered as the thorn_engine.* host namespace.
 *
 * This file is deliberately thin: parse the CLI, read the spec,
 * evaluate it with the emission epilogue appended, and relay the
 * script's exit code. The graph, the API, and the emitters all live
 * in src/thorn_engine.c, which doubles as the fallback link object
 * (it has no main() of its own, so the temp-executable child that
 * pith's engine builds on JIT-blocked hosts can link it cleanly).
 *
 * Identifier discipline: no identifier here may collide with a host
 * API stem (project, exe, static_lib, shared_lib, add_target,
 * add_source, add_cflag, add_ldflag, add_include, add_order_dep,
 * pkg_config, emit); this file is compiled WITHOUT the renames and
 * only references the engine through thorn_engine.h.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pith.h>
#include <pith_embed.h>

#include "include/thorn_engine.h"

/*
 * The engine's host API, referenced through its c_thorn_engine_*
 * link symbols (the author-aware mangling pith applies to imported C
 * units; the bootstrap compiles src/thorn_engine.c under the matching
 * -D renames). Declaring the mangled names keeps this file free of
 * the renames, and keeps the engine object self-contained for the
 * temp-executable fallback child (which links it without pith).
 */
extern int c_thorn_engine_project(PithValue *name);
extern int c_thorn_engine_exe(void);
extern int c_thorn_engine_static_lib(void);
extern int c_thorn_engine_shared_lib(void);
extern int c_thorn_engine_add_target(PithValue *name, int type);
extern int c_thorn_engine_add_source(PithValue *target, PithValue *src);
extern int c_thorn_engine_add_cflag(PithValue *target, PithValue *flag);
extern int c_thorn_engine_add_ldflag(PithValue *target, PithValue *flag);
extern int c_thorn_engine_add_include(PithValue *target, PithValue *dir);
extern int c_thorn_engine_add_order_dep(PithValue *target,
                                        PithValue *prereq);
extern int c_thorn_engine_pkg_config(PithValue *target, PithValue *pkg);
extern int c_thorn_engine_emit(void);

/* Register the full thorn_engine.* host namespace. */
int thorn_host_register(PithContext *ctx)
{
    static const struct {
        const char *name;
        void *fn;
        const char *params;   /* 'p' = PithValue*, 'w' = int */
    } api[] = {
        { "project",       c_thorn_engine_project,       "p"  },
        { "exe",           c_thorn_engine_exe,           ""   },
        { "static_lib",    c_thorn_engine_static_lib,    ""   },
        { "shared_lib",    c_thorn_engine_shared_lib,    ""   },
        { "add_target",    c_thorn_engine_add_target,    "pw" },
        { "add_source",    c_thorn_engine_add_source,   "pp" },
        { "add_cflag",     c_thorn_engine_add_cflag,     "pp" },
        { "add_ldflag",    c_thorn_engine_add_ldflag,    "pp" },
        { "add_include",   c_thorn_engine_add_include,   "pp" },
        { "add_order_dep", c_thorn_engine_add_order_dep, "pp" },
        { "pkg_config",    c_thorn_engine_pkg_config,    "pp" },
        { "emit",          c_thorn_engine_emit,          ""   },
    };
    for (size_t i = 0; i < sizeof(api) / sizeof(api[0]); i++)
        if (pith_register_ns_fn(ctx, "thorn_engine", api[i].name,
                                api[i].fn, 'w',
                                api[i].params) != 0)
            return -1;
    return 0;
}

/* The emission epilogue: once the spec finishes configuring, write
 * the backends (thorn_engine.emit() reads THORN_OUT_DIR and the
 * THORN_EMIT_* selection the CLI exported). A spec that exits early
 * with proc.exit() before finishing configuration skips emission,
 * which is the correct behavior for an aborted configure. */
static const char *EMISSION_EPILOGUE =
    "\nif thorn_engine.emit() == 0\n    proc.exit(1)\nend\n";

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

static void usage(void)
{
    printf("thorn " THORN_VERSION " - a lean meta-build generator "
           "that embeds pith\n\n"
           "USAGE:\n"
           "    thorn                       evaluate build.thorn, emit "
           "backends\n"
           "    thorn build [-f spec] [--out-dir dir] [--ninja|--make]\n"
           "    thorn decompile <build.ninja|Makefile> [-o out.thorn]\n"
           "    thorn help | version\n\n"
           "The specification (build.thorn, or thorn.pith) is a pith "
           "script\nevaluated at runtime; the engine API is the "
           "thorn_engine.* host\nnamespace. Backends are written to "
           "--out-dir (default: cwd):\n\n"
           "    thorn --out-dir out/artifacts\n"
           "    samu -f out/artifacts/build.ninja\n"
           "    make -f out/artifacts/Makefile\n\n"
           "CC/AR override the baked toolchain; THORN_ENGINE_OBJ "
           "overrides the\nfallback link object (default: "
           "<thorn-dir>/artifacts/thorn_engine.o).\n");
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

/* Resolve and register the fallback link object for the context. */
static void register_link_obj(PithContext *ctx, const char *argv0)
{
    const char *obj = getenv("THORN_ENGINE_OBJ");
    if (obj && *obj) {
        pith_register_link_object(ctx, obj);
        return;
    }
    char path[4096];
    snprintf(path, sizeof(path), "%s",
             argv0 && *argv0 ? argv0 : "./thorn");
    char *slash = strrchr(path, '/');
    if (slash)
        *slash = '\0';
    else
        snprintf(path, sizeof(path), ".");
    char cand[4096];

    /* the bootstrap layout: <thorn>/out/artifacts/thorn_engine.o */
    snprintf(cand, sizeof(cand), "%s/artifacts/thorn_engine.o", path);
    if (access(cand, R_OK) == 0) {
        pith_register_link_object(ctx, cand);
        return;
    }
    /* the self-built layout: thorn_core.a beside the binary carries
     * the engine member (archives resolve members on demand) */
    snprintf(cand, sizeof(cand), "%s/thorn_core.a", path);
    if (access(cand, R_OK) == 0) {
        pith_register_link_object(ctx, cand);
        return;
    }
    /* flat beside the binary */
    snprintf(cand, sizeof(cand), "%s/thorn_engine.o", path);
    pith_register_link_object(ctx, cand);
}

#ifndef THORN_RUNTIME_DEFAULT
#define THORN_RUNTIME_DEFAULT ""
#endif

/*
 * The temp-executable fallback links pith's runtime archive. Locate
 * it: an explicit PITH_RUNTIME wins, then the path baked at bootstrap
 * time, then the dev layout (pith checked out beside the thorn
 * tree, which covers out/thorn, out/artifacts/thorn, and running
 * from a project subdirectory alike).
 */
static void configure_runtime_env(const char *argv0)
{
    const char *env = getenv("PITH_RUNTIME");
    if (env && *env)
        return;
    if (THORN_RUNTIME_DEFAULT[0] &&
        access(THORN_RUNTIME_DEFAULT, F_OK) == 0) {
        setenv("PITH_RUNTIME", THORN_RUNTIME_DEFAULT, 1);
        return;
    }
    char path[4096];
    snprintf(path, sizeof(path), "%s",
             argv0 && *argv0 ? argv0 : "thorn");
    char *slash = strrchr(path, '/');
    if (slash)
        *slash = '\0';
    else
        snprintf(path, sizeof(path), ".");
    char cand[4096];
    snprintf(cand, sizeof(cand), "%s/../../pith/runtime/libruntime.a",
             path);
    if (access(cand, F_OK) == 0)
        setenv("PITH_RUNTIME", cand, 1);
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
        const char *spec_path = NULL;
        const char *out_dir = ".";
        int want_ninja = 1, want_make = 1;

        for (int i = argbase; i < argc; i++) {
            if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
                spec_path = argv[++i];
            } else if (strcmp(argv[i], "--out-dir") == 0 &&
                       i + 1 < argc) {
                out_dir = argv[++i];
            } else if (strcmp(argv[i], "--ninja") == 0) {
                want_make = 0;
            } else if (strcmp(argv[i], "--make") == 0) {
                want_ninja = 0;
            } else {
                thorn_diag("unknown flag `%s`", argv[i]);
                usage();
                return 2;
            }
        }
        if (!spec_path) {
            if (access("build.thorn", R_OK) == 0)
                spec_path = "build.thorn";
            else if (access("thorn.pith", R_OK) == 0)
                spec_path = "thorn.pith";
            else {
                thorn_diag("no build.thorn or thorn.pith in the "
                           "current directory (pass -f <spec>)");
                return 1;
            }
        }

        PithContext *ctx = pith_context_new();
        if (!ctx) {
            thorn_diag("cannot create the pith context");
            return 1;
        }
        if (thorn_host_register(ctx) != 0) {
            thorn_diag("cannot register the thorn_engine namespace");
            pith_context_free(ctx);
            return 1;
        }
        configure_runtime_env(argc > 0 ? argv[0] : "thorn");
        register_link_obj(ctx, argc > 0 ? argv[0] : "thorn");

        char *src = read_all(spec_path);
        if (!src) {
            thorn_diag("cannot read the specification %s", spec_path);
            pith_context_free(ctx);
            return 1;
        }
        size_t sl = strlen(src), el = strlen(EMISSION_EPILOGUE);
        char *full = malloc(sl + el + 1);
        if (!full) {
            thorn_diag("out of memory");
            free(src);
            pith_context_free(ctx);
            return 1;
        }
        memcpy(full, src, sl);
        memcpy(full + sl, EMISSION_EPILOGUE, el + 1);
        free(src);

        if (setenv("THORN_OUT_DIR", out_dir, 1) != 0 ||
            setenv("THORN_EMIT_NINJA", want_ninja ? "1" : "0", 1) != 0 ||
            setenv("THORN_EMIT_MAKE", want_make ? "1" : "0", 1) != 0) {
            thorn_diag("cannot export the emission environment");
            free(full);
            pith_context_free(ctx);
            return 1;
        }

        int rc = pith_eval_string(ctx, full);
        pith_context_free(ctx);
        free(full);
        return rc == 0 ? 0 : 1;
    }

    if (strcmp(verb, "decompile") == 0) {
        const char *in = NULL;
        const char *out = "build.thorn";
        for (int i = argbase; i < argc; i++) {
            if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
                out = argv[++i];
            else if (!in)
                in = argv[i];
            else {
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
