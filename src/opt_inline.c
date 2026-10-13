#include <string.h>

#include "opt_ir.h"

/*
 * Inlining: a direct call to a small function is replaced by a copy of
 * the function's (already optimized) IR, with its vregs, slots and labels
 * renamed into the caller. Functions are processed callees first, so a
 * small function that calls another small one brings both along.
 *
 * Only plain cases qualify: scalar parameters passed in registers, a
 * scalar or void result, no inline asm, and no recursion into the caller.
 * The caller is then optimized again, which is where the payoff is:
 * arguments that are constants fold through the copied code, and
 * `this->field` reads become ordinary loads.
 */

enum {
    INLINE_MAX = 30,          /* instructions in a function that can be inlined */
    CALLER_MAX = 4000,        /* stop growing a caller past this */
};

typedef struct {
    Fn **fns;                 /* by function index; NULL for those not compiled here */
    size_t n;
    int *state;               /* 0 new, 1 in progress, 2 done */
} Inliner;

static Fn *find_fn(Inliner *in, const char *name) {
    for (size_t i = 0; i < in->n; i++)
        if (in->fns[i] && strcmp(in->fns[i]->fn->asm_name, name) == 0) return in->fns[i];
    return NULL;
}

static int body_size(const Fn *f) {
    int n = 0;
    for (size_t i = 0; i < f->n; i++) {
        IrOp op = f->code[i].op;
        if (op != I_NOP && op != I_LABEL && op != I_PARAMS && op != I_RET && op != I_PANIC) n++;
    }
    return n;
}

static bool inlinable(const Fn *callee, const Fn *caller) {
    if (callee == caller || callee->hidden >= 0 || callee->ret_slot >= 0 || callee->fn->is_variadic) return false;
    for (size_t p = 0; p < callee->nparams; p++) {
        const ParamPlace *pp = &callee->params[p];
        if (pp->on_stack || type_in_memory(pp->type)) return false;
    }
    for (size_t i = 0; i < callee->n; i++)
        if (callee->code[i].op == I_ASM) return false;
    return body_size(callee) <= INLINE_MAX;
}

typedef struct {
    Ins *code;
    size_t n, cap;
} Code;

static Ins *push(Code *c, Ins in) {
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 256;
        c->code = realloc(c->code, c->cap * sizeof *c->code);
        if (!c->code) fatal("out of memory");
    }
    c->code[c->n] = in;
    return &c->code[c->n++];
}

static Ins blank(IrOp op) {
    Ins in;
    memset(&in, 0, sizeof in);
    in.op = op;
    in.d = in.a = in.b = in.idx = in.slot = -1;
    return in;
}

