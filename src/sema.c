#include "sema.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "clone.h"
#include "target.h"
#include "vec.h"

/*
 * Checking is bidirectional: check() takes an optional `hint`, the type
 * the context expects, so literals can adapt (`u8 x = 200;` types 200 as
 * u8) and arithmetic picks up the right width. coerce() then verifies the
 * result fits where it's going, inserting conversions that need code
 * (only f32 -> f64; integer widening is free because values are kept
 * sign/zero-extended to 64 bits).
 */

typedef struct {
    Arena *arena;
    Module *mod;        /* every module's declarations, flattened */
    Module **mods;      /* mods[0] is the main file; the runtime is last */
    size_t nmods;
    int cur;            /* the module whose code is being checked */
    PtrVec funcs;       /* Symbol* */
    PtrVec globals;     /* Symbol* */
    PtrVec structs;     /* Type*: structs and enums */
    PtrVec enum_fns;    /* Symbol*: generated enum-name functions */

    /* per function */
    Symbol *fn;
    PtrVec locals;      /* Symbol*, innermost last */
    size_t *marks;
    size_t nmarks, marks_cap;
    int loop_depth;
    bool in_defer;      /* checking the body of a `defer` */
    int defer_loop_depth;   /* loop_depth where that body starts */
    int fn_errors_before;
    bool needs_libc;
    int temps;          /* numbering for names sema invents (foreach) */
    bool freestanding;  /* --freestanding: no heap, no libc, no main */
    PtrVec vsites;      /* VSite*: virtual calls, for reachability */
    Loc cur_loc;        /* the expression being checked, for the call graph */
    uint32_t classes;   /* class ids handed out */
    PtrVec templates;   /* Decl*: generic functions, structs and classes */
    PtrVec instances;   /* Instance* */
    int phase;          /* PH_*: the declaration passes finished so far */
    int inst_depth;     /* nested instantiations, to stop runaway recursion */
} Sema;

/* Declaration passes, in order. A generic type instantiated late catches
 * up on the passes already finished. */
enum { PH_TYPES, PH_STRUCTS, PH_HIERARCHY, PH_CLASS_LAYOUT, PH_METHODS, PH_DEFAULTS, PH_INHERIT };

/* One use of a generic declaration with particular type arguments. */
typedef struct {
    const Decl *tmpl;
    Type **targs;
    Type *type;         /* structs and classes */
    Symbol *fn;         /* functions */
} Instance;

/* A virtual call: fn calls `name` on a receiver of static type `t`. */
typedef struct {
    Symbol *fn;
    Type *t;
    const char *name;
} VSite;

typedef struct {
    uint64_t bits;      /* ints (normalized), bools, pointers */
    double f;           /* floats */
} ConstVal;

static Type *check(Sema *S, Expr **slot, Type *hint);
static void check_effect(Sema *S, Expr **slot);
static bool both_values(Sema *S, const Expr *a, const Expr *b);
static bool require_value(Sema *S, const Expr *e);
static bool const_eval(Sema *S, const Expr *e, ConstVal *out, bool report);
static bool coerce(Sema *S, Expr **slot, Type *to, const Loc *why, const char *why_text);
static Type *check_coalesce(Sema *S, Expr *e, Type *hint);
static const char *closest(const char *name, const char **candidates, size_t n);
static Expr *to_mixed(Sema *S, Expr *e);
static Expr *desc_addr(Sema *S, Type *ct, Loc loc);
static Expr *bool_const(Sema *S, Loc loc, bool v);
static Type *resolve_generic(Sema *S, const char *name, TypeExpr *const *targs, size_t n, Loc loc);
static void report_missing_targs(Sema *S, Loc loc, const char *name);
static void check_args_only(Sema *S, Expr *e);
static bool is_literal(const Expr *e);

/* ---- small helpers ------------------------------------------------------- */

static void not_yet(Loc loc, const char *what, int milestone) {
    Diag *d = diag_start(DIAG_ERROR, "E0900", loc, "not supported yet: %s", what);
    diag_label(d, "planned for milestone %d", milestone);
    diag_note(d, "watrlang is still being built; docs/DESIGN.md has the roadmap");
    diag_end(d);
}

static Type *set_type(Expr *e, Type *t) {
    e->type = t;
    return t;
}

static bool is_err(const Type *t) {
    return t->kind == TY_ERROR;
}

/* Source text of `loc` if it's on one line, else NULL. */
static char *loc_text(Sema *S, Loc loc) {
    if (!loc.file || loc.line == 0) return NULL;
    const char *s = loc.file->text + loc.offset;
    if (memchr(s, '\n', loc.len)) return NULL;
    return arena_strndup(S->arena, s, loc.len);
}

/* Does `e` need parentheses before ` as T` or ` != 0` is appended? */
static bool is_simple_expr(const Expr *e) {
    if (e->parenthesized) return true;
    switch (e->kind) {
    case EXPR_INT: case EXPR_FLOAT: case EXPR_CHAR: case EXPR_BOOL: case EXPR_NULL: case EXPR_IDENT:
    case EXPR_CALL: case EXPR_INDEX: case EXPR_MEMBER:
        return true;
    default:
        return false;
    }
}

/* Suggests replacing `e` with `<e> <suffix>`, parenthesizing if needed. */
static void fix_append(Sema *S, Diag *d, const Expr *e, const char *suffix, const char *msg) {
    char *text = loc_text(S, e->loc);
    if (!text) {
        diag_help(d, "%s", msg);
        return;
    }
    size_t n = strlen(text) + strlen(suffix) + 4;
    char *fixed = arena_alloc(S->arena, n);
    if (is_simple_expr(e)) snprintf(fixed, n, "%s%s", text, suffix);
    else snprintf(fixed, n, "(%s)%s", text, suffix);
    diag_fix(d, e->loc, fixed, "%s", msg);
}

static Loc ret_type_loc(const Decl *fn) {
    return fn->u.func.ret ? fn->u.func.ret->loc : fn->name_loc;
}

static bool const_keyword_loc(const TypeExpr *t, Loc *out) {
    if (!t || !t->is_const || !t->loc.file) return false;
    const char *src = t->loc.file->text + t->loc.offset;
    if (strncmp(src, "const", 5) != 0) return false;
    uint32_t len = 5;
    while (src[len] == ' ') len++;
    *out = t->loc;
    out->len = len;
    return true;
}

/* ---- symbols and scopes ---------------------------------------------------- */

static Symbol *new_symbol(Sema *S, SymKind kind, const char *name, Loc loc) {
    Symbol *s = ARENA_NEW(S->arena, Symbol);
    s->kind = kind;
    s->name = name;
    s->loc = loc;
    s->type = ty_error;
    s->module = S->cur;
    return s;
}

/* ---- modules ------------------------------------------------------------------ */

static int module_of(Sema *S, const SrcFile *f) {
    for (size_t i = 0; i < S->nmods; i++)
        if (S->mods[i]->file == f) return (int)i;
    return 0;
}

static bool imports(Sema *S, int from, int mod) {
    const Module *m = S->mods[from];
    for (size_t i = 0; i < m->nimports; i++)
        if (m->imports[i] == mod) return true;
    return false;
}

/* Can code in module `from` see a declaration of module `mod`? Its own
 * declarations, the runtime's, and non-private ones of modules it imports. */
static bool visible(Sema *S, int from, int mod, bool is_private) {
    if (mod == from || S->mods[mod]->is_runtime) return true;
    return !is_private && imports(S, from, mod);
}

/* The module an alias (`geo` in geo::area) names from the current module, or -1. */
static int module_by_alias(Sema *S, const char *alias) {
    const Module *m = S->mods[S->cur];
    for (size_t i = 0; i < m->nimports; i++)
        if (strcmp(m->import_aliases[i], alias) == 0) return m->imports[i];
    return -1;
}

/* Finds a function or global by name, as seen from the current module: a
 * definition in the module itself wins; otherwise the visible definitions
 * of imported modules. If two imports define it, *other gets the second. */
static Symbol *find_in(Sema *S, PtrVec *v, const char *name, Symbol **other) {
    if (other) *other = NULL;
    for (size_t i = 0; i < v->len; i++) {
        Symbol *s = v->items[i];
        if (s->module == S->cur && !s->owner && strcmp(s->name, name) == 0) return s;
    }
    Symbol *found = NULL;
    for (size_t i = 0; i < v->len; i++) {
        Symbol *s = v->items[i];
        if (s->module == S->cur || s->owner || strcmp(s->name, name) != 0) continue;
        if (!visible(S, S->cur, s->module, s->is_private)) continue;
        if (!found) found = s;
        else if (other && !*other) *other = s;
    }
    return found;
}

/* A public function or global of a specific module. */
static Symbol *find_in_module(PtrVec *v, int mod, const char *name) {
    for (size_t i = 0; i < v->len; i++) {
        Symbol *s = v->items[i];
        if (s->module == mod && !s->owner && strcmp(s->name, name) == 0) return s;
    }
    return NULL;
}

/* E0341 */
static void report_ambiguous(Sema *S, Loc loc, const char *name, const Symbol *a, const Symbol *b) {
    Diag *d = diag_start(DIAG_ERROR, "E0341", loc, "`%s` is ambiguous", name);
    diag_label(d, "defined by more than one imported module");
    diag_span(d, a->loc, "one `%s` is here, in `%s`", name, S->mods[a->module]->file->path);
    diag_span(d, b->loc, "another is here, in `%s`", S->mods[b->module]->file->path);
    const Module *m = S->mods[S->cur];
    const char *alias_a = NULL, *alias_b = NULL;
    for (size_t i = 0; i < m->nimports; i++) {
        if (m->imports[i] == a->module) alias_a = m->import_aliases[i];
        if (m->imports[i] == b->module) alias_b = m->import_aliases[i];
    }
    if (alias_a && alias_b) diag_help(d, "say which one: `%s::%s` or `%s::%s`", alias_a, name, alias_b, name);
    diag_end(d);
}

/* E0344 */
static void report_unknown_module(Sema *S, Loc loc, const char *alias) {
    loc.len = (uint32_t)strlen(alias);   /* just the module part of `alias::name` */
    if (alias[0] >= 'A' && alias[0] <= 'Z') {   /* probably meant an enum */
        Diag *d = diag_start(DIAG_ERROR, "E0344", loc, "cannot find an enum or module named `%s`", alias);
        diag_label(d, "not found");
        const char *names[64];
        size_t n = 0;
        for (size_t i = 0; i < S->structs.len && n < 64; i++) {
            Type *t = S->structs.items[i];
            if (t->kind == TY_ENUM) names[n++] = t->name;
        }
        const char *guess = closest(alias, names, n);
        if (guess) diag_fix(d, loc, guess, "an enum with a similar name exists");
        else diag_help(d, "declare it: `enum %s { ... }`", alias);
        diag_end(d);
        return;
    }
    Diag *d = diag_start(DIAG_ERROR, "E0344", loc, "`%s` isn't a module imported here", alias);
    diag_label(d, "unknown module");
    const Module *m = S->mods[S->cur];
    const char *names[64];
    size_t n = 0;
    for (size_t i = 0; i < m->nimports && n < 64; i++) names[n++] = m->import_aliases[i];
    const char *guess = NULL;
    size_t best = (size_t)-1;
    for (size_t i = 0; i < n; i++) {
        size_t dist = diag_edit_distance(alias, names[i]);
        if (diag_similar(alias, names[i]) && dist < best) {
            best = dist;
            guess = names[i];
        }
    }
    if (guess) diag_fix(d, loc, guess, "a module with a similar name is imported");
    else diag_help(d, "import it first, like `import \"std/%s\";`", alias);
    diag_end(d);
}

/* `alias::name` for functions (want_func) or globals. */
static Symbol *find_qualified(Sema *S, PtrVec *v, Loc loc, const char *alias, const char *name, const char *what) {
    int mod = module_by_alias(S, alias);
    if (mod < 0) {
        report_unknown_module(S, loc, alias);
        return NULL;
    }
    Symbol *s = find_in_module(v, mod, name);
    if (s && !s->is_private) return s;
    Diag *d;
    if (s) {
        d = diag_start(DIAG_ERROR, "E0345", loc, "`%s::%s` is private", alias, name);
        diag_label(d, "not visible outside `%s`", S->mods[mod]->file->path);
        diag_span(d, s->loc, "declared `private` here");
    } else {
        d = diag_start(DIAG_ERROR, "E0345", loc, "module `%s` has no %s named `%s`", alias, what, name);
        diag_label(d, "not found in `%s`", S->mods[mod]->file->path);
    }
    diag_end(d);
    return NULL;
}

static Symbol *find_local(Sema *S, const char *name) {
    for (size_t i = S->locals.len; i-- > 0;) {
        Symbol *s = S->locals.items[i];
        if (strcmp(s->name, name) == 0) return s;
    }
    return NULL;
}

static Symbol *find_var(Sema *S, const char *name) {
    Symbol *s = find_local(S, name);
    return s ? s : find_in(S, &S->globals, name, NULL);
}

static Symbol *find_func(Sema *S, const char *name) {
    return find_in(S, &S->funcs, name, NULL);
}

/* A runtime prelude function (never shadowed by user code). */
static Symbol *find_runtime_func(Sema *S, const char *name) {
    for (size_t i = 0; i < S->funcs.len; i++) {
        Symbol *s = S->funcs.items[i];
        if (s->is_runtime && strcmp(s->name, name) == 0) return s;
    }
    return NULL;
}

/* Struct types follow the same visibility rules; `geo::Point` names one
 * in a specific module. */
static Type *find_struct(Sema *S, const char *name) {
    const char *sep = strstr(name, "::");
    if (sep) {
        char alias[128];
        snprintf(alias, sizeof alias, "%.*s", (int)(sep - name), name);
        int mod = module_by_alias(S, alias);
        if (mod < 0) return NULL;
        for (size_t i = 0; i < S->structs.len; i++) {
            Type *t = S->structs.items[i];
            if (module_of(S, t->decl->loc.file) == mod && t->decl->vis != VIS_PRIVATE && strcmp(t->name, sep + 2) == 0)
                return t;
        }
        return NULL;
    }
    Type *found = NULL;
    for (size_t i = 0; i < S->structs.len; i++) {
        Type *t = S->structs.items[i];
        int mod = module_of(S, t->decl->loc.file);
        if (strcmp(t->name, name) != 0 || !visible(S, S->cur, mod, t->decl->vis == VIS_PRIVATE)) continue;
        if (mod == S->cur) return t;
        if (!found) found = t;
    }
    return found;
}

static void push_scope(Sema *S) {
    if (S->nmarks == S->marks_cap) {
        S->marks_cap = S->marks_cap ? S->marks_cap * 2 : 16;
        size_t *grown = realloc(S->marks, S->marks_cap * sizeof *grown);
        if (!grown) fatal("out of memory");
        S->marks = grown;
    }
    S->marks[S->nmarks++] = S->locals.len;
}

/* W0001 */
static void warn_if_unused(const Symbol *v) {
    if (v->reads > 0 || v->name[0] == '_') return;
    Diag *d;
    if (v->writes > 0) {
        d = diag_start(DIAG_WARNING, "W0001", v->loc, "variable `%s` is assigned to, but never read", v->name);
        diag_label(d, "its value is never used");
    } else {
        d = diag_start(DIAG_WARNING, "W0001", v->loc, "unused %s `%s`", v->kind == SYM_PARAM ? "parameter" : "variable",
                       v->name);
        diag_label(d, "never used");
    }
    char renamed[128];
    snprintf(renamed, sizeof renamed, "_%s", v->name);
    diag_fix(d, v->loc, renamed, "if this is intentional, prefix it with an underscore");
    diag_end(d);
}

static void pop_scope(Sema *S) {
    size_t mark = S->marks[--S->nmarks];
    /* After an error, "unused" is often wrong: the broken code may be the
     * use. Stay quiet until the function compiles. */
    if (diag_error_count() == S->fn_errors_before)
        for (size_t i = mark; i < S->locals.len; i++) warn_if_unused(S->locals.items[i]);
    S->locals.len = mark;
}

static Symbol *declare_local(Sema *S, SymKind kind, const char *name, Loc loc, Type *type, const TypeExpr *te) {
    Symbol *prev = find_local(S, name);
    if (prev) {
        Diag *d = diag_start(DIAG_ERROR, "E0203", loc, "`%s` is already declared in this function", name);
        diag_label(d, "declared again here");
        diag_span(d, prev->loc, "first declared here%s", prev->kind == SYM_PARAM ? ", as a parameter" : "");
        diag_help(d, "give it a different name; watrlang doesn't allow shadowing inside a function");
        diag_end(d);
        prev->reads++;  /* don't also warn that the first one is unused */
    }
    Symbol *s = new_symbol(S, kind, name, loc);
    s->type = type;
    s->type_expr = te;
    s->is_const = te && te->is_const;
    ptrvec_push(&S->locals, s);
    return s;
}

/* ---- did you mean ---------------------------------------------------------- */

static const char *closest(const char *name, const char **candidates, size_t n) {
    const char *best = NULL;
    size_t best_dist = (size_t)-1;
    for (size_t i = 0; i < n; i++) {
        if (!candidates[i] || !diag_similar(name, candidates[i])) continue;
        size_t d = diag_edit_distance(name, candidates[i]);
        if (d < best_dist) {
            best = candidates[i];
            best_dist = d;
        }
    }
    return best;
}

static Symbol *similar_var(Sema *S, const char *name) {
    Symbol *best = NULL;
    size_t best_dist = (size_t)-1;
    for (int pass = 0; pass < 2; pass++) {
        PtrVec *v = pass == 0 ? &S->locals : &S->globals;
        for (size_t i = 0; i < v->len; i++) {
            Symbol *s = v->items[i];
            if (!diag_similar(name, s->name)) continue;
            if (pass == 1 && (s->is_runtime || !visible(S, S->cur, s->module, s->is_private))) continue;
            size_t d = diag_edit_distance(name, s->name);
            if (d < best_dist) {
                best = s;
                best_dist = d;
            }
        }
    }
    return best;
}

static void report_unknown_var(Sema *S, const Expr *e) {
    Diag *d = diag_start(DIAG_ERROR, "E0200", e->loc, "cannot find variable `%s` in this scope", e->u.name);
    diag_label(d, "not found in this scope");
    Symbol *guess = similar_var(S, e->u.name);
    if (guess) {
        const char *what = guess->kind == SYM_GLOBAL ? "global" : guess->kind == SYM_PARAM ? "parameter" : "variable";
        diag_span(d, guess->loc, "a %s with a similar name is declared here", what);
        diag_fix(d, e->loc, guess->name, "a %s with a similar name exists", what);
    }
    diag_end(d);
}

/* ---- types ------------------------------------------------------------------- */

static const char *builtin_type_names[] = {"int", "float", "bool", "void", "i8", "i16", "i32", "i64",
                                           "u8", "u16", "u32", "u64", "usize", "f32", "f64"};

static void report_unknown_type(Sema *S, Loc loc, const char *name) {
    const char *sep = strstr(name, "::");
    if (sep) {   /* geo::Point: maybe it exists but is private */
        char alias[128];
        snprintf(alias, sizeof alias, "%.*s", (int)(sep - name), name);
        int mod = module_by_alias(S, alias);
        if (mod < 0) {
            report_unknown_module(S, loc, alias);
            return;
        }
        for (size_t i = 0; i < S->structs.len; i++) {
            Type *t = S->structs.items[i];
            if (module_of(S, t->decl->loc.file) != mod || strcmp(t->name, sep + 2) != 0) continue;
            Diag *d = diag_start(DIAG_ERROR, "E0345", loc, "`%s` is private", name);
            diag_label(d, "not visible outside `%s`", S->mods[mod]->file->path);
            diag_span(d, t->decl->name_loc, "declared `private` here");
            diag_end(d);
            return;
        }
    }
    Diag *d = diag_start(DIAG_ERROR, "E0322", loc, "cannot find type `%s`", name);
    diag_label(d, "not a known type");
    const char *names[64];
    size_t n = 0;
    for (size_t i = 0; i < sizeof builtin_type_names / sizeof *builtin_type_names; i++) names[n++] = builtin_type_names[i];
    for (size_t i = 0; i < S->structs.len && n < 64; i++)
        if (!strchr(((Type *)S->structs.items[i])->name, '<')) names[n++] = ((Type *)S->structs.items[i])->name;
    const char *guess = closest(name, names, n);
    if (guess) diag_fix(d, loc, guess, "a type with a similar name exists");
    else if (strcmp(name, "char") == 0) diag_fix(d, loc, "u8", "watrlang's byte type is `u8`");
    else if (strcmp(name, "long") == 0 || strcmp(name, "size_t") == 0)
        diag_fix(d, loc, strcmp(name, "long") == 0 ? "i64" : "usize", "watrlang uses sized type names");
    diag_end(d);
}

/* Resolves a written type. Qualifiers on the outermost type are the
 * caller's business (a `const int` variable); qualifiers on a pointee
 * become part of the pointer type. */
static Type *resolve_type(Sema *S, const TypeExpr *te, bool allow_void) {
    Type *t = ty_error;
    switch (te->kind) {
    case TYPEX_BUILTIN:
        t = type_builtin(te->u.builtin);
        if (!t) {
            not_yet(te->loc, te->u.builtin == TOK_KW_STRING_T ? "the `string` type" : "PHP-style `array`",
                    te->u.builtin == TOK_KW_STRING_T ? 7 : 12);
            return ty_error;
        }
        break;
    case TYPEX_NAMED:
        if (te->ntargs) return resolve_generic(S, te->u.name, te->targs, te->ntargs, te->loc);
        t = find_struct(S, te->u.name);
        if (!t) {
            report_missing_targs(S, te->loc, te->u.name);
            return ty_error;
        }
        break;
    case TYPEX_PTR: {
        Type *base = resolve_type(S, te->u.pointee, true);
        if (is_err(base)) return ty_error;
        return type_ptr(base, te->u.pointee->is_const, te->u.pointee->is_volatile);
    }
    case TYPEX_ARRAY: {
        Type *elem = resolve_type(S, te->u.array.elem, false);
        Expr **len_slot = (Expr **)&te->u.array.len;  /* sema annotates the tree in place */
        Type *lt = check(S, len_slot, ty_usize);
        Expr *len_expr = *len_slot;
        if (is_err(elem) || is_err(lt)) return ty_error;
        ConstVal cv;
        if (!type_is_int(lt) || !const_eval(S, len_expr, &cv, false)) {
            Diag *d = diag_start(DIAG_ERROR, "E0321", len_expr->loc, "array length must be a constant integer");
            diag_label(d, "not a compile-time constant");
            diag_help(d, "use a literal or a `const` global, like `int[16]`");
            diag_end(d);
            return ty_error;
        }
        int64_t n = (int64_t)cv.bits;
        if ((lt->is_signed && n <= 0) || cv.bits == 0) {
            Diag *d = diag_start(DIAG_ERROR, "E0321", len_expr->loc, "array length must be at least 1");
            diag_label(d, "this is %lld", (long long)n);
            diag_end(d);
            return ty_error;
        }
        if (cv.bits > (1u << 30) / (elem->size ? elem->size : 1)) {
            Diag *d = diag_start(DIAG_ERROR, "E0321", len_expr->loc, "array is too large");
            diag_label(d, "%llu elements", (unsigned long long)cv.bits);
            diag_note(d, "fixed arrays are limited to 1 GiB");
            diag_end(d);
            return ty_error;
        }
        return type_array(elem, cv.bits);
    }
    case TYPEX_SLICE:
        not_yet(te->loc, "slices (`T[]`)", 7);
        return ty_error;
    case TYPEX_RESOLVED:
        return te->u.resolved;
    case TYPEX_ERROR_UNION: {
        Type *vt = resolve_type(S, te->u.error_union.value, true);
        Type *et = resolve_type(S, te->u.error_union.error, false);
        if (is_err(vt) || is_err(et)) return ty_error;
        const char *why = NULL;
        const TypeExpr *at = te->u.error_union.error;
        if (et->kind != TY_ENUM) why = "the error type must be an enum";
        else if (vt == et) why = "the value type and the error type must be different";
        else if (vt->kind == TY_ERRUNION) { why = "error unions can't be nested"; at = te->u.error_union.value; }
        else if (vt->kind == TY_ARRAY) { why = "arrays can't be returned or passed by value"; at = te->u.error_union.value; }
        if (why) {
            Diag *d = diag_start(DIAG_ERROR, "E0349", at->loc, "invalid error union type `%s!%s`", type_name(vt),
                                 type_name(et));
            diag_label(d, "%s", why);
            if (et->kind != TY_ENUM)
                diag_help(d, "declare the errors as an enum: `enum MyError { NotFound, Denied }`");
            diag_end(d);
            return ty_error;
        }
        return type_errunion(vt, et);
    }
    }
    if (t->kind == TY_VOID && !allow_void) {
        Diag *d = diag_start(DIAG_ERROR, "E0330", te->loc, "`void` can't be used as a value type");
        diag_label(d, "`void` has no values");
        diag_help(d, "`void` is only for function return types and `void*` pointers");
        diag_end(d);
        return ty_error;
    }
    return t;
}

/* Computes field offsets, C-style. Detects structs that contain themselves. */
static void layout_struct(Sema *S, Type *st) {
    if (st->layout_state == 2) return;
    const Decl *d = st->decl;
    int saved_cur = S->cur;
    S->cur = module_of(S, d->loc.file);
    if (st->layout_state == 1) return;  /* cycle: reported by the field that closes it */
    st->layout_state = 1;

    size_t n = d->u.struct_.n;
    st->fields = arena_alloc(S->arena, (n ? n : 1) * sizeof(Field));
    st->nfields = 0;
    uint32_t offset = 0, align = 1;
    for (size_t i = 0; i < n; i++) {
        const VarDecl *f = d->u.struct_.fields[i];
        for (size_t j = 0; j < st->nfields; j++) {
            if (strcmp(st->fields[j].name, f->name) == 0) {
                Diag *e = diag_start(DIAG_ERROR, "E0331", f->loc, "field `%s` is declared twice in struct `%s`", f->name,
                                     st->name);
                diag_label(e, "declared again here");
                diag_span(e, st->fields[j].loc, "first declared here");
                diag_end(e);
            }
        }
        Type *ft = resolve_type(S, f->type, false);
        /* A struct field (or array of them) needs that struct's size. */
        Type *inner = ft;
        while (inner->kind == TY_ARRAY) inner = inner->base;
        if (inner->kind == TY_STRUCT) {
            if (inner->layout_state == 1) {
                Diag *e = inner == st
                              ? diag_start(DIAG_ERROR, "E0323", f->type->loc, "struct `%s` contains itself", st->name)
                              : diag_start(DIAG_ERROR, "E0323", f->type->loc, "structs `%s` and `%s` contain each other",
                                           inner->name, st->name);
                diag_label(e, "this would make `%s` infinitely large", st->name);
                diag_span(e, d->name_loc, "`%s` is declared here", st->name);
                char fixed[160];
                snprintf(fixed, sizeof fixed, "%s*", inner->name);
                diag_fix(e, f->type->loc, fixed, "use a pointer instead");
                diag_end(e);
                ft = ty_error;
            } else {
                layout_struct(S, inner);
            }
        }
        if (ft->kind == TY_ARRAY) {
            /* Array sizes are fixed at creation; recompute in case the element was laid out late. */
            uint64_t size = (uint64_t)ft->base->size * ft->len;
            ft->size = size > UINT32_MAX ? UINT32_MAX : (uint32_t)size;
            ft->align = ft->base->align ? ft->base->align : 1;
        }
        uint32_t fa = d->packed ? 1 : ft->align ? ft->align : 1;
        offset = (offset + fa - 1) & ~(fa - 1);
        Field *field = &st->fields[st->nfields++];
        field->name = f->name;
        field->loc = f->loc;
        field->type = ft;
        field->offset = offset;
        offset += ft->size;
        if (fa > align) align = fa;
    }
    if (d->align > align) align = d->align;   /* #[align(N)] only raises it */
    st->packed = d->packed;
    st->align = align;
    st->size = (offset + align - 1) & ~(align - 1);
    st->layout_state = 2;
    S->cur = saved_cur;
}

/* Gives each variant its value: written ones must be constant; the rest
 * count up from the previous one, starting at 0. */
static void check_enum_variants(Sema *S, Type *et) {
    const Decl *d = et->decl;
    size_t n = d->u.enum_.n;
    et->variants = arena_alloc(S->arena, (n ? n : 1) * sizeof(EnumVariant));
    et->nvariants = 0;
    Type *base = et->base;
    int64_t next = 0;
    bool next_ok = true;   /* false once the previous value was the type's maximum */
    for (size_t i = 0; i < n; i++) {
        EnumItem *item = d->u.enum_.items[i];
        for (size_t j = 0; j < et->nvariants; j++) {
            if (strcmp(et->variants[j].name, item->name) == 0) {
                Diag *e = diag_start(DIAG_ERROR, "E0351", item->loc, "variant `%s` is declared twice in enum `%s`",
                                     item->name, et->name);
                diag_label(e, "declared again here");
                diag_span(e, et->variants[j].loc, "first declared here");
                diag_end(e);
            }
        }
        int64_t value = next;
        if (item->value) {
            Type *t = check(S, &item->value, base);
            ConstVal cv;
            if (is_err(t)) {
                value = next;
            } else if (!type_is_int(t) || !const_eval(S, item->value, &cv, false)) {
                Diag *e = diag_start(DIAG_ERROR, "E0351", item->value->loc, "enum values must be constant integers");
                diag_label(e, "not a constant integer");
                diag_end(e);
            } else if (!int_widens(t, base)) {
                coerce(S, &item->value, base, &d->name_loc, "the enum's underlying type");
            } else {
                value = (int64_t)cv.bits;
            }
        } else if (!next_ok) {
            Diag *e = diag_start(DIAG_ERROR, "E0351", item->loc, "the value of `%s` doesn't fit in `%s`", item->name,
                                 type_name(base));
            diag_label(e, "the previous variant already has the largest `%s`", type_name(base));
            diag_help(e, "give it a value explicitly, or use a wider type: `enum %s: i64`", et->name);
            diag_end(e);
        }
        EnumVariant *v = &et->variants[et->nvariants++];
        v->name = item->name;
        v->loc = item->loc;
        v->value = (int64_t)int_normalize(base, (uint64_t)value);
        next_ok = base->is_signed ? v->value != (int64_t)int_type_max(base) : (uint64_t)v->value != int_type_max(base);
        next = (int64_t)((uint64_t)v->value + 1);
    }
}

/* A method of a class, its own or inherited; for interfaces, any of theirs. */
static Symbol *find_method(const Type *ct, const char *name) {
    if (ct->is_interface) {
        for (size_t i = 0; i < ct->nvtable; i++)
            if (strcmp(ct->vtable[i]->name, name) == 0) return ct->vtable[i];
        return NULL;
    }
    for (const Type *c = ct; c; c = c->base_class)
        for (size_t i = 0; i < c->nmethods; i++)
            if (strcmp(c->methods[i]->name, name) == 0) return c->methods[i];
    return NULL;
}

static const Field *find_field(const Type *st, const char *name) {
    for (size_t i = 0; i < st->nfields; i++)
        if (strcmp(st->fields[i].name, name) == 0) return &st->fields[i];
    return NULL;
}

/* E0317 */
static void report_unknown_field(Sema *S, const Type *st, const char *name, Loc loc) {
    Diag *d = diag_start(DIAG_ERROR, "E0317", loc, "%s `%s` has no field named `%s`",
                         st->kind == TY_CLASS ? "class" : "struct", st->name, name);
    diag_label(d, "unknown field");
    const char *names[64];
    size_t n = 0;
    for (size_t i = 0; i < st->nfields && n < 64; i++) names[n++] = st->fields[i].name;
    const char *guess = closest(name, names, n);
    if (guess) {
        diag_fix(d, loc, guess, "a field with a similar name exists");
    } else if (st->nfields) {
        char list[256] = "";
        size_t used = 0;
        for (size_t i = 0; i < st->nfields && used < sizeof list - 40; i++)
            used += (size_t)snprintf(list + used, sizeof list - used, "%s`%s`", i ? ", " : "", st->fields[i].name);
        diag_note(d, "available fields: %s", list);
    }
    if (st->decl) diag_span(d, st->decl->name_loc, "`%s` is declared here", st->name);
    (void)S;
    diag_end(d);
}

