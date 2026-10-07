#include "include/graph.h"

/* Diagnostics */
__attribute__((weak))
void pith_emit_diagnostic(const char *severity, const char *message,
                          const char *filepath, const char *source,
                          size_t line, size_t col, size_t span);

int g_errors = 0;

void diag(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    const char *sev = "error";
    const char *msg = buf;
    if (strncmp(buf, "warning: ", 9) == 0) {
        sev = "warning";
        msg = buf + 9;
    } else if (strncmp(buf, "error: ", 7) == 0) {
        sev = "error";
        msg = buf + 7;
    }

    if (strcmp(sev, "error") == 0)
        g_errors++;

    if (pith_emit_diagnostic)
        pith_emit_diagnostic(sev, msg, NULL, NULL, 0, 0, 0);
    else
        fprintf(stderr, "%s: %s\n", sev, msg);
    fflush(stderr);
}

int sb_init(SBuf *b)
{
    b->cap = 128;
    b->len = 0;
    b->buf = malloc(b->cap);
    if (!b->buf)
        return -1;
    b->buf[0] = '\0';
    return 0;
}

int sb_add(SBuf *b, const char *s)
{
    size_t sl = strlen(s);
    if (b->len + sl + 1 > b->cap) {
        while (b->len + sl + 1 > b->cap)
            b->cap *= 2;
        char *nb = realloc(b->buf, b->cap);
        if (!nb)
            return -1;
        b->buf = nb;
    }
    memcpy(b->buf + b->len, s, sl);
    b->len += sl;
    b->buf[b->len] = '\0';
    return 0;
}

void sb_free(SBuf *b)
{
    free(b->buf);
    b->buf = NULL;
    b->len = b->cap = 0;
}

int strlist_push(StrList *l, const char *s)
{
    for (size_t i = 0; i < l->count; i++)
        if (strcmp(l->items[i], s) == 0)
            return 0;
    return strlist_push_force(l, s);
}

int strlist_push_force(StrList *l, const char *s)
{
    if (l->count == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 8;
        char **ni = realloc(l->items, nc * sizeof(char *));
        if (!ni)
            return 0;
        l->items = ni;
        l->cap = nc;
    }
    char *c = strdup(s);
    if (!c)
        return 0;
    l->items[l->count++] = c;
    return 1;
}

void strlist_free(StrList *l)
{
    for (size_t i = 0; i < l->count; i++)
        free(l->items[i]);
    free(l->items);
    l->items = NULL;
    l->count = l->cap = 0;
}

void graph_init(Graph *g)
{
    memset(g, 0, sizeof(*g));
    g_errors = 0;
}

void graph_free(Graph *g)
{
    for (size_t i = 0; i < g->count; i++) {
        Target *t = &g->targets[i];
        strlist_free(&t->sources);
        free(t->source_kinds);
        t->source_kinds = NULL;
        strlist_free(&t->cflags);
        strlist_free(&t->asflags);
        strlist_free(&t->ldflags);
        strlist_free(&t->includes);
        strlist_free(&t->order_deps);
    }
    free(g->targets);
    g->targets = NULL;
    g->count = g->cap = 0;
    free(g->commands);
    g->commands = NULL;
    g->cmd_count = g->cmd_cap = 0;
}

Target *graph_find(Graph *g, const char *name)
{
    for (size_t i = 0; i < g->count; i++)
        if (strcmp(g->targets[i].name, name) == 0)
            return &g->targets[i];
    return NULL;
}

Target *graph_add(Graph *g, const char *name, int type)
{
    if (g->count == g->cap) {
        size_t nc = g->cap ? g->cap * 2 : 4;
        Target *nt = realloc(g->targets, nc * sizeof(Target));
        if (!nt)
            return NULL;
        g->targets = nt;
        g->cap = nc;
    }
    Target *t = &g->targets[g->count++];
    memset(t, 0, sizeof(*t));
    snprintf(t->name, sizeof(t->name), "%s", name);
    t->type = type;
    return t;
}

int graph_add_command(Graph *g, const char *out, const char *cmd,
                      const char *in)
{
    if (g->cmd_count == g->cmd_cap) {
        size_t nc = g->cmd_cap ? g->cmd_cap * 2 : 4;
        Command *nc_arr = realloc(g->commands, nc * sizeof(Command));
        if (!nc_arr)
            return 0;
        g->commands = nc_arr;
        g->cmd_cap = nc;
    }
    Command *c = &g->commands[g->cmd_count++];
    memset(c, 0, sizeof(*c));
    snprintf(c->output, sizeof(c->output), "%s", out);
    snprintf(c->command, sizeof(c->command), "%s", cmd);
    if (in)
        snprintf(c->input, sizeof(c->input), "%s", in);
    return 1;
}

