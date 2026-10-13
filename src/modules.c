/* realpath() is an XSI extension. */
#define _XOPEN_SOURCE 700

#include "modules.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "parser.h"
#include "prelude.h"
#include "vec.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool dir_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* A canonical path, shown relative to the current directory when it's
 * inside it (so messages say std/str.watr, not /home/.../std/str.watr). */
static const char *display_path(Arena *a, const char *path) {
    char real[PATH_MAX], cwd[PATH_MAX];
    if (!realpath(path, real)) return path;
    if (getcwd(cwd, sizeof cwd)) {
        size_t n = strlen(cwd);
        if (strncmp(real, cwd, n) == 0 && real[n] == '/') return arena_strndup(a, real + n + 1, strlen(real + n + 1));
    }
    return arena_strndup(a, real, strlen(real));
}

/* The standard library: $WATR_STD, or std/ next to (or one level above)
 * the directory holding the watrc binary. */
static const char *std_dir(Arena *a) {
    const char *env = getenv("WATR_STD");
    if (env && env[0]) return display_path(a, env);
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return "std";
    exe[n] = '\0';
    char *slash = strrchr(exe, '/');
    if (slash) *slash = '\0';
    char candidate[PATH_MAX + 16];
    snprintf(candidate, sizeof candidate, "%s/std", exe);
    if (dir_exists(candidate)) return display_path(a, candidate);
    snprintf(candidate, sizeof candidate, "%s/../std", exe);
    if (dir_exists(candidate)) return display_path(a, candidate);
    return "std";
}

static char *join(Arena *a, const char *dir, const char *rel, size_t rel_len) {
    size_t n = strlen(dir) + rel_len + 8;
    char *out = arena_alloc(a, n);
    snprintf(out, n, "%s/%.*s", dir, (int)rel_len, rel);
    size_t len = strlen(out);
    if (len < 5 || strcmp(out + len - 5, ".watr") != 0) strcat(out, ".watr");
    return out;
}

static char *dir_of(Arena *a, const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return ".";
    return arena_strndup(a, path, (size_t)(slash - path));
}

/* "std/io" -> "io"; "../lib/geo-utils" -> "geo_utils" */
static const char *alias_of(Arena *a, const char *path, size_t len) {
    const char *start = path;
    for (size_t i = 0; i < len; i++)
        if (path[i] == '/') start = path + i + 1;
    size_t n = (size_t)(path + len - start);
    if (n > 5 && strncmp(start + n - 5, ".watr", 5) == 0) n -= 5;
    char *alias = arena_strndup(a, start, n);
    for (char *c = alias; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_')) *c = '_';
    return alias;
}

/* A symbol prefix from the import path: "std/io" -> "std.io." */
static const char *prefix_of(Arena *a, const char *path, size_t len) {
    if (len > 5 && strncmp(path + len - 5, ".watr", 5) == 0) len -= 5;
    char *p = arena_alloc(a, len + 2);
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        char c = path[i];
        if (c == '/') c = '.';
        else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) c = '_';
        if (n == 0 && c == '.') continue;   /* "./x" or "/x" */
        p[n++] = c;
    }
    p[n++] = '.';
    p[n] = '\0';
    return p;
}

typedef struct {
    Arena *arena;
    PtrVec mods;       /* Module* */
    PtrVec real;       /* char*: realpath of each module, for loading each file once */
    const char *std;
    bool ok;
} Loader;

static int find_loaded(Loader *L, const char *real) {
    for (size_t i = 0; i < L->real.len; i++)
        if (strcmp(L->real.items[i], real) == 0) return (int)i;
    return -1;
}

static Module *parse_path(Loader *L, const char *path) {
    SrcFile *f = src_load(L->arena, path);
    if (!f) return NULL;
    return parse_file(L->arena, f);
}

/* Loads `path` (already resolved) unless it's loaded; returns its index. */
static int load(Loader *L, const char *path, const char *alias, const char *prefix) {
    char real[PATH_MAX];
    if (!realpath(path, real)) return -1;
    int existing = find_loaded(L, real);
    if (existing >= 0) return existing;

    Module *m = parse_path(L, path);
    if (!m) return -1;
    m->alias = alias;
    m->prefix = prefix;
    int index = (int)L->mods.len;
    ptrvec_push(&L->mods, m);
    ptrvec_push(&L->real, arena_strndup(L->arena, real, strlen(real)));
    return index;
}

