#include <string.h>

#include "opt_ir.h"

/*
 * Passes over the IR, then register allocation:
 *
 *   - unreachable code is dropped
 *   - int constants that fit in 32 bits become immediate operands, and
 *     float constants memory operands (a constant pool after the function)
 *   - `t = <op>; x = t` becomes `x = <op>` (forwarding)
 *   - constants fold, and simple algebra simplifies (fold)
 *   - over extended blocks: copies propagate, and branches the path has
 *     already decided become jumps (bounds and null checks among them)
 *   - pure instructions whose results are unused are removed
 *
 * Allocation is linear scan over live intervals (from block-level liveness,
 * ignoring holes). Values live across a call (or inline asm) get a
 * callee-saved register (rbx, r12-r15) or are spilled; others prefer the
 * caller-saved r10/r11. Floats use xmm8-xmm15 and are spilled across calls.
 * rax, rcx, rdx, rsi, rdi, r8, r9 and xmm0-xmm7 are never allocated: the
 * emitter uses them as scratch and for passing arguments.
 */

/* Pointers to every vreg field an instruction reads. */
void ir_operand_ptrs(Ins *in, int **ptrs, int *n) {
    *n = 0;
    if (in->a >= 0) ptrs[(*n)++] = &in->a;
    if (in->b >= 0) ptrs[(*n)++] = &in->b;
    if (in->idx >= 0) ptrs[(*n)++] = &in->idx;
}

/* Calls and asm have lists of operands too. */
typedef struct {
    int **p;
    int n, cap;
} Uses;

static void uses_of(Ins *in, Uses *u) {
    u->n = 0;
    size_t extra = in->call ? in->call->n : in->as ? in->as->stmt->u.asm_.nops : 0;
    if ((size_t)u->cap < extra + 3) {
        u->cap = (int)(extra + 3) * 2;
        u->p = realloc(u->p, (size_t)u->cap * sizeof *u->p);
        if (!u->p) fatal("out of memory");
    }
    int n;
    ir_operand_ptrs(in, u->p, &n);
    u->n = n;
    if (in->op == I_CALL)
        for (size_t i = 0; i < in->call->n; i++) u->p[u->n++] = &in->call->args[i];
    if (in->op == I_ASM)
        for (size_t i = 0; i < in->as->stmt->u.asm_.nops; i++)
            if (in->as->ins[i] >= 0) u->p[u->n++] = &in->as->ins[i];
}

/* Calls f(v) for each vreg an instruction defines. */
#define FOR_DEFS(in, v, body)                                                         \
    do {                                                                              \
        if ((in)->d >= 0) {                                                           \
            int v = (in)->d;                                                          \
            body;                                                                     \
        }                                                                             \
        if ((in)->op == I_ASM)                                                        \
            for (size_t k_ = 0; k_ < (in)->as->stmt->u.asm_.nops; k_++)              \
                if ((in)->as->outs[k_] >= 0) {                                        \
                    int v = (in)->as->outs[k_];                                       \
                    body;                                                             \
                }                                                                     \
    } while (0)

static bool is_terminator(IrOp op) {
    return op == I_JMP || op == I_CBR || op == I_RET || op == I_PANIC;
}

/* ---- basic blocks ------------------------------------------------------------ */

typedef struct {
    size_t first, last;     /* instruction range, inclusive */
    int succ[2];
    int nsucc;
} Block;

typedef struct {
    Block *b;
    int n;
    int *of_label;          /* label -> block (labels are global numbers; offset by min) */
    int label_min, label_max;
} Cfg;

static void build_cfg(Fn *f, Cfg *c) {
    c->n = 0;
    c->b = xmalloc((f->n + 1) * sizeof *c->b);
    c->label_min = 0x7fffffff;
    c->label_max = -1;
    for (size_t i = 0; i < f->n; i++) {
        const Ins *in = &f->code[i];
        if (in->op == I_LABEL) {
            if (in->l1 < c->label_min) c->label_min = in->l1;
            if (in->l1 > c->label_max) c->label_max = in->l1;
        }
    }
    size_t nl = c->label_max >= c->label_min ? (size_t)(c->label_max - c->label_min + 1) : 1;
    c->of_label = xmalloc(nl * sizeof *c->of_label);
    for (size_t i = 0; i < f->n;) {
        Block *b = &c->b[c->n];
        b->first = i;
        size_t j = i;
        while (j + 1 < f->n && !is_terminator(f->code[j].op) && f->code[j + 1].op != I_LABEL) j++;
        b->last = j;
        for (size_t k = i; k <= j; k++)
            if (f->code[k].op == I_LABEL) c->of_label[f->code[k].l1 - c->label_min] = c->n;
        c->n++;
        i = j + 1;
    }
    for (int k = 0; k < c->n; k++) {
        Block *b = &c->b[k];
        const Ins *last = &f->code[b->last];
        b->nsucc = 0;
        if (last->op == I_JMP) {
            b->succ[b->nsucc++] = c->of_label[last->l1 - c->label_min];
        } else if (last->op == I_CBR) {
            b->succ[b->nsucc++] = c->of_label[last->l1 - c->label_min];
            b->succ[b->nsucc++] = c->of_label[last->l2 - c->label_min];
        } else if (last->op != I_RET && last->op != I_PANIC && k + 1 < c->n) {
            b->succ[b->nsucc++] = k + 1;
        }
    }
}

static void free_cfg(Cfg *c) {
    free(c->b);
    free(c->of_label);
}

static void compact(Fn *f) {
    size_t to = 0;
    for (size_t i = 0; i < f->n; i++)
        if (f->code[i].op != I_NOP) f->code[to++] = f->code[i];
    f->n = to;
}

