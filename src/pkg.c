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

/* The project manifest: thorn.toml preferred, pith.toml accepted. */
const char *
thorn_pkg_config(void)
{
    static char path[512];
    if (!path[0]) {
        if (access("thorn.toml", R_OK) == 0)
            snprintf(path, sizeof(path), "thorn.toml");
        else
            snprintf(path, sizeof(path), "pith.toml");
    }
    return path;
}

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

/* ------------------------------------------------------------------ */
/* Remote sources: github releases first, git source fallback          */
/* ------------------------------------------------------------------ */

/* curl, then wget.  Returns 0 when dest exists and is readable. */
static int
http_get(const char *url, const char *dest)
{
    char cmd[4200];

    snprintf(cmd, sizeof(cmd),
             "curl -fsSL --retry 2 -o '%s' '%s' 2>/dev/null", dest, url);
    if (system(cmd) == 0 && access(dest, R_OK) == 0)
        return 0;
    snprintf(cmd, sizeof(cmd),
             "wget -q -O '%s' '%s' 2>/dev/null", dest, url);
    if (system(cmd) == 0 && access(dest, R_OK) == 0)
        return 0;
    return -1;
}

/* Read a JSON string field without mutating the buffer.  0 = found. */
static int
json_field(const char *mem, const char *needle, char *out, size_t n)
{
    const char *hit = strstr(mem, needle);
    if (!hit)
        return 1;
    hit += strlen(needle);
    while (*hit == ' ' || *hit == ':')
        hit++;
    if (*hit != '"')
        return 1;
    hit++;
    const char *end = strchr(hit, '"');
    if (!end)
        return 1;
    size_t len = (size_t)(end - hit);
    if (len >= n)
        len = n - 1;
    memcpy(out, hit, len);
    out[len] = '\0';
    return 0;
}

/* Probe the latest GitHub release for a .tpkg asset.  0 = found,
   1 = no asset, -1 = unreachable. */
static int
gh_release_probe(const char *user, const char *repo, char *asset_url,
                 size_t an, char *tag, size_t tn)
{
    char api[512];
    snprintf(api, sizeof(api),
             "https://api.github.com/repos/%s/%s/releases/latest",
             user, repo);
    char tmp[4096];
    if (pith_make_temp(".json", tmp, sizeof(tmp)) != 0)
        return -1;
    if (http_get(api, tmp) != 0) {
        unlink(tmp);
        return -1;
    }
    long sz = 0;
    char *mem = read_bundle(tmp, &sz);
    unlink(tmp);
    if (!mem)
        return -1;

    char url_try[1024];
    char *url = NULL;
    char *scan = mem;
    while ((scan = strstr(scan, "\"browser_download_url\"")) != NULL) {
        if (json_field(scan, "\"browser_download_url\"", url_try,
                       sizeof(url_try)) == 0 &&
            strstr(url_try, ".tpkg")) {
            url = url_try;
            break;
        }
        scan += strlen("\"browser_download_url\"");
    }
    char tag_try[128];
    int tgr = json_field(mem, "\"tag_name\"", tag_try, sizeof(tag_try));
    if (!url || tgr != 0) {
        free(mem);
        return 1;
    }
    snprintf(asset_url, an, "%s", url);
    snprintf(tag, tn, "%s", tag_try);
    free(mem);
    return 0;
}

/* Split "user/repo", "github.com/user/repo" or a github URL.  0 when
   it is a github source. */
