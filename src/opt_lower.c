#include <string.h>

#include "opt_ir.h"

/*
 * AST -> IR. Every expression lowers to a vreg holding its value (for
 * aggregates: its address), mirroring the -O0 generator construct by
 * construct. Scalar locals and parameters whose address is never taken
 * get vregs; everything else lives in frame slots.
 */

/* ---- building blocks -------------------------------------------------------- */

int ir_new_vreg(Fn *f, VKind k) {
    if (f->nv == f->vcap) {
        f->vcap = f->vcap ? f->vcap * 2 : 64;
        VKind *grown = realloc(f->vk, (size_t)f->vcap * sizeof *grown);
        if (!grown) fatal("out of memory");
        f->vk = grown;
    }
    f->vk[f->nv] = k;
    return f->nv++;
}

int ir_new_slot(Fn *f, uint32_t size, uint32_t align) {
    if (f->nslots == f->slots_cap) {
        f->slots_cap = f->slots_cap ? f->slots_cap * 2 : 16;
        Slot *grown = realloc(f->slots, (size_t)f->slots_cap * sizeof *grown);
        if (!grown) fatal("out of memory");
        f->slots = grown;
    }
    Slot *s = &f->slots[f->nslots];
    s->size = size ? size : 1;
    s->align = align < 8 ? 8 : align;
    s->incoming = false;
    s->off = 0;
    return f->nslots++;
}

Ins *ir_emit(Fn *f, IrOp op) {
    if (f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 256;
        Ins *grown = realloc(f->code, f->cap * sizeof *grown);
        if (!grown) fatal("out of memory");
        f->code = grown;
    }
    Ins *in = &f->code[f->n++];
    memset(in, 0, sizeof *in);
    in->op = op;
    in->d = in->a = in->b = in->idx = in->slot = -1;
    in->scale = 1;
    return in;
}

static VKind kind_of(const Type *t) {
    if (t->kind == TY_FLOAT) return t->size == 4 ? VK_F32 : VK_F64;
    return VK_INT;
}

static int konst(Fn *f, int64_t v) {
    Ins *in = ir_emit(f, I_CONST);
    in->d = ir_new_vreg(f, VK_INT);
    in->imm = v;
    return in->d;
}

static int fkonst(Fn *f, const Type *t, double v) {
    Ins *in = ir_emit(f, I_FCONST);
    in->d = ir_new_vreg(f, kind_of(t));
    if (t->size == 4) {
        float x = (float)v;
        uint32_t bits;
        memcpy(&bits, &x, 4);
        in->imm = bits;
    } else {
        uint64_t bits;
        memcpy(&bits, &v, 8);
        in->imm = (int64_t)bits;
    }
    return in->d;
}

static void move(Fn *f, int d, int a) {
    Ins *in = ir_emit(f, I_MOV);
    in->d = d;
    in->a = a;
}

static int copy(Fn *f, int a) {
    int d = ir_new_vreg(f, f->vk[a]);
    move(f, d, a);
    return d;
}

static int bin(Fn *f, int op, int a, int b) {
    Ins *in = ir_emit(f, I_BIN);
    in->d = ir_new_vreg(f, VK_INT);
    in->a = a;
    in->b = b;
    in->sub = op;
    return in->d;
}

static int unop(Fn *f, IrOp op, int a, VKind k) {
    Ins *in = ir_emit(f, op);
    in->d = ir_new_vreg(f, k);
    in->a = a;
    return in->d;
}

/* Re-establishes the invariant that an int vreg holds `t` sign/zero-extended. */
static int ext(Fn *f, int a, const Type *t) {
    if (!type_is_intlike(t) || t->size >= 8) return a;
    Ins *in = ir_emit(f, I_EXT);
    in->d = ir_new_vreg(f, VK_INT);
    in->a = a;
    in->sub = (int)t->size;
    in->sgn = t->is_signed;
    return in->d;
}

static int cmp(Fn *f, int a, int cc, int b) {
    Ins *in = ir_emit(f, f->vk[a] == VK_INT ? I_CMP : I_FCMP);
    in->d = ir_new_vreg(f, VK_INT);
    in->a = a;
    in->b = b;
    in->sub = cc;
    return in->d;
}

static int label(Fn *f) {
    return new_label(f->g);
}

static void place(Fn *f, int l) {
    ir_emit(f, I_LABEL)->l1 = l;
}

static void jmp(Fn *f, int l) {
    ir_emit(f, I_JMP)->l1 = l;
}

static void cbr(Fn *f, int a, int cc, int b, int lt, int lf) {
    Ins *in = ir_emit(f, I_CBR);
    in->a = a;
    in->b = b;
    in->sub = cc;
    in->l1 = lt;
    in->l2 = lf;
}

/* Panics with `site` when (a cc b); the reporting code goes at the end. */
static void panic_if(Fn *f, int a, int cc, int b, int site) {
    int lp = label(f), ok = label(f);
    cbr(f, a, cc, b, lp, ok);
    int *pair = xmalloc(2 * sizeof *pair);
    pair[0] = lp;
    pair[1] = site;
    ptrvec_push(&f->panic_jumps, pair);
    place(f, ok);
}

static void null_check(Fn *f, int v, const Expr *at, const char *what) {
    panic_if(f, v, CC_EQ, konst(f, 0), add_panic(f->g, at, what));
}

/* ---- lvalues ------------------------------------------------------------------- */

typedef struct {
    bool is_vreg;
    int vreg;           /* is_vreg */
    int base;           /* address base vreg, or -1 */
    int slot;           /* or a frame slot */
    const char *sym;    /* or a global */
    int idx, scale;
    int64_t off;
} LVal;