/* ---- conversions --------------------------------------------------------------- */

/* A literal whose type isn't fixed yet and should adapt to the other side. */
static bool is_literal(const Expr *e) {
    if (e->type && e->type->kind == TY_ENUM) return false;   /* Color::Red, folded */
    if (e->kind == EXPR_INT || e->kind == EXPR_FLOAT || e->kind == EXPR_CHAR) return true;
    return e->kind == EXPR_UNARY && e->u.unary.op == TOK_MINUS &&
           (e->u.unary.operand->kind == EXPR_INT || e->u.unary.operand->kind == EXPR_FLOAT);
}

static Expr *wrap_cast(Sema *S, Expr *inner, Type *to) {
    Expr *c = ARENA_NEW(S->arena, Expr);
    c->kind = EXPR_CAST;
    c->loc = inner->loc;
    c->implicit = true;
    c->u.cast.expr = inner;
    c->type = to;
    return c;
}

/* Reports `void` used as a value. */
static bool require_value(Sema *S, const Expr *e) {
    if (e->type->kind != TY_VOID) return true;
    Diag *d = diag_start(DIAG_ERROR, "E0300", e->loc, "expected a value, but this call returns nothing");
    diag_label(d, "has no value");
    if (e->kind == EXPR_CALL && e->sym) {
        const Decl *fn = e->sym->decl;
        diag_span(d, ret_type_loc(fn), fn->u.func.ret ? "`%s` is declared to return `void`"
                                                      : "`%s` has no return type, so it returns nothing", fn->name);
    }
    (void)S;
    diag_end(d);
    return false;
}

/* Both operands are usable values; reports void operands even when the
 * other side already failed. */
static bool both_values(Sema *S, const Expr *a, const Expr *b) {
    bool ok = !is_err(a->type) && !is_err(b->type);
    if (!is_err(a->type)) ok = require_value(S, a) && ok;
    if (!is_err(b->type)) ok = require_value(S, b) && ok;
    return ok;
}

/* Explains the most common mismatches with a concrete fix. */
static void mismatch_help(Sema *S, Diag *d, const Expr *e, const Type *from, const Type *to) {
    char suffix[160];
    if (to->kind == TY_BOOL && type_is_int(from)) {
        fix_append(S, d, e, " != 0", "compare explicitly to get a `bool`");
    } else if (to->kind == TY_BOOL && from->kind == TY_PTR) {
        fix_append(S, d, e, " != null", "compare explicitly to get a `bool`");
    } else if (type_is_int(to) && from->kind == TY_BOOL) {
        snprintf(suffix, sizeof suffix, " as %s", type_name(to));
        fix_append(S, d, e, suffix, "convert with `as` (true becomes 1, false becomes 0)");
    } else if (type_is_numeric(to) && type_is_numeric(from)) {
        snprintf(suffix, sizeof suffix, " as %s", type_name(to));
        const char *why = type_is_int(to) && type_is_int(from)
                              ? "convert with `as`, which wraps values that don't fit"
                              : "convert with `as`";
        fix_append(S, d, e, suffix, why);
        if (type_is_int(to) && type_is_int(from) && from->is_signed != to->is_signed && from->size == to->size)
            diag_note(d, "`%s` and `%s` have the same size, but one can hold negative values and the other can't",
                      type_name(from), type_name(to));
    } else if (from->kind == TY_PTR && to->kind == TY_PTR) {
        if ((from->base_const && !to->base_const) || (from->base_volatile && !to->base_volatile))
            diag_note(d, "this would drop the `%s` qualifier", from->base_const && !to->base_const ? "const" : "volatile");
        else if (from->base->kind == TY_VOID)
            diag_note(d, "`void*` doesn't convert to other pointers implicitly; use `as %s`", type_name(to));
    } else if (from->kind == TY_ARRAY && to->kind == TY_PTR) {
        diag_help(d, "to point at the first element, write `&arr[0]`");
    } else if (from->kind == TY_ERRUNION && from->base->kind != TY_VOID && type_assignable(from->base, to)) {
        fix_append(S, d, e, "?", "to pass the error on to the caller, add `?`");
        diag_help(d, "to handle the error here instead: `??` gives a fallback value, and `->ok` says whether it succeeded");
    } else if (to->kind == TY_ERRUNION && from->kind == TY_ENUM) {
        diag_note(d, "this function's errors are `%s`, not `%s`", type_name(to->err), type_name(from));
    } else if (from->kind == TY_MIXED) {
        snprintf(suffix, sizeof suffix, " as %s", type_name(to));
        fix_append(S, d, e, suffix, "convert it with `as`, which checks the type at run time");
    }
}

/* Checks that the expression in *slot can be used as `to`, inserting any
 * conversion that needs code. `why`/`why_text` add a secondary label
 * explaining where the expectation comes from. */
static bool coerce(Sema *S, Expr **slot, Type *to, const Loc *why, const char *why_text) {
    Expr *e = *slot;
    Type *from = e->type;
    if (is_err(from) || is_err(to)) return true;
    if (!require_value(S, e)) return false;
    if (type_assignable(from, to)) {
        if (from->kind == TY_FLOAT && to->kind == TY_FLOAT && from->size != to->size) *slot = wrap_cast(S, e, to);
        return true;
    }
    if (to->kind == TY_MIXED) {   /* any storable value becomes mixed */
        Expr *m = to_mixed(S, e);
        if (m) {
            *slot = m;
            return true;
        }
    }
    if (to->kind == TY_ERRUNION && from->kind != TY_ERRUNION) {
        /* A value or an error becomes a T!E. */
        if (from == to->err) {
            *slot = wrap_cast(S, e, to);
            return true;
        }
        if (to->base->kind != TY_VOID && type_assignable(from, to->base)) {
            coerce(S, slot, to->base, why, why_text);
            *slot = wrap_cast(S, *slot, to);
            return true;
        }
    }
    bool qual = from->kind == TY_PTR && to->kind == TY_PTR && type_equiv(from->base, to->base);
    Diag *d = diag_start(DIAG_ERROR, qual ? "E0326" : "E0309", e->loc, "mismatched types: expected `%s`, found `%s`",
                         type_name(to), type_name(from));
    diag_label(d, "expected `%s`, found `%s`", type_name(to), type_name(from));
    if (why && why->file) diag_span(d, *why, "%s", why_text);
    mismatch_help(S, d, e, from, to);
    diag_end(d);
    return false;
}

/* Conditions of if/while/for/?: must be bool. */
static void require_bool(Sema *S, Expr **slot, const char *what) {
    Expr *e = *slot;
    Type *t = check(S, slot, ty_bool);
    e = *slot;
    if (is_err(t) || t->kind == TY_BOOL) return;
    if (!require_value(S, e)) return;
    Diag *d = diag_start(DIAG_ERROR, "E0312", e->loc, "%s must be a `bool`, found `%s`", what, type_name(t));
    diag_label(d, "expected `bool`");
    if (e->kind == EXPR_INT && e->u.int_val <= 1) diag_fix(d, e->loc, e->u.int_val ? "true" : "false", "use a `bool` literal");
    else if (type_is_int(t)) fix_append(S, d, e, " != 0", "compare explicitly");
    else if (t->kind == TY_PTR) fix_append(S, d, e, " != null", "compare explicitly");
    diag_note(d, "watrlang doesn't treat numbers or pointers as true/false");
    diag_end(d);
}

/* The common type of two numeric operands, or NULL (error reported). */
static Type *unify_numeric(Sema *S, const Expr *whole, Expr *lhs, Expr *rhs, TokenKind op) {
    Type *a = lhs->type, *b = rhs->type;
    if (type_is_int(a) && type_is_int(b)) {
        if (type_equiv(a, b) || int_widens(b, a)) return a;
        if (int_widens(a, b)) return b;
    } else if (type_is_float(a) && type_is_float(b)) {
        return a->size >= b->size ? a : b;
    }
    Diag *d = diag_start(DIAG_ERROR, "E0310", whole->loc, "mismatched types: can't use `%s` with `%s` and `%s`",
                         token_kind_spelling(op), type_name(a), type_name(b));
    diag_span(d, lhs->loc, "`%s`", type_name(a));
    diag_span(d, rhs->loc, "`%s`", type_name(b));
    if (type_is_numeric(a) && type_is_numeric(b)) {
        /* Suggest converting the right-hand side to the left's type. */
        char suffix[96];
        snprintf(suffix, sizeof suffix, " as %s", type_name(a));
        fix_append(S, d, rhs, suffix, "convert one side with `as`");
        if (type_is_int(a) && type_is_int(b) && a->is_signed != b->is_signed)
            diag_note(d, "watrlang never mixes signed and unsigned integers implicitly");
    }
    diag_end(d);
    return NULL;
}

/* Inserts the f32 -> f64 conversion on whichever side needs it. */
static void widen_to(Sema *S, Expr **slot, Type *t) {
    Expr *e = *slot;
    if (type_is_float(e->type) && type_is_float(t) && e->type->size != t->size) *slot = wrap_cast(S, e, t);
    else if (type_is_int(t) && is_literal(e)) e->type = t;
}

/* Does integer literal `e` (possibly negated) fit in `t`? */
static bool literal_fits(const Expr *e, const Type *t) {
    bool neg = e->kind == EXPR_UNARY;
    if (neg) e = e->u.unary.operand;
    if (e->kind != EXPR_INT && e->kind != EXPR_CHAR) return true;
    if (!type_is_int(t)) return true;
    if (neg) return t->is_signed && e->u.int_val <= int_type_max(t) + 1;
    return e->u.int_val <= int_type_max(t);
}

/* The type a literal operand should take next to an operand of type
 * `other`: usually `other` (`x + 1` with x: u8 is u8 arithmetic), but if
 * the literal doesn't fit and the context wants a wider type that `other`
 * widens into, that wider type (`int n = small + 1000;` computes in int). */
static Type *literal_hint(const Expr *lit, Type *other, Type *hint) {
    if (!type_is_numeric(other)) return hint;
    if (hint && type_is_int(hint) && type_is_int(other) && !literal_fits(lit, other) && int_widens(other, hint))
        return hint;
    return other;
}

/* Checks two operands so that a literal on either side takes the other
 * side's type: `1 + x` with x: u8 types the 1 as u8. */
static void check_operands(Sema *S, Expr **lhs, Expr **rhs, Type *hint) {
    if (is_literal(*lhs) && !is_literal(*rhs)) {
        Type *rt = check(S, rhs, hint);
        check(S, lhs, literal_hint(*lhs, rt, hint));
    } else {
        Type *lt = check(S, lhs, hint);
        Type *rhint = lt->kind == TY_PTR || lt->kind == TY_BOOL ? lt : literal_hint(*rhs, lt, hint);
        check(S, rhs, rhint);
    }
}

/* ---- runtime calls ------------------------------------------------------------- */

static bool is_runtime_file(const SrcFile *f) {
    return f && f->path[0] == '<';
}

static void add_callee(Sema *S, Symbol *callee) {
    Symbol *fn = S->fn;
    if (!fn) return;
    for (size_t i = 0; i < fn->ncallees; i++)
        if (fn->callees[i] == callee) return;
    if (fn->ncallees == fn->callees_cap) {
        size_t cap = fn->callees_cap ? fn->callees_cap * 2 : 8;
        Symbol **grown = arena_alloc(S->arena, cap * sizeof *grown);
        Loc *locs = arena_alloc(S->arena, cap * sizeof *locs);
        if (fn->ncallees) {
            memcpy(grown, fn->callees, fn->ncallees * sizeof *grown);
            memcpy(locs, fn->callee_locs, fn->ncallees * sizeof *locs);
        }
        fn->callees = grown;
        fn->callee_locs = locs;
        fn->callees_cap = cap;
    }
    fn->callee_locs[fn->ncallees] = S->cur_loc;
    fn->callees[fn->ncallees++] = callee;
}

static Expr *new_node(Sema *S, ExprKind kind, Loc loc, Type *t) {
    Expr *e = ARENA_NEW(S->arena, Expr);
    e->kind = kind;
    e->loc = loc;
    e->type = t;
    return e;
}

/* A call to a runtime prelude function, with already-checked arguments. */
static Expr *rt_call(Sema *S, const char *name, Loc loc, Expr **args, size_t n) {
    Symbol *fn = find_runtime_func(S, name);
    S->cur_loc = loc;
    if (!fn) fatal("runtime function %s is missing", name);
    Expr *callee = new_node(S, EXPR_IDENT, loc, ty_error);
    callee->u.name = fn->name;
    callee->sym = fn;
    Expr *e = new_node(S, EXPR_CALL, loc, fn->type);
    e->sym = fn;
    e->u.call.callee = callee;
    e->u.call.nargs = n;
    e->u.call.args = arena_alloc(S->arena, (n ? n : 1) * sizeof(Expr *));
    for (size_t i = 0; i < n; i++) e->u.call.args[i] = args[i];
    add_callee(S, fn);
    return e;
}

static Expr *rt_call1(Sema *S, const char *name, Expr *arg) {
    return rt_call(S, name, arg->loc, &arg, 1);
}

static Expr *wrap_cast(Sema *S, Expr *inner, Type *to);

static Expr *convert(Sema *S, Expr *e, Type *t) {
    return type_equiv(e->type, t) ? e : wrap_cast(S, e, t);
}

/* E0338 */
static void report_not_stringable(const Expr *e, const char *context) {
    Diag *d = diag_start(DIAG_ERROR, "E0338", e->loc, "a value of type `%s` can't be %s", type_name(e->type), context);
    diag_label(d, "no text form");
    diag_note(d, "strings, numbers, bools, enums and pointers can be printed and joined with `.`");
    if (e->type->kind == TY_ERRUNION)
        diag_help(d, "unwrap it first: `?` passes the error on, `??` gives a fallback value");
    else if (e->type->kind == TY_CLASS)
        diag_help(d, "give `%s` a `public function __toString(): string` method", e->type->name);
    else if (e->type->kind == TY_STRUCT) diag_help(d, "print the fields you want instead, like `\"{v->x}, {v->y}\"`");
    diag_end(d);
}

/* "file:line:col" as a string literal, for runtime check messages. */
static Expr *where_string(Sema *S, Loc loc) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s:%u:%u", loc.file ? loc.file->path : "?", loc.line, loc.col);
    Expr *e = new_node(S, EXPR_STRING, loc, ty_string);
    e->u.str.data = arena_strndup(S->arena, buf, strlen(buf));
    e->u.str.len = strlen(buf);
    return e;
}

static Expr *u64_const(Sema *S, Loc loc, uint64_t v) {
    Expr *e = new_node(S, EXPR_INT, loc, ty_u64);
    e->u.int_val = v;
    return e;
}

static Expr *rt_call2(Sema *S, const char *name, Loc loc, Expr *a, Expr *b) {
    Expr *args[2] = {a, b};
    return rt_call(S, name, loc, args, 2);
}

/* A checked value as `mixed`, or NULL if its type can't be stored in one. */
static Expr *to_mixed(Sema *S, Expr *e) {
    Type *t = e->type;
    switch (t->kind) {
    case TY_MIXED: return e;
    case TY_NULL: return rt_call(S, "__rt_mixed_null", e->loc, NULL, 0);
    case TY_BOOL: return rt_call1(S, "__rt_mixed_bool", e);
    case TY_INT:
    case TY_ENUM: return rt_call1(S, "__rt_mixed_int", convert(S, e, ty_i64));
    case TY_FLOAT: return rt_call1(S, "__rt_mixed_float", convert(S, e, ty_f64));
    case TY_STRING: return rt_call1(S, "__rt_mixed_string", e);
    case TY_PHPARRAY: return rt_call1(S, "__rt_mixed_array", e);
    case TY_CLASS:
        return rt_call1(S, "__rt_mixed_object", wrap_cast(S, e, ty_u64));
    default: return NULL;
    }
}

/* A mixed value as `to`, checked at run time; NULL if `to` can't be held in a mixed. */
static Expr *from_mixed(Sema *S, Expr *e, Type *to, Loc loc) {
    Expr *args[4];
    switch (to->kind) {
    case TY_MIXED: return e;
    case TY_BOOL: return rt_call2(S, "__rt_as_bool", loc, e, where_string(S, loc));
    case TY_INT:
    case TY_ENUM: return convert(S, rt_call2(S, "__rt_as_int", loc, e, where_string(S, loc)), to);
    case TY_FLOAT: return convert(S, rt_call2(S, "__rt_as_float", loc, e, where_string(S, loc)), to);
    case TY_STRING: return rt_call2(S, "__rt_as_string", loc, e, where_string(S, loc));
    case TY_PHPARRAY: return rt_call2(S, "__rt_as_array", loc, e, where_string(S, loc));
    case TY_CLASS: {
        Expr *name = new_node(S, EXPR_STRING, loc, ty_string);
        name->u.str.data = to->name;
        name->u.str.len = strlen(to->name);
        Expr *oargs[5] = {e, desc_addr(S, to, loc), bool_const(S, loc, to->is_interface), name, where_string(S, loc)};
        (void)args;
        return wrap_cast(S, rt_call(S, "__rt_as_object", loc, oargs, 5), to);
    }
    default: return NULL;
    }
}

/* An `array` (or a mixed holding one, checked at run time) as an array expression. */
static Expr *as_array_expr(Sema *S, Expr *base) {
    if (base->type->kind == TY_PHPARRAY) return base;
    return rt_call2(S, "__rt_as_array", base->loc, base, where_string(S, base->loc));
}

/* E0366 */
static void report_not_mixable(const Expr *e, const char *what) {
    Diag *d = diag_start(DIAG_ERROR, "E0366", e->loc, "a value of type `%s` can't be %s", type_name(e->type), what);
    diag_label(d, "not storable in `mixed`");
    diag_note(d, "`mixed` holds null, bools, ints, floats, strings, arrays and objects");
    if (e->type->kind == TY_STRUCT) diag_help(d, "use a class instead of a struct, or store the fields one by one");
    diag_end(d);
}

/* A call to the generated function that names a variant of enum `e`'s
 * type ("Red"); values that aren't a variant print as numbers. */
static Expr *enum_name_call(Sema *S, Expr *e) {
    Type *et = e->type;
    if (!et->name_fn) {
        Symbol *fn = new_symbol(S, SYM_FUNC, et->name, et->decl->name_loc);
        fn->module = module_of(S, et->decl->loc.file);
        const char *prefix = S->mods[fn->module]->prefix;
        size_t n = strlen(prefix) + strlen(et->name) + 8;
        char *name = arena_alloc(S->arena, n);
        snprintf(name, n, "w.enum.%s%s", prefix, et->name);
        fn->asm_name = name;
        fn->type = ty_string;
        fn->nparams = 1;
        fn->param_types = arena_alloc(S->arena, sizeof(Type *));
        fn->param_types[0] = et;
        fn->is_runtime = true;
        fn->enum_type = et;
        Symbol *saved = S->fn;
        S->fn = fn;
        /* Values that aren't a variant print as numbers; without a heap, from a static buffer. */
        add_callee(S, find_runtime_func(S, S->freestanding ? "__rt_static_num_to_string"
                                           : et->is_signed ? "__rt_i64_to_string" : "__rt_u64_to_string"));
        S->fn = saved;
        ptrvec_push(&S->enum_fns, fn);
        et->name_fn = fn;
    }
    Symbol *fn = et->name_fn;
    Expr *callee = new_node(S, EXPR_IDENT, e->loc, ty_error);
    callee->u.name = fn->name;
    callee->sym = fn;
    Expr *call = new_node(S, EXPR_CALL, e->loc, ty_string);
    call->sym = fn;
    call->u.call.callee = callee;
    call->u.call.nargs = 1;
    call->u.call.args = arena_alloc(S->arena, sizeof(Expr *));
    call->u.call.args[0] = e;
    add_callee(S, fn);
    return call;
}

/* obj->__toString() */
/* Marks a method call on a receiver of type `ot` as virtual when it is,
 * and records it so every override counts as reachable. */
static void set_dispatch(Sema *S, Expr *call, Type *ot, Symbol *m) {
    if (m->is_static || (m->vindex < 0 && !ot->is_interface)) return;
    if (ot->is_interface) {
        for (size_t i = 0; i < ot->nvtable; i++)
            if (ot->vtable[i] == m) call->vslot = (int)i + 1;
        call->viface = ot;
    } else {
        call->vslot = m->vindex + 1;
    }
    VSite *v = arena_alloc(S->arena, sizeof *v);
    v->fn = S->fn;
    v->t = ot;
    v->name = m->name;
    if (S->fn) ptrvec_push(&S->vsites, v);
}

/* The address of a class's or interface's runtime descriptor, as a u64. */
static Expr *desc_addr(Sema *S, Type *ct, Loc loc) {
    if (!ct->desc_sym) {
        Symbol *g = new_symbol(S, SYM_GLOBAL, ct->desc_name, loc);
        g->asm_name = ct->desc_name;
        g->type = ty_u8;
        g->is_runtime = true;
        ct->desc_sym = g;
    }
    Expr *id = new_node(S, EXPR_IDENT, loc, ty_u8);
    id->u.name = ct->desc_name;
    id->sym = ct->desc_sym;
    Expr *amp = new_node(S, EXPR_UNARY, loc, type_ptr(ty_u8, true, false));
    amp->u.unary.op = TOK_AMP;
    amp->u.unary.operand = id;
    return wrap_cast(S, amp, ty_u64);
}

static Expr *bool_const(Sema *S, Loc loc, bool v) {
    Expr *e = new_node(S, EXPR_BOOL, loc, ty_bool);
    e->u.bool_val = v;
    return e;
}

static Expr *to_string_call(Sema *S, Expr *e) {
    Symbol *m = find_method(e->type, "__toString");
    add_callee(S, m);
    Expr *callee = new_node(S, EXPR_IDENT, e->loc, ty_error);
    callee->u.name = m->name;
    callee->sym = m;
    Expr *call = new_node(S, EXPR_CALL, e->loc, ty_string);
    call->sym = m;
    call->builtin = BUILTIN_METHOD;
    call->u.call.callee = callee;
    call->u.call.nargs = 1;
    call->u.call.args = arena_alloc(S->arena, sizeof(Expr *));
    call->u.call.args[0] = e;
    set_dispatch(S, call, e->type, m);
    return call;
}

/* The text form of a checked expression, as a `string` expression:
 * numbers in decimal, floats like "%.14g", bools as true/false, pointers in hex. */
static Expr *stringify(Sema *S, Expr *e, const char *context) {
    Type *t = e->type;
    switch (t->kind) {
    case TY_STRING:
        return e;
    case TY_INT:
        if (t->is_signed) return rt_call1(S, "__rt_i64_to_string", convert(S, e, ty_i64));
        return rt_call1(S, "__rt_u64_to_string", convert(S, e, ty_u64));
    case TY_FLOAT:
        return rt_call1(S, "__rt_f64_to_string", convert(S, e, ty_f64));
    case TY_BOOL:
        return rt_call1(S, "__rt_bool_to_string", e);
    case TY_ENUM:
        return enum_name_call(S, e);
    case TY_MIXED:
        return rt_call1(S, "__rt_mixed_to_string", e);
    case TY_PHPARRAY:
        return rt_call1(S, "__rt_mixed_to_string", rt_call1(S, "__rt_mixed_array", e));
    case TY_CLASS:
        if (find_method(t, "__toString")) return to_string_call(S, e);
        report_not_stringable(e, context);
        return new_node(S, EXPR_NULL, e->loc, ty_error);
    case TY_PTR:
    case TY_NULL:
        return rt_call1(S, "__rt_ptr_to_string", convert(S, e, ty_u64));
    case TY_ERROR:
        return e;
    default:
        report_not_stringable(e, context);
        return new_node(S, EXPR_NULL, e->loc, ty_error);
    }
}

/* `echo e`: writes the value without building a string first. */
static Expr *echo_call(Sema *S, Expr *e) {
    Type *t = e->type;
    switch (t->kind) {
    case TY_STRING: return rt_call1(S, "__rt_echo_str", e);
    case TY_INT:
        if (t->is_signed) return rt_call1(S, "__rt_echo_i64", convert(S, e, ty_i64));
        return rt_call1(S, "__rt_echo_u64", convert(S, e, ty_u64));
    case TY_FLOAT: return rt_call1(S, "__rt_echo_f64", convert(S, e, ty_f64));
    case TY_BOOL: return rt_call1(S, "__rt_echo_bool", e);
    case TY_ENUM: return rt_call1(S, "__rt_echo_str", enum_name_call(S, e));
    case TY_MIXED:
    case TY_PHPARRAY: return rt_call1(S, "__rt_echo_str", stringify(S, e, "printed"));
    case TY_CLASS:
        if (find_method(t, "__toString")) return rt_call1(S, "__rt_echo_str", to_string_call(S, e));
        report_not_stringable(e, "printed with `echo`");
        return NULL;
    case TY_PTR:
    case TY_NULL: return rt_call1(S, "__rt_echo_ptr", convert(S, e, ty_u64));
    case TY_ERROR: return NULL;
    default:
        report_not_stringable(e, "printed with `echo`");
        return NULL;
    }
}

static Type *check_interp(Sema *S, Expr **slot) {
    Expr *e = *slot;
    Expr *acc = NULL;
    bool ok = true;
    for (size_t i = 0; i < e->u.interp.n; i++) {
        Type *t = check(S, &e->u.interp.parts[i], NULL);
        Expr *part = e->u.interp.parts[i];
        if (is_err(t)) {
            ok = false;
            continue;
        }
        if (!require_value(S, part)) {
            ok = false;
            continue;
        }
        Expr *str = stringify(S, part, "interpolated into a string");
        if (is_err(str->type)) {
            ok = false;
            continue;
        }
        if (!acc) {
            acc = str;
        } else {
            Expr *pair[2] = {acc, str};
            acc = rt_call(S, "__rt_concat", e->loc, pair, 2);
        }
    }
    if (!ok) return set_type(e, ty_error);
    if (!acc) {   /* can't happen: the parser makes at least one part */
        acc = new_node(S, EXPR_STRING, e->loc, ty_string);
    }
    *slot = acc;
    return acc->type;
}

/* a . b: joins two values as text. */
static Type *check_concat(Sema *S, Expr **slot) {
    Expr *e = *slot;
    Type *lt = check(S, &e->u.binary.lhs, ty_string);
    /* `p.x` written C-style: `.` is concatenation, fields are `p->x`. */
    Expr *r = e->u.binary.rhs;
    if (r->kind == EXPR_IDENT && !r->parenthesized && !is_err(lt) && !find_var(S, r->u.name)) {
        const Type *st = lt->kind == TY_PTR ? lt->base : lt;
        bool member = (st->kind == TY_STRUCT || st->kind == TY_CLASS) && find_field(st, r->u.name);
        if (st->kind == TY_CLASS && find_method(st, r->u.name)) member = true;
        if (member) {
            Diag *d = diag_start(DIAG_ERROR, "E0384", e->loc, "`.` joins strings; members are reached with `->`");
            diag_label(d, "this reads as `%s` joined with a variable `%s`", loc_text(S, e->u.binary.lhs->loc), r->u.name);
            char fixed[256];
            snprintf(fixed, sizeof fixed, "%s->%s", loc_text(S, e->u.binary.lhs->loc), r->u.name);
            diag_fix(d, e->loc, fixed, "use `->`, which works on values and pointers alike");
            diag_end(d);
            return set_type(e, ty_error);
        }
    }
    Type *rt = check(S, &e->u.binary.rhs, ty_string);
    if (!both_values(S, e->u.binary.lhs, e->u.binary.rhs) || is_err(lt) || is_err(rt)) return set_type(e, ty_error);
    Expr *a = stringify(S, e->u.binary.lhs, "joined with `.`");
    Expr *b = stringify(S, e->u.binary.rhs, "joined with `.`");
    if (is_err(a->type) || is_err(b->type)) return set_type(e, ty_error);
    Expr *pair[2] = {a, b};
    *slot = rt_call(S, "__rt_concat", e->loc, pair, 2);
    return ty_string;
}

/* ---- lvalues ------------------------------------------------------------------- */

typedef enum { LV_NONE, LV_MUT, LV_CONST_VAR, LV_CONST_PTR, LV_STRING } LvalueKind;

static LvalueKind lvalue_kind(const Expr *e) {
    switch (e->kind) {
    case EXPR_IDENT:
        if (!e->sym || e->sym->kind == SYM_FUNC) return LV_NONE;
        return e->sym->is_const ? LV_CONST_VAR : LV_MUT;
    case EXPR_UNARY:
        if (e->u.unary.op != TOK_STAR) return LV_NONE;
        return e->u.unary.operand->type->kind == TY_PTR && e->u.unary.operand->type->base_const ? LV_CONST_PTR : LV_MUT;
    case EXPR_MEMBER: {
        const Type *ot = e->u.member.object->type;
        if (ot->kind == TY_STRING || (ot->kind == TY_PTR && ot->base->kind == TY_STRING)) return LV_STRING;
        if (e->builtin != BUILTIN_NONE) return LV_NONE;   /* parts of an error union */
        if (e->via_pointer) return ot->base_const ? LV_CONST_PTR : LV_MUT;
        return lvalue_kind(e->u.member.object);
    }
    case EXPR_INDEX:
        if (e->u.index.base->type->kind == TY_STRING) return LV_STRING;
        if (e->u.index.base->type->kind == TY_PTR) return e->u.index.base->type->base_const ? LV_CONST_PTR : LV_MUT;
        return lvalue_kind(e->u.index.base);
    default:
        return LV_NONE;
    }
}

/* The variable at the root of an lvalue like `v->pos[2]`, if any. */
static Symbol *root_var(const Expr *e) {
    for (;;) {
        switch (e->kind) {
        case EXPR_IDENT: return e->sym;
        case EXPR_MEMBER: if (e->via_pointer) return NULL; e = e->u.member.object; break;
        case EXPR_INDEX: if (e->u.index.base->type->kind == TY_PTR) return NULL; e = e->u.index.base; break;
        default: return NULL;
        }
    }
}

/* Checks an assignment/++/-- target (already type-checked). */
static bool check_target(Expr *target, const char *action) {
    if (is_err(target->type)) return false;
    LvalueKind k = lvalue_kind(target);
    if (k == LV_MUT) {
        Symbol *v = root_var(target);
        if (v) v->writes++;
        return true;
    }
    Diag *d;
    if (k == LV_STRING) {
        d = diag_start(DIAG_ERROR, "E0340", target->loc, "strings are immutable");
        diag_label(d, "can't %s part of a string", action);
        diag_help(d, "build a new string instead, for example with `.` or \"{interpolation}\"");
        diag_end(d);
        return false;
    }
    if (k == LV_NONE) {
        if (target->kind == EXPR_IDENT && target->sym && target->sym->kind == SYM_FUNC) {
            d = diag_start(DIAG_ERROR, "E0209", target->loc, "can't %s `%s` because it's a function", action,
                           target->u.name);
            diag_label(d, "not a variable");
            diag_span(d, target->sym->loc, "`%s` is defined here", target->u.name);
        } else {
            d = diag_start(DIAG_ERROR, "E0209", target->loc, "invalid left-hand side: can't %s this", action);
            diag_label(d, "not something that can be assigned to");
            diag_note(d, "only variables, fields, array elements and `*pointer` can be assigned");
        }
    } else if (k == LV_CONST_VAR) {
        Symbol *v = root_var(target);
        d = diag_start(DIAG_ERROR, "E0207", target->loc, "cannot %s `%s`, because it's a constant", action,
                       v ? v->name : "this");
        diag_label(d, "can't %s a constant", action);
        if (v) {
            diag_span(d, v->loc, "`%s` is declared `const` here", v->name);
            Loc kw;
            if (const_keyword_loc(v->type_expr, &kw)) diag_fix(d, kw, "", "if `%s` needs to change, remove `const`", v->name);
        }
    } else {
        d = diag_start(DIAG_ERROR, "E0327", target->loc, "can't write through a `const` pointer");
        diag_label(d, "this points to `const` data");
        diag_note(d, "a `const T*` can be read through but not written through");
    }
    diag_end(d);
    return false;
}