/* ---- passes ------------------------------------------------------------------- */

static void drop_unreachable(Fn *f) {
    Cfg c;
    build_cfg(f, &c);
    bool *seen = calloc((size_t)c.n + 1, 1);
    int *work = xmalloc(((size_t)c.n + 1) * sizeof *work);
    int nw = 0;
    if (c.n) {
        seen[0] = true;
        work[nw++] = 0;
    }
    while (nw) {
        int k = work[--nw];
        for (int s = 0; s < c.b[k].nsucc; s++) {
            int t = c.b[k].succ[s];
            if (!seen[t]) {
                seen[t] = true;
                work[nw++] = t;
            }
        }
    }
    for (int k = 0; k < c.n; k++)
        if (!seen[k])
            for (size_t i = c.b[k].first; i <= c.b[k].last; i++) {
                Ins *in = &f->code[i];
                if (in->call) {
                    free(in->call->args);
                    free(in->call->types);
                    free(in->call);
                    in->call = NULL;
                }
                if (in->as) {
                    free(in->as->ins);
                    free(in->as->outs);
                    free(in->as->in_slots);
                    free(in->as->out_slots);
                    free(in->as);
                    in->as = NULL;
                }
                in->op = I_NOP;
            }
    free(seen);
    free(work);
    free_cfg(&c);
    compact(f);
}

static bool is_pure(const Ins *in) {
    switch (in->op) {
    case I_CONST: case I_FCONST: case I_MOV: case I_NEG: case I_NOT: case I_EXT: case I_FBIN: case I_FNEG:
    case I_CMP: case I_FCMP: case I_I2F: case I_U2F: case I_F2I: case I_F2U: case I_F2F: case I_LEA:
    case I_GOT: case I_STR:
        return true;
    case I_BIN:   /* division can trap */
        return in->sub != B_SDIV && in->sub != B_UDIV && in->sub != B_SREM && in->sub != B_UREM;
    default:
        return false;
    }
}

/* ---- constants ------------------------------------------------------------------ */

/* Grows the immediate tables to cover every vreg. */
void ir_ensure_imm(Fn *f) {
    if (f->imm_cap >= f->nv + 1) return;
    int cap = (f->nv + 1) * 2;
    f->is_imm = realloc(f->is_imm, (size_t)cap * sizeof *f->is_imm);
    f->imm_val = realloc(f->imm_val, (size_t)cap * sizeof *f->imm_val);
    if (!f->is_imm || !f->imm_val) fatal("out of memory");
    memset(f->is_imm + f->imm_cap, 0, (size_t)(cap - f->imm_cap) * sizeof *f->is_imm);
    memset(f->imm_val + f->imm_cap, 0, (size_t)(cap - f->imm_cap) * sizeof *f->imm_val);
    f->imm_cap = cap;
}

/* A new vreg that is the int32 constant v. */
static int new_imm(Fn *f, int64_t v) {
    int r = ir_new_vreg(f, VK_INT);
    ir_ensure_imm(f);
    f->is_imm[r] = true;
    f->imm_val[r] = v;
    return r;
}

static void mark_immediates(Fn *f, int *defs) {
    ir_ensure_imm(f);
    for (size_t i = 0; i < f->n; i++) {
        Ins *in = &f->code[i];
        if (in->op == I_CONST && defs[in->d] == 1 && in->imm >= INT32_MIN && in->imm <= INT32_MAX) {
            f->is_imm[in->d] = true;
            f->imm_val[in->d] = in->imm;
            in->op = I_NOP;
        } else if (in->op == I_FCONST && defs[in->d] == 1) {   /* read from a constant pool */
            f->is_imm[in->d] = true;
            f->imm_val[in->d] = in->imm;
            in->op = I_NOP;
        }
    }
}

/* `t = <op>; x = t` -> `x = <op>` when t is used only there. */
static bool forward(Fn *f, int *defs, int *uses) {
    bool changed = false;
    for (size_t i = 0; i + 1 < f->n; i++) {
        Ins *in = &f->code[i];
        if (in->op == I_NOP || in->d < 0) continue;
        int t = in->d;
        if (defs[t] != 1 || uses[t] != 1 || f->is_imm[t]) continue;
        size_t j = i + 1;
        while (j < f->n && f->code[j].op == I_NOP) j++;
        if (j >= f->n) continue;
        Ins *mv = &f->code[j];
        if (mv->op != I_MOV || mv->a != t || f->vk[mv->d] != f->vk[t]) continue;
        in->d = mv->d;
        mv->op = I_NOP;
        defs[t] = 0;
        uses[t] = 0;
        changed = true;
    }
    return changed;
}

typedef struct {
    int *defs, *uses;
    int *def_at;        /* single-def vregs: the defining instruction, else -1 */
    int cap;
    Uses u;
} Counts;