static LVal lv_vreg(int v) {
    LVal l = {true, v, -1, -1, NULL, -1, 1, 0};
    return l;
}

static LVal lv_mem(int base) {
    LVal l = {false, -1, base, -1, NULL, -1, 1, 0};
    return l;
}

static void set_addr(Ins *in, const LVal *l) {
    in->a = l->base;
    in->slot = l->slot;
    in->sym = l->sym;
    in->idx = l->idx;
    in->scale = l->scale;
    in->imm = l->off;
}

/* The address an lvalue names, as a vreg. */
static int lea(Fn *f, const LVal *l) {
    if (l->base >= 0 && l->idx < 0 && l->off == 0) return l->base;
    Ins *in = ir_emit(f, I_LEA);
    in->d = ir_new_vreg(f, VK_INT);
    set_addr(in, l);
    return in->d;
}

/* Adds idx * scale to an address. */
static LVal lv_index(Fn *f, LVal l, int idx, uint32_t scale) {
    if (l.idx >= 0 || l.sym) {   /* one index per address, and none with rip-relative globals */
        l = lv_mem(lea(f, &l));
    }
    if (scale != 1 && scale != 2 && scale != 4 && scale != 8) {
        idx = bin(f, B_MUL, idx, konst(f, scale));
        scale = 1;
    }
    l.idx = idx;
    l.scale = (int)scale;
    return l;
}

static int load(Fn *f, const LVal *l, const Type *t) {
    if (l->is_vreg) return copy(f, l->vreg);
    if (cls(t) == CLS_MEM) return lea(f, l);
    Ins *in = ir_emit(f, I_LOAD);
    in->d = ir_new_vreg(f, kind_of(t));
    set_addr(in, l);
    in->sub = (int)t->size;
    in->sgn = type_is_intlike(t) && t->is_signed;
    return in->d;
}

static void store(Fn *f, const LVal *l, const Type *t, int v) {
    if (l->is_vreg) {
        move(f, l->vreg, v);
        return;
    }
    if (cls(t) == CLS_MEM) {
        int dst = lea(f, l);   /* before the copy: lea may emit instructions */
        Ins *in = ir_emit(f, I_COPY);
        in->a = dst;
        in->b = v;
        in->imm = t->size;
        return;
    }
    Ins *in = ir_emit(f, I_STORE);
    set_addr(in, l);
    in->b = v;
    in->sub = (int)t->size;
}

static void zero(Fn *f, int addr, uint32_t size) {
    Ins *in = ir_emit(f, I_ZERO);
    in->a = addr;
    in->imm = size;
}

static int slot_addr(Fn *f, int slot) {
    LVal l = lv_mem(-1);
    l.slot = slot;
    return lea(f, &l);
}

/* ---- calls ------------------------------------------------------------------------ */

static int lv(Fn *f, const Expr *e);

static int call_with(Fn *f, const char *name, bool variadic, int *args, const Type **types, size_t n, const Type *ret) {
    IrCall *c = xmalloc(sizeof *c);
    c->name = name;
    c->variadic = variadic;
    c->n = n;
    c->args = args;
    c->types = types;
    c->ret = ret;
    c->ret_slot = -1;
    c->vslot = 0;
    c->iface = NULL;
    Ins *in = ir_emit(f, I_CALL);
    in->call = c;
    if (ret->kind == TY_VOID) return -1;
    if (type_in_memory(ret)) {
        Classification rc = classify(ret);
        c->ret_slot = rc.memory ? ir_new_slot(f, ret->size, ret->align) : ir_new_slot(f, 16, 8);
    }
    in->d = ir_new_vreg(f, kind_of(ret));
    return in->d;
}

/* A call to a runtime or compiler-known function with scalar arguments. */
static int call_simple(Fn *f, const Symbol *fn, int *vals, size_t n) {
    int *args = xmalloc((n ? n : 1) * sizeof *args);
    const Type **types = xmalloc((n ? n : 1) * sizeof *types);
    for (size_t i = 0; i < n; i++) {
        args[i] = vals[i];
        types[i] = i < fn->nparams ? fn->param_types[i] : ty_u64;
    }
    return call_with(f, fn->asm_name, false, args, types, n, fn->type);
}

static int lower_call(Fn *f, const Expr *e) {
    const Symbol *fn = e->sym;
    if (e->builtin == BUILTIN_FREE) {
        int v = lv(f, e->u.call.args[0]);
        return call_simple(f, fn, &v, 1);
    }
    size_t n = e->u.call.nargs;
    int *args = xmalloc((n ? n : 1) * sizeof *args);
    const Type **types = xmalloc((n ? n : 1) * sizeof *types);
    for (size_t i = 0; i < n; i++) {
        const Expr *arg = e->u.call.args[i];
        const Type *t = i < fn->nparams ? fn->param_types[i] : arg->type;
        if (t->kind == TY_NULL) t = arg->type;
        types[i] = t;
        int v = lv(f, arg);
        if (i == 0 && e->builtin == BUILTIN_METHOD) {
            char what[160];
            snprintf(what, sizeof what, "method `%s` called on a null `%s`", fn->name, fn->owner->name);
            null_check(f, v, arg, what);
        }
        if (type_in_memory(t)) {   /* pass a snapshot: later arguments may change the original */
            int s = ir_new_slot(f, (t->size + 7) & ~7u, t->align > 8 ? t->align : 8);
            int a = slot_addr(f, s);
            Ins *in = ir_emit(f, I_COPY);
            in->a = a;
            in->b = v;
            in->imm = t->size;
            v = a;
        }
        args[i] = v;
    }
    int d = call_with(f, fn->asm_name, fn->is_variadic, args, types, n, fn->type);
    if (e->vslot) {   /* the call is the last instruction emitted */
        for (size_t i = f->n; i-- > 0;)
            if (f->code[i].op == I_CALL) {
                f->code[i].call->vslot = e->vslot;
                f->code[i].call->iface = e->viface ? e->viface->desc_name : NULL;
                break;
            }
    }
    return d;
}