/* ---- expressions --------------------------------------------------------------- */

static Type *check_int_literal(Sema *S, Expr *e, Type *hint, bool negated) {
    uint64_t v = e->u.int_val;
    Type *t = hint && (type_is_int(hint) || type_is_float(hint)) ? hint : (e->kind == EXPR_CHAR ? ty_u8 : ty_int);
    if (type_is_float(t)) return set_type(e, t);
    if (negated && !t->is_signed) {
        Diag *d = diag_start(DIAG_ERROR, "E0328", e->loc, "`%s` can't hold negative numbers", type_name(t));
        diag_label(d, "negative value for an unsigned type");
        diag_end(d);
        return set_type(e, ty_error);
    }
    /* -128 fits in i8 even though 128 doesn't. */
    uint64_t limit = negated ? int_type_max(t) + 1 : int_type_max(t);
    if (v > limit) {
        Diag *d = diag_start(DIAG_ERROR, "E0301", e->loc, "literal `%s%llu` doesn't fit in `%s`", negated ? "-" : "",
                             (unsigned long long)v, type_name(t));
        diag_label(d, "out of range for `%s`", type_name(t));
        if (t->is_signed)
            diag_note(d, "`%s` holds %lld to %llu", type_name(t), (long long)int_type_min(t),
                      (unsigned long long)int_type_max(t));
        else
            diag_note(d, "`%s` holds 0 to %llu", type_name(t), (unsigned long long)int_type_max(t));
        if (!negated && t == ty_int && v <= UINT64_MAX) diag_help(d, "for values this large, use `u64`");
        (void)S;
        diag_end(d);
        return set_type(e, ty_error);
    }
    return set_type(e, t);
}

static Type *check_ident(Sema *S, Expr *e, bool counts_as_read) {
    Symbol *v = find_local(S, e->u.name);
    if (!v) {
        Symbol *other;
        v = find_in(S, &S->globals, e->u.name, &other);
        if (v && other) {
            report_ambiguous(S, e->loc, e->u.name, v, other);
            return set_type(e, ty_error);
        }
    }
    if (v) {
        e->sym = v;
        if (counts_as_read) v->reads++;
        return set_type(e, v->type);
    }
    Symbol *fn = find_func(S, e->u.name);
    if (fn) {
        e->sym = fn;
        Diag *d = diag_start(DIAG_ERROR, "E0205", e->loc, "`%s` is a function, not a value", e->u.name);
        diag_label(d, "function used without calling it");
        if (fn->nparams == 0) {
            char call[128];
            snprintf(call, sizeof call, "%s()", e->u.name);
            diag_fix(d, e->loc, call, "call it");
        } else {
            diag_help(d, "call it with %zu argument%s: `%s(...)`", fn->nparams, fn->nparams == 1 ? "" : "s", e->u.name);
        }
        diag_end(d);
        return set_type(e, ty_error);
    }
    report_unknown_var(S, e);
    return set_type(e, ty_error);
}

static Type *check_unary(Sema *S, Expr *e, Type *hint) {
    TokenKind op = e->u.unary.op;
    Expr *operand = e->u.unary.operand;

    if (op == TOK_MINUS && operand->kind == EXPR_INT) {
        Type *t = check_int_literal(S, operand, hint, true);
        return set_type(e, t);
    }

    switch (op) {
    case TOK_MINUS: {
        Type *t = check(S, &e->u.unary.operand, hint);
        operand = e->u.unary.operand;
        if (is_err(t) || !require_value(S, operand)) return set_type(e, ty_error);
        if (!type_is_numeric(t) || (type_is_int(t) && !t->is_signed)) {
            Diag *d = diag_start(DIAG_ERROR, type_is_int(t) ? "E0328" : "E0311", e->loc,
                                 "can't negate a value of type `%s`", type_name(t));
            diag_label(d, type_is_int(t) ? "unsigned values can't be negative" : "not a number");
            if (type_is_int(t)) {
                char *text = loc_text(S, operand->loc);
                if (text) {
                    char fixed[256];
                    snprintf(fixed, sizeof fixed, "0 - %s", text);
                    diag_fix(d, e->loc, fixed, "for wrapping negation, subtract from zero");
                }
            }
            diag_end(d);
            return set_type(e, ty_error);
        }
        return set_type(e, t);
    }
    case TOK_TILDE: {
        Type *t = check(S, &e->u.unary.operand, hint);
        operand = e->u.unary.operand;
        if (is_err(t) || !require_value(S, operand)) return set_type(e, ty_error);
        if (!type_is_int(t)) {
            Diag *d = diag_start(DIAG_ERROR, "E0311", e->loc, "`~` needs an integer, found `%s`", type_name(t));
            diag_label(d, "not an integer");
            if (t->kind == TY_BOOL) diag_help(d, "for logical not, use `!`");
            diag_end(d);
            return set_type(e, ty_error);
        }
        return set_type(e, t);
    }
    case TOK_BANG: {
        Type *t = check(S, &e->u.unary.operand, ty_bool);
        operand = e->u.unary.operand;
        if (is_err(t) || !require_value(S, operand)) return set_type(e, ty_error);
        if (t->kind != TY_BOOL) {
            Diag *d = diag_start(DIAG_ERROR, "E0311", e->loc, "`!` needs a `bool`, found `%s`", type_name(t));
            diag_label(d, "expected `bool`");
            if (type_is_int(t)) {
                char *text = loc_text(S, operand->loc);
                if (text) {
                    char fixed[256];
                    snprintf(fixed, sizeof fixed, is_simple_expr(operand) ? "%s == 0" : "(%s) == 0", text);
                    diag_fix(d, e->loc, fixed, "to test for zero, compare explicitly");
                }
                diag_help(d, "for bitwise not, use `~`");
            }
            diag_end(d);
            return set_type(e, ty_error);
        }
        return set_type(e, ty_bool);
    }
    case TOK_AMP: {
        Type *t = check(S, &e->u.unary.operand, NULL);
        operand = e->u.unary.operand;
        if (is_err(t)) return set_type(e, ty_error);
        LvalueKind k = lvalue_kind(operand);
        if (k == LV_NONE) {
            Diag *d = diag_start(DIAG_ERROR, "E0316", operand->loc, "can't take the address of this expression");
            diag_label(d, "this value isn't stored anywhere");
            if (operand->kind == EXPR_IDENT && operand->sym && operand->sym->kind == SYM_FUNC)
                diag_note(d, "function pointers aren't supported yet");
            else
                diag_help(d, "store it in a variable first, then take the variable's address");
            diag_end(d);
            return set_type(e, ty_error);
        }
        Type *base = operand->type;
        Symbol *root = root_var(operand);
        if (root) root->addr_taken = true;   /* it can't live in a register */
        bool is_const = k == LV_CONST_VAR || k == LV_CONST_PTR || k == LV_STRING;
        return set_type(e, type_ptr(base, is_const, false));
    }
    case TOK_STAR: {
        Type *t = check(S, &e->u.unary.operand, NULL);
        operand = e->u.unary.operand;
        if (is_err(t) || !require_value(S, operand)) return set_type(e, ty_error);
        if (t->kind != TY_PTR || t->base->kind == TY_VOID) {
            Diag *d = diag_start(DIAG_ERROR, "E0315", e->loc, "can't dereference a value of type `%s`", type_name(t));
            if (t->kind == TY_PTR) {
                diag_label(d, "`void*` doesn't say what it points to");
                diag_help(d, "cast it to a typed pointer first, like `p as u8*`");
            } else {
                diag_label(d, "not a pointer");
            }
            diag_end(d);
            return set_type(e, ty_error);
        }
        return set_type(e, t->base);
    }
    case TOK_PLUSPLUS:
    case TOK_MINUSMINUS: {
        Expr *target = e->u.unary.operand;
        Type *t = target->kind == EXPR_IDENT ? check_ident(S, target, false) : check(S, &e->u.unary.operand, NULL);
        target = e->u.unary.operand;
        const char *action = op == TOK_PLUSPLUS ? "increment" : "decrement";
        if (is_err(t)) return set_type(e, ty_error);
        if (!type_is_int(t) && !(t->kind == TY_PTR && t->base->kind != TY_VOID)) {
            Diag *d = diag_start(DIAG_ERROR, "E0311", e->loc, "can't %s a value of type `%s`", action, type_name(t));
            diag_label(d, "`%s` only works on integers and pointers", token_kind_spelling(op));
            diag_end(d);
            return set_type(e, ty_error);
        }
        if (!check_target(target, action)) return set_type(e, ty_error);
        if (target->kind == EXPR_IDENT && target->sym) target->sym->reads++;  /* x++ reads x */
        return set_type(e, t);
    }
    default:
        return set_type(e, ty_error);
    }
}

static bool is_cmp(TokenKind op) {
    return op == TOK_EQ || op == TOK_NE || op == TOK_LT || op == TOK_GT || op == TOK_LE || op == TOK_GE;
}

static Type *check_binary(Sema *S, Expr **slot, Type *hint) {
    Expr *e = *slot;
    TokenKind op = e->u.binary.op;
    Expr **lhs = &e->u.binary.lhs, **rhs = &e->u.binary.rhs;

    if (op == TOK_DOT) return check_concat(S, slot);
    if (op == TOK_QQ) return check_coalesce(S, e, hint);

    if (op == TOK_ANDAND || op == TOK_OROR) {
        require_bool(S, lhs, "each side of `&&` and `||`");
        require_bool(S, rhs, "each side of `&&` and `||`");
        return set_type(e, ty_bool);
    }

    if (op == TOK_SHL || op == TOK_SHR) {
        Type *lt = check(S, lhs, hint);
        Type *rt = check(S, rhs, NULL);
        if (!both_values(S, *lhs, *rhs)) return set_type(e, ty_error);
        if (!type_is_int(lt) || !type_is_int(rt)) {
            const Expr *bad = !type_is_int(lt) ? *lhs : *rhs;
            Diag *d = diag_start(DIAG_ERROR, "E0310", bad->loc, "shifts need integers, found `%s`", type_name(bad->type));
            diag_label(d, "not an integer");
            diag_end(d);
            return set_type(e, ty_error);
        }
        return set_type(e, lt);
    }

    Type *ctx = is_cmp(op) ? NULL : hint;
    check_operands(S, lhs, rhs, ctx);
    Type *lt = (*lhs)->type, *rt = (*rhs)->type;
    if (!both_values(S, *lhs, *rhs)) return set_type(e, ty_error);

    /* Pointer arithmetic: p + n, n + p, p - n, p - q */
    if ((op == TOK_PLUS || op == TOK_MINUS) && (lt->kind == TY_PTR || rt->kind == TY_PTR)) {
        Type *ptr = lt->kind == TY_PTR ? lt : rt;
        if (ptr->base->kind == TY_VOID) {
            Diag *d = diag_start(DIAG_ERROR, "E0330", e->loc, "can't do arithmetic on `void*`");
            diag_label(d, "the size of what it points to is unknown");
            diag_help(d, "cast to `u8*` to move by bytes");
            diag_end(d);
            return set_type(e, ty_error);
        }
        if (lt->kind == TY_PTR && type_is_int(rt)) return set_type(e, lt);
        if (op == TOK_PLUS && type_is_int(lt) && rt->kind == TY_PTR) return set_type(e, rt);
        if (op == TOK_MINUS && lt->kind == TY_PTR && rt->kind == TY_PTR && type_equiv(lt->base, rt->base))
            return set_type(e, ty_int);
        Diag *d = diag_start(DIAG_ERROR, "E0310", e->loc, "can't use `%s` with `%s` and `%s`", token_kind_spelling(op),
                             type_name(lt), type_name(rt));
        diag_span(d, (*lhs)->loc, "`%s`", type_name(lt));
        diag_span(d, (*rhs)->loc, "`%s`", type_name(rt));
        diag_note(d, "pointers support `p + n`, `p - n`, and `p - q` for pointers of the same type");
        diag_end(d);
        return set_type(e, ty_error);
    }

    if (is_cmp(op) && lt->kind == TY_STRING && rt->kind == TY_STRING) {
        /* Strings compare by content. */
        Expr *pair[2] = {*lhs, *rhs};
        if (op == TOK_EQ || op == TOK_NE) {
            Expr *call = rt_call(S, "__rt_str_eq", e->loc, pair, 2);
            if (op == TOK_NE) {
                Expr *not_ = new_node(S, EXPR_UNARY, e->loc, ty_bool);
                not_->u.unary.op = TOK_BANG;
                not_->u.unary.operand = call;
                call = not_;
            }
            *slot = call;
        } else {
            Expr *call = rt_call(S, "__rt_str_cmp", e->loc, pair, 2);
            Expr *zero = new_node(S, EXPR_INT, e->loc, ty_int);
            e->u.binary.lhs = call;
            e->u.binary.rhs = zero;
        }
        return set_type(*slot, ty_bool);
    }

    if ((op == TOK_EQ || op == TOK_NE) && (lt->kind == TY_MIXED || rt->kind == TY_MIXED)) {
        /* Strict, like PHP's ===: the same type and value. */
        Expr *a = to_mixed(S, *lhs), *b = to_mixed(S, *rhs);
        if (!a || !b) {
            report_not_mixable(!a ? *lhs : *rhs, "compared with a `mixed` value");
            return set_type(e, ty_error);
        }
        Expr *call = rt_call2(S, "__rt_mixed_eq", e->loc, a, b);
        if (op == TOK_NE) {
            Expr *not_ = new_node(S, EXPR_UNARY, e->loc, ty_bool);
            not_->u.unary.op = TOK_BANG;
            not_->u.unary.operand = call;
            call = not_;
        }
        *slot = call;
        return ty_bool;
    }

    if (is_cmp(op)) {
        bool eq = op == TOK_EQ || op == TOK_NE;
        if (type_is_numeric(lt) && type_is_numeric(rt)) {
            Type *t = unify_numeric(S, e, *lhs, *rhs, op);
            if (!t) return set_type(e, ty_error);
            widen_to(S, lhs, t);
            widen_to(S, rhs, t);
            return set_type(e, ty_bool);
        }
        if (eq && lt->kind == TY_BOOL && rt->kind == TY_BOOL) return set_type(e, ty_bool);
        if (lt->kind == TY_ENUM && lt == rt) return set_type(e, ty_bool);
        /* Objects compare by identity, with each other or with null. */
        bool lref = lt->kind == TY_CLASS || lt->kind == TY_PHPARRAY, rref = rt->kind == TY_CLASS || rt->kind == TY_PHPARRAY;
        if (eq && (lref || rref) &&
            (lt == rt || lt->kind == TY_NULL || rt->kind == TY_NULL || class_subtype(lt, rt) || class_subtype(rt, lt)))
            return set_type(e, ty_bool);
        bool lp = lt->kind == TY_PTR || lt->kind == TY_NULL, rp = rt->kind == TY_PTR || rt->kind == TY_NULL;
        if (lp && rp && (eq || (lt->kind == TY_PTR && rt->kind == TY_PTR)) &&
            (type_assignable(lt, rt) || type_assignable(rt, lt) ||
             (lt->kind == TY_PTR && rt->kind == TY_PTR && type_equiv(lt->base, rt->base))))
            return set_type(e, ty_bool);
        Diag *d = diag_start(DIAG_ERROR, "E0310", e->loc, "can't compare `%s` with `%s`", type_name(lt), type_name(rt));
        diag_span(d, (*lhs)->loc, "`%s`", type_name(lt));
        diag_span(d, (*rhs)->loc, "`%s`", type_name(rt));
        if (lt->kind == TY_MIXED || rt->kind == TY_MIXED)
            diag_help(d, "`mixed` values only compare with `==` and `!=`; convert with `as` first");
        else if (type_is_aggregate(lt)) diag_note(d, "structs and arrays can't be compared with operators");
        else if (lt->kind == TY_ERRUNION || rt->kind == TY_ERRUNION)
            diag_help(d, "unwrap the error union first, with `?` or `??`");
        else if (lt->kind == TY_ENUM || rt->kind == TY_ENUM)
            diag_help(d, "an enum compares only with the same enum; convert with `as` to compare numbers");
        else if (type_is_numeric(lt) != type_is_numeric(rt)) diag_help(d, "convert one side with `as`");
        diag_end(d);
        return set_type(e, ty_error);
    }

    /* & | ^ also work on bools (without short-circuiting). */
    if ((op == TOK_AMP || op == TOK_PIPE || op == TOK_CARET) && lt->kind == TY_BOOL && rt->kind == TY_BOOL)
        return set_type(e, ty_bool);

    if (!type_is_numeric(lt) || !type_is_numeric(rt)) {
        const Expr *bad = !type_is_numeric(lt) ? *lhs : *rhs;
        Diag *d = diag_start(DIAG_ERROR, "E0310", bad->loc, "`%s` needs numbers, found `%s`", token_kind_spelling(op),
                             type_name(bad->type));
        diag_label(d, "not a number");
        if (bad->type->kind == TY_BOOL && (op == TOK_PLUS || op == TOK_STAR))
            diag_help(d, "to count `true` as 1, convert with `as int`");
        else if (bad->type->kind == TY_ENUM)
            fix_append(S, d, bad, " as int", "enums don't do arithmetic; convert to an integer first");
        else if (bad->type->kind == TY_ERRUNION)
            diag_help(d, "unwrap the error union first, with `?` or `??`");
        else if (bad->type->kind == TY_STRING && op == TOK_PLUS)
            diag_help(d, "join strings with `.` (like PHP): `a . b`, or interpolate: \"{a}{b}\"");
        diag_end(d);
        return set_type(e, ty_error);
    }
    Type *t = unify_numeric(S, e, *lhs, *rhs, op);
    if (!t) return set_type(e, ty_error);
    if (type_is_float(t) && (op == TOK_PERCENT || op == TOK_AMP || op == TOK_PIPE || op == TOK_CARET)) {
        Diag *d = diag_start(DIAG_ERROR, "E0310", e->loc, "`%s` doesn't work on floats", token_kind_spelling(op));
        diag_label(d, "`%s` operands", type_name(t));
        diag_end(d);
        return set_type(e, ty_error);
    }
    widen_to(S, lhs, t);
    widen_to(S, rhs, t);
    return set_type(e, t);
}

/* E0350: `what` needs an error union operand. */
static bool require_errunion(const Expr *e, const char *what) {
    Type *t = e->type;
    if (t->kind == TY_ERRUNION) return true;
    Diag *d = diag_start(DIAG_ERROR, "E0350", e->loc, "%s needs an error union, found `%s`", what, type_name(t));
    diag_label(d, "this can't fail");
    diag_note(d, "error unions are written `T!E`, like `int!IoError`");
    diag_end(d);
    return false;
}

/* r ?? fallback: r's value, or the fallback if r is an error. */
static Type *check_coalesce(Sema *S, Expr *e, Type *hint) {
    Type *lt = check(S, &e->u.binary.lhs, NULL);
    Expr *lhs = e->u.binary.lhs;
    if (is_err(lt) || !require_value(S, lhs) || !require_errunion(lhs, "`??`")) {
        check(S, &e->u.binary.rhs, hint);
        return set_type(e, ty_error);
    }
    if (lt->base->kind == TY_VOID) {
        Diag *d = diag_start(DIAG_ERROR, "E0350", e->loc, "`??` needs a value to fall back from, but `%s` has none",
                             type_name(lt));
        diag_label(d, "`void` results have no value");
        diag_help(d, "check `->ok` instead, or pass the error on with `?`");
        diag_end(d);
        check(S, &e->u.binary.rhs, NULL);
        return set_type(e, ty_error);
    }
    check(S, &e->u.binary.rhs, lt->base);
    char why[160];
    snprintf(why, sizeof why, "this is `%s`, so the fallback must be a `%s`", type_name(lt), type_name(lt->base));
    coerce(S, &e->u.binary.rhs, lt->base, &lhs->loc, why);
    return set_type(e, lt->base);
}

/* r?: r's value, or return r's error from the current function. */
static Type *check_try(Sema *S, Expr *e) {
    Type *t = check(S, &e->u.try_expr, NULL);
    Expr *inner = e->u.try_expr;
    if (is_err(t) || !require_value(S, inner) || !require_errunion(inner, "`?`")) return set_type(e, ty_error);
    const Decl *fd = S->fn->decl;
    Type *rt = S->fn->type;
    Loc q = loc_at(e->loc.file, e->loc.offset + e->loc.len - 1, 1);
    if (S->in_defer) {
        Diag *d = diag_start(DIAG_ERROR, "E0346", q, "`?` can't be used inside `defer`");
        diag_label(d, "this would return from the function");
        diag_note(d, "deferred code runs while its block is already exiting, so it must run to its end");
        diag_end(d);
        return set_type(e, ty_error);
    }
    if (is_err(rt)) return set_type(e, t->base);
    if (rt->kind != TY_ERRUNION) {
        Diag *d = diag_start(DIAG_ERROR, "E0347", q, "`?` can only be used in a function that returns an error union");
        diag_label(d, "this returns the error from `%s`", fd->name);
        diag_span(d, ret_type_loc(fd), "`%s` returns `%s`", fd->name, type_name(rt));
        if (fd->u.func.ret && strcmp(fd->name, "main") != 0) {
            char fixed[256];
            snprintf(fixed, sizeof fixed, "%s!%s", type_name(rt), type_name(t->err));
            diag_fix(d, fd->u.func.ret->loc, fixed, "make `%s` return errors too", fd->name);
        } else if (t->base->kind != TY_VOID) {
            diag_help(d, "handle the error here: `??` gives a fallback value, and `->ok` says whether it succeeded");
        }
        diag_end(d);
        return set_type(e, ty_error);
    }
    if (rt->err != t->err) {
        Diag *d = diag_start(DIAG_ERROR, "E0348", q, "`?` can't pass on `%s` errors from a function that returns `%s` errors",
                             type_name(t->err), type_name(rt->err));
        diag_label(d, "this error is of type `%s`", type_name(t->err));
        diag_span(d, ret_type_loc(fd), "`%s` returns `%s`", fd->name, type_name(rt));
        diag_help(d, "check `->ok` and return one of `%s`'s errors yourself, like `return %s::...;`", type_name(rt->err),
                  type_name(rt->err));
        diag_end(d);
        return set_type(e, ty_error);
    }
    return set_type(e, t->base);
}

static Type *check_ternary(Sema *S, Expr *e, Type *hint) {
    require_bool(S, &e->u.ternary.cond, "the condition of `?:`");
    Expr **a = &e->u.ternary.then_expr, **b = &e->u.ternary.else_expr;
    check_operands(S, a, b, hint);
    Type *at = (*a)->type, *bt = (*b)->type;
    if (!both_values(S, *a, *b)) return set_type(e, ty_error);
    if (type_is_numeric(at) && type_is_numeric(bt)) {
        Type *t = unify_numeric(S, e, *a, *b, TOK_QUESTION);
        if (!t) return set_type(e, ty_error);
        widen_to(S, a, t);
        widen_to(S, b, t);
        return set_type(e, t);
    }
    if (type_assignable(bt, at)) return set_type(e, at);
    if (type_assignable(at, bt)) return set_type(e, bt);
    Diag *d = diag_start(DIAG_ERROR, "E0309", e->loc, "the two branches of `?:` have different types");
    diag_span(d, (*a)->loc, "this is `%s`", type_name(at));
    diag_span(d, (*b)->loc, "this is `%s`", type_name(bt));
    diag_end(d);
    return set_type(e, ty_error);
}

/* Checks a call's arguments against `fn`. The first `skip` arguments
 * (a method's object) are already checked and aren't counted in messages. */
static Type *check_args(Sema *S, Expr *e, Symbol *fn, size_t skip) {
    Expr *callee = e->u.call.callee;
    e->sym = fn;
    callee->sym = fn;
    add_callee(S, fn);
    size_t n = e->u.call.nargs, want = fn->nparams;
    bool ok = true;
    if (fn->is_variadic && n >= want) {
        /* Fixed parameters as usual; the rest get C's default promotions. */
        for (size_t i = 0; i < n; i++) {
            if (i < want) {
                check(S, &e->u.call.args[i], fn->param_types[i]);
                coerce(S, &e->u.call.args[i], fn->param_types[i], &fn->decl->u.func.params[i]->type->loc,
                       "declared parameter type");
                continue;
            }
            Expr *arg = e->u.call.args[i];
            Type *hint = arg->kind == EXPR_STRING ? type_ptr(ty_u8, true, false) : NULL;
            Type *t = check(S, &e->u.call.args[i], hint);
            arg = e->u.call.args[i];
            if (is_err(t) || !require_value(S, arg)) continue;
            if (t->kind == TY_ARRAY) {
                Diag *d = diag_start(DIAG_ERROR, "E0335", arg->loc, "arrays can't be passed by value");
                diag_label(d, "this is `%s`", type_name(t));
                diag_help(d, "pass a pointer to the first element: `&arr[0]`");
                diag_end(d);
            } else if (t->kind == TY_FLOAT && t->size == 4) {
                e->u.call.args[i] = wrap_cast(S, arg, ty_f64);   /* C promotes float to double */
            }
        }
        return set_type(e, fn->type);
    }
    if (n != want) {
        size_t un = n - skip, uwant = want - skip;   /* as written, without `this` */
        const char *what = !fn->owner ? "function" : fn->is_static ? "static method" : "method";
        Diag *d = fn->owner && strcmp(fn->name, "__construct") == 0
                      ? diag_start(DIAG_ERROR, "E0206", callee->loc, "`new %s(...)` takes %zu argument%s but %zu %s supplied",
                                   fn->owner->name, uwant, uwant == 1 ? "" : "s", un, un == 1 ? "was" : "were")
                      : diag_start(DIAG_ERROR, "E0206", callee->loc, "%s `%s` takes %zu argument%s but %zu %s supplied",
                                   what, fn->name, uwant, uwant == 1 ? "" : "s", un, un == 1 ? "was" : "were");
        diag_label(d, "expected %zu argument%s, found %zu", uwant, uwant == 1 ? "" : "s", un);
        diag_span(d, fn->loc, "`%s` is defined here", fn->name);
        for (size_t i = want; i < n && i < want + 3; i++) diag_span(d, e->u.call.args[i]->loc, "unexpected argument");
        if (n < want) {
            char missing[256] = "";
            size_t used = 0;
            for (size_t i = n; i < want && used < sizeof missing - 60; i++)
                used += (size_t)snprintf(missing + used, sizeof missing - used, "%s`%s: %s`", i > n ? ", " : "",
                                         fn->decl->u.func.params[i - skip]->name, type_name(fn->param_types[i]));
            diag_note(d, "missing: %s", missing);
        }
        diag_end(d);
        ok = false;
    }
    for (size_t i = skip; i < n; i++) {
        Type *pt = i < want ? fn->param_types[i] : NULL;
        check(S, &e->u.call.args[i], pt);
        if (pt) {
            const Param *p = fn->decl->u.func.params[i - skip];
            char why[160];
            snprintf(why, sizeof why, "parameter `%s` is declared as `%s`", p->name, type_name(pt));
            ok = coerce(S, &e->u.call.args[i], pt, &p->type->loc, why) && ok;
        }
    }
    (void)ok;
    return set_type(e, fn->type);
}

static void check_args_only(Sema *S, Expr *e) {
    for (size_t i = 0; i < e->u.call.nargs; i++) check(S, &e->u.call.args[i], NULL);
}

/* E0362 for methods */
/* Can the code being checked use a member declared in `owner`? */
static bool member_visible(Sema *S, bool is_private, bool is_protected, const Type *owner) {
    if (!is_private && !is_protected) return true;
    const Type *here = S->fn ? S->fn->owner : NULL;
    if (!here) return false;
    return is_private ? here == owner : class_subtype(here, owner);
}

static bool method_visible(Sema *S, const Symbol *m, Loc loc) {
    if (member_visible(S, m->is_private, m->is_protected, m->owner)) return true;
    const char *kind = m->is_private ? "private" : "protected";
    Diag *d = diag_start(DIAG_ERROR, "E0362", loc, "method `%s` of class `%s` is %s", m->name, m->owner->name, kind);
    diag_label(d, "%s method", kind);
    diag_span(d, m->loc, "declared `%s` here", kind);
    diag_note(d, "%s", m->is_private ? "private members can only be used by the class's own methods"
                                     : "protected members can only be used by the class and its subclasses");
    diag_end(d);
    return false;
}

/* E0364 */
static void report_unknown_method(Sema *S, const Type *ct, const char *name, Loc loc) {
    Diag *d = diag_start(DIAG_ERROR, "E0364", loc, "class `%s` has no method named `%s`", ct->name, name);
    diag_label(d, "unknown method");
    const char *names[64];
    size_t n = 0;
    for (size_t i = 0; i < ct->nmethods && n < 64; i++) names[n++] = ct->methods[i]->name;
    const char *guess = closest(name, names, n);
    if (guess) diag_fix(d, loc, guess, "a method with a similar name exists");
    if (find_field(ct, name)) diag_note(d, "`%s` is a field; fields can't be called", name);
    diag_span(d, ct->decl->name_loc, "`%s` is declared here", ct->name);
    (void)S;
    diag_end(d);
}

/* obj->method(args): the object becomes the first argument. */
static Type *check_method_call(Sema *S, Expr *e) {
    Expr *callee = e->u.call.callee;
    Type *ot = check(S, &callee->u.member.object, NULL);
    Expr *obj = callee->u.member.object;
    const char *name = callee->u.member.name;
    Loc name_loc = callee->u.member.name_loc;
    if (is_err(ot) || !require_value(S, obj)) {
        check_args_only(S, e);
        return set_type(e, ty_error);
    }
    if (ot->kind != TY_CLASS) {
        Diag *d = diag_start(DIAG_ERROR, "E0364", name_loc, "`->%s(...)` calls a method, but `%s` has no methods", name,
                             type_name(ot));
        diag_label(d, "not an object");
        diag_span(d, obj->loc, "this is `%s`", type_name(ot));
        if (ot->kind == TY_STRUCT) diag_help(d, "structs only hold data; use a `class` for methods, or call a function");
        diag_end(d);
        check_args_only(S, e);
        return set_type(e, ty_error);
    }
    Symbol *m = find_method(ot, name);
    if (!m) {
        report_unknown_method(S, ot, name, name_loc);
        check_args_only(S, e);
        return set_type(e, ty_error);
    }
    if (m->is_static) {
        Diag *d = diag_start(DIAG_ERROR, "E0364", name_loc, "`%s` is a static method, so it isn't called on an object",
                             name);
        diag_label(d, "static method");
        diag_span(d, m->loc, "declared `static` here");
        diag_help(d, "call it on the class: `%s::%s(...)`", ot->name, name);
        diag_end(d);
        check_args_only(S, e);
        return set_type(e, ty_error);
    }
    if (!method_visible(S, m, name_loc)) {
        check_args_only(S, e);
        return set_type(e, ty_error);
    }
    size_t n = e->u.call.nargs;
    Expr **args = arena_alloc(S->arena, (n + 1) * sizeof(Expr *));
    args[0] = obj;
    for (size_t i = 0; i < n; i++) args[i + 1] = e->u.call.args[i];
    e->u.call.args = args;
    e->u.call.nargs = n + 1;
    callee->kind = EXPR_IDENT;
    callee->u.name = m->name;
    callee->loc = name_loc;
    e->builtin = BUILTIN_METHOD;
    Type *t = check_args(S, e, m, 1);
    set_dispatch(S, e, ot, m);
    return t;
}