static void count(Fn *f, Counts *c) {
    if (c->cap < f->nv + 1) {
        c->cap = (f->nv + 1) * 2;
        c->defs = realloc(c->defs, (size_t)c->cap * sizeof *c->defs);
        c->uses = realloc(c->uses, (size_t)c->cap * sizeof *c->uses);
        c->def_at = realloc(c->def_at, (size_t)c->cap * sizeof *c->def_at);
        if (!c->defs || !c->uses || !c->def_at) fatal("out of memory");
    }
    memset(c->defs, 0, (size_t)f->nv * sizeof *c->defs);
    memset(c->uses, 0, (size_t)f->nv * sizeof *c->uses);
    for (size_t i = 0; i < f->n; i++) {
        Ins *in = &f->code[i];
        if (in->op == I_NOP) continue;
        FOR_DEFS(in, v, {
            c->defs[v]++;
            c->def_at[v] = (int)i;
        });
        if (in->op == I_PARAMS)   /* defines every parameter kept in a vreg */
            for (size_t p = 0; p < f->nparams; p++)
                if (f->params[p].vreg >= 0) {
                    c->defs[f->params[p].vreg]++;
                    c->def_at[f->params[p].vreg] = (int)i;
                }
        uses_of(in, &c->u);
        for (int k = 0; k < c->u.n; k++) c->uses[*c->u.p[k]]++;
    }
    for (int v = 0; v < f->nv; v++)
        if (c->defs[v] != 1) c->def_at[v] = -1;
}

/* Is v a known int constant? */
static bool cval(Fn *f, const Counts *c, int v, int64_t *out) {
    if (v < 0 || f->vk[v] != VK_INT) return false;
    if (f->is_imm[v]) {
        *out = f->imm_val[v];
        return true;
    }
    int at = c->def_at[v];
    if (at >= 0 && f->code[at].op == I_CONST) {
        *out = f->code[at].imm;
        return true;
    }
    return false;
}

static bool cc_holds(int cc, int64_t a, int64_t b) {
    uint64_t ua = (uint64_t)a, ub = (uint64_t)b;
    switch (cc) {
    case CC_EQ: return a == b;
    case CC_NE: return a != b;
    case CC_LT: return a < b;
    case CC_LE: return a <= b;
    case CC_GT: return a > b;
    case CC_GE: return a >= b;
    case CC_B: return ua < ub;
    case CC_BE: return ua <= ub;
    case CC_A: return ua > ub;
    default: return ua >= ub;
    }
}

static int invert_cc(int cc) {
    static const int inv[] = {CC_NE, CC_EQ, CC_GE, CC_GT, CC_LE, CC_LT, CC_AE, CC_A, CC_BE, CC_B};
    return inv[cc];
}

static int64_t extend(int64_t v, int size, bool sgn) {
    switch (size) {
    case 1: return sgn ? (int64_t)(int8_t)v : (int64_t)(uint8_t)v;
    case 2: return sgn ? (int64_t)(int16_t)v : (int64_t)(uint16_t)v;
    case 4: return sgn ? (int64_t)(int32_t)v : (int64_t)(uint32_t)v;
    default: return v;
    }
}

/* a <op> b on 64 bits, as the emitted code computes it; false if it would trap. */
static bool eval_bin(int op, int64_t a, int64_t b, int64_t *out) {
    uint64_t ua = (uint64_t)a, ub = (uint64_t)b;
    switch (op) {
    case B_ADD: *out = (int64_t)(ua + ub); return true;
    case B_SUB: *out = (int64_t)(ua - ub); return true;
    case B_MUL: *out = (int64_t)(ua * ub); return true;
    case B_AND: *out = a & b; return true;
    case B_OR: *out = a | b; return true;
    case B_XOR: *out = a ^ b; return true;
    case B_SHL: *out = (int64_t)(ua << (b & 63)); return true;
    case B_SHR: *out = (int64_t)(ua >> (b & 63)); return true;
    case B_SAR: *out = a < 0 ? (int64_t)~(~ua >> (b & 63)) : (int64_t)(ua >> (b & 63)); return true;
    case B_SDIV: case B_SREM:
        if (b == 0 || (a == INT64_MIN && b == -1)) return false;
        *out = op == B_SDIV ? a / b : a % b;
        return true;
    default:
        if (b == 0) return false;
        *out = (int64_t)(op == B_UDIV ? ua / ub : ua % ub);
        return true;
    }
}

static int log2_exact(int64_t v) {
    if (v <= 0 || (v & (v - 1))) return -1;
    int k = 0;
    while ((v >> k) != 1) k++;
    return k;
}

static void become_const(Ins *in, int64_t v) {
    in->op = I_CONST;
    in->a = in->b = in->idx = -1;
    in->imm = v;
}

static void become_mov(Ins *in, int from) {
    in->op = I_MOV;
    in->a = from;
    in->b = in->idx = -1;
}

/* Constant folding and algebraic simplification: `2 * 8` is 16, `x + 0`
 * is x, `x * 8` is `x << 3`, a branch on constants is a jump. */
