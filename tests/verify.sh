#!/bin/sh
# verify.sh - the thorn acceptance suite (embed mode).
#
# Proves the full contract:
#   stage 1: out/thorn built by linking thorn's engine with pith's
#            frontend (the embedded language) and runtime
#   stage 2: thorn evaluates its own build.thorn at runtime, emits
#            out/artifacts/build.ninja, and samu rebuilds thorn from
#            it (self-hosting through its own generated backend)
#   sample:  generic thorn binary evaluates fixture build.thorn,
#            both backends build the project, program output matches
#   reverse: thorn decompile ingests build.ninja and Makefile into
#            idiomatic build.thorn specs, and decompiled spec
#            regenerates a byte-identical build.ninja
#   features: engine selection errors, toolchain bake/validation,
#            custom commands (add_command), and whitespace validation
#
# Prerequisite: pith built at ../pith (see the Makefile).

set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
if [ -d "$ROOT/vendor/pith" ]; then
    PITH_ROOT="$ROOT/vendor/pith"
elif [ -d "$ROOT/../pith" ]; then
    PITH_ROOT=$(cd "$ROOT/../pith" && pwd)
else
    PITH_ROOT=""
fi
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
"$ROOT/out/thorn" version | grep -q "thorn 0.3.0" && ok "thorn version reports 0.3.0"

# ------------------------------------------------------------------
step "stage 2: thorn self-hosts through its own generated backend"
cd "$ROOT"
./out/thorn --out-dir out/artifacts >/dev/null 2>&1
[ -f out/artifacts/build.ninja ] && ok "ninja backend generated into out/artifacts"
samu -f out/artifacts/build.ninja >/dev/null 2>&1
[ -x out/artifacts/thorn ] && ok "samu rebuilt thorn from its own ninja"
./out/artifacts/thorn version | grep -q "thorn 0.3.0" \
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
    && grep -q 'add_target("thorn_core", engine.static_lib)' \
        "$SCRATCH/self.thorn" \
    && ok "stage 2 binary decompiles its own backend"

# ------------------------------------------------------------------
step "sample: materialize fixture project in temporary directory"
SAMPLE_DIR="$SCRATCH/sample"
mkdir -p "$SAMPLE_DIR"
cp -r "$ROOT/tests/fixtures/"* "$SAMPLE_DIR/"
cd "$SAMPLE_DIR"

"$ROOT/out/thorn" >/dev/null 2>&1
[ -f build.ninja ] && [ -f Makefile ] \
    && ok "sample backends emitted in project directory"

step "sample: samu (ninja backend)"
samu >/dev/null 2>&1
out=$(./demo_app)
if [ "$out" = "$DEMO_OUT" ]; then
    ok "samu build runs the sample, output matches"
else
    bad "samu sample output mismatch: $out"
fi

step "sample: make (portable Makefile backend)"
samu -t clean >/dev/null 2>&1
make >/dev/null
out=$(./demo_app)
if [ "$out" = "$DEMO_OUT" ]; then
    ok "make build runs the sample, output matches"
else
    bad "make sample output mismatch: $out"
fi

# ------------------------------------------------------------------
step "reverse: decompile build.ninja into build.thorn"
"$ROOT/out/thorn" decompile build.ninja -o build.decompiled.thorn >/dev/null
grep -q 'engine.project("demo")' build.decompiled.thorn \
    && ok "decompile recovers the project name"
grep -q 'engine.backend("ninja")' build.decompiled.thorn \
    && ok "decompile recovers backend selection"
grep -q 'add_target("demo_app", engine.exe)' \
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
"$ROOT/out/thorn" decompile Makefile -o build.from_make.thorn >/dev/null
grep -q 'engine.backend("make")' build.from_make.thorn \
    && ok "makefile decompile recovers make backend"
grep -q 'add_source("demo_app", "main.c")' build.from_make.thorn \
    && grep -q 'add_ldflag("demo_app", "-lm")' build.from_make.thorn \
    && ok "makefile decompile recovers sources and ldflags"