/* Copies callee's body in place of the call `call` (in caller), into out. */
static void expand(Fn *caller, const Ins *call, const Fn *callee, Code *out) {
    /* Callee vreg -> caller vreg; immediates stay immediates. */
    int *vmap = xmalloc(((size_t)callee->nv + 1) * sizeof *vmap);
    for (int v = 0; v < callee->nv; v++) {
        vmap[v] = ir_new_vreg(caller, callee->vk[v]);
        ir_ensure_imm(caller);
        if (callee->is_imm[v]) {
            caller->is_imm[vmap[v]] = true;
            caller->imm_val[vmap[v]] = callee->imm_val[v];
        }
    }
    int slot_base = caller->nslots;
    for (int s = 0; s < callee->nslots; s++) ir_new_slot(caller, callee->slots[s].size, callee->slots[s].align);

    /* Labels: each gets a fresh number. */
    int lmin = 0x7fffffff, lmax = -1;
    for (size_t i = 0; i < callee->n; i++) {
        const Ins *in = &callee->code[i];
        if (in->op == I_LABEL || in->op == I_JMP || in->op == I_CBR) {
            int ls[2] = {in->l1, in->op == I_CBR ? in->l2 : in->l1};
            for (int k = 0; k < 2; k++) {
                if (ls[k] < lmin) lmin = ls[k];
                if (ls[k] > lmax) lmax = ls[k];
            }
        }
    }
    size_t nl = lmax >= lmin ? (size_t)(lmax - lmin + 1) : 1;
    int *lmap = xmalloc(nl * sizeof *lmap);
    for (size_t k = 0; k < nl; k++) lmap[k] = -1;
#define LABEL(l) (lmap[(l) - lmin] >= 0 ? lmap[(l) - lmin] : (lmap[(l) - lmin] = new_label(caller->g)))

    /* Arguments into the parameters. */
    for (size_t p = 0; p < callee->nparams && p < call->call->n; p++) {
        const ParamPlace *pp = &callee->params[p];
        int arg = call->call->args[p];
        if (pp->vreg >= 0) {
            Ins mv = blank(I_MOV);
            mv.d = vmap[pp->vreg];
            mv.a = arg;
            push(out, mv);
        } else if (pp->slot >= 0) {
            Ins st = blank(I_STORE);
            st.slot = slot_base + pp->slot;
            st.b = arg;
            st.sub = (int)pp->type->size;
            push(out, st);
        }
    }

    int end = new_label(caller->g);
    for (size_t i = 0; i < callee->n; i++) {
        const Ins *src = &callee->code[i];
        if (src->op == I_NOP || src->op == I_PARAMS) continue;
        if (src->op == I_RET) {   /* the result, then on past the inlined code */
            if (call->d >= 0 && src->a >= 0) {
                Ins mv = blank(I_MOV);
                mv.d = call->d;
                mv.a = vmap[src->a];
                push(out, mv);
            }
            Ins j = blank(I_JMP);
            j.l1 = end;
            push(out, j);
            continue;
        }
        Ins in = *src;
        if (in.d >= 0) in.d = vmap[in.d];
        if (in.a >= 0) in.a = vmap[in.a];
        if (in.b >= 0) in.b = vmap[in.b];
        if (in.idx >= 0) in.idx = vmap[in.idx];
        if (in.slot >= 0) in.slot += slot_base;
        if (in.op == I_LABEL || in.op == I_JMP || in.op == I_CBR) in.l1 = LABEL(in.l1);
        if (in.op == I_CBR) in.l2 = LABEL(in.l2);
        if (in.call) {
            IrCall *c = xmalloc(sizeof *c);
            *c = *src->call;
            c->args = xmalloc((c->n ? c->n : 1) * sizeof *c->args);
            c->types = xmalloc((c->n ? c->n : 1) * sizeof *c->types);
            for (size_t k = 0; k < c->n; k++) {
                c->args[k] = vmap[src->call->args[k]];
                c->types[k] = src->call->types[k];
            }
            if (c->ret_slot >= 0) c->ret_slot += slot_base;
            in.call = c;
        }
        push(out, in);
    }
    Ins l = blank(I_LABEL);
    l.l1 = end;
    push(out, l);
#undef LABEL
    free(vmap);
    free(lmap);
}

static void free_call(Ins *in) {
    free(in->call->args);
    free(in->call->types);
    free(in->call);
    in->call = NULL;
}

/* Inlines what qualifies into f; true if anything was. */
static bool inline_into(Inliner *il, Fn *f) {
    bool any = false;
    Code out = {0};
    for (size_t i = 0; i < f->n; i++) {
        Ins *in = &f->code[i];
        Fn *callee = NULL;
        if (in->op == I_CALL && !in->call->vslot && f->n + out.n < CALLER_MAX) callee = find_fn(il, in->call->name);
        if (callee && inlinable(callee, f)) {
            expand(f, in, callee, &out);
            free_call(in);
            any = true;
            continue;
        }
        push(&out, *in);
    }
    if (any) {
        free(f->code);
        f->code = out.code;
        f->n = out.n;
        f->cap = out.cap;
    } else {
        free(out.code);
    }
    return any;
}

static void visit(Inliner *il, size_t k) {
    Fn *f = il->fns[k];
    if (!f || il->state[k]) return;
    il->state[k] = 1;
    for (size_t i = 0; i < f->n; i++) {   /* callees first */
        const Ins *in = &f->code[i];
        if (in->op != I_CALL || in->call->vslot) continue;
        for (size_t j = 0; j < il->n; j++)
            if (il->fns[j] && il->state[j] == 0 && strcmp(il->fns[j]->fn->asm_name, in->call->name) == 0) visit(il, j);
    }
    if (inline_into(il, f)) ir_optimize(f);
    il->state[k] = 2;
}

void ir_inline_all(Fn **fns, size_t n) {
    Inliner il = {fns, n, calloc(n + 1, sizeof(int))};
    for (size_t k = 0; k < n; k++) visit(&il, k);
    free(il.state);
}