static bool fold(Fn *f, Counts *c, bool *cfg_changed) {
    bool changed = false;
    for (size_t i = 0; i < f->n; i++) {
        Ins *in = &f->code[i];
        int64_t ka = 0, kb = 0;
        bool A, B;
        switch (in->op) {
        case I_BIN: {
            A = cval(f, c, in->a, &ka);
            B = cval(f, c, in->b, &kb);
            int64_t r;
            if (A && B) {
                if (!eval_bin(in->sub, ka, kb, &r)) break;
                become_const(in, r);
                changed = true;
                break;
            }
            bool comm = in->sub == B_ADD || in->sub == B_MUL || in->sub == B_AND || in->sub == B_OR || in->sub == B_XOR;
            if (A && comm) {   /* constant second */
                int t = in->a;
                in->a = in->b;
                in->b = t;
                kb = ka;
                B = true;
            }
            if (!B) break;
            int k = log2_exact(kb);
            switch (in->sub) {
            case B_ADD: case B_SUB: case B_OR: case B_XOR: case B_SHL: case B_SHR: case B_SAR:
                if ((in->sub >= B_SHL ? kb & 63 : kb) == 0) {
                    become_mov(in, in->a);
                    changed = true;
                } else if (in->sub == B_OR && kb == -1) {
                    become_const(in, -1);
                    changed = true;
                }
                break;
            case B_MUL:
                if (kb == 0) become_const(in, 0);
                else if (kb == 1) become_mov(in, in->a);
                else if (k > 0) {
                    in->sub = B_SHL;
                    in->b = new_imm(f, k);
                } else break;
                changed = true;
                break;
            case B_AND:
                if (kb == 0) become_const(in, 0);
                else if (kb == -1) become_mov(in, in->a);
                else break;
                changed = true;
                break;
            case B_UDIV:
            case B_SDIV:
                if (kb == 1) {
                    become_mov(in, in->a);
                    changed = true;
                } else if (in->sub == B_UDIV && k > 0) {
                    in->sub = B_SHR;
                    in->b = new_imm(f, k);
                    changed = true;
                }
                break;
            case B_UREM:
            case B_SREM:
                if (kb == 1 || (in->sub == B_SREM && kb == -1)) {
                    become_const(in, 0);
                    changed = true;
                } else if (in->sub == B_UREM && k > 0 && k <= 31) {
                    in->sub = B_AND;
                    in->b = new_imm(f, kb - 1);
                    changed = true;
                }
                break;
            }
            break;
        }
        case I_NEG:
        case I_NOT:
            if (cval(f, c, in->a, &ka)) {
                become_const(in, in->op == I_NEG ? (int64_t)(0 - (uint64_t)ka) : ~ka);
                changed = true;
            }
            break;
        case I_EXT:
            if (cval(f, c, in->a, &ka)) {
                become_const(in, extend(ka, in->sub, in->sgn));
                changed = true;
            }
            break;
        case I_CMP:
            A = cval(f, c, in->a, &ka);
            B = cval(f, c, in->b, &kb);
            if ((A && B) || in->a == in->b) {
                become_const(in, A && B ? cc_holds(in->sub, ka, kb) : cc_holds(in->sub, 0, 0));
                changed = true;
            }
            break;
        case I_CBR:
            A = cval(f, c, in->a, &ka);
            B = cval(f, c, in->b, &kb);
            if ((A && B) || (in->a == in->b && f->vk[in->a] == VK_INT)) {
                bool taken = A && B ? cc_holds(in->sub, ka, kb) : cc_holds(in->sub, 0, 0);
                in->op = I_JMP;
                in->l1 = taken ? in->l1 : in->l2;
                in->a = in->b = -1;
                changed = *cfg_changed = true;
            }
            break;
        case I_I2F:
            if (cval(f, c, in->a, &ka)) {   /* (float) 1200 is 1200.0 */
                in->op = I_FCONST;
                if (f->vk[in->d] == VK_F32) {
                    float x = (float)ka;
                    uint32_t bits;
                    memcpy(&bits, &x, sizeof bits);
                    in->imm = bits;
                } else {
                    double x = (double)ka;
                    memcpy(&in->imm, &x, sizeof x);
                }
                in->a = in->b = in->idx = -1;
                changed = true;
            }
            break;
        case I_MOV:
            /* A single-def copy of a constant is that constant (and may become an immediate). */
            if (c->defs[in->d] == 1 && cval(f, c, in->a, &ka)) {
                become_const(in, ka);
                changed = true;
            }
            break;
        default:
            break;
        }
    }
    return changed;
}

/* ---- extended blocks ---------------------------------------------------------------
 *
 * A block with exactly one predecessor runs only after it, so whatever was
 * true at the end of the predecessor (and, after a conditional branch, the
 * branch's condition) still holds at its start. Walking the tree of
 * single-predecessor blocks:
 *
 *   - copies: after `t = x`, uses of t read x, while neither changes;
 *   - branches: a comparison the path already decided is replaced by a
 *     jump, like a bounds check inside `for (i = 0; i < 16; i++)`.
 */

typedef struct {
    int a, cc, b;
    int va, vb;         /* versions of a and b when it was learned */
} Fact;

typedef struct {
    int *slot;
    int old;
} Undo;

typedef struct {
    Fn *f;
    Counts *c;
    Cfg cfg;
    int *npred;
    int *copy, *copy_ver;   /* per vreg: what it's a copy of (or -1), and that vreg's version then */
    int *ver;               /* per vreg: bumped at each definition */
    bool *nonneg;           /* per vreg: never negative as a signed value */
    Fact *facts;
    int nfacts, facts_cap;
    Undo *log;
    int nlog, log_cap;
    int zero;               /* a vreg that is the constant 0 */
    bool changed, cfg_changed;
} Ebb;

static void set_logged(Ebb *e, int *slot, int v) {
    if (e->nlog == e->log_cap) {
        e->log_cap = e->log_cap ? e->log_cap * 2 : 256;
        e->log = realloc(e->log, (size_t)e->log_cap * sizeof *e->log);
        if (!e->log) fatal("out of memory");
    }
    e->log[e->nlog].slot = slot;
    e->log[e->nlog].old = *slot;
    e->nlog++;
    *slot = v;
}

static int resolve(Ebb *e, int v) {
    int x = e->copy[v];
    if (x >= 0 && e->ver[x] == e->copy_ver[v]) return x;
    return v;
}

static bool fact_valid(Ebb *e, const Fact *ft) {
    return e->ver[ft->a] == ft->va && e->ver[ft->b] == ft->vb;
}

