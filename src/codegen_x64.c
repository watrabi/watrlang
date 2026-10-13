#include "codegen.h"

#include <stdarg.h>
#include <string.h>

#include "vec.h"

/*
 * x86-64 code generator, stack-machine style: every expression leaves its
 * value in rax, and binary operators park the left operand on the stack
 * while the right one is computed. The output is slow but simple to get
 * right; a register allocator comes later (milestone 15).
 *
 * Until the type checker exists (milestone 5) this file also does the
 * checking for the subset it supports: int variables, expressions,
 * control flow and functions. Everything else is reported as "not
 * supported yet" with the milestone that adds it.
 *
 * Frame layout (System V ABI):
 *   [rbp + 16 + 8*k]  stack argument k (the 7th parameter and later)
 *   [rbp + 8]         return address
 *   [rbp]             caller's rbp
 *   [rbp - 8*(i+1)]   local slot i (register parameters are spilled here first)
 * The frame size is rounded to 16 so rsp stays aligned for calls.
 *
 * Symbols: functions and globals are emitted as `w.<name>`. That keeps
 * them from clashing with register names (a function called `rax` would
 * otherwise assemble as `call rax`) and with C symbols once we link
 * against libc.
 */

typedef enum {
    VT_ERROR,   /* already reported */
    VT_VOID,
    VT_INT,
} ValType;

typedef struct {
    const char *name;
    Loc loc;
    const TypeExpr *type;  /* as written, for pointing at `const` in errors */
    bool is_global;
    bool is_param;
    bool is_const;
    int offset;          /* locals and params: rbp-relative */
    bool has_value;      /* const globals: value known at compile time */
    int64_t value;
    int reads;           /* for the unused-variable warning */
    int writes;          /* assignments after the declaration */
} Var;

typedef struct {
    int break_label;
    int continue_label;
} Loop;

static const char *const arg_regs[6] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};

typedef struct {
    FILE *out;              /* where emit() writes; a buffer while a function body is generated */
    const Module *mod;
    Arena arena;            /* Vars */
    int labels;

    PtrVec globals;         /* Var* */

    /* per function */
    const Decl *fn;
    ValType fn_ret;
    int ret_label;
    int depth;              /* 8-byte pushes since the frame was set up */
    int slots;              /* local slots used so far */
    int fn_errors_before;   /* error count when this function started */
    PtrVec locals;          /* Var*, innermost last */
    size_t *scope_marks;    /* locals.len at each open scope */
    size_t nscopes, scopes_cap;
    Loop *loops;
    size_t nloops, loops_cap;
} Gen;

static void emit(Gen *g, const char *fmt, ...) WATR_PRINTF(2, 3);
static void emit(Gen *g, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("    ", g->out);
    vfprintf(g->out, fmt, ap);
    fputc('\n', g->out);
    va_end(ap);
}

static int new_label(Gen *g) {
    return g->labels++;
}

static void emit_label(Gen *g, int label) {
    fprintf(g->out, ".L%d:\n", label);
}

static void push_rax(Gen *g) {
    emit(g, "push rax");
    g->depth++;
}

static void pop_reg(Gen *g, const char *reg) {
    emit(g, "pop %s", reg);
    g->depth--;
}

static void not_yet(Loc loc, const char *what, int milestone) {
    Diag *d = diag_start(DIAG_ERROR, "E0900", loc, "not supported yet: %s", what);
    diag_label(d, "planned for milestone %d", milestone);
    diag_note(d, "watrlang is still being built; docs/DESIGN.md has the roadmap");
    diag_end(d);
}

/* Where a function's return type is written, or its name if there's none. */
static Loc ret_type_loc(const Decl *fn) {
    return fn->u.func.ret ? fn->u.func.ret->loc : fn->name_loc;
}

/* The `const ` at the start of a type, for "remove `const`" fixes. */
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

/* ---- types (int and void only, until milestone 5) ------------------------- */

static bool is_int_type(const TypeExpr *t) {
    return t && t->kind == TYPEX_BUILTIN && t->u.builtin == TOK_KW_INT_T && !t->is_volatile;
}

static bool is_void_type(const TypeExpr *t) {
    return t && t->kind == TYPEX_BUILTIN && t->u.builtin == TOK_KW_VOID && !t->is_const && !t->is_volatile;
}

/* Checks a variable/parameter type is (const) int. */
static bool check_var_type(const TypeExpr *t) {
    if (is_int_type(t)) return true;
    not_yet(t->loc, "types other than int", 5);
    return false;
}

static ValType ret_type_of(const Decl *fn) {
    const TypeExpr *t = fn->u.func.ret;
    if (!t || is_void_type(t)) return VT_VOID;
    if (is_int_type(t) && !t->is_const) return VT_INT;
    return VT_ERROR;  /* reported once, at the definition */
}

static const Decl *find_function(const Module *m, const char *name) {
    for (size_t i = 0; i < m->n; i++) {
        const Decl *d = m->decls[i];
        if (d->kind == DECL_FUNC && strcmp(d->name, name) == 0) return d;
    }
    return NULL;
}

/* ---- scopes ---------------------------------------------------------------- */

static void push_scope(Gen *g) {
    if (g->nscopes == g->scopes_cap) {
        g->scopes_cap = g->scopes_cap ? g->scopes_cap * 2 : 16;
        size_t *grown = realloc(g->scope_marks, g->scopes_cap * sizeof *grown);
        if (!grown) fatal("out of memory");
        g->scope_marks = grown;
    }
    g->scope_marks[g->nscopes++] = g->locals.len;
}

static void warn_if_unused(const Var *v);

static void pop_scope(Gen *g) {
    size_t mark = g->scope_marks[--g->nscopes];
    /* After an error, "unused" is often wrong: the broken code may be
     * the use. Stay quiet until the function compiles. */
    if (diag_error_count() == g->fn_errors_before)
        for (size_t i = mark; i < g->locals.len; i++) warn_if_unused(g->locals.items[i]);
    g->locals.len = mark;
}

static Var *find_local(Gen *g, const char *name) {
    for (size_t i = g->locals.len; i-- > 0;) {
        Var *v = g->locals.items[i];
        if (strcmp(v->name, name) == 0) return v;
    }
    return NULL;
}

static Var *find_global(Gen *g, const char *name) {
    for (size_t i = 0; i < g->globals.len; i++) {
        Var *v = g->globals.items[i];
        if (strcmp(v->name, name) == 0) return v;
    }
    return NULL;
}

