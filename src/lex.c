#include "include/lex.h"

int pat_match(const char *pat, const char *str)
{
    if (!pat || !str)
        return 0;
    if (strcmp(pat, "*") == 0)
        return 1;
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat)
                return 1;
            while (*str) {
                if (pat_match(pat, str))
                    return 1;
                str++;
            }
            return 0;
        }
        if (*pat != '?' && *pat != *str)
            return 0;
        pat++;
        str++;
    }
    return *str == '\0';
}

int matches_any(const StrList *list, const char *str)
{
    if (!list || list->count == 0 || !str)
        return 0;
    for (size_t i = 0; i < list->count; i++) {
        if (pat_match(list->items[i], str))
            return 1;
    }
    return 0;
}

void note_add(char ***notes, size_t *n, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    char **ng = realloc(*notes, (*n + 1) * sizeof(char *));
    if (!ng)
        return;
    *notes = ng;
    char *d = strdup(buf);
    if (d)
        (*notes)[(*n)++] = d;
}

int tok_push(char ***arr, size_t *n, const char *tok)
{
    char **na = realloc(*arr, (*n + 1) * sizeof(char *));
    if (!na)
        return 0;
    *arr = na;
    char *d = strdup(tok);
    if (!d)
        return 0;
    (*arr)[(*n)++] = d;
    return 1;
}

char **split_owned(const char *text, size_t *count)
{
    char **out = NULL;
    size_t n = 0, cap = 0;
    const char *p = text;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (!*p)
            break;
        const char *st = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
            p++;
        char *tok = strndup(st, (size_t)(p - st));
        if (!tok)
            break;
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            char **ni = realloc(out, cap * sizeof(char *));
            if (!ni) {
                free(tok);
                break;
            }
            out = ni;
        }
        out[n++] = tok;
    }
    *count = n;
    return out;
}

void free_tokens(char **toks, size_t n)
{
    for (size_t i = 0; i < n; i++)
        free(toks[i]);
    free(toks);
}

char *trim_inplace(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    size_t l = strlen(s);
    while (l && (s[l - 1] == ' ' || s[l - 1] == '\t' ||
                 s[l - 1] == '\r' || s[l - 1] == '\n'))
        s[--l] = '\0';
    return s;
}

int ends_with(const char *s, const char *suffix)
{
    size_t sl = strlen(s), fl = strlen(suffix);
    return sl >= fl && strcmp(s + sl - fl, suffix) == 0;
}

char *read_all(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) {
        fclose(fp);
        return NULL;
    }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    buf[sz] = '\0';
    return buf;
}

int is_cc_token(const char *tok)
{
    if (tok[0] == '$')
        tok++;
    static const char *names[] = {
        "cc", "gcc", "clang", "clang++", "g++", "c++", "ld", NULL
    };
    for (int i = 0; names[i]; i++) {
        if (strcmp(tok, names[i]) == 0)
            return 1;
        size_t nl = strlen(names[i]);
        size_t tl = strlen(tok);
        if (tl > nl + 1 && tok[tl - nl - 1] == '/' &&
            strcmp(tok + tl - nl, names[i]) == 0)
            return 1;
    }
    return 0;
}

int is_as_token(const char *tok)
{
    if (tok[0] == '$')
        tok++;
    static const char *names[] = {
        "as", "gas", NULL
    };
    for (int i = 0; names[i]; i++) {
        if (strcmp(tok, names[i]) == 0)
            return 1;
        size_t nl = strlen(names[i]);
        size_t tl = strlen(tok);
        if (tl > nl + 1 && tok[tl - nl - 1] == '/' &&
            strcmp(tok + tl - nl, names[i]) == 0)
            return 1;
    }
    return 0;
}

int is_source_ext(const char *path)
{
    size_t l = strlen(path);
    if (l >= 2 && path[l - 2] == '.') {
        char ext = path[l - 1];
        return (ext == 'c' || ext == 's' || ext == 'S');
    }
    return 0;
}

char *swap_ext_source(const char *path)
{
    size_t l = strlen(path);
    char *c = strdup(path);
    if (!c)
        return NULL;
    if (l >= 2 && c[l - 2] == '.') {
        c[l - 1] = 'c';
        if (access(c, F_OK) == 0)
            return c;
        c[l - 1] = 'S';
        if (access(c, F_OK) == 0)
            return c;
        c[l - 1] = 's';
        if (access(c, F_OK) == 0)
            return c;
        c[l - 1] = 'c';
    }
    return c;
}

char *prefix_scoped_path(const char *path, const char *scope)
{
    if (!scope || !*scope || strcmp(scope, ".") == 0 || strcmp(scope, "./") == 0)
        return strdup(path);
    if (!path || !*path || path[0] == '/')
        return strdup(path);

    size_t slen = strlen(scope);
    int scope_has_slash = (scope[slen - 1] == '/');
    size_t pfx_len = scope_has_slash ? slen : slen + 1;

    if (strncmp(path, scope, slen) == 0) {
        if (scope_has_slash || path[slen] == '/')
            return strdup(path);
    }

    size_t total = pfx_len + strlen(path) + 1;
    char *out = malloc(total);
    if (!out)
        return strdup(path);
    if (scope_has_slash)
        snprintf(out, total, "%s%s", scope, path);
    else
        snprintf(out, total, "%s/%s", scope, path);
    return out;
}