static void add_fact(Ebb *e, int a, int cc, int b) {
    if (e->nfacts == e->facts_cap) {
        e->facts_cap = e->facts_cap ? e->facts_cap * 2 : 64;
        e->facts = realloc(e->facts, (size_t)e->facts_cap * sizeof *e->facts);
        if (!e->facts) fatal("out of memory");
    }
    e->facts[e->nfacts++] = (Fact){a, cc, b, e->ver[a], e->ver[b]};
}

static int swap_cc(int cc) {
    static const int sw[] = {CC_EQ, CC_NE, CC_GT, CC_GE, CC_LT, CC_LE, CC_A, CC_AE, CC_B, CC_BE};
    return sw[cc];
}

/* Outcomes (1 <, 2 =, 4 >) a condition allows, and its order: 0 either, 1 signed, 2 unsigned. */
static int cc_set(int cc) {
    static const int set[] = {2, 5, 1, 3, 4, 6, 1, 3, 4, 6};
    return set[cc];
}

static int cc_domain(int cc) {
    return cc <= CC_NE ? 0 : cc <= CC_GE ? 1 : 2;
}

/* Does knowing (a fcc b) decide (a qcc b)? 1 true, 0 false, -1 unknown. */
static int implies(int fcc, int qcc) {
    int df = cc_domain(fcc), dq = cc_domain(qcc);
    if (df && dq && df != dq) return -1;
    if (fcc == CC_NE && dq != 0) return -1;   /* a != b says nothing about order */
    int F = cc_set(fcc), Q = cc_set(qcc);
    if ((F & Q) == F) return 1;
    if ((F & Q) == 0) return 0;
    return -1;
}

/* What the facts on the path say about (a cc k), k constant: from the
 * ranges they give a. */
static int decide_range(Ebb *e, int a, int cc, int64_t k) {
    int64_t slo = INT64_MIN, shi = INT64_MAX;
    uint64_t ulo = 0, uhi = UINT64_MAX;
    if (e->nonneg[a]) slo = 0;
    for (int i = 0; i < e->nfacts; i++) {
        Fact *ft = &e->facts[i];
        if (!fact_valid(e, ft)) continue;
        int fcc;
        int64_t kf;
        if (ft->a == a && cval(e->f, e->c, ft->b, &kf)) fcc = ft->cc;
        else if (ft->b == a && cval(e->f, e->c, ft->a, &kf)) fcc = swap_cc(ft->cc);
        else continue;
        uint64_t ukf = (uint64_t)kf;
        switch (fcc) {
        case CC_EQ:
            if (kf > slo) slo = kf;
            if (kf < shi) shi = kf;
            if (ukf > ulo) ulo = ukf;
            if (ukf < uhi) uhi = ukf;
            break;
        case CC_LT: if (kf != INT64_MIN && kf - 1 < shi) shi = kf - 1; break;
        case CC_LE: if (kf < shi) shi = kf; break;
        case CC_GT: if (kf != INT64_MAX && kf + 1 > slo) slo = kf + 1; break;
        case CC_GE: if (kf > slo) slo = kf; break;
        case CC_B: if (ukf != 0 && ukf - 1 < uhi) uhi = ukf - 1; break;
        case CC_BE: if (ukf < uhi) uhi = ukf; break;
        case CC_A: if (ukf != UINT64_MAX && ukf + 1 > ulo) ulo = ukf + 1; break;
        case CC_AE: if (ukf > ulo) ulo = ukf; break;
        default: break;
        }
    }
    /* Non-negative signed ranges are unsigned ranges too, and the reverse. */
    if (slo >= 0) {
        if ((uint64_t)slo > ulo) ulo = (uint64_t)slo;
        if ((uint64_t)shi < uhi) uhi = (uint64_t)shi;
    }
    if (uhi <= (uint64_t)INT64_MAX) {
        if ((int64_t)ulo > slo) slo = (int64_t)ulo;
        if ((int64_t)uhi < shi) shi = (int64_t)uhi;
    }
    if (slo > shi || ulo > uhi) return -1;   /* contradictory: an unreachable path */
    bool always, never;
    if (cc_domain(cc) == 2) {
        uint64_t uk = (uint64_t)k;
        switch (cc) {
        case CC_B: always = uhi < uk; never = ulo >= uk; break;
        case CC_BE: always = uhi <= uk; never = ulo > uk; break;
        case CC_A: always = ulo > uk; never = uhi <= uk; break;
        default: always = ulo >= uk; never = uhi < uk; break;
        }
    } else {
        switch (cc) {
        case CC_EQ: always = slo == k && shi == k; never = k < slo || k > shi; break;
        case CC_NE: always = k < slo || k > shi; never = slo == k && shi == k; break;
        case CC_LT: always = shi < k; never = slo >= k; break;
        case CC_LE: always = shi <= k; never = slo > k; break;
        case CC_GT: always = slo > k; never = shi <= k; break;
        default: always = slo >= k; never = shi < k; break;
        }
    }
    return always ? 1 : never ? 0 : -1;
}

/* The same vreg, or two vregs that are the same constant. */
static bool same_operand(Ebb *e, int x, int y) {
    int64_t kx, ky;
    return x == y || (cval(e->f, e->c, x, &kx) && cval(e->f, e->c, y, &ky) && kx == ky);
}

