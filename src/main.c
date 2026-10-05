#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pith.h>
#include <pith_embed.h>

#include "include/engine.h"

/* Engine host API symbols */
extern int c_engine_project(PithValue *name);
extern int c_engine_backend(PithValue *name);
extern int c_engine_exe(void);
extern int c_engine_static_lib(void);
extern int c_engine_shared_lib(void);
extern int c_engine_add_target(PithValue *name, int type);
extern int c_engine_add_source(PithValue *target, PithValue *src);
extern int c_engine_add_cflag(PithValue *target, PithValue *flag);
extern int c_engine_add_ldflag(PithValue *target, PithValue *flag);
extern int c_engine_add_include(PithValue *target, PithValue *dir);
extern int c_engine_add_order_dep(PithValue *target, PithValue *prereq);
extern int c_engine_add_command(PithValue *output, PithValue *command,
                                PithValue *input);
extern int c_engine_pkg_config(PithValue *target, PithValue *pkg);
extern int c_engine_emit(void);

/* Register engine.* host namespace */
int host_register(PithContext *ctx)
{
    static const struct {
        const char *name;
        void *fn;
        const char *params;   /* 'p' = PithValue*, 'w' = int */
    } api[] = {
        { "project",       c_engine_project,       "p"   },
        { "backend",       c_engine_backend,       "p"   },
        { "exe",           c_engine_exe,           ""    },
        { "static_lib",    c_engine_static_lib,    ""    },
        { "shared_lib",    c_engine_shared_lib,    ""    },
        { "add_target",    c_engine_add_target,    "pw"  },
        { "add_source",    c_engine_add_source,    "pp"  },
        { "add_cflag",     c_engine_add_cflag,     "pp"  },
        { "add_ldflag",    c_engine_add_ldflag,    "pp"  },
        { "add_include",   c_engine_add_include,   "pp"  },
        { "add_order_dep", c_engine_add_order_dep, "pp"  },
        { "add_command",   c_engine_add_command,   "ppp" },
        { "pkg_config",    c_engine_pkg_config,    "pp"  },
        { "emit",          c_engine_emit,          ""    },
    };
    for (size_t i = 0; i < sizeof(api) / sizeof(api[0]); i++)
        if (pith_register_ns_fn(ctx, "engine", api[i].name,
                                api[i].fn, 'w',
                                api[i].params) != 0)
            return -1;
    return 0;
}

/* Emission epilogue appended to spec */
static const char *EMISSION_EPILOGUE =
    "\nif engine.emit() == 0\n    proc.exit(1)\nend\n";

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
           "    thorn build [-f spec] [--out-dir dir] [--engine ninja|make|both]\n"
           "                [--compiler cc] [--ar ar]\n"
           "    thorn decompile <build.ninja|Makefile> [-o out.thorn]\n"
           "    thorn help | version\n\n"
           "The specification (build.thorn, or thorn.pith) is a pith "
           "script\nevaluated at runtime; the engine API is the "
           "engine.* host\nnamespace. Backends are written to "
           "--out-dir (default: cwd):\n\n"
           "    thorn --out-dir out/artifacts\n"
           "    samu -f out/artifacts/build.ninja\n"
           "    make -f out/artifacts/Makefile\n\n"
           "Toolchain selection:\n"
           "    --compiler <clang|gcc|cc|tcc|...>\n"
           "    --ar <ar|llvm-ar|...>\n"
           "Precedence: flag > CC/AR env > cc/ar (validated in PATH).\n\n"
           "Engine selection:\n"
           "    engine.backend(\"ninja\") in spec, or --engine <ninja|make|both>\n"
           "(flag overrides spec).\n");
}

