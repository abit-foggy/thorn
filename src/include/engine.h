#ifndef THORN_ENGINE_H
#define THORN_ENGINE_H

#include <stddef.h>
#include <stdio.h>

#include <pith_embed.h>

#define THORN_VERSION "0.3.0"

/* Target types */
enum {
    THORN_EXE = 1,
    THORN_STATIC_LIB = 2,
    THORN_SHARED_LIB = 3
};

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
    StrList cflags;
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

#endif /* THORN_ENGINE_H */