/* Is (a cc b) already known on this path? */
static int decide(Ebb *e, int a, int cc, int b) {
    Fn *f = e->f;
    if (f->vk[a] != VK_INT) return -1;
    int64_t ka, kb;
    bool A = cval(f, e->c, a, &ka), B = cval(f, e->c, b, &kb);
    if (A && B) return cc_holds(cc, ka, kb);
    if (a == b) return cc_holds(cc, 0, 0);
    for (int i = e->nfacts; i-- > 0;) {
        Fact *ft = &e->facts[i];
        if (!fact_valid(e, ft)) continue;
        int r = -1;
        if (same_operand(e, ft->a, a) && same_operand(e, ft->b, b)) r = implies(ft->cc, cc);
        else if (same_operand(e, ft->a, b) && same_operand(e, ft->b, a)) r = implies(swap_cc(ft->cc), cc);
        if (r >= 0) return r;
    }
    if (B) return decide_range(e, a, cc, kb);
    if (A) return decide_range(e, b, swap_cc(cc), ka);
    return -1;
}

static void walk_block(Ebb *e, int k) {
    Fn *f = e->f;
    Block *blk = &e->cfg.b[k];
    int saved_log = e->nlog, saved_facts = e->nfacts;
    Uses *u = &e->c->u;
    for (size_t i = blk->first; i <= blk->last; i++) {
        Ins *in = &f->code[i];
        if (in->op == I_NOP || in->op == I_LABEL) continue;
        uses_of(in, u);
        for (int q = 0; q < u->n; q++) {
            int r = resolve(e, *u->p[q]);
            if (r != *u->p[q] && f->vk[r] == f->vk[*u->p[q]]) {
                *u->p[q] = r;
                e->changed = true;
            }
        }
        if (in->op == I_CBR) {
            int r = decide(e, in->a, in->sub, in->b);
            if (r >= 0) {
                in->op = I_JMP;
                in->l1 = r ? in->l1 : in->l2;
                in->a = in->b = -1;
                e->changed = e->cfg_changed = true;
            }
        }
        if (in->op == I_MOV && in->d == in->a) {
            in->op = I_NOP;
            e->changed = true;
            continue;
        }
        FOR_DEFS(in, v, {
            set_logged(e, &e->copy[v], -1);
            set_logged(e, &e->ver[v], e->ver[v] + 1);
        });
        if (in->op == I_MOV && f->vk[in->d] == f->vk[in->a] && !f->is_imm[in->d]) {
            set_logged(e, &e->copy[in->d], in->a);
            set_logged(e, &e->copy_ver[in->d], e->ver[in->a]);
        }
        /* After a load or store through p near its start, p isn't null: if it
         * were, that access would have faulted (page 0 is never mapped,
         * except perhaps on bare metal). */
        if ((in->op == I_LOAD || in->op == I_STORE) && in->a >= 0 && in->idx < 0 && in->slot < 0 && !in->sym &&
            in->imm > -4096 && in->imm < 4096 && !f->g->freestanding && in->a != in->d)
            add_fact(e, in->a, CC_NE, e->zero);
    }

    const Ins *last = &f->code[blk->last];
    for (int s = 0; s < blk->nsucc; s++) {
        int t = blk->succ[s];
        if (t == k || t == 0 || e->npred[t] != 1) continue;
        int before = e->nfacts;
        if (last->op == I_CBR && last->l1 != last->l2) {
            int lt = e->cfg.of_label[last->l1 - e->cfg.label_min];
            add_fact(e, last->a, t == lt ? last->sub : invert_cc(last->sub), last->b);
        }
        walk_block(e, t);
        e->nfacts = before;
    }

    while (e->nlog > saved_log) {
        e->nlog--;
        *e->log[e->nlog].slot = e->log[e->nlog].old;
    }
    e->nfacts = saved_facts;
}

/* Vregs that can't hold a negative value: every definition is a
 * non-negative constant, a small unsigned load or extension, a comparison
 * result, a mask, a right shift, or adds a small constant to the vreg
 * itself (a counter going up from 0 would need 2^59 steps to wrap). */
static void find_nonneg(Fn *f, Counts *c, bool *nonneg) {
    for (int v = 0; v < f->nv; v++)
        nonneg[v] = f->vk[v] == VK_INT && (f->is_imm[v] ? f->imm_val[v] >= 0 : c->defs[v] > 0);
    for (size_t p = 0; p < f->nparams; p++)   /* set by PARAMS */
        if (f->params[p].vreg >= 0) nonneg[f->params[p].vreg] = false;
    for (size_t i = 0; i < f->n; i++) {
        Ins *in = &f->code[i];
        if (in->op == I_NOP) continue;
        int64_t k;
        FOR_DEFS(in, v, {
            bool ok;
            switch (in->op) {
            case I_CONST: ok = in->imm >= 0; break;
            case I_CMP: case I_FCMP: ok = true; break;
            case I_EXT: ok = !in->sgn && in->sub < 8; break;
            case I_LOAD: ok = !in->sgn && in->sub < 8; break;
            case I_MOV: ok = cval(f, c, in->a, &k) && k >= 0; break;
            case I_BIN:
                ok = in->sub == B_ADD &&
                     ((in->a == v && cval(f, c, in->b, &k)) || (in->b == v && cval(f, c, in->a, &k))) && k >= 0 && k <= 16;
                if (in->sub == B_AND && ((cval(f, c, in->b, &k) && k >= 0) || (cval(f, c, in->a, &k) && k >= 0))) ok = true;
                if (in->sub == B_SHR && cval(f, c, in->b, &k) && (k & 63) > 0) ok = true;
                break;
            default: ok = false; break;
            }
            if (!ok) nonneg[v] = false;
        });
    }
}

