/*
 * pkg.h - the .tpkg package scope and `thorn pkg` entry point.
 *
 * Part of thorn, the lean meta-build generator that embeds pith.
 * Copyright (c) 2026, abit-foggy
 */

#ifndef THORN_PKG_H
#define THORN_PKG_H

/* The local .tpkg package scope. */
#define PKG_SCOPE ".thorn/pkgs"

/* The project manifest: thorn.toml preferred, pith.toml accepted. */
const char *thorn_pkg_config(void);

/* True when a dependency value names a remote source (a git url or a
   user/repo shorthand) rather than a local path or an installed
   version. */
int thorn_pkg_source_is_remote(const char *src);

int thorn_pkg_main(int argc, char **argv);

#endif /* THORN_PKG_H */