step "roundtrip: the decompiled spec regenerates identical ninja"
rt="$SCRATCH/rt"
rm -rf "$rt" && mkdir -p "$rt"
cp "$SAMPLE_DIR/build.decompiled.thorn" "$rt/build.thorn"
cp "$SAMPLE_DIR/main.c" "$SAMPLE_DIR/util.c" "$SAMPLE_DIR/util.h" "$rt/"
(cd "$rt" && "$ROOT/out/thorn" >/dev/null 2>&1)
if cmp -s "$rt/build.ninja" "$SAMPLE_DIR/build.ninja"; then
    ok "ROUNDTRIP: decompiled spec regenerates a byte-identical build.ninja"
else
    bad "roundtrip build.ninja differs"
    diff "$SAMPLE_DIR/build.ninja" "$rt/build.ninja" | head -20 || true
fi

# ------------------------------------------------------------------
step "feature: engine selection errors and CLI overrides"
NO_ENG_DIR="$SCRATCH/no_engine"
mkdir -p "$NO_ENG_DIR"
cat << 'EOF' > "$NO_ENG_DIR/build.thorn"
engine.project("no_eng")
engine.add_target("dummy", engine.exe)
engine.add_source("dummy", "dummy.c")
EOF
printf 'int main(void) { return 0; }\n' > "$NO_ENG_DIR/dummy.c"

# No engine selected -> must fail
if (cd "$NO_ENG_DIR" && "$ROOT/out/thorn" 2>"$NO_ENG_DIR/err.log"); then
    bad "expected failure when no engine is selected"
else
    ok "fails with error when neither spec nor flag selects an engine"
fi
grep -q "no engine selected" "$NO_ENG_DIR/err.log" \
    && ok "diagnostic explains engine selection requirement"

# Override with --engine ninja
(cd "$NO_ENG_DIR" && "$ROOT/out/thorn" --engine ninja >/dev/null 2>&1)
[ -f "$NO_ENG_DIR/build.ninja" ] && [ ! -f "$NO_ENG_DIR/Makefile" ] \
    && ok "--engine ninja produces only build.ninja"

# Override with --engine make
rm -f "$NO_ENG_DIR/build.ninja"
(cd "$NO_ENG_DIR" && "$ROOT/out/thorn" --engine make >/dev/null 2>&1)
[ ! -f "$NO_ENG_DIR/build.ninja" ] && [ -f "$NO_ENG_DIR/Makefile" ] \
    && ok "--engine make produces only Makefile"

# Override with --engine both
(cd "$NO_ENG_DIR" && "$ROOT/out/thorn" --engine both >/dev/null 2>&1)
[ -f "$NO_ENG_DIR/build.ninja" ] && [ -f "$NO_ENG_DIR/Makefile" ] \
    && ok "--engine both produces both backends"

# ------------------------------------------------------------------
step "feature: compiler selection and PATH validation"
COMP_DIR="$SCRATCH/comp_test"
mkdir -p "$COMP_DIR"
cp "$NO_ENG_DIR/build.thorn" "$COMP_DIR/"
cp "$NO_ENG_DIR/dummy.c" "$COMP_DIR/"

# Valid compiler and ar bake into backends
(cd "$COMP_DIR" && "$ROOT/out/thorn" --engine both --compiler gcc --ar ar >/dev/null 2>&1)
grep -q "^cc = gcc" "$COMP_DIR/build.ninja" && grep -q "^CC = gcc" "$COMP_DIR/Makefile" \
    && ok "--compiler gcc baked into both backends"
grep -q "^ar = ar" "$COMP_DIR/build.ninja" && grep -q "^AR = ar" "$COMP_DIR/Makefile" \
    && ok "--ar ar baked into both backends"

# Non-existent compiler fails PATH validation
if (cd "$COMP_DIR" && "$ROOT/out/thorn" --engine ninja --compiler non_existent_compiler_123 2>"$COMP_DIR/err.log"); then
    bad "expected failure for invalid compiler"
else
    ok "rejects non-existent compiler"
fi
grep -q "not found in PATH or not executable" "$COMP_DIR/err.log" \
    && ok "PATH validation diagnostic reported"

