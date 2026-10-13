#include "sema.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

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
    Module *mod;
    PtrVec funcs;       /* Symbol* */
    PtrVec globals;     /* Symbol* */
    PtrVec structs;     /* Type* */

    /* per function */
    Symbol *fn;
    PtrVec locals;      /* Symbol*, innermost last */
    size_t *marks;
    size_t nmarks, marks_cap;
    int loop_depth;
    int fn_errors_before;
} Sema;

typedef struct {
    uint64_t bits;      /* ints (normalized), bools, pointers */
    double f;           /* floats */
} ConstVal;

static Type *check(Sema *S, Expr **slot, Type *hint);
static void check_effect(Sema *S, Expr **slot);
static bool const_eval(Sema *S, const Expr *e, ConstVal *out, bool report);

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
    return s;
}

static Symbol *find_in(PtrVec *v, const char *name) {
    for (size_t i = 0; i < v->len; i++) {
        Symbol *s = v->items[i];
        if (strcmp(s->name, name) == 0) return s;
    }
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
    return s ? s : find_in(&S->globals, name);
}

static Symbol *find_func(Sema *S, const char *name) {
    return find_in(&S->funcs, name);
}

static Type *find_struct(Sema *S, const char *name) {
    for (size_t i = 0; i < S->structs.len; i++) {
        Type *t = S->structs.items[i];
        if (strcmp(t->name, name) == 0) return t;
    }
    return NULL;
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
    Diag *d = diag_start(DIAG_ERROR, "E0322", loc, "cannot find type `%s`", name);
    diag_label(d, "not a known type");
    const char *names[64];
    size_t n = 0;
    for (size_t i = 0; i < sizeof builtin_type_names / sizeof *builtin_type_names; i++) names[n++] = builtin_type_names[i];
    for (size_t i = 0; i < S->structs.len && n < 64; i++) names[n++] = ((Type *)S->structs.items[i])->name;
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
        t = find_struct(S, te->u.name);
        if (!t) {
            report_unknown_type(S, te->loc, te->u.name);
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
    case TYPEX_ERROR_UNION:
        not_yet(te->loc, "error unions (`T!E`)", 9);
        return ty_error;
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
        uint32_t fa = ft->align ? ft->align : 1;
        offset = (offset + fa - 1) & ~(fa - 1);
        Field *field = &st->fields[st->nfields++];
        field->name = f->name;
        field->loc = f->loc;
        field->type = ft;
        field->offset = offset;
        offset += ft->size;
        if (fa > align) align = fa;
    }
    st->align = align;
    st->size = (offset + align - 1) & ~(align - 1);
    st->layout_state = 2;
}

static const Field *find_field(const Type *st, const char *name) {
    for (size_t i = 0; i < st->nfields; i++)
        if (strcmp(st->fields[i].name, name) == 0) return &st->fields[i];
    return NULL;
}

