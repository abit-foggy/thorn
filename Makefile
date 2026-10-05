CC = cc
PITH_ROOT ?= $(shell if [ -d vendor/pith ]; then echo "vendor/pith"; else echo "../pith"; fi)
PITH_INC = $(PITH_ROOT)/include

OUT = out
ART = $(OUT)/artifacts

# Embedded pith frontend objects and libraries
PITH_FRONT = $(PITH_ROOT)/src/lexer.o $(PITH_ROOT)/src/parser.o \
	$(PITH_ROOT)/src/gen_qbe.o $(PITH_ROOT)/src/engine_proxy.o \
	$(PITH_ROOT)/src/pith_embed.o $(PITH_ROOT)/src/tar.o \
	$(PITH_ROOT)/src/config.o $(PITH_ROOT)/src/cffi.o
PITH_LIBS = $(PITH_ROOT)/vendor/qbe/libqbe.a \
	$(PITH_ROOT)/vendor/tcc/libtcc.a -ldl \
	$(PITH_ROOT)/runtime/libruntime.a

RENAME_DEFS = \
	-Dproject=c_engine_project \
	-Dbackend=c_engine_backend \
	-Dexe=c_engine_exe \
	-Dstatic_lib=c_engine_static_lib \
	-Dshared_lib=c_engine_shared_lib \
	-Dadd_target=c_engine_add_target \
	-Dadd_source=c_engine_add_source \
	-Dadd_cflag=c_engine_add_cflag \
	-Dadd_ldflag=c_engine_add_ldflag \
	-Dadd_include=c_engine_add_include \
	-Dadd_order_dep=c_engine_add_order_dep \
	-Dadd_command=c_engine_add_command \
	-Dpkg_config=c_engine_pkg_config \
	-Demit=c_engine_emit

CFLAGS = -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter \
	-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE

all: $(OUT)/thorn

pith-check:
	@test -x $(PITH_ROOT)/pith || { \
		echo "error: $(PITH_ROOT)/pith is missing - run 'make' there first"; \
		exit 1; }
	@test -f $(PITH_ROOT)/runtime/libruntime.a || { \
		echo "error: $(PITH_ROOT)/runtime/libruntime.a is missing"; \
		exit 1; }

$(ART)/engine.o: src/engine.c src/include/engine.h pith-check
	mkdir -p $(ART)
	$(CC) $(CFLAGS) $(RENAME_DEFS) -I$(PITH_INC) -c src/engine.c -o $@

$(ART)/decompile.o: src/decompile.c src/include/engine.h pith-check
	mkdir -p $(ART)
	$(CC) $(CFLAGS) -I$(PITH_INC) -c src/decompile.c -o $@

$(ART)/main.o: src/main.c src/include/engine.h pith-check
	mkdir -p $(ART)
	$(CC) $(CFLAGS) -I$(PITH_INC) -c src/main.c -o $@

$(OUT)/thorn: $(ART)/main.o $(ART)/engine.o \
	$(ART)/decompile.o $(PITH_FRONT) $(PITH_LIBS)
	mkdir -p $(OUT)
	RT=$$(cd $(PITH_ROOT) && pwd)/runtime/libruntime.a; \
	$(CC) $(ART)/main.o $(ART)/engine.o \
		$(ART)/decompile.o $(PITH_FRONT) $(PITH_LIBS) \
		-DTHORN_RUNTIME_DEFAULT="\"$$RT\"" -o $@

# Stage 2 self-hosting
selfhost: $(OUT)/thorn
	$(OUT)/thorn --out-dir $(ART)
	samu -f $(ART)/build.ninja
	$(ART)/thorn version

check: all
	./tests/verify.sh

clean:
	rm -rf $(OUT)

.PHONY: all selfhost clean pith-check check