/* parent::method(args): the base class's version, called directly on `this`. */
static Type *check_parent_call(Sema *S, Expr *e) {
    Expr *callee = e->u.call.callee;
    const char *name = callee->u.scope.member;
    Type *base = S->fn && S->fn->owner ? S->fn->owner->base_class : NULL;
    if (!base || !S->fn->this_param) {
        Diag *d = diag_start(DIAG_ERROR, "E0374", callee->loc, "`parent::` needs a method of a class that extends another");
        diag_label(d, !S->fn || !S->fn->owner ? "not inside a method"
                      : !S->fn->this_param    ? "a static method has no `this`"
                                              : "this class has no base class");
        diag_end(d);
        check_args_only(S, e);
        return set_type(e, ty_error);
    }
    Symbol *m = find_method(base, name);
    if (!m) {
        report_unknown_method(S, base, name, callee->loc);
        check_args_only(S, e);
        return set_type(e, ty_error);
    }
    if (m->is_abstract) {
        Diag *d = diag_start(DIAG_ERROR, "E0374", callee->loc, "`%s::%s` is abstract, so there's nothing to call", base->name,
                             name);
        diag_label(d, "no body");
        diag_span(d, m->loc, "declared abstract here");
        diag_end(d);
        check_args_only(S, e);
        return set_type(e, ty_error);
    }
    if (!method_visible(S, m, callee->loc)) {
        check_args_only(S, e);
        return set_type(e, ty_error);
    }
    callee->kind = EXPR_IDENT;
    callee->u.name = m->name;
    if (m->is_static) return check_args(S, e, m, 0);
    size_t n = e->u.call.nargs;   /* `this` goes first */
    Expr **args = arena_alloc(S->arena, (n + 1) * sizeof(Expr *));
    args[0] = new_node(S, EXPR_THIS, callee->loc, S->fn->owner);
    args[0]->sym = S->fn->this_param;
    args[0]->checked = true;
    for (size_t i = 0; i < n; i++) args[i + 1] = e->u.call.args[i];
    e->u.call.args = args;
    e->u.call.nargs = n + 1;
    return check_args(S, e, m, 1);
}

/* Class::method(args) */
static Type *check_static_call(Sema *S, Expr *e, Type *ct) {
    Expr *callee = e->u.call.callee;
    const char *name = callee->u.scope.member;
    Symbol *m = find_method(ct, name);
    if (!m) {
        report_unknown_method(S, ct, name, callee->loc);
        check_args_only(S, e);
        return set_type(e, ty_error);
    }
    if (!m->is_static) {
        Diag *d = diag_start(DIAG_ERROR, "E0364", callee->loc, "`%s::%s` needs an object", ct->name, name);
        diag_label(d, "not a static method");
        diag_span(d, m->loc, "`%s` is declared here, without `static`", name);
        diag_help(d, "call it on an object: `obj->%s(...)`", name);
        diag_end(d);
        check_args_only(S, e);
        return set_type(e, ty_error);
    }
    if (!method_visible(S, m, callee->loc)) {
        check_args_only(S, e);
        return set_type(e, ty_error);
    }
    callee->kind = EXPR_IDENT;
    callee->u.name = m->name;
    return check_args(S, e, m, 0);
}


/* ---- generics ------------------------------------------------------------------- */

static void layout_struct(Sema *S, Type *st);
static void resolve_hierarchy(Sema *S, Type *ct);
static void layout_class(Sema *S, Type *ct);
static void declare_methods(Sema *S, Type *ct);
static void check_field_defaults(Sema *S, Type *ct);
static void inherit_defaults(Type *ct);
static void declare_function(Sema *S, Decl *d);

/* Generic declarations follow the same visibility rules as the rest;
 * `types` picks structs and classes, else functions. */
static const Decl *find_template(Sema *S, const char *name, bool types) {
    const char *sep = strstr(name, "::");
    int want = -1;
    if (sep) {
        char alias[128];
        snprintf(alias, sizeof alias, "%.*s", (int)(sep - name), name);
        want = module_by_alias(S, alias);
        if (want < 0) return NULL;
        name = sep + 2;
    }
    const Decl *found = NULL;
    for (size_t i = 0; i < S->templates.len; i++) {
        const Decl *d = S->templates.items[i];
        if ((d->kind != DECL_FUNC) != types || strcmp(d->name, name) != 0) continue;
        int mod = module_of(S, d->loc.file);
        if (want >= 0) {
            if (mod == want && d->vis != VIS_PRIVATE) return d;
            continue;
        }
        if (!visible(S, S->cur, mod, d->vis == VIS_PRIVATE)) continue;
        if (mod == S->cur) return d;
        if (!found) found = d;
    }
    return found;
}

/* "Box<int, u8*>" */
static const char *instance_name(Sema *S, const char *base, Type **types, size_t n) {
    char buf[512];
    size_t len = (size_t)snprintf(buf, sizeof buf, "%s<", base);
    for (size_t i = 0; i < n && len < sizeof buf; i++)
        len += (size_t)snprintf(buf + len, sizeof buf - len, "%s%s", i ? ", " : "", type_name(types[i]));
    if (len < sizeof buf) len += (size_t)snprintf(buf + len, sizeof buf - len, ">");
    if (len >= sizeof buf) len = sizeof buf - 1;
    return arena_strndup(S->arena, buf, len);
}

/* An assembler-friendly spelling of a name: `max<int*>` -> `max$Lint$P$G`. */
static const char *mangle(Sema *S, const char *name) {
    if (!strpbrk(name, "<>, *[]:!")) return name;
    size_t n = strlen(name);
    char *out = arena_alloc(S->arena, n * 2 + 1);
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        const char *r = c == '<' ? "$L" : c == '>' ? "$G" : c == ',' ? "$C" : c == '*' ? "$P" : c == '[' ? "$A"
                      : c == ']' ? "$E" : c == ':' ? "$S" : c == '!' ? "$X" : c == ' ' ? "" : NULL;
        if (!r) {
            out[j++] = c;
            continue;
        }
        while (*r) out[j++] = *r++;
    }
    out[j] = '\0';
    return out;
}

static bool same_targs(Type **a, Type **b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!type_equiv(a[i], b[i])) return false;
    return true;
}

/* "<T, U>" for messages */
static const char *tparams_text(Sema *S, const Decl *d) {
    char buf[256];
    size_t len = 1;
    buf[0] = '<';
    for (size_t i = 0; i < d->ntparams && len < sizeof buf; i++)
        len += (size_t)snprintf(buf + len, sizeof buf - len, "%s%s", i ? ", " : "", d->tparams[i]);
    if (len < sizeof buf) len += (size_t)snprintf(buf + len, sizeof buf - len, ">");
    if (len >= sizeof buf) len = sizeof buf - 1;
    return arena_strndup(S->arena, buf, len);
}

static bool too_deep(Sema *S, Loc loc, const Decl *tmpl) {
    if (S->inst_depth < 32) return false;
    Diag *d = diag_start(DIAG_ERROR, "E0380", loc, "generic `%s` keeps instantiating itself with bigger types", tmpl->name);
    diag_label(d, "each instance of `%s` needs a new, bigger one", tmpl->name);
    diag_note(d, "a generic declaration that uses itself with bigger type arguments, like `Node<Node<T>>`, never ends");
    diag_end(d);
    return true;
}

/* E0376 */
static bool check_targ_count(Sema *S, const Decl *tmpl, size_t n, Loc loc) {
    if (n == tmpl->ntparams) return true;
    Diag *d = diag_start(DIAG_ERROR, "E0376", loc, "`%s` takes %zu type argument%s but %zu %s supplied", tmpl->name,
                         tmpl->ntparams, tmpl->ntparams == 1 ? "" : "s", n, n == 1 ? "was" : "were");
    diag_label(d, "expected %zu type argument%s", tmpl->ntparams, tmpl->ntparams == 1 ? "" : "s");
    diag_span(d, tmpl->name_loc, "`%s%s` is declared here", tmpl->name, tparams_text(S, tmpl));
    diag_end(d);
    return false;
}

static Type *type_instance(Sema *S, const Decl *tmpl, Type **types, Loc loc) {
    size_t n = tmpl->ntparams;
    for (size_t i = 0; i < S->instances.len; i++) {
        Instance *in = S->instances.items[i];
        if (in->tmpl == tmpl && same_targs(in->targs, types, n)) return in->type;
    }
    const char *name = instance_name(S, tmpl->name, types, n);
    if (too_deep(S, loc, tmpl)) return ty_error;
    Decl *d = clone_instance(S->arena, tmpl, types);
    d->name = name;
    Type *t;
    if (tmpl->kind == DECL_STRUCT) {
        t = type_new_struct(name, d);
    } else {
        t = type_new_class(name, d);
        t->class_id = S->classes++;
    }
    t->tmpl = tmpl;
    t->targs = types;
    t->ntargs = n;
    t->inst_loc = loc;
    d->type = t;
    Instance *in = ARENA_NEW(S->arena, Instance);
    in->tmpl = tmpl;
    in->targs = types;
    in->type = t;
    ptrvec_push(&S->instances, in);
    ptrvec_push(&S->structs, t);

    /* Catch up on the passes that already ran. */
    int saved_cur = S->cur;
    S->cur = module_of(S, d->loc.file);
    S->inst_depth++;
    diag_push_context(loc, "in `%s`", name);
    if (t->kind == TY_STRUCT) {
        if (S->phase >= PH_STRUCTS) layout_struct(S, t);
    } else {
        if (S->phase >= PH_HIERARCHY) resolve_hierarchy(S, t);
        if (S->phase >= PH_CLASS_LAYOUT) layout_class(S, t);
        if (S->phase >= PH_METHODS) declare_methods(S, t);
        if (S->phase >= PH_DEFAULTS) check_field_defaults(S, t);
        if (S->phase >= PH_INHERIT) inherit_defaults(t);
    }
    diag_pop_context();
    S->inst_depth--;
    S->cur = saved_cur;
    return t;
}

/* `Box<int>` as written: name plus type arguments. */
static Type *resolve_generic(Sema *S, const char *name, TypeExpr *const *targs, size_t n, Loc loc) {
    const Decl *tmpl = find_template(S, name, true);
    if (!tmpl) {
        Type *t = find_struct(S, name);
        if (!t) {
            report_unknown_type(S, loc, name);
            return ty_error;
        }
        Diag *d = diag_start(DIAG_ERROR, "E0377", loc, "`%s` isn't generic", name);
        diag_label(d, "takes no type arguments");
        if (t->decl) diag_span(d, t->decl->name_loc, "`%s` is declared here", name);
        diag_help(d, "write just `%s`", name);
        diag_end(d);
        return ty_error;
    }
    if (!check_targ_count(S, tmpl, n, loc)) return ty_error;
    Type **types = arena_alloc(S->arena, n * sizeof *types);
    bool ok = true;
    for (size_t i = 0; i < n; i++) {
        types[i] = resolve_type(S, targs[i], false);
        ok &= !is_err(types[i]);
    }
    return ok ? type_instance(S, tmpl, types, loc) : ty_error;
}

/* E0375, or E0322 */
static void report_missing_targs(Sema *S, Loc loc, const char *name) {
    const Decl *tmpl = find_template(S, name, true);
    if (!tmpl) {
        report_unknown_type(S, loc, name);
        return;
    }
    Diag *d = diag_start(DIAG_ERROR, "E0375", loc, "`%s` is generic, so it needs type arguments", name);
    diag_label(d, "missing `<...>`");
    diag_span(d, tmpl->name_loc, "`%s%s` is declared here", tmpl->name, tparams_text(S, tmpl));
    char fixed[160];
    snprintf(fixed, sizeof fixed, "%s<%s>", name, tmpl->ntparams == 1 ? "int" : "int, ...");
    diag_fix(d, loc, fixed, "say what it holds");
    diag_end(d);
}

static Symbol *func_instance(Sema *S, const Decl *tmpl, Type **types, Loc loc) {
    size_t n = tmpl->ntparams;
    for (size_t i = 0; i < S->instances.len; i++) {
        Instance *in = S->instances.items[i];
        if (in->tmpl == tmpl && same_targs(in->targs, types, n)) return in->fn;
    }
    const char *name = instance_name(S, tmpl->name, types, n);
    if (too_deep(S, loc, tmpl)) return NULL;
    Decl *d = clone_instance(S->arena, tmpl, types);
    d->name = name;
    int saved_cur = S->cur;
    S->cur = module_of(S, d->loc.file);
    diag_push_context(loc, "in `%s`", name);
    S->inst_depth++;
    declare_function(S, d);
    S->inst_depth--;
    diag_pop_context();
    S->cur = saved_cur;
    Symbol *fn = d->sym;
    fn->is_instance = true;
    fn->inst_loc = loc;
    Instance *in = ARENA_NEW(S->arena, Instance);
    in->tmpl = tmpl;
    in->targs = types;
    in->fn = fn;
    ptrvec_push(&S->instances, in);
    return fn;
}

static bool literal_is_float(const Expr *e) {
    if (e->kind == EXPR_UNARY) e = e->u.unary.operand;
    return e->kind == EXPR_FLOAT;
}

static int tparam_index(const Decl *tmpl, const char *name) {
    for (size_t i = 0; i < tmpl->ntparams; i++)
        if (strcmp(tmpl->tparams[i], name) == 0) return (int)i;
    return -1;
}

typedef struct {
    const Decl *tmpl;
    Type **bound;
    Loc *bound_at;      /* the argument each was learned from */
    bool *from_literal; /* bound[k] came from a literal (and may still widen to float) */
    int conflict;       /* the type parameter with two different types, or -1 */
    Type *other;
    Loc other_at;
} Infer;

/* Matches parameter type `te` against argument type `at`, learning what
 * the type parameters must be. */
static void unify(Sema *S, Infer *inf, const TypeExpr *te, Type *at, Loc arg_loc) {
    if (is_err(at) || inf->conflict >= 0) return;
    switch (te->kind) {
    case TYPEX_NAMED: {
        if (te->ntargs == 0) {
            int k = tparam_index(inf->tmpl, te->u.name);
            if (k < 0 || at->kind == TY_NULL) return;
            if (!inf->bound[k]) {
                inf->bound[k] = at;
                inf->bound_at[k] = arg_loc;
            } else if (!type_equiv(inf->bound[k], at)) {
                inf->conflict = k;
                inf->other = at;
                inf->other_at = arg_loc;
            }
            return;
        }
        /* Box<T> against Box<int>, or against a class that extends it */
        const Decl *want = find_template(S, te->u.name, true);
        if (!want) return;
        for (Type *c = at; c; c = c->kind == TY_CLASS ? c->base_class : NULL) {
            Type *hit = c->tmpl == want ? c : NULL;
            for (size_t i = 0; !hit && c->kind == TY_CLASS && i < c->nifaces; i++)
                if (c->ifaces[i]->tmpl == want) hit = c->ifaces[i];
            if (hit && hit->ntargs == te->ntargs) {
                for (size_t i = 0; i < te->ntargs; i++) unify(S, inf, te->targs[i], hit->targs[i], arg_loc);
                return;
            }
        }
        return;
    }
    case TYPEX_PTR:
        if (at->kind == TY_PTR) unify(S, inf, te->u.pointee, at->base, arg_loc);
        return;
    case TYPEX_ARRAY:
        if (at->kind == TY_ARRAY) unify(S, inf, te->u.array.elem, at->base, arg_loc);
        return;
    case TYPEX_ERROR_UNION:
        if (at->kind == TY_ERRUNION) {
            unify(S, inf, te->u.error_union.value, at->base, arg_loc);
            unify(S, inf, te->u.error_union.error, at->err, arg_loc);
        }
        return;
    default:
        return;
    }
}

/* The instance a call to generic function `tmpl` uses: from explicit type
 * arguments, `max<int>(a, b)`, or learned from the arguments. NULL after
 * reporting an error. */
static Symbol *instantiate_call(Sema *S, Expr *e, const Decl *tmpl) {
    Expr *callee = e->u.call.callee;
    size_t n = tmpl->ntparams;
    Type **types = arena_alloc(S->arena, n * sizeof *types);
    if (callee->ntargs) {
        if (!check_targ_count(S, tmpl, callee->ntargs, callee->loc)) return NULL;
        bool ok = true;
        for (size_t i = 0; i < n; i++) {
            types[i] = resolve_type(S, callee->targs[i], false);
            ok &= !is_err(types[i]);
        }
        return ok ? func_instance(S, tmpl, types, e->loc) : NULL;
    }

    Infer inf = {tmpl, types, arena_alloc(S->arena, n * sizeof(Loc)), arena_alloc(S->arena, n * sizeof(bool)), -1, NULL,
                 {0}};
    size_t m = e->u.call.nargs < tmpl->u.func.nparams ? e->u.call.nargs : tmpl->u.func.nparams;
    bool arg_error = false;
    /* Literals adapt to their context, so they only decide a type parameter
     * nothing else does: max(x, 1) with an i32 x is max<i32>. */
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < m; i++) {
            const TypeExpr *pt = tmpl->u.func.params[i]->type;
            if (is_literal(e->u.call.args[i]) != (pass == 1)) continue;
            int k = -1;
            if (pass == 1) {
                k = pt->kind == TYPEX_NAMED && !pt->ntargs ? tparam_index(tmpl, pt->u.name) : -1;
                if (k < 0) continue;
                if (types[k]) {   /* max(1, 2.5) is max<f64> */
                    if (inf.from_literal[k] && type_is_int(types[k]) && literal_is_float(e->u.call.args[i])) {
                        types[k] = ty_f64;
                        inf.bound_at[k] = e->u.call.args[i]->loc;
                    }
                    continue;
                }
                Type *at = check(S, &e->u.call.args[i], NULL);
                types[k] = at;
                inf.bound_at[k] = e->u.call.args[i]->loc;
                inf.from_literal[k] = true;
                continue;
            }
            Type *at = check(S, &e->u.call.args[i], NULL);
            e->u.call.args[i]->checked = true;
            if (is_err(at)) arg_error = true;
            unify(S, &inf, pt, at, e->u.call.args[i]->loc);
        }
    }
    if (inf.conflict >= 0) {
        const char *tp = tmpl->tparams[inf.conflict];
        Diag *d = diag_start(DIAG_ERROR, "E0379", inf.other_at, "`%s` can't be both `%s` and `%s` in this call to `%s`",
                             tp, type_name(types[inf.conflict]), type_name(inf.other), tmpl->name);
        diag_label(d, "this makes `%s` = `%s`", tp, type_name(inf.other));
        diag_span(d, inf.bound_at[inf.conflict], "this makes `%s` = `%s`", tp, type_name(types[inf.conflict]));
        diag_span(d, tmpl->name_loc, "`%s%s` is declared here", tmpl->name, tparams_text(S, tmpl));
        diag_help(d, "convert one of them with `as`, or say which type you mean: `%s<%s>(...)`", tmpl->name,
                  type_name(types[inf.conflict]));
        diag_end(d);
        return NULL;
    }
    if (arg_error) return NULL;
    for (size_t k = 0; k < n; k++) {
        if (types[k]) continue;
        Diag *d = diag_start(DIAG_ERROR, "E0378", callee->loc, "can't tell what `%s` is in this call to `%s`",
                             tmpl->tparams[k], tmpl->name);
        diag_label(d, "type parameter `%s` unknown", tmpl->tparams[k]);
        diag_span(d, tmpl->tparam_locs[k], "`%s` is declared here", tmpl->tparams[k]);
        diag_note(d, "type parameters are learned from the arguments; none of them decides `%s`", tmpl->tparams[k]);
        char example[160];
        size_t len = 0;
        for (size_t j = 0; j < n && len < 100; j++) len += (size_t)snprintf(example + len, sizeof example - len, "%sint", j ? ", " : "");
        diag_help(d, "write the type arguments, like `%s<%s>(...)`", tmpl->name, example);
        diag_end(d);
        return NULL;
    }
    return func_instance(S, tmpl, types, e->loc);
}

/* A call that names a generic function. Returns false if `e` isn't one. */
static bool check_generic_call(Sema *S, Expr *e, Type **out) {
    Expr *callee = e->u.call.callee;
    const Decl *tmpl;
    char full[256];
    const char *name = callee->u.name;
    if (callee->kind == EXPR_SCOPE) {
        snprintf(full, sizeof full, "%s::%s", callee->u.scope.scope, callee->u.scope.member);
        name = full;
        tmpl = find_template(S, full, false);
    } else {
        Symbol *other = NULL;
        tmpl = find_in(S, &S->funcs, name, &other) ? NULL : find_template(S, name, false);
    }
    if (!tmpl) {
        if (!callee->ntargs) return false;
        Symbol *plain = callee->kind == EXPR_SCOPE ? NULL : find_func(S, name);
        if (!plain && callee->kind == EXPR_IDENT) return false;   /* reported as not found */
        Diag *d = diag_start(DIAG_ERROR, "E0377", callee->loc, "`%s` isn't a generic function", name);
        diag_label(d, "takes no type arguments");
        if (plain) diag_span(d, plain->loc, "`%s` is declared here", name);
        diag_end(d);
        check_args_only(S, e);
        *out = set_type(e, ty_error);
        return true;
    }
    Symbol *fn = instantiate_call(S, e, tmpl);
    if (!fn) {
        check_args_only(S, e);
        *out = set_type(e, ty_error);
        return true;
    }
    callee->kind = EXPR_IDENT;
    callee->u.name = fn->name;
    callee->sym = fn;
    *out = check_args(S, e, fn, 0);
    return true;
}

static Type *check_call(Sema *S, Expr *e) {
    Expr *callee = e->u.call.callee;
    if (callee->kind == EXPR_MEMBER) return check_method_call(S, e);
    if (callee->kind == EXPR_SCOPE && strcmp(callee->u.scope.scope, "parent") == 0 && module_by_alias(S, "parent") < 0 &&
        !find_struct(S, "parent"))
        return check_parent_call(S, e);
    if (callee->kind == EXPR_SCOPE &&
        (strstr(callee->u.scope.scope, "::") || module_by_alias(S, callee->u.scope.scope) < 0)) {
        Type *ct = find_struct(S, callee->u.scope.scope);
        if (ct && ct->kind == TY_CLASS) return check_static_call(S, e, ct);
    }
    if (callee->kind == EXPR_IDENT || callee->kind == EXPR_SCOPE) {
        Type *r;
        if (check_generic_call(S, e, &r)) return r;
    }
    if (callee->kind == EXPR_SCOPE) {   /* module::function(...) */
        Symbol *qf = find_qualified(S, &S->funcs, callee->loc, callee->u.scope.scope, callee->u.scope.member, "function");
        if (!qf) {
            for (size_t i = 0; i < e->u.call.nargs; i++) check(S, &e->u.call.args[i], NULL);
            return set_type(e, ty_error);
        }
        const char *name = qf->name;
        callee->kind = EXPR_IDENT;
        callee->u.name = name;
        callee->sym = qf;
    }
    if (callee->kind != EXPR_IDENT) {
        Diag *d = diag_start(DIAG_ERROR, "E0204", callee->loc, "this can't be called");
        diag_label(d, "not a function or method");
        diag_note(d, "function pointers aren't supported yet");
        diag_end(d);
        check_args_only(S, e);
        return set_type(e, ty_error);
    }
    Symbol *other_fn = NULL;
    Symbol *fn = callee->sym && callee->sym->kind == SYM_FUNC ? callee->sym : NULL;
    if (!fn) fn = find_in(S, &S->funcs, callee->u.name, &other_fn);
    if (fn && other_fn) {
        report_ambiguous(S, callee->loc, callee->u.name, fn, other_fn);
        for (size_t i = 0; i < e->u.call.nargs; i++) check(S, &e->u.call.args[i], NULL);
        return set_type(e, ty_error);
    }

    /* len(array) is a compile-time constant. */
    if (!fn && strcmp(callee->u.name, "len") == 0 && !find_var(S, "len")) {
        if (e->u.call.nargs != 1) {
            Diag *d = diag_start(DIAG_ERROR, "E0206", callee->loc, "`len` takes 1 argument but %zu were supplied",
                                 e->u.call.nargs);
            diag_label(d, "expected 1 argument");
            diag_end(d);
            return set_type(e, ty_error);
        }
        Type *t = check(S, &e->u.call.args[0], NULL);
        if (is_err(t)) return set_type(e, ty_error);
        if (t->kind == TY_PHPARRAY) {
            *e = *rt_call1(S, "__rt_array_len", e->u.call.args[0]);
            return ty_usize;
        }
        if (t->kind == TY_MIXED) {
            *e = *rt_call1(S, "__rt_array_len", as_array_expr(S, e->u.call.args[0]));
            return ty_usize;
        }
        if (t->kind == TY_STRING) {   /* known at run time: the string's len field */
            Expr *arg = e->u.call.args[0];
            e->kind = EXPR_MEMBER;
            e->u.member.object = arg;
            e->u.member.name = "len";
            e->u.member.name_loc = arg->loc;
            e->field_offset = 8;
            e->via_pointer = false;
            return set_type(e, ty_usize);
        }
        if (t->kind != TY_ARRAY) {
            Diag *d = diag_start(DIAG_ERROR, "E0332", e->u.call.args[0]->loc, "`len` needs an array, found `%s`",
                                 type_name(t));
            diag_label(d, "not an array");
            if (t->kind == TY_PTR) diag_note(d, "a pointer doesn't know how many elements it points to");
            diag_end(d);
            return set_type(e, ty_error);
        }
        e->kind = EXPR_INT;
        e->u.int_val = t->len;
        return set_type(e, ty_usize);
    }

    /* hash_of(x): a 64-bit hash of a number, bool, string, pointer, object
     * (its identity) or mixed value; std/map's Map<K, V> uses it. */
    if (!fn && strcmp(callee->u.name, "hash_of") == 0 && !find_var(S, "hash_of")) {
        if (e->u.call.nargs != 1) {
            Diag *d = diag_start(DIAG_ERROR, "E0206", callee->loc, "`hash_of` takes 1 argument but %zu were supplied",
                                 e->u.call.nargs);
            diag_label(d, "expected 1 argument");
            diag_end(d);
            check_args_only(S, e);
            return set_type(e, ty_error);
        }
        Type *t = check(S, &e->u.call.args[0], NULL);
        Expr *arg = e->u.call.args[0];
        if (is_err(t) || !require_value(S, arg)) return set_type(e, ty_error);
        const char *rt;
        if (t->kind == TY_STRING) {
            rt = "__rt_str_hash";
        } else if (t->kind == TY_MIXED) {
            rt = "__rt_key_hash";
        } else if (t->kind == TY_FLOAT) {
            rt = "__rt_f64_hash";
            if (t->size == 4) arg = wrap_cast(S, arg, ty_f64);
        } else if (type_in_gpr(t)) {
            rt = "__rt_int_hash";
            arg = wrap_cast(S, arg, ty_u64);
        } else {
            Diag *d = diag_start(DIAG_ERROR, "E0383", arg->loc, "`hash_of` can't hash a value of type `%s`", type_name(t));
            diag_label(d, "not hashable");
            diag_note(d, "it hashes numbers, bools, enums, strings, pointers, objects (by identity) and `mixed`");
            if (t->kind == TY_STRUCT && t->nfields >= 2) {
                char *text = loc_text(S, arg->loc);
                diag_help(d, "combine the hashes of the fields, for example `hash_of(%s->%s) ^ hash_of(%s->%s) * 31`", text,
                          t->fields[0].name, text, t->fields[1].name);
            }
            diag_end(d);
            return set_type(e, ty_error);
        }
        *e = *rt_call1(S, rt, arg);
        return set_type(e, ty_u64);
    }

    /* free(p) releases memory from alloc. */
    if (!fn && strcmp(callee->u.name, "free") == 0 && !find_var(S, "free")) {
        e->builtin = BUILTIN_FREE;
        e->sym = find_runtime_func(S, "__rt_free");
        add_callee(S, e->sym);
        if (e->u.call.nargs != 1) {
            Diag *d = diag_start(DIAG_ERROR, "E0206", callee->loc, "`free` takes 1 argument but %zu were supplied",
                                 e->u.call.nargs);
            diag_label(d, "expected a pointer from `alloc`");
            diag_end(d);
            return set_type(e, ty_error);
        }
        Type *t = check(S, &e->u.call.args[0], NULL);
        if (!is_err(t) && t->kind != TY_PTR && t->kind != TY_NULL) {
            Diag *d = diag_start(DIAG_ERROR, "E0309", e->u.call.args[0]->loc, "`free` needs a pointer, found `%s`",
                                 type_name(t));
            diag_label(d, "expected a pointer");
            diag_end(d);
        }
        return set_type(e, ty_void);
    }

    if (!fn) {
        Symbol *v = find_var(S, callee->u.name);
        Diag *d;
        if (v) {
            v->reads++;
            d = diag_start(DIAG_ERROR, "E0204", callee->loc, "`%s` is a variable, not a function", callee->u.name);
            diag_label(d, "called like a function");
            diag_span(d, v->loc, "`%s` is declared here", v->name);
            diag_help(d, "to read the variable, remove the parentheses");
        } else {
            d = diag_start(DIAG_ERROR, "E0201", callee->loc, "cannot find function `%s`", callee->u.name);
            diag_label(d, "not found");
            const char *names[256];
            size_t n = 0;
            for (size_t i = 0; i < S->funcs.len && n < 256; i++) {
                Symbol *f = S->funcs.items[i];
                if (!f->is_runtime && !f->owner && visible(S, S->cur, f->module, f->is_private)) names[n++] = f->name;
            }
            const char *guess = closest(callee->u.name, names, n);
            if (guess) {
                diag_span(d, find_func(S, guess)->loc, "a function with a similar name is defined here");
                diag_fix(d, callee->loc, guess, "a function with a similar name exists");
            }
        }
        diag_end(d);
        for (size_t i = 0; i < e->u.call.nargs; i++) check(S, &e->u.call.args[i], NULL);
        return set_type(e, ty_error);
    }

    return check_args(S, e, fn, 0);
}