/* ---- expressions -------------------------------------------------------------- */

static void lower_body(Fn *f, const Stmt *s);
static void lcond(Fn *f, const Expr *e, int lt, int lf);

static bool is_lvalue_expr(const Expr *e) {
    switch (e->kind) {
    case EXPR_IDENT: case EXPR_THIS: case EXPR_INDEX: return true;
    case EXPR_UNARY: return e->u.unary.op == TOK_STAR;
    case EXPR_MEMBER: return e->builtin == BUILTIN_NONE;
    default: return false;
    }
}

static LVal lower_lval(Fn *f, const Expr *e) {
    switch (e->kind) {
    case EXPR_IDENT: {
        const Symbol *s = e->sym;
        if (s->kind == SYM_GLOBAL) {
            if (s->is_extern) {
                Ins *in = ir_emit(f, I_GOT);
                in->d = ir_new_vreg(f, VK_INT);
                in->sym = s->asm_name;
                return lv_mem(in->d);
            }
            LVal l = lv_mem(-1);
            l.sym = s->asm_name;
            return l;
        }
        if (s->vreg >= 0) return lv_vreg(s->vreg);
        LVal l = lv_mem(-1);
        l.slot = s->slot;
        return l;
    }
    case EXPR_THIS:
        return lv_vreg(e->sym->vreg);
    case EXPR_UNARY:   /* *p */
        return lv_mem(lv(f, e->u.unary.operand));
    case EXPR_MEMBER: {
        const Expr *obj = e->u.member.object;
        LVal l;
        if (obj->type->kind == TY_CLASS) {
            int v = lv(f, obj);
            char what[160];
            snprintf(what, sizeof what, "field `%s` of a null `%s`", e->u.member.name, obj->type->name);
            null_check(f, v, obj, what);
            l = lv_mem(v);
        } else if (e->via_pointer || !is_lvalue_expr(obj)) {
            l = lv_mem(lv(f, obj));   /* the pointer, or the aggregate rvalue's address */
        } else {
            l = lower_lval(f, obj);
        }
        l.off += e->field_offset;
        return l;
    }
    case EXPR_INDEX: {
        const Expr *base = e->u.index.base, *index = e->u.index.index;
        if (base->type->kind == TY_STRING) {
            /* Check against the string's length, then index its bytes. */
            LVal s = is_lvalue_expr(base) ? lower_lval(f, base) : lv_mem(lv(f, base));
            int i = lv(f, index);
            LVal len_at = s, ptr_at = s;
            len_at.off += 8;
            int len = load(f, &len_at, ty_u64);
            panic_if(f, i, CC_AE, len, add_panic(f->g, index, "string index out of bounds"));
            return lv_index(f, lv_mem(load(f, &ptr_at, ty_u64)), i, 1);
        }
        if (base->type->kind == TY_ARRAY) {
            LVal a = is_lvalue_expr(base) ? lower_lval(f, base) : lv_mem(lv(f, base));
            uint64_t n = base->type->len;
            if (index->kind == EXPR_INT && index->u.int_val < n) {   /* checked at compile time */
                a.off += (int64_t)(index->u.int_val * e->type->size);
                return a;
            }
            int i = lv(f, index);
            char what[96];
            snprintf(what, sizeof what, "index out of bounds (array length %llu)", (unsigned long long)n);
            panic_if(f, i, CC_AE, konst(f, (int64_t)n), add_panic(f->g, index, what));
            return lv_index(f, a, i, e->type->size);
        }
        int p = lv(f, base);   /* pointer */
        int i = lv(f, index);
        return lv_index(f, lv_mem(p), i, e->type->size);
    }
    default:
        return lv_mem(lv(f, e));   /* aggregate rvalues: their value is an address */
    }
}

static int int_op(TokenKind op, bool sgn) {
    switch (op) {
    case TOK_PLUS: return B_ADD;
    case TOK_MINUS: return B_SUB;
    case TOK_STAR: return B_MUL;
    case TOK_SLASH: return sgn ? B_SDIV : B_UDIV;
    case TOK_PERCENT: return sgn ? B_SREM : B_UREM;
    case TOK_AMP: return B_AND;
    case TOK_PIPE: return B_OR;
    case TOK_CARET: return B_XOR;
    case TOK_SHL: return B_SHL;
    default: return sgn ? B_SAR : B_SHR;
    }
}

static int float_op(TokenKind op) {
    switch (op) {
    case TOK_PLUS: return F_ADD;
    case TOK_MINUS: return F_SUB;
    case TOK_STAR: return F_MUL;
    default: return F_DIV;
    }
}