static int
gh_split(const char *src, char *user, size_t un, char *repo, size_t rn)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", src);

    const char *ur;
    if (strncmp(buf, "git@github.com:", 15) == 0)
        ur = buf + 15;
    else if (strstr(buf, "github.com/"))
        ur = strstr(buf, "github.com/") + 11;
    else if (strchr(buf, '/') && !strchr(buf, ':') &&
             !strstr(buf, "://"))
        ur = buf;
    else
        return 1;

    char repo_full[512];
    snprintf(repo_full, sizeof(repo_full), "%.*s", (int)(sizeof(repo_full) - 1), ur);
    char *cut = strstr(repo_full, ".git");
    if (cut)
        *cut = '\0';
    cut = strstr(repo_full, "/tree/");
    if (cut)
        *cut = '\0';

    char *slash = strchr(repo_full, '/');
    if (!slash || slash == repo_full || !slash[1])
        return 1;
    *slash = '\0';
    if (strlen(repo_full) >= un || strlen(slash + 1) >= rn)
        return 1;
    snprintf(user, un, "%s", repo_full);
    snprintf(repo, rn, "%s", slash + 1);
    return 0;
}

/* Clone, build with make, and hunt for a produced .tpkg. */
static int
install_from_source(const char *name, const char *url, const char *vc)
{
    char srcdir[4096], cmd[8192];

    snprintf(srcdir, sizeof(srcdir), "%s/src/%s", PKG_SCOPE, name);
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", srcdir);
    if (system(cmd) != 0)
        return 1;
    snprintf(cmd, sizeof(cmd),
             "git clone --depth 1 '%s' '%s' 2>/dev/null", url, srcdir);
    if (system(cmd) != 0) {
        diag("pkg: cannot clone '%s'", url);
        return 1;
    }
    diag("pkg: building from source in %s", srcdir);
    snprintf(cmd, sizeof(cmd), "make -C '%s' >/dev/null 2>&1", srcdir);
    if (system(cmd) != 0) {
        diag("pkg: the source build failed for '%s'", name);
        return 1;
    }

    /* hunt for a produced .tpkg: dist/ first, then the root */
    char found[4096] = "";
    static const char *const dirs[] = { "/dist", "", NULL };
    for (size_t i = 0; dirs[i] && !found[0]; i++) {
        char dir[4200];
        snprintf(dir, sizeof(dir), "%s%s", srcdir, dirs[i]);
        DIR *d = opendir(dir);
        if (!d)
            continue;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (ends_with(e->d_name, ".tpkg")) {
                size_t dl = strlen(dir), nl = strlen(e->d_name);
                if (dl + 1 + nl < sizeof(found)) {
                    memcpy(found, dir, dl);
                    found[dl] = '/';
                    memcpy(found + dl + 1, e->d_name, nl + 1);
                }
                break;
            }
        }
        closedir(d);
    }
    if (!found[0]) {
        diag("pkg: the source build produced no .tpkg for '%s'", name);
        return 1;
    }

    char dest[4096];
    snprintf(dest, sizeof(dest), "%s/%s@%s", PKG_SCOPE, name, vc);
    return unpack_into(found, dest);
}

/* Install from a git-ish source: github releases first, then source. */
static int
tpkg_install_remote(const char *name, const char *src)
{
    /* a direct .tpkg url: download and unpack */
    if (ends_with(src, ".tpkg")) {
        char tmp[4096];
        if (pith_make_temp(".tpkg", tmp, sizeof(tmp)) != 0)
            return 1;
        if (http_get(src, tmp) != 0) {
            diag("pkg: cannot download '%s'", src);
            unlink(tmp);
            return 1;
        }
        char vc[PKG_MAX_NAME];
        mangled_ver(src, vc, sizeof(vc));
        char dest[4096];
        snprintf(dest, sizeof(dest), "%s/%s@%s", PKG_SCOPE, name, vc);
        int rc = unpack_into(tmp, dest);
        unlink(tmp);
        if (rc == 0)
            diag("pkg: installed %s from the url", name);
        return rc;
    }

    char user[128], repo[192];
    if (gh_split(src, user, sizeof(user), repo, sizeof(repo)) == 0) {
        char asset[1024], tag[128];
        int pr = gh_release_probe(user, repo, asset, sizeof(asset),
                                  tag, sizeof(tag));
        if (pr == 0) {
            char tmp[4096];
            if (pith_make_temp(".tpkg", tmp, sizeof(tmp)) != 0)
                return 1;
            if (http_get(asset, tmp) != 0) {
                diag("pkg: cannot download '%s'", asset);
                unlink(tmp);
                return 1;
            }
            char dest[4096];
            snprintf(dest, sizeof(dest), "%s/%s@%s", PKG_SCOPE, name,
                     tag);
            int rc = unpack_into(tmp, dest);
            unlink(tmp);
            if (rc == 0)
                diag("pkg: installed %s@%s from the release asset",
                     name, tag);
            return rc;
        }
        if (pr < 0)
            diag("pkg: github unreachable for %s/%s; trying source",
                 user, repo);
        else
            diag("pkg: no .tpkg release asset for %s/%s; trying source",
                 user, repo);
        char url[512];
        snprintf(url, sizeof(url), "https://github.com/%s/%s.git",
                 user, repo);
        return install_from_source(name, url, "src");
    }

    /* any other git url: clone and build from source */
    return install_from_source(name, src, "src");
}