static int validate_executable(const char *name)
{
    if (!name || !*name)
        return 0;
    if (strchr(name, '/'))
        return access(name, X_OK) == 0;
    const char *path_env = getenv("PATH");
    if (!path_env)
        path_env = "/usr/bin:/bin";
    char *copy = strdup(path_env);
    if (!copy)
        return 0;
    char *saveptr = NULL;
    char *dir = strtok_r(copy, ":", &saveptr);
    int found = 0;
    while (dir) {
        char cand[4096];
        snprintf(cand, sizeof(cand), "%s/%s", dir, name);
        if (access(cand, X_OK) == 0) {
            found = 1;
            break;
        }
        dir = strtok_r(NULL, ":", &saveptr);
    }
    free(copy);
    return found;
}

static int cmd_decompile(const char *in, const char *out)
{
    Graph g;
    graph_init(&g);
    char **notes = NULL;
    size_t nnotes = 0;

    if (decompile_file(in, &g, &notes, &nnotes) != 0) {
        graph_free(&g);
        return 1;
    }
    if (g.count == 0 && g.cmd_count == 0) {
        diag("no buildable targets or commands found in %s", in);
        graph_free(&g);
        return 1;
    }

    FILE *f = fopen(out, "w");
    if (!f) {
        diag("cannot write %s", out);
        graph_free(&g);
        return 1;
    }
    print_spec(&g, f, (const char **)notes, nnotes);
    fclose(f);

    if (g.cmd_count > 0) {
        printf("thorn: decompiled %s into %s (%zu target%s, %zu command%s, %zu note%s)\n",
               in, out, g.count, g.count == 1 ? "" : "s",
               g.cmd_count, g.cmd_count == 1 ? "" : "s",
               nnotes, nnotes == 1 ? "" : "s");
    } else {
        printf("thorn: decompiled %s into %s (%zu target%s, %zu note%s)\n",
               in, out, g.count, g.count == 1 ? "" : "s",
               nnotes, nnotes == 1 ? "" : "s");
    }

    for (size_t i = 0; i < nnotes; i++)
        free(notes[i]);
    free(notes);
    graph_free(&g);
    return 0;
}

/* Resolve and register the fallback link object for the context. */
static void register_link_obj(PithContext *ctx, const char *argv0)
{
    const char *obj = getenv("THORN_LINK_OBJ");
    if (!obj || !*obj)
        obj = getenv("THORN_ENGINE_OBJ");
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
    char cand[8192];

    /* Check out/artifacts/engine.o */
    snprintf(cand, sizeof(cand), "%s/artifacts/engine.o", path);
    if (access(cand, R_OK) == 0) {
        pith_register_link_object(ctx, cand);
        return;
    }
    /* Check thorn_core.a beside binary */
    snprintf(cand, sizeof(cand), "%s/thorn_core.a", path);
    if (access(cand, R_OK) == 0) {
        pith_register_link_object(ctx, cand);
        return;
    }
    /* Check engine.o beside binary */
    snprintf(cand, sizeof(cand), "%s/engine.o", path);
    pith_register_link_object(ctx, cand);
}

#ifndef THORN_RUNTIME_DEFAULT
#define THORN_RUNTIME_DEFAULT ""
#endif

/* Locate and export PITH_RUNTIME if needed */
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
    char cand[8192];
    snprintf(cand, sizeof(cand), "%s/../vendor/pith/runtime/libruntime.a",
             path);
    if (access(cand, F_OK) == 0) {
        setenv("PITH_RUNTIME", cand, 1);
        return;
    }
    snprintf(cand, sizeof(cand), "%s/../../pith/runtime/libruntime.a",
             path);
    if (access(cand, F_OK) == 0)
        setenv("PITH_RUNTIME", cand, 1);
}