static Type *check_member(Sema *S, Expr *e) {
    Type *t = check(S, &e->u.member.object, NULL);
    Expr *obj = e->u.member.object;
    if (is_err(t)) return set_type(e, ty_error);
    Type *st = t;
    e->via_pointer = false;
    if (t->kind == TY_PTR && (t->base->kind == TY_STRUCT || t->base->kind == TY_STRING || t->base->kind == TY_ERRUNION ||
                              t->base->kind == TY_MIXED)) {
        st = t->base;
        e->via_pointer = true;
    }
    if (st->kind == TY_MIXED && S->mods[S->cur]->is_runtime) {   /* the runtime's m->tag, m->bits */
        const Field *f = find_field(st, e->u.member.name);
        if (f) {
            e->field_offset = f->offset;
            return set_type(e, f->type);
        }
    }
    if (st->kind == TY_MIXED) {
        Diag *d = diag_start(DIAG_ERROR, "E0318", e->u.member.name_loc, "a `mixed` value has no fields");
        diag_label(d, "can't use `->` here");
        diag_span(d, obj->loc, "this is `mixed`");
        diag_help(d, "convert it to the type it holds first, like `(m as Point)->%s`", e->u.member.name);
        diag_end(d);
        return set_type(e, ty_error);
    }
    if (st->kind == TY_ERRUNION) {   /* r->ok, r->value, r->error */
        const char *name = e->u.member.name;
        const Field *f = NULL;
        if (strcmp(name, "ok") == 0) f = find_field(st, "is_err");
        else if (strcmp(name, "error") == 0) f = find_field(st, "err");
        else if (strcmp(name, "value") == 0) f = find_field(st, "value");   /* absent for void!E */
        if (!f) {
            const char *tname = type_name(st);
            Diag *d = diag_start(DIAG_ERROR, "E0317", e->u.member.name_loc, "`%s` has no field named `%s`",
                                 tname ? tname : "?", name ? name : "?");   /* (never NULL; quiets gcc -fsanitize) */
            diag_label(d, "unknown field");
            if (strcmp(name, "value") == 0) diag_note(d, "a `void` result has no value; check `->ok` instead");
            else diag_note(d, "an error union has `ok` (bool), `value` and `error`");
            diag_end(d);
            return set_type(e, ty_error);
        }
        e->field_offset = f->offset;
        if (strcmp(name, "ok") == 0) {
            e->builtin = BUILTIN_EU_OK;
            return set_type(e, ty_bool);
        }
        e->builtin = strcmp(name, "value") == 0 ? BUILTIN_EU_VALUE : BUILTIN_EU_ERROR;
        return set_type(e, f->type);
    }
    if (st->kind == TY_CLASS) {   /* obj->field: through the reference */
        e->via_pointer = true;
        const Field *f = find_field(st, e->u.member.name);
        if (!f) {
            Symbol *m = find_method(st, e->u.member.name);
            if (m) {
                Diag *d = diag_start(DIAG_ERROR, "E0205", e->u.member.name_loc, "`%s` is a method, not a field",
                                     e->u.member.name);
                diag_label(d, "method used without calling it");
                char *text = loc_text(S, e->loc);
                if (text && m->nparams == 1 && !m->is_static) {
                    size_t n = strlen(text) + 3;
                    char *fixed = arena_alloc(S->arena, n);
                    snprintf(fixed, n, "%s()", text);
                    diag_fix(d, e->loc, fixed, "call it");
                }
                diag_end(d);
                return set_type(e, ty_error);
            }
            report_unknown_field(S, st, e->u.member.name, e->u.member.name_loc);
            return set_type(e, ty_error);
        }
        if (!member_visible(S, f->is_private, f->is_protected, f->owner)) {
            const char *kind = f->is_private ? "private" : "protected";
            Diag *d = diag_start(DIAG_ERROR, "E0362", e->u.member.name_loc, "field `%s` of class `%s` is %s", f->name,
                                 f->owner->name, kind);
            diag_label(d, "%s field", kind);
            diag_span(d, f->loc, "declared `%s` here", kind);
            diag_note(d, "%s", f->is_private ? "private members can only be used by the class's own methods"
                                             : "protected members can only be used by the class and its subclasses");
            diag_end(d);
            return set_type(e, ty_error);
        }
        e->field_offset = f->offset;
        return set_type(e, f->type);
    }
    if (st->kind == TY_STRING) {   /* string is {const u8* ptr; usize len} */
        const char *name = e->u.member.name;
        if (strcmp(name, "ptr") == 0) {
            e->field_offset = 0;
            return set_type(e, type_ptr(ty_u8, true, false));
        }
        if (strcmp(name, "len") == 0) {
            e->field_offset = 8;
            return set_type(e, ty_usize);
        }
        Diag *d = diag_start(DIAG_ERROR, "E0317", e->u.member.name_loc, "`string` has no field named `%s`", name);
        diag_label(d, "unknown field");
        diag_note(d, "a string has `ptr` (const u8*) and `len` (usize)");
        diag_end(d);
        return set_type(e, ty_error);
    }
    if (st->kind != TY_STRUCT) {
        Diag *d = diag_start(DIAG_ERROR, "E0318", e->u.member.name_loc, "`->` needs a struct or object, found `%s`",
                             type_name(t));
        diag_label(d, "unknown field");
        diag_span(d, obj->loc, "this is `%s`", type_name(t));
        if (t->kind == TY_PTR && t->base->kind == TY_PTR) diag_help(d, "dereference it first: `(*p)->field`");
        diag_end(d);
        return set_type(e, ty_error);
    }
    const Field *f = find_field(st, e->u.member.name);
    if (!f) {
        report_unknown_field(S, st, e->u.member.name, e->u.member.name_loc);
        return set_type(e, ty_error);
    }
    e->field_offset = f->offset;
    return set_type(e, f->type);
}

/* The key of a[key] on an `array`, as mixed; NULL after an error. */
static Expr *array_key(Sema *S, Expr **slot) {
    Type *kt = check(S, slot, NULL);
    Expr *k = *slot;
    if (is_err(kt) || !require_value(S, k)) return NULL;
    if (!type_is_intlike(kt) && kt->kind != TY_STRING && kt->kind != TY_MIXED && kt->kind != TY_BOOL) {
        Diag *d = diag_start(DIAG_ERROR, "E0366", k->loc, "array keys must be ints or strings, found `%s`", type_name(kt));
        diag_label(d, "invalid key");
        diag_end(d);
        return NULL;
    }
    return to_mixed(S, k);
}

static void report_append_as_value(const Expr *e) {
    Diag *d = diag_start(DIAG_ERROR, "E0366", e->loc, "`a[]` can only be assigned to");
    diag_label(d, "appends a new element");
    diag_help(d, "write `a[] = value;` to append, or `a[key]` to read an element");
    diag_end(d);
}

static Type *check_index_rest(Sema *S, Expr **slot, Type *bt);

static Type *check_index(Sema *S, Expr **slot) {
    Expr *e = *slot;
    Type *bt = check(S, &e->u.index.base, NULL);
    return check_index_rest(S, slot, bt);
}

/* a[i], with `a` already checked. */
static Type *check_index_rest(Sema *S, Expr **slot, Type *bt) {
    Expr *e = *slot;
    if (!e->u.index.index) {
        if (bt->kind == TY_PHPARRAY || bt->kind == TY_MIXED) report_append_as_value(e);
        else if (!is_err(bt)) {
            Diag *d = diag_start(DIAG_ERROR, "E0319", e->loc, "`[]` appends to an `array`, but this is `%s`", type_name(bt));
            diag_label(d, "can't append to this");
            diag_end(d);
        }
        return set_type(e, ty_error);
    }
    if (bt->kind == TY_PHPARRAY || bt->kind == TY_MIXED) {   /* a[key]: null if missing */
        Expr *key = array_key(S, &e->u.index.index);
        if (!key) return set_type(e, ty_error);
        *slot = rt_call2(S, "__rt_array_get", e->loc, as_array_expr(S, e->u.index.base), key);
        return ty_mixed;
    }
    Type *it = check(S, &e->u.index.index, ty_usize);
    Expr *base = e->u.index.base, *index = e->u.index.index;
    if (is_err(bt) || is_err(it)) return set_type(e, ty_error);
    if (!require_value(S, index)) return set_type(e, ty_error);
    if (!type_is_int(it)) {
        Diag *d = diag_start(DIAG_ERROR, "E0309", index->loc, "array index must be an integer, found `%s`", type_name(it));
        diag_label(d, "expected an integer");
        diag_end(d);
        return set_type(e, ty_error);
    }
    if (bt->kind == TY_ARRAY) {
        ConstVal cv;
        if (const_eval(S, index, &cv, false)) {
            bool negative = it->is_signed && (int64_t)cv.bits < 0;
            if (negative || cv.bits >= bt->len) {
                Diag *d = diag_start(DIAG_ERROR, "E0320", index->loc, "index %s%lld is out of bounds for `%s`",
                                     "", (long long)(int64_t)cv.bits, type_name(bt));
                diag_label(d, "out of bounds");
                diag_note(d, "valid indexes are 0 to %llu", (unsigned long long)(bt->len - 1));
                diag_span(d, base->loc, "this array has %llu element%s", (unsigned long long)bt->len,
                          bt->len == 1 ? "" : "s");
                diag_end(d);
                return set_type(e, ty_error);
            }
        }
        return set_type(e, bt->base);
    }
    if (bt->kind == TY_PTR && bt->base->kind != TY_VOID) return set_type(e, bt->base);
    if (bt->kind == TY_STRING) return set_type(e, ty_u8);   /* bounds-checked at run time */
    Diag *d = diag_start(DIAG_ERROR, "E0319", base->loc, "can't index into a value of type `%s`", type_name(bt));
    diag_label(d, bt->kind == TY_PTR ? "`void*` doesn't say what it points to" : "not an array or pointer");
    diag_end(d);
    return set_type(e, ty_error);
}

static Type *check_cast(Sema *S, Expr *e) {
    Type *to = resolve_type(S, e->u.cast.type, false);
    /* A literal too big for int (like 0xFFFF_FFFF_FFFF_FFFF) is read as u64;
     * other literals are ints, so `300 as u8` truncates like any cast. */
    Expr *lit = e->u.cast.expr;
    Type *hint = lit->kind == EXPR_INT && lit->u.int_val > (uint64_t)INT64_MAX ? ty_u64 : NULL;
    Type *from = check(S, &e->u.cast.expr, hint);
    Expr *inner = e->u.cast.expr;
    if (is_err(to) || is_err(from)) return set_type(e, ty_error);
    if (!require_value(S, inner)) return set_type(e, ty_error);
    if (from->kind == TY_MIXED && to->kind != TY_MIXED) {   /* checked at run time */
        Expr *conv = from_mixed(S, inner, to, e->loc);
        if (!conv) {
            Diag *d = diag_start(DIAG_ERROR, "E0366", e->loc, "a `mixed` value can't hold a `%s`", type_name(to));
            diag_label(d, "invalid cast");
            diag_note(d, "`mixed` holds null, bools, ints, floats, strings, arrays and objects");
            diag_end(d);
            return set_type(e, ty_error);
        }
        *e = *conv;
        return e->type;
    }
    if (to->kind == TY_MIXED) {
        Expr *conv = to_mixed(S, inner);
        if (!conv) {
            report_not_mixable(inner, "converted to `mixed`");
            return set_type(e, ty_error);
        }
        *e = *conv;
        return e->type;
    }
    if (from->kind == TY_CLASS && to->kind == TY_CLASS && !type_assignable(from, to) && type_castable(from, to)) {
        /* A downcast: checked at run time (null passes). */
        Expr *name = new_node(S, EXPR_STRING, e->loc, ty_string);
        name->u.str.data = to->name;
        name->u.str.len = strlen(to->name);
        Expr *args[5] = {wrap_cast(S, inner, ty_u64), desc_addr(S, to, e->loc), bool_const(S, e->loc, to->is_interface),
                         name, where_string(S, e->loc)};
        *e = *wrap_cast(S, rt_call(S, "__rt_cast_object", e->loc, args, 5), to);
        return to;
    }
    /* `0xB8000 as u16*`: the literal must not be range-checked as int if it fits u64. */
    if (type_castable(from, to)) return set_type(e, to);
    Diag *d = diag_start(DIAG_ERROR, "E0313", e->loc, "can't cast `%s` to `%s`", type_name(from), type_name(to));
    diag_label(d, "invalid cast");
    if (to->kind == TY_BOOL && type_is_numeric(from))
        fix_append(S, d, inner, " != 0", "to get a `bool`, compare instead");
    else if ((from->kind == TY_PTR && type_is_int(to)) || (type_is_int(from) && to->kind == TY_PTR))
        diag_help(d, "only 64-bit integers can hold an address; use `usize` or `u64`");
    else if (type_is_aggregate(from) || type_is_aggregate(to))
        diag_note(d, "structs and arrays can't be cast");
    else if (from->kind == TY_CLASS && to->kind == TY_CLASS)
        diag_note(d, "`%s` and `%s` aren't related: neither class extends the other", from->name, to->name);
    diag_end(d);
    return set_type(e, ty_error);
}

/* string { ptr: p, len: n } */
static Type *check_string_lit(Sema *S, Expr *e) {
    for (size_t i = 0; i < e->u.struct_lit.n; i++) {
        FieldInit *fi = e->u.struct_lit.fields[i];
        bool is_ptr = strcmp(fi->name, "ptr") == 0, is_len = strcmp(fi->name, "len") == 0;
        if (!is_ptr && !is_len) {
            Diag *d = diag_start(DIAG_ERROR, "E0317", fi->loc, "`string` has no field named `%s`", fi->name);
            diag_label(d, "unknown field");
            diag_note(d, "a string has `ptr` (const u8*) and `len` (usize)");
            diag_end(d);
            check(S, &fi->value, NULL);
            continue;
        }
        Type *t = is_ptr ? type_ptr(ty_u8, true, false) : ty_usize;
        check(S, &fi->value, t);
        coerce(S, &fi->value, t, NULL, NULL);
    }
    return set_type(e, ty_string);
}

/* mixed { tag: t, bits: b }: the runtime builds mixed values from parts. */
static Type *check_mixed_lit(Sema *S, Expr *e) {
    if (!S->mods[S->cur]->is_runtime) {
        Diag *d = diag_start(DIAG_ERROR, "E0365", e->loc, "`mixed` values can't be built from parts");
        diag_label(d, "the representation of `mixed` is private to the runtime");
        diag_help(d, "convert a value instead: `mixed m = 5;`");
        diag_end(d);
        for (size_t i = 0; i < e->u.struct_lit.n; i++) check(S, &e->u.struct_lit.fields[i]->value, NULL);
        return set_type(e, ty_error);
    }
    for (size_t i = 0; i < e->u.struct_lit.n; i++) {
        FieldInit *fi = e->u.struct_lit.fields[i];
        check(S, &fi->value, ty_u64);
        coerce(S, &fi->value, ty_u64, NULL, NULL);
    }
    return set_type(e, ty_mixed);
}

static Type *check_struct_lit(Sema *S, Expr *e) {
    if (strcmp(e->u.struct_lit.type_name, "string") == 0) return check_string_lit(S, e);
    if (strcmp(e->u.struct_lit.type_name, "mixed") == 0) return check_mixed_lit(S, e);
    Type *st;
    if (e->ntargs) {
        st = resolve_generic(S, e->u.struct_lit.type_name, e->targs, e->ntargs, e->loc);
        if (is_err(st)) {
            for (size_t i = 0; i < e->u.struct_lit.n; i++) check(S, &e->u.struct_lit.fields[i]->value, NULL);
            return set_type(e, ty_error);
        }
    } else {
        st = find_struct(S, e->u.struct_lit.type_name);
    }
    if (st && st->kind == TY_CLASS) {
        Diag *d = diag_start(DIAG_ERROR, "E0363", e->loc, "`%s` is a class, not a struct", st->name);
        diag_label(d, "can't be built with `{ ... }`");
        diag_help(d, "create objects with `new %s(...)`", st->name);
        diag_end(d);
        st = NULL;
    } else if (st && st->kind != TY_STRUCT) {
        Diag *d = diag_start(DIAG_ERROR, "E0352", e->loc, "`%s` is an enum, not a struct", st->name);
        diag_label(d, "can't be built with `{ ... }`");
        diag_help(d, "name a variant instead, like `%s::%s`", st->name, st->nvariants ? st->variants[0].name : "Variant");
        diag_end(d);
        st = NULL;
    }
    if (!st) {
        if (!find_struct(S, e->u.struct_lit.type_name)) {
            Loc name_loc = e->loc;
            name_loc.len = (uint32_t)strlen(e->u.struct_lit.type_name);
            report_missing_targs(S, name_loc, e->u.struct_lit.type_name);
        }
        for (size_t i = 0; i < e->u.struct_lit.n; i++) check(S, &e->u.struct_lit.fields[i]->value, NULL);
        return set_type(e, ty_error);
    }
    for (size_t i = 0; i < e->u.struct_lit.n; i++) {
        FieldInit *fi = e->u.struct_lit.fields[i];
        const Field *f = find_field(st, fi->name);
        for (size_t j = 0; j < i; j++) {
            if (strcmp(e->u.struct_lit.fields[j]->name, fi->name) == 0) {
                Diag *d = diag_start(DIAG_ERROR, "E0324", fi->loc, "field `%s` is set twice", fi->name);
                diag_label(d, "set again here");
                diag_span(d, e->u.struct_lit.fields[j]->loc, "first set here");
                diag_end(d);
                break;
            }
        }
        if (!f) {
            report_unknown_field(S, st, fi->name, fi->loc);
            check(S, &fi->value, NULL);
            continue;
        }
        check(S, &fi->value, f->type);
        char why[160];
        snprintf(why, sizeof why, "field `%s` is declared as `%s`", f->name, type_name(f->type));
        coerce(S, &fi->value, f->type, &f->loc, why);
    }
    return set_type(e, st);
}

/* [a, b, k => v] as an `array`: created, then filled by runtime calls. */
static Type *check_php_array_lit(Sema *S, Expr *e) {
    Symbol *tmp = new_symbol(S, SYM_LOCAL, "array", e->loc);
    tmp->type = ty_phparray;
    e->u.array.tmp = tmp;
    e->u.array.create = rt_call1(S, "__rt_array_new", u64_const(S, e->loc, e->u.array.n));
    e->u.array.calls = arena_alloc(S->arena, (e->u.array.n ? e->u.array.n : 1) * sizeof(Expr *));
    e->u.array.ncalls = 0;
    bool ok = true;
    for (size_t i = 0; i < e->u.array.n; i++) {
        ArrayEntry *ent = e->u.array.entries[i];
        Expr *key = NULL;
        if (ent->key && !(key = array_key(S, &ent->key))) ok = false;
        Type *vt = check(S, &ent->value, NULL);
        if (is_err(vt) || !require_value(S, ent->value)) {
            ok = false;
            continue;
        }
        Expr *v = to_mixed(S, ent->value);
        if (!v) {
            report_not_mixable(ent->value, "stored in an array");
            ok = false;
            continue;
        }
        if (!ok) continue;
        Expr *self = new_node(S, EXPR_IDENT, e->loc, ty_phparray);
        self->u.name = "array";
        self->sym = tmp;
        if (key) {
            Expr *args[3] = {self, key, v};
            e->u.array.calls[e->u.array.ncalls++] = rt_call(S, "__rt_array_set", ent->value->loc, args, 3);
        } else {
            e->u.array.calls[e->u.array.ncalls++] = rt_call2(S, "__rt_array_push", ent->value->loc, self, v);
        }
    }
    return set_type(e, ok ? ty_phparray : ty_error);
}

static Type *check_array_lit(Sema *S, Expr *e, Type *hint) {
    bool has_keys = false;
    for (size_t i = 0; i < e->u.array.n; i++) has_keys |= e->u.array.entries[i]->key != NULL;
    if (!hint || hint->kind != TY_ARRAY || has_keys) return check_php_array_lit(S, e);
    if (e->u.array.n != hint->len) {
        Diag *d = diag_start(DIAG_ERROR, "E0325", e->loc, "expected %llu element%s, found %zu",
                             (unsigned long long)hint->len, hint->len == 1 ? "" : "s", e->u.array.n);
        diag_label(d, "%zu element%s", e->u.array.n, e->u.array.n == 1 ? "" : "s");
        diag_note(d, "the type is `%s`; every element must be listed", type_name(hint));
        diag_end(d);
    }
    for (size_t i = 0; i < e->u.array.n; i++) {
        check(S, &e->u.array.entries[i]->value, hint->base);
        coerce(S, &e->u.array.entries[i]->value, hint->base, NULL, NULL);
    }
    return set_type(e, hint);
}

/* Enum::Variant folds to the variant's value, typed as the enum. */
static Type *check_enum_value(Sema *S, Expr *e, Type *et) {
    const char *scope = e->u.scope.scope, *name = e->u.scope.member;
    Loc scope_loc = e->loc;
    scope_loc.len = (uint32_t)strlen(scope);
    if (!et) {
        report_unknown_type(S, scope_loc, scope);
        return set_type(e, ty_error);
    }
    if (et->kind != TY_ENUM) {
        Diag *d = diag_start(DIAG_ERROR, "E0352", scope_loc, "`%s` is a struct, not an enum", scope);
        diag_label(d, "`::` names an enum variant or something in a module");
        diag_span(d, et->decl->name_loc, "`%s` is declared here", et->name);
        diag_end(d);
        return set_type(e, ty_error);
    }
    for (size_t i = 0; i < et->nvariants; i++) {
        if (strcmp(et->variants[i].name, name) == 0) {
            e->kind = EXPR_INT;
            e->u.int_val = (uint64_t)et->variants[i].value;
            return set_type(e, et);
        }
    }
    Loc name_loc = e->loc;
    name_loc.offset += name_loc.len - (uint32_t)strlen(name);
    name_loc.col += name_loc.len - (uint32_t)strlen(name);
    name_loc.len = (uint32_t)strlen(name);
    Diag *d = diag_start(DIAG_ERROR, "E0352", name_loc, "enum `%s` has no variant named `%s`", et->name, name);
    diag_label(d, "unknown variant");
    const char *names[64];
    size_t n = 0;
    for (size_t i = 0; i < et->nvariants && n < 64; i++) names[n++] = et->variants[i].name;
    const char *guess = closest(name, names, n);
    if (guess) {
        diag_fix(d, name_loc, guess, "a variant with a similar name exists");
    } else if (n) {
        char list[256] = "";
        size_t used = 0;
        for (size_t i = 0; i < n && used < sizeof list - 40; i++)
            used += (size_t)snprintf(list + used, sizeof list - used, "%s`%s`", i ? ", " : "", names[i]);
        diag_note(d, "the variants are %s", list);
    }
    diag_span(d, et->decl->name_loc, "`%s` is declared here", et->name);
    diag_end(d);
    return set_type(e, ty_error);
}

/* new Class(args): allocates the object, sets field defaults, then runs
 * __construct(args) if the class has one. */
static Type *check_new(Sema *S, Expr *e) {
    Type *ct = resolve_type(S, e->u.new_.type, false);
    if (is_err(ct)) {
        for (size_t i = 0; i < e->u.new_.nargs; i++) check(S, &e->u.new_.args[i], NULL);
        return set_type(e, ty_error);
    }
    if (ct->kind != TY_CLASS) {
        Diag *d = diag_start(DIAG_ERROR, "E0363", e->u.new_.type->loc, "`new` needs a class, found `%s`", type_name(ct));
        diag_label(d, "not a class");
        if (ct->kind == TY_STRUCT)
            diag_help(d, "structs are values: write `%s { field: value, ... }`, or use `alloc(%s)` for heap memory",
                      ct->name, ct->name);
        else
            diag_help(d, "for heap memory holding a `%s`, use `alloc(%s)`", type_name(ct), type_name(ct));
        diag_end(d);
        for (size_t i = 0; i < e->u.new_.nargs; i++) check(S, &e->u.new_.args[i], NULL);
        return set_type(e, ty_error);
    }
    if (ct->is_abstract) {
        Diag *d = diag_start(DIAG_ERROR, "E0373", e->u.new_.type->loc, "can't create an object of %s `%s`",
                             ct->is_interface ? "interface" : "abstract class", ct->name);
        diag_label(d, "%s", ct->is_interface ? "an interface only describes methods" : "abstract");
        diag_span(d, ct->decl->name_loc, "declared %s here", ct->is_interface ? "as an interface" : "`abstract`");
        diag_help(d, "create an object of a class that %s it", ct->is_interface ? "implements" : "extends");
        diag_end(d);
        for (size_t i = 0; i < e->u.new_.nargs; i++) check(S, &e->u.new_.args[i], NULL);
        return set_type(e, ty_error);
    }
    e->u.new_.alloc = find_runtime_func(S, "__rt_alloc");
    add_callee(S, e->u.new_.alloc);
    for (size_t i = 0; i < ct->nfields; i++)   /* array fields start as new empty arrays */
        if (ct->fields[i].init && ct->fields[i].init->kind == EXPR_ARRAY_LIT)
            add_callee(S, find_runtime_func(S, "__rt_array_new"));
    Symbol *tmp = new_symbol(S, SYM_LOCAL, "this", e->loc);
    tmp->type = ct;
    e->u.new_.tmp = tmp;
    Symbol *ctor = find_method(ct, "__construct");
    if (!ctor) {
        if (e->u.new_.nargs) {
            Diag *d = diag_start(DIAG_ERROR, "E0206", e->u.new_.args[0]->loc, "class `%s` has no `__construct`, so `new %s` takes no arguments",
                                 ct->name, ct->name);
            diag_label(d, "unexpected argument%s", e->u.new_.nargs == 1 ? "" : "s");
            diag_span(d, ct->decl->name_loc, "`%s` is declared here", ct->name);
            diag_help(d, "add `public function __construct(...)` to the class to take arguments");
            diag_end(d);
            for (size_t i = 0; i < e->u.new_.nargs; i++) check(S, &e->u.new_.args[i], NULL);
        }
        return set_type(e, ct);
    }
    if (!method_visible(S, ctor, e->loc)) return set_type(e, ct);
    /* The __construct call, with the new object as `this`. */
    Expr *self = new_node(S, EXPR_IDENT, e->loc, ct);
    self->u.name = "this";
    self->sym = tmp;
    Expr *callee = new_node(S, EXPR_IDENT, e->u.new_.type->loc, ty_error);
    callee->u.name = ctor->name;
    Expr *call = new_node(S, EXPR_CALL, e->loc, ty_void);
    call->u.call.callee = callee;
    call->u.call.nargs = e->u.new_.nargs + 1;
    call->u.call.args = arena_alloc(S->arena, call->u.call.nargs * sizeof(Expr *));
    call->u.call.args[0] = self;
    for (size_t i = 0; i < e->u.new_.nargs; i++) call->u.call.args[i + 1] = e->u.new_.args[i];
    check_args(S, call, ctor, 1);
    e->u.new_.ctor = call;
    return set_type(e, ct);
}

/* x instanceof C: is x an object of class C (or a subclass), or one that
 * implements interface C? False for null. */
static Type *check_instanceof(Sema *S, Expr **slot) {
    Expr *e = *slot;
    Type *ct = resolve_type(S, e->u.cast.type, false);
    Type *t = check(S, &e->u.cast.expr, NULL);
    Expr *x = e->u.cast.expr;
    if (is_err(ct) || is_err(t) || !require_value(S, x)) return set_type(e, ty_error);
    if (ct->kind != TY_CLASS) {
        Diag *d = diag_start(DIAG_ERROR, "E0313", e->u.cast.type->loc, "`instanceof` needs a class or interface, found `%s`",
                             type_name(ct));
        diag_label(d, "not a class");
        diag_end(d);
        return set_type(e, ty_error);
    }
    if (t->kind != TY_CLASS && t->kind != TY_MIXED) {
        Diag *d = diag_start(DIAG_ERROR, "E0313", x->loc, "`instanceof` needs an object, found `%s`", type_name(t));
        diag_label(d, "not an object");
        diag_end(d);
        return set_type(e, ty_error);
    }
    Expr *args[3];
    args[0] = t->kind == TY_MIXED ? x : wrap_cast(S, x, ty_u64);
    args[1] = desc_addr(S, ct, e->loc);
    args[2] = bool_const(S, e->loc, ct->is_interface);
    *slot = rt_call(S, t->kind == TY_MIXED ? "__rt_mixed_instanceof" : "__rt_instanceof", e->loc, args, 3);
    return ty_bool;
}

static void report_assign_as_value(const Expr *e) {
    Diag *d = diag_start(DIAG_ERROR, "E0210", e->loc, "an assignment can't be used as a value");
    diag_label(d, "assignment used as a value");
    diag_note(d, "in watrlang, assignment is a statement, not an expression");
    if (e->u.binary.op == TOK_ASSIGN) {
        const SrcFile *f = e->loc.file;
        uint32_t from = e->u.binary.lhs->loc.offset + e->u.binary.lhs->loc.len;
        uint32_t to = e->u.binary.rhs->loc.offset;
        for (uint32_t i = from; i < to && i < f->len; i++) {
            if (f->text[i] == '=') {
                diag_fix(d, loc_at(f, i, 1), "==", "if you meant to compare, use `==`");
                break;
            }
        }
    } else {
        diag_help(d, "do the assignment on its own line, then use the variable");
    }
    diag_end(d);
}

static Type *check(Sema *S, Expr **slot, Type *hint) {
    Expr *e = *slot;
    if (e->checked) return e->type;
    S->cur_loc = e->loc;
    if (hint && is_err(hint)) hint = NULL;
    if (hint && hint->kind == TY_ERRUNION) hint = hint->base;   /* literals take the value type */
    switch (e->kind) {
    case EXPR_INT:
    case EXPR_CHAR:
        return check_int_literal(S, e, hint, false);
    case EXPR_FLOAT:
        return set_type(e, hint && type_is_float(hint) ? hint : ty_f64);
    case EXPR_BOOL:
        return set_type(e, ty_bool);
    case EXPR_NULL:
        return set_type(e, ty_null);
    case EXPR_IDENT:
        return check_ident(S, e, true);
    case EXPR_UNARY:
        return check_unary(S, e, hint);
    case EXPR_POSTFIX:
        return check_unary(S, e, hint);
    case EXPR_BINARY:
        return check_binary(S, slot, hint);
    case EXPR_TERNARY:
        return check_ternary(S, e, hint);
    case EXPR_CALL:
        return check_call(S, e);
    case EXPR_MEMBER:
        return check_member(S, e);
    case EXPR_INDEX:
        return check_index(S, slot);
    case EXPR_CAST:
        return check_cast(S, e);
    case EXPR_STRUCT_LIT:
        return check_struct_lit(S, e);
    case EXPR_ARRAY_LIT:
        return check_array_lit(S, e, hint);
    case EXPR_SIZEOF: {
        Type *t = resolve_type(S, e->u.sizeof_type, false);
        if (is_err(t)) return set_type(e, ty_error);
        e->kind = EXPR_INT;
        e->u.int_val = t->size;
        return set_type(e, ty_usize);
    }
    case EXPR_ASSIGN:
        report_assign_as_value(e);
        return set_type(e, ty_error);
    case EXPR_SCOPE: {   /* Enum::Variant, module::Enum::Variant, or module::global */
        const char *scope = e->u.scope.scope;
        if (strstr(scope, "::") || module_by_alias(S, scope) < 0) {
            Type *et = find_struct(S, scope);
            if (et || strstr(scope, "::")) return check_enum_value(S, e, et);
        }
        Symbol *g = find_qualified(S, &S->globals, e->loc, e->u.scope.scope, e->u.scope.member, "global variable");
        if (!g) return set_type(e, ty_error);
        e->kind = EXPR_IDENT;
        e->u.name = g->name;
        e->sym = g;
        g->reads++;
        return set_type(e, g->type);
    }

    case EXPR_STRING:
        /* A literal is a `string`, or, where a byte pointer is expected
         * (C functions), a pointer to its NUL-terminated bytes. */
        if (hint && hint->kind == TY_PTR && type_is_int(hint->base) && hint->base->size == 1)
            return set_type(e, hint);
        return set_type(e, ty_string);
    case EXPR_INTERP:
        return check_interp(S, slot);
    case EXPR_THIS:
        if (S->fn && S->fn->this_param) {
            e->sym = S->fn->this_param;
            return set_type(e, S->fn->owner);
        } else {
            Diag *d = diag_start(DIAG_ERROR, "E0361", e->loc, "`this` can only be used inside a method");
            if (S->fn && S->fn->owner) {
                diag_label(d, "`%s` is static, so it has no `this`", S->fn->name);
                diag_span(d, S->fn->loc, "declared `static` here");
            } else {
                diag_label(d, "not inside a class method");
            }
            diag_end(d);
            return set_type(e, ty_error);
        }

    case EXPR_NEW:
        return check_new(S, e);
    case EXPR_ALLOC: {
        /* alloc(T) or alloc(T, n): zeroed memory for n values of T. */
        e->sym = find_runtime_func(S, "__rt_alloc_manual");
        add_callee(S, e->sym);
        Type *t = resolve_type(S, e->u.alloc.type, false);
        if (e->u.alloc.count) {
            Type *ct = check(S, &e->u.alloc.count, ty_usize);
            if (!is_err(ct) && !type_is_int(ct)) {
                Diag *d = diag_start(DIAG_ERROR, "E0309", e->u.alloc.count->loc, "`alloc` count must be an integer, found `%s`",
                                     type_name(ct));
                diag_label(d, "expected an integer");
                diag_end(d);
            }
        }
        if (is_err(t)) return set_type(e, ty_error);
        if (t->size == 0) {
            Diag *d = diag_start(DIAG_ERROR, "E0330", e->u.alloc.type->loc, "can't allocate a zero-sized type");
            diag_label(d, "`%s` has no size", type_name(t));
            diag_end(d);
            return set_type(e, ty_error);
        }
        return set_type(e, type_ptr(t, false, false));
    }
    case EXPR_TRY:
        return check_try(S, e);
    case EXPR_INSTANCEOF:
        return check_instanceof(S, slot);
    }
    return set_type(e, ty_error);
}

/* An lvalue that's safe to evaluate twice: no calls, no ++/--. */
static bool pure_lvalue(const Expr *e) {
    switch (e->kind) {
    case EXPR_IDENT: case EXPR_INT: case EXPR_CHAR: return true;
    case EXPR_MEMBER: return pure_lvalue(e->u.member.object);
    case EXPR_INDEX: return pure_lvalue(e->u.index.base) && pure_lvalue(e->u.index.index);
    case EXPR_UNARY: return e->u.unary.op == TOK_STAR && pure_lvalue(e->u.unary.operand);
    default: return false;
    }
}