static Var *find_var(Gen *g, const char *name) {
    Var *v = find_local(g, name);
    return v ? v : find_global(g, name);
}

/* Declares a local in the current scope. Locals can't shadow other locals
 * or parameters of the same function (a classic source of bugs); they may
 * shadow globals. */
static Var *declare_local_at(Gen *g, const char *name, Loc loc, const TypeExpr *type, bool is_param, int offset) {
    Var *prev = find_local(g, name);
    if (prev) {
        Diag *d = diag_start(DIAG_ERROR, "E0203", loc, "`%s` is already declared in this function", name);
        diag_label(d, "declared again here");
        diag_span(d, prev->loc, "first declared here%s", prev->is_param ? ", as a parameter" : "");
        diag_help(d, "give it a different name; watrlang doesn't allow shadowing inside a function");
        diag_end(d);
        prev->reads++;  /* don't also warn that the first one is unused */
    }
    Var *v = ARENA_NEW(&g->arena, Var);
    v->name = name;
    v->loc = loc;
    v->type = type;
    v->is_param = is_param;
    v->is_const = type->is_const;
    v->offset = offset;
    ptrvec_push(&g->locals, v);
    return v;
}

static Var *declare_local(Gen *g, const char *name, Loc loc, const TypeExpr *type, bool is_param) {
    return declare_local_at(g, name, loc, type, is_param, -8 * (++g->slots));
}

/* W0001: called for each local as its scope closes. */
static void warn_if_unused(const Var *v) {
    if (v->reads > 0 || v->name[0] == '_') return;
    Diag *d;
    if (v->writes > 0) {
        d = diag_start(DIAG_WARNING, "W0001", v->loc, "variable `%s` is assigned to, but never read", v->name);
        diag_label(d, "its value is never used");
    } else {
        d = diag_start(DIAG_WARNING, "W0001", v->loc, "unused %s `%s`", v->is_param ? "parameter" : "variable",
                       v->name);
        diag_label(d, "never used");
    }
    char renamed[128];
    snprintf(renamed, sizeof renamed, "_%s", v->name);
    diag_fix(d, v->loc, renamed, "if this is intentional, prefix it with an underscore");
    diag_end(d);
}

/* Best "did you mean" candidate among visible variables, or NULL. */
static const char *similar_var(Gen *g, const char *name) {
    const char *best = NULL;
    size_t best_dist = (size_t)-1;
    for (size_t pass = 0; pass < 2; pass++) {
        PtrVec *vars = pass == 0 ? &g->locals : &g->globals;
        for (size_t i = 0; i < vars->len; i++) {
            const Var *v = vars->items[i];
            if (!diag_similar(name, v->name)) continue;
            size_t dist = diag_edit_distance(name, v->name);
            if (dist < best_dist) {
                best = v->name;
                best_dist = dist;
            }
        }
    }
    return best;
}

static const char *similar_function(Gen *g, const char *name) {
    const char *best = NULL;
    size_t best_dist = (size_t)-1;
    for (size_t i = 0; i < g->mod->n; i++) {
        const Decl *d = g->mod->decls[i];
        if (d->kind != DECL_FUNC || !diag_similar(name, d->name)) continue;
        size_t dist = diag_edit_distance(name, d->name);
        if (dist < best_dist) {
            best = d->name;
            best_dist = dist;
        }
    }
    return best;
}

/* E0200 */
static void report_unknown_var(Gen *g, const Expr *e) {
    Diag *d = diag_start(DIAG_ERROR, "E0200", e->loc, "cannot find variable `%s` in this scope", e->u.name);
    diag_label(d, "not found in this scope");
    const char *guess = similar_var(g, e->u.name);
    if (guess) {
        Var *v = find_var(g, guess);
        diag_span(d, v->loc, "a %s with a similar name is declared here",
                  v->is_global ? "global" : v->is_param ? "parameter" : "variable");
        diag_fix(d, e->loc, guess, "a %s with a similar name exists", v->is_global ? "global" : "variable");
    }
    diag_end(d);
}

static void load_var(Gen *g, const Var *v) {
    if (v->is_global) emit(g, "mov rax, qword ptr [rip + w.%s]", v->name);
    else emit(g, "mov rax, qword ptr [rbp %+d]", v->offset);
}

static void store_var(Gen *g, const Var *v, const char *reg) {
    if (v->is_global) emit(g, "mov qword ptr [rip + w.%s], %s", v->name, reg);
    else emit(g, "mov qword ptr [rbp %+d], %s", v->offset, reg);
}

/* ---- constant evaluation --------------------------------------------------- */

/* Evaluates an int expression at compile time (global initializers, and
 * spotting `while (1)` for return analysis). With report=false it fails
 * silently. Arithmetic wraps exactly like the generated code. */