/* a op b, with the operand types of `l` and `r` and result type `t`. */
static int arith(Fn *f, TokenKind op, int a, const Type *lt, int b, const Type *rt, const Type *t) {
    if (type_is_float(t)) {
        Ins *in = ir_emit(f, I_FBIN);
        in->d = ir_new_vreg(f, kind_of(t));
        in->a = a;
        in->b = b;
        in->sub = float_op(op);
        return in->d;
    }
    if (lt->kind == TY_PTR && rt->kind == TY_PTR) {   /* p - q, in elements */
        int d = bin(f, B_SUB, a, b);
        uint32_t size = lt->base->size;
        if (size > 1) {
            if ((size & (size - 1)) == 0) {
                int shift = 0;
                while ((1u << shift) < size) shift++;
                d = bin(f, B_SAR, d, konst(f, shift));
            } else {
                d = bin(f, B_SDIV, d, konst(f, size));
            }
        }
        return d;
    }
    if (lt->kind == TY_PTR || rt->kind == TY_PTR) {   /* p + n, p - n, n + p */
        int p = lt->kind == TY_PTR ? a : b, n = lt->kind == TY_PTR ? b : a;
        uint32_t size = (lt->kind == TY_PTR ? lt : rt)->base->size;
        if (op == TOK_MINUS) {
            if (size != 1) n = bin(f, B_MUL, n, konst(f, size));
            return bin(f, B_SUB, p, n);
        }
        LVal l = lv_index(f, lv_mem(p), n, size);
        Ins *in = ir_emit(f, I_LEA);
        in->d = ir_new_vreg(f, VK_INT);
        set_addr(in, &l);
        return in->d;
    }
    bool sgn = t->kind == TY_INT && t->is_signed;
    return ext(f, bin(f, int_op(op, sgn), a, b), t);
}

static int compare_cc(TokenKind op, bool sgn) {
    switch (op) {
    case TOK_EQ: return CC_EQ;
    case TOK_NE: return CC_NE;
    case TOK_LT: return sgn ? CC_LT : CC_B;
    case TOK_GT: return sgn ? CC_GT : CC_A;
    case TOK_LE: return sgn ? CC_LE : CC_BE;
    default: return sgn ? CC_GE : CC_AE;
    }
}

static bool is_compare(TokenKind op) {
    return op == TOK_EQ || op == TOK_NE || op == TOK_LT || op == TOK_GT || op == TOK_LE || op == TOK_GE;
}

static int compare(Fn *f, const Expr *e, int *ccp, int *bp) {
    const Type *lt = e->u.binary.lhs->type, *rt = e->u.binary.rhs->type;
    int a = lv(f, e->u.binary.lhs);
    int b = lv(f, e->u.binary.rhs);
    bool sgn = (type_is_intlike(lt) && lt->is_signed) || (type_is_intlike(rt) && rt->is_signed);
    *ccp = type_is_float(lt) ? compare_cc(e->u.binary.op, true) : compare_cc(e->u.binary.op, sgn);
    *bp = b;
    return a;
}

/* ++x, --x, x++, x-- */
static int incdec(Fn *f, const Expr *e, bool prefix) {
    const Expr *target = e->u.unary.operand;
    const Type *t = target->type;
    LVal l = lower_lval(f, target);
    int old = load(f, &l, t);
    int step = konst(f, t->kind == TY_PTR ? t->base->size : 1);
    int now = bin(f, e->u.unary.op == TOK_PLUSPLUS ? B_ADD : B_SUB, old, step);
    now = ext(f, now, t);
    store(f, &l, t, now);
    return prefix ? now : old;
}

static int lower_cast(Fn *f, const Expr *e) {
    const Expr *inner = e->u.cast.expr;
    const Type *from = inner->type, *to = e->type;
    if (to->kind == TY_ERRUNION) {   /* wrap a value or an error */
        int s = ir_new_slot(f, to->size, to->align);
        int a = slot_addr(f, s);
        zero(f, a, to->size);
        int v = lv(f, inner);
        bool is_error = from == to->err;
        const Field *fl = eu_field(to, is_error ? "err" : "value");
        LVal l = lv_mem(-1);
        l.slot = s;
        l.off = fl->offset;
        store(f, &l, fl->type, v);
        if (is_error) {
            l.off = 0;
            store(f, &l, ty_u8, konst(f, 1));
        }
        return a;
    }
    int v = lv(f, inner);
    if (type_is_float(from) && type_is_float(to)) {
        if (from->size == to->size) return v;
        return unop(f, I_F2F, v, kind_of(to));
    }
    if (type_is_float(to))   /* int or bool -> float */
        return unop(f, type_is_int(from) && !from->is_signed && from->size == 8 ? I_U2F : I_I2F, v, kind_of(to));
    if (type_is_float(from)) {   /* float -> int: truncates toward zero */
        if (to->kind == TY_INT && !to->is_signed && to->size == 8) {
            if (from->size == 4) v = unop(f, I_F2F, v, VK_F64);
            return unop(f, I_F2U, v, VK_INT);
        }
        return ext(f, unop(f, I_F2I, v, VK_INT), to);
    }
    return ext(f, v, to);   /* int <-> int, bool -> int, pointers: re-extend for the new type */
}

static int lower_aggregate_lit(Fn *f, const Expr *e) {
    const Type *t = e->type;
    int s = ir_new_slot(f, t->size, t->align);
    int a = slot_addr(f, s);
    zero(f, a, t->size);   /* fields that aren't listed are zero */
    LVal l = lv_mem(-1);
    l.slot = s;
    if (t->kind == TY_STRING) {   /* string { ptr: p, len: n } */
        for (size_t i = 0; i < e->u.struct_lit.n; i++) {
            const FieldInit *fi = e->u.struct_lit.fields[i];
            int v = lv(f, fi->value);
            l.off = strcmp(fi->name, "len") == 0 ? 8 : 0;
            store(f, &l, ty_u64, v);
        }
    } else if (e->kind == EXPR_STRUCT_LIT) {
        for (size_t i = 0; i < e->u.struct_lit.n; i++) {
            const FieldInit *fi = e->u.struct_lit.fields[i];
            const Field *fl = NULL;
            for (size_t j = 0; j < t->nfields; j++)
                if (strcmp(t->fields[j].name, fi->name) == 0) fl = &t->fields[j];
            int v = lv(f, fi->value);
            l.off = fl->offset;
            store(f, &l, fl->type, v);
        }
    } else {
        for (size_t i = 0; i < e->u.array.n; i++) {
            int v = lv(f, e->u.array.entries[i]->value);
            l.off = (int64_t)(i * t->base->size);
            store(f, &l, t->base, v);
        }
    }
    return a;
}