static TokenKind compound_to_binary(TokenKind op) {
    switch (op) {
    case TOK_PLUS_ASSIGN:    return TOK_PLUS;
    case TOK_MINUS_ASSIGN:   return TOK_MINUS;
    case TOK_STAR_ASSIGN:    return TOK_STAR;
    case TOK_SLASH_ASSIGN:   return TOK_SLASH;
    case TOK_PERCENT_ASSIGN: return TOK_PERCENT;
    case TOK_AMP_ASSIGN:     return TOK_AMP;
    case TOK_PIPE_ASSIGN:    return TOK_PIPE;
    case TOK_CARET_ASSIGN:   return TOK_CARET;
    case TOK_SHL_ASSIGN:     return TOK_SHL;
    case TOK_SHR_ASSIGN:     return TOK_SHR;
    default:                 return TOK_ERROR;
    }
}

/* a[key] = v and a[] = v on an `array`: become runtime calls. */
static void check_array_store(Sema *S, Expr *e) {
    Expr *target = e->u.binary.lhs;
    TokenKind op = e->u.binary.op;
    if (op != TOK_ASSIGN) {
        Diag *d = diag_start(DIAG_ERROR, "E0366", e->loc, "`%s` doesn't work on array elements", token_kind_spelling(op));
        diag_label(d, "elements are `mixed`");
        diag_help(d, "read, convert and store explicitly, like `a[k] = (a[k] as int) + 1;`");
        diag_end(d);
        if (target->u.index.index) check(S, &target->u.index.index, NULL);
        check(S, &e->u.binary.rhs, NULL);
        return;
    }
    Expr *arr = as_array_expr(S, target->u.index.base);
    Expr *key = NULL;
    if (target->u.index.index) {
        key = array_key(S, &target->u.index.index);
        if (!key) {
            check(S, &e->u.binary.rhs, NULL);
            return;
        }
    }
    Type *vt = check(S, &e->u.binary.rhs, NULL);
    Expr *v = e->u.binary.rhs;
    if (is_err(vt) || !require_value(S, v)) return;
    Expr *mv = to_mixed(S, v);
    if (!mv) {
        report_not_mixable(v, "stored in an array");
        return;
    }
    Expr *call;
    if (key) {
        Expr *args[3] = {arr, key, mv};
        call = rt_call(S, "__rt_array_set", e->loc, args, 3);
    } else {
        call = rt_call2(S, "__rt_array_push", e->loc, arr, mv);
    }
    *e = *call;
}

static void check_assign(Sema *S, Expr *e) {
    TokenKind op = e->u.binary.op;
    set_type(e, ty_void);
    if (op == TOK_DOT_ASSIGN) {
        /* s .= x  is  s = s . x  (the target must be simple, since it's used twice). */
        Expr *target = e->u.binary.lhs;
        if (target->kind == EXPR_INDEX) {   /* a[k] .= x on an array */
            Type *bt = check(S, &target->u.index.base, NULL);
            if (bt->kind == TY_PHPARRAY || bt->kind == TY_MIXED) {
                check_array_store(S, e);
                return;
            }
            Type *tt0 = check_index_rest(S, &e->u.binary.lhs, bt);
            (void)tt0;
        }
        Type *tt = target->kind == EXPR_IDENT ? check_ident(S, target, true)
                   : target->kind == EXPR_INDEX ? e->u.binary.lhs->type
                                                : check(S, &e->u.binary.lhs, NULL);
        target = e->u.binary.lhs;
        if (is_err(tt)) {
            check(S, &e->u.binary.rhs, NULL);
            return;
        }
        if (tt->kind != TY_STRING) {
            Diag *d = diag_start(DIAG_ERROR, "E0310", target->loc, "`.=` needs a string, found `%s`", type_name(tt));
            diag_label(d, "not a string");
            diag_end(d);
            check(S, &e->u.binary.rhs, NULL);
            return;
        }
        if (!pure_lvalue(target)) {
            Diag *d = diag_start(DIAG_ERROR, "E0339", target->loc, "the target of `.=` must be a variable or field");
            diag_label(d, "this would be evaluated twice");
            diag_help(d, "store it in a variable first");
            diag_end(d);
            return;
        }
        if (!check_target(target, "append to")) return;
        Type *rt = check(S, &e->u.binary.rhs, ty_string);
        if (is_err(rt) || !require_value(S, e->u.binary.rhs)) return;
        Expr *rhs = stringify(S, e->u.binary.rhs, "appended with `.=`");
        if (is_err(rhs->type)) return;
        Expr *pair[2] = {target, rhs};
        e->u.binary.op = TOK_ASSIGN;
        e->u.binary.rhs = rt_call(S, "__rt_concat", e->loc, pair, 2);
        return;
    }
    if (e->u.binary.rhs->kind == EXPR_ASSIGN) {
        Diag *d = diag_start(DIAG_ERROR, "E0211", e->u.binary.rhs->loc, "assignments can't be chained");
        diag_label(d, "second assignment");
        diag_span(d, e->u.binary.lhs->loc, "first assignment target");
        diag_help(d, "assign each variable in its own statement");
        diag_end(d);
        return;
    }
    Expr *target = e->u.binary.lhs;
    if (target->kind == EXPR_IDENT && !find_var(S, target->u.name) && find_func(S, target->u.name)) {
        Symbol *fn = find_func(S, target->u.name);
        Diag *d = diag_start(DIAG_ERROR, "E0209", target->loc, "can't assign to `%s` because it's a function", fn->name);
        diag_label(d, "not a variable");
        diag_span(d, fn->loc, "`%s` is defined here", fn->name);
        diag_end(d);
        check(S, &e->u.binary.rhs, NULL);
        return;
    }
    Type *tt;
    if (target->kind == EXPR_INDEX) {
        Type *bt = check(S, &target->u.index.base, NULL);
        if (bt->kind == TY_PHPARRAY || bt->kind == TY_MIXED) {
            check_array_store(S, e);
            return;
        }
        tt = check_index_rest(S, &e->u.binary.lhs, bt);
    } else {
        tt = target->kind == EXPR_IDENT ? check_ident(S, target, op != TOK_ASSIGN) : check(S, &e->u.binary.lhs, NULL);
    }
    target = e->u.binary.lhs;
    bool target_ok = check_target(target, "assign to");

    if (op == TOK_ASSIGN) {
        check(S, &e->u.binary.rhs, target_ok ? tt : NULL);
        if (target_ok) coerce(S, &e->u.binary.rhs, tt, NULL, NULL);
        return;
    }

    TokenKind bop = compound_to_binary(op);
    bool shift = bop == TOK_SHL || bop == TOK_SHR;
    Type *rt = check(S, &e->u.binary.rhs, shift ? NULL : tt);
    if (!target_ok || is_err(tt) || is_err(rt)) return;
    if (!require_value(S, e->u.binary.rhs)) return;
    if (tt->kind == TY_PTR && (bop == TOK_PLUS || bop == TOK_MINUS)) {
        if (!type_is_int(rt) || tt->base->kind == TY_VOID) {
            Diag *d = diag_start(DIAG_ERROR, "E0310", e->loc, "`%s` on a pointer needs an integer offset",
                                 token_kind_spelling(op));
            diag_span(d, e->u.binary.rhs->loc, "this is `%s`", type_name(rt));
            diag_end(d);
        }
        return;
    }
    bool bool_ok = tt->kind == TY_BOOL && (bop == TOK_AMP || bop == TOK_PIPE || bop == TOK_CARET);
    if (!type_is_numeric(tt) && !bool_ok) {
        Diag *d = diag_start(DIAG_ERROR, "E0310", target->loc, "`%s` needs a number, found `%s`",
                             token_kind_spelling(op), type_name(tt));
        diag_label(d, "not a number");
        diag_end(d);
        return;
    }
    if (shift) {
        if (!type_is_int(tt) || !type_is_int(rt)) {
            Diag *d = diag_start(DIAG_ERROR, "E0310", e->loc, "shifts need integers");
            diag_label(d, "`%s` and `%s`", type_name(tt), type_name(rt));
            diag_end(d);
        }
        return;
    }
    if (type_is_float(tt) && (bop == TOK_PERCENT || bop == TOK_AMP || bop == TOK_PIPE || bop == TOK_CARET)) {
        Diag *d = diag_start(DIAG_ERROR, "E0310", e->loc, "`%s` doesn't work on floats", token_kind_spelling(op));
        diag_label(d, "`%s` operand", type_name(tt));
        diag_end(d);
        return;
    }
    coerce(S, &e->u.binary.rhs, tt, &target->loc, "the result is stored here");
}

/* An expression whose value is thrown away: statements and for-loop steps. */
static void check_effect(Sema *S, Expr **slot) {
    Expr *e = *slot;
    if (e->kind == EXPR_ASSIGN) {
        check_assign(S, e);
        return;
    }
    Type *t = check(S, slot, NULL);
    if (t->kind == TY_ERRUNION) {
        Diag *d = diag_start(DIAG_WARNING, "W0005", e->loc, "this result may be an error, which is being ignored");
        diag_label(d, "returns `%s`", type_name(t));
        if (S->fn && S->fn->type->kind == TY_ERRUNION && S->fn->type->err == t->err)
            fix_append(S, d, e, "?", "to pass the error on to the caller, add `?`");
        else
            diag_help(d, "check it: `if (!r->ok) { ... }`, or pass it on with `?`");
        diag_note(d, "to ignore it on purpose, store it in a variable whose name starts with `_`");
        diag_end(d);
    }
    /* A bare `x++;` writes x without reading it, for the unused warning. */
    bool incdec = (e->kind == EXPR_POSTFIX || e->kind == EXPR_UNARY) &&
                  (e->u.unary.op == TOK_PLUSPLUS || e->u.unary.op == TOK_MINUSMINUS);
    if (incdec && e->u.unary.operand->kind == EXPR_IDENT && e->u.unary.operand->sym)
        e->u.unary.operand->sym->reads--;
}

/* ---- constant evaluation ------------------------------------------------- */

static double round_to(const Type *t, double v) {
    return t->size == 4 ? (double)(float)v : v;
}

static bool const_fail(const Expr *e, bool report, const char *code, const char *msg, const char *label) {
    if (report) {
        Diag *d = diag_start(DIAG_ERROR, code, e->loc, "%s", msg);
        diag_label(d, "%s", label);
        diag_end(d);
    }
    return false;
}

/* Evaluates a checked expression at compile time, mirroring the generated
 * code exactly (wrapping, truncating division, shift counts mod 64). */
static bool const_eval(Sema *S, const Expr *e, ConstVal *out, bool report) {
    const Type *t = e->type;
    out->bits = 0;
    out->f = 0;
    if (!t || is_err(t)) return false;

    switch (e->kind) {
    case EXPR_INT:
    case EXPR_CHAR:
        if (type_is_float(t)) out->f = round_to(t, (double)e->u.int_val);
        else out->bits = type_is_int(t) ? int_normalize(t, e->u.int_val) : e->u.int_val;
        return true;
    case EXPR_FLOAT:
        out->f = round_to(t, e->u.float_val);
        return true;
    case EXPR_BOOL:
        out->bits = e->u.bool_val;
        return true;
    case EXPR_NULL:
        return true;
    case EXPR_IDENT:
        if (e->sym && e->sym->kind == SYM_GLOBAL && e->sym->has_value) {
            out->bits = e->sym->value_bits;
            if (type_is_float(t)) memcpy(&out->f, &e->sym->value_bits, sizeof out->f);
            return true;
        }
        if (report) {
            Diag *d = diag_start(DIAG_ERROR, "E0307", e->loc, "`%s` isn't a constant", e->u.name);
            diag_label(d, "not a compile-time constant");
            if (e->sym && e->sym->kind == SYM_GLOBAL && !e->sym->is_const) {
                diag_span(d, e->sym->loc, "`%s` is declared without `const`, so it can change", e->sym->name);
                diag_help(d, "make `%s` const, or set this global at the start of `main` instead", e->sym->name);
            }
            diag_end(d);
        }
        return false;
    case EXPR_UNARY: {
        TokenKind op = e->u.unary.op;
        const Expr *operand = e->u.unary.operand;
        if (op == TOK_MINUS && operand->kind == EXPR_INT) {
            if (type_is_float(t)) out->f = -(double)operand->u.int_val;
            else out->bits = int_normalize(t, 0 - operand->u.int_val);
            return true;
        }
        if (op != TOK_MINUS && op != TOK_TILDE && op != TOK_BANG) break;
        ConstVal v;
        if (!const_eval(S, operand, &v, report)) return false;
        if (op == TOK_BANG) out->bits = !v.bits;
        else if (type_is_float(t)) out->f = round_to(t, -v.f);
        else out->bits = int_normalize(t, op == TOK_MINUS ? 0 - v.bits : ~v.bits);
        return true;
    }
    case EXPR_BINARY: {
        TokenKind op = e->u.binary.op;
        const Expr *l = e->u.binary.lhs, *r = e->u.binary.rhs;
        ConstVal a, b;
        if (!const_eval(S, l, &a, report)) return false;
        if (op == TOK_ANDAND || op == TOK_OROR) {
            if ((op == TOK_ANDAND) == (a.bits == 0)) {
                out->bits = op == TOK_OROR;
                return true;
            }
            if (!const_eval(S, r, &b, report)) return false;
            out->bits = b.bits != 0;
            return true;
        }
        if (!const_eval(S, r, &b, report)) return false;
        const Type *ot = l->type;  /* operand type (comparisons produce bool) */
        if (ot->kind == TY_PTR || r->type->kind == TY_PTR) break;  /* pointer arithmetic isn't constant */
        if (type_is_float(ot)) {
            double x = a.f, y = b.f;
            switch (op) {
            case TOK_PLUS:  out->f = round_to(t, x + y); return true;
            case TOK_MINUS: out->f = round_to(t, x - y); return true;
            case TOK_STAR:  out->f = round_to(t, x * y); return true;
            case TOK_SLASH: out->f = round_to(t, x / y); return true;
            case TOK_EQ: out->bits = x == y; return true;
            case TOK_NE: out->bits = x != y; return true;
            case TOK_LT: out->bits = x < y; return true;
            case TOK_GT: out->bits = x > y; return true;
            case TOK_LE: out->bits = x <= y; return true;
            case TOK_GE: out->bits = x >= y; return true;
            default: break;
            }
            break;
        }
        uint64_t x = a.bits, y = b.bits;
        bool sgn = ot->is_signed;
        int64_t sx = (int64_t)x, sy = (int64_t)y;
        uint64_t r64;
        switch (op) {
        case TOK_PLUS:  r64 = x + y; break;
        case TOK_MINUS: r64 = x - y; break;
        case TOK_STAR:  r64 = x * y; break;
        case TOK_SLASH:
        case TOK_PERCENT:
            if (y == 0) return const_fail(e, report, "E0308", "division by zero in a constant expression", "dividing by zero");
            if (sgn && ot->size == 8 && sx == INT64_MIN && sy == -1)
                return const_fail(e, report, "E0308", "constant division overflows", "this overflows");
            if (sgn) r64 = (uint64_t)(op == TOK_SLASH ? (sx == INT64_MIN && sy == -1 ? sx : sx / sy)
                                                      : (sy == -1 ? 0 : sx % sy));
            else r64 = op == TOK_SLASH ? x / y : x % y;
            break;
        case TOK_AMP:   r64 = x & y; break;
        case TOK_PIPE:  r64 = x | y; break;
        case TOK_CARET: r64 = x ^ y; break;
        case TOK_SHL:   r64 = x << (y & 63); break;
        case TOK_SHR:
            r64 = sgn && sx < 0 ? ~(~x >> (y & 63)) : x >> (y & 63);
            break;
        case TOK_EQ: out->bits = x == y; return true;
        case TOK_NE: out->bits = x != y; return true;
        case TOK_LT: out->bits = sgn ? sx < sy : x < y; return true;
        case TOK_GT: out->bits = sgn ? sx > sy : x > y; return true;
        case TOK_LE: out->bits = sgn ? sx <= sy : x <= y; return true;
        case TOK_GE: out->bits = sgn ? sx >= sy : x >= y; return true;
        default: goto not_const;
        }
        out->bits = t->kind == TY_BOOL ? (r64 != 0) : int_normalize(t, r64);
        return true;
    }
    case EXPR_TERNARY: {
        ConstVal c;
        if (!const_eval(S, e->u.ternary.cond, &c, report)) return false;
        return const_eval(S, c.bits ? e->u.ternary.then_expr : e->u.ternary.else_expr, out, report);
    }
    case EXPR_CAST: {
        const Expr *inner = e->u.cast.expr;
        const Type *from = inner->type;
        ConstVal v;
        if (!const_eval(S, inner, &v, report)) return false;
        if (type_is_float(t)) {
            if (type_is_float(from)) out->f = round_to(t, v.f);
            else if (from->is_signed) out->f = round_to(t, (double)(int64_t)v.bits);
            else out->f = round_to(t, (double)v.bits);
            return true;
        }
        if (type_is_float(from)) {
            if (!(v.f > -9.3e18 && v.f < 1.8e19))
                return const_fail(e, report, "E0308", "constant float doesn't fit in an integer", "out of range");
            int64_t iv = v.f < 0 ? (int64_t)v.f : 0;
            out->bits = v.f < 0 ? (uint64_t)iv : (uint64_t)v.f;
            out->bits = int_normalize(t, out->bits);
            return true;
        }
        out->bits = type_is_int(t) ? int_normalize(t, v.bits) : v.bits;
        return true;
    }
    default:
        break;
    }
not_const:
    if (report) {
        Diag *d = diag_start(DIAG_ERROR, "E0307", e->loc, "global initializers must be constant expressions");
        diag_label(d, "not a compile-time constant");
        if (e->kind == EXPR_CALL) diag_note(d, "function calls aren't evaluated at compile time");
        else if (e->kind == EXPR_UNARY && e->u.unary.op == TOK_AMP)
            diag_note(d, "addresses aren't known until the program is linked");
        diag_help(d, "set this global at the start of `main` instead");
        diag_end(d);
    }
    return false;
}

static void write_bytes(unsigned char *buf, uint64_t v, uint32_t size) {
    for (uint32_t i = 0; i < size; i++) buf[i] = (unsigned char)(v >> (8 * i));
}

static void add_reloc(Sema *S, Symbol *g, uint32_t offset, StrBuf str) {
    StrReloc *grown = arena_alloc(S->arena, (g->nrelocs + 1) * sizeof *grown);
    if (g->nrelocs) memcpy(grown, g->relocs, g->nrelocs * sizeof *grown);
    grown[g->nrelocs].offset = offset;
    grown[g->nrelocs].str = str;
    g->relocs = grown;
    g->nrelocs++;
}

static Symbol *init_target;   /* the global whose initial bytes const_init is filling */

/* Fills `buf` with the initial bytes of a global. */
static bool const_init(Sema *S, const Expr *e, const Type *t, unsigned char *buf) {
    if (e->kind == EXPR_STRING && init_target) {
        /* The literal's address is filled in by the linker. */
        add_reloc(S, init_target, (uint32_t)(buf - init_target->init), e->u.str);
        if (t->kind == TY_STRING) write_bytes(buf + 8, e->u.str.len, 8);
        return true;
    }
    if (e->kind == EXPR_STRUCT_LIT && t->kind == TY_STRUCT) {
        bool ok = true;
        for (size_t i = 0; i < e->u.struct_lit.n; i++) {
            const FieldInit *fi = e->u.struct_lit.fields[i];
            const Field *f = find_field(t, fi->name);
            if (f) ok = const_init(S, fi->value, f->type, buf + f->offset) && ok;
        }
        return ok;
    }
    if (e->kind == EXPR_CAST && e->implicit && t->kind == TY_ERRUNION) {
        /* A T!E global starts as a value or an error. */
        const Expr *inner = e->u.cast.expr;
        bool is_error = inner->type == t->err;
        buf[0] = is_error;
        const Field *f = find_field(t, is_error ? "err" : "value");
        return const_init(S, inner, f->type, buf + f->offset);
    }
    if (e->kind == EXPR_ARRAY_LIT && t->kind == TY_ARRAY) {
        bool ok = true;
        for (size_t i = 0; i < e->u.array.n && i < t->len; i++)
            ok = const_init(S, e->u.array.entries[i]->value, t->base, buf + i * t->base->size) && ok;
        return ok;
    }
    ConstVal v;
    if (!const_eval(S, e, &v, true)) return false;
    if (type_is_float(t)) {
        if (t->size == 4) {
            float f = (float)v.f;
            uint32_t bits;
            memcpy(&bits, &f, 4);
            write_bytes(buf, bits, 4);
        } else {
            uint64_t bits;
            memcpy(&bits, &v.f, 8);
            write_bytes(buf, bits, 8);
        }
    } else {
        write_bytes(buf, v.bits, t->size);
    }
    return true;
}

/* ---- statements ------------------------------------------------------------ */

static void check_stmt(Sema *S, Stmt *s);
static bool falls_through(Sema *S, const Stmt *s);

static void check_stmt_list(Sema *S, Stmt **stmts, size_t n) {
    bool warned = false;
    for (size_t i = 0; i < n; i++) {
        if (!warned && i > 0 && diag_error_count() == S->fn_errors_before && !falls_through(S, stmts[i - 1])) {
            Diag *d = diag_start(DIAG_WARNING, "W0002", stmts[i]->loc, "unreachable statement");
            diag_label(d, "this can never run");
            diag_span(d, stmts[i - 1]->loc, "any code after this is unreachable");
            diag_end(d);
            warned = true;
        }
        check_stmt(S, stmts[i]);
    }
}

static void check_scoped(Sema *S, Stmt *s) {
    push_scope(S);
    if (s->kind == STMT_BLOCK) check_stmt_list(S, s->u.block.stmts, s->u.block.n);
    else check_stmt(S, s);
    pop_scope(S);
}

static void report_missing_const_value(const VarDecl *vd) {
    Diag *d = diag_start(DIAG_ERROR, "E0208", vd->loc, "constant `%s` needs a value", vd->name);
    diag_label(d, "declared without a value");
    diag_note(d, "a constant can never be assigned later, so it must be given a value here");
    diag_fix(d, loc_after(vd->loc), " = 0", "give it a value");
    diag_end(d);
}

static Expr *empty_array_lit(Sema *S, Loc loc) {
    return new_node(S, EXPR_ARRAY_LIT, loc, NULL);
}

/* The frame is only 16-byte aligned, so more-aligned values can't live there. */
static void check_stack_align(Type *t, Loc loc, const char *what) {
    if (t->align <= 16) return;
    Diag *d = diag_start(DIAG_ERROR, "E0370", loc, "%s of type `%s` can't be on the stack", what, type_name(t));
    diag_label(d, "needs %u-byte alignment", t->align);
    diag_note(d, "stack frames are only 16-byte aligned");
    diag_help(d, "make it a global and use a pointer to it");
    diag_end(d);
}

static void check_var_decl(Sema *S, VarDecl *vd) {
    Type *t = resolve_type(S, vd->type, false);
    if (!is_err(t)) check_stack_align(t, vd->type->loc, "a local variable");
    if (vd->type->is_const && !vd->init) report_missing_const_value(vd);
    if (t->kind == TY_PHPARRAY && !vd->init) vd->init = empty_array_lit(S, vd->loc);   /* `array a;` starts empty */
    if (vd->init) {
        if (vd->init->kind == EXPR_ASSIGN) {
            report_assign_as_value(vd->init);
        } else {
            check(S, &vd->init, t);
            coerce(S, &vd->init, t, &vd->type->loc, "expected because of this type");
        }
    }
    vd->sym = declare_local(S, SYM_LOCAL, vd->name, vd->loc, t, vd->type);
}

/* `return &local;` hands back a pointer to memory that's about to go away. */
static void check_dangling_return(const Expr *e) {
    if (e->kind != EXPR_UNARY || e->u.unary.op != TOK_AMP) return;
    Symbol *v = root_var(e->u.unary.operand);
    if (!v || (v->kind != SYM_LOCAL && v->kind != SYM_PARAM)) return;
    Diag *d = diag_start(DIAG_ERROR, "E0333", e->loc, "returns a pointer to local %s `%s`",
                         v->kind == SYM_PARAM ? "parameter" : "variable", v->name);
    diag_label(d, "this pointer would dangle");
    diag_span(d, v->loc, "`%s` stops existing when the function returns", v->name);
    diag_help(d, "return the value itself, or have the caller pass in a pointer to fill");
    diag_end(d);
}

/* ---- foreach, rewritten into a for loop ---------------------------------- */

static Stmt *mk_stmt(Sema *S, StmtKind kind, Loc loc) {
    Stmt *s = ARENA_NEW(S->arena, Stmt);
    s->kind = kind;
    s->loc = loc;
    return s;
}

static TypeExpr *mk_type(Sema *S, Type *t, Loc loc) {
    TypeExpr *te = ARENA_NEW(S->arena, TypeExpr);
    te->kind = TYPEX_RESOLVED;
    te->loc = loc;
    te->u.resolved = t;
    return te;
}

static Expr *mk_ident(Sema *S, const char *name, Loc loc) {
    Expr *e = new_node(S, EXPR_IDENT, loc, NULL);
    e->u.name = name;
    return e;
}

static Expr *mk_call(Sema *S, const char *fname, Loc loc, Expr *a, Expr *b) {
    Expr *e = new_node(S, EXPR_CALL, loc, NULL);
    e->u.call.callee = mk_ident(S, fname, loc);
    e->u.call.nargs = b ? 2 : 1;
    e->u.call.args = arena_alloc(S->arena, 2 * sizeof(Expr *));
    e->u.call.args[0] = a;
    e->u.call.args[1] = b;
    return e;
}

/* obj->name() or obj->name(arg) */
static Expr *mk_method_call(Sema *S, Expr *obj, const char *name, Expr *arg, Loc loc) {
    Expr *m = new_node(S, EXPR_MEMBER, loc, NULL);
    m->u.member.object = obj;
    m->u.member.name = name;
    m->u.member.name_loc = loc;
    Expr *c = new_node(S, EXPR_CALL, loc, NULL);
    c->u.call.callee = m;
    c->u.call.nargs = arg ? 1 : 0;
    c->u.call.args = arena_alloc(S->arena, sizeof(Expr *));
    c->u.call.args[0] = arg;
    return c;
}

static Expr *mk_binary(Sema *S, ExprKind kind, TokenKind op, Expr *a, Expr *b, Loc loc) {
    Expr *e = new_node(S, kind, loc, NULL);
    e->u.binary.op = op;
    e->u.binary.lhs = a;
    e->u.binary.rhs = b;
    return e;
}

static Expr *mk_index(Sema *S, Expr *base, Expr *index, Loc loc) {
    Expr *e = new_node(S, EXPR_INDEX, loc, NULL);
    e->u.index.base = base;
    e->u.index.index = index;
    return e;
}

static Expr *mk_cast(Sema *S, Expr *inner, TypeExpr *te) {
    Expr *e = new_node(S, EXPR_CAST, inner->loc, NULL);
    e->u.cast.expr = inner;
    e->u.cast.type = te;
    return e;
}

static Stmt *mk_var(Sema *S, VarDecl *vd, Expr *init) {
    Stmt *s = mk_stmt(S, STMT_VAR, vd->loc);
    vd->init = init;
    s->u.var = vd;
    return s;
}

static VarDecl *mk_vardecl(Sema *S, Type *t, const char *name, Loc loc) {
    VarDecl *vd = ARENA_NEW(S->arena, VarDecl);
    vd->type = mk_type(S, t, loc);
    vd->name = name;
    vd->loc = loc;
    return vd;
}

/* A foreach binding's initial value: converted with `as` if it has a
 * declared type other than `natural` (the element's own type). */
static Expr *binding_init(Sema *S, VarDecl *vd, Expr *value, Type *natural) {
    if (!vd->type) {
        vd->type = mk_type(S, natural, vd->loc);
        return value;
    }
    return mk_cast(S, value, vd->type);
}

/*
 * foreach (items as k => v) body  becomes
 *   { T __fe = items;
 *     for (usize __fi = 0; __fi < <count>; __fi++) {
 *         [if (!<live>) continue;]   (arrays have holes)
 *         K k = <key>; V v = <value>;
 *         body } }
 */