static bool const_eval(Gen *g, const Expr *e, int64_t *out, bool report) {
    switch (e->kind) {
    case EXPR_INT:
        if (e->u.int_val > (uint64_t)INT64_MAX) {
            if (report) {
                Diag *d = diag_start(DIAG_ERROR, "E0301", e->loc, "integer literal is too large for `int`");
                diag_label(d, "doesn't fit in `int`");
                diag_note(d, "`int` is 64-bit signed; the largest value is 9223372036854775807");
                diag_end(d);
            }
            return false;
        }
        *out = (int64_t)e->u.int_val;
        return true;
    case EXPR_CHAR:
        *out = (int64_t)e->u.int_val;
        return true;
    case EXPR_IDENT: {
        /* Constant globals fold only at the top level. Inside a function a
         * local could shadow the name, so `while (FLAG)` isn't treated as
         * a constant there. */
        Var *v = g->fn ? NULL : find_global(g, e->u.name);
        if (v && v->has_value) {
            *out = v->value;
            return true;
        }
        if (report) {
            Diag *d = diag_start(DIAG_ERROR, "E0307", e->loc, "`%s` isn't a constant", e->u.name);
            diag_label(d, "not a compile-time constant");
            Var *gv = find_global(g, e->u.name);
            if (gv && !gv->is_const) {
                diag_span(d, gv->loc, "`%s` is declared without `const`, so it can change", gv->name);
                diag_help(d, "make `%s` const, or set this global at the start of `main` instead", gv->name);
            } else if (!gv) {
                diag_note(d, "global initializers can only use literals, operators and earlier `const` globals");
            }
            diag_end(d);
        }
        return false;
    }
    case EXPR_UNARY: {
        const Expr *operand = e->u.unary.operand;
        TokenKind op = e->u.unary.op;
        if (op == TOK_MINUS && operand->kind == EXPR_INT && operand->u.int_val == (uint64_t)INT64_MAX + 1) {
            *out = INT64_MIN;
            return true;
        }
        int64_t v;
        if (op != TOK_MINUS && op != TOK_TILDE && op != TOK_BANG) break;
        if (!const_eval(g, operand, &v, report)) return false;
        if (op == TOK_MINUS) *out = (int64_t)(0 - (uint64_t)v);
        else if (op == TOK_TILDE) *out = ~v;
        else *out = v == 0;
        return true;
    }
    case EXPR_BINARY: {
        TokenKind op = e->u.binary.op;
        int64_t a, b;
        if (!const_eval(g, e->u.binary.lhs, &a, report)) return false;
        if (op == TOK_ANDAND || op == TOK_OROR) {
            if ((op == TOK_ANDAND) == (a == 0)) {
                *out = op == TOK_OROR;
                return true;
            }
            if (!const_eval(g, e->u.binary.rhs, &b, report)) return false;
            *out = b != 0;
            return true;
        }
        if (!const_eval(g, e->u.binary.rhs, &b, report)) return false;
        uint64_t ua = (uint64_t)a, ub = (uint64_t)b;
        switch (op) {
        case TOK_PLUS:  *out = (int64_t)(ua + ub); return true;
        case TOK_MINUS: *out = (int64_t)(ua - ub); return true;
        case TOK_STAR:  *out = (int64_t)(ua * ub); return true;
        case TOK_SLASH:
        case TOK_PERCENT:
            if (b == 0 || (a == INT64_MIN && b == -1)) {
                if (report) {
                    Diag *d = diag_start(DIAG_ERROR, "E0308", e->loc, b == 0 ? "division by zero in a constant expression"
                                                                       : "constant division overflows `int`");
                    diag_label(d, b == 0 ? "dividing by zero" : "-9223372036854775808 / -1 doesn't fit in `int`");
                    diag_span(d, e->u.binary.rhs->loc, "this is %lld", (long long)b);
                    diag_end(d);
                }
                return false;
            }
            *out = op == TOK_SLASH ? a / b : a % b;
            return true;
        case TOK_AMP:   *out = a & b; return true;
        case TOK_PIPE:  *out = a | b; return true;
        case TOK_CARET: *out = a ^ b; return true;
        case TOK_SHL:   *out = (int64_t)(ua << (ub & 63)); return true;
        case TOK_SHR:   /* arithmetic shift without relying on implementation-defined >> */
            *out = a < 0 ? (int64_t)~(~ua >> (ub & 63)) : (int64_t)(ua >> (ub & 63));
            return true;
        case TOK_EQ: *out = a == b; return true;
        case TOK_NE: *out = a != b; return true;
        case TOK_LT: *out = a < b; return true;
        case TOK_GT: *out = a > b; return true;
        case TOK_LE: *out = a <= b; return true;
        case TOK_GE: *out = a >= b; return true;
        default: break;
        }
        break;
    }
    case EXPR_TERNARY: {
        int64_t c;
        if (!const_eval(g, e->u.ternary.cond, &c, report)) return false;
        return const_eval(g, c ? e->u.ternary.then_expr : e->u.ternary.else_expr, out, report);
    }
    default:
        break;
    }
    if (report) {
        Diag *d = diag_start(DIAG_ERROR, "E0307", e->loc, "global initializers must be constant expressions");
        diag_label(d, "not a compile-time constant");
        if (e->kind == EXPR_CALL) diag_note(d, "function calls aren't evaluated at compile time");
        diag_help(d, "set this global at the start of `main` instead");
        diag_end(d);
    }
    return false;
}

/* ---- expressions --------------------------------------------------------- */

static ValType gen_expr(Gen *g, const Expr *e);
static void gen_effect(Gen *g, const Expr *e);
static void report_assign_as_value(const Expr *e);

/* Generates `e` and requires it to produce an int. */
static bool gen_int(Gen *g, const Expr *e) {
    ValType t = gen_expr(g, e);
    if (t == VT_VOID) {
        Diag *d = diag_start(DIAG_ERROR, "E0300", e->loc, "expected a value, but this call returns nothing");
        diag_label(d, "has no value");
        if (e->kind == EXPR_CALL && e->u.call.callee->kind == EXPR_IDENT) {
            const Decl *fn = find_function(g->mod, e->u.call.callee->u.name);
            if (fn) diag_span(d, ret_type_loc(fn), fn->u.func.ret ? "`%s` is declared to return `void`"
                                                                  : "`%s` has no return type, so it returns nothing",
                              fn->name);
        }
        diag_end(d);
        return false;
    }
    return t == VT_INT;
}

static void emit_mov_imm(Gen *g, int64_t v) {
    if (v == 0) emit(g, "xor eax, eax");
    else if (v >= INT32_MIN && v <= INT32_MAX) emit(g, "mov rax, %lld", (long long)v);
    else emit(g, "movabs rax, %lld", (long long)v);
}

/* rax = rax <op> rcx for every non-short-circuit binary operator. */
static bool emit_arith(Gen *g, TokenKind op) {
    switch (op) {
    case TOK_PLUS:    emit(g, "add rax, rcx"); return true;
    case TOK_MINUS:   emit(g, "sub rax, rcx"); return true;
    case TOK_STAR:    emit(g, "imul rax, rcx"); return true;
    case TOK_SLASH:   emit(g, "cqo"); emit(g, "idiv rcx"); return true;
    case TOK_PERCENT: emit(g, "cqo"); emit(g, "idiv rcx"); emit(g, "mov rax, rdx"); return true;
    case TOK_AMP:     emit(g, "and rax, rcx"); return true;
    case TOK_PIPE:    emit(g, "or rax, rcx"); return true;
    case TOK_CARET:   emit(g, "xor rax, rcx"); return true;
    case TOK_SHL:     emit(g, "shl rax, cl"); return true;
    case TOK_SHR:     emit(g, "sar rax, cl"); return true;  /* int is signed: arithmetic shift */
    default: break;
    }
    const char *setcc = NULL;
    switch (op) {
    case TOK_EQ: setcc = "sete"; break;
    case TOK_NE: setcc = "setne"; break;
    case TOK_LT: setcc = "setl"; break;
    case TOK_GT: setcc = "setg"; break;
    case TOK_LE: setcc = "setle"; break;
    case TOK_GE: setcc = "setge"; break;
    default: return false;
    }
    emit(g, "cmp rax, rcx");
    emit(g, "%s al", setcc);
    emit(g, "movzx eax, al");
    return true;
}

