#include "codegen.h"

#include <stdarg.h>
#include <string.h>

#include "sema.h"
#include "vec.h"

/*
 * x86-64 code generator, stack-machine style. It works from the tree that
 * sema checked and annotated, so it reports no errors of its own.
 *
 * Where values live while an expression is computed:
 *   ints, bools, pointers   rax, always sign/zero-extended to 64 bits for
 *                           their type, so widening an int costs nothing
 *   f32, f64                xmm0 (f32 stays single precision)
 *   structs, arrays         rax holds their address
 * Binary operators park the left operand on the stack while the right one
 * is computed.
 *
 * Frame layout (System V ABI):
 *   [rbp + 16 + 8*k]  stack argument k (parameters that didn't fit in registers)
 *   [rbp + 8]         return address
 *   [rbp]             caller's rbp
 *   [rbp - n]         locals, spilled register parameters, temporaries
 * The frame is a multiple of 16 bytes, so rsp is aligned at calls.
 *
 * Symbols: functions and globals are emitted as `w.<name>`, so a function
 * called `rax` can't assemble as `call rax`, and names stay apart from C's.
 */

typedef struct {
    const char *q, *d, *w, *b;  /* 64/32/16/8-bit names */
} Reg;

static const Reg R_RAX = {"rax", "eax", "ax", "al"};
static const Reg R_RDI = {"rdi", "edi", "di", "dil"};
static const Reg R_RSI = {"rsi", "esi", "si", "sil"};
static const Reg R_RDX = {"rdx", "edx", "dx", "dl"};
static const Reg R_RCX = {"rcx", "ecx", "cx", "cl"};
static const Reg R_R8 = {"r8", "r8d", "r8w", "r8b"};
static const Reg R_R9 = {"r9", "r9d", "r9w", "r9b"};
static const Reg *const arg_regs[6] = {&R_RDI, &R_RSI, &R_RDX, &R_RCX, &R_R8, &R_R9};

typedef struct {
    int break_label;
    int continue_label;
    size_t scopes;          /* defer scopes open outside the loop */
} Loop;

typedef struct {
    int label;
    char *msg;
} PanicSite;

typedef struct {
    int label;
    StrBuf bytes;
} StringLit;