static void check_foreach(Sema *S, Stmt *s) {
    Loc loc = s->loc;
    Expr **itp = &s->u.foreach_.iterable;
    Type *t = check(S, itp, NULL);
    Expr *it = *itp;
    VarDecl *kb = s->u.foreach_.key, *vb = s->u.foreach_.value;
    if (!is_err(t) && !require_value(S, it)) t = ty_error;
    /* Objects with `len()` and `get(i)` methods, like std/vec's Vec<T>. */
    Symbol *len_m = NULL, *get_m = NULL;
    if (!is_err(t) && t->kind == TY_CLASS) {
        len_m = find_method(t, "len");
        get_m = find_method(t, "get");
        bool ok = len_m && get_m && !len_m->is_static && !get_m->is_static && len_m->nparams == 1 &&
                  get_m->nparams == 2 && type_is_int(len_m->type) && type_assignable(len_m->type, get_m->param_types[1]) &&
                  get_m->type->kind != TY_VOID;
        if (!ok) {
            Diag *d = diag_start(DIAG_ERROR, "E0367", it->loc, "can't loop over `%s`", type_name(t));
            diag_label(d, "this class isn't iterable");
            diag_note(d, "foreach works on objects whose class has `len(): int` and `get(int i): T` methods");
            if (len_m && len_m->nparams != 1) diag_span(d, len_m->loc, "`len` here takes arguments");
            else if (get_m && get_m->nparams != 2) diag_span(d, get_m->loc, "`get` here doesn't take exactly one index");
            diag_end(d);
            t = ty_error;
        } else if (!method_visible(S, len_m, it->loc) || !method_visible(S, get_m, it->loc)) {
            t = ty_error;
        }
    }
    if (!is_err(t) && t->kind != TY_PHPARRAY && t->kind != TY_ARRAY && t->kind != TY_STRING && t->kind != TY_CLASS) {
        Diag *d = diag_start(DIAG_ERROR, "E0367", it->loc, "can't loop over a value of type `%s`", type_name(t));
        diag_label(d, "not iterable");
        char *text = t->kind == TY_MIXED ? loc_text(S, it->loc) : NULL;
        if (text) {   /* parenthesized: `foreach (m as array as x)` wouldn't parse */
            size_t len = strlen(text) + 16;
            char *fixed = arena_alloc(S->arena, len);
            snprintf(fixed, len, "(%s as array)", text);
            diag_fix(d, it->loc, fixed, "if it holds an array, convert it");
        }
        if (t->kind != TY_MIXED)
            diag_note(d, "foreach works on `array`, fixed arrays (`T[N]`), strings, and objects with `len()` and `get(i)`");
        diag_end(d);
        t = ty_error;
    }
    if (is_err(t)) {   /* still check the body, with error-typed bindings */
        push_scope(S);
        if (kb) kb->sym = declare_local(S, SYM_LOCAL, kb->name, kb->loc, ty_error, NULL);
        vb->sym = declare_local(S, SYM_LOCAL, vb->name, vb->loc, ty_error, NULL);
        if (kb) kb->sym->reads++;
        vb->sym->reads++;
        S->loop_depth++;
        check_scoped(S, s->u.foreach_.body);
        S->loop_depth--;
        pop_scope(S);
        return;
    }
    it->checked = true;
    int n = ++S->temps;
    char buf[32];
    snprintf(buf, sizeof buf, "__fe%d", n);
    const char *fe = arena_strndup(S->arena, buf, strlen(buf));
    snprintf(buf, sizeof buf, "__fi%d", n);
    const char *fi = arena_strndup(S->arena, buf, strlen(buf));

    Stmt *setup, *skip = NULL;
    Expr *count, *key, *value;
    Type *key_type, *value_type, *index_type = ty_usize;
    if (t->kind == TY_CLASS) {   /* for (i = 0; i < obj->len(); i++) { v = obj->get(i); ... } */
        setup = mk_var(S, mk_vardecl(S, t, fe, loc), it);
        count = mk_method_call(S, mk_ident(S, fe, loc), "len", NULL, loc);
        index_type = key_type = len_m->type;
        key = mk_ident(S, fi, loc);
        value = mk_method_call(S, mk_ident(S, fe, loc), "get", mk_ident(S, fi, loc), loc);
        value_type = get_m->type;
    } else if (t->kind == TY_PHPARRAY) {
        setup = mk_var(S, mk_vardecl(S, ty_phparray, fe, loc), it);
        count = mk_call(S, "__rt_array_slots", loc, mk_ident(S, fe, loc), NULL);
        Expr *live = mk_call(S, "__rt_array_live", loc, mk_ident(S, fe, loc), mk_ident(S, fi, loc));
        Expr *not_live = new_node(S, EXPR_UNARY, loc, NULL);
        not_live->u.unary.op = TOK_BANG;
        not_live->u.unary.operand = live;
        skip = mk_stmt(S, STMT_IF, loc);
        skip->u.if_.cond = not_live;
        skip->u.if_.then_stmt = mk_stmt(S, STMT_CONTINUE, loc);
        key = mk_call(S, "__rt_array_key", loc, mk_ident(S, fe, loc), mk_ident(S, fi, loc));
        value = mk_call(S, "__rt_array_value", loc, mk_ident(S, fe, loc), mk_ident(S, fi, loc));
        key_type = value_type = ty_mixed;
    } else if (t->kind == TY_ARRAY) {
        /* Through a pointer to the first element, so `items` is evaluated once. */
        Expr *first = new_node(S, EXPR_UNARY, loc, NULL);
        first->u.unary.op = TOK_AMP;
        Expr *zero = new_node(S, EXPR_INT, loc, NULL);
        first->u.unary.operand = mk_index(S, it, zero, loc);
        setup = mk_var(S, mk_vardecl(S, type_ptr(t->base, true, false), fe, loc), first);
        count = new_node(S, EXPR_INT, loc, NULL);
        count->u.int_val = t->len;
        key = mk_ident(S, fi, loc);
        value = mk_index(S, mk_ident(S, fe, loc), mk_ident(S, fi, loc), loc);
        key_type = ty_usize;
        value_type = t->base;
    } else {
        setup = mk_var(S, mk_vardecl(S, ty_string, fe, loc), it);
        count = new_node(S, EXPR_MEMBER, loc, NULL);
        count->u.member.object = mk_ident(S, fe, loc);
        count->u.member.name = "len";
        count->u.member.name_loc = loc;
        key = mk_ident(S, fi, loc);
        value = mk_index(S, mk_ident(S, fe, loc), mk_ident(S, fi, loc), loc);
        key_type = ty_usize;
        value_type = ty_u8;
    }
    PtrVec body = {0};
    if (skip) ptrvec_push(&body, skip);
    if (kb) ptrvec_push(&body, mk_var(S, kb, binding_init(S, kb, key, key_type)));
    ptrvec_push(&body, mk_var(S, vb, binding_init(S, vb, value, value_type)));
    ptrvec_push(&body, s->u.foreach_.body);
    Stmt *inner = mk_stmt(S, STMT_BLOCK, loc);
    inner->u.block.stmts = (Stmt **)ptrvec_finish(&body, S->arena, &inner->u.block.n);

    Stmt *loop = mk_stmt(S, STMT_FOR, loc);
    Expr *zero = new_node(S, EXPR_INT, loc, NULL);
    loop->u.for_.init = mk_var(S, mk_vardecl(S, index_type, fi, loc), zero);
    loop->u.for_.cond = mk_binary(S, EXPR_BINARY, TOK_LT, mk_ident(S, fi, loc), count, loc);
    Expr *step = new_node(S, EXPR_POSTFIX, loc, NULL);
    step->u.unary.op = TOK_PLUSPLUS;
    step->u.unary.operand = mk_ident(S, fi, loc);
    loop->u.for_.step = step;
    loop->u.for_.body = inner;

    Stmt *stmts[2] = {setup, loop};
    s->kind = STMT_BLOCK;
    s->u.block.stmts = arena_alloc(S->arena, sizeof stmts);
    memcpy(s->u.block.stmts, stmts, sizeof stmts);
    s->u.block.n = 2;
    check_scoped(S, s);
}

/* ---- inline asm ---------------------------------------------------------------- */

/* The register an asm operand names, as 0-15 for rax..r15 (any width) or
 * 16-31 for xmm0..xmm15; -1 if it isn't one. */
int asm_register(const char *name, size_t len) {
    static const char *const regs[16][4] = {
        {"rax", "eax", "ax", "al"},     {"rcx", "ecx", "cx", "cl"},     {"rdx", "edx", "dx", "dl"},
        {"rbx", "ebx", "bx", "bl"},     {"rsp", "esp", "sp", "spl"},    {"rbp", "ebp", "bp", "bpl"},
        {"rsi", "esi", "si", "sil"},    {"rdi", "edi", "di", "dil"},    {"r8", "r8d", "r8w", "r8b"},
        {"r9", "r9d", "r9w", "r9b"},    {"r10", "r10d", "r10w", "r10b"}, {"r11", "r11d", "r11w", "r11b"},
        {"r12", "r12d", "r12w", "r12b"}, {"r13", "r13d", "r13w", "r13b"}, {"r14", "r14d", "r14w", "r14b"},
        {"r15", "r15d", "r15w", "r15b"},
    };
    for (int i = 0; i < 16; i++)
        for (int w = 0; w < 4; w++)
            if (strlen(regs[i][w]) == len && memcmp(regs[i][w], name, len) == 0) return i;
    if (len >= 4 && len <= 5 && memcmp(name, "xmm", 3) == 0) {
        int n = 0;
        for (size_t i = 3; i < len; i++) {
            if (name[i] < '0' || name[i] > '9') return -1;
            n = n * 10 + (name[i] - '0');
        }
        if (n < 16 && !(len == 5 && name[3] == '0')) return 16 + n;
    }
    return -1;
}

/* asm("..." : in("rdx") port, out("al") value): values go into registers
 * before the instructions and come out of them afterwards. */
static void check_asm(Sema *S, Stmt *s) {
    bool used_in[32] = {false}, used_out[32] = {false};
    for (size_t i = 0; i < s->u.asm_.nops; i++) {
        AsmOperand *op = s->u.asm_.ops[i];
        int reg = asm_register(op->reg.data, op->reg.len);
        bool writes = op->dir != ASM_IN, reads = op->dir != ASM_OUT;
        Type *t;
        if (writes) {
            Expr *target = op->expr;
            t = target->kind == EXPR_IDENT ? check_ident(S, target, op->dir == ASM_INOUT) : check(S, &op->expr, NULL);
            if (!is_err(t) && !check_target(op->expr, "write to")) t = ty_error;
        } else {
            t = check(S, &op->expr, NULL);
            if (!is_err(t) && !require_value(S, op->expr)) t = ty_error;
        }
        if (reg < 0 || reg == 4 || reg == 5) {
            Diag *d = diag_start(DIAG_ERROR, "E0371", op->loc, "`%.*s` can't be used as an asm operand register",
                                 (int)op->reg.len, op->reg.data);
            diag_label(d, reg < 0 ? "not a register" : "the stack and frame pointers are off limits");
            diag_note(d, "operands can use rax..r15 (any width, like `al` or `edx`) except rsp and rbp, and xmm0..xmm15");
            diag_end(d);
            continue;
        }
        if (is_err(t)) continue;
        bool xmm = reg >= 16;
        if (xmm ? !type_is_float(t) : !type_in_gpr(t)) {
            Diag *d = diag_start(DIAG_ERROR, "E0371", op->expr->loc, "a value of type `%s` can't go in register `%.*s`",
                                 type_name(t), (int)op->reg.len, op->reg.data);
            diag_label(d, "this is `%s`", type_name(t));
            diag_note(d, xmm ? "xmm registers take `f32` and `f64` values"
                             : "general registers take integers, bools, pointers, enums and object references");
            diag_end(d);
            continue;
        }
        if ((reads && used_in[reg]) || (writes && used_out[reg])) {
            Diag *d = diag_start(DIAG_ERROR, "E0371", op->loc, "register `%.*s` is used by two %s operands",
                                 (int)op->reg.len, op->reg.data, reads ? "input" : "output");
            diag_label(d, "used again here");
            diag_end(d);
        }
        if (reads) used_in[reg] = true;
        if (writes) used_out[reg] = true;
    }
}

/* E0346 */
static void report_defer_escape(const Stmt *s, const char *what) {
    Diag *d = diag_start(DIAG_ERROR, "E0346", s->loc, "`%s` can't jump out of a `defer`", what);
    diag_label(d, "this would leave the deferred code early");
    diag_note(d, "deferred code runs while its block is already exiting, so it must run to its end");
    diag_end(d);
}

static void check_stmt(Sema *S, Stmt *s) {
    const Decl *fn = S->fn->decl;
    switch (s->kind) {
    case STMT_BLOCK:
        check_scoped(S, s);
        break;
    case STMT_EXPR:
        check_effect(S, &s->u.expr);
        break;
    case STMT_VAR:
        check_var_decl(S, s->u.var);
        break;
    case STMT_RETURN: {
        Type *rt = S->fn->type;
        if (S->in_defer) report_defer_escape(s, "return");
        if (s->u.expr) {
            Type *t = check(S, &s->u.expr, rt->kind == TY_VOID ? NULL : rt);
            if (rt->kind == TY_VOID && !is_err(t)) {
                Diag *d = diag_start(DIAG_ERROR, "E0302", s->u.expr->loc, "function `%s` doesn't return a value",
                                     fn->name);
                diag_label(d, "unexpected return value");
                diag_span(d, ret_type_loc(fn), fn->u.func.ret ? "declared to return `void` here"
                                                              : "no return type is declared, so it returns nothing");
                diag_help(d, "remove the value, or declare a return type: `function %s(...): %s`", fn->name,
                          t->kind == TY_VOID ? "int" : type_name(t));
                diag_end(d);
            } else if (!is_err(rt)) {
                char why[160];
                snprintf(why, sizeof why, "expected `%s` because of the return type", type_name(rt));
                Loc rl = ret_type_loc(fn);
                if (coerce(S, &s->u.expr, rt, &rl, why)) check_dangling_return(s->u.expr);
            }
        } else if (rt->kind != TY_VOID && !is_err(rt) && !(rt->kind == TY_ERRUNION && rt->base->kind == TY_VOID)) {
            Diag *d = diag_start(DIAG_ERROR, "E0303", s->loc, "missing return value in function `%s`", fn->name);
            diag_label(d, "expected a `%s` value", type_name(rt));
            diag_span(d, ret_type_loc(fn), "`%s` is declared to return `%s`", fn->name, type_name(rt));
            diag_end(d);
        }
        break;
    }
    case STMT_IF:
        require_bool(S, &s->u.if_.cond, "an `if` condition");
        check_scoped(S, s->u.if_.then_stmt);
        if (s->u.if_.else_stmt) check_scoped(S, s->u.if_.else_stmt);
        break;
    case STMT_WHILE:
    case STMT_DO_WHILE:
        if (s->kind == STMT_WHILE) require_bool(S, &s->u.while_.cond, "a `while` condition");
        S->loop_depth++;
        check_scoped(S, s->u.while_.body);
        S->loop_depth--;
        if (s->kind == STMT_DO_WHILE) require_bool(S, &s->u.while_.cond, "a `while` condition");
        break;
    case STMT_FOR:
        push_scope(S);
        if (s->u.for_.init) check_stmt(S, s->u.for_.init);
        if (s->u.for_.cond) require_bool(S, &s->u.for_.cond, "a `for` condition");
        S->loop_depth++;
        check_scoped(S, s->u.for_.body);
        S->loop_depth--;
        if (s->u.for_.step) check_effect(S, &s->u.for_.step);
        pop_scope(S);
        break;
    case STMT_BREAK:
    case STMT_CONTINUE:
        if (S->in_defer && S->loop_depth == S->defer_loop_depth) {
            report_defer_escape(s, s->kind == STMT_BREAK ? "break" : "continue");
        } else if (S->loop_depth == 0) {
            bool is_break = s->kind == STMT_BREAK;
            Diag *d = diag_start(DIAG_ERROR, "E0212", s->loc, "`%s` outside of a loop", is_break ? "break" : "continue");
            diag_label(d, "can only be used inside a loop");
            if (is_break) diag_help(d, "to leave the function early, use `return`");
            diag_end(d);
        }
        break;
    case STMT_FOREACH: check_foreach(S, s); break;
    case STMT_ECHO: {
        /* Each value becomes a runtime call; interpolations print part by part. */
        PtrVec calls = {0};
        for (size_t i = 0; i < s->u.echo.n; i++) {
            Expr *e = s->u.echo.exprs[i];
            if (e->kind == EXPR_INTERP) {
                for (size_t j = 0; j < e->u.interp.n; j++) {
                    Type *t = check(S, &e->u.interp.parts[j], NULL);
                    if (is_err(t) || !require_value(S, e->u.interp.parts[j])) continue;
                    Expr *call = echo_call(S, e->u.interp.parts[j]);
                    if (call) ptrvec_push(&calls, call);
                }
                continue;
            }
            Type *t = check(S, &s->u.echo.exprs[i], ty_string);
            if (is_err(t) || !require_value(S, s->u.echo.exprs[i])) continue;
            Expr *call = echo_call(S, s->u.echo.exprs[i]);
            if (call) ptrvec_push(&calls, call);
        }
        s->u.echo.exprs = (Expr **)ptrvec_finish(&calls, S->arena, &s->u.echo.n);
        break;
    }
    case STMT_DEFER: {
        bool saved_in = S->in_defer;
        int saved_depth = S->defer_loop_depth;
        S->in_defer = true;
        S->defer_loop_depth = S->loop_depth;
        check_scoped(S, s->u.defer_body);
        S->in_defer = saved_in;
        S->defer_loop_depth = saved_depth;
        break;
    }
    case STMT_ASM:     check_asm(S, s); break;
    }
}

/* ---- return analysis ------------------------------------------------------ */

static bool is_const_true(Sema *S, const Expr *e) {
    ConstVal v;
    return e->type && e->type->kind == TY_BOOL && const_eval(S, e, &v, false) && v.bits;
}

static bool has_jump(const Stmt *s, StmtKind kind) {
    if (!s) return false;
    switch (s->kind) {
    case STMT_BREAK:
    case STMT_CONTINUE:
        return s->kind == kind;
    case STMT_BLOCK:
        for (size_t i = 0; i < s->u.block.n; i++)
            if (has_jump(s->u.block.stmts[i], kind)) return true;
        return false;
    case STMT_IF:
        return has_jump(s->u.if_.then_stmt, kind) || has_jump(s->u.if_.else_stmt, kind);
    default:
        return false;
    }
}

static bool falls_through(Sema *S, const Stmt *s) {
    switch (s->kind) {
    case STMT_RETURN:
    case STMT_BREAK:
    case STMT_CONTINUE:
        return false;
    case STMT_BLOCK:
        for (size_t i = 0; i < s->u.block.n; i++)
            if (!falls_through(S, s->u.block.stmts[i])) return false;
        return true;
    case STMT_IF:
        if (!s->u.if_.else_stmt) return true;
        return falls_through(S, s->u.if_.then_stmt) || falls_through(S, s->u.if_.else_stmt);
    case STMT_WHILE:
        return !is_const_true(S, s->u.while_.cond) || has_jump(s->u.while_.body, STMT_BREAK);
    case STMT_DO_WHILE:
        if (has_jump(s->u.while_.body, STMT_BREAK)) return true;
        if (is_const_true(S, s->u.while_.cond)) return false;
        return falls_through(S, s->u.while_.body) || has_jump(s->u.while_.body, STMT_CONTINUE);
    case STMT_FOR:
        if (s->u.for_.cond && !is_const_true(S, s->u.for_.cond)) return true;
        return has_jump(s->u.for_.body, STMT_BREAK);
    default:
        return true;
    }
}

/* ---- declarations ------------------------------------------------------------ */

static void check_function_body_inner(Sema *S, Symbol *fn);

static void check_function_body(Sema *S, Symbol *fn) {
    if (fn->is_instance) diag_push_context(fn->inst_loc, "in `%s`", fn->name);
    else if (fn->owner && fn->owner->tmpl) diag_push_context(fn->owner->inst_loc, "in `%s`", fn->owner->name);
    else {
        check_function_body_inner(S, fn);
        return;
    }
    check_function_body_inner(S, fn);
    diag_pop_context();
}

static void check_function_body_inner(Sema *S, Symbol *fn) {
    const Decl *d = fn->decl;
    S->fn = fn;
    S->cur = fn->module;
    S->fn_errors_before = diag_error_count();
    S->locals.len = 0;
    S->nmarks = 0;
    S->loop_depth = 0;
    S->in_defer = false;

    push_scope(S);
    size_t skip = 0;
    if (fn->owner && !fn->is_static) {
        fn->this_param = new_symbol(S, SYM_PARAM, "this", d->name_loc);
        fn->this_param->type = fn->owner;
        skip = 1;
    }
    for (size_t i = 0; i < d->u.func.nparams; i++) {
        Param *p = d->u.func.params[i];
        p->sym = declare_local(S, SYM_PARAM, p->name, p->loc, fn->param_types[i + skip], p->type);
        p->sym->param_index = (int)(i + skip);
    }
    Stmt *b = d->u.func.body;
    check_stmt_list(S, b->u.block.stmts, b->u.block.n);
    pop_scope(S);

    bool void_result = fn->type->kind == TY_VOID || (fn->type->kind == TY_ERRUNION && fn->type->base->kind == TY_VOID);
    if (!void_result && !is_err(fn->type) && falls_through(S, b)) {
        Loc closing = loc_at(b->loc.file, b->loc.offset + b->loc.len - 1, 1);
        Diag *e = diag_start(DIAG_ERROR, "E0304", closing, "function `%s` can reach its end without returning a value",
                             d->name);
        diag_label(e, "the function can end here without returning");
        diag_span(e, ret_type_loc(d), "`%s` is declared to return `%s`", d->name, type_name(fn->type));
        diag_help(e, "add a `return` before the closing brace, or make sure every path returns");
        diag_end(e);
    }
    S->fn = NULL;
}

/* Structs can be passed and returned by value (System V rules); arrays
 * can't, as in C. */
static bool passable(Sema *S, Type *t, const TypeExpr *te, Loc loc, const char *what) {
    if (t->align > 16) {
        check_stack_align(t, loc, t->kind == TY_STRUCT ? "a struct" : "a value");
        return false;
    }
    if (t->kind != TY_ARRAY) return true;
    Diag *d = diag_start(DIAG_ERROR, "E0335", loc, "arrays can't be %s by value", what);
    diag_label(d, "`%s` is an array", type_name(t));
    char fixed[160];
    /* as written: in a generic function that's `T*`, not `int*` */
    if (te && te->kind == TYPEX_ARRAY) snprintf(fixed, sizeof fixed, "%s*", loc_text(S, te->u.array.elem->loc));
    else snprintf(fixed, sizeof fixed, "%s*", type_name(t->base));
    diag_fix(d, loc, fixed, "pass a pointer to the first element instead");
    diag_note(d, "the caller can pass `&arr[0]`, and the length alongside it");
    diag_end(d);
    return false;
}

/* x86 register names can't be symbol names in Intel-syntax assembly:
 * `call rax` would call the register. watrlang's own names are prefixed
 * with `w.`, but extern/export names are used as they are. */
static bool is_register_name(const char *n) {
    static const char *const regs[] = {
        "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp",
        "esp", "ax", "bx", "cx", "dx", "si", "di", "bp", "sp", "al", "bl", "cl", "dl", "ah", "bh", "ch", "dh",
        "sil", "dil", "bpl", "spl", "rip", "eip", "cs", "ds", "es", "fs", "gs", "ss", "cr0", "cr2", "cr3", "cr4",
    };
    for (size_t i = 0; i < sizeof regs / sizeof regs[0]; i++)
        if (strcmp(n, regs[i]) == 0) return true;
    if (n[0] == 'r' && n[1] >= '8' && n[1] <= '9') return true;            /* r8..r9, r8d, ... */
    if (n[0] == 'r' && n[1] == '1' && n[2] >= '0' && n[2] <= '5') return true;  /* r10..r15 */
    if (strncmp(n, "xmm", 3) == 0 || strncmp(n, "ymm", 3) == 0 || strncmp(n, "zmm", 3) == 0) return true;
    if (n[0] == 's' && n[1] == 't' && (n[2] == '\0' || n[2] == '(')) return true;
    return false;
}

/* Sets the assembly name: plain for extern/export, `w.name` otherwise. */
static void set_asm_name(Sema *S, Symbol *sym, const Decl *d) {
    sym->is_extern = d->is_extern;
    sym->is_export = d->is_export;
    if (!d->is_extern && !d->is_export) {
        const char *prefix = S->mods[sym->module]->prefix;
        const char *plain = mangle(S, sym->name);
        size_t n = strlen(prefix) + strlen(plain) + 3;
        char *name = arena_alloc(S->arena, n);
        snprintf(name, n, "w.%s%s", prefix, plain);
        sym->asm_name = name;
        return;
    }
    if (d->is_extern && is_runtime_file(d->loc.file)) {
        /* Runtime primitives that codegen writes in assembly. */
        size_t n = strlen(sym->name) + 3;
        char *name = arena_alloc(S->arena, n);
        snprintf(name, n, "w.%s", sym->name);
        sym->asm_name = name;
        sym->is_runtime = true;
        return;
    }
    sym->asm_name = sym->name;
    if (d->is_export && d->kind == DECL_FUNC && target_is_windows()) {
        /* The body keeps watrlang's convention under an internal name;
         * `name` itself is a thunk that C calls (codegen_win64.c). */
        size_t n = strlen(sym->name) + 12;
        char *name = arena_alloc(S->arena, n);
        snprintf(name, n, "w.__export.%s", sym->name);
        sym->asm_name = name;
    }
    /* C functions link libc only if something reachable calls them (see the
     * reachability pass); C globals as soon as they're declared. */
    if (d->is_extern && d->kind == DECL_VAR) S->needs_libc = true;
    if (is_register_name(sym->name)) {
        Diag *e = diag_start(DIAG_ERROR, "E0336", sym->loc, "`%s` can't be %s because it's an x86 register name",
                             sym->name, d->is_extern ? "an extern symbol" : "exported");
        diag_label(e, "assembles as a register, not a symbol");
        diag_note(e, "watrlang's own functions are renamed internally, but extern and export names are used as-is");
        diag_end(e);
    }
}

static void declare_function(Sema *S, Decl *d) {
    Symbol *fn = new_symbol(S, SYM_FUNC, d->name, d->name_loc);
    fn->decl = d;
    fn->is_private = d->vis == VIS_PRIVATE;
    fn->is_runtime = is_runtime_file(d->loc.file);
    d->sym = fn;
    ptrvec_push(&S->funcs, fn);

    set_asm_name(S, fn, d);
    fn->is_variadic = d->u.func.variadic;
    if (d->u.func.variadic && !d->is_extern) {
        Diag *e = diag_start(DIAG_ERROR, "E0334", d->name_loc, "only `extern` functions can be variadic");
        diag_label(e, "`%s` takes `...`", d->name);
        diag_note(e, "watrlang functions can't read variadic arguments; `...` is for calling C functions like printf");
        diag_end(e);
    }
    if (strcmp(d->name, "main") == 0 && (d->is_extern || d->is_export)) {
        Diag *e = diag_start(DIAG_ERROR, "E0337", d->loc, "`main` can't be `%s`", d->is_extern ? "extern" : "export");
        diag_label(e, "remove `%s`", d->is_extern ? "extern" : "export");
        diag_note(e, "the compiler already makes `main` the program's entry point");
        diag_end(e);
    }

    fn->type = d->u.func.ret ? resolve_type(S, d->u.func.ret, true) : ty_void;
    if (!is_err(fn->type) && !passable(S, fn->type, d->u.func.ret, d->u.func.ret ? d->u.func.ret->loc : d->name_loc, "returned"))
        fn->type = ty_error;
    fn->nparams = d->u.func.nparams;
    fn->param_types = arena_alloc(S->arena, (fn->nparams ? fn->nparams : 1) * sizeof(Type *));
    for (size_t i = 0; i < fn->nparams; i++) {
        Param *p = d->u.func.params[i];
        Type *t = resolve_type(S, p->type, false);
        if (!is_err(t) && !passable(S, t, p->type, p->type->loc, "passed")) t = ty_error;
        fn->param_types[i] = t;
    }
}

static void declare_global(Sema *S, Decl *d) {
    VarDecl *vd = d->u.var;
    Symbol *g = new_symbol(S, SYM_GLOBAL, vd->name, vd->loc);
    g->decl = d;
    g->is_private = d->vis == VIS_PRIVATE;
    g->type_expr = vd->type;
    g->is_const = vd->type->is_const;
    g->is_runtime = is_runtime_file(d->loc.file);
    d->sym = g;
    vd->sym = g;
    set_asm_name(S, g, d);
    g->type = resolve_type(S, vd->type, false);
    g->align = d->align;
    if (g->is_const && !vd->init) report_missing_const_value(vd);

    if (vd->init && !is_err(g->type)) {
        if (vd->init->kind == EXPR_ASSIGN) {
            report_assign_as_value(vd->init);
        } else {
            check(S, &vd->init, g->type);
            if (coerce(S, &vd->init, g->type, &vd->type->loc, "expected because of this type")) {
                g->init = arena_alloc(S->arena, g->type->size ? g->type->size : 1);
                init_target = g;
                bool ok = const_init(S, vd->init, g->type, g->init);
                init_target = NULL;
                if (ok && g->is_const && !type_in_memory(g->type) && g->nrelocs == 0) {
                    ConstVal v;
                    const_eval(S, vd->init, &v, false);
                    g->has_value = true;
                    if (type_is_float(g->type)) memcpy(&g->value_bits, &v.f, sizeof v.f);
                    else g->value_bits = v.bits;
                }
            }
        }
    }
    /* Push after checking the initializer, so `int x = x;` can't see itself. */
    ptrvec_push(&S->globals, g);
}

/* ---- classes ------------------------------------------------------------------ */

/* E0360 */
static void report_member_twice(const Type *ct, const char *name, Loc loc, Loc first) {
    Diag *d = diag_start(DIAG_ERROR, "E0360", loc, "`%s` is declared twice in class `%s`", name, ct->name);
    diag_label(d, "declared again here");
    diag_span(d, first, "first declared here");
    diag_note(d, "a class's fields and methods share one namespace");
    diag_end(d);
}

static void add_iface(Sema *S, Type *ct, Type *it, size_t *cap) {
    for (size_t i = 0; i < ct->nifaces; i++)
        if (ct->ifaces[i] == it) return;
    if (ct->nifaces == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        Type **grown = arena_alloc(S->arena, *cap * sizeof *grown);
        if (ct->nifaces) memcpy(grown, ct->ifaces, ct->nifaces * sizeof *grown);
        ct->ifaces = grown;
    }
    ct->ifaces[ct->nifaces++] = it;
}

/* Resolves `extends` and `implements` (base classes and interfaces first);
 * fills in the flattened interface list. */
static void resolve_hierarchy(Sema *S, Type *ct) {
    if (ct->class_state == 2) return;
    const Decl *d = ct->decl;
    if (ct->class_state == 1) {
        Diag *e = diag_start(DIAG_ERROR, "E0372", d->name_loc, "`%s` inherits from itself", ct->name);
        diag_label(e, "the inheritance goes around in a circle");
        diag_end(e);
        ct->base_class = NULL;
        ct->nifaces = 0;
        return;
    }
    ct->class_state = 1;
    int saved_cur = S->cur;
    S->cur = module_of(S, d->loc.file);
    ct->is_interface = d->u.class_.is_interface;
    ct->is_abstract = d->is_abstract;
    size_t cap = 0;
    if (d->u.class_.base) {
        Type *b = resolve_type(S, d->u.class_.base, false);
        Loc at = d->u.class_.base->loc;
        if (is_err(b)) {
            /* reported */
        } else if (b->kind != TY_CLASS || b->decl->u.class_.is_interface) {
            Diag *e = diag_start(DIAG_ERROR, "E0372", at, "`%s` can't be extended: it isn't a class", type_name(b));
            diag_label(e, b->kind == TY_CLASS ? "an interface" : "not a class");
            if (b->kind == TY_CLASS) diag_help(e, "implement interfaces with `implements %s`", type_name(b));
            diag_end(e);
        } else {
            resolve_hierarchy(S, b);
            if (b->class_state == 2 && ct->class_state == 1) {
                ct->base_class = b;
                for (size_t i = 0; i < b->nifaces; i++) add_iface(S, ct, b->ifaces[i], &cap);
            }
        }
    }
    for (size_t k = 0; k < d->u.class_.nifaces; k++) {
        Loc at = d->u.class_.ifaces[k]->loc;
        Type *it = resolve_type(S, d->u.class_.ifaces[k], false);
        if (is_err(it)) continue;
        const char *name = type_name(it);
        if (it->kind != TY_CLASS || !it->decl->u.class_.is_interface) {
            Diag *e = diag_start(DIAG_ERROR, "E0372", at, "`%s` isn't an interface", name);
            diag_label(e, ct->is_interface ? "interfaces can only extend interfaces" : "`implements` takes interfaces");
            if (it->kind == TY_CLASS && !ct->is_interface) diag_help(e, "to inherit from a class, use `extends %s`", name);
            diag_end(e);
            continue;
        }
        resolve_hierarchy(S, it);
        if (it->class_state != 2 || ct->class_state != 1) continue;
        add_iface(S, ct, it, &cap);
        for (size_t i = 0; i < it->nifaces; i++) add_iface(S, ct, it->ifaces[i], &cap);
    }
    const char *prefix = S->mods[S->cur]->prefix;
    size_t n = strlen(prefix) + strlen(mangle(S, ct->name)) + 16;
    char *desc = arena_alloc(S->arena, n);
    snprintf(desc, n, "w.%s.%s%s", ct->is_interface ? "iface" : "class", prefix, mangle(S, ct->name));
    ct->desc_name = desc;
    S->cur = saved_cur;
    if (ct->class_state == 1) ct->class_state = 2;
}

/* The object's fields, laid out like a struct after the base class's
 * fields (or after the descriptor pointer at offset 0). */
static void layout_class(Sema *S, Type *ct) {
    if (ct->fields || ct->is_interface) return;
    const Decl *d = ct->decl;
    Type *b = ct->base_class;
    if (b) layout_class(S, b);
    int saved_cur = S->cur;
    S->cur = module_of(S, d->loc.file);
    size_t n = d->u.class_.n + (b ? b->nfields : 0);
    ct->fields = arena_alloc(S->arena, (n ? n : 1) * sizeof(Field));
    ct->nfields = 0;
    uint32_t offset = 8, align = 8;
    if (b) {
        for (size_t i = 0; i < b->nfields; i++) ct->fields[ct->nfields++] = b->fields[i];
        offset = b->obj_size;
        align = b->obj_align;
    }
    for (size_t i = 0; i < d->u.class_.n; i++) {
        const Decl *m = d->u.class_.members[i];
        if (m->kind != DECL_VAR) continue;
        const VarDecl *f = m->u.var;
        if (m->is_static) {
            Diag *e = diag_start(DIAG_ERROR, "E0360", m->loc, "static fields aren't supported");
            diag_label(e, "`static` field");
            diag_help(e, "use a global variable next to the class instead");
            diag_end(e);
        }
        for (size_t j = 0; j < ct->nfields; j++) {
            if (strcmp(ct->fields[j].name, f->name) != 0) continue;
            if (ct->fields[j].owner != ct) {
                Diag *e = diag_start(DIAG_ERROR, "E0372", f->loc, "field `%s` is already declared in base class `%s`",
                                     f->name, ct->fields[j].owner->name);
                diag_label(e, "declared again here");
                diag_span(e, ct->fields[j].loc, "inherited from here");
                diag_note(e, "a subclass has all of its base class's fields; give this one a different name");
                diag_end(e);
            } else {
                report_member_twice(ct, f->name, f->loc, ct->fields[j].loc);
            }
        }
        Type *ft = resolve_type(S, f->type, false);
        Type *inner = ft;
        while (inner->kind == TY_ARRAY) inner = inner->base;
        if (inner->kind == TY_STRUCT) layout_struct(S, inner);
        uint32_t fa = ft->align ? ft->align : 1;
        offset = (offset + fa - 1) & ~(fa - 1);
        Field *field = &ct->fields[ct->nfields++];
        memset(field, 0, sizeof *field);
        field->name = f->name;
        field->loc = f->loc;
        field->type = ft;
        field->offset = offset;
        field->init = f->init;
        field->is_private = m->vis == VIS_PRIVATE;
        field->is_protected = m->vis == VIS_PROTECTED;
        field->owner = ct;
        offset += ft->size;
        if (fa > align) align = fa;
    }
    ct->obj_align = align;
    ct->obj_size = (offset + align - 1) & ~(align - 1);
    S->cur = saved_cur;
}