/* a && b, a || b: short-circuit, result is 0 or 1. */
static ValType gen_logical(Gen *g, const Expr *e) {
    bool is_and = e->u.binary.op == TOK_ANDAND;
    int short_label = new_label(g), end_label = new_label(g);

    bool ok = gen_int(g, e->u.binary.lhs);
    emit(g, "test rax, rax");
    emit(g, "%s .L%d", is_and ? "je" : "jne", short_label);
    ok = gen_int(g, e->u.binary.rhs) && ok;
    emit(g, "test rax, rax");
    emit(g, "setne al");
    emit(g, "movzx eax, al");
    emit(g, "jmp .L%d", end_label);
    emit_label(g, short_label);
    emit(g, "mov eax, %d", is_and ? 0 : 1);
    emit_label(g, end_label);
    return ok ? VT_INT : VT_ERROR;
}

static ValType gen_binary(Gen *g, const Expr *e) {
    TokenKind op = e->u.binary.op;
    if (op == TOK_DOT) {
        not_yet(e->loc, "string concatenation with `.`", 7);
        return VT_ERROR;
    }
    if (op == TOK_ANDAND || op == TOK_OROR) return gen_logical(g, e);

    bool ok = gen_int(g, e->u.binary.lhs);
    push_rax(g);
    ok = gen_int(g, e->u.binary.rhs) && ok;
    emit(g, "mov rcx, rax");
    pop_reg(g, "rax");
    if (!emit_arith(g, op)) {
        diag_error(e->loc, "internal error: unexpected binary operator '%s'", token_kind_spelling(op));
        return VT_ERROR;
    }
    return ok ? VT_INT : VT_ERROR;
}

/* The variable an assignment or ++/-- writes to, or NULL after an error. */
static Var *assign_target(Gen *g, const Expr *target, const char *action) {
    if (target->kind != EXPR_IDENT) {
        if (target->kind == EXPR_INDEX || target->kind == EXPR_MEMBER ||
            (target->kind == EXPR_UNARY && target->u.unary.op == TOK_STAR)) {
            not_yet(target->loc, "assigning through pointers, fields or indexes", 5);
        } else {
            Diag *d = diag_start(DIAG_ERROR, "E0209", target->loc, "invalid left-hand side: can't %s this", action);
            diag_label(d, "not a variable");
            diag_note(d, "only variables can be assigned to");
            diag_end(d);
        }
        return NULL;
    }
    Var *v = find_var(g, target->u.name);
    if (!v) {
        const Decl *fn = find_function(g->mod, target->u.name);
        if (fn) {
            Diag *d = diag_start(DIAG_ERROR, "E0209", target->loc, "can't %s `%s` because it's a function", action,
                                 target->u.name);
            diag_label(d, "not a variable");
            diag_span(d, fn->name_loc, "`%s` is defined here", fn->name);
            diag_end(d);
        } else {
            report_unknown_var(g, target);
        }
        return NULL;
    }
    if (v->is_const) {
        Diag *d = diag_start(DIAG_ERROR, "E0207", target->loc, "cannot %s `%s`, because it's a constant", action, v->name);
        diag_label(d, "can't %s a constant", action);
        diag_span(d, v->loc, "`%s` is declared `const` here", v->name);
        Loc kw;
        if (const_keyword_loc(v->type, &kw)) diag_fix(d, kw, "", "if `%s` needs to change, remove `const`", v->name);
        diag_end(d);
        return NULL;
    }
    v->writes++;
    return v;
}

/* ++x --x x++ x-- */
static ValType gen_incdec(Gen *g, const Expr *e, bool prefix) {
    bool inc = e->u.unary.op == TOK_PLUSPLUS;
    Var *v = assign_target(g, e->u.unary.operand, inc ? "increment" : "decrement");
    if (!v) return VT_ERROR;
    load_var(g, v);
    if (prefix) {
        emit(g, "%s rax, 1", inc ? "add" : "sub");
        store_var(g, v, "rax");
    } else {
        emit(g, "lea rcx, [rax %s 1]", inc ? "+" : "-");
        store_var(g, v, "rcx");
    }
    return VT_INT;
}

static ValType gen_unary(Gen *g, const Expr *e) {
    TokenKind op = e->u.unary.op;
    const Expr *operand = e->u.unary.operand;

    /* -9223372036854775808: the literal alone doesn't fit in int, the negation does. */
    if (op == TOK_MINUS && operand->kind == EXPR_INT && operand->u.int_val == (uint64_t)INT64_MAX + 1) {
        emit(g, "movabs rax, %lld", (long long)INT64_MIN);
        return VT_INT;
    }

    switch (op) {
    case TOK_MINUS: case TOK_TILDE: case TOK_BANG:
        break;
    case TOK_PLUSPLUS: case TOK_MINUSMINUS:
        return gen_incdec(g, e, true);
    default:
        not_yet(e->loc, op == TOK_AMP ? "taking addresses with `&`" : "dereferencing with `*`", 5);
        return VT_ERROR;
    }

    if (!gen_int(g, operand)) return VT_ERROR;
    if (op == TOK_MINUS) {
        emit(g, "neg rax");
    } else if (op == TOK_TILDE) {
        emit(g, "not rax");
    } else {
        emit(g, "test rax, rax");
        emit(g, "sete al");
        emit(g, "movzx eax, al");
    }
    return VT_INT;
}

static ValType gen_ternary(Gen *g, const Expr *e) {
    int else_label = new_label(g), end_label = new_label(g);
    bool ok = gen_int(g, e->u.ternary.cond);
    emit(g, "test rax, rax");
    emit(g, "je .L%d", else_label);
    ok = gen_int(g, e->u.ternary.then_expr) && ok;
    emit(g, "jmp .L%d", end_label);
    emit_label(g, else_label);
    ok = gen_int(g, e->u.ternary.else_expr) && ok;
    emit_label(g, end_label);
    return ok ? VT_INT : VT_ERROR;
}

/*
 * Arguments are evaluated left to right and pushed. Then the 7th and later
 * are copied down to where the ABI wants them ([rsp], [rsp+8], ...), the
 * first six are loaded into registers, and rsp is padded so it's 16-byte
 * aligned at the call.
 */