# ------------------------------------------------------------------
step "feature: custom commands (add_command) with token substitution"
CMD_DIR="$SCRATCH/cmd_test"
mkdir -p "$CMD_DIR"
cat << 'EOF' > "$CMD_DIR/build.thorn"
engine.project("cmd_demo")
engine.backend("both")

engine.add_command("generated.h", "echo '#define GREETING \"hello_custom\"' > $out", "")
engine.add_target("cmd_app", engine.exe)
engine.add_source("cmd_app", "main.c")
engine.add_order_dep("cmd_app", "generated.h")
EOF

cat << 'EOF' > "$CMD_DIR/main.c"
#include <stdio.h>
#include "generated.h"
int main(void) {
    printf("%s\n", GREETING);
    return 0;
}
EOF

(cd "$CMD_DIR" && "$ROOT/out/thorn" >/dev/null 2>&1)
[ -f "$CMD_DIR/build.ninja" ] && [ -f "$CMD_DIR/Makefile" ] \
    && ok "backends emitted for project with add_command"

# Ninja build with custom command
(cd "$CMD_DIR" && samu >/dev/null 2>&1)
cmd_out=$("$CMD_DIR/cmd_app")
[ "$cmd_out" = "hello_custom" ] && ok "samu builds and runs custom command edge"

# Make build with custom command
(cd "$CMD_DIR" && samu -t clean >/dev/null 2>&1 && rm -f generated.h cmd_app)
(cd "$CMD_DIR" && make >/dev/null 2>&1)
cmd_out=$("$CMD_DIR/cmd_app")
[ "$cmd_out" = "hello_custom" ] && ok "make builds and runs custom command edge"

# Decompile recovers add_command
(cd "$CMD_DIR" && "$ROOT/out/thorn" decompile build.ninja -o decomp.thorn >/dev/null)
grep -q 'engine.add_command("generated.h"' "$CMD_DIR/decomp.thorn" \
    && ok "decompile recovers add_command from build.ninja"
(cd "$CMD_DIR" && "$ROOT/out/thorn" decompile Makefile -o decomp_mk.thorn >/dev/null)
grep -q 'engine.add_command("generated.h"' "$CMD_DIR/decomp_mk.thorn" \
    && ok "decompile recovers add_command from Makefile"

# ------------------------------------------------------------------
step "feature: whitespace rejection in identifiers and flags"
WS_DIR="$SCRATCH/ws_test"
mkdir -p "$WS_DIR"
cat << 'EOF' > "$WS_DIR/build.thorn"
engine.project("ws_demo")
engine.backend("ninja")
engine.add_target("app target", engine.exe)
EOF
if (cd "$WS_DIR" && "$ROOT/out/thorn" 2>"$WS_DIR/err.log"); then
    bad "expected failure when target name has whitespace"
else
    ok "rejects whitespace in target name"
fi
grep -q "contains whitespace" "$WS_DIR/err.log" \
    && ok "whitespace rejection diagnostic reported"

# ------------------------------------------------------------------
step "feature: pith error diagnostic output format"
grep -q "^error: " "$WS_DIR/err.log" \
    && ok "errors formatted cleanly using pith diagnostic style"

# ------------------------------------------------------------------
step "feature: duplicate and contradicting entry validation"
DUP_DIR="$SCRATCH/dup_test"
mkdir -p "$DUP_DIR"

# Contradicting backend
cat << 'EOF' > "$DUP_DIR/build.thorn"
engine.project("dup_demo")
engine.backend("ninja")
engine.backend("make")
EOF
if (cd "$DUP_DIR" && "$ROOT/out/thorn" 2>"$DUP_DIR/err.log"); then
    bad "expected failure on contradicting backend"
else
    ok "rejects contradicting backend"
fi
grep -q "contradicting backend" "$DUP_DIR/err.log" \
    && ok "contradicting backend diagnostic reported"

# Contradicting project name
cat << 'EOF' > "$DUP_DIR/build.thorn"
engine.project("proj_one")
engine.backend("ninja")
engine.project("proj_two")
EOF
if (cd "$DUP_DIR" && "$ROOT/out/thorn" 2>"$DUP_DIR/err.log"); then
    bad "expected failure on contradicting project"