static void emit_defers(Fn *f, size_t from) {
    for (size_t i = f->defers.len; i-- > from;) lower_body(f, f->defers.items[i]);
}

/* r?: on an error, return it from this function (running defers). */
static int lower_try(Fn *f, const Expr *e) {
    const Expr *inner = e->u.try_expr;
    const Type *it = inner->type, *rt = f->fn->type;
    int v = lv(f, inner);
    LVal l = lv_mem(v);
    int flag = load(f, &l, ty_u8);
    int ok = label(f), bad = label(f);
    cbr(f, flag, CC_EQ, konst(f, 0), ok, bad);

    place(f, bad);
    l.off = eu_field(it, "err")->offset;
    int err = load(f, &l, it->err);
    /* Build the function's result in place: zeroed, is_err = 1, err. */
    int base = f->hidden >= 0 ? f->hidden : slot_addr(f, f->ret_slot);
    zero(f, base, rt->size);
    LVal r = lv_mem(base);
    store(f, &r, ty_u8, konst(f, 1));
    r.off = eu_field(rt, "err")->offset;
    store(f, &r, rt->err, err);
    emit_defers(f, 0);
    jmp(f, f->exit_label);

    place(f, ok);
    if (it->base->kind == TY_VOID) return -1;
    l.off = eu_field(it, "value")->offset;
    return load(f, &l, it->base);
}

static int lower_eu_member(Fn *f, const Expr *e) {
    int v = lv(f, e->u.member.object);   /* the error union's address, either way */
    LVal l = lv_mem(v);
    int flag = load(f, &l, ty_u8);
    if (e->builtin == BUILTIN_EU_OK) return bin(f, B_XOR, flag, konst(f, 1));
    bool want_value = e->builtin == BUILTIN_EU_VALUE;
    int site = add_panic(f->g, e, want_value ? "`->value` of a result that holds an error"
                                             : "`->error` of a result that holds a value");
    panic_if(f, flag, want_value ? CC_NE : CC_EQ, konst(f, 0), site);
    l.off = e->field_offset;
    return load(f, &l, e->type);
}