static ValType gen_call(Gen *g, const Expr *e) {
    const Expr *callee = e->u.call.callee;
    if (callee->kind != EXPR_IDENT) {
        not_yet(callee->loc, "calling anything but a named function", 10);
        return VT_ERROR;
    }
    const Decl *fn = find_function(g->mod, callee->u.name);
    if (!fn) {
        Var *v = find_var(g, callee->u.name);
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
            const char *guess = similar_function(g, callee->u.name);
            if (guess) {
                diag_span(d, find_function(g->mod, guess)->name_loc, "a function with a similar name is defined here");
                diag_fix(d, callee->loc, guess, "a function with a similar name exists");
            }
        }
        diag_end(d);
        for (size_t i = 0; i < e->u.call.nargs; i++) gen_expr(g, e->u.call.args[i]);  /* still check them */
        return VT_ERROR;
    }

    size_t n = e->u.call.nargs;
    size_t want = fn->u.func.nparams;
    bool ok = true;
    if (n != want) {
        Diag *d = diag_start(DIAG_ERROR, "E0206", callee->loc, "function `%s` takes %zu argument%s but %zu %s supplied",
                             fn->name, want, want == 1 ? "" : "s", n, n == 1 ? "was" : "were");
        diag_label(d, "expected %zu argument%s, found %zu", want, want == 1 ? "" : "s", n);
        diag_span(d, fn->name_loc, "`%s` is defined here", fn->name);
        for (size_t i = want; i < n && i < want + 3; i++)
            diag_span(d, e->u.call.args[i]->loc, "unexpected argument");
        if (n < want) {
            char missing[256] = "";
            size_t used = 0;
            for (size_t i = n; i < want && used < sizeof missing - 40; i++)
                used += (size_t)snprintf(missing + used, sizeof missing - used, "%s`%s`", i > n ? ", " : "",
                                         fn->u.func.params[i]->name);
            diag_note(d, "missing: %s", missing);
        }
        diag_end(d);
        ok = false;
    }

    for (size_t i = 0; i < n; i++) {
        ok = gen_int(g, e->u.call.args[i]) && ok;
        push_rax(g);
    }

    int nstack = n > 6 ? (int)n - 6 : 0;
    int pad = (g->depth + nstack) % 2 != 0;
    int extra = 8 * (nstack + pad);
    if (extra) emit(g, "sub rsp, %d", extra);
    /* With everything pushed, argument i sits at [rsp + extra + 8*(n-1-i)]. */
    for (size_t i = 6; i < n; i++) {
        emit(g, "mov rax, qword ptr [rsp + %d]", extra + 8 * (int)(n - 1 - i));
        emit(g, "mov qword ptr [rsp + %d], rax", 8 * (int)(i - 6));
    }
    for (size_t i = 0; i < n && i < 6; i++)
        emit(g, "mov %s, qword ptr [rsp + %d]", arg_regs[i], extra + 8 * (int)(n - 1 - i));

    emit(g, "call w.%s", fn->name);
    int cleanup = extra + 8 * (int)n;
    if (cleanup) emit(g, "add rsp, %d", cleanup);
    g->depth -= (int)n;

    return ok ? ret_type_of(fn) : VT_ERROR;
}

static ValType gen_expr(Gen *g, const Expr *e) {
    switch (e->kind) {
    case EXPR_INT:
        if (e->u.int_val > (uint64_t)INT64_MAX) {
            Diag *d = diag_start(DIAG_ERROR, "E0301", e->loc, "integer literal is too large for `int`");
            diag_label(d, "doesn't fit in `int`");
            diag_note(d, "`int` is 64-bit signed; the largest value is 9223372036854775807");
            diag_end(d);
            return VT_ERROR;
        }
        emit_mov_imm(g, (int64_t)e->u.int_val);
        return VT_INT;
    case EXPR_CHAR:
        emit_mov_imm(g, (int64_t)e->u.int_val);
        return VT_INT;
    case EXPR_IDENT: {
        Var *v = find_var(g, e->u.name);
        if (v) {
            v->reads++;
            load_var(g, v);
            return VT_INT;
        }
        const Decl *fn = find_function(g->mod, e->u.name);
        if (fn) {
            Diag *d = diag_start(DIAG_ERROR, "E0205", e->loc, "`%s` is a function, not a value", e->u.name);
            diag_label(d, "function used without calling it");
            if (fn->u.func.nparams == 0) {
                char call[128];
                snprintf(call, sizeof call, "%s()", e->u.name);
                diag_fix(d, e->loc, call, "call it");
            } else {
                diag_help(d, "call it with %zu argument%s: `%s(...)`", fn->u.func.nparams,
                          fn->u.func.nparams == 1 ? "" : "s", e->u.name);
            }
            diag_end(d);
        } else {
            report_unknown_var(g, e);
        }
        return VT_ERROR;
    }
    case EXPR_UNARY:
        return gen_unary(g, e);
    case EXPR_POSTFIX:
        return gen_incdec(g, e, false);
    case EXPR_BINARY:
        return gen_binary(g, e);
    case EXPR_TERNARY:
        return gen_ternary(g, e);
    case EXPR_CALL:
        return gen_call(g, e);
    case EXPR_ASSIGN:
        report_assign_as_value(e);
        return VT_ERROR;

    case EXPR_FLOAT:      not_yet(e->loc, "floats", 5); break;
    case EXPR_STRING:
    case EXPR_INTERP:     not_yet(e->loc, "strings", 7); break;
    case EXPR_BOOL:       not_yet(e->loc, "true and false", 5); break;
    case EXPR_NULL:       not_yet(e->loc, "null", 5); break;
    case EXPR_THIS:       not_yet(e->loc, "classes", 10); break;
    case EXPR_SCOPE:      not_yet(e->loc, "enums", 12); break;
    case EXPR_INDEX:      not_yet(e->loc, "indexing", 5); break;
    case EXPR_MEMBER:     not_yet(e->loc, "member access with '->'", 5); break;
    case EXPR_CAST:       not_yet(e->loc, "casts with 'as'", 5); break;
    case EXPR_NEW:        not_yet(e->loc, "'new'", 11); break;
    case EXPR_ALLOC:      not_yet(e->loc, "'alloc'", 5); break;
    case EXPR_SIZEOF:     not_yet(e->loc, "'sizeof'", 5); break;
    case EXPR_TRY:        not_yet(e->loc, "error propagation with '?'", 9); break;
    case EXPR_ARRAY_LIT:  not_yet(e->loc, "array literals", 5); break;
    case EXPR_STRUCT_LIT: not_yet(e->loc, "struct literals", 5); break;
    }
    return VT_ERROR;
}