static bool ebb_pass(Fn *f, Counts *c, bool *cfg_changed) {
    Ebb e = {0};
    e.f = f;
    e.c = c;
    e.zero = new_imm(f, 0);
    build_cfg(f, &e.cfg);
    e.npred = calloc((size_t)e.cfg.n + 1, sizeof *e.npred);
    for (int k = 0; k < e.cfg.n; k++)
        for (int s = 0; s < e.cfg.b[k].nsucc; s++) e.npred[e.cfg.b[k].succ[s]]++;
    size_t nv = (size_t)f->nv + 1;
    e.copy = xmalloc(nv * sizeof *e.copy);
    e.copy_ver = calloc(nv, sizeof *e.copy_ver);
    e.ver = calloc(nv, sizeof *e.ver);
    e.nonneg = calloc(nv, sizeof *e.nonneg);
    for (size_t v = 0; v < nv; v++) e.copy[v] = -1;
    find_nonneg(f, c, e.nonneg);
    for (int k = 0; k < e.cfg.n; k++)
        if (k == 0 || e.npred[k] != 1) walk_block(&e, k);
    free(e.npred);
    free(e.copy);
    free(e.copy_ver);
    free(e.ver);
    free(e.nonneg);
    free(e.facts);
    free(e.log);
    free_cfg(&e.cfg);
    *cfg_changed |= e.cfg_changed;
    return e.changed;
}

static bool eliminate_dead(Fn *f, Counts *c) {
    bool changed = false;
    for (size_t i = f->n; i-- > 0;) {
        Ins *in = &f->code[i];
        if (in->op == I_NOP || in->d < 0 || c->uses[in->d] != 0 || !is_pure(in)) continue;
        uses_of(in, &c->u);
        for (int k = 0; k < c->u.n; k++) c->uses[*c->u.p[k]]--;
        in->op = I_NOP;
        changed = true;
    }
    return changed;
}

void ir_optimize(Fn *f) {
    ir_ensure_imm(f);
    Counts c = {0};
    drop_unreachable(f);
    count(f, &c);
    mark_immediates(f, c.defs);
    for (int round = 0; round < 8; round++) {
        bool cfg_changed = false;
        count(f, &c);
        bool changed = forward(f, c.defs, c.uses);
        count(f, &c);
        changed |= fold(f, &c, &cfg_changed);
        count(f, &c);
        changed |= ebb_pass(f, &c, &cfg_changed);
        if (cfg_changed) drop_unreachable(f);
        count(f, &c);
        changed |= eliminate_dead(f, &c);
        compact(f);
        count(f, &c);
        mark_immediates(f, c.defs);
        if (!changed) break;
    }
    free(c.defs);
    free(c.uses);
    free(c.def_at);
    free(c.u.p);
}

/* ---- liveness ------------------------------------------------------------------- */

typedef uint64_t Word;

static bool bit(const Word *s, int v) {
    return (s[v >> 6] >> (v & 63)) & 1;
}

static void set_bit(Word *s, int v) {
    s[v >> 6] |= (Word)1 << (v & 63);
}

/* Live intervals [start, end] in positions: instruction i reads at 2i and
 * writes at 2i + 1. crosses[v]: v is live across a call or asm block (live
 * after it and not defined by it), so it needs a callee-saved register. */
static void intervals(Fn *f, int *start, int *end, bool *crosses) {
    Cfg c;
    build_cfg(f, &c);
    size_t words = ((size_t)f->nv + 63) / 64;
    Word *use = calloc((size_t)c.n * words + 1, sizeof(Word));
    Word *def = calloc((size_t)c.n * words + 1, sizeof(Word));
    Word *in = calloc((size_t)c.n * words + 1, sizeof(Word));
    Word *out = calloc((size_t)c.n * words + 1, sizeof(Word));
    if (!use || !def || !in || !out) fatal("out of memory");
    Uses u = {0};
    for (int k = 0; k < c.n; k++) {
        Word *bu = use + (size_t)k * words, *bd = def + (size_t)k * words;
        for (size_t i = c.b[k].first; i <= c.b[k].last; i++) {
            Ins *ins = &f->code[i];
            uses_of(ins, &u);
            for (int q = 0; q < u.n; q++) {
                int v = *u.p[q];
                if (!f->is_imm[v] && !bit(bd, v)) set_bit(bu, v);
            }
            FOR_DEFS(ins, v, set_bit(bd, v));
        }
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (int k = c.n - 1; k >= 0; k--) {
            Word *bo = out + (size_t)k * words, *bi = in + (size_t)k * words;
            for (int s = 0; s < c.b[k].nsucc; s++) {
                const Word *si = in + (size_t)c.b[k].succ[s] * words;
                for (size_t w = 0; w < words; w++) bo[w] |= si[w];
            }
            const Word *bu = use + (size_t)k * words, *bd = def + (size_t)k * words;
            for (size_t w = 0; w < words; w++) {
                Word now = bu[w] | (bo[w] & ~bd[w]);
                if (now != bi[w]) {
                    bi[w] = now;
                    changed = true;
                }
            }
        }
    }
    Word *live = calloc(words + 1, sizeof(Word));
    memset(crosses, 0, (size_t)f->nv * sizeof *crosses);
    for (int k = 0; k < c.n; k++) {
        memcpy(live, out + (size_t)k * words, words * sizeof(Word));
        for (size_t i = c.b[k].last + 1; i-- > c.b[k].first;) {
            Ins *ins = &f->code[i];
            FOR_DEFS(ins, v, live[v >> 6] &= ~((Word)1 << (v & 63)));
            if (ins->op == I_CALL || ins->op == I_ASM)
                for (size_t w = 0; w < words; w++)
                    for (Word m = live[w]; m; m &= m - 1) crosses[w * 64 + (size_t)__builtin_ctzll(m)] = true;
            uses_of(ins, &u);
            for (int q = 0; q < u.n; q++)
                if (!f->is_imm[*u.p[q]]) set_bit(live, *u.p[q]);
        }
    }
    free(live);
    for (int v = 0; v < f->nv; v++) {
        start[v] = 0x7fffffff;
        end[v] = -1;
    }
#define EXTEND(v, p)                        \
    do {                                    \
        if ((p) < start[v]) start[v] = (p); \
        if ((p) > end[v]) end[v] = (p);     \
    } while (0)
    for (int k = 0; k < c.n; k++) {
        const Word *bi = in + (size_t)k * words, *bo = out + (size_t)k * words;
        for (int v = 0; v < f->nv; v++) {
            if (bit(bi, v)) EXTEND(v, (int)(2 * c.b[k].first));
            if (bit(bo, v)) EXTEND(v, (int)(2 * c.b[k].last + 1));
        }
    }
    for (size_t i = 0; i < f->n; i++) {
        Ins *ins = &f->code[i];
        uses_of(ins, &u);
        for (int q = 0; q < u.n; q++)
            if (!f->is_imm[*u.p[q]]) EXTEND(*u.p[q], (int)(2 * i));
        FOR_DEFS(ins, v, EXTEND(v, (int)(2 * i + 1)));
    }
#undef EXTEND
    free(u.p);
    free(use);
    free(def);
    free(in);
    free(out);
    free_cfg(&c);
}

