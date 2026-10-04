# thorn bootstrap Makefile - stage 1.
#
# Prerequisites (see README.md):
#   - the pith toolchain built in ../pith: run `make` there
#     (on glibc >= 2.43, use
#      `make CFLAGS="-std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -Iinclude -D_DEFAULT_SOURCE"`)
#   - a POSIX cc, tar, and nm
#
# Stage 1 (this Makefile):
#   1. pith compiles build.thorn --plugin -> build_thorn.ppkg
#   2. tar extracts plugin.o -> build_thorn.o
#      (exports c_thorn_build_thorn_configure, the zero-param Pith
#      entry point; the name comes from pith.toml [project])
#   3. cc compiles the engine with the same -D symbol renames pith
#      applies to imported C units
#   4. cc links engine + decompiler + plugin object + pith's
#      runtime/libruntime.a -> ./thorn
#
# Stage 2 (self-hosting): ./thorn --ninja && samu rebuilds thorn from
# build.thorn's own generated build.ninja.

CC = cc
PITH_ROOT = ../pith
PITH = $(PITH_ROOT)/pith
RUNTIME = $(PITH_ROOT)/runtime/libruntime.a
PITH_INC = $(PITH_ROOT)/include

# pith.toml [project] author="thorn" name="build" fixes the exported
# entry point symbol: c_<author>_<name>_thorn_configure
CONFIGURE_SYM = c_thorn_build_thorn_configure

# The author-aware renames pith applies to every imported C unit; the
# engine is compiled with the identical defines so its symbols match
# the call sites the compiled build.thorn emits.
RENAME_DEFS = \
	-Dproject=c_thorn_engine_project \
	-Dexe=c_thorn_engine_exe \
	-Dstatic_lib=c_thorn_engine_static_lib \
	-Dshared_lib=c_thorn_engine_shared_lib \
	-Dadd_target=c_thorn_engine_add_target \
	-Dadd_source=c_thorn_engine_add_source \
	-Dadd_cflag=c_thorn_engine_add_cflag \
	-Dadd_ldflag=c_thorn_engine_add_ldflag \
	-Dadd_include=c_thorn_engine_add_include \
	-Dadd_order_dep=c_thorn_engine_add_order_dep \
	-Dpkg_config=c_thorn_engine_pkg_config

CFLAGS = -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter \
	-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE

all: thorn

build_thorn.o: build.thorn thorn_engine.c thorn_engine.h pith.toml
	$(PITH) build build.thorn --plugin -o build_thorn
	tar -xOf build_thorn.ppkg plugin.o > $@
	@nm $@ | grep -q " T $(CONFIGURE_SYM)" || { \
		echo "thorn: $(CONFIGURE_SYM) is missing from the plugin object"; \
		echo "thorn: check pith.toml [project] author/name (must be thorn/build)"; \
		exit 1; }

thorn_engine.o: thorn_engine.c thorn_engine.h
	$(CC) $(CFLAGS) $(RENAME_DEFS) \
		-DTHORN_CONFIGURE_SYMBOL=$(CONFIGURE_SYM) \
		-I$(PITH_INC) -c thorn_engine.c -o $@

thorn_decompile.o: thorn_decompile.c thorn_engine.h
	$(CC) $(CFLAGS) -I$(PITH_INC) -c thorn_decompile.c -o $@

thorn: thorn_engine.o thorn_decompile.o build_thorn.o $(RUNTIME)
	$(CC) thorn_engine.o thorn_decompile.o build_thorn.o $(RUNTIME) -o $@

clean:
	rm -f thorn thorn_engine.o thorn_decompile.o build_thorn.o \
		build_thorn.ppkg build.ninja thorn__*.o
	rm -rf thorn
