#ifndef THORN_ENGINE_H
#define THORN_ENGINE_H

#include <stddef.h>
#include <stdio.h>

struct PithContext;
typedef struct PithContext PithContext;

#define THORN_VERSION "1.0"

/* Target types */
enum {
    THORN_EXE = 1,
    THORN_STATIC_LIB = 2,
    THORN_SHARED_LIB = 3
};

/* Source kinds */
typedef enum SourceKind {
    SRC_C = 0,
    SRC_ASM_RAW,
    SRC_ASM_CPP
} SourceKind;

/* String list */
typedef struct StrList {
    char **items;
    size_t count;
    size_t cap;
} StrList;

typedef struct Target {
    char name[256];
    int type;
    StrList sources;
    SourceKind *source_kinds;
    StrList cflags;
    StrList asflags;
    StrList ldflags;
    StrList includes;
    StrList order_deps;
} Target;

typedef struct Command {
    char output[256];
    char command[1024];
    char input[256];
} Command;

typedef struct Graph {
    char proj[256];
    char backend[32];
    Target *targets;
    size_t count;
    size_t cap;
    Command *commands;
    size_t cmd_count;
    size_t cmd_cap;
} Graph;

void graph_init(Graph *g);
void graph_free(Graph *g);
Target *graph_find(Graph *g, const char *name);
Target *graph_add(Graph *g, const char *name, int type);
int graph_add_command(Graph *g, const char *out, const char *cmd,
                      const char *in);

int thorn_add_asflag(Target *t, const char *flag);

int strlist_push(StrList *l, const char *s);
int strlist_push_force(StrList *l, const char *s);
void strlist_free(StrList *l);

/* Diagnostics & path helpers */
void diag(const char *fmt, ...);
void join_path(char *out, size_t n, const char *dir,
               const char *name);
void dir_prefix(const char *path, char *prefix, size_t n);
int makedirs(const char *dir);

/* Backend emitters */
int emit_ninja(const Graph *g, const char *cc, const char *ar,
               const char *path);
int emit_makefile(const Graph *g, const char *cc, const char *ar,
                  const char *path);

/* Spec printer */
int print_spec(const Graph *g, FILE *out, const char **notes,
               size_t nnotes);

/* Host registration */
int host_register(PithContext *ctx);

/* Reverse decompilation */
int decompile_file(const char *path, Graph *g, char ***notes,
                   size_t *nnotes);
int decompile_file_scoped(const char *path, const char *dir_scope,
                          Graph *g, char ***notes, size_t *nnotes);

/* Hookable decompiler API (usable from C and standalone Pith scripts) */
struct PithValue;
typedef struct PithValue PithValue;

int add_asflag(PithValue *target, PithValue *flag);

void reset(void);
void set_project(PithValue *name);
void set_compiler(PithValue *cc);
void set_ar(PithValue *ar);
void set_dir_prefix(PithValue *pfx);
void ignore_target(PithValue *pattern);
void keep_target(PithValue *pattern);
void strip_cflag(PithValue *pattern);
void inject_cflag(PithValue *target_pattern, PithValue *flag);
void inject_include(PithValue *target_pattern, PithValue *inc);
void remap_target(PithValue *old_name, PithValue *new_name);
void add_command_edge(PithValue *out, PithValue *cmd, PithValue *in);
int parse_file(PithValue *path);
int parse_scoped(PithValue *path, PithValue *dir_scope);
int parse_string(PithValue *content);
long target_count(void);
long command_count(void);
PithValue *get_target_name(long index);
long get_target_type(long index);
PithValue *get_target_sources(long index);
PithValue *get_target_cflags(long index);
PithValue *get_target_ldflags(long index);
PithValue *get_target_includes(long index);
PithValue *get_target_order_deps(long index);
PithValue *get_command_output(long index);
PithValue *get_command_line(long index);
PithValue *get_command_input(long index);
void set_target_type(PithValue *target_name, long type);
void add_target_source(PithValue *target_name, PithValue *src);
void remove_target(PithValue *target_name);
int emit_ninja_file(PithValue *out_path);
int emit_posix_make_file(PithValue *out_path);
int emit_thorn_file(PithValue *out_path);
PithValue *to_ninja(void);
PithValue *to_posix_make(void);
PithValue *to_thorn(void);
int convert(PithValue *in_path, PithValue *out_path, PithValue *format);

#define decompile_reset reset
#define decompile_set_project set_project
#define decompile_set_compiler set_compiler
#define decompile_set_ar set_ar
#define decompile_set_dir_prefix set_dir_prefix
#define decompile_ignore_target ignore_target
#define decompile_keep_target keep_target
#define decompile_strip_cflag strip_cflag
#define decompile_inject_cflag inject_cflag
#define decompile_inject_include inject_include
#define decompile_remap_target remap_target
#define decompile_add_command_edge add_command_edge
#define decompile_parse parse_file
#define decompile_parse_scoped parse_scoped
#define decompile_parse_string parse_string
#define decompile_target_count target_count
#define decompile_command_count command_count
#define decompile_get_target_name get_target_name
#define decompile_get_target_type get_target_type
#define decompile_get_target_sources get_target_sources
#define decompile_get_target_cflags get_target_cflags
#define decompile_get_target_ldflags get_target_ldflags
#define decompile_get_target_includes get_target_includes
#define decompile_get_target_order_deps get_target_order_deps
#define decompile_get_command_output get_command_output
#define decompile_get_command_line get_command_line
#define decompile_get_command_input get_command_input
#define decompile_set_target_type set_target_type
#define decompile_add_target_source add_target_source
#define decompile_remove_target remove_target
#define decompile_emit_ninja emit_ninja_file
#define decompile_emit_posix_make emit_posix_make_file
#define decompile_emit_thorn emit_thorn_file
#define decompile_to_ninja_string to_ninja
#define decompile_to_posix_make_string to_posix_make
#define decompile_to_thorn_string to_thorn
#define decompile_convert convert

#endif /* THORN_ENGINE_H */