/* ---- linear scan --------------------------------------------------------------------- */

static const int callee_saved[] = {3, 12, 13, 14, 15};   /* rbx, r12-r15 */
static const int caller_saved[] = {10, 11};              /* r10, r11 */

static int *sort_start;
static int by_start(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return sort_start[a] != sort_start[b] ? (sort_start[a] < sort_start[b] ? -1 : 1) : a - b;
}

void ir_allocate(Fn *f) {
    int nv = f->nv;
    int *start = xmalloc(((size_t)nv + 1) * sizeof *start);
    int *end = xmalloc(((size_t)nv + 1) * sizeof *end);
    bool *crosses_call = xmalloc(((size_t)nv + 1) * sizeof *crosses_call);
    intervals(f, start, end, crosses_call);
    f->loc = xmalloc(((size_t)nv + 1) * sizeof *f->loc);

    int *order = xmalloc(((size_t)nv + 1) * sizeof *order);
    int norder = 0;
    for (int v = 0; v < nv; v++) {
        f->loc[v] = 0x7fffffff;   /* unassigned */
        if (end[v] >= 0 && !f->is_imm[v]) order[norder++] = v;
    }
    sort_start = start;
    qsort(order, (size_t)norder, sizeof *order, by_start);

    /* Hints: a value computed from a vreg that dies there would like its register. */
    int *hint = xmalloc(((size_t)nv + 1) * sizeof *hint);
    for (int v = 0; v < nv; v++) hint[v] = -1;
    for (size_t i = 0; i < f->n; i++) {
        const Ins *in = &f->code[i];
        if (in->d < 0 || in->a < 0) continue;
        if (in->op == I_MOV || in->op == I_BIN || in->op == I_NEG || in->op == I_NOT || in->op == I_EXT ||
            in->op == I_FBIN || in->op == I_FNEG)
            if (!f->is_imm[in->a] && f->vk[in->a] == f->vk[in->d]) hint[in->d] = in->a;
    }

    int owner[32];   /* register -> vreg holding it, or -1 */
    for (int r = 0; r < 32; r++) owner[r] = -1;
    memset(f->used_reg, 0, sizeof f->used_reg);
    int spills = 0;

    for (int oi = 0; oi < norder; oi++) {
        int v = order[oi];
        for (int r = 0; r < 32; r++)   /* expire */
            if (owner[r] >= 0 && end[owner[r]] < start[v]) owner[r] = -1;
        bool crosses = crosses_call[v];

        int cand[8], nc = 0;
        if (f->vk[v] == VK_INT) {
            if (!crosses)
                for (size_t k = 0; k < sizeof caller_saved / sizeof *caller_saved; k++) cand[nc++] = caller_saved[k];
            for (size_t k = 0; k < sizeof callee_saved / sizeof *callee_saved; k++) cand[nc++] = callee_saved[k];
        } else if (!crosses) {
            for (int k = 0; k < 8; k++) cand[nc++] = 24 + k;   /* xmm8-xmm15 */
        }
        int got = -1;
        if (hint[v] >= 0 && f->loc[hint[v]] >= 0 && f->loc[hint[v]] != 0x7fffffff && owner[f->loc[hint[v]]] < 0)
            for (int k = 0; k < nc && got < 0; k++)
                if (cand[k] == f->loc[hint[v]]) got = cand[k];
        for (int k = 0; k < nc && got < 0; k++)
            if (owner[cand[k]] < 0) got = cand[k];
        if (got < 0) {   /* steal from the candidate that's live longest, if that's longer than v */
            int victim = -1;
            for (int k = 0; k < nc; k++)
                if (victim < 0 || end[owner[cand[k]]] > end[owner[victim]]) victim = cand[k];
            if (victim >= 0 && end[owner[victim]] > end[v]) {
                f->loc[owner[victim]] = -(ir_new_slot(f, 8, 8) + 1);
                spills++;
                got = victim;
            }
        }
        if (got >= 0) {
            owner[got] = v;
            f->loc[v] = got;
            f->used_reg[got] = true;
        } else {
            f->loc[v] = -(ir_new_slot(f, 8, 8) + 1);
            spills++;
        }
    }
    (void)spills;
    free(hint);
    free(order);
    free(crosses_call);
    free(start);
    free(end);
}