/* E0317 */
static void report_unknown_field(Sema *S, const Type *st, const char *name, Loc loc) {
    Diag *d = diag_start(DIAG_ERROR, "E0317", loc, "struct `%s` has no field named `%s`", st->name, name);
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

/* ---- lvalues ------------------------------------------------------------------- */

typedef enum { LV_NONE, LV_MUT, LV_CONST_VAR, LV_CONST_PTR } LvalueKind;

static LvalueKind lvalue_kind(const Expr *e) {
    switch (e->kind) {
    case EXPR_IDENT:
        if (!e->sym || e->sym->kind == SYM_FUNC) return LV_NONE;
        return e->sym->is_const ? LV_CONST_VAR : LV_MUT;
    case EXPR_UNARY:
        if (e->u.unary.op != TOK_STAR) return LV_NONE;
        return e->u.unary.operand->type->kind == TY_PTR && e->u.unary.operand->type->base_const ? LV_CONST_PTR : LV_MUT;
    case EXPR_MEMBER:
        if (e->via_pointer) return e->u.member.object->type->base_const ? LV_CONST_PTR : LV_MUT;
        return lvalue_kind(e->u.member.object);
    case EXPR_INDEX:
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
    Symbol *v = find_var(S, e->u.name);
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
        bool is_const = k == LV_CONST_VAR || k == LV_CONST_PTR;
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

static Type *check_binary(Sema *S, Expr *e, Type *hint) {
    TokenKind op = e->u.binary.op;
    Expr **lhs = &e->u.binary.lhs, **rhs = &e->u.binary.rhs;

    if (op == TOK_DOT) {
        not_yet(e->loc, "string concatenation with `.`", 7);
        return set_type(e, ty_error);
    }

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
        bool lp = lt->kind == TY_PTR || lt->kind == TY_NULL, rp = rt->kind == TY_PTR || rt->kind == TY_NULL;
        if (lp && rp && (eq || (lt->kind == TY_PTR && rt->kind == TY_PTR)) &&
            (type_assignable(lt, rt) || type_assignable(rt, lt) ||
             (lt->kind == TY_PTR && rt->kind == TY_PTR && type_equiv(lt->base, rt->base))))
            return set_type(e, ty_bool);
        Diag *d = diag_start(DIAG_ERROR, "E0310", e->loc, "can't compare `%s` with `%s`", type_name(lt), type_name(rt));
        diag_span(d, (*lhs)->loc, "`%s`", type_name(lt));
        diag_span(d, (*rhs)->loc, "`%s`", type_name(rt));
        if (type_is_aggregate(lt)) diag_note(d, "structs and arrays can't be compared with operators");
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

static Type *check_call(Sema *S, Expr *e) {
    Expr *callee = e->u.call.callee;
    if (callee->kind != EXPR_IDENT) {
        not_yet(callee->loc, "calling anything but a named function", 10);
        return set_type(e, ty_error);
    }
    Symbol *fn = find_func(S, callee->u.name);

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
            for (size_t i = 0; i < S->funcs.len && n < 256; i++) names[n++] = ((Symbol *)S->funcs.items[i])->name;
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

    e->sym = fn;
    callee->sym = fn;
    size_t n = e->u.call.nargs, want = fn->nparams;
    bool ok = true;
    if (n != want) {
        Diag *d = diag_start(DIAG_ERROR, "E0206", callee->loc, "function `%s` takes %zu argument%s but %zu %s supplied",
                             fn->name, want, want == 1 ? "" : "s", n, n == 1 ? "was" : "were");
        diag_label(d, "expected %zu argument%s, found %zu", want, want == 1 ? "" : "s", n);
        diag_span(d, fn->loc, "`%s` is defined here", fn->name);
        for (size_t i = want; i < n && i < want + 3; i++) diag_span(d, e->u.call.args[i]->loc, "unexpected argument");
        if (n < want) {
            char missing[256] = "";
            size_t used = 0;
            for (size_t i = n; i < want && used < sizeof missing - 60; i++)
                used += (size_t)snprintf(missing + used, sizeof missing - used, "%s`%s: %s`", i > n ? ", " : "",
                                         fn->decl->u.func.params[i]->name, type_name(fn->param_types[i]));
            diag_note(d, "missing: %s", missing);
        }
        diag_end(d);
        ok = false;
    }
    for (size_t i = 0; i < n; i++) {
        Type *pt = i < want ? fn->param_types[i] : NULL;
        check(S, &e->u.call.args[i], pt);
        if (pt) {
            const Param *p = fn->decl->u.func.params[i];
            char why[160];
            snprintf(why, sizeof why, "parameter `%s` is declared as `%s`", p->name, type_name(pt));
            ok = coerce(S, &e->u.call.args[i], pt, &p->type->loc, why) && ok;
        }
    }
    (void)ok;
    return set_type(e, fn->type);
}

static Type *check_member(Sema *S, Expr *e) {
    Type *t = check(S, &e->u.member.object, NULL);
    Expr *obj = e->u.member.object;
    if (is_err(t)) return set_type(e, ty_error);
    Type *st = t;
    e->via_pointer = false;
    if (t->kind == TY_PTR && t->base->kind == TY_STRUCT) {
        st = t->base;
        e->via_pointer = true;
    }
    if (st->kind != TY_STRUCT) {
        Diag *d = diag_start(DIAG_ERROR, "E0318", e->u.member.name_loc, "`->` needs a struct, found `%s`", type_name(t));
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

static Type *check_index(Sema *S, Expr *e) {
    Type *bt = check(S, &e->u.index.base, NULL);
    if (!e->u.index.index) {
        not_yet(e->loc, "appending with `a[] = x` (PHP-style arrays)", 12);
        return set_type(e, ty_error);
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
    diag_end(d);
    return set_type(e, ty_error);
}

static Type *check_struct_lit(Sema *S, Expr *e) {
    Type *st = find_struct(S, e->u.struct_lit.type_name);
    if (!st) {
        report_unknown_type(S, e->loc, e->u.struct_lit.type_name);
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

static Type *check_array_lit(Sema *S, Expr *e, Type *hint) {
    for (size_t i = 0; i < e->u.array.n; i++) {
        if (e->u.array.entries[i]->key) {
            not_yet(e->u.array.entries[i]->key->loc, "keys in array literals (PHP-style arrays)", 12);
            return set_type(e, ty_error);
        }
    }
    if (!hint || hint->kind != TY_ARRAY) {
        Diag *d = diag_start(DIAG_ERROR, "E0325", e->loc, "can't tell what type this array literal is");
        diag_label(d, hint ? "expected `%s` here" : "no type to infer from", hint ? type_name(hint) : "");
        diag_help(d, "array literals need a declared array type, like `int[3] xs = [1, 2, 3];`");
        diag_end(d);
        for (size_t i = 0; i < e->u.array.n; i++) check(S, &e->u.array.entries[i]->value, NULL);
        return set_type(e, ty_error);
    }
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
    if (hint && is_err(hint)) hint = NULL;
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
        return check_binary(S, e, hint);
    case EXPR_TERNARY:
        return check_ternary(S, e, hint);
    case EXPR_CALL:
        return check_call(S, e);
    case EXPR_MEMBER:
        return check_member(S, e);
    case EXPR_INDEX:
        return check_index(S, e);
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

    case EXPR_STRING:
    case EXPR_INTERP:     not_yet(e->loc, "strings", 7); break;
    case EXPR_THIS:       not_yet(e->loc, "classes", 10); break;
    case EXPR_SCOPE:      not_yet(e->loc, "enums", 12); break;
    case EXPR_NEW:        not_yet(e->loc, "`new`", 11); break;
    case EXPR_ALLOC:      not_yet(e->loc, "`alloc`", 6); break;
    case EXPR_TRY:        not_yet(e->loc, "error propagation with `?`", 9); break;
    }
    return set_type(e, ty_error);
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

static void check_assign(Sema *S, Expr *e) {
    TokenKind op = e->u.binary.op;
    set_type(e, ty_void);
    if (op == TOK_DOT_ASSIGN) {
        not_yet(e->loc, "string concatenation with `.=`", 7);
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
    Type *tt = target->kind == EXPR_IDENT ? check_ident(S, target, op != TOK_ASSIGN) : check(S, &e->u.binary.lhs, NULL);
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
    check(S, slot, NULL);
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

/* Fills `buf` with the initial bytes of a global. */
static bool const_init(Sema *S, const Expr *e, const Type *t, unsigned char *buf) {
    if (e->kind == EXPR_STRUCT_LIT && t->kind == TY_STRUCT) {
        bool ok = true;
        for (size_t i = 0; i < e->u.struct_lit.n; i++) {
            const FieldInit *fi = e->u.struct_lit.fields[i];
            const Field *f = find_field(t, fi->name);
            if (f) ok = const_init(S, fi->value, f->type, buf + f->offset) && ok;
        }
        return ok;
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

static void check_var_decl(Sema *S, VarDecl *vd) {
    Type *t = resolve_type(S, vd->type, false);
    if (vd->type->is_const && !vd->init) report_missing_const_value(vd);
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
        } else if (rt->kind != TY_VOID && !is_err(rt)) {
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
        if (S->loop_depth == 0) {
            bool is_break = s->kind == STMT_BREAK;
            Diag *d = diag_start(DIAG_ERROR, "E0212", s->loc, "`%s` outside of a loop", is_break ? "break" : "continue");
            diag_label(d, "can only be used inside a loop");
            if (is_break) diag_help(d, "to leave the function early, use `return`");
            diag_end(d);
        }
        break;
    case STMT_FOREACH: not_yet(s->loc, "`foreach`", 12); break;
    case STMT_ECHO:    not_yet(s->loc, "`echo`", 7); break;
    case STMT_DEFER:   not_yet(s->loc, "`defer`", 9); break;
    case STMT_ASM:     not_yet(s->loc, "inline asm", 13); break;
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

static void check_function_body(Sema *S, Symbol *fn) {
    const Decl *d = fn->decl;
    S->fn = fn;
    S->fn_errors_before = diag_error_count();
    S->locals.len = 0;
    S->nmarks = 0;
    S->loop_depth = 0;

    push_scope(S);
    for (size_t i = 0; i < d->u.func.nparams; i++) {
        Param *p = d->u.func.params[i];
        p->sym = declare_local(S, SYM_PARAM, p->name, p->loc, fn->param_types[i], p->type);
        p->sym->param_index = (int)i;
    }
    Stmt *b = d->u.func.body;
    check_stmt_list(S, b->u.block.stmts, b->u.block.n);
    pop_scope(S);

    if (fn->type->kind != TY_VOID && !is_err(fn->type) && falls_through(S, b)) {
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

/* Parameters and return values travel in registers; structs and arrays
 * by value need the full System V classification (milestone 6). */
static bool passable(Sema *S, Type *t, Loc loc, const char *what) {
    (void)S;
    if (!type_is_aggregate(t)) return true;
    Diag *d = diag_start(DIAG_ERROR, "E0900", loc, "not supported yet: %s of type `%s`", what, type_name(t));
    diag_label(d, "planned for milestone 6");
    char fixed[160];
    snprintf(fixed, sizeof fixed, "%s*", type_name(t));
    diag_fix(d, loc, fixed, "pass a pointer instead");
    diag_end(d);
    return false;
}

static void declare_function(Sema *S, Decl *d) {
    Symbol *fn = new_symbol(S, SYM_FUNC, d->name, d->name_loc);
    fn->decl = d;
    d->sym = fn;
    ptrvec_push(&S->funcs, fn);

    if (d->is_extern || d->is_export) {
        not_yet(d->loc, d->is_extern ? "`extern` functions" : "`export`", d->is_extern ? 6 : 13);
    }
    if (d->u.func.variadic) not_yet(d->name_loc, "variadic functions", 6);

    fn->type = d->u.func.ret ? resolve_type(S, d->u.func.ret, true) : ty_void;
    if (!is_err(fn->type) && !passable(S, fn->type, d->u.func.ret ? d->u.func.ret->loc : d->name_loc, "returning a value"))
        fn->type = ty_error;
    fn->nparams = d->u.func.nparams;
    fn->param_types = arena_alloc(S->arena, (fn->nparams ? fn->nparams : 1) * sizeof(Type *));
    for (size_t i = 0; i < fn->nparams; i++) {
        Param *p = d->u.func.params[i];
        Type *t = resolve_type(S, p->type, false);
        if (!is_err(t) && !passable(S, t, p->type->loc, "a parameter")) t = ty_error;
        fn->param_types[i] = t;
    }
}

static void declare_global(Sema *S, Decl *d) {
    VarDecl *vd = d->u.var;
    Symbol *g = new_symbol(S, SYM_GLOBAL, vd->name, vd->loc);
    g->decl = d;
    g->type_expr = vd->type;
    g->is_const = vd->type->is_const;
    d->sym = g;
    vd->sym = g;
    if (d->is_extern || d->is_export) not_yet(d->loc, d->is_extern ? "`extern` variables" : "`export`", d->is_extern ? 6 : 13);
    g->type = resolve_type(S, vd->type, false);
    if (g->is_const && !vd->init) report_missing_const_value(vd);

    if (vd->init && !is_err(g->type)) {
        if (vd->init->kind == EXPR_ASSIGN) {
            report_assign_as_value(vd->init);
        } else {
            check(S, &vd->init, g->type);
            if (coerce(S, &vd->init, g->type, &vd->type->loc, "expected because of this type")) {
                g->init = arena_alloc(S->arena, g->type->size ? g->type->size : 1);
                if (const_init(S, vd->init, g->type, g->init) && g->is_const && !type_is_aggregate(g->type)) {
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

static void check_duplicates(Sema *S) {
    Module *m = S->mod;
    for (size_t i = 0; i < m->n; i++) {
        const Decl *d = m->decls[i];
        bool is_type = d->kind == DECL_STRUCT;
        bool is_value = d->kind == DECL_FUNC || d->kind == DECL_VAR;
        if (!is_type && !is_value) continue;
        for (size_t j = 0; j < i; j++) {
            const Decl *prev = m->decls[j];
            bool same_space = is_type ? prev->kind == DECL_STRUCT : (prev->kind == DECL_FUNC || prev->kind == DECL_VAR);
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

bool sema_check(Arena *arena, Module *m, Program *prog) {
    int errors_before = diag_error_count();
    Sema S = {0};
    S.arena = arena;
    S.mod = m;

    check_duplicates(&S);

    /* Types first, so functions and globals can use any struct. */
    for (size_t i = 0; i < m->n; i++) {
        Decl *d = m->decls[i];
        switch (d->kind) {
        case DECL_STRUCT:
            if (!find_struct(&S, d->name)) {
                d->type = type_new_struct(d->name, d);
                ptrvec_push(&S.structs, d->type);
            }
            break;
        case DECL_IMPORT: not_yet(d->loc, "imports", 8); break;
        case DECL_CLASS:  not_yet(d->loc, "classes", 10); break;
        case DECL_ENUM:   not_yet(d->loc, "enums", 12); break;
        default: break;
        }
    }
    for (size_t i = 0; i < S.structs.len; i++) layout_struct(&S, S.structs.items[i]);

    for (size_t i = 0; i < m->n; i++)
        if (m->decls[i]->kind == DECL_FUNC && !find_func(&S, m->decls[i]->name)) declare_function(&S, m->decls[i]);
    for (size_t i = 0; i < m->n; i++)
        if (m->decls[i]->kind == DECL_VAR) declare_global(&S, m->decls[i]);

    Symbol *main_fn = find_func(&S, "main");
    if (!main_fn) {
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

    for (size_t i = 0; i < S.funcs.len; i++) {
        Symbol *fn = S.funcs.items[i];
        if (fn->decl->u.func.body) check_function_body(&S, fn);
    }

    prog->funcs = (Symbol **)ptrvec_finish(&S.funcs, arena, &prog->nfuncs);
    prog->globals = (Symbol **)ptrvec_finish(&S.globals, arena, &prog->nglobals);
    free(S.structs.items);
    free(S.locals.items);
    free(S.marks);
    return diag_error_count() == errors_before;
}