/* E0210. For `x = y` it suggests `x == y`. */
static void report_assign_as_value(const Expr *e) {
    Diag *d = diag_start(DIAG_ERROR, "E0210", e->loc, "an assignment can't be used as a value");
    diag_label(d, "assignment used as a value");
    diag_note(d, "in watrlang, assignment is a statement, not an expression");
    if (e->u.binary.op == TOK_ASSIGN) {
        /* Find the `=` between the two sides to suggest `==`. */
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
    default:                 return TOK_ERROR;  /* .= */
    }
}

static void gen_assign(Gen *g, const Expr *e) {
    TokenKind op = e->u.binary.op;
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
    Var *v = assign_target(g, e->u.binary.lhs, "assign to");
    if (op == TOK_ASSIGN) {
        if (gen_int(g, e->u.binary.rhs) && v) store_var(g, v, "rax");
        return;
    }
    if (!v) {
        gen_int(g, e->u.binary.rhs);
        return;
    }
    load_var(g, v);
    push_rax(g);
    bool ok = gen_int(g, e->u.binary.rhs);
    emit(g, "mov rcx, rax");
    pop_reg(g, "rax");
    emit_arith(g, compound_to_binary(op));
    if (ok) store_var(g, v, "rax");
}

/* An expression whose value is thrown away: statements, for-loop steps. */
static void gen_effect(Gen *g, const Expr *e) {
    if (e->kind == EXPR_ASSIGN) gen_assign(g, e);
    else gen_expr(g, e);
}

/* ---- statements ---------------------------------------------------------- */

static void gen_stmt(Gen *g, const Stmt *s);
static bool falls_through(Gen *g, const Stmt *s);

/* Generates a statement list, warning once if code follows a statement
 * control can't get past (W0002). */
static void gen_stmt_list(Gen *g, Stmt **stmts, size_t n) {
    bool warned = false;
    for (size_t i = 0; i < n; i++) {
        if (!warned && i > 0 && diag_error_count() == g->fn_errors_before && !falls_through(g, stmts[i - 1])) {
            Diag *d = diag_start(DIAG_WARNING, "W0002", stmts[i]->loc, "unreachable statement");
            diag_label(d, "this can never run");
            diag_span(d, stmts[i - 1]->loc, "any code after this is unreachable");
            diag_end(d);
            warned = true;
        }
        gen_stmt(g, stmts[i]);
    }
}

static void gen_scoped(Gen *g, const Stmt *s) {
    push_scope(g);
    if (s->kind == STMT_BLOCK) gen_stmt_list(g, s->u.block.stmts, s->u.block.n);
    else gen_stmt(g, s);
    pop_scope(g);
}

static void push_loop(Gen *g, int break_label, int continue_label) {
    if (g->nloops == g->loops_cap) {
        g->loops_cap = g->loops_cap ? g->loops_cap * 2 : 8;
        Loop *grown = realloc(g->loops, g->loops_cap * sizeof *grown);
        if (!grown) fatal("out of memory");
        g->loops = grown;
    }
    g->loops[g->nloops].break_label = break_label;
    g->loops[g->nloops].continue_label = continue_label;
    g->nloops++;
}

static void gen_cond_jump_false(Gen *g, const Expr *cond, int label) {
    gen_int(g, cond);
    emit(g, "test rax, rax");
    emit(g, "je .L%d", label);
}

/* E0208 */
static void report_missing_const_value(const VarDecl *vd) {
    Diag *d = diag_start(DIAG_ERROR, "E0208", vd->loc, "constant `%s` needs a value", vd->name);
    diag_label(d, "declared without a value");
    diag_note(d, "a constant can never be assigned later, so it must be given a value here");
    diag_fix(d, loc_after(vd->loc), " = 0", "give it a value");
    diag_end(d);
}

static void gen_var_decl(Gen *g, const VarDecl *vd) {
    bool type_ok = check_var_type(vd->type);
    if (vd->type->is_const && !vd->init) report_missing_const_value(vd);

    /* The initializer is generated before the name exists, so `int x = x;`
     * can't read the variable it's declaring. */
    if (vd->init) {
        if (vd->init->kind == EXPR_ASSIGN) {
            report_assign_as_value(vd->init);
            type_ok = false;
        } else if (!gen_int(g, vd->init)) {
            type_ok = false;
        }
    } else {
        emit(g, "xor eax, eax");  /* locals start at zero */
    }
    Var *v = declare_local(g, vd->name, vd->loc, vd->type, false);
    if (type_ok) store_var(g, v, "rax");
}