const char *strip_pfx(const char *s, const char *pfx)
{
    if (pfx && *pfx) {
        size_t l = strlen(pfx);
        if (strncmp(s, pfx, l) == 0)
            return s + l;
    }
    return s;
}

const char *clean_target(const char *raw, const char *pfx,
                         char *buf, size_t n)
{
    const char *s = strip_pfx(raw, pfx);
    snprintf(buf, n, "%s", s);
    size_t l = strlen(buf);
    if (l > 3 && ends_with(buf, ".so"))
        buf[l - 3] = '\0';
    else if (l > 2 && ends_with(buf, ".a"))
        buf[l - 2] = '\0';
    return buf;
}

int classify_tokens(char **toks, size_t n)
{
    int has_c = 0, has_rcs = 0, has_shared = 0, ccish = 0, asish = 0;
    for (size_t i = 0; i < n; i++) {
        const char *t = toks[i];
        if (strcmp(t, "-c") == 0)
            has_c = 1;
        else if (strcmp(t, "rcs") == 0)
            has_rcs = 1;
        else if (strcmp(t, "-shared") == 0)
            has_shared = 1;
        else if (is_cc_token(t))
            ccish = 1;
        else if (is_as_token(t))
            asish = 1;
    }
    if (has_rcs)
        return RC_AR;
    if (has_c && (ccish || asish))
        return RC_COMPILE;
    if (asish)
        return RC_COMPILE;
    if (has_shared && ccish)
        return RC_SHARED;
    if (ccish)
        return RC_LINK;
    return RC_OTHER;
}

int classify_text(const char *text)
{
    size_t nt = 0;
    char **toks = split_owned(text, &nt);
    int rc = classify_tokens(toks, nt);
    free_tokens(toks, nt);
    return rc;
}

void harvest_flags_text(const char *text, Target *t)
{
    size_t nt;
    char **toks = split_owned(text, &nt);
    for (size_t i = 0; i < nt; i++) {
        const char *tok = toks[i];
        if (strncmp(tok, "-I", 2) == 0 && tok[2])
            strlist_push(&t->includes, tok + 2);
        else if (strcmp(tok, "-I") == 0 && i + 1 < nt)
            strlist_push(&t->includes, toks[++i]);
        else
            strlist_push(&t->cflags, tok);
    }
    free_tokens(toks, nt);
}

void harvest_asflags_text(const char *text, Target *t)
{
    size_t nt;
    char **toks = split_owned(text, &nt);
    for (size_t i = 0; i < nt; i++) {
        const char *tok = toks[i];
        if (strncmp(tok, "-I", 2) == 0 && tok[2])
            strlist_push(&t->includes, tok + 2);
        else if (strcmp(tok, "-I") == 0 && i + 1 < nt)
            strlist_push(&t->includes, toks[++i]);
        else
            strlist_push(&t->asflags, tok);
    }
    free_tokens(toks, nt);
}

void harvest_cmd_compile_text(const char *text, Target *t)
{
    size_t nt;
    char **toks = split_owned(text, &nt);
    for (size_t i = 0; i < nt; i++) {
        const char *tok = toks[i];
        if (tok[0] == '$')
            continue;
        if (strcmp(tok, "-c") == 0 || strcmp(tok, "-MD") == 0 ||
            strcmp(tok, "-MMD") == 0 || strcmp(tok, "-MP") == 0)
            continue;
        if (strcmp(tok, "-MF") == 0 || strcmp(tok, "-o") == 0) {
            i++;
            continue;
        }
        if (ends_with(tok, ".c") || ends_with(tok, ".s") ||
            ends_with(tok, ".S") || ends_with(tok, ".o") || ends_with(tok, ".d"))
            continue;
        if (strncmp(tok, "-I", 2) == 0 && tok[2]) {
            strlist_push(&t->includes, tok + 2);
            continue;
        }
        if (strcmp(tok, "-I") == 0 && i + 1 < nt) {
            strlist_push(&t->includes, toks[++i]);
            continue;
        }
        if (tok[0] == '-')
            strlist_push(&t->cflags, tok);
    }
    free_tokens(toks, nt);
}

void harvest_cmd_link_text(const char *text, Target *t)
{
    size_t nt;
    char **toks = split_owned(text, &nt);
    for (size_t i = 0; i < nt; i++) {
        const char *tok = toks[i];
        if (tok[0] == '$')
            continue;
        if (strcmp(tok, "-o") == 0) {
            i++;
            continue;
        }
        if (strcmp(tok, "-shared") == 0 || strcmp(tok, "-c") == 0)
            continue;
        if (ends_with(tok, ".o") || ends_with(tok, ".d"))
            continue;
        if (tok[0] == '-' || ends_with(tok, ".a") ||
            ends_with(tok, ".so"))
            strlist_push(&t->ldflags, tok);
    }
    free_tokens(toks, nt);
}
