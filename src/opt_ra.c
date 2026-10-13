#include <string.h>

#include "opt_ir.h"

/*
 * Cleanup passes over the IR, then register allocation:
 *
 *   - unreachable code is dropped
 *   - int constants that fit in 32 bits become immediate operands
 *   - `t = <op>; x = t` becomes `x = <op>` (forwarding)
 *   - copies are propagated inside basic blocks
 *   - pure instructions whose results are unused are removed
 *
 * Allocation is linear scan over live intervals (from block-level liveness,
 * ignoring holes). Intervals that span a call (or inline asm) get a
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

static void count(Fn *f, int *defs, int *uses, Uses *u) {
    memset(defs, 0, (size_t)f->nv * sizeof *defs);
    memset(uses, 0, (size_t)f->nv * sizeof *uses);
    for (size_t i = 0; i < f->n; i++) {
        Ins *in = &f->code[i];
        if (in->op == I_NOP) continue;
        FOR_DEFS(in, v, defs[v]++);
        uses_of(in, u);
        for (int k = 0; k < u->n; k++) uses[*u->p[k]]++;
    }
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

static bool defines(Ins *in, int x) {
    bool hit = false;
    FOR_DEFS(in, v, hit |= v == x);
    return hit;
}

static void mark_immediates(Fn *f, int *defs) {
    for (size_t i = 0; i < f->n; i++) {
        Ins *in = &f->code[i];
        if (in->op == I_CONST && defs[in->d] == 1 && in->imm >= INT32_MIN && in->imm <= INT32_MAX) {
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

/* Inside a block, uses of t after `t = x` read x directly while x is unchanged. */
static bool propagate(Fn *f, int *defs, int *uses, Uses *u) {
    bool changed = false;
    for (size_t i = 0; i < f->n; i++) {
        Ins *mv = &f->code[i];
        if (mv->op != I_MOV) continue;
        int t = mv->d, x = mv->a;
        if (t == x) {
            mv->op = I_NOP;
            changed = true;
            continue;
        }
        if (defs[t] != 1 || f->vk[t] != f->vk[x]) continue;
        int replaced = 0;
        for (size_t k = i + 1; k < f->n; k++) {
            Ins *in = &f->code[k];
            if (in->op == I_LABEL) break;
            uses_of(in, u);
            for (int q = 0; q < u->n; q++)
                if (*u->p[q] == t) {
                    *u->p[q] = x;
                    replaced++;
                }
            if (is_terminator(in->op) || defines(in, x)) break;
        }
        if (replaced) {
            changed = true;
            uses[t] -= replaced;
            if (!f->is_imm[x]) uses[x] += replaced;
        }
        if (uses[t] == 0) mv->op = I_NOP;
    }
    return changed;
}

static bool eliminate_dead(Fn *f, int *uses, Uses *u) {
    bool changed = false;
    for (size_t i = f->n; i-- > 0;) {
        Ins *in = &f->code[i];
        if (in->op == I_NOP || in->d < 0 || uses[in->d] != 0 || !is_pure(in)) continue;
        uses_of(in, u);
        for (int k = 0; k < u->n; k++) uses[*u->p[k]]--;
        in->op = I_NOP;
        changed = true;
    }
    return changed;
}

void ir_optimize(Fn *f) {
    f->is_imm = calloc((size_t)f->nv + 1, sizeof *f->is_imm);
    f->imm_val = calloc((size_t)f->nv + 1, sizeof *f->imm_val);
    int *defs = xmalloc(((size_t)f->nv + 1) * sizeof *defs);
    int *uses = xmalloc(((size_t)f->nv + 1) * sizeof *uses);
    Uses u = {0};
    drop_unreachable(f);
    count(f, defs, uses, &u);
    mark_immediates(f, defs);
    for (int round = 0; round < 4; round++) {
        count(f, defs, uses, &u);
        bool changed = forward(f, defs, uses);
        count(f, defs, uses, &u);
        changed |= propagate(f, defs, uses, &u);
        count(f, defs, uses, &u);
        changed |= eliminate_dead(f, uses, &u);
        compact(f);
        if (!changed) break;
    }
    free(defs);
    free(uses);
    free(u.p);
}

/* ---- liveness ------------------------------------------------------------------- */

typedef uint64_t Word;

static bool bit(const Word *s, int v) {
    return (s[v >> 6] >> (v & 63)) & 1;
}

static void set_bit(Word *s, int v) {
    s[v >> 6] |= (Word)1 << (v & 63);
}

/* Live intervals [start, end] in positions: instruction i reads at 2i and writes at 2i + 1. */
static void intervals(Fn *f, int *start, int *end) {
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
    intervals(f, start, end);
    f->loc = xmalloc(((size_t)nv + 1) * sizeof *f->loc);

    /* Positions of calls (and asm), which clobber caller-saved registers. */
    int *calls = xmalloc((f->n + 1) * sizeof *calls);
    int ncalls = 0;
    for (size_t i = 0; i < f->n; i++)
        if (f->code[i].op == I_CALL || f->code[i].op == I_ASM) calls[ncalls++] = (int)(2 * i);

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
        bool crosses = false;
        int lo = 0, hi = ncalls;   /* first call at or after start */
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (calls[mid] < start[v]) lo = mid + 1;
            else hi = mid;
        }
        if (lo < ncalls && calls[lo] + 2 <= end[v]) crosses = true;

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
    free(calls);
    free(start);
    free(end);
}