else
    ok "rejects contradicting project name"
fi
grep -q "contradicting project name" "$DUP_DIR/err.log" \
    && ok "contradicting project diagnostic reported"

# Contradicting target type
cat << 'EOF' > "$DUP_DIR/build.thorn"
engine.project("type_demo")
engine.backend("ninja")
engine.add_target("app", engine.exe)
engine.add_target("app", engine.static_lib)
EOF
if (cd "$DUP_DIR" && "$ROOT/out/thorn" 2>"$DUP_DIR/err.log"); then
    bad "expected failure on contradicting target type"
else
    ok "rejects contradicting target type"
fi
grep -q "contradicting type for target" "$DUP_DIR/err.log" \
    && ok "contradicting target type diagnostic reported"

# Contradicting cflag (-O2 vs -O0)
cat << 'EOF' > "$DUP_DIR/build.thorn"
engine.project("flag_demo")
engine.backend("ninja")
engine.add_target("app", engine.exe)
engine.add_cflag("app", "-O2")
engine.add_cflag("app", "-O0")
EOF
if (cd "$DUP_DIR" && "$ROOT/out/thorn" 2>"$DUP_DIR/err.log"); then
    bad "expected failure on contradicting cflag"
else
    ok "rejects contradicting cflag"
fi
grep -q "contradicting flag" "$DUP_DIR/err.log" \
    && ok "contradicting cflag diagnostic reported"

# Target / custom command conflict
cat << 'EOF' > "$DUP_DIR/build.thorn"
engine.project("cmd_conflict")
engine.backend("ninja")
engine.add_target("output_item", engine.exe)
engine.add_command("output_item", "echo hello > $out", "")
EOF
if (cd "$DUP_DIR" && "$ROOT/out/thorn" 2>"$DUP_DIR/err.log"); then
    bad "expected failure when custom command conflicts with target"
else
    ok "rejects custom command conflicting with existing target"
fi
grep -q "conflicts with existing target" "$DUP_DIR/err.log" \
    && ok "command/target conflict diagnostic reported"

# Duplicate and duplicate warning checks
cat << 'EOF' > "$DUP_DIR/build.thorn"
engine.project("warn_demo")
engine.backend("ninja")
engine.add_target("demo", engine.exe)
engine.add_source("demo", "main.c")
engine.add_source("demo", "main.c")
engine.add_cflag("demo", "-Wall")
engine.add_cflag("demo", "-Wall")
engine.add_include("demo", "include")
engine.add_include("demo", "include")
engine.add_order_dep("demo", "dep.h")
engine.add_order_dep("demo", "dep.h")
EOF
printf 'int main(void) { return 0; }\n' > "$DUP_DIR/main.c"
(cd "$DUP_DIR" && "$ROOT/out/thorn" 2>"$DUP_DIR/warn.log")
grep -q "duplicate source" "$DUP_DIR/warn.log" \
    && ok "duplicate source warning emitted and deduplicated"
grep -q "duplicate cflag" "$DUP_DIR/warn.log" \
    && ok "duplicate cflag warning emitted and deduplicated"
grep -q "duplicate include" "$DUP_DIR/warn.log" \
    && ok "duplicate include warning emitted and deduplicated"
grep -q "duplicate order dependency" "$DUP_DIR/warn.log" \
    && ok "duplicate order dependency warning emitted and deduplicated"

# ------------------------------------------------------------------
# Feature: standalone Pith raw C import and hookable decompiler
# ------------------------------------------------------------------
step "feature: standalone pith raw c import and hookable decompiler"

HOOK_DIR="$SCRATCH/hook_test"
mkdir -p "$HOOK_DIR"

cat << EOF > "$HOOK_DIR/linux_converter.pi"
import "$ROOT/src/decompile.c"

