#include "codegen.h"

#include <stdarg.h>
#include <string.h>

/*
 * x86-64 code generator, stack-machine style: every expression leaves its
 * value in rax, and binary operators park the left operand on the stack
 * while the right one is computed. The output is slow but simple to get
 * right; a register allocator comes later (milestone 15).
 *
 * Until the type checker exists (milestone 5) this file also does the
 * checking for the subset it supports: int expressions, calls to functions
 * with no parameters, and return. Everything else is reported as "not
 * supported yet" with the milestone that adds it.
 *
 * Symbols: user functions are emitted as `w.<name>`. That keeps them from
 * clashing with register names (a function called `rax` would otherwise
 * assemble as `call rax`) and with C symbols once we link against libc.
 */

typedef enum {
    VT_ERROR,   /* already reported */
    VT_VOID,
    VT_INT,
} ValType;

typedef struct {
    FILE *out;
    const Module *mod;
    const Decl *fn;     /* function being generated */
    ValType fn_ret;
    int ret_label;
    int depth;          /* 8-byte pushes since the frame was set up */
    int labels;
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

static ValType resolve_ret_type(const TypeExpr *t) {
    if (!t) return VT_VOID;
    if (t->kind == TYPEX_BUILTIN && !t->is_const && !t->is_volatile) {
        if (t->u.builtin == TOK_KW_INT_T) return VT_INT;
        if (t->u.builtin == TOK_KW_VOID) return VT_VOID;
    }
    not_yet(t->loc, "return types other than int and void", 5);
    return VT_ERROR;
}

static const Decl *find_function(const Module *m, const char *name) {
    for (size_t i = 0; i < m->n; i++) {
        const Decl *d = m->decls[i];
        if (d->kind == DECL_FUNC && strcmp(d->name, name) == 0) return d;
    }
    return NULL;
}

/* ---- expressions --------------------------------------------------------- */

static ValType gen_expr(Gen *g, const Expr *e);

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

static const char *setcc_for(TokenKind op) {
    switch (op) {
    case TOK_EQ: return "sete";
    case TOK_NE: return "setne";
    case TOK_LT: return "setl";
    case TOK_GT: return "setg";
    case TOK_LE: return "setle";
    case TOK_GE: return "setge";
    default:     return NULL;
    }
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

    switch (op) {
    case TOK_PLUS:    emit(g, "add rax, rcx"); break;
    case TOK_MINUS:   emit(g, "sub rax, rcx"); break;
    case TOK_STAR:    emit(g, "imul rax, rcx"); break;
    case TOK_SLASH:   emit(g, "cqo"); emit(g, "idiv rcx"); break;
    case TOK_PERCENT: emit(g, "cqo"); emit(g, "idiv rcx"); emit(g, "mov rax, rdx"); break;
    case TOK_AMP:     emit(g, "and rax, rcx"); break;
    case TOK_PIPE:    emit(g, "or rax, rcx"); break;
    case TOK_CARET:   emit(g, "xor rax, rcx"); break;
    case TOK_SHL:     emit(g, "shl rax, cl"); break;
    case TOK_SHR:     emit(g, "sar rax, cl"); break;  /* int is signed: arithmetic shift */
    default: {
        const char *setcc = setcc_for(op);
        if (!setcc) {
            diag_error(e->loc, "internal error: unexpected binary operator '%s'", token_kind_spelling(op));
            return VT_ERROR;
        }
        emit(g, "cmp rax, rcx");
        emit(g, "%s al", setcc);
        emit(g, "movzx eax, al");
    }
    }
    return ok ? VT_INT : VT_ERROR;
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
    case TOK_AMP: case TOK_STAR:
        not_yet(e->loc, op == TOK_AMP ? "taking addresses with '&'" : "dereferencing with '*'", 5);
        return VT_ERROR;
    default:
        not_yet(e->loc, "'++' and '--'", 4);
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

static ValType gen_call(Gen *g, const Expr *e) {
    const Expr *callee = e->u.call.callee;
    if (callee->kind != EXPR_IDENT) {
        not_yet(callee->loc, "calling anything but a named function", 10);
        return VT_ERROR;
    }
    const Decl *fn = find_function(g->mod, callee->u.name);
    if (!fn) {
        diag_error(callee->loc, "there is no function named '%s'", callee->u.name);
        return VT_ERROR;
    }
    if (e->u.call.nargs > 0) {
        not_yet(e->u.call.args[0]->loc, "passing arguments to functions", 4);
        return VT_ERROR;
    }

    /* The ABI wants rsp 16-byte aligned at the call. The frame starts
     * aligned, so an odd number of pending pushes needs 8 bytes of padding. */
    bool pad = g->depth % 2 != 0;
    if (pad) emit(g, "sub rsp, 8");
    emit(g, "call w.%s", fn->name);
    if (pad) emit(g, "add rsp, 8");

    /* Report a bad return type once, at the definition, not at every call. */
    if (!fn->u.func.ret) return VT_VOID;
    const TypeExpr *t = fn->u.func.ret;
    if (t->kind == TYPEX_BUILTIN && t->u.builtin == TOK_KW_INT_T && !t->is_const && !t->is_volatile) return VT_INT;
    if (t->kind == TYPEX_BUILTIN && t->u.builtin == TOK_KW_VOID) return VT_VOID;
    return VT_ERROR;
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
    case EXPR_UNARY:
        return gen_unary(g, e);
    case EXPR_BINARY:
        return gen_binary(g, e);
    case EXPR_TERNARY:
        return gen_ternary(g, e);
    case EXPR_CALL:
        return gen_call(g, e);
    case EXPR_IDENT:
        if (find_function(g->mod, e->u.name))
            diag_error(e->loc, "'%s' is a function; call it with %s()", e->u.name, e->u.name);
        else
            not_yet(e->loc, "variables", 4);
        return VT_ERROR;

    case EXPR_FLOAT:      not_yet(e->loc, "floats", 5); break;
    case EXPR_STRING:
    case EXPR_INTERP:     not_yet(e->loc, "strings", 7); break;
    case EXPR_BOOL:       not_yet(e->loc, "true and false", 5); break;
    case EXPR_NULL:       not_yet(e->loc, "null", 5); break;
    case EXPR_THIS:       not_yet(e->loc, "classes", 10); break;
    case EXPR_SCOPE:      not_yet(e->loc, "enums", 12); break;
    case EXPR_POSTFIX:    not_yet(e->loc, "'++' and '--'", 4); break;
    case EXPR_ASSIGN:     not_yet(e->loc, "assignment", 4); break;
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

/* ---- statements ---------------------------------------------------------- */

static void gen_stmt(Gen *g, const Stmt *s) {
    switch (s->kind) {
    case STMT_BLOCK:
        for (size_t i = 0; i < s->u.block.n; i++) gen_stmt(g, s->u.block.stmts[i]);
        break;
    case STMT_EXPR:
        gen_expr(g, s->u.expr);
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

    case STMT_VAR:      not_yet(s->loc, "local variables", 4); break;
    case STMT_IF:       not_yet(s->loc, "'if'", 4); break;
    case STMT_WHILE:
    case STMT_DO_WHILE: not_yet(s->loc, "'while' loops", 4); break;
    case STMT_FOR:      not_yet(s->loc, "'for' loops", 4); break;
    case STMT_FOREACH:  not_yet(s->loc, "'foreach'", 12); break;
    case STMT_BREAK:
    case STMT_CONTINUE: not_yet(s->loc, "'break' and 'continue'", 4); break;
    case STMT_ECHO:     not_yet(s->loc, "'echo'", 7); break;
    case STMT_DEFER:    not_yet(s->loc, "'defer'", 9); break;
    case STMT_ASM:      not_yet(s->loc, "inline asm", 13); break;
    }
}

/* Until there is control flow, "ends with a return" is exactly
 * "can't fall off the end". Milestone 4 replaces this with real analysis. */
static bool ends_with_return(const Stmt *s) {
    if (s->kind == STMT_RETURN) return true;
    if (s->kind == STMT_BLOCK && s->u.block.n > 0) return ends_with_return(s->u.block.stmts[s->u.block.n - 1]);
    return false;
}

static void gen_function(Gen *g, const Decl *d) {
    if (d->is_extern || d->is_export) {
        not_yet(d->loc, d->is_extern ? "extern functions" : "export", d->is_extern ? 6 : 13);
        return;
    }
    if (d->u.func.nparams > 0 || d->u.func.variadic) {
        Loc loc = d->u.func.nparams ? d->u.func.params[0]->loc : d->name_loc;
        not_yet(loc, "function parameters", 4);
        return;
    }

    g->fn = d;
    g->fn_ret = resolve_ret_type(d->u.func.ret);
    g->ret_label = new_label(g);
    g->depth = 0;

    fprintf(g->out, "\n    .globl w.%s\n    .type w.%s, @function\nw.%s:\n", d->name, d->name, d->name);
    emit(g, "push rbp");
    emit(g, "mov rbp, rsp");
    gen_stmt(g, d->u.func.body);
    if (g->fn_ret == VT_INT && !ends_with_return(d->u.func.body))
        diag_error(d->name_loc, "function '%s' can reach its end without returning a value", d->name);
    emit_label(g, g->ret_label);
    emit(g, "pop rbp");
    emit(g, "ret");
    fprintf(g->out, "    .size w.%s, .-w.%s\n", d->name, d->name);
}

/* ---- module ---------------------------------------------------------------- */

static bool check_top_level(const Module *m) {
    bool ok = true;
    const Decl *main_fn = NULL;

    for (size_t i = 0; i < m->n; i++) {
        const Decl *d = m->decls[i];
        switch (d->kind) {
        case DECL_FUNC:
            for (size_t j = 0; j < i; j++) {
                const Decl *prev = m->decls[j];
                if (prev->kind == DECL_FUNC && strcmp(prev->name, d->name) == 0) {
                    diag_error(d->name_loc, "function '%s' is defined twice", d->name);
                    diag_note(prev->name_loc, "the first definition is here");
                    ok = false;
                    break;
                }
            }
            if (strcmp(d->name, "main") == 0 && !main_fn) main_fn = d;
            break;
        case DECL_IMPORT: not_yet(d->loc, "imports", 8); ok = false; break;
        case DECL_STRUCT: not_yet(d->loc, "structs", 5); ok = false; break;
        case DECL_CLASS:  not_yet(d->loc, "classes", 10); ok = false; break;
        case DECL_ENUM:   not_yet(d->loc, "enums", 12); ok = false; break;
        case DECL_VAR:    not_yet(d->loc, "global variables", 4); ok = false; break;
        }
    }

    if (!main_fn) {
        Loc loc = {m->file, 0, 1, 1, 0};
        diag_error(loc, "there is no main function; a program starts at: function main(): int { ... }");
        return false;
    }
    const TypeExpr *ret = main_fn->u.func.ret;
    bool returns_int = ret && ret->kind == TYPEX_BUILTIN && ret->u.builtin == TOK_KW_INT_T;
    if (!returns_int) {
        diag_error(main_fn->name_loc, "main must return int (its value is the program's exit code)");
        ok = false;
    }
    return ok;
}

bool codegen_x64(const Module *m, FILE *out) {
    int errors_before = diag_error_count();
    Gen g = {0};
    g.out = out;
    g.mod = m;

    check_top_level(m);

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

    /* Tell the linker this code doesn't need an executable stack. */
    fputs("\n    .section .note.GNU-stack,\"\",@progbits\n", out);
    return diag_error_count() == errors_before;
}