static void gen_stmt(Gen *g, const Stmt *s) {
    switch (s->kind) {
    case STMT_BLOCK:
        gen_scoped(g, s);
        break;

    case STMT_EXPR:
        gen_effect(g, s->u.expr);
        break;

    case STMT_VAR:
        gen_var_decl(g, s->u.var);
        break;

    case STMT_RETURN:
        if (g->fn_ret == VT_VOID && s->u.expr) {
            Diag *d = diag_start(DIAG_ERROR, "E0302", s->u.expr->loc, "function `%s` doesn't return a value",
                                 g->fn->name);
            diag_label(d, "unexpected return value");
            diag_span(d, ret_type_loc(g->fn), g->fn->u.func.ret ? "declared to return `void` here"
                                                                : "no return type is declared, so it returns nothing");
            diag_help(d, "remove the value, or declare a return type: `function %s(...): int`", g->fn->name);
            diag_end(d);
            gen_expr(g, s->u.expr);
        } else if (g->fn_ret == VT_INT && !s->u.expr) {
            Diag *d = diag_start(DIAG_ERROR, "E0303", s->loc, "missing return value in function `%s`", g->fn->name);
            diag_label(d, "expected an `int` value");
            diag_span(d, ret_type_loc(g->fn), "`%s` is declared to return `int`", g->fn->name);
            diag_end(d);
        } else if (s->u.expr) {
            gen_int(g, s->u.expr);
        }
        emit(g, "jmp .L%d", g->ret_label);
        break;

    case STMT_IF: {
        int else_label = new_label(g), end_label = new_label(g);
        gen_cond_jump_false(g, s->u.if_.cond, else_label);
        gen_scoped(g, s->u.if_.then_stmt);
        if (s->u.if_.else_stmt) emit(g, "jmp .L%d", end_label);
        emit_label(g, else_label);
        if (s->u.if_.else_stmt) {
            gen_scoped(g, s->u.if_.else_stmt);
            emit_label(g, end_label);
        }
        break;
    }

    case STMT_WHILE: {
        int top = new_label(g), end = new_label(g);
        emit_label(g, top);
        gen_cond_jump_false(g, s->u.while_.cond, end);
        push_loop(g, end, top);
        gen_scoped(g, s->u.while_.body);
        g->nloops--;
        emit(g, "jmp .L%d", top);
        emit_label(g, end);
        break;
    }

    case STMT_DO_WHILE: {
        int top = new_label(g), cont = new_label(g), end = new_label(g);
        emit_label(g, top);
        push_loop(g, end, cont);
        gen_scoped(g, s->u.while_.body);
        g->nloops--;
        emit_label(g, cont);
        gen_int(g, s->u.while_.cond);
        emit(g, "test rax, rax");
        emit(g, "jne .L%d", top);
        emit_label(g, end);
        break;
    }

    case STMT_FOR: {
        int top = new_label(g), cont = new_label(g), end = new_label(g);
        push_scope(g);  /* the init variable lives for the whole loop */
        if (s->u.for_.init) gen_stmt(g, s->u.for_.init);
        emit_label(g, top);
        if (s->u.for_.cond) gen_cond_jump_false(g, s->u.for_.cond, end);
        push_loop(g, end, cont);
        gen_scoped(g, s->u.for_.body);
        g->nloops--;
        emit_label(g, cont);
        if (s->u.for_.step) gen_effect(g, s->u.for_.step);
        emit(g, "jmp .L%d", top);
        emit_label(g, end);
        pop_scope(g);
        break;
    }

    case STMT_BREAK:
    case STMT_CONTINUE: {
        bool is_break = s->kind == STMT_BREAK;
        if (g->nloops == 0) {
            Diag *d = diag_start(DIAG_ERROR, "E0212", s->loc, "`%s` outside of a loop", is_break ? "break" : "continue");
            diag_label(d, "can only be used inside a loop");
            if (is_break) diag_help(d, "to leave the function early, use `return`");
            diag_end(d);
            break;
        }
        const Loop *loop = &g->loops[g->nloops - 1];
        emit(g, "jmp .L%d", is_break ? loop->break_label : loop->continue_label);
        break;
    }

    case STMT_FOREACH:  not_yet(s->loc, "'foreach'", 12); break;
    case STMT_ECHO:     not_yet(s->loc, "'echo'", 7); break;
    case STMT_DEFER:    not_yet(s->loc, "'defer'", 9); break;
    case STMT_ASM:      not_yet(s->loc, "inline asm", 13); break;
    }
}

/* ---- return analysis ------------------------------------------------------ */

static bool is_const_true(Gen *g, const Expr *e) {
    int64_t v;
    return const_eval(g, e, &v, false) && v != 0;
}

/* Does `s` contain a break (or continue) aimed at the loop it's directly in?
 * Doesn't look inside nested loops, whose breaks are their own. */
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

/* Can control reach the end of `s`? Used to require a return at the end of
 * int functions. Loops with a constant-true condition and no break never
 * end, so `while (true) { ... }` needs no return after it. */
static bool falls_through(Gen *g, const Stmt *s) {
    switch (s->kind) {
    case STMT_RETURN:
    case STMT_BREAK:
    case STMT_CONTINUE:
        return false;
    case STMT_BLOCK:
        for (size_t i = 0; i < s->u.block.n; i++)
            if (!falls_through(g, s->u.block.stmts[i])) return false;
        return true;
    case STMT_IF:
        if (!s->u.if_.else_stmt) return true;
        return falls_through(g, s->u.if_.then_stmt) || falls_through(g, s->u.if_.else_stmt);
    case STMT_WHILE:
        return !is_const_true(g, s->u.while_.cond) || has_jump(s->u.while_.body, STMT_BREAK);
    case STMT_DO_WHILE:
        if (has_jump(s->u.while_.body, STMT_BREAK)) return true;
        if (is_const_true(g, s->u.while_.cond)) return false;
        return falls_through(g, s->u.while_.body) || has_jump(s->u.while_.body, STMT_CONTINUE);
    case STMT_FOR:
        if (s->u.for_.cond && !is_const_true(g, s->u.for_.cond)) return true;
        return has_jump(s->u.for_.body, STMT_BREAK);
    default:
        return true;
    }
}

/* ---- functions ------------------------------------------------------------- */

static void gen_function(Gen *g, const Decl *d) {
    if (d->is_extern || d->is_export) {
        not_yet(d->loc, d->is_extern ? "extern functions" : "export", d->is_extern ? 6 : 13);
        return;
    }
    if (d->u.func.variadic) {
        not_yet(d->name_loc, "variadic functions", 6);
        return;
    }

    g->fn = d;
    g->fn_errors_before = diag_error_count();
    g->fn_ret = ret_type_of(d);
    if (g->fn_ret == VT_ERROR) not_yet(d->u.func.ret->loc, "return types other than `int` and `void`", 5);
    g->ret_label = new_label(g);
    g->depth = 0;
    g->slots = 0;
    g->locals.len = 0;
    g->nscopes = 0;
    g->nloops = 0;

    /* The body goes to a buffer first: the prologue needs the frame size,
     * which is only known once every local has been seen. */
    char *body = NULL;
    size_t body_len = 0;
    FILE *real_out = g->out;
    g->out = open_memstream(&body, &body_len);
    if (!g->out) fatal("open_memstream failed");

    push_scope(g);
    for (size_t i = 0; i < d->u.func.nparams; i++) {
        const Param *p = d->u.func.params[i];
        check_var_type(p->type);
        if (i < 6) {
            Var *v = declare_local(g, p->name, p->loc, p->type, true);
            store_var(g, v, arg_regs[i]);
        } else {
            /* Stack arguments already live in the caller's frame. */
            declare_local_at(g, p->name, p->loc, p->type, true, 16 + 8 * (int)(i - 6));
        }
    }
    /* Body statements share the parameters' scope, so a local can't
     * redeclare a parameter. */
    const Stmt *b = d->u.func.body;
    gen_stmt_list(g, b->u.block.stmts, b->u.block.n);
    pop_scope(g);

    if (g->fn_ret == VT_INT && falls_through(g, b)) {
        Loc closing = loc_at(b->loc.file, b->loc.offset + b->loc.len - 1, 1);
        Diag *e = diag_start(DIAG_ERROR, "E0304", closing, "function `%s` can reach its end without returning a value",
                             d->name);
        diag_label(e, "the function can end here without returning");
        diag_span(e, ret_type_loc(d), "`%s` is declared to return `int`", d->name);
        diag_help(e, "add a `return` before the closing brace, or make sure every path returns");
        diag_end(e);
    }

    fclose(g->out);
    g->out = real_out;

    int frame = (g->slots * 8 + 15) & ~15;
    fprintf(g->out, "\n    .globl w.%s\n    .type w.%s, @function\nw.%s:\n", d->name, d->name, d->name);
    emit(g, "push rbp");
    emit(g, "mov rbp, rsp");
    if (frame) emit(g, "sub rsp, %d", frame);
    fwrite(body, 1, body_len, g->out);
    free(body);
    emit_label(g, g->ret_label);
    emit(g, "leave");
    emit(g, "ret");
    fprintf(g->out, "    .size w.%s, .-w.%s\n", d->name, d->name);
}