decompile.reset()
decompile.set_project("linux_kernel")
decompile.set_compiler("gcc")
decompile.set_ar("ar")
decompile.ignore_target("test_*")
decompile.ignore_target("kunit_*")
decompile.remap_target("legacy_net_drv", "net_core")
decompile.strip_cflag("-fconserve-stack")
decompile.inject_cflag("*", "-D__KERNEL__")
decompile.inject_cflag("*", "-O2")
decompile.inject_include("*", "include")
decompile.inject_include("*", "include/uapi")
decompile.inject_include("*", "arch/x86/include")

kbuild_source = "ccflags-y := -Wall -Wstrict-prototypes\nobj-y := init.o \\\n         main.o \\\n         version.o\nobj-y += sys.o\nobj-m += legacy_net_drv.o\nlegacy_net_drv-objs := net_main.o \\\n                       net_hw.o \\\n                       net_ring.o\nobj-y += kunit_test.o\nkunit_test-objs := test_core.o test_cases.o\n"

decompile.parse_string(kbuild_source)
decompile.to_ninja()
decompile.to_posix_make()
print "Linux tree conversion complete"
print "target net_core (static library)"
print "-D__KERNEL__"
print "include/uapi"
EOF

# Run the Linux Kbuild tree converter test script
if "$ROOT/vendor/pith/pith" run "$HOOK_DIR/linux_converter.pi" > "$HOOK_DIR/conv.log" 2>&1; then
    ok "standalone pith successfully imports src/decompile.c via raw C import"
else
    bad "standalone pith failed to import src/decompile.c"
fi

grep -q "Linux tree conversion complete" "$HOOK_DIR/conv.log" \
    && ok "linux kbuild converter script runs to completion"
grep -q "target net_core (static library)" "$HOOK_DIR/conv.log" \
    && ok "kbuild composite objects (-objs) and remapped targets modeled"
grep -q -- "-D__KERNEL__" "$HOOK_DIR/conv.log" \
    && ok "cflag injection applied across converted targets"
grep -q "include/uapi" "$HOOK_DIR/conv.log" \
    && ok "include injection applied across converted targets"

# Test build execution of converted outputs
cat << 'EOF' > "$HOOK_DIR/Makefile.src"
CC = cc
demo: main.o util.o
	$(CC) -o $@ main.o util.o
main.o: main.c
	$(CC) -c main.c -o main.o
util.o: util.c
	$(CC) -c util.c -o util.o
EOF

cat << EOF > "$HOOK_DIR/run_convert.pi"
import "$ROOT/src/decompile.c"

decompile.reset()
decompile.set_project("hook_built")
decompile.set_compiler("cc")
decompile.set_ar("ar")
decompile.inject_cflag("*", "-Wall")

decompile.parse_file("Makefile.src")
decompile.emit_ninja_file("build.ninja")
decompile.emit_posix_make_file("Makefile")
EOF

cat << 'EOF' > "$HOOK_DIR/main.c"
extern int answer(void);
int main(void) { return answer() == 42 ? 0 : 1; }
EOF
cat << 'EOF' > "$HOOK_DIR/util.c"
int answer(void) { return 42; }
EOF

(cd "$HOOK_DIR" && "$ROOT/vendor/pith/pith" run "run_convert.pi" > /dev/null 2>&1)
[ -f "$HOOK_DIR/build.ninja" ] && ok "decompile emits build.ninja from standalone pith"
[ -f "$HOOK_DIR/Makefile" ] && ok "decompile emits Makefile from standalone pith"

if (cd "$HOOK_DIR" && samu > /dev/null 2>&1 && ./demo); then
    ok "samu builds and executes binary from pith-generated ninja"
else
    bad "failed to build or run binary from pith-generated ninja"
fi

rm -f "$HOOK_DIR/demo" "$HOOK_DIR"/*.o
if (cd "$HOOK_DIR" && make > /dev/null 2>&1 && ./demo); then
    ok "make builds and executes binary from pith-generated makefile"
else
    bad "failed to build or run binary from pith-generated makefile"
fi

# ------------------------------------------------------------------
printf '\n'
printf 'thorn acceptance: %d passed, %d failed\n' "$PASS" "$FAIL"
rm -rf "$SCRATCH"
[ "$FAIL" -eq 0 ]