int main(int argc, char **argv)
{
    int argbase = 1;
    const char *verb = "build";
    if (argc >= 2 && argv[1][0] != '-') {
        verb = argv[1];
        argbase = 2;
    }

    if (strcmp(verb, "build") == 0) {
        const char *spec_path = NULL;
        const char *out_dir = ".";
        const char *cli_engine = NULL;
        const char *cli_compiler = NULL;
        const char *cli_ar = NULL;

        for (int i = argbase; i < argc; i++) {
            if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
                spec_path = argv[++i];
            } else if (strcmp(argv[i], "--out-dir") == 0 && i + 1 < argc) {
                out_dir = argv[++i];
            } else if (strcmp(argv[i], "--engine") == 0 && i + 1 < argc) {
                cli_engine = argv[++i];
            } else if (strcmp(argv[i], "--ninja") == 0) {
                cli_engine = "ninja";
            } else if (strcmp(argv[i], "--make") == 0) {
                cli_engine = "make";
            } else if (strcmp(argv[i], "--compiler") == 0 && i + 1 < argc) {
                cli_compiler = argv[++i];
            } else if (strcmp(argv[i], "--ar") == 0 && i + 1 < argc) {
                cli_ar = argv[++i];
            } else {
                diag("unknown flag `%s`", argv[i]);
                usage();
                return 2;
            }
        }

        if (cli_engine) {
            if (strcmp(cli_engine, "ninja") != 0 &&
                strcmp(cli_engine, "make") != 0 &&
                strcmp(cli_engine, "both") != 0) {
                diag("unknown engine `%s` (expected ninja, make, or both)",
                     cli_engine);
                return 2;
            }
        }

        /* Compiler resolution: flag > CC env > cc */
        const char *cc = cli_compiler;
        if (!cc || !*cc)
            cc = getenv("CC");
        if (!cc || !*cc)
            cc = "cc";
        if (!validate_executable(cc)) {
            diag("compiler `%s` not found in PATH or not executable", cc);
            return 1;
        }
        setenv("CC", cc, 1);

        /* Ar resolution: flag > AR env > ar */
        const char *ar = cli_ar;
        if (!ar || !*ar)
            ar = getenv("AR");
        if (!ar || !*ar)
            ar = "ar";
        if (!validate_executable(ar)) {
            diag("archiver `%s` not found in PATH or not executable", ar);
            return 1;
        }
        setenv("AR", ar, 1);

        if (!spec_path) {
            if (access("build.thorn", R_OK) == 0)
                spec_path = "build.thorn";
            else if (access("thorn.pith", R_OK) == 0)
                spec_path = "thorn.pith";
            else {
                diag("no build.thorn or thorn.pith in the current directory (pass -f <spec>)");
                return 1;
            }
        }

        PithContext *ctx = pith_context_new();
        if (!ctx) {
            diag("cannot create the pith context");
            return 1;
        }
        if (host_register(ctx) != 0) {
            diag("cannot register the engine namespace");
            pith_context_free(ctx);
            return 1;
        }
        configure_runtime_env(argc > 0 ? argv[0] : "thorn");
        register_link_obj(ctx, argc > 0 ? argv[0] : "thorn");

        char *src = read_all(spec_path);
        if (!src) {
            diag("cannot read the specification %s", spec_path);
            pith_context_free(ctx);
            return 1;
        }
        size_t sl = strlen(src), el = strlen(EMISSION_EPILOGUE);
        char *full = malloc(sl + el + 1);
        if (!full) {
            diag("out of memory");
            free(src);
            pith_context_free(ctx);
            return 1;
        }
        memcpy(full, src, sl);
        memcpy(full + sl, EMISSION_EPILOGUE, el + 1);
        free(src);

        if (setenv("THORN_OUT_DIR", out_dir, 1) != 0) {
            diag("cannot export the output directory");
            free(full);
            pith_context_free(ctx);
            return 1;
        }
        if (cli_engine)
            setenv("THORN_ENGINE", cli_engine, 1);
        else
            unsetenv("THORN_ENGINE");

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
                diag("unexpected argument `%s`", argv[i]);
                usage();
                return 2;
            }
        }
        if (!in) {
            diag("decompile expects an input file");
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

    diag("unknown command `%s`", verb);
    usage();
    return 2;
}
