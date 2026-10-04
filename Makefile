# thorn bootstrap Makefile - builds out/thorn.
#
# thorn embeds pith (the Lua-in-a-game-engine model): the binary
# carries pith's frontend and evaluates build.thorn at runtime, so
# ONE thorn serves every project. Prerequisites:
#
#   - the pith toolchain built in ../pith (run `make` there; on
#     glibc >= 2.43 see the -D_DEFAULT_SOURCE note in README.md)
#   - a POSIX cc
#
# Layout: sources in src/, headers in src/include/, objects and
# generated backends in out/artifacts/, the binary in out/.
#
# src/thorn_engine.c compiles under the -D<stem>=c_thorn_engine_<stem>
# renames (the same author-aware mangling pith applies to imported C
# units) so it doubles as the temp-executable fallback link object:
# when a hardened kernel blocks in-memory execution, pith links this
# object into the child process, whose script calls then resolve.

CC = cc
PITH_ROOT = ../pith
PITH_INC = $(PITH_ROOT)/include

OUT = out
ART = $(OUT)/artifacts

# the embedded language: pith's frontend objects and libraries
PITH_FRONT = $(PITH_ROOT)/src/lexer.o $(PITH_ROOT)/src/parser.o \
	$(PITH_ROOT)/src/gen_qbe.o $(PITH_ROOT)/src/engine_proxy.o \
	$(PITH_ROOT)/src/pith_embed.o $(PITH_ROOT)/src/tar.o \
	$(PITH_ROOT)/src/config.o $(PITH_ROOT)/src/cffi.o
PITH_LIBS = $(PITH_ROOT)/vendor/qbe/libqbe.a \
	$(PITH_ROOT)/vendor/tcc/libtcc.a -ldl \
	$(PITH_ROOT)/runtime/libruntime.a

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
	-Dpkg_config=c_thorn_engine_pkg_config \
	-Demit=c_thorn_engine_emit

CFLAGS = -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter \
	-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE

all: $(OUT)/thorn

pith-check:
	@test -x $(PITH_ROOT)/pith || { \
		echo "thorn: $(PITH_ROOT)/pith is missing - run 'make' there first"; \
		exit 1; }
	@test -f $(PITH_ROOT)/runtime/libruntime.a || { \
		echo "thorn: $(PITH_ROOT)/runtime/libruntime.a is missing"; \
		exit 1; }

$(ART)/thorn_engine.o: src/thorn_engine.c src/include/thorn_engine.h pith-check
	mkdir -p $(ART)
	$(CC) $(CFLAGS) $(RENAME_DEFS) -I$(PITH_INC) -c src/thorn_engine.c -o $@

$(ART)/thorn_decompile.o: src/thorn_decompile.c src/include/thorn_engine.h pith-check
	mkdir -p $(ART)
	$(CC) $(CFLAGS) -I$(PITH_INC) -c src/thorn_decompile.c -o $@

$(ART)/thorn_main.o: src/thorn_main.c src/include/thorn_engine.h pith-check
	mkdir -p $(ART)
	$(CC) $(CFLAGS) -I$(PITH_INC) -c src/thorn_main.c -o $@

$(OUT)/thorn: $(ART)/thorn_main.o $(ART)/thorn_engine.o \
	$(ART)/thorn_decompile.o $(PITH_FRONT) $(PITH_LIBS)
	mkdir -p $(OUT)
	RT=$$(cd $(PITH_ROOT) && pwd)/runtime/libruntime.a; \
	$(CC) $(ART)/thorn_main.o $(ART)/thorn_engine.o \
		$(ART)/thorn_decompile.o $(PITH_FRONT) $(PITH_LIBS) \
		-DTHORN_RUNTIME_DEFAULT="\"$$RT\"" -o $@

# stage 2: thorn generates its own backends and rebuilds itself
selfhost: $(OUT)/thorn
	$(OUT)/thorn --out-dir $(ART)
	samu -f $(ART)/build.ninja
	$(ART)/thorn version

clean:
	rm -rf $(OUT)

.PHONY: all selfhost clean pith-check