/* ---- module ---------------------------------------------------------------- */

/* Registers globals (evaluating their constant initializers) and checks
 * top-level names. Returns the main function, or NULL. */
static const Decl *check_top_level(Gen *g) {
    const Module *m = g->mod;
    const Decl *main_fn = NULL;

    for (size_t i = 0; i < m->n; i++) {
        const Decl *d = m->decls[i];
        if (d->kind != DECL_FUNC && d->kind != DECL_VAR) {
            switch (d->kind) {
            case DECL_IMPORT: not_yet(d->loc, "imports", 8); break;
            case DECL_STRUCT: not_yet(d->loc, "structs", 5); break;
            case DECL_CLASS:  not_yet(d->loc, "classes", 10); break;
            case DECL_ENUM:   not_yet(d->loc, "enums", 12); break;
            default: break;
            }
            continue;
        }

        for (size_t j = 0; j < i; j++) {
            const Decl *prev = m->decls[j];
            if ((prev->kind == DECL_FUNC || prev->kind == DECL_VAR) && strcmp(prev->name, d->name) == 0) {
                Diag *e = diag_start(DIAG_ERROR, "E0202", d->name_loc, "the name `%s` is defined more than once", d->name);
                diag_label(e, "`%s` redefined here", d->name);
                diag_span(e, prev->name_loc, "first definition of `%s` here", d->name);
                if (prev->kind != d->kind) diag_note(e, "functions and global variables share one namespace");
                diag_end(e);
                break;
            }
        }

        if (d->kind == DECL_FUNC) {
            if (strcmp(d->name, "main") == 0 && !main_fn) main_fn = d;
            continue;
        }

        const VarDecl *vd = d->u.var;
        if (d->is_extern || d->is_export) {
            not_yet(d->loc, d->is_extern ? "extern variables" : "export", d->is_extern ? 6 : 13);
            continue;
        }
        if (!check_var_type(vd->type)) continue;
        Var *v = ARENA_NEW(&g->arena, Var);
        v->name = vd->name;
        v->loc = vd->loc;
        v->type = vd->type;
        v->is_global = true;
        v->is_const = vd->type->is_const;
        if (v->is_const && !vd->init) report_missing_const_value(vd);
        bool known = vd->init && const_eval(g, vd->init, &v->value, true);
        if (!known) v->value = 0;
        /* Only constants fold into later initializers; mutable globals may change. */
        v->has_value = known && v->is_const;
        ptrvec_push(&g->globals, v);
    }

    if (!main_fn) {
        Loc loc = {m->file, 0, 0, 0, 0};
        Diag *d = diag_start(DIAG_ERROR, "E0305", loc, "`main` function not found in `%s`", m->file->path);
        diag_note(d, "a program starts running at `main`");
        diag_help(d, "add one: `function main(): int { return 0; }`");
        diag_end(d);
        return NULL;
    }
    if (!is_int_type(main_fn->u.func.ret) || main_fn->u.func.ret->is_const) {
        Diag *d = diag_start(DIAG_ERROR, "E0306", ret_type_loc(main_fn), "`main` must return `int`");
        diag_label(d, main_fn->u.func.ret ? "expected `int` here" : "no return type");
        diag_note(d, "main's return value becomes the program's exit code");
        if (main_fn->u.func.ret) diag_fix(d, main_fn->u.func.ret->loc, "int", "change the return type");
        diag_end(d);
    }
    if (main_fn->u.func.nparams > 0)
        not_yet(main_fn->u.func.params[0]->loc, "command-line arguments to main", 7);
    return main_fn;
}

static void emit_globals(Gen *g) {
    bool any_data = false, any_rodata = false;
    for (size_t i = 0; i < g->globals.len; i++) {
        const Var *v = g->globals.items[i];
        if (v->is_const) any_rodata = true;
        else any_data = true;
    }
    for (int pass = 0; pass < 2; pass++) {
        bool want_const = pass == 1;
        if (want_const ? !any_rodata : !any_data) continue;
        fprintf(g->out, "\n    .section %s\n    .p2align 3\n", want_const ? ".rodata" : ".data");
        for (size_t i = 0; i < g->globals.len; i++) {
            const Var *v = g->globals.items[i];
            if (v->is_const != want_const) continue;
            fprintf(g->out, "w.%s:\n    .quad %lld\n", v->name, (long long)v->value);
        }
    }
}

bool codegen_x64(const Module *m, FILE *out) {
    int errors_before = diag_error_count();
    Gen g = {0};
    g.out = out;
    g.mod = m;

    check_top_level(&g);

    fprintf(out, "# generated by watrc from %s\n", m->file->path);
    fputs("    .intel_syntax noprefix\n"
          "    .text\n"
          "\n"
          "# Process entry: call main, exit with its return value.\n"
          "    .globl _start\n"
          "_start:\n"
          "    xor ebp, ebp\n"
          "    call w.main\n"
          "    mov edi, eax\n"
          "    mov eax, 60\n"
          "    syscall\n",
          out);

    for (size_t i = 0; i < m->n; i++)
        if (m->decls[i]->kind == DECL_FUNC) gen_function(&g, m->decls[i]);

    emit_globals(&g);

    /* Tell the linker this code doesn't need an executable stack. */
    fputs("\n    .section .note.GNU-stack,\"\",@progbits\n", out);

    free(g.globals.items);
    free(g.locals.items);
    free(g.scope_marks);
    free(g.loops);
    arena_free(&g.arena);
    return diag_error_count() == errors_before;
}