static int lv(Fn *f, const Expr *e) {
    const Type *t = e->type;
    switch (e->kind) {
    case EXPR_INT:
    case EXPR_CHAR:
        if (type_is_float(t)) return fkonst(f, t, (double)e->u.int_val);
        return konst(f, (int64_t)(type_is_int(t) ? int_normalize(t, e->u.int_val) : e->u.int_val));
    case EXPR_FLOAT:
        return fkonst(f, t, e->u.float_val);
    case EXPR_STRING: {
        Ins *in = ir_emit(f, I_STR);
        in->d = ir_new_vreg(f, VK_INT);
        in->l1 = t->kind == TY_STRING ? string_obj_label(f->g, e->u.str) : string_label(f->g, e->u.str);
        return in->d;
    }
    case EXPR_ALLOC: {
        int vals[2];
        vals[0] = e->u.alloc.count ? lv(f, e->u.alloc.count) : konst(f, 1);
        vals[1] = konst(f, t->base->size);
        return call_simple(f, e->sym, vals, 2);
    }
    case EXPR_BOOL:
        return konst(f, e->u.bool_val ? 1 : 0);
    case EXPR_NULL:
        return konst(f, 0);
    case EXPR_IDENT:
    case EXPR_INDEX:
    case EXPR_THIS: {
        LVal l = lower_lval(f, e);
        return load(f, &l, t);
    }
    case EXPR_UNARY: {
        TokenKind op = e->u.unary.op;
        const Expr *operand = e->u.unary.operand;
        if (op == TOK_MINUS && operand->kind == EXPR_INT) {   /* a negative literal */
            if (type_is_float(t)) return fkonst(f, t, -(double)operand->u.int_val);
            return konst(f, (int64_t)int_normalize(t, 0 - operand->u.int_val));
        }
        switch (op) {
        case TOK_MINUS:
            if (type_is_float(t)) return unop(f, I_FNEG, lv(f, operand), kind_of(t));
            return ext(f, unop(f, I_NEG, lv(f, operand), VK_INT), t);
        case TOK_TILDE:
            return ext(f, unop(f, I_NOT, lv(f, operand), VK_INT), t);
        case TOK_BANG:
            return bin(f, B_XOR, lv(f, operand), konst(f, 1));
        case TOK_AMP: {
            LVal l = lower_lval(f, operand);
            return lea(f, &l);
        }
        case TOK_STAR: {
            LVal l = lv_mem(lv(f, operand));
            return load(f, &l, t);
        }
        case TOK_PLUSPLUS:
        case TOK_MINUSMINUS:
            return incdec(f, e, true);
        default:
            fatal("opt: unexpected unary operator");
        }
    }
    case EXPR_POSTFIX:
        return incdec(f, e, false);
    case EXPR_BINARY: {
        TokenKind op = e->u.binary.op;
        if (op == TOK_ANDAND || op == TOK_OROR) {
            int d = ir_new_vreg(f, VK_INT), yes = label(f), no = label(f), end = label(f);
            lcond(f, e, yes, no);
            place(f, yes);
            move(f, d, konst(f, 1));
            jmp(f, end);
            place(f, no);
            move(f, d, konst(f, 0));
            place(f, end);
            return d;
        }
        if (is_compare(op)) {
            int cc, b;
            int a = compare(f, e, &cc, &b);
            return cmp(f, a, cc, b);
        }
        if (op == TOK_QQ) {   /* r ?? fallback */
            const Expr *l = e->u.binary.lhs;
            int v = lv(f, l);
            LVal at = lv_mem(v);
            int flag = load(f, &at, ty_u8);
            int d = ir_new_vreg(f, kind_of(cls(t) == CLS_MEM ? ty_u64 : t));
            int fallback = label(f), value = label(f), end = label(f);
            cbr(f, flag, CC_NE, konst(f, 0), fallback, value);
            place(f, value);
            at.off = eu_field(l->type, "value")->offset;
            move(f, d, load(f, &at, t));
            jmp(f, end);
            place(f, fallback);
            move(f, d, lv(f, e->u.binary.rhs));
            place(f, end);
            return d;
        }
        const Expr *l = e->u.binary.lhs, *r = e->u.binary.rhs;
        int a = lv(f, l);
        int b = lv(f, r);
        return arith(f, op, a, l->type, b, r->type, t);
    }
    case EXPR_TERNARY: {
        int d = ir_new_vreg(f, kind_of(cls(t) == CLS_MEM ? ty_u64 : t));
        int yes = label(f), no = label(f), end = label(f);
        lcond(f, e->u.ternary.cond, yes, no);
        place(f, yes);
        move(f, d, lv(f, e->u.ternary.then_expr));
        jmp(f, end);
        place(f, no);
        move(f, d, lv(f, e->u.ternary.else_expr));
        place(f, end);
        return d;
    }
    case EXPR_CALL:
        return lower_call(f, e);
    case EXPR_MEMBER: {
        if (e->builtin != BUILTIN_NONE) return lower_eu_member(f, e);
        LVal l = lower_lval(f, e);
        return load(f, &l, t);
    }
    case EXPR_TRY:
        return lower_try(f, e);
    case EXPR_NEW: {
        const Type *ct = t;
        int size = konst(f, ct->obj_size ? ct->obj_size : 1);
        int obj = call_simple(f, e->u.new_.alloc, &size, 1);   /* zeroed memory */
        e->u.new_.tmp->vreg = obj;
        LVal desc = lv_mem(-1);   /* the object's class */
        desc.sym = ct->desc_name;
        LVal head = lv_mem(obj);
        store(f, &head, ty_u64, lea(f, &desc));
        for (size_t i = 0; i < ct->nfields; i++) {
            const Field *fl = &ct->fields[i];
            if (!fl->init) continue;
            int v = lv(f, fl->init);
            LVal at = lv_mem(obj);
            at.off = fl->offset;
            store(f, &at, fl->type, v);
        }
        if (e->u.new_.ctor) lower_call(f, e->u.new_.ctor);
        return obj;
    }
    case EXPR_CAST:
        return lower_cast(f, e);
    case EXPR_ARRAY_LIT:
        if (t->kind == TY_PHPARRAY) {   /* create, then fill */
            int a = lower_call(f, e->u.array.create);
            e->u.array.tmp->vreg = a;
            for (size_t i = 0; i < e->u.array.ncalls; i++) lower_call(f, e->u.array.calls[i]);
            return a;
        }
        return lower_aggregate_lit(f, e);
    case EXPR_STRUCT_LIT:
        return lower_aggregate_lit(f, e);
    default:
        fatal("opt: unexpected expression kind %d (sema should have rejected it)", (int)e->kind);
    }
}

/* Jumps to lt if e is true, else to lf. */
static void lcond(Fn *f, const Expr *e, int lt, int lf) {
    if (e->kind == EXPR_BINARY && (e->u.binary.op == TOK_ANDAND || e->u.binary.op == TOK_OROR)) {
        int mid = label(f);
        if (e->u.binary.op == TOK_ANDAND) lcond(f, e->u.binary.lhs, mid, lf);
        else lcond(f, e->u.binary.lhs, lt, mid);
        place(f, mid);
        lcond(f, e->u.binary.rhs, lt, lf);
        return;
    }
    if (e->kind == EXPR_UNARY && e->u.unary.op == TOK_BANG) {
        lcond(f, e->u.unary.operand, lf, lt);
        return;
    }
    if (e->kind == EXPR_BOOL) {
        jmp(f, e->u.bool_val ? lt : lf);
        return;
    }
    if (e->kind == EXPR_BINARY && is_compare(e->u.binary.op) && !type_is_float(e->u.binary.lhs->type)) {
        int cc, b;
        int a = compare(f, e, &cc, &b);
        cbr(f, a, cc, b, lt, lf);
        return;
    }
    cbr(f, lv(f, e), CC_NE, konst(f, 0), lt, lf);
}

/* ---- statements ------------------------------------------------------------------- */

static void lower_stmt(Fn *f, const Stmt *s);

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

static void lower_effect(Fn *f, const Expr *e) {
    if (e->kind != EXPR_ASSIGN) {
        lv(f, e);
        return;
    }
    const Expr *target = e->u.binary.lhs, *value = e->u.binary.rhs;
    const Type *t = target->type;
    LVal l = lower_lval(f, target);   /* left to right: the target, then the value */
    if (e->u.binary.op == TOK_ASSIGN) {
        store(f, &l, t, lv(f, value));
        return;
    }
    int old = load(f, &l, t);
    int v = lv(f, value);
    store(f, &l, t, arith(f, compound_to_binary(e->u.binary.op), old, t, v, value->type, t));
}

