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
    bool is_global;
    bool is_const;
    int offset;          /* locals and params: rbp-relative */
    bool has_value;      /* const globals: value known at compile time */
    int64_t value;
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
    diag_error(loc, "not supported yet: %s (planned for milestone %d)", what, milestone);
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

static void pop_scope(Gen *g) {
    g->locals.len = g->scope_marks[--g->nscopes];
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
static Var *declare_local(Gen *g, const char *name, Loc loc, bool is_const) {
    Var *prev = find_local(g, name);
    if (prev) {
        diag_error(loc, "'%s' is already declared in this function", name);
        diag_note(prev->loc, "the earlier declaration is here");
    }
    Var *v = ARENA_NEW(&g->arena, Var);
    v->name = name;
    v->loc = loc;
    v->is_const = is_const;
    v->offset = -8 * (++g->slots);
    ptrvec_push(&g->locals, v);
    return v;
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
            if (report) diag_error(e->loc, "%llu doesn't fit in int", (unsigned long long)e->u.int_val);
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
        if (report) diag_error(e->loc, "'%s' isn't a constant; global initializers must be constant", e->u.name);
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
                if (report) diag_error(e->loc, b == 0 ? "division by zero" : "this division overflows int");
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
    if (report) diag_error(e->loc, "global initializers must be constant expressions");
    return false;
}

/* ---- expressions --------------------------------------------------------- */

static ValType gen_expr(Gen *g, const Expr *e);
static void gen_effect(Gen *g, const Expr *e);

/* Generates `e` and requires it to produce an int. */
static bool gen_int(Gen *g, const Expr *e) {
    ValType t = gen_expr(g, e);
    if (t == VT_VOID) {
        diag_error(e->loc, "this expression has no value (it calls a void function)");
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
        not_yet(e->loc, "string concatenation with '.'", 7);
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
            (target->kind == EXPR_UNARY && target->u.unary.op == TOK_STAR))
            not_yet(target->loc, "assigning through pointers, fields or indexes", 5);
        else
            diag_error(target->loc, "can't %s this; only variables can be assigned", action);
        return NULL;
    }
    Var *v = find_var(g, target->u.name);
    if (!v) {
        if (find_function(g->mod, target->u.name))
            diag_error(target->loc, "'%s' is a function, not a variable", target->u.name);
        else
            diag_error(target->loc, "there is no variable named '%s'", target->u.name);
        return NULL;
    }
    if (v->is_const) {
        diag_error(target->loc, "can't %s '%s' because it's a constant", action, v->name);
        diag_note(v->loc, "'%s' is declared const here", v->name);
        return NULL;
    }
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
        not_yet(e->loc, op == TOK_AMP ? "taking addresses with '&'" : "dereferencing with '*'", 5);
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
        if (find_var(g, callee->u.name))
            diag_error(callee->loc, "'%s' is a variable, not a function", callee->u.name);
        else
            diag_error(callee->loc, "there is no function named '%s'", callee->u.name);
        return VT_ERROR;
    }

    size_t n = e->u.call.nargs;
    bool ok = true;
    if (n != fn->u.func.nparams) {
        diag_error(e->loc, "'%s' takes %zu argument%s, but %zu %s given", fn->name, fn->u.func.nparams,
                   fn->u.func.nparams == 1 ? "" : "s", n, n == 1 ? "was" : "were");
        diag_note(fn->name_loc, "'%s' is defined here", fn->name);
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
            diag_error(e->loc, "%llu doesn't fit in int (the largest int is 9223372036854775807)",
                       (unsigned long long)e->u.int_val);
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
            load_var(g, v);
            return VT_INT;
        }
        if (find_function(g->mod, e->u.name))
            diag_error(e->loc, "'%s' is a function; call it with %s(...)", e->u.name, e->u.name);
        else
            diag_error(e->loc, "there is no variable named '%s'", e->u.name);
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
        diag_error(e->loc, "an assignment can't be used as a value%s",
                   e->u.binary.op == TOK_ASSIGN ? " (did you mean '=='?)" : "");
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
        not_yet(e->loc, "string concatenation with '.='", 7);
        return;
    }
    if (e->u.binary.rhs->kind == EXPR_ASSIGN) {
        diag_error(e->u.binary.rhs->loc, "assignments can't be chained; assign each variable separately");
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

static void gen_scoped(Gen *g, const Stmt *s) {
    push_scope(g);
    if (s->kind == STMT_BLOCK) {
        for (size_t i = 0; i < s->u.block.n; i++) gen_stmt(g, s->u.block.stmts[i]);
    } else {
        gen_stmt(g, s);
    }
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

static void gen_var_decl(Gen *g, const VarDecl *vd) {
    bool type_ok = check_var_type(vd->type);
    bool is_const = vd->type->is_const;
    if (is_const && !vd->init) diag_error(vd->loc, "constant '%s' needs a value", vd->name);

    /* The initializer is generated before the name exists, so `int x = x;`
     * can't read the variable it's declaring. */
    if (vd->init) {
        if (vd->init->kind == EXPR_ASSIGN) {
            diag_error(vd->init->loc, "an assignment can't be used as a value");
            type_ok = false;
        } else if (!gen_int(g, vd->init)) {
            type_ok = false;
        }
    } else {
        emit(g, "xor eax, eax");  /* locals start at zero */
    }
    Var *v = declare_local(g, vd->name, vd->loc, is_const);
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
            diag_error(s->u.expr->loc, "function '%s' returns void, so 'return' can't have a value", g->fn->name);
        } else if (g->fn_ret == VT_INT && !s->u.expr) {
            diag_error(s->loc, "function '%s' must return an int value", g->fn->name);
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
            diag_error(s->loc, "'%s' can only be used inside a loop", is_break ? "break" : "continue");
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
    g->fn_ret = ret_type_of(d);
    if (g->fn_ret == VT_ERROR) not_yet(d->u.func.ret->loc, "return types other than int and void", 5);
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
            Var *v = declare_local(g, p->name, p->loc, p->type->is_const);
            store_var(g, v, arg_regs[i]);
        } else {
            Var *prev = find_local(g, p->name);
            if (prev) {
                diag_error(p->loc, "'%s' is already declared in this function", p->name);
                diag_note(prev->loc, "the earlier declaration is here");
            }
            Var *v = ARENA_NEW(&g->arena, Var);
            v->name = p->name;
            v->loc = p->loc;
            v->is_const = p->type->is_const;
            v->offset = 16 + 8 * (int)(i - 6);
            ptrvec_push(&g->locals, v);
        }
    }
    /* Body statements share the parameters' scope, so a local can't
     * redeclare a parameter. */
    const Stmt *b = d->u.func.body;
    for (size_t i = 0; i < b->u.block.n; i++) gen_stmt(g, b->u.block.stmts[i]);
    pop_scope(g);

    if (g->fn_ret == VT_INT && falls_through(g, b))
        diag_error(d->name_loc, "function '%s' can reach its end without returning a value", d->name);

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
                diag_error(d->name_loc, "'%s' is defined twice", d->name);
                diag_note(prev->name_loc, "the first definition is here");
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
        v->is_global = true;
        v->is_const = vd->type->is_const;
        if (v->is_const && !vd->init) diag_error(vd->loc, "constant '%s' needs a value", vd->name);
        bool known = vd->init && const_eval(g, vd->init, &v->value, true);
        if (!known) v->value = 0;
        /* Only constants fold into later initializers; mutable globals may change. */
        v->has_value = known && v->is_const;
        ptrvec_push(&g->globals, v);
    }

    if (!main_fn) {
        Loc loc = {m->file, 0, 1, 1, 0};
        diag_error(loc, "there is no main function; a program starts at: function main(): int { ... }");
        return NULL;
    }
    if (!is_int_type(main_fn->u.func.ret) || main_fn->u.func.ret->is_const)
        diag_error(main_fn->name_loc, "main must return int (its value is the program's exit code)");
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