void join_path(char *out, size_t n, const char *dir,
               const char *name)
{
    if (!dir || !*dir || strcmp(dir, ".") == 0)
        snprintf(out, n, "%s", name);
    else if (dir[strlen(dir) - 1] == '/')
        snprintf(out, n, "%s%s", dir, name);
    else
        snprintf(out, n, "%s/%s", dir, name);
}

void dir_prefix(const char *path, char *prefix, size_t n)
{
    const char *slash = strrchr(path, '/');
    if (!slash) {
        prefix[0] = '\0';
        return;
    }
    size_t len = (size_t)(slash - path + 1);
    if (len >= n)
        len = n - 1;
    memcpy(prefix, path, len);
    prefix[len] = '\0';
}

int makedirs(const char *dir)
{
    char tmp[4096];
    size_t l = strlen(dir);
    if (l >= sizeof(tmp)) {
        diag("directory path too long: %s", dir);
        return -1;
    }
    memcpy(tmp, dir, l + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                diag("cannot create directory %s: %s", tmp,
                     strerror(errno));
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        diag("cannot create directory %s: %s", tmp, strerror(errno));
        return -1;
    }
    return 0;
}

int has_whitespace(const char *s)
{
    if (!s)
        return 0;
    for (const char *p = s; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            return 1;
    }
    return 0;
}

SourceKind classify_source(const char *path)
{
    size_t l = strlen(path);
    if (l >= 2 && path[l - 2] == '.') {
        if (path[l - 1] == 'c') return SRC_C;
        if (path[l - 1] == 's') return SRC_ASM_RAW;
        if (path[l - 1] == 'S') return SRC_ASM_CPP;
    }
    return SRC_C;
}

int is_c_source(const char *s)
{
    size_t l = strlen(s);
    return l >= 2 && s[l - 2] == '.' && s[l - 1] == 'c';
}

int is_asm_source(const char *s)
{
    size_t l = strlen(s);
    return l >= 2 && s[l - 2] == '.' && (s[l - 1] == 's' || s[l - 1] == 'S');
}

int is_buildable_source(const char *s)
{
    return is_c_source(s) || is_asm_source(s);
}

const char *type_name(int type)
{
    switch (type) {
    case THORN_STATIC_LIB:  return "static library";
    case THORN_SHARED_LIB: return "shared library";
    default:                return "executable";
    }
}

const char *type_const(int type)
{
    switch (type) {
    case THORN_STATIC_LIB:  return "static_lib";
    case THORN_SHARED_LIB: return "shared_lib";
    default:                return "exe";
    }
}

void obj_name(const Target *t, const char *src, char *out,
              size_t n)
{
    char tmp[512];
    size_t o = 0;
    for (const char *p = src; *p && o + 1 < sizeof(tmp); p++)
        tmp[o++] = (*p == '/') ? '_' : *p;
    tmp[o] = '\0';
    if (o >= 2 && tmp[o - 2] == '.' &&
        (tmp[o - 1] == 'c' || tmp[o - 1] == 's' || tmp[o - 1] == 'S'))
        tmp[o - 1] = 'o';
    snprintf(out, n, "%s__%s", t->name, tmp);
}

int flags_text(const Target *t, SBuf *b)
{
    if (sb_init(b) != 0)
        return -1;
    for (size_t i = 0; i < t->includes.count; i++) {
        sb_add(b, "-I");
        sb_add(b, t->includes.items[i]);
        if (i + 1 < t->includes.count || t->cflags.count)
            sb_add(b, " ");
    }
    for (size_t i = 0; i < t->cflags.count; i++) {
        sb_add(b, t->cflags.items[i]);
        if (i + 1 < t->cflags.count)
            sb_add(b, " ");
    }
    return 0;
}

int asflags_text(const Target *t, SBuf *b)
{
    if (sb_init(b) != 0)
        return -1;
    for (size_t i = 0; i < t->asflags.count; i++) {
        sb_add(b, t->asflags.items[i]);
        if (i + 1 < t->asflags.count)
            sb_add(b, " ");
    }
    return 0;
}

int ldflags_text(const Target *t, SBuf *b)
{
    if (sb_init(b) != 0)
        return -1;
    for (size_t i = 0; i < t->ldflags.count; i++) {
        sb_add(b, t->ldflags.items[i]);
        if (i + 1 < t->ldflags.count)
            sb_add(b, " ");
    }
    return 0;
}

void out_name(char *out, size_t n, const char *pfx,
              const char *name)
{
    snprintf(out, n, "%s%s", pfx, name);
}

void artifact_name(const Target *t, char *out, size_t n)
{
    if (t->type == THORN_STATIC_LIB) {
        size_t l = strlen(t->name);
        if (l >= 2 && strcmp(t->name + l - 2, ".a") == 0)
            snprintf(out, n, "%s", t->name);
        else
            snprintf(out, n, "%s.a", t->name);
    }
    else if (t->type == THORN_SHARED_LIB)
        snprintf(out, n, "%s.so", t->name);
    else
        snprintf(out, n, "%s", t->name);
}
