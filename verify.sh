#!/bin/sh
# verify.sh - the thorn acceptance suite (embed mode).
#
# Proves the full contract:
#   stage 1: out/thorn built by linking thorn's engine with pith's
#            frontend (the embedded language) and runtime
#   stage 2: thorn evaluates its own build.thorn at runtime, emits
#            out/artifacts/build.ninja, and samu rebuilds thorn from
#            it (self-hosting through its own generated backend)
#   demo:    one generic thorn binary evaluates demo/build.thorn,
#            both backends build the demo, program output matches
#   reverse: thorn decompile ingests build.ninja and the Makefile
#            into idiomatic build.thorn specs, and the decompiled
#            spec regenerates a byte-identical build.ninja
#
# Prerequisite: pith built at ../pith (see the Makefile).

set -e

ROOT=$(cd "$(dirname "$0")" && pwd)
PITH_ROOT=$(cd "$ROOT/../pith" && pwd)
SCRATCH=$(mktemp -d /tmp/opencode/thorn-verify.XXXXXX)

PASS=0
FAIL=0

ok()  { printf '  ok  %s\n' "$1"; PASS=$((PASS + 1)); }
bad() { printf '  FAIL %s\n' "$1"; FAIL=$((FAIL + 1)); }
step() { printf '\n== %s ==\n' "$1"; }

DEMO_OUT='demo: add(2,3) = 5
demo: mul(4,5) = 20
demo: sqrt(144.0) = 12.0'

# ------------------------------------------------------------------
step "stage 1: build out/thorn (thorn engine + embedded pith)"
make -C "$ROOT" clean >/dev/null 2>&1 || true
make -C "$ROOT" >/dev/null
if [ -x "$ROOT/out/thorn" ]; then
    ok "out/thorn built"
else
    bad "out/thorn missing"; exit 1
fi
"$ROOT/out/thorn" version | grep -q "thorn " && ok "thorn version reports"

# ------------------------------------------------------------------
step "stage 2: thorn self-hosts through its own generated backend"
cd "$ROOT"
./out/thorn --out-dir out/artifacts >/dev/null 2>&1
[ -f out/artifacts/build.ninja ] && [ -f out/artifacts/Makefile ] \
    && ok "backends generated into out/artifacts"
samu -f out/artifacts/build.ninja >/dev/null 2>&1
[ -x out/artifacts/thorn ] && ok "samu rebuilt thorn from its own ninja"
./out/artifacts/thorn version >/dev/null \
    && ok "stage 2 thorn runs (embeds pith, reads specs at runtime)"

# determinism: a second run emits identical bytes
cp out/artifacts/build.ninja "$SCRATCH/self.ninja"
./out/thorn --out-dir out/artifacts >/dev/null 2>&1
if cmp -s out/artifacts/build.ninja "$SCRATCH/self.ninja"; then
    ok "deterministic: identical bytes across runs"
else
    bad "ninja output not deterministic"
fi

# the stage 2 binary fully works too (decompile path, no pith needed)
./out/artifacts/thorn decompile out/artifacts/build.ninja \
    -o "$SCRATCH/self.thorn" >/dev/null \
    && grep -q 'add_target("thorn_core", thorn_engine.static_lib)' \
        "$SCRATCH/self.thorn" \
    && ok "stage 2 binary decompiles its own backend"

# ------------------------------------------------------------------
step "demo: one generic thorn binary serves any project"
cd "$ROOT/demo"
../out/thorn >/dev/null 2>&1
[ -f build.ninja ] && [ -f Makefile ] \
    && ok "demo backends emitted in the project directory"

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
../out/thorn decompile build.ninja -o build.decompiled.thorn >/dev/null
grep -q 'thorn_engine.project("demo")' build.decompiled.thorn \
    && ok "decompile recovers the project name"
grep -q 'add_target("demo_app", thorn_engine.exe)' \
    build.decompiled.thorn \
    && ok "decompile recovers the target and type"
grep -q 'add_source("demo_app", "main.c")' build.decompiled.thorn \
    && grep -q 'add_source("demo_app", "util.c")' \
    build.decompiled.thorn \
    && ok "decompile recovers the sources"
grep -q 'add_include("demo_app", ".")' build.decompiled.thorn \
    && grep -q 'add_cflag("demo_app", "-O2")' build.decompiled.thorn \
    && ok "decompile recovers includes and cflags"
grep -q 'add_ldflag("demo_app", "-lm")' build.decompiled.thorn \
    && ok "decompile recovers ldflags"

step "reverse: decompile the Makefile"
../out/thorn decompile Makefile -o build.from_make.thorn >/dev/null
grep -q 'add_source("demo_app", "main.c")' build.from_make.thorn \
    && grep -q 'add_ldflag("demo_app", "-lm")' build.from_make.thorn \
    && ok "makefile decompile recovers sources and ldflags"

step "roundtrip: the decompiled spec regenerates identical ninja"
rt="$ROOT/demo/rt"
rm -rf "$rt" && mkdir -p "$rt"
cp build.decompiled.thorn "$rt/build.thorn"
cp main.c util.c util.h "$rt/"
(cd "$rt" && ../../out/thorn >/dev/null 2>&1)
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