/* Default values are set by `new`, before __construct runs. They must be
 * constants. Each class checks its own; subclasses then copy them. */
static void check_field_defaults(Sema *S, Type *ct) {
    for (size_t i = 0; i < ct->nfields; i++) {
        Field *f = &ct->fields[i];
        if (f->owner != ct) continue;
        if (f->type->kind == TY_PHPARRAY && !f->init) {   /* array fields start empty */
            Expr *lit = empty_array_lit(S, f->loc);
            check(S, &lit, ty_phparray);
            f->init = lit;
            continue;
        }
        if (!f->init || is_err(f->type)) continue;
        const Decl *m = NULL;
        for (size_t j = 0; j < ct->decl->u.class_.n; j++) {
            const Decl *c = ct->decl->u.class_.members[j];
            if (c->kind == DECL_VAR && c->u.var->init == f->init) m = c;
        }
        VarDecl *vd = m->u.var;
        if (vd->init->kind == EXPR_ASSIGN) {
            report_assign_as_value(vd->init);
            f->init = NULL;
            continue;
        }
        Type *t = check(S, &vd->init, f->type);
        if (is_err(t) || !coerce(S, &vd->init, f->type, &vd->type->loc, "expected because of this type")) {
            f->init = NULL;
            continue;
        }
        f->init = vd->init;
        ConstVal cv;
        bool is_lit = f->init->kind == EXPR_STRING || (f->init->kind == EXPR_ARRAY_LIT && f->init->u.array.n == 0);
        if (!is_lit && !const_eval(S, f->init, &cv, false)) {
            Diag *d = diag_start(DIAG_ERROR, "E0307", f->init->loc, "field defaults must be constant expressions");
            diag_label(d, "not a compile-time constant");
            diag_help(d, "set it in `__construct` instead");
            diag_end(d);
            f->init = NULL;
        }
    }
}

static void inherit_defaults(Type *ct) {
    if (!ct->base_class || ct->class_state == 3) return;
    inherit_defaults(ct->base_class);
    for (size_t i = 0; i < ct->base_class->nfields; i++) ct->fields[i].init = ct->base_class->fields[i].init;
    ct->class_state = 3;
}

/* Does `m` have the same parameters as `base` (and a compatible result)? */
static bool same_signature(const Symbol *m, const Symbol *base) {
    if (m->nparams != base->nparams) return false;
    for (size_t i = 1; i < m->nparams; i++)   /* [0] is `this` */
        if (!type_equiv(m->param_types[i], base->param_types[i]) && !is_err(m->param_types[i])) return false;
    if (type_equiv(m->type, base->type) || is_err(m->type)) return true;
    return m->type->kind == TY_CLASS && base->type->kind == TY_CLASS && class_subtype(m->type, base->type);
}

/* E0372 */
static void report_signature(const Symbol *m, const Symbol *base, const char *what) {
    Diag *e = diag_start(DIAG_ERROR, "E0372", m->loc, "method `%s` doesn't match the %s it overrides", m->name, what);
    diag_label(e, "different parameters or result");
    diag_span(e, base->loc, "`%s::%s` is declared here", base->owner->name, base->name);
    diag_note(e, "an override takes the same parameter types and returns the same type (or a subclass of it)");
    diag_end(e);
}

static void declare_methods(Sema *S, Type *ct) {
    if (ct->methods) return;
    const Decl *cd = ct->decl;
    if (ct->base_class) declare_methods(S, ct->base_class);
    for (size_t i = 0; i < ct->nifaces; i++) declare_methods(S, ct->ifaces[i]);
    int saved_cur = S->cur;
    S->cur = module_of(S, cd->loc.file);
    size_t n = cd->u.class_.n;
    ct->methods = arena_alloc(S->arena, (n ? n : 1) * sizeof(Symbol *));
    ct->nmethods = 0;
    for (size_t i = 0; i < n; i++) {
        Decl *d = cd->u.class_.members[i];
        if (d->kind != DECL_FUNC) continue;
        Symbol *prev = NULL;
        for (size_t j = 0; j < ct->nmethods; j++)
            if (strcmp(ct->methods[j]->name, d->name) == 0) prev = ct->methods[j];
        if (prev) {
            report_member_twice(ct, d->name, d->name_loc, prev->loc);
            continue;
        }
        for (size_t j = 0; j < ct->nfields; j++) {
            if (strcmp(ct->fields[j].name, d->name) != 0) continue;
            if (ct->fields[j].owner == ct) {
                report_member_twice(ct, d->name, d->name_loc, ct->fields[j].loc);
                continue;
            }
            Diag *e = diag_start(DIAG_ERROR, "E0372", d->name_loc, "`%s` is already a field of base class `%s`", d->name,
                                 ct->fields[j].owner->name);
            diag_label(e, "a method with the same name");
            diag_span(e, ct->fields[j].loc, "inherited from here");
            diag_note(e, "a class's fields and methods share one namespace, including inherited ones");
            diag_end(e);
        }

        Symbol *fn = new_symbol(S, SYM_FUNC, d->name, d->name_loc);
        fn->decl = d;
        fn->owner = ct;
        fn->is_static = d->is_static;
        fn->is_private = d->vis == VIS_PRIVATE;
        fn->is_protected = d->vis == VIS_PROTECTED;
        fn->is_abstract = d->is_abstract;
        fn->vindex = -1;
        d->sym = fn;
        const char *prefix = S->mods[fn->module]->prefix;
        const char *cname = mangle(S, ct->name);
        size_t len = strlen(prefix) + strlen(cname) + strlen(d->name) + 4;
        char *asm_name = arena_alloc(S->arena, len);
        snprintf(asm_name, len, "w.%s%s.%s", prefix, cname, d->name);
        fn->asm_name = asm_name;
        if (d->u.func.variadic) {
            Diag *e = diag_start(DIAG_ERROR, "E0334", d->name_loc, "only `extern` functions can be variadic");
            diag_label(e, "method `%s` takes `...`", d->name);
            diag_end(e);
        }
        if (d->is_abstract && !ct->is_abstract) {
            Diag *e = diag_start(DIAG_ERROR, "E0372", d->name_loc, "class `%s` has an abstract method, so it must be abstract too",
                                 ct->name);
            diag_label(e, "abstract method");
            diag_help(e, "declare it `abstract class %s`, or give `%s` a body", ct->name, d->name);
            diag_end(e);
            ct->is_abstract = true;   /* treat it as abstract from here on, to avoid follow-on errors */
        }
        if (d->is_abstract && (d->is_static || d->vis == VIS_PRIVATE)) {
            Diag *e = diag_start(DIAG_ERROR, "E0372", d->name_loc, "an abstract method can't be %s",
                                 d->is_static ? "static" : "private");
            diag_label(e, "subclasses couldn't override it");
            diag_end(e);
        }

        fn->type = d->u.func.ret ? resolve_type(S, d->u.func.ret, true) : ty_void;
        if (!is_err(fn->type) && !passable(S, fn->type, d->u.func.ret, ret_type_loc(d), "returned")) fn->type = ty_error;
        size_t skip = fn->is_static ? 0 : 1;
        fn->nparams = d->u.func.nparams + skip;
        fn->param_types = arena_alloc(S->arena, fn->nparams * sizeof(Type *) + sizeof(Type *));
        if (skip) fn->param_types[0] = ct;
        for (size_t j = 0; j < d->u.func.nparams; j++) {
            Param *p = d->u.func.params[j];
            Type *t = resolve_type(S, p->type, false);
            if (!is_err(t) && !passable(S, t, p->type, p->type->loc, "passed")) t = ty_error;
            fn->param_types[j + skip] = t;
        }

        /* PHP's magic methods. */
        bool ctor = strcmp(d->name, "__construct") == 0, to_string = strcmp(d->name, "__toString") == 0;
        if ((ctor || to_string) && d->is_static) {
            Diag *e = diag_start(DIAG_ERROR, "E0360", d->name_loc, "`%s` can't be static", d->name);
            diag_label(e, "it runs on an object");
            diag_end(e);
        }
        if (ctor && d->u.func.ret && fn->type->kind != TY_VOID) {
            Diag *e = diag_start(DIAG_ERROR, "E0360", d->u.func.ret->loc, "`__construct` can't return a value");
            diag_label(e, "remove the return type");
            diag_note(e, "`new %s(...)` always produces the new object", ct->name);
            diag_end(e);
            fn->type = ty_void;
        }
        if (to_string && (d->u.func.nparams != 0 || fn->type->kind != TY_STRING)) {
            Diag *e = diag_start(DIAG_ERROR, "E0360", d->name_loc, "`__toString` must take no parameters and return `string`");
            diag_label(e, "wrong signature");
            diag_note(e, "echo and interpolation call `__toString()` to print an object");
            diag_end(e);
        }
        ct->methods[ct->nmethods++] = fn;
        ptrvec_push(&S->funcs, fn);
    }

    /* The vtable: the base class's, with overrides replacing entries and
     * new virtual methods appended. Interfaces list all their methods. */
    size_t cap = (ct->base_class ? ct->base_class->nvtable : 0) + ct->nmethods + 1;
    for (size_t i = 0; ct->is_interface && i < ct->nifaces; i++) cap += ct->ifaces[i]->nvtable;
    ct->vtable = arena_alloc(S->arena, cap * sizeof(Symbol *));
    ct->nvtable = 0;
    if (ct->is_interface) {
        for (size_t i = 0; i < ct->nifaces; i++)
            for (size_t k = 0; k < ct->ifaces[i]->decl->u.class_.n; k++) {
                Decl *md = ct->ifaces[i]->decl->u.class_.members[k];
                if (md->kind == DECL_FUNC && md->sym && !find_method(ct, md->name)) ct->vtable[ct->nvtable++] = md->sym;
            }
        for (size_t i = 0; i < ct->nmethods; i++) {
            Symbol *m = ct->methods[i];
            Symbol *inherited = find_method(ct, m->name);
            if (inherited && !same_signature(m, inherited)) report_signature(m, inherited, "interface method");
            if (!inherited) {
                m->vindex = (int)ct->nvtable;
                ct->vtable[ct->nvtable++] = m;
            }
        }
        S->cur = saved_cur;
        return;
    }
    if (ct->base_class)
        for (size_t i = 0; i < ct->base_class->nvtable; i++) ct->vtable[ct->nvtable++] = ct->base_class->vtable[i];
    for (size_t i = 0; i < ct->nmethods; i++) {
        Symbol *m = ct->methods[i];
        Symbol *base = ct->base_class ? find_method(ct->base_class, m->name) : NULL;
        bool virt = !m->is_static && !m->is_private && strcmp(m->name, "__construct") != 0;
        if (base && base->vindex >= 0) {
            if (!virt) {
                Diag *e = diag_start(DIAG_ERROR, "E0372", m->loc, "`%s` can't be %s: it overrides `%s::%s`", m->name,
                                     m->is_static ? "static" : "private", base->owner->name, base->name);
                diag_label(e, "here");
                diag_span(e, base->loc, "declared here");
                diag_end(e);
                continue;
            }
            if (!same_signature(m, base)) report_signature(m, base, "method");
            if (base->is_protected && !m->is_protected) { /* widening to public is fine */ }
            if (!base->is_protected && m->is_protected) {
                Diag *e = diag_start(DIAG_ERROR, "E0372", m->loc, "`%s` can't be protected: `%s::%s` is public", m->name,
                                     base->owner->name, base->name);
                diag_label(e, "less visible than what it overrides");
                diag_end(e);
            }
            m->vindex = base->vindex;
            ct->vtable[m->vindex] = m;
        } else if (virt) {
            m->vindex = (int)ct->nvtable;
            ct->vtable[ct->nvtable++] = m;
        }
    }
    if (!ct->is_abstract) {
        for (size_t i = 0; i < ct->nvtable; i++) {
            Symbol *m = ct->vtable[i];
            if (!m->is_abstract) continue;
            Diag *e = diag_start(DIAG_ERROR, "E0372", cd->name_loc, "class `%s` doesn't implement abstract method `%s`",
                                 ct->name, m->name);
            diag_label(e, "missing `%s`", m->name);
            diag_span(e, m->loc, "declared abstract in `%s`", m->owner->name);
            diag_help(e, "define `%s` in `%s`, or declare the class `abstract`", m->name, ct->name);
            diag_end(e);
        }
    }
    /* Interface tables: for each interface, the methods that implement it. */
    ct->itables = arena_alloc(S->arena, (ct->nifaces ? ct->nifaces : 1) * sizeof(Symbol **));
    for (size_t i = 0; i < ct->nifaces; i++) {
        Type *it = ct->ifaces[i];
        Symbol **tab = arena_alloc(S->arena, (it->nvtable ? it->nvtable : 1) * sizeof(Symbol *));
        bool complete = true;
        for (size_t k = 0; k < it->nvtable; k++) {
            Symbol *want = it->vtable[k];
            Symbol *have = find_method(ct, want->name);
            if (have && (have->vindex < 0 || have->is_static)) have = NULL;
            if (have && have->is_abstract && !ct->is_abstract) have = NULL;
            if (have && !same_signature(have, want)) {
                report_signature(have, want, "interface method");
                have = NULL;
                complete = false;
            } else if (!have && !ct->is_abstract) {
                Diag *e = diag_start(DIAG_ERROR, "E0372", cd->name_loc, "class `%s` doesn't implement `%s::%s`", ct->name,
                                     it->name, want->name);
                diag_label(e, "missing `%s`", want->name);
                diag_span(e, want->loc, "required by interface `%s`", it->name);
                diag_end(e);
            }
            if (!have) complete = false;
            tab[k] = have;
        }
        ct->itables[i] = complete ? tab : NULL;
    }
    S->cur = saved_cur;
}

static void check_duplicates(Sema *S) {
    Module *m = S->mod;
    for (size_t i = 0; i < m->n; i++) {
        const Decl *d = m->decls[i];
        bool is_type = d->kind == DECL_STRUCT || d->kind == DECL_ENUM || d->kind == DECL_CLASS;
        bool is_value = d->kind == DECL_FUNC || d->kind == DECL_VAR;
        if (!is_type && !is_value) continue;
        for (size_t j = 0; j < i; j++) {
            const Decl *prev = m->decls[j];
            if (prev->loc.file != d->loc.file) {
                /* Different modules may reuse names, but not exported symbols,
                 * which share the linker's single namespace. */
                if (d->is_export && prev->is_export && d->kind == prev->kind && strcmp(prev->name, d->name) == 0) {
                    Diag *e = diag_start(DIAG_ERROR, "E0202", d->name_loc, "`%s` is exported by two modules", d->name);
                    diag_label(e, "exported here");
                    diag_span(e, prev->name_loc, "and here");
                    diag_note(e, "exported names keep their plain names, so they must be unique in the program");
                    diag_end(e);
                }
                continue;
            }
            bool same_space = is_type ? (prev->kind == DECL_STRUCT || prev->kind == DECL_ENUM || prev->kind == DECL_CLASS)
                                      : (prev->kind == DECL_FUNC || prev->kind == DECL_VAR);
            if (!same_space || strcmp(prev->name, d->name) != 0) continue;
            Diag *e = diag_start(DIAG_ERROR, "E0202", d->name_loc, "the name `%s` is defined more than once", d->name);
            diag_label(e, "`%s` redefined here", d->name);
            diag_span(e, prev->name_loc, "first definition of `%s` here", d->name);
            if (prev->kind != d->kind) diag_note(e, "functions and global variables share one namespace");
            diag_end(e);
            break;
        }
    }
}

/* What a runtime function that needs the heap (or libc) is used for. */
static const char *heap_feature(const char *name) {
    if (strcmp(name, "__rt_alloc") == 0) return "`new` needs the garbage-collected heap";
    if (strcmp(name, "__rt_alloc_manual") == 0 || strcmp(name, "__rt_free") == 0)
        return "`alloc` and `free` need the runtime heap";
    if (strstr(name, "f64") || strstr(name, "fmt")) return "printing floats needs the C library";
    if (strncmp(name, "__rt_mixed", 10) == 0 || strncmp(name, "__rt_array", 10) == 0 || strncmp(name, "__rt_as_", 8) == 0)
        return "`array` and `mixed` need the garbage-collected heap";
    if (strcmp(name, "__rt_concat") == 0) return "joining strings needs the heap";
    if (strstr(name, "to_string")) return "turning a value into a string needs the heap";
    if (strncmp(name, "__rt_gc", 7) == 0) return "the garbage collector isn't available";
    return "this needs the runtime heap";
}

/* --freestanding: nothing reachable may need the heap or libc, and echo
 * needs the program's `watr_write`. */
static void check_freestanding(Sema *S) {
    /* Runtime functions that need the heap or libc, directly or not. */
    size_t n = S->funcs.len;
    bool *heap = calloc(n ? n : 1, sizeof *heap);
    if (!heap) fatal("out of memory");
    for (size_t i = 0; i < n; i++) {
        Symbol *fn = S->funcs.items[i];
        heap[i] = fn->is_runtime &&
                  (strcmp(fn->name, "__gc_alloc") == 0 || strcmp(fn->name, "__rt_mmap") == 0 ||
                   strcmp(fn->name, "__rt_fmt_f64") == 0 || strcmp(fn->name, "__rt_gc_collect") == 0);
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t i = 0; i < n; i++) {
            Symbol *fn = S->funcs.items[i];
            if (heap[i] || !fn->is_runtime) continue;
            for (size_t j = 0; j < fn->ncallees && !heap[i]; j++)
                for (size_t k = 0; k < n; k++)
                    if (heap[k] && S->funcs.items[k] == fn->callees[j]) heap[i] = true;
            changed |= heap[i];
        }
    }
    Loc echo_loc = {0};
    for (size_t i = 0; i < n; i++) {
        Symbol *fn = S->funcs.items[i];
        if (!fn->reachable || fn->is_runtime) continue;
        for (size_t j = 0; j < fn->ncallees; j++) {
            Symbol *c = fn->callees[j];
            if (!c->is_runtime) continue;
            if (strncmp(c->name, "__rt_echo", 9) == 0 && !echo_loc.file) echo_loc = fn->callee_locs[j];
            size_t k = 0;
            while (k < n && S->funcs.items[k] != c) k++;
            if (k == n || !heap[k]) continue;
            Loc at = fn->callee_locs[j];
            bool seen = false;   /* one error per feature per line */
            for (size_t p = 0; p < j && !seen; p++) {
                size_t q = 0;
                while (q < n && S->funcs.items[q] != fn->callees[p]) q++;
                seen = q < n && heap[q] && fn->callee_locs[p].file == at.file && fn->callee_locs[p].line == at.line &&
                       strcmp(heap_feature(fn->callees[p]->name), heap_feature(c->name)) == 0;
            }
            if (seen) continue;
            Diag *d = diag_start(DIAG_ERROR, "E0368", fn->callee_locs[j], "%s, which --freestanding programs don't have",
                                 heap_feature(c->name));
            diag_label(d, "not available with --freestanding");
            diag_note(d, "without an operating system there's no heap; echo, string literals, comparisons, structs "
                         "and fixed arrays still work");
            diag_end(d);
        }
    }
    free(heap);
    if (echo_loc.file) {
        bool has_write = false;
        for (size_t i = 0; i < n; i++) {
            Symbol *fn = S->funcs.items[i];
            has_write |= fn->is_export && strcmp(fn->name, "watr_write") == 0;
        }
        if (!has_write) {
            Diag *d = diag_start(DIAG_ERROR, "E0369", echo_loc, "`echo` needs a `watr_write` function in --freestanding programs");
            diag_label(d, "has nowhere to write");
            diag_help(d, "define where output goes: `export function watr_write(const u8* p, usize n): void { ... }`");
            diag_note(d, "for example, write the bytes to the VGA text buffer or a serial port");
            diag_end(d);
        }
    }
}

bool sema_check(Arena *arena, Module **mods, size_t nmods, Program *prog) {
    int errors_before = diag_error_count();
    Sema S = {0};
    S.arena = arena;
    S.mods = mods;
    S.nmods = nmods;
    S.freestanding = prog->freestanding;

    /* One flat list of declarations; each knows its module through its file. */
    size_t total = 0;
    for (size_t i = 0; i < nmods; i++) total += mods[i]->n;
    Module flat = {mods[0]->file, ARENA_ARRAY(arena, Decl *, total ? total : 1), 0, "", "", NULL, NULL, 0, false};
    for (size_t i = 0; i < nmods; i++)
        for (size_t j = 0; j < mods[i]->n; j++) flat.decls[flat.n++] = mods[i]->decls[j];
    Module *m = &flat;
    S.mod = m;

    check_duplicates(&S);

    /* Types first, so functions and globals can use any struct. */
    for (size_t i = 0; i < m->n; i++) {
        Decl *d = m->decls[i];
        S.cur = module_of(&S, d->loc.file);
        if (d->ntparams) {
            if (d->is_extern) {
                Diag *e = diag_start(DIAG_ERROR, "E0381", d->name_loc, "extern function `%s` can't be generic", d->name);
                diag_label(e, "has type parameters");
                diag_note(e, "a generic function is compiled once for each set of types it's used with; C can't do that");
                diag_end(e);
            } else {
                ptrvec_push(&S.templates, d);
            }
            continue;
        }
        switch (d->kind) {
        case DECL_STRUCT:
        case DECL_CLASS:
        case DECL_ENUM: {
            bool dup = false;
            for (size_t j = 0; j < S.structs.len; j++) {
                Type *t = S.structs.items[j];
                dup |= t->decl->loc.file == d->loc.file && strcmp(t->name, d->name) == 0;
            }
            if (dup) break;
            if (d->kind == DECL_STRUCT) {
                d->type = type_new_struct(d->name, d);
            } else if (d->kind == DECL_CLASS) {
                d->type = type_new_class(d->name, d);
                d->type->class_id = S.classes++;
            } else {
                Type *base = d->u.enum_.base ? resolve_type(&S, d->u.enum_.base, false) : ty_i32;
                if (!is_err(base) && base->kind != TY_INT) {
                    Diag *e = diag_start(DIAG_ERROR, "E0351", d->u.enum_.base->loc,
                                         "an enum's underlying type must be an integer type, found `%s`",
                                         type_name(base));
                    diag_label(e, "not an integer type");
                    diag_end(e);
                    base = ty_error;
                }
                d->type = type_new_enum(d->name, d, is_err(base) ? ty_i32 : base);
            }
            ptrvec_push(&S.structs, d->type);
            break;
        }
        case DECL_IMPORT: break;   /* handled by the module loader */
        default: break;
        }
    }
    for (size_t i = 0; i < S.structs.len; i++) {
        Type *t = S.structs.items[i];
        S.cur = module_of(&S, t->decl->loc.file);
        if (t->kind == TY_STRUCT) layout_struct(&S, t);
        else if (t->kind == TY_ENUM) check_enum_variants(&S, t);
    }
    S.phase = PH_STRUCTS;
    for (size_t i = 0; i < S.structs.len; i++) {   /* extends / implements */
        Type *t = S.structs.items[i];
        if (t->kind == TY_CLASS) resolve_hierarchy(&S, t);
    }
    S.phase = PH_HIERARCHY;
    for (size_t i = 0; i < S.structs.len; i++) {   /* classes may hold structs and enums */
        Type *t = S.structs.items[i];
        S.cur = module_of(&S, t->decl->loc.file);
        if (t->kind == TY_CLASS) layout_class(&S, t);
    }
    S.phase = PH_CLASS_LAYOUT;

    for (size_t i = 0; i < m->n; i++) {
        Decl *d = m->decls[i];
        S.cur = module_of(&S, d->loc.file);
        if (d->kind == DECL_FUNC && !d->ntparams && !find_in_module(&S.funcs, S.cur, d->name)) declare_function(&S, d);
        if (d->kind == DECL_CLASS && d->type) declare_methods(&S, d->type);
    }
    for (size_t i = 0; i < S.structs.len; i++) {   /* generic instances made so far */
        Type *t = S.structs.items[i];
        S.cur = module_of(&S, t->decl->loc.file);
        if (t->kind == TY_CLASS) declare_methods(&S, t);
    }
    S.phase = PH_METHODS;
    for (size_t i = 0; i < m->n; i++) {
        if (m->decls[i]->kind != DECL_VAR) continue;
        S.cur = module_of(&S, m->decls[i]->loc.file);
        declare_global(&S, m->decls[i]);
    }
    /* Field defaults are constants, checked once globals are known. */
    for (size_t i = 0; i < S.structs.len; i++) {
        Type *t = S.structs.items[i];
        S.cur = module_of(&S, t->decl->loc.file);
        if (t->kind == TY_CLASS) check_field_defaults(&S, t);
    }
    S.phase = PH_DEFAULTS;
    for (size_t i = 0; i < S.structs.len; i++) {   /* subclasses take their base classes' defaults */
        Type *t = S.structs.items[i];
        if (t->kind == TY_CLASS) inherit_defaults(t);
    }
    S.phase = PH_INHERIT;

    S.cur = 0;
    Symbol *main_fn = S.freestanding ? NULL : find_in_module(&S.funcs, 0, "main");
    if (S.freestanding) {
        /* No main: exported functions are the entry points. */
        bool any_export = false;
        for (size_t i = 0; i < S.funcs.len; i++) any_export |= ((Symbol *)S.funcs.items[i])->is_export;
        if (!any_export) {
            Loc loc = {m->file, 0, 0, 0, 0};
            Diag *d = diag_start(DIAG_ERROR, "E0305", loc, "a --freestanding program needs an entry point");
            diag_note(d, "without an operating system there's no `main`; exported functions are what other code calls");
            diag_help(d, "add one, like `export function kmain(): void { ... }`, and call it from your boot code");
            diag_end(d);
        }
    } else if (!main_fn) {
        Loc loc = {m->file, 0, 0, 0, 0};
        Diag *d = diag_start(DIAG_ERROR, "E0305", loc, "`main` function not found in `%s`", m->file->path);
        diag_note(d, "a program starts running at `main`");
        diag_help(d, "add one: `function main(): int { return 0; }`");
        diag_end(d);
    } else {
        const Decl *md = main_fn->decl;
        if (main_fn->type != ty_int && !is_err(main_fn->type)) {
            Diag *d = diag_start(DIAG_ERROR, "E0306", ret_type_loc(md), "`main` must return `int`");
            diag_label(d, md->u.func.ret ? "expected `int` here" : "no return type");
            diag_note(d, "main's return value becomes the program's exit code");
            if (md->u.func.ret) diag_fix(d, md->u.func.ret->loc, "int", "change the return type");
            diag_end(d);
        }
        if (md->u.func.nparams > 0) not_yet(md->u.func.params[0]->loc, "command-line arguments to main", 7);
    }

    /* Bodies, then what they can reach. Methods of generic classes are
     * checked only once something can call them (as C++ does), so a
     * Vec<Key> is fine even though Vec's join() can't print a Key. Those
     * methods can reach more code, so this repeats until nothing changes. */
    PtrVec work = {0};
    for (;;) {
        bool progress = false;
        for (size_t i = 0; i < S.funcs.len; i++) {
            Symbol *fn = S.funcs.items[i];
            bool lazy = fn->owner && fn->owner->tmpl;
            if (!fn->decl->u.func.body || fn->body_checked || (lazy && !fn->reachable)) continue;
            fn->body_checked = true;
            check_function_body(&S, fn);
            progress = true;
        }

        /* A virtual call can reach the method in any subclass of the receiver's type. */
        for (size_t i = 0; i < S.vsites.len; i++) {
            VSite *v = S.vsites.items[i];
            for (size_t k = 0; k < S.structs.len; k++) {
                Type *c = S.structs.items[k];
                if (c->kind != TY_CLASS || c->is_interface || c->is_abstract || !class_subtype(c, v->t)) continue;
                Symbol *impl = find_method(c, v->name);
                if (!impl || impl->is_abstract) continue;
                S.fn = v->fn;
                add_callee(&S, impl);
                S.fn = NULL;
            }
        }

        /* Only functions reachable from main (or exported) are compiled. */
        work.len = 0;
        for (size_t i = 0; i < S.funcs.len; i++) {
            Symbol *fn = S.funcs.items[i];
            if (fn == main_fn || fn->is_export) fn->reachable = true;
            if (fn->reachable) ptrvec_push(&work, fn);
        }
        while (work.len) {
            Symbol *fn = work.items[--work.len];
            for (size_t i = 0; i < fn->ncallees; i++) {
                if (!fn->callees[i]->reachable) {
                    fn->callees[i]->reachable = true;
                    ptrvec_push(&work, fn->callees[i]);
                }
            }
        }
        if (!progress) break;
    }
    free(work.items);
    for (size_t i = 0; i < S.funcs.len; i++) {
        Symbol *fn = S.funcs.items[i];
        if (!fn->reachable) continue;
        if (fn->is_extern && !fn->is_runtime) S.needs_libc = true;   /* calls into C */
        if (fn->is_runtime && strcmp(fn->name, "__rt_fmt_f64") == 0) S.needs_libc = true;   /* snprintf */
    }
    if (S.freestanding) {
        check_freestanding(&S);
        S.needs_libc = false;
    }
    if (diag_error_count() == errors_before) {
        for (size_t i = 0; i < S.funcs.len; i++) {
            Symbol *fn = S.funcs.items[i];
            if (fn->reachable || fn->is_abstract || fn->is_runtime || fn->is_extern || fn->name[0] == '_') continue;
            if (fn->is_instance || (fn->owner && fn->owner->tmpl)) continue;
            if (fn->module != 0) continue;   /* libraries may have functions a program doesn't use */
            Diag *d = fn->owner ? diag_start(DIAG_WARNING, "W0004", fn->loc, "method `%s::%s` is never used",
                                             fn->owner->name, fn->name)
                                : diag_start(DIAG_WARNING, "W0004", fn->loc, "function `%s` is never used", fn->name);
            diag_label(d, "never called");
            char renamed[128];
            snprintf(renamed, sizeof renamed, "_%s", fn->name);
            diag_fix(d, fn->loc, renamed, "if this is intentional, prefix it with an underscore");
            diag_end(d);
        }
    }

    prog->needs_libc = S.needs_libc;
    prog->funcs = (Symbol **)ptrvec_finish(&S.funcs, arena, &prog->nfuncs);
    prog->globals = (Symbol **)ptrvec_finish(&S.globals, arena, &prog->nglobals);
    prog->enum_fns = (Symbol **)ptrvec_finish(&S.enum_fns, arena, &prog->nenum_fns);
    PtrVec classes = {0};
    for (size_t i = 0; i < S.structs.len; i++)
        if (((Type *)S.structs.items[i])->kind == TY_CLASS) ptrvec_push(&classes, S.structs.items[i]);
    prog->classes = (Type **)ptrvec_finish(&classes, arena, &prog->nclasses);
    free(S.vsites.items);
    free(S.templates.items);
    free(S.instances.items);
    free(S.structs.items);
    free(S.locals.items);
    free(S.marks);
    return diag_error_count() == errors_before;
}