static void push_loop(Fn *f, int brk, int cont) {
    if (f->nloops == f->loops_cap) {
        f->loops_cap = f->loops_cap ? f->loops_cap * 2 : 8;
        Loop *grown = realloc(f->loops, f->loops_cap * sizeof *grown);
        if (!grown) fatal("out of memory");
        f->loops = grown;
    }
    f->loops[f->nloops].break_label = brk;
    f->loops[f->nloops].continue_label = cont;
    f->loops[f->nloops].scopes = f->nscopes;
    f->nloops++;
}

static void lower_body(Fn *f, const Stmt *s) {
    if (f->nscopes == f->scopes_cap) {
        f->scopes_cap = f->scopes_cap ? f->scopes_cap * 2 : 16;
        size_t *grown = realloc(f->scopes, f->scopes_cap * sizeof *grown);
        if (!grown) fatal("out of memory");
        f->scopes = grown;
    }
    size_t mark = f->defers.len;
    f->scopes[f->nscopes++] = mark;
    if (s->kind == STMT_BLOCK)
        for (size_t i = 0; i < s->u.block.n; i++) lower_stmt(f, s->u.block.stmts[i]);
    else
        lower_stmt(f, s);
    emit_defers(f, mark);
    f->defers.len = mark;
    f->nscopes--;
}

static void lower_var(Fn *f, const VarDecl *vd) {
    Symbol *v = vd->sym;
    const Type *t = v->type;
    if (cls(t) != CLS_MEM && !v->addr_taken) {
        v->vreg = ir_new_vreg(f, kind_of(t));
        v->slot = -1;
        int init = vd->init ? lv(f, vd->init) : type_is_float(t) ? fkonst(f, t, 0.0) : konst(f, 0);
        move(f, v->vreg, init);
        return;
    }
    v->vreg = -1;
    v->slot = ir_new_slot(f, t->size, t->align);
    LVal l = lv_mem(-1);
    l.slot = v->slot;
    if (vd->init) store(f, &l, t, lv(f, vd->init));
    else zero(f, lea(f, &l), t->size);   /* locals start at zero */
}

