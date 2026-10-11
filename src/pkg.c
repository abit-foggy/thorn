/*
 * pkg.c - the .tpkg package scope and the `thorn pkg` command.
 *
 * A .tpkg bundle is a tar archive containing plugin.o and a manifest.
 * Installed packages live in .thorn/pkgs/<name>@<version>/ where the
 * plugin loader finds their plugin.o at spec-evaluation time.
 *
 * Part of thorn, the lean meta-build generator that embeds pith.
 * Copyright (c) 2026, abit-foggy
 */

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pith.h>
#include <compiler.h>

#include "include/engine.h"
#include "include/pkg.h"


#define PKG_MAX_NAME 256

static int
ends_with(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    return ls >= lx && strcmp(s + ls - lx, suffix) == 0;
}

/* pith's dep values mangle path separators into the version slot. */
static void
mangled_ver(const char *depval, char *out, size_t n)
{
    snprintf(out, n, "%s", depval);
    for (char *c = out; *c; c++)
        if (*c == '/' || *c == '\\')
            *c = '_';
}

static char *
read_bundle(const char *path, long *out_sz)
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
    char *mem = malloc((size_t)sz + 1);
    if (!mem || fread(mem, 1, (size_t)sz, fp) != (size_t)sz) {
        free(mem);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *out_sz = sz;
    return mem;
}

static int
copy_file(const char *src, const char *dest)
{
    long sz = 0;
    char *mem = read_bundle(src, &sz);
    if (!mem)
        return -1;
    FILE *fp = fopen(dest, "wb");
    if (!fp) {
        free(mem);
        return -1;
    }
    size_t rc = fwrite(mem, 1, (size_t)sz, fp);
    fclose(fp);
    free(mem);
    return rc == (size_t)sz ? 0 : -1;
}

static int
unpack_into(const char *bundle, const char *dest)
{
    long sz = 0;
    char *mem = read_bundle(bundle, &sz);
    if (!mem) {
        diag("pkg: cannot read bundle '%s'", bundle);
        return 1;
    }
    if (makedirs(dest) != 0) {
        free(mem);
        return 1;
    }
    int rc = pith_tar_extract_mem(mem, (size_t)sz, dest);
    free(mem);
    if (rc != 0) {
        diag("pkg: cannot unpack '%s'", bundle);
        return 1;
    }
    return 0;
}

static int
scope_has_plugin(const char *name, const char *vc, char *obj, size_t n)
{
    snprintf(obj, n, "%s/%s@%s/plugin.o", PKG_SCOPE, name, vc);
    return access(obj, R_OK) == 0;
}

static int
tpkg_install_one(const char *name, const char *src)
{
    char vc[PKG_MAX_NAME];
    mangled_ver(src, vc, sizeof(vc));
    char obj[4096];
    if (scope_has_plugin(name, vc, obj, sizeof(obj))) {
        diag("pkg: %s@%s already installed", name, vc);
        return 0;
    }
    char dest[4096];
    snprintf(dest, sizeof(dest), "%s/%s@%s", PKG_SCOPE, name, vc);

    if (strchr(src, '/') || strchr(src, '\\')) {
        if (ends_with(src, ".tpkg") || ends_with(src, ".tar"))
            return unpack_into(src, dest);
        char cand[4200];
        snprintf(cand, sizeof(cand), "%s/plugin.o", src);
        if (access(cand, R_OK) != 0) {
            diag("pkg: no plugin.o in '%s'", src);
            return 1;
        }
        if (makedirs(dest) != 0)
            return 1;
        if (copy_file(cand, obj) != 0) {
            diag("pkg: cannot stage '%s'", cand);
            return 1;
        }
    } else {
        const char *reg = getenv("THORN_REGISTRY");
        if (!reg || !*reg) {
            diag("pkg: '%s' not installed and THORN_REGISTRY is unset",
                 name);
            return 1;
        }
        char cand[4200];
        snprintf(cand, sizeof(cand), "%s/%s-%s.tpkg", reg, name, src);
        if (access(cand, R_OK) != 0)
            snprintf(cand, sizeof(cand), "%s/%s-%s.tar", reg, name, src);
        if (access(cand, R_OK) != 0) {
            diag("pkg: '%s' not found in the registry", cand);
            return 1;
        }
        if (unpack_into(cand, dest) != 0)
            return 1;
    }
    diag("pkg: installed %s@%s", name, vc);
    return 0;
}

