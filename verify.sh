#!/bin/sh
# verify.sh - the thorn acceptance suite.
#
# Proves the full mission contract:
#   stage 1: pith compiles build.thorn -> plugin object -> link with the
#            engine and pith's runtime -> ./thorn
#   stage 2: thorn emits its own build.ninja, samu rebuilds thorn with it
#   demo:    a small multi-file C project configured in pith, built by
#            both samu (ninja) and make, with identical program output
#   reverse: thorn decompile ingests build.ninja and the Makefile into
#            idiomatic build.thorn specs, and the decompiled spec
#            regenerates a byte-identical build.ninja (full roundtrip)
#
# Prerequisite: pith built at ../pith (see the Makefile header).

set -e

ROOT=$(cd "$(dirname "$0")" && pwd)
PITH_ROOT=$(cd "$ROOT/../pith" && pwd)
PITH="$PITH_ROOT/pith"
RUNTIME="$PITH_ROOT/runtime/libruntime.a"
PITH_INC="$PITH_ROOT/include"
SCRATCH=$(mktemp -d /tmp/opencode/thorn-verify.XXXXXX)

PASS=0
FAIL=0

ok()  { printf '  ok  %s\n' "$1"; PASS=$((PASS + 1)); }
bad() { printf '  FAIL %s\n' "$1"; FAIL=$((FAIL + 1)); }
step() { printf '\n== %s ==\n' "$1"; }

# The author-aware symbol renames pith applies to imported C units
# (must match the bootstrap Makefile).
RENAME_DEFS="-Dproject=c_thorn_engine_project -Dexe=c_thorn_engine_exe \
-Dstatic_lib=c_thorn_engine_static_lib -Dshared_lib=c_thorn_engine_shared_lib \
-Dadd_target=c_thorn_engine_add_target -Dadd_source=c_thorn_engine_add_source \
-Dadd_cflag=c_thorn_engine_add_cflag -Dadd_ldflag=c_thorn_engine_add_ldflag \
-Dadd_include=c_thorn_engine_add_include -Dadd_order_dep=c_thorn_engine_add_order_dep \
-Dpkg_config=c_thorn_engine_pkg_config"

ENGINE_CFLAGS="-std=c99 -O2 -Wall -Wextra -Wno-unused-parameter \
-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE"

# build_project <dir> <author>_<name> <out-binary>
# The per-project recipe: compile the spec with pith, extract the
# plugin object, compile the engine against the project's configure
# symbol, link everything with pith's runtime.
build_project() {
    dir="$1"; sym="c_$2"; bin="$3"
    (cd "$dir" &&
        "$PITH" build build.thorn --plugin -o build_thorn >/dev/null &&
        tar -xOf build_thorn.ppkg plugin.o > build_thorn.o &&
        nm build_thorn.o | grep -q "T $sym" &&
        cc $ENGINE_CFLAGS $RENAME_DEFS \
            "-DTHORN_CONFIGURE_SYMBOL=$sym" -I"$PITH_INC" \
            -c thorn_engine.c -o thorn_engine.o &&
        cc $ENGINE_CFLAGS -I"$PITH_INC" \
            -c thorn_decompile.c -o thorn_decompile.o &&
        cc thorn_engine.o thorn_decompile.o build_thorn.o "$RUNTIME" \
            -o "$bin")
}

DEMO_OUT='demo: add(2,3) = 5
demo: mul(4,5) = 20
demo: sqrt(144.0) = 12.0'

# ------------------------------------------------------------------
step "stage 1: bootstrap thorn (pith plugin + engine + runtime)"
make -C "$ROOT" clean >/dev/null 2>&1 || true
make -C "$ROOT" >/dev/null
if [ -x "$ROOT/thorn" ]; then
    ok "thorn binary built"
else
    bad "thorn binary missing"; exit 1
fi
"$ROOT/thorn" version | grep -q "thorn 0.1.0" && ok "thorn version reports"

# ------------------------------------------------------------------
step "stage 2: thorn emits its own build.ninja; samu rebuilds thorn"
cd "$ROOT"
./thorn --ninja >/dev/null
[ -f build.ninja ] && ok "build.ninja emitted for thorn itself"
samu >/dev/null 2>&1
[ -x ./thorn ] && ok "samu rebuilt thorn from its own ninja"
./thorn version >/dev/null && ok "stage 2 thorn runs"