int
thorn_pkg_source_is_remote(const char *src)
{
    if (strstr(src, "://") || strncmp(src, "git@", 4) == 0)
        return 1;
    /* user/repo shorthand: one slash, no scheme, not a local path */
    const char *slash = strchr(src, '/');
    if (slash && !strchr(slash + 1, '/') && src[0] != '.' &&
        src[0] != '/' && access(src, F_OK) != 0)
        return 1;
    return 0;
}

static int
tpkg_install_one(const char *name, const char *src)
{
    if (thorn_pkg_source_is_remote(src))
        return tpkg_install_remote(name, src);
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
    if (pith_config_load(thorn_pkg_config(), &cfg) != 0) {
        diag("pkg: no thorn.toml or pith.toml in the current directory");
        return 1;
    }
    int fail = 0;
    for (size_t i = 0; i < cfg.count; i++) {
        const char *key = cfg.entries[i].key;
        if (strncmp(key, "deps.", 5) != 0 &&
            strncmp(key, "plugins.", 8) != 0)
            continue;
        const char *nkey = strncmp(key, "plugins.", 8) == 0
                               ? key + 8 : key + 5;
        if (tpkg_install_one(nkey, cfg.entries[i].value) != 0)
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
            /* a remote source installs directly: the plugin name is
               the repo (or the bundle file) name; otherwise look the
               name up in the manifest */
            if (thorn_pkg_source_is_remote(argv[1])) {
                char rname[PKG_MAX_NAME], user[128], repo[192];
                if (ends_with(argv[1], ".tpkg")) {
                    const char *b = strrchr(argv[1], '/');
                    b = b ? b + 1 : argv[1];
                    snprintf(rname, sizeof(rname), "%.*s",
                             (int)(strlen(b) - 5), b);
                } else if (gh_split(argv[1], user, sizeof(user), repo,
                                    sizeof(repo)) == 0) {
                    snprintf(rname, sizeof(rname), "%s", repo);
                } else {
                    snprintf(rname, sizeof(rname), "%s", argv[1]);
                }
                return tpkg_install_one(rname, argv[1]);
            }
            PithConfig cfg;
            if (pith_config_load(thorn_pkg_config(), &cfg) != 0) {
                diag("pkg: no thorn.toml or pith.toml in the current "
                     "directory");
                return 1;
            }
            for (size_t i = 0; i < cfg.count; i++) {
                const char *key = cfg.entries[i].key;
                const char *nkey = strncmp(key, "plugins.", 8) == 0
                                       ? key + 8 : key + 5;
                if (strncmp(key, "deps.", 5) != 0 &&
                    strncmp(key, "plugins.", 8) != 0)
                    continue;
                if (strcmp(nkey, argv[1]) == 0)
                    return tpkg_install_one(nkey,
                                            cfg.entries[i].value);
            }
            diag("pkg: '%s' is not declared in the manifest", argv[1]);
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