static int
tpkg_sync(void)
{
    PithConfig cfg;
    if (pith_config_load("pith.toml", &cfg) != 0) {
        diag("pkg: no pith.toml in the current directory");
        return 1;
    }
    int fail = 0;
    for (size_t i = 0; i < cfg.count; i++) {
        if (strncmp(cfg.entries[i].key, "deps.", 5) != 0)
            continue;
        if (tpkg_install_one(cfg.entries[i].key + 5,
                            cfg.entries[i].value) != 0)
            fail = 1;
    }
    return fail;
}

static int
tpkg_add(const char *bundle)
{
    char base[PKG_MAX_NAME];
    const char *b = strrchr(bundle, '/');
    snprintf(base, sizeof(base), "%s", b ? b + 1 : bundle);
    if (ends_with(base, ".tpkg") || ends_with(base, ".tar"))
        base[strlen(base) - 5] = '\0';

    /* <name>-<version>.tpkg splits at the last dash; bare names
       install as <name>@local */
    char name[PKG_MAX_NAME], ver[PKG_MAX_NAME];
    char *dash = strrchr(base, '-');
    if (dash && dash != base) {
        *dash = '\0';
        snprintf(name, sizeof(name), "%s", base);
        snprintf(ver, sizeof(ver), "%s", dash + 1);
    } else {
        snprintf(name, sizeof(name), "%s", base);
        snprintf(ver, sizeof(ver), "local");
    }
    return tpkg_install_one(name, ver[0] ? bundle : bundle);
}

static int
tpkg_list(void)
{
    DIR *d = opendir(PKG_SCOPE);
    if (!d) {
        printf("no packages installed\n");
        return 0;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char obj[4096];
        snprintf(obj, sizeof(obj), "%s/%s/plugin.o", PKG_SCOPE, e->d_name);
        if (e->d_name[0] != '.' && access(obj, R_OK) == 0)
            printf("%s\n", e->d_name);
    }
    closedir(d);
    return 0;
}

int
thorn_pkg_main(int argc, char **argv)
{
    if (argc >= 1 && strcmp(argv[0], "install") == 0) {
        if (argc >= 3)
            return tpkg_install_one(argv[1], argv[2]);
        if (argc == 2) {
            PithConfig cfg;
            if (pith_config_load("pith.toml", &cfg) != 0) {
                diag("pkg: no pith.toml in the current directory");
                return 1;
            }
            for (size_t i = 0; i < cfg.count; i++) {
                if (strncmp(cfg.entries[i].key, "deps.", 5) != 0)
                    continue;
                if (strcmp(cfg.entries[i].key + 5, argv[1]) == 0)
                    return tpkg_install_one(argv[1],
                                           cfg.entries[i].value);
            }
            diag("pkg: '%s' is not declared in pith.toml", argv[1]);
            return 1;
        }
        return tpkg_sync();
    }
    if (argc >= 1 && strcmp(argv[0], "sync") == 0)
        return tpkg_sync();
    if (argc >= 1 && strcmp(argv[0], "add") == 0) {
        if (argc < 2) {
            diag("pkg add expects a .tpkg path");
            return 2;
        }
        return tpkg_add(argv[1]);
    }
    if (argc >= 1 && strcmp(argv[0], "list") == 0)
        return tpkg_list();

    diag("usage: thorn pkg [install [name <src>]] | [sync] | "
         "[add <bundle>] | [list]");
    return 2;
}