static const char *const asm_reg64[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                          "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};

static void lower_asm(Fn *f, const Stmt *s) {
    size_t n = s->u.asm_.nops;
    IrAsm *as = xmalloc(sizeof *as);
    as->stmt = s;
    as->ins = xmalloc((n ? n : 1) * sizeof(int));
    as->outs = xmalloc((n ? n : 1) * sizeof(int));
    as->in_slots = xmalloc((n ? n : 1) * sizeof(int));
    as->out_slots = xmalloc((n ? n : 1) * sizeof(int));
    for (size_t i = 0; i < n; i++) {
        const AsmOperand *op = s->u.asm_.ops[i];
        as->ins[i] = as->outs[i] = as->in_slots[i] = as->out_slots[i] = -1;
        if (op->dir != ASM_OUT) {
            as->ins[i] = lv(f, op->expr);
            as->in_slots[i] = ir_new_slot(f, 8, 8);
        }
        if (op->dir != ASM_IN) {
            int r = asm_register(op->reg.data, op->reg.len);
            as->outs[i] = ir_new_vreg(f, r >= 16 ? kind_of(op->expr->type) : VK_INT);
            as->out_slots[i] = ir_new_slot(f, 8, 8);
        }
    }
    ir_emit(f, I_ASM)->as = as;
    (void)asm_reg64;
    for (size_t i = 0; i < n; i++) {
        const AsmOperand *op = s->u.asm_.ops[i];
        if (op->dir == ASM_IN) continue;
        const Type *t = op->expr->type;
        int v = as->outs[i];
        if (t->kind == TY_BOOL) {   /* any nonzero low byte is true */
            Ins *in = ir_emit(f, I_EXT);
            in->d = ir_new_vreg(f, VK_INT);
            in->a = v;
            in->sub = 1;
            v = cmp(f, in->d, CC_NE, konst(f, 0));
        } else if (cls(t) == CLS_GPR) {
            v = ext(f, v, t);
        }
        LVal l = lower_lval(f, op->expr);
        store(f, &l, t, v);
    }
}

static void lower_stmt(Fn *f, const Stmt *s) {
    switch (s->kind) {
    case STMT_BLOCK:
        lower_body(f, s);
        break;
    case STMT_EXPR:
        lower_effect(f, s->u.expr);
        break;
    case STMT_VAR:
        lower_var(f, s->u.var);
        break;
    case STMT_RETURN:
        if (s->u.expr) {
            int v = lv(f, s->u.expr);
            const Type *rt = f->fn->type;
            if (f->hidden >= 0 || f->ret_slot >= 0) {
                int dst = f->hidden >= 0 ? f->hidden : slot_addr(f, f->ret_slot);
                Ins *in = ir_emit(f, I_COPY);
                in->a = dst;
                in->b = v;
                in->imm = rt->size;
            } else {
                move(f, f->retv, v);
            }
        }
        emit_defers(f, 0);
        jmp(f, f->exit_label);
        break;
    case STMT_DEFER:
        ptrvec_push(&f->defers, s->u.defer_body);
        break;
    case STMT_IF: {
        int yes = label(f), no = label(f), end = label(f);
        lcond(f, s->u.if_.cond, yes, no);
        place(f, yes);
        lower_body(f, s->u.if_.then_stmt);
        if (s->u.if_.else_stmt) jmp(f, end);
        place(f, no);
        if (s->u.if_.else_stmt) {
            lower_body(f, s->u.if_.else_stmt);
            place(f, end);
        }
        break;
    }
    case STMT_WHILE: {   /* the test goes at the bottom: one jump per iteration */
        int body = label(f), test = label(f), end = label(f);
        jmp(f, test);
        place(f, body);
        push_loop(f, end, test);
        lower_body(f, s->u.while_.body);
        f->nloops--;
        place(f, test);
        lcond(f, s->u.while_.cond, body, end);
        place(f, end);
        break;
    }
    case STMT_DO_WHILE: {
        int body = label(f), test = label(f), end = label(f);
        place(f, body);
        push_loop(f, end, test);
        lower_body(f, s->u.while_.body);
        f->nloops--;
        place(f, test);
        lcond(f, s->u.while_.cond, body, end);
        place(f, end);
        break;
    }
    case STMT_FOR: {
        int body = label(f), cont = label(f), test = label(f), end = label(f);
        if (s->u.for_.init) lower_stmt(f, s->u.for_.init);
        jmp(f, test);
        place(f, body);
        push_loop(f, end, cont);
        lower_body(f, s->u.for_.body);
        f->nloops--;
        place(f, cont);
        if (s->u.for_.step) lower_effect(f, s->u.for_.step);
        place(f, test);
        if (s->u.for_.cond) lcond(f, s->u.for_.cond, body, end);
        else jmp(f, body);
        place(f, end);
        break;
    }
    case STMT_ECHO:   /* sema turned each value into a runtime call */
        for (size_t i = 0; i < s->u.echo.n; i++) lv(f, s->u.echo.exprs[i]);
        break;
    case STMT_BREAK:
    case STMT_CONTINUE: {
        const Loop *loop = &f->loops[f->nloops - 1];
        if (loop->scopes < f->nscopes) emit_defers(f, f->scopes[loop->scopes]);
        jmp(f, s->kind == STMT_BREAK ? loop->break_label : loop->continue_label);
        break;
    }
    case STMT_ASM:
        lower_asm(f, s);
        break;
    default:
        fatal("opt: unexpected statement kind %d", (int)s->kind);
    }
}

/* ---- functions ------------------------------------------------------------------- */

static bool in_register(const Symbol *s) {
    return cls(s->type) != CLS_MEM && !s->addr_taken;
}

void ir_lower_function(Fn *f) {
    const Symbol *fn = f->fn;
    const Decl *d = fn->decl;
    f->exit_label = label(f);
    f->retv = f->ret_slot = f->hidden = -1;

    /* Where parameters arrive; this must match the caller's placement. */
    size_t np = d->u.func.nparams + (fn->this_param ? 1 : 0);
    f->params = xmalloc((np ? np : 1) * sizeof *f->params);
    f->nparams = 0;
    int ngpr = 0, nsse = 0, stack = 16;
    Classification ret = classify(fn->type);
    if (ret.memory) {
        f->hidden = ir_new_vreg(f, VK_INT);
        ngpr = 1;
    }
    for (size_t i = 0; i < np; i++) {
        Symbol *p = fn->this_param && i == 0 ? fn->this_param : d->u.func.params[i - (fn->this_param ? 1 : 0)]->sym;
        ParamPlace *pp = &f->params[f->nparams++];
        memset(pp, 0, sizeof *pp);
        pp->type = p->type;
        pp->c = classify(p->type);
        int kint = 0, ksse = 0;
        for (int j = 0; j < pp->c.n; j++) {
            if (pp->c.eb[j] == EB_INT) kint++;
            else ksse++;
        }
        uint32_t size = type_in_memory(p->type) ? (p->type->size + 7) & ~7u : 8;
        pp->on_stack = pp->c.memory || ngpr + kint > 6 || nsse + ksse > 8;
        if (pp->on_stack) {
            pp->stack_off = stack;
            stack += (int)size;
        } else {
            for (int j = 0; j < pp->c.n; j++) pp->reg[j] = pp->c.eb[j] == EB_INT ? ngpr++ : nsse++;
        }
        pp->vreg = pp->slot = -1;
        if (in_register(p)) {
            pp->vreg = p->vreg = ir_new_vreg(f, kind_of(p->type));
            p->slot = -1;
        } else if (pp->on_stack) {   /* already in memory, where the caller put it */
            pp->slot = p->slot = ir_new_slot(f, size, 8);
            f->slots[pp->slot].incoming = true;
            f->slots[pp->slot].off = pp->stack_off;
            p->vreg = -1;
        } else {
            pp->slot = p->slot = ir_new_slot(f, size, p->type->align > 8 ? p->type->align : 8);
            p->vreg = -1;
        }
    }
    ir_emit(f, I_PARAMS);

    const Type *rt = fn->type;
    if (!ret.memory && type_in_memory(rt)) {
        f->ret_slot = ir_new_slot(f, 16, 8);
        /* `void!E` functions succeed by reaching their end: is_err = 0. */
        if (rt->kind == TY_ERRUNION) zero(f, slot_addr(f, f->ret_slot), 16);
    } else if (!ret.memory && rt->kind != TY_VOID) {
        f->retv = ir_new_vreg(f, kind_of(rt));
    }

    lower_body(f, d->u.func.body);
    jmp(f, f->exit_label);

    place(f, f->exit_label);
    int result = f->hidden >= 0 ? f->hidden : f->ret_slot >= 0 ? slot_addr(f, f->ret_slot) : f->retv;
    ir_emit(f, I_RET)->a = result;

    for (size_t i = 0; i < f->panic_jumps.len; i++) {
        int *pair = f->panic_jumps.items[i];
        place(f, pair[0]);
        ir_emit(f, I_PANIC)->l1 = pair[1];
        free(pair);
    }
}