typedef struct {
    FILE *out;
    int labels;

    /* per function */
    int ret_label;
    int depth;              /* 8-byte pushes since the frame was set up */
    int frame;              /* bytes of locals/temporaries */
    Loop *loops;
    size_t nloops, loops_cap;
    const Symbol *fn;
    int hidden_slot;        /* where the caller's return buffer pointer is kept (struct returns) */
    int ret_slot;           /* where `return` leaves the value for the epilogue */
    PtrVec defers;          /* Stmt*: deferred statements of the open blocks, innermost last */
    size_t *scopes;         /* per open block: where its defers start in `defers` */
    size_t nscopes, scopes_cap;

    bool libc;              /* linked with the C runtime */
    bool need_panic;        /* emit w.panic even without check sites */
    PtrVec panics;          /* PanicSite*: runtime check failure messages */
    PtrVec strings;         /* StringLit*: literals for .rodata */
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

/* ---- value classes ----------------------------------------------------------- */

typedef enum { CLS_GPR, CLS_SSE, CLS_MEM, CLS_NONE } ValClass;

static ValClass cls(const Type *t) {
    switch (t->kind) {
    case TY_FLOAT: return CLS_SSE;
    case TY_ARRAY:
    case TY_STRUCT:
    case TY_STRING:
    case TY_ERRUNION: return CLS_MEM;
    case TY_VOID: return CLS_NONE;
    default: return CLS_GPR;
    }
}

/* System V classification of a value passed in registers: up to two
 * eightbytes, each INTEGER or SSE; larger structs go in MEMORY. */
typedef enum { EB_NONE, EB_INT, EB_SSE } Eightbyte;

typedef struct {
    bool memory;
    int n;              /* eightbytes */
    Eightbyte eb[2];
} Classification;

static void classify_into(const Type *t, uint32_t off, Eightbyte eb[2]) {
    switch (t->kind) {
    case TY_STRUCT:
    case TY_ERRUNION:
        for (size_t i = 0; i < t->nfields; i++) classify_into(t->fields[i].type, off + t->fields[i].offset, eb);
        break;
    case TY_ARRAY:
        for (uint64_t i = 0; i < t->len; i++) classify_into(t->base, off + (uint32_t)(i * t->base->size), eb);
        break;
    case TY_FLOAT:
        if (eb[off / 8] == EB_NONE) eb[off / 8] = EB_SSE;   /* INTEGER wins if both appear */
        break;
    case TY_STRING:   /* {ptr, len} */
        eb[off / 8] = EB_INT;
        eb[off / 8 + 1] = EB_INT;
        break;
    default:
        eb[off / 8] = EB_INT;
        break;
    }
}

static Classification classify(const Type *t) {
    Classification c = {false, 0, {EB_NONE, EB_NONE}};
    if (t->kind == TY_VOID) return c;
    if (type_in_memory(t)) {
        if (t->size > 16) {
            c.memory = true;
            return c;
        }
        c.n = (int)((t->size + 7) / 8);
        classify_into(t, 0, c.eb);
        for (int i = 0; i < c.n; i++)
            if (c.eb[i] == EB_NONE) c.eb[i] = EB_SSE;   /* padding-only eightbyte */
        return c;
    }
    c.n = 1;
    c.eb[0] = t->kind == TY_FLOAT ? EB_SSE : EB_INT;
    return c;
}

static const char *size_ptr(uint32_t size) {
    switch (size) {
    case 1: return "byte";
    case 2: return "word";
    case 4: return "dword";
    default: return "qword";
    }
}

static const char *reg_of_size(const Reg *r, uint32_t size) {
    switch (size) {
    case 1: return r->b;
    case 2: return r->w;
    case 4: return r->d;
    default: return r->q;
    }
}

static void push_value(Gen *g, const Type *t) {
    if (cls(t) == CLS_SSE) {
        emit(g, "sub rsp, 8");
        emit(g, "movsd qword ptr [rsp], xmm0");
    } else {
        emit(g, "push rax");
    }
    g->depth++;
}

/* Pops into xmm<n> for floats, or the named register otherwise. */
static void pop_value(Gen *g, const Type *t, const char *reg, int xmm) {
    if (cls(t) == CLS_SSE) {
        emit(g, "movsd xmm%d, qword ptr [rsp]", xmm);
        emit(g, "add rsp, 8");
    } else {
        emit(g, "pop %s", reg);
    }
    g->depth--;
}

/* Re-establishes the invariant that rax holds `t` sign/zero-extended. */
static void normalize(Gen *g, const Type *t) {
    if (!type_is_intlike(t)) return;
    switch (t->size) {
    case 1: emit(g, t->is_signed ? "movsx rax, al" : "movzx eax, al"); break;
    case 2: emit(g, t->is_signed ? "movsx rax, ax" : "movzx eax, ax"); break;
    case 4: emit(g, t->is_signed ? "movsxd rax, eax" : "mov eax, eax"); break;
    default: break;
    }
}

/* Loads a value of type `t` from [addr] into rax/xmm0 (for aggregates:
 * puts the address in rax). */
static void emit_load(Gen *g, const Type *t, const char *addr) {
    switch (cls(t)) {
    case CLS_SSE:
        emit(g, "%s xmm0, %s ptr [%s]", t->size == 4 ? "movss" : "movsd", size_ptr(t->size), addr);
        return;
    case CLS_MEM:
        emit(g, "lea rax, [%s]", addr);
        return;
    case CLS_NONE:
        return;
    case CLS_GPR:
        break;
    }
    bool sgn = type_is_intlike(t) && t->is_signed;
    switch (t->size) {
    case 1: emit(g, "%s %s, byte ptr [%s]", sgn ? "movsx" : "movzx", sgn ? "rax" : "eax", addr); break;
    case 2: emit(g, "%s %s, word ptr [%s]", sgn ? "movsx" : "movzx", sgn ? "rax" : "eax", addr); break;
    case 4:
        if (sgn) emit(g, "movsxd rax, dword ptr [%s]", addr);
        else emit(g, "mov eax, dword ptr [%s]", addr);
        break;
    default: emit(g, "mov rax, qword ptr [%s]", addr); break;
    }
}

/* Stores rax/xmm0 to [addr]; for aggregates, copies from the address in
 * rax. `addr` must not be based on rsi or rdi. */
static void emit_store(Gen *g, const Type *t, const char *addr) {
    switch (cls(t)) {
    case CLS_SSE:
        emit(g, "%s %s ptr [%s], xmm0", t->size == 4 ? "movss" : "movsd", size_ptr(t->size), addr);
        return;
    case CLS_MEM:
        emit(g, "lea rdi, [%s]", addr);
        emit(g, "mov rsi, rax");
        emit(g, "mov ecx, %u", t->size);
        emit(g, "rep movsb");
        return;
    case CLS_NONE:
        return;
    case CLS_GPR:
        emit(g, "mov %s ptr [%s], %s", size_ptr(t->size), addr, reg_of_size(&R_RAX, t->size));
        return;
    }
}

/* Where a variable lives, as an addressing expression. */
static const char *var_addr(char *buf, size_t cap, const Symbol *s) {
    if (s->kind == SYM_GLOBAL) snprintf(buf, cap, "rip + %s", s->asm_name);
    else snprintf(buf, cap, "rbp %c %d", s->offset < 0 ? '-' : '+', s->offset < 0 ? -s->offset : s->offset);
    return buf;
}

static int alloc_slot(Gen *g, uint32_t size, uint32_t align) {
    if (size < 8) size = 8;
    if (align < 8) align = 8;
    g->frame = (int)(((uint32_t)g->frame + size + align - 1) & ~(align - 1));
    return -g->frame;
}

static void zero_memory(Gen *g, int offset, uint32_t size) {
    if (size <= 8) {
        emit(g, "mov qword ptr [rbp - %d], 0", -offset);
        return;
    }
    emit(g, "lea rdi, [rbp - %d]", -offset);
    emit(g, "xor eax, eax");
    emit(g, "mov ecx, %u", size);
    emit(g, "rep stosb");
}

static void emit_float_const(Gen *g, const Type *t, double v) {
    if (t->size == 4) {
        float f = (float)v;
        uint32_t bits;
        memcpy(&bits, &f, 4);
        emit(g, "mov eax, %u", bits);
        emit(g, "movd xmm0, eax");
    } else {
        uint64_t bits;
        memcpy(&bits, &v, 8);
        emit(g, "movabs rax, %llu", (unsigned long long)bits);
        emit(g, "movq xmm0, rax");
    }
}

static void emit_int_const(Gen *g, uint64_t bits) {
    int64_t v = (int64_t)bits;
    if (v == 0) emit(g, "xor eax, eax");
    else if (v >= INT32_MIN && v <= INT32_MAX) emit(g, "mov rax, %lld", (long long)v);
    else emit(g, "movabs rax, %lld", (long long)v);
}

static int string_label(Gen *g, StrBuf bytes) {
    StringLit *lit = xmalloc(sizeof *lit);
    lit->label = new_label(g);
    lit->bytes = bytes;
    ptrvec_push(&g->strings, lit);
    return lit->label;
}

/* Calls a C function whose arguments are already in registers, keeping
 * rsp 16-byte aligned. */
static void emit_c_call(Gen *g, const char *name) {
    bool pad = g->depth % 2 != 0;
    if (pad) emit(g, "sub rsp, 8");
    emit(g, "call %s", name);
    if (pad) emit(g, "add rsp, 8");
}

static int add_panic(Gen *g, const Expr *at, const char *what) {
    PanicSite *site = xmalloc(sizeof *site);
    site->label = new_label(g);
    char msg[512];
    snprintf(msg, sizeof msg, "panic at %s:%u:%u: %s\n", at->loc.file->path, at->loc.line, at->loc.col, what);
    site->msg = xmalloc(strlen(msg) + 1);
    strcpy(site->msg, msg);
    ptrvec_push(&g->panics, site);
    return site->label;
}

static void emit_panic_call(Gen *g, int label) {
    PanicSite *site = NULL;
    for (size_t i = 0; i < g->panics.len; i++)
        if (((PanicSite *)g->panics.items[i])->label == label) site = g->panics.items[i];
    emit(g, "lea rdi, [rip + .Lpanic%d]", label);
    emit(g, "mov esi, %zu", strlen(site->msg));
    emit(g, "call w.panic");
}

/* ---- expressions --------------------------------------------------------- */

static void gen_value(Gen *g, const Expr *e);
static void gen_body(Gen *g, const Stmt *s);

/* A field of an error union type ("is_err", "err", "value"). */
static const Field *eu_field(const Type *t, const char *name) {
    for (size_t i = 0; i < t->nfields; i++)
        if (strcmp(t->fields[i].name, name) == 0) return &t->fields[i];
    fatal("codegen: error union without field %s", name);
}

/* Emits the deferred statements registered since `from`, newest first.
 * They stay registered: other exits from the same blocks run them too. */
static void emit_defers(Gen *g, size_t from) {
    for (size_t i = g->defers.len; i-- > from;) gen_body(g, g->defers.items[i]);
}

/* Leaves the function: the return value is already in the return slot. */
static void emit_return_jump(Gen *g) {
    emit_defers(g, 0);
    emit(g, "jmp .L%d", g->ret_label);
}

/* r->ok, r->value, r->error */
static void gen_eu_member(Gen *g, const Expr *e) {
    gen_value(g, e->u.member.object);   /* the error union's address, either way */
    if (e->builtin == BUILTIN_EU_OK) {
        emit(g, "movzx eax, byte ptr [rax]");
        emit(g, "xor eax, 1");
        return;
    }
    bool want_value = e->builtin == BUILTIN_EU_VALUE;
    int site = add_panic(g, e, want_value ? "`->value` of a result that holds an error"
                                          : "`->error` of a result that holds a value");
    int ok = new_label(g);
    emit(g, "cmp byte ptr [rax], 0");
    emit(g, "%s .L%d", want_value ? "je" : "jne", ok);
    emit_panic_call(g, site);
    emit_label(g, ok);
    char addr[64];
    snprintf(addr, sizeof addr, "rax + %u", e->field_offset);
    emit_load(g, e->type, addr);
}

static void gen_call(Gen *g, const Expr *e);

/* new Class(args) */
static void gen_new(Gen *g, const Expr *e) {
    const Type *ct = e->type;
    Symbol *tmp = e->u.new_.tmp;
    emit(g, "mov edi, %u", ct->obj_size ? ct->obj_size : 1);
    emit_c_call(g, e->u.new_.alloc->asm_name);   /* zeroed memory */
    tmp->offset = alloc_slot(g, 8, 8);
    emit(g, "mov qword ptr [rbp - %d], rax", -tmp->offset);
    char addr[64];
    for (size_t i = 0; i < ct->nfields; i++) {
        const Field *f = &ct->fields[i];
        if (!f->init) continue;
        gen_value(g, f->init);
        emit(g, "mov rdx, qword ptr [rbp - %d]", -tmp->offset);
        snprintf(addr, sizeof addr, "rdx + %u", f->offset);
        emit_store(g, f->type, addr);
    }
    if (e->u.new_.ctor) gen_call(g, e->u.new_.ctor);
    emit(g, "mov rax, qword ptr [rbp - %d]", -tmp->offset);
}

/* Panics if the object reference in rax is null. */
static void emit_null_check(Gen *g, const Expr *at, const char *what) {
    int site = add_panic(g, at, what);
    int ok = new_label(g);
    emit(g, "test rax, rax");
    emit(g, "jnz .L%d", ok);
    emit_panic_call(g, site);
    emit_label(g, ok);
}

/* r?: on an error, return it from this function (running defers);
 * otherwise produce the value. */
static void gen_try(Gen *g, const Expr *e) {
    const Expr *inner = e->u.try_expr;
    const Type *it = inner->type, *rt = g->fn->type;
    gen_value(g, inner);
    int ok = new_label(g);
    emit(g, "cmp byte ptr [rax], 0");
    emit(g, "je .L%d", ok);

    char addr[64];
    snprintf(addr, sizeof addr, "rax + %u", eu_field(it, "err")->offset);
    emit_load(g, it->err, addr);
    emit(g, "mov rcx, rax");
    /* Build the function's result in place: zeroed, is_err = 1, err. */
    bool memory = classify(rt).memory;
    if (memory) emit(g, "mov rdx, qword ptr [rbp - %d]", -g->hidden_slot);
    for (uint32_t k = 0; k < rt->size;) {
        uint32_t n = rt->size - k >= 8 ? 8 : rt->size - k >= 4 ? 4 : rt->size - k >= 2 ? 2 : 1;
        if (memory) emit(g, "mov %s ptr [rdx + %u], 0", size_ptr(n), k);
        else emit(g, "mov %s ptr [rbp - %d], 0", size_ptr(n), -g->ret_slot - (int)k);
        k += n;
    }
    uint32_t err_off = eu_field(rt, "err")->offset;
    uint32_t esize = rt->err->size;
    if (memory) {
        emit(g, "mov byte ptr [rdx], 1");
        emit(g, "mov %s ptr [rdx + %u], %s", size_ptr(esize), err_off, reg_of_size(&R_RCX, esize));
    } else {
        emit(g, "mov byte ptr [rbp - %d], 1", -g->ret_slot);
        emit(g, "mov %s ptr [rbp - %d], %s", size_ptr(esize), -g->ret_slot - (int)err_off, reg_of_size(&R_RCX, esize));
    }
    emit_return_jump(g);

    emit_label(g, ok);
    if (it->base->kind != TY_VOID) {
        snprintf(addr, sizeof addr, "rax + %u", eu_field(it, "value")->offset);
        emit_load(g, it->base, addr);
    }
}

/* rcx = rcx * n, for pointer arithmetic and indexing. */
static void emit_scale(Gen *g, const char *reg, uint32_t n) {
    if (n == 1) return;
    if ((n & (n - 1)) == 0) {
        int shift = 0;
        while ((1u << shift) < n) shift++;
        emit(g, "shl %s, %d", reg, shift);
    } else {
        emit(g, "imul %s, %s, %u", reg, reg, n);
    }
}

/* Index in rax: continue if it's below `len`, otherwise report where and
 * exit with status 101. Negative indexes are huge when compared unsigned. */
static void emit_bounds_check(Gen *g, const Expr *index, uint64_t len) {
    char what[96];
    snprintf(what, sizeof what, "index out of bounds (array length %llu)", (unsigned long long)len);
    int site = add_panic(g, index, what);
    int ok = new_label(g);
    emit(g, "cmp rax, %llu", (unsigned long long)len);
    emit(g, "jb .L%d", ok);
    emit_panic_call(g, site);
    emit_label(g, ok);
}

/* Puts the address of an lvalue (or of an aggregate rvalue) in rax. */
static void gen_addr(Gen *g, const Expr *e) {
    char buf[96];
    switch (e->kind) {
    case EXPR_IDENT:
        if (e->sym->is_extern) emit(g, "mov rax, qword ptr [rip + %s@GOTPCREL]", e->sym->asm_name);
        else emit(g, "lea rax, [%s]", var_addr(buf, sizeof buf, e->sym));
        return;
    case EXPR_UNARY:  /* *p */
        gen_value(g, e->u.unary.operand);
        return;
    case EXPR_MEMBER:
        /* The pointer's value, or the struct's address: either way, rax. */
        gen_value(g, e->u.member.object);
        if (e->u.member.object->type->kind == TY_CLASS) {
            char what[160];
            snprintf(what, sizeof what, "field `%s` of a null `%s`", e->u.member.name, e->u.member.object->type->name);
            emit_null_check(g, e->u.member.object, what);
        }
        if (e->field_offset) emit(g, "add rax, %u", e->field_offset);
        return;
    case EXPR_INDEX: {
        const Expr *base = e->u.index.base;
        if (base->type->kind == TY_STRING) {
            /* Check against the string's length, then index its bytes. */
            gen_value(g, base);
            emit(g, "push rax");
            g->depth++;
            gen_value(g, e->u.index.index);
            emit(g, "mov rcx, rax");
            emit(g, "pop rax");
            g->depth--;
            int site = add_panic(g, e->u.index.index, "string index out of bounds");
            int ok = new_label(g);
            emit(g, "cmp rcx, qword ptr [rax + 8]");
            emit(g, "jb .L%d", ok);
            emit_panic_call(g, site);
            emit_label(g, ok);
            emit(g, "mov rax, qword ptr [rax]");
            emit(g, "add rax, rcx");
            return;
        }
        gen_value(g, base);   /* array address or pointer value */
        emit(g, "push rax");
        g->depth++;
        gen_value(g, e->u.index.index);
        if (base->type->kind == TY_ARRAY) emit_bounds_check(g, e->u.index.index, base->type->len);
        emit(g, "mov rcx, rax");
        emit(g, "pop rax");
        g->depth--;
        emit_scale(g, "rcx", e->type->size);
        emit(g, "add rax, rcx");
        return;
    }
    default:
        gen_value(g, e);   /* struct/array literals: the value is an address */
        return;
    }
}

static void gen_logical(Gen *g, const Expr *e) {
    bool is_and = e->u.binary.op == TOK_ANDAND;
    int short_label = new_label(g), end_label = new_label(g);
    gen_value(g, e->u.binary.lhs);
    emit(g, "test rax, rax");
    emit(g, "%s .L%d", is_and ? "je" : "jne", short_label);
    gen_value(g, e->u.binary.rhs);
    emit(g, "jmp .L%d", end_label);
    emit_label(g, short_label);
    emit(g, "mov eax, %d", is_and ? 0 : 1);
    emit_label(g, end_label);
}

/* rax = rax <op> rcx for integers (and bools) of type t, normalized. */
static void emit_int_op(Gen *g, TokenKind op, const Type *t) {
    bool sgn = t->kind == TY_INT && t->is_signed;
    switch (op) {
    case TOK_PLUS:  emit(g, "add rax, rcx"); break;
    case TOK_MINUS: emit(g, "sub rax, rcx"); break;
    case TOK_STAR:  emit(g, "imul rax, rcx"); break;
    case TOK_SLASH:
    case TOK_PERCENT:
        if (sgn) {
            emit(g, "cqo");
            emit(g, "idiv rcx");
        } else {
            emit(g, "xor edx, edx");
            emit(g, "div rcx");
        }
        if (op == TOK_PERCENT) emit(g, "mov rax, rdx");
        break;
    case TOK_AMP:   emit(g, "and rax, rcx"); break;
    case TOK_PIPE:  emit(g, "or rax, rcx"); break;
    case TOK_CARET: emit(g, "xor rax, rcx"); break;
    case TOK_SHL:   emit(g, "shl rax, cl"); break;
    case TOK_SHR:   emit(g, sgn ? "sar rax, cl" : "shr rax, cl"); break;
    default: break;
    }
    normalize(g, t);
}

static void emit_float_op(Gen *g, TokenKind op, const Type *t) {
    const char *sfx = t->size == 4 ? "ss" : "sd";
    switch (op) {
    case TOK_PLUS:  emit(g, "add%s xmm0, xmm1", sfx); break;
    case TOK_MINUS: emit(g, "sub%s xmm0, xmm1", sfx); break;
    case TOK_STAR:  emit(g, "mul%s xmm0, xmm1", sfx); break;
    case TOK_SLASH: emit(g, "div%s xmm0, xmm1", sfx); break;
    default: break;
    }
}

static void gen_compare(Gen *g, const Expr *e) {
    TokenKind op = e->u.binary.op;
    const Type *lt = e->u.binary.lhs->type, *rt = e->u.binary.rhs->type;
    gen_value(g, e->u.binary.lhs);
    push_value(g, lt);
    gen_value(g, e->u.binary.rhs);

    if (cls(lt) == CLS_SSE) {
        /* lhs in xmm0, rhs in xmm1. ucomis sets the flags like an unsigned
         * compare, plus PF for NaN; seta/setae are false for NaN. */
        emit(g, "movaps xmm1, xmm0");
        pop_value(g, lt, NULL, 0);
        const char *ucom = lt->size == 4 ? "ucomiss" : "ucomisd";
        switch (op) {
        case TOK_EQ:
            emit(g, "%s xmm0, xmm1", ucom);
            emit(g, "sete al");
            emit(g, "setnp cl");
            emit(g, "and al, cl");
            break;
        case TOK_NE:
            emit(g, "%s xmm0, xmm1", ucom);
            emit(g, "setne al");
            emit(g, "setp cl");
            emit(g, "or al, cl");
            break;
        case TOK_LT: emit(g, "%s xmm1, xmm0", ucom); emit(g, "seta al"); break;
        case TOK_LE: emit(g, "%s xmm1, xmm0", ucom); emit(g, "setae al"); break;
        case TOK_GT: emit(g, "%s xmm0, xmm1", ucom); emit(g, "seta al"); break;
        case TOK_GE: emit(g, "%s xmm0, xmm1", ucom); emit(g, "setae al"); break;
        default: break;
        }
        emit(g, "movzx eax, al");
        return;
    }

    emit(g, "mov rcx, rax");
    pop_value(g, lt, "rax", 0);
    /* Values are extended to 64 bits, so one 64-bit compare works for any
     * width. If either side is signed, the common type is signed (sema only
     * widens unsigned into a larger signed type); pointers compare unsigned. */
    bool sgn = (type_is_intlike(lt) && lt->is_signed) || (type_is_intlike(rt) && rt->is_signed);
    const char *setcc = "sete";
    switch (op) {
    case TOK_EQ: setcc = "sete"; break;
    case TOK_NE: setcc = "setne"; break;
    case TOK_LT: setcc = sgn ? "setl" : "setb"; break;
    case TOK_GT: setcc = sgn ? "setg" : "seta"; break;
    case TOK_LE: setcc = sgn ? "setle" : "setbe"; break;
    case TOK_GE: setcc = sgn ? "setge" : "setae"; break;
    default: break;
    }
    emit(g, "cmp rax, rcx");
    emit(g, "%s al", setcc);
    emit(g, "movzx eax, al");
}

static void gen_binary(Gen *g, const Expr *e) {
    TokenKind op = e->u.binary.op;
    const Expr *l = e->u.binary.lhs, *r = e->u.binary.rhs;
    const Type *t = e->type;
    char buf[64];

    if (op == TOK_ANDAND || op == TOK_OROR) {
        gen_logical(g, e);
        return;
    }
    if (op == TOK_EQ || op == TOK_NE || op == TOK_LT || op == TOK_GT || op == TOK_LE || op == TOK_GE) {
        gen_compare(g, e);
        return;
    }
    if (op == TOK_QQ) {   /* r ?? fallback */
        int fallback = new_label(g), end = new_label(g);
        gen_value(g, l);
        emit(g, "cmp byte ptr [rax], 0");
        emit(g, "jne .L%d", fallback);
        snprintf(buf, sizeof buf, "rax + %u", eu_field(l->type, "value")->offset);
        emit_load(g, t, buf);
        emit(g, "jmp .L%d", end);
        emit_label(g, fallback);
        gen_value(g, r);
        emit_label(g, end);
        return;
    }

    gen_value(g, l);
    push_value(g, l->type);
    gen_value(g, r);

    if (cls(t) == CLS_SSE) {
        emit(g, "movaps xmm1, xmm0");
        pop_value(g, t, NULL, 0);
        emit_float_op(g, op, t);
        return;
    }

    emit(g, "mov rcx, rax");
    pop_value(g, l->type, "rax", 0);

    if (l->type->kind == TY_PTR && r->type->kind == TY_PTR) {   /* p - q, in elements */
        emit(g, "sub rax, rcx");
        uint32_t size = l->type->base->size;
        if (size > 1) {
            if ((size & (size - 1)) == 0) {
                int shift = 0;
                while ((1u << shift) < size) shift++;
                emit(g, "sar rax, %d", shift);
            } else {
                emit(g, "cqo");
                emit(g, "mov rcx, %u", size);
                emit(g, "idiv rcx");
            }
        }
        return;
    }
    if (l->type->kind == TY_PTR) {   /* p + n, p - n */
        emit_scale(g, "rcx", l->type->base->size);
        emit(g, op == TOK_PLUS ? "add rax, rcx" : "sub rax, rcx");
        return;
    }
    if (r->type->kind == TY_PTR) {   /* n + p */
        emit_scale(g, "rax", r->type->base->size);
        emit(g, "add rax, rcx");
        return;
    }
    emit_int_op(g, op, t);
}

/* ++x, --x, x++, x-- on ints and pointers. */
static void gen_incdec(Gen *g, const Expr *e, bool prefix) {
    const Expr *target = e->u.unary.operand;
    const Type *t = target->type;
    uint32_t step = t->kind == TY_PTR ? t->base->size : 1;
    bool inc = e->u.unary.op == TOK_PLUSPLUS;

    gen_addr(g, target);
    emit(g, "push rax");
    g->depth++;
    emit_load(g, t, "rax");
    emit(g, "mov rsi, rax");   /* the old value, for x++ */
    emit(g, "%s rax, %u", inc ? "add" : "sub", step);
    normalize(g, t);
    emit(g, "pop rdx");
    g->depth--;
    emit_store(g, t, "rdx");
    if (!prefix) emit(g, "mov rax, rsi");
}

static void gen_cast(Gen *g, const Expr *e) {
    const Expr *inner = e->u.cast.expr;
    const Type *from = inner->type, *to = e->type;
    if (to->kind == TY_ERRUNION) {   /* wrap a value or an error */
        int slot = alloc_slot(g, to->size, to->align);
        zero_memory(g, slot, to->size);
        gen_value(g, inner);
        bool is_error = from == to->err;
        const Field *f = eu_field(to, is_error ? "err" : "value");
        char addr[64];
        snprintf(addr, sizeof addr, "rbp - %d", -slot - (int)f->offset);
        emit_store(g, f->type, addr);
        if (is_error) emit(g, "mov byte ptr [rbp - %d], 1", -slot);
        emit(g, "lea rax, [rbp - %d]", -slot);
        return;
    }
    gen_value(g, inner);

    if (type_is_float(from) && type_is_float(to)) {
        if (from->size != to->size) emit(g, from->size == 4 ? "cvtss2sd xmm0, xmm0" : "cvtsd2ss xmm0, xmm0");
        return;
    }
    if (type_is_float(to)) {   /* int or bool -> float */
        const char *cvt = to->size == 4 ? "cvtsi2ss" : "cvtsi2sd";
        if (type_is_int(from) && !from->is_signed && from->size == 8) {
            /* A u64 above 2^63 doesn't fit cvtsi2sd's signed input: halve it,
             * keeping the low bit so rounding stays right, convert, double. */
            int big = new_label(g), done = new_label(g);
            emit(g, "test rax, rax");
            emit(g, "js .L%d", big);
            emit(g, "%s xmm0, rax", cvt);
            emit(g, "jmp .L%d", done);
            emit_label(g, big);
            emit(g, "mov rcx, rax");
            emit(g, "shr rcx, 1");
            emit(g, "and eax, 1");
            emit(g, "or rcx, rax");
            emit(g, "%s xmm0, rcx", cvt);
            emit(g, to->size == 4 ? "addss xmm0, xmm0" : "addsd xmm0, xmm0");
            emit_label(g, done);
        } else {
            emit(g, "%s xmm0, rax", cvt);
        }
        return;
    }
    if (type_is_float(from)) {   /* float -> int: truncates toward zero */
        if (to->kind == TY_INT && !to->is_signed && to->size == 8) {
            /* Values from 2^63 up need an unsigned conversion: subtract 2^63
             * first and put the top bit back afterwards. */
            if (from->size == 4) emit(g, "cvtss2sd xmm0, xmm0");
            int big = new_label(g), done = new_label(g);
            emit(g, "movabs rax, 4890909195324358656");  /* 2^63 as a double */
            emit(g, "movq xmm1, rax");
            emit(g, "ucomisd xmm0, xmm1");
            emit(g, "jae .L%d", big);
            emit(g, "cvttsd2si rax, xmm0");
            emit(g, "jmp .L%d", done);
            emit_label(g, big);
            emit(g, "subsd xmm0, xmm1");
            emit(g, "cvttsd2si rax, xmm0");
            emit(g, "btc rax, 63");
            emit_label(g, done);
        } else {
            emit(g, from->size == 4 ? "cvttss2si rax, xmm0" : "cvttsd2si rax, xmm0");
            normalize(g, to);
        }
        return;
    }
    /* int <-> int, bool -> int, pointer <-> pointer/int: re-extend for the new type. */
    normalize(g, to);
}

/* Where one argument goes. */
typedef struct {
    const Type *type;
    int slot;            /* frame temp holding the evaluated argument */
    uint32_t size;       /* bytes copied when passed on the stack (multiple of 8) */
    bool on_stack;
    int stack_off;
    int reg[2];          /* per eightbyte: GPR or XMM index */
    Classification c;
} ArgPlace;

/*
 * System V calls: arguments are evaluated left to right into frame
 * temporaries, then placed: each eightbyte of an INTEGER-class value in
 * rdi, rsi, rdx, rcx, r8, r9; SSE-class in xmm0-7; a struct whose
 * eightbytes don't all fit (or that's bigger than 16 bytes) goes on the
 * stack whole. A struct returned in MEMORY gets a hidden pointer in rdi.
 */
static void gen_call(Gen *g, const Expr *e) {
    if (e->builtin == BUILTIN_FREE) {
        gen_value(g, e->u.call.args[0]);
        emit(g, "mov rdi, rax");
        emit_c_call(g, "free");
        return;
    }

    const Symbol *fn = e->sym;
    size_t n = e->u.call.nargs;
    ArgPlace *args = xmalloc((n ? n : 1) * sizeof *args);
    char addr[64];

    for (size_t i = 0; i < n; i++) {
        const Expr *arg = e->u.call.args[i];
        const Type *t = i < fn->nparams ? fn->param_types[i] : arg->type;
        if (t->kind == TY_NULL) t = arg->type;
        args[i].type = t;
        gen_value(g, arg);
        if (i == 0 && e->builtin == BUILTIN_METHOD) {
            char what[160];
            snprintf(what, sizeof what, "method `%s` called on a null `%s`", fn->name, fn->owner->name);
            emit_null_check(g, arg, what);
        }
        bool agg = type_in_memory(t);
        args[i].size = agg ? (t->size + 7) & ~7u : 8;
        args[i].slot = alloc_slot(g, args[i].size, 8);
        snprintf(addr, sizeof addr, "rbp - %d", -args[i].slot);
        if (agg) emit_store(g, t, addr);
        else if (cls(t) == CLS_SSE) emit(g, "movsd qword ptr [%s], xmm0", addr);
        else emit(g, "mov qword ptr [%s], rax", addr);
    }

    Classification ret = classify(fn->type);
    int ngpr = ret.memory ? 1 : 0, nsse = 0, stack = 0;
    for (size_t i = 0; i < n; i++) {
        ArgPlace *a = &args[i];
        a->c = classify(a->type);
        int kint = 0, ksse = 0;
        for (int j = 0; j < a->c.n; j++) {
            if (a->c.eb[j] == EB_INT) kint++;
            else ksse++;
        }
        a->on_stack = a->c.memory || ngpr + kint > 6 || nsse + ksse > 8;
        if (a->on_stack) {
            a->stack_off = stack;
            stack += (int)a->size;
        } else {
            for (int j = 0; j < a->c.n; j++) a->reg[j] = a->c.eb[j] == EB_INT ? ngpr++ : nsse++;
        }
    }

    int pad = (g->depth * 8 + stack) % 16 ? 8 : 0;
    int total = stack + pad;
    if (total) emit(g, "sub rsp, %d", total);
    for (size_t i = 0; i < n; i++) {
        if (!args[i].on_stack) continue;
        for (uint32_t j = 0; j < args[i].size; j += 8) {
            emit(g, "mov rax, qword ptr [rbp - %d]", -args[i].slot - (int)j);
            emit(g, "mov qword ptr [rsp + %d], rax", args[i].stack_off + (int)j);
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (args[i].on_stack) continue;
        for (int j = 0; j < args[i].c.n; j++) {
            int src = -args[i].slot - 8 * j;
            if (args[i].c.eb[j] == EB_INT) emit(g, "mov %s, qword ptr [rbp - %d]", arg_regs[args[i].reg[j]]->q, src);
            else emit(g, "movsd xmm%d, qword ptr [rbp - %d]", args[i].reg[j], src);
        }
    }
    int ret_slot = 0;
    if (ret.memory) {
        ret_slot = alloc_slot(g, fn->type->size, fn->type->align);
        emit(g, "lea rdi, [rbp - %d]", -ret_slot);
    }
    if (fn->is_variadic) emit(g, "mov eax, %d", nsse);   /* vector registers used, for varargs */

    emit(g, "call %s", fn->asm_name);
    if (total) emit(g, "add rsp, %d", total);
    free(args);

    const Type *rt = fn->type;
    if (ret.memory) {
        emit(g, "lea rax, [rbp - %d]", -ret_slot);
    } else if (type_in_memory(rt)) {
        /* Reassemble the struct from rax/rdx and xmm0/xmm1. */
        int slot = alloc_slot(g, 16, 8);
        int gi = 0, si = 0;
        for (int j = 0; j < ret.n; j++) {
            if (ret.eb[j] == EB_INT) emit(g, "mov qword ptr [rbp - %d], %s", -slot - 8 * j, gi++ ? "rdx" : "rax");
            else emit(g, "movsd qword ptr [rbp - %d], xmm%d", -slot - 8 * j, si++);
        }
        emit(g, "lea rax, [rbp - %d]", -slot);
    } else if (rt->kind == TY_BOOL) {
        emit(g, "movzx eax, al");   /* C only defines the low byte */
    } else {
        normalize(g, rt);           /* C leaves the upper bits of small ints undefined */
    }
}

static void gen_aggregate_lit(Gen *g, const Expr *e) {
    const Type *t = e->type;
    int slot = alloc_slot(g, t->size, t->align);
    zero_memory(g, slot, t->size);   /* fields that aren't listed are zero */
    char addr[64];
    if (t->kind == TY_STRING) {   /* string { ptr: p, len: n } */
        for (size_t i = 0; i < e->u.struct_lit.n; i++) {
            const FieldInit *fi = e->u.struct_lit.fields[i];
            gen_value(g, fi->value);
            emit(g, "mov qword ptr [rbp - %d], rax", -slot - (strcmp(fi->name, "len") == 0 ? 8 : 0));
        }
    } else if (e->kind == EXPR_STRUCT_LIT) {
        for (size_t i = 0; i < e->u.struct_lit.n; i++) {
            const FieldInit *fi = e->u.struct_lit.fields[i];
            const Field *f = NULL;
            for (size_t j = 0; j < t->nfields; j++)
                if (strcmp(t->fields[j].name, fi->name) == 0) f = &t->fields[j];
            gen_value(g, fi->value);
            snprintf(addr, sizeof addr, "rbp - %d", -slot - (int)f->offset);
            emit_store(g, f->type, addr);
        }
    } else {
        for (size_t i = 0; i < e->u.array.n; i++) {
            gen_value(g, e->u.array.entries[i]->value);
            snprintf(addr, sizeof addr, "rbp - %d", -slot - (int)(i * t->base->size));
            emit_store(g, t->base, addr);
        }
    }
    emit(g, "lea rax, [rbp - %d]", -slot);
}

static void gen_value(Gen *g, const Expr *e) {
    const Type *t = e->type;
    char buf[96];
    switch (e->kind) {
    case EXPR_INT:
    case EXPR_CHAR:
        if (type_is_float(t)) emit_float_const(g, t, (double)e->u.int_val);
        else emit_int_const(g, type_is_int(t) ? int_normalize(t, e->u.int_val) : e->u.int_val);
        return;
    case EXPR_FLOAT:
        emit_float_const(g, t, e->u.float_val);
        return;
    case EXPR_STRING: {
        int label = string_label(g, e->u.str);
        emit(g, "lea rax, [rip + .Lstr%d]", label);
        if (t->kind == TY_STRING) {   /* a {ptr, len} value in a temporary */
            int slot = alloc_slot(g, 16, 8);
            emit(g, "mov qword ptr [rbp - %d], rax", -slot);
            emit(g, "mov qword ptr [rbp - %d], %zu", -slot - 8, e->u.str.len);
            emit(g, "lea rax, [rbp - %d]", -slot);
        }
        return;
    }
    case EXPR_ALLOC: {
        /* calloc(count, size): zeroed, and it checks count * size for overflow. */
        if (e->u.alloc.count) gen_value(g, e->u.alloc.count);
        else emit(g, "mov eax, 1");
        emit(g, "mov rdi, rax");
        emit(g, "mov esi, %u", t->base->size);
        emit_c_call(g, "calloc");
        int site = add_panic(g, e, "out of memory");
        int ok = new_label(g);
        emit(g, "test rax, rax");
        emit(g, "jnz .L%d", ok);
        emit_panic_call(g, site);
        emit_label(g, ok);
        return;
    }
    case EXPR_BOOL:
        emit(g, "mov eax, %d", e->u.bool_val ? 1 : 0);
        return;
    case EXPR_NULL:
        emit(g, "xor eax, eax");
        return;
    case EXPR_IDENT:
        if (e->sym->is_extern) {
            gen_addr(g, e);
            emit_load(g, t, "rax");
        } else {
            emit_load(g, t, var_addr(buf, sizeof buf, e->sym));
        }
        return;
    case EXPR_UNARY: {
        TokenKind op = e->u.unary.op;
        const Expr *operand = e->u.unary.operand;
        if (op == TOK_MINUS && operand->kind == EXPR_INT) {   /* a negative literal */
            if (type_is_float(t)) emit_float_const(g, t, -(double)operand->u.int_val);
            else emit_int_const(g, int_normalize(t, 0 - operand->u.int_val));
            return;
        }
        switch (op) {
        case TOK_MINUS:
            gen_value(g, operand);
            if (type_is_float(t)) {   /* flip the sign bit */
                if (t->size == 4) {
                    emit(g, "movd eax, xmm0");
                    emit(g, "btc eax, 31");
                    emit(g, "movd xmm0, eax");
                } else {
                    emit(g, "movq rax, xmm0");
                    emit(g, "btc rax, 63");
                    emit(g, "movq xmm0, rax");
                }
            } else {
                emit(g, "neg rax");
                normalize(g, t);
            }
            return;
        case TOK_TILDE:
            gen_value(g, operand);
            emit(g, "not rax");
            normalize(g, t);
            return;
        case TOK_BANG:
            gen_value(g, operand);
            emit(g, "xor eax, 1");
            return;
        case TOK_AMP:
            gen_addr(g, operand);
            return;
        case TOK_STAR:
            gen_value(g, operand);
            emit_load(g, t, "rax");
            return;
        case TOK_PLUSPLUS:
        case TOK_MINUSMINUS:
            gen_incdec(g, e, true);
            return;
        default:
            return;
        }
    }
    case EXPR_POSTFIX:
        gen_incdec(g, e, false);
        return;
    case EXPR_BINARY:
        gen_binary(g, e);
        return;
    case EXPR_TERNARY: {
        int else_label = new_label(g), end_label = new_label(g);
        gen_value(g, e->u.ternary.cond);
        emit(g, "test rax, rax");
        emit(g, "je .L%d", else_label);
        gen_value(g, e->u.ternary.then_expr);
        emit(g, "jmp .L%d", end_label);
        emit_label(g, else_label);
        gen_value(g, e->u.ternary.else_expr);
        emit_label(g, end_label);
        return;
    }
    case EXPR_CALL:
        gen_call(g, e);
        return;
    case EXPR_MEMBER:
        if (e->builtin != BUILTIN_NONE) {
            gen_eu_member(g, e);
            return;
        }
        gen_addr(g, e);
        if (cls(t) != CLS_MEM) emit_load(g, t, "rax");
        return;
    case EXPR_INDEX:
        gen_addr(g, e);
        if (cls(t) != CLS_MEM) emit_load(g, t, "rax");
        return;
    case EXPR_TRY:
        gen_try(g, e);
        return;
    case EXPR_THIS:
        emit_load(g, t, var_addr(buf, sizeof buf, e->sym));
        return;
    case EXPR_NEW:
        gen_new(g, e);
        return;
    case EXPR_CAST:
        gen_cast(g, e);
        return;
    case EXPR_STRUCT_LIT:
    case EXPR_ARRAY_LIT:
        gen_aggregate_lit(g, e);
        return;
    default:
        fatal("codegen: unexpected expression kind %d (sema should have rejected it)", (int)e->kind);
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
    default:                 return TOK_SHR;
    }
}

static void gen_assign(Gen *g, const Expr *e) {
    TokenKind op = e->u.binary.op;
    const Expr *target = e->u.binary.lhs, *value = e->u.binary.rhs;
    const Type *t = target->type;
    char buf[96];

    if (op == TOK_ASSIGN) {
        if (target->kind == EXPR_IDENT && !target->sym->is_extern) {
            gen_value(g, value);
            emit_store(g, t, var_addr(buf, sizeof buf, target->sym));
            return;
        }
        /* Left to right: the target's address, then the value. */
        gen_addr(g, target);
        emit(g, "push rax");
        g->depth++;
        gen_value(g, value);
        emit(g, "pop rdx");
        g->depth--;
        emit_store(g, t, "rdx");
        return;
    }

    TokenKind bop = compound_to_binary(op);
    gen_addr(g, target);
    emit(g, "push rax");
    g->depth++;
    emit_load(g, t, "rax");
    push_value(g, t);
    gen_value(g, value);
    if (cls(t) == CLS_SSE) {
        emit(g, "movaps xmm1, xmm0");
        pop_value(g, t, NULL, 0);
        emit_float_op(g, bop, t);
    } else {
        emit(g, "mov rcx, rax");
        pop_value(g, t, "rax", 0);
        if (t->kind == TY_PTR) {
            emit_scale(g, "rcx", t->base->size);
            emit(g, bop == TOK_PLUS ? "add rax, rcx" : "sub rax, rcx");
        } else {
            emit_int_op(g, bop, t);
        }
    }
    emit(g, "pop rdx");
    g->depth--;
    emit_store(g, t, "rdx");
}

static void gen_effect(Gen *g, const Expr *e) {
    if (e->kind == EXPR_ASSIGN) gen_assign(g, e);
    else gen_value(g, e);
}

/* ---- statements ---------------------------------------------------------- */

static void gen_stmt(Gen *g, const Stmt *s);

static void push_loop(Gen *g, int break_label, int continue_label) {
    if (g->nloops == g->loops_cap) {
        g->loops_cap = g->loops_cap ? g->loops_cap * 2 : 8;
        Loop *grown = realloc(g->loops, g->loops_cap * sizeof *grown);
        if (!grown) fatal("out of memory");
        g->loops = grown;
    }
    g->loops[g->nloops].break_label = break_label;
    g->loops[g->nloops].continue_label = continue_label;
    g->loops[g->nloops].scopes = g->nscopes;
    g->nloops++;
}

static void gen_jump_if_false(Gen *g, const Expr *cond, int label) {
    gen_value(g, cond);
    emit(g, "test rax, rax");
    emit(g, "je .L%d", label);
}

/* A block (or a single statement used as one): runs its defers, newest
 * first, when control reaches its end. */
static void gen_body(Gen *g, const Stmt *s) {
    if (g->nscopes == g->scopes_cap) {
        g->scopes_cap = g->scopes_cap ? g->scopes_cap * 2 : 16;
        size_t *grown = realloc(g->scopes, g->scopes_cap * sizeof *grown);
        if (!grown) fatal("out of memory");
        g->scopes = grown;
    }
    size_t mark = g->defers.len;
    g->scopes[g->nscopes++] = mark;
    if (s->kind == STMT_BLOCK)
        for (size_t i = 0; i < s->u.block.n; i++) gen_stmt(g, s->u.block.stmts[i]);
    else
        gen_stmt(g, s);
    emit_defers(g, mark);
    g->defers.len = mark;
    g->nscopes--;
}

/* Puts the value being returned (in rax/xmm0) where the epilogue expects it. */
static void store_return_value(Gen *g) {
    const Type *rt = g->fn->type;
    if (classify(rt).memory) {   /* straight into the caller's buffer */
        emit(g, "mov rdi, qword ptr [rbp - %d]", -g->hidden_slot);
        emit(g, "mov rsi, rax");
        emit(g, "mov ecx, %u", rt->size);
        emit(g, "rep movsb");
    } else if (type_in_memory(rt)) {
        emit(g, "lea rdi, [rbp - %d]", -g->ret_slot);
        emit(g, "mov rsi, rax");
        emit(g, "mov ecx, %u", rt->size);
        emit(g, "rep movsb");
    } else if (cls(rt) == CLS_SSE) {
        emit(g, "movsd qword ptr [rbp - %d], xmm0", -g->ret_slot);
    } else if (cls(rt) == CLS_GPR) {
        emit(g, "mov qword ptr [rbp - %d], rax", -g->ret_slot);
    }
}

static void gen_var_decl(Gen *g, const VarDecl *vd) {
    Symbol *v = vd->sym;
    const Type *t = v->type;
    v->offset = alloc_slot(g, t->size, t->align);
    char buf[96];
    if (vd->init) {
        gen_value(g, vd->init);
        emit_store(g, t, var_addr(buf, sizeof buf, v));
    } else {
        zero_memory(g, v->offset, t->size);   /* locals start at zero */
    }
}

static void gen_stmt(Gen *g, const Stmt *s) {
    switch (s->kind) {
    case STMT_BLOCK:
        gen_body(g, s);
        break;
    case STMT_EXPR:
        gen_effect(g, s->u.expr);
        break;
    case STMT_VAR:
        gen_var_decl(g, s->u.var);
        break;
    case STMT_RETURN:
        if (s->u.expr) {
            gen_value(g, s->u.expr);
            store_return_value(g);
        }
        emit_return_jump(g);
        break;
    case STMT_DEFER:   /* runs when its block exits */
        ptrvec_push(&g->defers, s->u.defer_body);
        break;
    case STMT_IF: {
        int else_label = new_label(g), end_label = new_label(g);
        gen_jump_if_false(g, s->u.if_.cond, else_label);
        gen_body(g, s->u.if_.then_stmt);
        if (s->u.if_.else_stmt) emit(g, "jmp .L%d", end_label);
        emit_label(g, else_label);
        if (s->u.if_.else_stmt) {
            gen_body(g, s->u.if_.else_stmt);
            emit_label(g, end_label);
        }
        break;
    }
    case STMT_WHILE: {
        int top = new_label(g), end = new_label(g);
        emit_label(g, top);
        gen_jump_if_false(g, s->u.while_.cond, end);
        push_loop(g, end, top);
        gen_body(g, s->u.while_.body);
        g->nloops--;
        emit(g, "jmp .L%d", top);
        emit_label(g, end);
        break;
    }
    case STMT_DO_WHILE: {
        int top = new_label(g), cont = new_label(g), end = new_label(g);
        emit_label(g, top);
        push_loop(g, end, cont);
        gen_body(g, s->u.while_.body);
        g->nloops--;
        emit_label(g, cont);
        gen_value(g, s->u.while_.cond);
        emit(g, "test rax, rax");
        emit(g, "jne .L%d", top);
        emit_label(g, end);
        break;
    }
    case STMT_FOR: {
        int top = new_label(g), cont = new_label(g), end = new_label(g);
        if (s->u.for_.init) gen_stmt(g, s->u.for_.init);
        emit_label(g, top);
        if (s->u.for_.cond) gen_jump_if_false(g, s->u.for_.cond, end);
        push_loop(g, end, cont);
        gen_body(g, s->u.for_.body);
        g->nloops--;
        emit_label(g, cont);
        if (s->u.for_.step) gen_effect(g, s->u.for_.step);
        emit(g, "jmp .L%d", top);
        emit_label(g, end);
        break;
    }
    case STMT_ECHO:   /* sema turned each value into a runtime call */
        for (size_t i = 0; i < s->u.echo.n; i++) gen_value(g, s->u.echo.exprs[i]);
        break;
    case STMT_BREAK:
    case STMT_CONTINUE: {
        const Loop *loop = &g->loops[g->nloops - 1];
        if (loop->scopes < g->nscopes) emit_defers(g, g->scopes[loop->scopes]);
        emit(g, "jmp .L%d", s->kind == STMT_BREAK ? loop->break_label : loop->continue_label);
        break;
    }
    default:
        fatal("codegen: unexpected statement kind %d (sema should have rejected it)", (int)s->kind);
    }
}

/* ---- functions ------------------------------------------------------------- */

static void gen_function(Gen *g, const Symbol *fn) {
    const Decl *d = fn->decl;
    g->ret_label = new_label(g);
    g->depth = 0;
    g->frame = 0;
    g->nloops = 0;

    /* The body goes to a buffer first: the prologue needs the frame size,
     * which is only known once every local and temporary has been placed. */
    char *body = NULL;
    size_t body_len = 0;
    FILE *real_out = g->out;
    g->out = open_memstream(&body, &body_len);
    if (!g->out) fatal("open_memstream failed");

    /* Spill register parameters; stack parameters stay where the caller
     * put them. This must place arguments exactly like gen_call. */
    int ngpr = 0, nsse = 0, stack = 16;
    g->fn = fn;
    g->defers.len = 0;
    g->nscopes = 0;
    Classification ret = classify(fn->type);
    if (ret.memory) {
        g->hidden_slot = alloc_slot(g, 8, 8);
        emit(g, "mov qword ptr [rbp - %d], rdi", -g->hidden_slot);
        ngpr = 1;
    } else if (fn->type->kind != TY_VOID) {
        g->ret_slot = alloc_slot(g, 16, 8);
        /* `void!E` functions succeed by reaching their end: is_err = 0. */
        if (fn->type->kind == TY_ERRUNION) {
            emit(g, "mov qword ptr [rbp - %d], 0", -g->ret_slot);
            emit(g, "mov qword ptr [rbp - %d], 0", -g->ret_slot - 8);
        }
    }
    if (fn->this_param) {   /* methods: `this` comes first, in a register */
        fn->this_param->offset = alloc_slot(g, 8, 8);
        emit(g, "mov qword ptr [rbp - %d], %s", -fn->this_param->offset, arg_regs[ngpr++]->q);
    }
    for (size_t i = 0; i < d->u.func.nparams; i++) {
        Symbol *p = d->u.func.params[i]->sym;
        Classification c = classify(p->type);
        int kint = 0, ksse = 0;
        for (int j = 0; j < c.n; j++) {
            if (c.eb[j] == EB_INT) kint++;
            else ksse++;
        }
        uint32_t size = type_in_memory(p->type) ? (p->type->size + 7) & ~7u : 8;
        if (c.memory || ngpr + kint > 6 || nsse + ksse > 8) {
            p->offset = stack;
            stack += (int)size;
            continue;
        }
        p->offset = alloc_slot(g, size, 8);
        for (int j = 0; j < c.n; j++) {
            if (c.eb[j] == EB_INT) emit(g, "mov qword ptr [rbp - %d], %s", -p->offset - 8 * j, arg_regs[ngpr++]->q);
            else emit(g, "movsd qword ptr [rbp - %d], xmm%d", -p->offset - 8 * j, nsse++);
        }
    }
    gen_body(g, d->u.func.body);

    fclose(g->out);
    g->out = real_out;

    int frame = (g->frame + 15) & ~15;
    const char *name = fn->asm_name;
    fputc('\n', g->out);
    if (fn->is_export) fprintf(g->out, "    .globl %s\n", name);
    fprintf(g->out, "    .type %s, @function\n%s:\n", name, name);
    emit(g, "push rbp");
    emit(g, "mov rbp, rsp");
    if (frame) emit(g, "sub rsp, %d", frame);
    fwrite(body, 1, body_len, g->out);
    free(body);
    emit_label(g, g->ret_label);
    if (ret.memory) {
        emit(g, "mov rax, qword ptr [rbp - %d]", -g->hidden_slot);
    } else if (type_in_memory(fn->type)) {
        int gi = 0, si = 0;
        for (int j = 0; j < ret.n; j++) {
            if (ret.eb[j] == EB_INT) emit(g, "mov %s, qword ptr [rbp - %d]", gi++ ? "rdx" : "rax", -g->ret_slot - 8 * j);
            else emit(g, "movsd xmm%d, qword ptr [rbp - %d]", si++, -g->ret_slot - 8 * j);
        }
    } else if (cls(fn->type) == CLS_SSE) {
        emit(g, "movsd xmm0, qword ptr [rbp - %d]", -g->ret_slot);
    } else if (cls(fn->type) == CLS_GPR) {
        emit(g, "mov rax, qword ptr [rbp - %d]", -g->ret_slot);
    }
    emit(g, "leave");
    emit(g, "ret");
    fprintf(g->out, "    .size %s, .-%s\n", name, name);
}

/* ---- data ------------------------------------------------------------------ */

static void emit_bytes(Gen *g, const unsigned char *bytes, uint32_t size) {
    for (uint32_t i = 0; i < size; i += 16) {
        fputs("    .byte ", g->out);
        for (uint32_t j = i; j < size && j < i + 16; j++) fprintf(g->out, "%s%u", j > i ? ", " : "", bytes[j]);
        fputc('\n', g->out);
    }
}

static int log2_align(uint32_t align) {
    int n = 0;
    while ((1u << n) < align) n++;
    return n;
}

static void emit_globals(Gen *g, const Program *prog) {
    /* .data: mutable with an initializer; .rodata: const; .bss: all zero;
     * .data.rel.ro: const but holding addresses, which the dynamic loader
     * may need to fill in before making it read-only. */
    static const char *const sections[4] = {".data", ".rodata", ".bss", ".data.rel.ro"};
    for (int sec = 0; sec < 4; sec++) {
        bool any = false;
        for (size_t i = 0; i < prog->nglobals; i++) {
            const Symbol *v = prog->globals[i];
            int want = v->is_const ? (v->nrelocs ? 3 : 1) : v->init ? 0 : 2;
            if (want != sec || v->type->size == 0 || v->is_extern) continue;
            if (!any) fprintf(g->out, "\n    .section %s\n", sections[sec]);
            any = true;
            if (v->is_export) fprintf(g->out, "    .globl %s\n", v->asm_name);
            fprintf(g->out, "    .p2align %d\n%s:\n", log2_align(v->type->align), v->asm_name);
            if (!v->init) {
                fprintf(g->out, "    .zero %u\n", v->type->size);
                continue;
            }
            /* Bytes, with string literal addresses filled in by the linker. */
            uint32_t at = 0;
            for (size_t k = 0; k <= v->nrelocs; k++) {
                uint32_t next = k < v->nrelocs ? v->relocs[k].offset : v->type->size;
                emit_bytes(g, v->init + at, next - at);
                if (k == v->nrelocs) break;
                fprintf(g->out, "    .quad .Lstr%d\n", string_label(g, v->relocs[k].str));
                at = next + 8;
            }
        }
    }
}

/* The runtime's assembly primitives (declared `extern` in the prelude). */
static void emit_runtime_primitive(Gen *g, const char *name) {
    FILE *out = g->out;
    fprintf(out, "\n    .type w.%s, @function\nw.%s:\n", name, name);
    if (strcmp(name, "__rt_out") == 0) {
        if (g->libc) {   /* through stdio, so output interleaves correctly with printf */
            fputs("    sub rsp, 8\n"
                  "    mov rdx, rsi\n"
                  "    mov esi, 1\n"
                  "    mov rcx, qword ptr [rip + stdout@GOTPCREL]\n"
                  "    mov rcx, qword ptr [rcx]\n"
                  "    call fwrite\n"
                  "    add rsp, 8\n"
                  "    ret\n",
                  out);
        } else {         /* write(1, p, n), retrying partial writes */
            fputs("    mov rdx, rsi\n"
                  "    mov rsi, rdi\n"
                  ".Lrt_out_loop:\n"
                  "    test rdx, rdx\n"
                  "    jz .Lrt_out_done\n"
                  "    mov edi, 1\n"
                  "    mov eax, 1\n"
                  "    syscall\n"
                  "    test rax, rax\n"
                  "    js .Lrt_out_done\n"
                  "    add rsi, rax\n"
                  "    sub rdx, rax\n"
                  "    jmp .Lrt_out_loop\n"
                  ".Lrt_out_done:\n"
                  "    ret\n",
                  out);
        }
    } else if (strcmp(name, "__rt_mmap") == 0) {   /* mmap(NULL, n, RW, PRIVATE|ANON, -1, 0) */
        fputs("    mov rsi, rdi\n"
              "    xor edi, edi\n"
              "    mov edx, 3\n"
              "    mov r10d, 34\n"
              "    mov r8, -1\n"
              "    xor r9d, r9d\n"
              "    mov eax, 9\n"
              "    syscall\n"
              "    ret\n",
              out);
    } else if (strcmp(name, "__rt_out_of_memory") == 0) {
        fputs("    lea rdi, [rip + .Lrt_oom]\n"
              "    mov esi, 21\n"
              "    call w.panic\n"
              "    .section .rodata\n"
              ".Lrt_oom:\n"
              "    .ascii \"panic: out of memory\\n\"\n"
              "    .text\n",
              out);
        g->need_panic = true;
    } else if (strcmp(name, "__rt_fmt_f64") == 0) {   /* snprintf(buf, 32, "%.14g", x) */
        fputs("    sub rsp, 8\n"
              "    mov esi, 32\n"
              "    lea rdx, [rip + .Lrt_fmt_g]\n"
              "    mov eax, 1\n"
              "    call snprintf\n"
              "    mov eax, eax\n"
              "    add rsp, 8\n"
              "    ret\n"
              "    .section .rodata\n"
              ".Lrt_fmt_g:\n"
              "    .asciz \"%.14g\"\n"
              "    .text\n",
              out);
    }
}

/* string name(E v): the variant's name, or the number if v isn't one. */
static void emit_enum_name_fn(Gen *g, const Symbol *fn) {
    const Type *et = fn->enum_type;
    const char *name = fn->asm_name;
    fprintf(g->out, "\n    .type %s, @function\n%s:\n", name, name);
    int *labels = xmalloc((et->nvariants ? et->nvariants : 1) * sizeof *labels);
    for (size_t i = 0; i < et->nvariants; i++) {
        int64_t v = et->variants[i].value;
        labels[i] = new_label(g);
        if (v >= INT32_MIN && v <= INT32_MAX) {
            emit(g, "cmp rdi, %lld", (long long)v);
        } else {
            emit(g, "movabs rax, %lld", (long long)v);
            emit(g, "cmp rdi, rax");
        }
        emit(g, "je .L%d", labels[i]);
    }
    emit(g, "jmp %s", fn->callees[0]->asm_name);   /* not a variant: the number */
    for (size_t i = 0; i < et->nvariants; i++) {
        StrBuf text = {et->variants[i].name, strlen(et->variants[i].name)};
        emit_label(g, labels[i]);
        emit(g, "lea rax, [rip + .Lstr%d]", string_label(g, text));
        emit(g, "mov edx, %zu", text.len);
        emit(g, "ret");
    }
    fprintf(g->out, "    .size %s, .-%s\n", name, name);
    free(labels);
}

static void emit_ascii(FILE *out, const char *s) {
    fputs("    .ascii \"", out);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(out, "\\%c", c);
        else if (c == '\n') fputs("\\n", out);
        else if (c < 0x20 || c >= 0x7f) fprintf(out, "\\%03o", c);
        else fputc(c, out);
    }
    fputs("\"\n", out);
}

void codegen_x64(const Module *m, const Program *prog, FILE *out) {
    Gen g = {0};
    g.out = out;

    g.libc = prog->needs_libc;
    fprintf(out, "# generated by watrc from %s\n", m->file->path);
    fputs("    .intel_syntax noprefix\n    .text\n", out);
    if (g.libc) {
        /* Linked with the C runtime: its startup code calls this `main`. */
        fputs("\n# C entry point: the C runtime calls this, and exits with its result.\n"
              "    .globl main\n"
              "    .type main, @function\n"
              "main:\n"
              "    push rbp\n"
              "    mov rbp, rsp\n"
              "    call w.main\n"
              "    pop rbp\n"
              "    ret\n",
              out);
    } else {
        fputs("\n# Process entry: call main, exit with its return value.\n"
              "    .globl _start\n"
              "_start:\n"
              "    xor ebp, ebp\n"
              "    call w.main\n"
              "    mov edi, eax\n"
              "    mov eax, 60\n"
              "    syscall\n",
              out);
    }

    for (size_t i = 0; i < prog->nfuncs; i++) {
        const Symbol *fn = prog->funcs[i];
        if (!fn->reachable) continue;
        if (fn->decl->u.func.body) gen_function(&g, fn);
        else if (fn->is_runtime) emit_runtime_primitive(&g, fn->name);
    }

    for (size_t i = 0; i < prog->nenum_fns; i++)
        if (prog->enum_fns[i]->reachable) emit_enum_name_fn(&g, prog->enum_fns[i]);

    if (g.panics.len || g.need_panic) {
        /* w.panic(msg = rdi, len = rsi): print to stderr, exit with status 101. */
        fputs("\n# Runtime check failures.\n"
              "w.panic:\n", out);
        if (g.libc)   /* flush stdout first, so output printed before the panic comes before it */
            fputs("    mov rbx, rdi\n"   /* panic never returns, so rbx and r12 are free */
                  "    mov r12, rsi\n"
                  "    and rsp, -16\n"
                  "    xor edi, edi\n"
                  "    call fflush\n"
                  "    mov rdi, rbx\n"
                  "    mov rsi, r12\n",
                  out);
        fputs("    mov rdx, rsi\n"
              "    mov rsi, rdi\n"
              "    mov edi, 2\n"
              "    mov eax, 1\n"
              "    syscall\n"
              "    mov edi, 101\n",
              out);
        if (g.libc)   /* exit() flushes stdio buffers, so earlier printf output isn't lost */
            fputs("    and rsp, -16\n"
                  "    call exit\n",
                  out);
        else
            fputs("    mov eax, 60\n"
                  "    syscall\n",
                  out);
        fputs("\n    .section .rodata\n", out);
        for (size_t i = 0; i < g.panics.len; i++) {
            PanicSite *site = g.panics.items[i];
            fprintf(out, ".Lpanic%d:\n", site->label);
            emit_ascii(out, site->msg);
            free(site->msg);
            free(site);
        }
    }

    emit_globals(&g, prog);

    if (g.strings.len) {
        fputs("\n    .section .rodata\n", out);
        for (size_t i = 0; i < g.strings.len; i++) {
            StringLit *lit = g.strings.items[i];
            fprintf(out, ".Lstr%d:\n", lit->label);
            emit_bytes(&g, (const unsigned char *)lit->bytes.data, (uint32_t)lit->bytes.len);
            fputs("    .byte 0\n", out);
            free(lit);
        }
    }


    /* Tell the linker this code doesn't need an executable stack. */
    fputs("\n    .section .note.GNU-stack,\"\",@progbits\n", out);

    free(g.panics.items);
    free(g.strings.items);
    free(g.loops);
    free(g.defers.items);
    free(g.scopes);
}