static void resolve_imports(Loader *L, int index) {
    Module *m = L->mods.items[index];
    const char *dir = dir_of(L->arena, m->file->path);
    int *imports = arena_alloc(L->arena, (m->n ? m->n : 1) * sizeof(int));
    const char **aliases = arena_alloc(L->arena, (m->n ? m->n : 1) * sizeof(char *));
    size_t nimports = 0;

    for (size_t i = 0; i < m->n; i++) {
        Decl *d = m->decls[i];
        if (d->kind != DECL_IMPORT) continue;
        const char *path = d->u.import.path.data;
        size_t len = d->u.import.path.len;
        bool is_std = len > 4 && strncmp(path, "std/", 4) == 0;
        char *resolved = is_std ? join(L->arena, L->std, path + 4, len - 4) : join(L->arena, dir, path, len);

        if (!file_exists(resolved)) {
            Diag *e = diag_start(DIAG_ERROR, "E0342", d->loc, "can't find module `%.*s`", (int)len, path);
            diag_label(e, "no file at %s", resolved);
            if (is_std) diag_note(e, "the standard library is in %s (set WATR_STD to change it)", L->std);
            else diag_note(e, "import paths are relative to the importing file, without the .watr extension");
            diag_end(e);
            L->ok = false;
            d->u.import.module = -1;
            continue;
        }
        const char *alias = d->name ? d->name : alias_of(L->arena, path, len);
        int target = load(L, resolved, alias, prefix_of(L->arena, path, len));
        d->u.import.module = target;
        if (target < 0) {
            L->ok = false;
            continue;
        }
        if (target == index) {
            Diag *e = diag_start(DIAG_ERROR, "E0343", d->loc, "a module can't import itself");
            diag_label(e, "this is the current file");
            diag_end(e);
            L->ok = false;
            continue;
        }
        /* Two imports under the same name would make `alias::x` ambiguous. */
        for (size_t j = 0; j < i; j++) {
            Decl *prev = m->decls[j];
            if (prev->kind != DECL_IMPORT || prev->u.import.module < 0 || prev->u.import.module == target) continue;
            const char *prev_alias = prev->name ? prev->name : alias_of(L->arena, prev->u.import.path.data,
                                                                          prev->u.import.path.len);
            if (strcmp(prev_alias, alias) == 0) {
                Diag *e = diag_start(DIAG_ERROR, "E0343", d->loc, "two imports are both named `%s`", alias);
                diag_label(e, "second `%s`", alias);
                diag_span(e, prev->loc, "first `%s`", alias);
                diag_help(e, "rename one: `import \"%.*s\" as other_name;`", (int)len, path);
                diag_end(e);
                L->ok = false;
            }
        }
        bool dup = false;
        for (size_t j = 0; j < nimports; j++) dup |= imports[j] == target;
        if (!dup) {
            aliases[nimports] = alias;
            imports[nimports++] = target;
        }
    }
    m->imports = imports;
    m->import_aliases = aliases;
    m->nimports = nimports;
}

bool load_program(Arena *arena, const char *root_path, ModuleSet *out) {
    Loader L = {arena, {0}, {0}, std_dir(arena), true};
    int root = load(&L, root_path, alias_of(arena, root_path, strlen(root_path)), "");
    if (root < 0) {
        free(L.mods.items);
        free(L.real.items);
        return false;
    }
    /* New modules are appended as they're found, so this visits them all. */
    for (size_t i = 0; i < L.mods.len; i++) resolve_imports(&L, (int)i);

    SrcFile *rt = ARENA_NEW(arena, SrcFile);
    rt->path = WATR_PRELUDE_PATH;
    rt->text = watr_prelude_source;
    rt->len = strlen(watr_prelude_source);
    Module *rtm = parse_file(arena, rt);
    rtm->alias = "__runtime";
    rtm->prefix = "";
    rtm->is_runtime = true;
    ptrvec_push(&L.mods, rtm);

    out->mods = (Module **)ptrvec_finish(&L.mods, arena, &out->n);
    free(L.real.items);
    return L.ok && diag_error_count() == 0;
}

Module *load_root_only(Arena *arena, const char *path) {
    SrcFile *f = src_load(arena, path);
    return f ? parse_file(arena, f) : NULL;
}