# determinism: stage 2 emits what stage 1 emitted
cp build.ninja "$SCRATCH/self.ninja"
./thorn --ninja >/dev/null
if cmp -s build.ninja "$SCRATCH/self.ninja"; then
    ok "deterministic: stage 1 and stage 2 emit identical bytes"
else
    bad "ninja output not deterministic"
fi

# ------------------------------------------------------------------
step "demo: per-project thorn binary from build.thorn"
cd "$ROOT/demo"
cp -f "$ROOT/thorn_engine.c" "$ROOT/thorn_engine.h" "$ROOT/thorn_decompile.c" .
if build_project "$PWD" thorn_demo_thorn_configure demo-thorn; then
    ok "demo thorn binary built (spec compiled by pith, linked with engine)"
else
    bad "demo thorn build failed"; exit 1
fi

./demo-thorn >/dev/null
[ -f build.ninja ] && [ -f Makefile ] && ok "demo backends emitted (ninja + make)"

step "demo: samu (ninja backend)"
samu >/dev/null 2>&1
out=$(./demo_app)
if [ "$out" = "$DEMO_OUT" ]; then
    ok "samu build runs the demo, output matches"
else
    bad "samu demo output mismatch: $out"
fi

step "demo: make (portable Makefile backend)"
samu -t clean >/dev/null 2>&1
make >/dev/null
out=$(./demo_app)
if [ "$out" = "$DEMO_OUT" ]; then
    ok "make build runs the demo, output matches"
else
    bad "make demo output mismatch: $out"
fi

# ------------------------------------------------------------------
step "reverse: decompile build.ninja into build.thorn"
./demo-thorn decompile build.ninja -o build.decompiled.thorn >/dev/null
grep -q 'thorn_engine.project("demo")' build.decompiled.thorn \
    && ok "decompile recovers the project name"
grep -q 'add_target("demo_app", thorn_engine.exe)' build.decompiled.thorn \
    && ok "decompile recovers the target and type"
grep -q 'add_source("demo_app", "main.c")' build.decompiled.thorn \
    && grep -q 'add_source("demo_app", "util.c")' build.decompiled.thorn \
    && ok "decompile recovers the sources"
grep -q 'add_include("demo_app", ".")' build.decompiled.thorn \
    && grep -q 'add_cflag("demo_app", "-O2")' build.decompiled.thorn \
    && ok "decompile recovers includes and cflags"
grep -q 'add_ldflag("demo_app", "-lm")' build.decompiled.thorn \
    && ok "decompile recovers ldflags"

step "reverse: decompile the Makefile"
./demo-thorn decompile Makefile -o build.from_make.thorn >/dev/null
grep -q 'add_source("demo_app", "main.c")' build.from_make.thorn \
    && grep -q 'add_ldflag("demo_app", "-lm")' build.from_make.thorn \
    && ok "makefile decompile recovers sources and ldflags"

step "roundtrip: the decompiled spec regenerates identical ninja"
rt="$ROOT/demo/rt"
rm -rf "$rt" && mkdir -p "$rt"
cp build.decompiled.thorn "$rt/build.thorn"
cp thorn_engine.c thorn_engine.h thorn_decompile.c pith.toml "$rt/"
cp main.c util.c util.h "$rt/"
if build_project "$rt" thorn_demo_thorn_configure rt-thorn; then
    ok "decompiled build.thorn compiles with pith and links"
else
    bad "decompiled spec failed to rebuild"; exit 1
fi
(cd "$rt" && ./rt-thorn) >/dev/null
if cmp -s "$rt/build.ninja" "$ROOT/demo/build.ninja"; then
    ok "ROUNDTRIP: decompiled spec regenerates a byte-identical build.ninja"
else
    bad "roundtrip build.ninja differs"
    diff "$ROOT/demo/build.ninja" "$rt/build.ninja" | head -20 || true
fi

# ------------------------------------------------------------------
printf '\n'
printf 'thorn acceptance: %d passed, %d failed\n' "$PASS" "$FAIL"
rm -rf "$SCRATCH"
[ "$FAIL" -eq 0 ]
