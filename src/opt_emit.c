#include <stdlib.h>
#include <string.h>

#include "opt_ir.h"

/*
 * IR -> x86-64, after register allocation. A vreg is in a register
 * (rbx, r10-r15, xmm8-xmm15), in a spill slot, or an immediate. The
 * emitter's scratch registers are rax, rcx, rdx (values and addresses),
 * rsi, rdi (copies) and xmm0, xmm1; argument registers are only touched
 * while a call is being set up, so moving values into them never
 * conflicts with anything.
 *
 * Frame:  [rbp + 16 + k]   stack parameters
 *         [rbp]            caller's rbp
 *         [rbp - 8 * n]    saved callee-saved registers
 *         below            slots (locals, temporaries, spills)
 */

static const char *const R64[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                   "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
static const char *const R32[16] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
                                   "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"};
static const char *const R16[16] = {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di",
                                   "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"};
static const char *const R8[16] = {"al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil",
                                  "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"};
enum { RAX = 0, RCX = 1, RDX = 2, RSI = 6, RDI = 7 };
static const int ARG_GPR[6] = {7, 6, 2, 1, 8, 9};   /* rdi, rsi, rdx, rcx, r8, r9 */

static const char *rname(int r, uint32_t size) {
    switch (size) {
    case 1: return R8[r];
    case 2: return R16[r];
    case 4: return R32[r];
    default: return R64[r];
    }
}

typedef struct {
    Fn *f;
    Gen *g;
    int serial;             /* numbers this function's constant pool labels */
    int *uses;              /* per vreg: how many instructions read it */
    size_t skip;            /* an instruction already emitted as part of the one before */
    int nsaved;
    int saved[5];
    char buf[8][128];
    int nbuf;
} Em;

static char *tmpbuf(Em *e) {
    return e->buf[e->nbuf++ & 7];
}

static bool is_imm(Em *e, int v) {
    return e->f->is_imm[v];
}

static int64_t imm(Em *e, int v) {
    return e->f->imm_val[v];
}

/* A GPR (0-15), an xmm register (16-31), or -1 for a spill slot. */
static int reg_of(Em *e, int v) {
    if (is_imm(e, v)) return -1;
    return e->f->loc[v] >= 0 ? e->f->loc[v] : -1;
}

static int spill_off(Em *e, int v) {
    return e->f->slots[-e->f->loc[v] - 1].off;
}

/* "rbx", "qword ptr [rbp - 24]" or "42": v as an operand of `size` bytes. */
static const char *opnd(Em *e, int v, uint32_t size) {
    char *b = tmpbuf(e);
    if (is_imm(e, v) && e->f->vk[v] != VK_INT)
        snprintf(b, 128, "%s ptr [rip + .LF%d_%d]", e->f->vk[v] == VK_F32 ? "dword" : "qword", e->serial, v);
    else if (is_imm(e, v)) snprintf(b, 128, "%lld", (long long)imm(e, v));
    else if (reg_of(e, v) >= 16) snprintf(b, 128, "xmm%d", reg_of(e, v) - 16);
    else if (reg_of(e, v) >= 0) snprintf(b, 128, "%s", rname(reg_of(e, v), size));
    else snprintf(b, 128, "%s ptr [rbp - %d]", size_ptr(size), spill_off(e, v));
    return b;
}

static bool is_mem(Em *e, int v) {
    return !is_imm(e, v) && reg_of(e, v) < 0;
}

static void load_int(Em *e, int r, int v) {
    if (is_imm(e, v)) {
        if (imm(e, v) == 0) emit(e->g, "xor %s, %s", R32[r], R32[r]);
        else emit(e->g, "mov %s, %lld", R64[r], (long long)imm(e, v));
    } else if (reg_of(e, v) != r) {
        emit(e->g, "mov %s, %s", R64[r], opnd(e, v, 8));
    }
}

static void store_int(Em *e, int v, int r) {
    if (reg_of(e, v) == r) return;
    emit(e->g, "mov %s, %s", opnd(e, v, 8), R64[r]);
}

/* The register to compute v's new value in: its own, or `scratch`. */
static int target(Em *e, int v, int scratch) {
    int r = reg_of(e, v);
    return r >= 0 && r < 16 ? r : scratch;
}

static bool f32(Em *e, int v) {
    return e->f->vk[v] == VK_F32;
}

/* Loads float v into xmm x (0-15). */
static void load_f(Em *e, int x, int v) {
    int r = reg_of(e, v);
    if (r == 16 + x) return;
    if (is_imm(e, v) && imm(e, v) == 0) {   /* +0.0 */
        emit(e->g, "xorps xmm%d, xmm%d", x, x);
        return;
    }
    if (r >= 16) emit(e->g, "movaps xmm%d, xmm%d", x, r - 16);
    else emit(e->g, "%s xmm%d, %s", f32(e, v) ? "movss" : "movsd", x, opnd(e, v, f32(e, v) ? 4 : 8));
}

static void store_f(Em *e, int v, int x) {
    int r = reg_of(e, v);
    if (r == 16 + x) return;
    if (r >= 16) emit(e->g, "movaps xmm%d, xmm%d", r - 16, x);
    else emit(e->g, "%s %s, xmm%d", f32(e, v) ? "movss" : "movsd", opnd(e, v, f32(e, v) ? 4 : 8), x);
}

static int ftarget(Em *e, int v, int scratch) {
    int r = reg_of(e, v);
    return r >= 16 ? r - 16 : scratch;
}

/* The address of a LOAD/STORE/LEA, using base_s/idx_s as scratch when the
 * base or index vreg is spilled. */
static const char *address(Em *e, const Ins *in, int base_s, int idx_s) {
    char *b = tmpbuf(e);
    int64_t disp = in->imm;
    char base[24] = "", index[32] = "";
    if (in->slot >= 0) {
        const Slot *s = &e->f->slots[in->slot];
        snprintf(base, sizeof base, "rbp");
        disp += s->incoming ? s->off : -s->off;
    } else if (in->sym) {
        if (disp) snprintf(b, 128, "[rip + %s%+lld]", in->sym, (long long)disp);
        else snprintf(b, 128, "[rip + %s]", in->sym);
        return b;
    } else if (in->a >= 0) {
        if (is_imm(e, in->a)) {
            disp += imm(e, in->a);
        } else {
            int r = reg_of(e, in->a);
            if (r < 0) {
                emit(e->g, "mov %s, %s", R64[base_s], opnd(e, in->a, 8));
                r = base_s;
            }
            snprintf(base, sizeof base, "%s", R64[r]);
        }
    }
    if (in->idx >= 0) {
        if (is_imm(e, in->idx)) {
            disp += imm(e, in->idx) * in->scale;
        } else {
            int r = reg_of(e, in->idx);
            if (r < 0) {
                emit(e->g, "mov %s, %s", R64[idx_s], opnd(e, in->idx, 8));
                r = idx_s;
            }
            snprintf(index, sizeof index, "%s*%d", R64[r], in->scale);
        }
    }
    int n = snprintf(b, 128, "[");
    if (base[0]) n += snprintf(b + n, 128 - (size_t)n, "%s", base);
    if (index[0]) n += snprintf(b + n, 128 - (size_t)n, "%s%s", base[0] ? " + " : "", index);
    if (disp || (!base[0] && !index[0]))
        n += snprintf(b + n, 128 - (size_t)n, base[0] || index[0] ? " %c %lld" : "%c%lld", disp < 0 ? '-' : '+',
                      (long long)(disp < 0 ? -disp : disp));
    snprintf(b + n, 128 - (size_t)n, "]");
    if (!base[0] && !index[0]) snprintf(b, 128, "[%lld]", (long long)disp);
    return b;
}

static const char *cc_suffix(int cc) {
    static const char *const s[] = {"e", "ne", "l", "le", "g", "ge", "b", "be", "a", "ae"};
    return s[cc];
}

static int invert(int cc) {
    static const int inv[] = {CC_NE, CC_EQ, CC_GE, CC_GT, CC_LE, CC_LT, CC_AE, CC_A, CC_BE, CC_B};
    return inv[cc];
}

/* cmp a, b for ints: one side must be a register when both would be memory. */
static void emit_cmp(Em *e, int a, int b) {
    const char *A;
    if (is_imm(e, a) || (is_mem(e, a) && is_mem(e, b))) {
        load_int(e, RAX, a);
        A = "rax";
    } else {
        A = opnd(e, a, 8);
    }
    if (is_imm(e, b) && imm(e, b) == 0 && reg_of(e, a) >= 0 && reg_of(e, a) < 16)
        emit(e->g, "test %s, %s", A, A);
    else
        emit(e->g, "cmp %s, %s", A, opnd(e, b, 8));
}

static bool next_is_label(Em *e, size_t i, int l) {
    return i + 1 < e->f->n && e->f->code[i + 1].op == I_LABEL && e->f->code[i + 1].l1 == l;
}

/* Copies n bytes from [rsi] to [rdi] (both already set). */
static void copy_bytes(Em *e, uint32_t n) {
    if (n > 64) {
        emit(e->g, "mov ecx, %u", n);
        emit(e->g, "rep movsb");
        return;
    }
    for (uint32_t k = 0; k < n;) {
        uint32_t c = n - k >= 8 ? 8 : n - k >= 4 ? 4 : n - k >= 2 ? 2 : 1;
        emit(e->g, "mov %s, %s ptr [rsi + %u]", rname(RAX, c), size_ptr(c), k);
        emit(e->g, "mov %s ptr [rdi + %u], %s", size_ptr(c), k, rname(RAX, c));
        k += c;
    }
}

/* Re-extends a small int in register r to 64 bits. */
static void normalize_reg(Em *e, int r, const Type *t) {
    if (t->kind == TY_BOOL) {
        emit(e->g, "movzx %s, %s", R32[r], R8[r]);
        return;
    }
    if (!type_is_intlike(t) || t->size >= 8) return;
    switch (t->size) {
    case 1: emit(e->g, t->is_signed ? "movsx %s, %s" : "movzx %s, %s", t->is_signed ? R64[r] : R32[r], R8[r]); break;
    case 2: emit(e->g, t->is_signed ? "movsx %s, %s" : "movzx %s, %s", t->is_signed ? R64[r] : R32[r], R16[r]); break;
    default:
        if (t->is_signed) emit(e->g, "movsxd %s, %s", R64[r], R32[r]);
        else emit(e->g, "mov %s, %s", R32[r], R32[r]);
        break;
    }
}

/* ---- division by constants ------------------------------------------------------
 *
 * x / d for a constant d becomes a multiply by a "magic" reciprocal, taking
 * the high half of the 128-bit product, plus shifts: the method of
 * Granlund and Montgomery, as libdivide computes it.
 */

__extension__ typedef unsigned __int128 u128;

static int floor_log2(uint64_t v) {
    return 63 - __builtin_clzll(v);
}

/* Unsigned: q = mulhi(magic, x) >> shift, or with `add`,
 * q = (((x - t) >> 1) + t) >> shift where t = mulhi(magic, x). */
static void udiv_magic(uint64_t d, uint64_t *magic, int *shift, bool *add) {
    int l = floor_log2(d);
    u128 num = (u128)1 << (64 + l);
    uint64_t m = (uint64_t)(num / d), rem = (uint64_t)(num % d);
    if (d - rem < ((uint64_t)1 << l)) {
        *add = false;
    } else {
        m += m;
        uint64_t twice = rem + rem;
        if (twice >= d || twice < rem) m += 1;
        *add = true;
    }
    *magic = m + 1;
    *shift = l;
}

/* Signed (|d| not a power of two): q = mulhi_signed(magic, x), plus x (or
 * minus x, for negative d) with `add`, then >> shift, then +1 if negative. */
static void sdiv_magic(int64_t d, int64_t *magic, int *shift, bool *add) {
    uint64_t abs_d = d < 0 ? 0 - (uint64_t)d : (uint64_t)d;
    int l = floor_log2(abs_d);
    u128 num = (u128)1 << (64 + l - 1);
    uint64_t m = (uint64_t)(num / abs_d), rem = (uint64_t)(num % abs_d);
    if (abs_d - rem < ((uint64_t)1 << l)) {
        *add = false;
        *shift = l - 1;
    } else {
        m += m;
        uint64_t twice = rem + rem;
        if (twice >= abs_d || twice < rem) m += 1;
        *add = true;
        *shift = l;
    }
    m += 1;
    *magic = d < 0 ? (int64_t)(0 - m) : (int64_t)m;
}

/* x / d or x % d with a constant d (not 0, 1 or -1, already folded). The
 * dividend goes in rcx; the quotient ends in rax, the remainder in rcx. */
static bool emit_div_const(Em *e, const Ins *in) {
    int64_t d = imm(e, in->b);
    bool sgn = in->sub == B_SDIV || in->sub == B_SREM;
    bool rem = in->sub == B_SREM || in->sub == B_UREM;
    if (d == 0 || d == 1 || d == -1 || (!sgn && d < 0)) return false;
    load_int(e, RCX, in->a);
    uint64_t abs_d = d < 0 ? 0 - (uint64_t)d : (uint64_t)d;
    if (sgn && (abs_d & (abs_d - 1)) == 0) {   /* x / 2^k, rounding toward zero: bias negative x first */
        int k = floor_log2(abs_d);
        emit(e->g, "mov rax, rcx");
        emit(e->g, "sar rax, 63");
        emit(e->g, "shr rax, %d", 64 - k);
        emit(e->g, "add rax, rcx");
        emit(e->g, "sar rax, %d", k);
        if (d < 0) emit(e->g, "neg rax");
    } else if (sgn) {
        int64_t magic;
        int shift;
        bool add;
        sdiv_magic(d, &magic, &shift, &add);
        emit(e->g, "movabs rax, %lld", (long long)magic);
        emit(e->g, "imul rcx");
        if (add) emit(e->g, "%s rdx, rcx", d < 0 ? "sub" : "add");
        if (shift) emit(e->g, "sar rdx, %d", shift);
        emit(e->g, "mov rax, rdx");
        emit(e->g, "shr rax, 63");
        emit(e->g, "add rax, rdx");
    } else if ((abs_d & (abs_d - 1)) == 0) {   /* unsigned powers of two above 2^31 */
        emit(e->g, "mov rax, rcx");
        emit(e->g, "shr rax, %d", floor_log2(abs_d));
    } else {
        uint64_t magic;
        int shift;
        bool add;
        udiv_magic((uint64_t)d, &magic, &shift, &add);
        emit(e->g, "movabs rax, %lld", (long long)magic);
        emit(e->g, "mul rcx");
        if (add) {
            emit(e->g, "mov rax, rcx");
            emit(e->g, "sub rax, rdx");
            emit(e->g, "shr rax, 1");
            emit(e->g, "add rax, rdx");
            if (shift) emit(e->g, "shr rax, %d", shift);
        } else {
            if (shift) emit(e->g, "shr rdx, %d", shift);
            emit(e->g, "mov rax, rdx");
        }
    }
    if (rem) {   /* x - q * d */
        emit(e->g, "imul rax, rax, %lld", (long long)d);
        emit(e->g, "sub rcx, rax");
        store_int(e, in->d, RCX);
    } else {
        store_int(e, in->d, RAX);
    }
    return true;
}

/* ---- instructions -------------------------------------------------------------- */

static void emit_bin(Em *e, const Ins *in) {
    int a = in->a, b = in->b, d = in->d;
    switch (in->sub) {
    case B_SDIV: case B_UDIV: case B_SREM: case B_UREM: {
        if (is_imm(e, b) && emit_div_const(e, in)) return;
        bool sgn = in->sub == B_SDIV || in->sub == B_SREM;
        const char *divisor;
        if (is_imm(e, b)) {
            load_int(e, RCX, b);
            divisor = "rcx";
        } else {
            divisor = opnd(e, b, 8);
        }
        load_int(e, RAX, a);
        emit(e->g, sgn ? "cqo" : "xor edx, edx");
        emit(e->g, "%s %s", sgn ? "idiv" : "div", divisor);
        store_int(e, d, in->sub == B_SDIV || in->sub == B_UDIV ? RAX : RDX);
        return;
    }
    case B_SHL: case B_SHR: case B_SAR: {
        const char *mn = in->sub == B_SHL ? "shl" : in->sub == B_SHR ? "shr" : "sar";
        if (is_imm(e, b)) {
            int t = target(e, d, RAX);
            load_int(e, t, a);
            emit(e->g, "%s %s, %d", mn, R64[t], (int)(imm(e, b) & 63));
            store_int(e, d, t);
            return;
        }
        load_int(e, RCX, b);
        int t = target(e, d, RAX);
        load_int(e, t, a);
        emit(e->g, "%s %s, cl", mn, R64[t]);
        store_int(e, d, t);
        return;
    }
    default: {
        static const char *const mn[] = {"add", "sub", "imul", "and", "or", "xor"};
        int t = target(e, d, RAX);
        bool commutative = in->sub != B_SUB;
        if (reg_of(e, b) == t && t != RAX) {   /* d and b share a register */
            if (commutative) {
                int tmp = a;
                a = b;
                b = tmp;
            } else {
                t = RAX;
            }
        }
        load_int(e, t, a);
        if (in->sub == B_MUL && is_imm(e, b))
            emit(e->g, "imul %s, %s, %lld", R64[t], R64[t], (long long)imm(e, b));
        else
            emit(e->g, "%s %s, %s", mn[in->sub], R64[t], opnd(e, b, 8));
        store_int(e, d, t);
        return;
    }
    }
}

static void emit_load(Em *e, const Ins *in) {
    const char *addr = address(e, in, RAX, RCX);
    if (e->f->vk[in->d] != VK_INT) {
        int x = ftarget(e, in->d, 0);
        emit(e->g, "%s xmm%d, %s ptr %s", in->sub == 4 ? "movss" : "movsd", x, size_ptr((uint32_t)in->sub), addr);
        store_f(e, in->d, x);
        return;
    }
    int t = target(e, in->d, RDX);
    switch (in->sub) {
    case 1: emit(e->g, "%s %s, byte ptr %s", in->sgn ? "movsx" : "movzx", in->sgn ? R64[t] : R32[t], addr); break;
    case 2: emit(e->g, "%s %s, word ptr %s", in->sgn ? "movsx" : "movzx", in->sgn ? R64[t] : R32[t], addr); break;
    case 4:
        if (in->sgn) emit(e->g, "movsxd %s, dword ptr %s", R64[t], addr);
        else emit(e->g, "mov %s, dword ptr %s", R32[t], addr);
        break;
    default: emit(e->g, "mov %s, qword ptr %s", R64[t], addr); break;
    }
    store_int(e, in->d, t);
}

static void emit_store(Em *e, const Ins *in) {
    uint32_t size = (uint32_t)in->sub;
    int b = in->b;
    if (e->f->vk[b] != VK_INT) {
        int x = reg_of(e, b) >= 16 ? reg_of(e, b) - 16 : 0;
        if (reg_of(e, b) < 16) load_f(e, 0, b);
        const char *addr = address(e, in, RAX, RCX);
        emit(e->g, "%s %s ptr %s, xmm%d", size == 4 ? "movss" : "movsd", size_ptr(size), addr, x);
        return;
    }
    if (is_imm(e, b)) {
        const char *addr = address(e, in, RAX, RCX);
        int64_t v = imm(e, b);
        if (size == 1) v = (int8_t)v;
        else if (size == 2) v = (int16_t)v;
        else if (size == 4) v = (int32_t)v;
        emit(e->g, "mov %s ptr %s, %lld", size_ptr(size), addr, (long long)v);
        return;
    }
    int r = reg_of(e, b);
    if (r < 0) {
        load_int(e, RDX, b);
        r = RDX;
    }
    const char *addr = address(e, in, RAX, RCX);
    emit(e->g, "mov %s ptr %s, %s", size_ptr(size), addr, rname(r, size));
}

static void emit_ext(Em *e, const Ins *in) {
    int t = target(e, in->d, RAX);
    if (is_imm(e, in->a)) {   /* fold */
        uint64_t v = (uint64_t)imm(e, in->a);
        unsigned bits = (unsigned)in->sub * 8;
        uint64_t mask = ((uint64_t)1 << bits) - 1;
        v &= mask;
        if (in->sgn && (v >> (bits - 1))) v |= ~mask;
        emit(e->g, "mov %s, %lld", R64[t], (long long)v);
        store_int(e, in->d, t);
        return;
    }
    const char *src = opnd(e, in->a, (uint32_t)in->sub);
    switch (in->sub) {
    case 1:
    case 2: emit(e->g, "%s %s, %s", in->sgn ? "movsx" : "movzx", in->sgn ? R64[t] : R32[t], src); break;
    default:
        if (in->sgn) emit(e->g, "movsxd %s, %s", R64[t], src);
        else emit(e->g, "mov %s, %s", R32[t], src);
        break;
    }
    store_int(e, in->d, t);
}

/* ucomiss/ucomisd x, y: x from a register (or xmm0), y may be memory. */
static void emit_ucomis(Em *e, int x, int y) {
    const char *ucom = f32(e, x) ? "ucomiss" : "ucomisd";
    int rx = reg_of(e, x) >= 16 ? reg_of(e, x) - 16 : 0;
    if (rx == 0) load_f(e, 0, x);
    emit(e->g, "%s xmm%d, %s", ucom, rx, opnd(e, y, f32(e, y) ? 4 : 8));
}

/* Compares for (a cc b): which operand goes first, and the flag test that
 * means true (NaN compares false, except with !=). */
static const char *fcmp_setup(Em *e, const Ins *in) {
    switch (in->sub) {
    case CC_LT: emit_ucomis(e, in->b, in->a); return "a";
    case CC_LE: emit_ucomis(e, in->b, in->a); return "ae";
    case CC_GT: emit_ucomis(e, in->a, in->b); return "a";
    case CC_GE: emit_ucomis(e, in->a, in->b); return "ae";
    default: emit_ucomis(e, in->a, in->b); return NULL;   /* EQ, NE: need the parity flag too */
    }
}

static void emit_fcmp(Em *e, const Ins *in) {
    const char *cc = fcmp_setup(e, in);
    if (in->sub == CC_EQ) {
        emit(e->g, "sete al");
        emit(e->g, "setnp cl");
        emit(e->g, "and al, cl");
    } else if (in->sub == CC_NE) {
        emit(e->g, "setne al");
        emit(e->g, "setp cl");
        emit(e->g, "or al, cl");
    } else {
        emit(e->g, "set%s al", cc);
    }
    int t = target(e, in->d, RAX);
    emit(e->g, "movzx %s, al", R32[t]);
    store_int(e, in->d, t);
}

static const char *negate_flag(const char *cc) {
    return strcmp(cc, "a") == 0 ? "be" : "b";   /* !(CF=0 & ZF=0), !(CF=0) */
}

/* A float comparison whose only use is the branch right after it: one
 * ucomis and conditional jumps, without making a bool first. lt/lf are
 * where to go when the comparison is true/false. */
static void emit_fcmp_branch(Em *e, const Ins *in, int lt, int lf, size_t i_next) {
    const char *cc = fcmp_setup(e, in);
    bool lf_next = next_is_label(e, i_next, lf), lt_next = next_is_label(e, i_next, lt);
    if (in->sub == CC_EQ) {   /* true: ZF=1 and PF=0 */
        emit(e->g, "jp .L%d", lf);
        if (lt_next) {
            emit(e->g, "jne .L%d", lf);
        } else {
            emit(e->g, "je .L%d", lt);
            if (!lf_next) emit(e->g, "jmp .L%d", lf);
        }
    } else if (in->sub == CC_NE) {   /* true: ZF=0 or PF=1 */
        emit(e->g, "jp .L%d", lt);
        if (lf_next) {
            emit(e->g, "jne .L%d", lt);
        } else if (lt_next) {
            emit(e->g, "je .L%d", lf);
        } else {
            emit(e->g, "jne .L%d", lt);
            emit(e->g, "jmp .L%d", lf);
        }
    } else if (lt_next) {
        emit(e->g, "j%s .L%d", negate_flag(cc), lf);
    } else {
        emit(e->g, "j%s .L%d", cc, lt);
        if (!lf_next) emit(e->g, "jmp .L%d", lf);
    }
}

static void emit_fbin(Em *e, const Ins *in) {
    static const char *const mn[] = {"add", "sub", "mul", "div"};
    const char *sfx = f32(e, in->d) ? "ss" : "sd";
    int x = ftarget(e, in->d, 0);
    if (reg_of(e, in->b) == 16 + x) x = 0;   /* d shares b's register */
    load_f(e, x, in->a);
    emit(e->g, "%s%s xmm%d, %s", mn[in->sub], sfx, x, opnd(e, in->b, f32(e, in->b) ? 4 : 8));
    store_f(e, in->d, x);
}

static void emit_convert(Em *e, const Ins *in) {
    int a = in->a, d = in->d;
    switch (in->op) {
    case I_I2F: {
        int x = ftarget(e, d, 0);
        const char *src = is_imm(e, a) ? (load_int(e, RAX, a), "rax") : opnd(e, a, 8);
        emit(e->g, "%s xmm%d, %s", f32(e, d) ? "cvtsi2ss" : "cvtsi2sd", x, src);
        store_f(e, d, x);
        return;
    }
    case I_U2F: {
        /* A u64 above 2^63 doesn't fit cvtsi2sd's signed input: halve it,
         * keeping the low bit so rounding stays right, convert, double. */
        const char *cvt = f32(e, d) ? "cvtsi2ss" : "cvtsi2sd";
        int big = new_label(e->g), done = new_label(e->g);
        load_int(e, RAX, a);
        emit(e->g, "test rax, rax");
        emit(e->g, "js .L%d", big);
        emit(e->g, "%s xmm0, rax", cvt);
        emit(e->g, "jmp .L%d", done);
        emit_label(e->g, big);
        emit(e->g, "mov rcx, rax");
        emit(e->g, "shr rcx, 1");
        emit(e->g, "and eax, 1");
        emit(e->g, "or rcx, rax");
        emit(e->g, "%s xmm0, rcx", cvt);
        emit(e->g, f32(e, d) ? "addss xmm0, xmm0" : "addsd xmm0, xmm0");
        emit_label(e->g, done);
        store_f(e, d, 0);
        return;
    }
    case I_F2I: {
        int t = target(e, d, RAX);
        emit(e->g, "%s %s, %s", f32(e, a) ? "cvttss2si" : "cvttsd2si", R64[t], opnd(e, a, f32(e, a) ? 4 : 8));
        store_int(e, d, t);
        return;
    }
    case I_F2U: {   /* from f64: values from 2^63 up subtract 2^63 first, then put the top bit back */
        load_f(e, 0, a);
        int big = new_label(e->g), done = new_label(e->g);
        emit(e->g, "movabs rax, 4890909195324358656");
        emit(e->g, "movq xmm1, rax");
        emit(e->g, "ucomisd xmm0, xmm1");
        emit(e->g, "jae .L%d", big);
        emit(e->g, "cvttsd2si rax, xmm0");
        emit(e->g, "jmp .L%d", done);
        emit_label(e->g, big);
        emit(e->g, "subsd xmm0, xmm1");
        emit(e->g, "cvttsd2si rax, xmm0");
        emit(e->g, "btc rax, 63");
        emit_label(e->g, done);
        store_int(e, d, RAX);
        return;
    }
    default: {   /* I_F2F */
        int x = ftarget(e, d, 0);
        emit(e->g, "%s xmm%d, %s", f32(e, a) ? "cvtss2sd" : "cvtsd2ss", x, opnd(e, a, f32(e, a) ? 4 : 8));
        store_f(e, d, x);
        return;
    }
    }
}

/* Where each argument of a call goes. */
typedef struct {
    bool on_stack;
    int stack_off;
    int reg[2];
    Classification c;
    uint32_t size;
} Place;

static void emit_call(Em *e, const Ins *in) {
    const IrCall *c = in->call;
    Classification ret = classify(c->ret);
    Place *p = xmalloc((c->n ? c->n : 1) * sizeof *p);
    int ngpr = ret.memory ? 1 : 0, nsse = 0, stack = 0;
    for (size_t i = 0; i < c->n; i++) {
        const Type *t = c->types[i];
        p[i].c = classify(t);
        p[i].size = type_in_memory(t) ? (t->size + 7) & ~7u : 8;
        int kint = 0, ksse = 0;
        for (int j = 0; j < p[i].c.n; j++) {
            if (p[i].c.eb[j] == EB_INT) kint++;
            else ksse++;
        }
        p[i].on_stack = p[i].c.memory || ngpr + kint > 6 || nsse + ksse > 8;
        if (p[i].on_stack) {
            p[i].stack_off = stack;
            stack += (int)p[i].size;
        } else {
            for (int j = 0; j < p[i].c.n; j++) p[i].reg[j] = p[i].c.eb[j] == EB_INT ? ngpr++ : nsse++;
        }
    }
    int total = stack + (stack % 16 ? 8 : 0);
    if (total) emit(e->g, "sub rsp, %d", total);
    for (size_t i = 0; i < c->n; i++) {   /* the stack first: copying uses rsi, rdi, rcx */
        if (!p[i].on_stack) continue;
        int v = c->args[i];
        if (type_in_memory(c->types[i])) {
            load_int(e, RSI, v);
            emit(e->g, "lea rdi, [rsp + %d]", p[i].stack_off);
            copy_bytes(e, p[i].size);
        } else if (e->f->vk[v] != VK_INT) {
            load_f(e, 0, v);
            emit(e->g, "movsd qword ptr [rsp + %d], xmm0", p[i].stack_off);
        } else {
            load_int(e, RAX, v);
            emit(e->g, "mov qword ptr [rsp + %d], rax", p[i].stack_off);
        }
    }
    for (size_t i = 0; i < c->n; i++) {
        if (p[i].on_stack) continue;
        int v = c->args[i];
        if (type_in_memory(c->types[i])) {
            load_int(e, RAX, v);
            for (int j = 0; j < p[i].c.n; j++) {
                if (p[i].c.eb[j] == EB_INT) emit(e->g, "mov %s, qword ptr [rax + %d]", R64[ARG_GPR[p[i].reg[j]]], 8 * j);
                else emit(e->g, "movsd xmm%d, qword ptr [rax + %d]", p[i].reg[j], 8 * j);
            }
        } else if (e->f->vk[v] != VK_INT) {
            load_f(e, p[i].reg[0], v);
        } else {
            load_int(e, ARG_GPR[p[i].reg[0]], v);
        }
    }
    if (ret.memory) emit(e->g, "lea rdi, [rbp - %d]", e->f->slots[c->ret_slot].off);
    if (c->variadic) emit(e->g, "mov eax, %d", nsse);   /* vector registers used, for varargs */
    if (c->vslot) emit_virtual_call(e->g, R64[ARG_GPR[ret.memory ? 1 : 0]], c->vslot - 1, c->iface);
    else emit(e->g, "call %s", c->name);
    if (total) emit(e->g, "add rsp, %d", total);
    free(p);

    int d = in->d;
    if (d < 0) return;
    const Type *rt = c->ret;
    if (type_in_memory(rt)) {
        int off = e->f->slots[c->ret_slot].off;
        if (!ret.memory) {   /* reassemble the struct from rax/rdx and xmm0/xmm1 */
            int gi = 0, si = 0;
            for (int j = 0; j < ret.n; j++) {
                if (ret.eb[j] == EB_INT) emit(e->g, "mov qword ptr [rbp - %d], %s", off - 8 * j, gi++ ? "rdx" : "rax");
                else emit(e->g, "movsd qword ptr [rbp - %d], xmm%d", off - 8 * j, si++);
            }
        }
        int t = target(e, d, RAX);
        emit(e->g, "lea %s, [rbp - %d]", R64[t], off);
        store_int(e, d, t);
    } else if (rt->kind == TY_FLOAT) {
        store_f(e, d, 0);
    } else {
        normalize_reg(e, RAX, rt);   /* C leaves the upper bits of small ints undefined */
        store_int(e, d, RAX);
    }
}

static bool asm_mentions(const StrBuf *line, int r) {
    const char *t = line->data;
    size_t n = line->len;
    for (size_t i = 0; i < n; i++) {
        if (i > 0 && ((t[i - 1] >= 'a' && t[i - 1] <= 'z') || (t[i - 1] >= '0' && t[i - 1] <= '9'))) continue;
        size_t j = i;
        while (j < n && ((t[j] >= 'a' && t[j] <= 'z') || (t[j] >= '0' && t[j] <= '9'))) j++;
        if (j > i && asm_register(t + i, j - i) == r) return true;
    }
    return false;
}

static void emit_asm(Em *e, const Ins *in) {
    const IrAsm *as = in->as;
    const Stmt *s = as->stmt;
    size_t n = s->u.asm_.nops;
    for (size_t i = 0; i < n; i++) {   /* inputs into temporaries, so loading registers can't clobber them */
        int v = as->ins[i];
        if (v < 0) continue;
        int off = e->f->slots[as->in_slots[i]].off;
        if (e->f->vk[v] != VK_INT) {
            load_f(e, 0, v);
            emit(e->g, "movsd qword ptr [rbp - %d], xmm0", off);
        } else if (is_imm(e, v) || reg_of(e, v) >= 0) {
            emit(e->g, "mov qword ptr [rbp - %d], %s", off, opnd(e, v, 8));
        } else {
            load_int(e, RAX, v);
            emit(e->g, "mov qword ptr [rbp - %d], rax", off);
        }
    }
    static const int callee_saved[5] = {3, 12, 13, 14, 15};
    bool save[5] = {false};
    for (int k = 0; k < 5; k++) {
        for (size_t i = 0; i < n; i++)
            save[k] |= asm_register(s->u.asm_.ops[i]->reg.data, s->u.asm_.ops[i]->reg.len) == callee_saved[k];
        for (size_t i = 0; i < s->u.asm_.nlines; i++) save[k] |= asm_mentions(s->u.asm_.lines[i], callee_saved[k]);
        if (save[k]) emit(e->g, "push %s", R64[callee_saved[k]]);
    }
    for (size_t i = 0; i < n; i++) {
        if (as->ins[i] < 0) continue;
        int r = asm_register(s->u.asm_.ops[i]->reg.data, s->u.asm_.ops[i]->reg.len);
        int off = e->f->slots[as->in_slots[i]].off;
        if (r >= 16) emit(e->g, "movsd xmm%d, qword ptr [rbp - %d]", r - 16, off);
        else emit(e->g, "mov %s, qword ptr [rbp - %d]", R64[r], off);
    }
    fputs("    # asm\n", e->g->out);
    for (size_t i = 0; i < s->u.asm_.nlines; i++) {
        const StrBuf *line = s->u.asm_.lines[i];
        fprintf(e->g->out, "    %.*s\n", (int)line->len, line->data);
    }
    fputs("    # end asm\n", e->g->out);
    for (size_t i = 0; i < n; i++) {
        if (as->outs[i] < 0) continue;
        int r = asm_register(s->u.asm_.ops[i]->reg.data, s->u.asm_.ops[i]->reg.len);
        int off = e->f->slots[as->out_slots[i]].off;
        if (r >= 16) emit(e->g, "movsd qword ptr [rbp - %d], xmm%d", off, r - 16);
        else emit(e->g, "mov qword ptr [rbp - %d], %s", off, R64[r]);
    }
    for (int k = 4; k >= 0; k--)
        if (save[k]) emit(e->g, "pop %s", R64[callee_saved[k]]);
    for (size_t i = 0; i < n; i++) {
        int v = as->outs[i];
        if (v < 0) continue;
        int off = e->f->slots[as->out_slots[i]].off;
        if (e->f->vk[v] != VK_INT) {
            emit(e->g, "%s xmm0, %s ptr [rbp - %d]", f32(e, v) ? "movss" : "movsd", f32(e, v) ? "dword" : "qword", off);
            store_f(e, v, 0);
        } else {
            int t = target(e, v, RAX);
            emit(e->g, "mov %s, qword ptr [rbp - %d]", R64[t], off);
            store_int(e, v, t);
        }
    }
}

static void emit_params(Em *e) {
    Fn *f = e->f;
    if (f->hidden >= 0 && f->loc[f->hidden] != 0x7fffffff) store_int(e, f->hidden, RDI);
    for (size_t i = 0; i < f->nparams; i++) {
        const ParamPlace *p = &f->params[i];
        if (p->vreg >= 0 && f->loc[p->vreg] == 0x7fffffff) continue;   /* never used */
        if (p->vreg >= 0) {
            int v = p->vreg;
            if (f->vk[v] != VK_INT) {
                if (p->on_stack) {
                    emit(e->g, "%s xmm0, %s ptr [rbp + %d]", f32(e, v) ? "movss" : "movsd", f32(e, v) ? "dword" : "qword",
                         p->stack_off);
                    store_f(e, v, 0);
                } else {
                    store_f(e, v, p->reg[0]);
                }
                continue;
            }
            int t = target(e, v, RAX);
            if (p->on_stack) emit(e->g, "mov %s, qword ptr [rbp + %d]", R64[t], p->stack_off);
            else emit(e->g, "mov %s, %s", R64[t], R64[ARG_GPR[p->reg[0]]]);
            normalize_reg(e, t, p->type);   /* callers (C ones especially) may leave upper bits */
            store_int(e, v, t);
        } else if (p->slot >= 0 && !f->slots[p->slot].incoming) {
            int off = f->slots[p->slot].off;
            for (int j = 0; j < p->c.n; j++) {
                if (p->c.eb[j] == EB_INT) emit(e->g, "mov qword ptr [rbp - %d], %s", off - 8 * j, R64[ARG_GPR[p->reg[j]]]);
                else emit(e->g, "movsd qword ptr [rbp - %d], xmm%d", off - 8 * j, p->reg[j]);
            }
        }
    }
}

static void emit_ret(Em *e, const Ins *in) {
    const Type *rt = e->f->fn->type;
    Classification c = classify(rt);
    if (in->a >= 0) {
        if (c.memory) {
            load_int(e, RAX, in->a);
        } else if (type_in_memory(rt)) {
            load_int(e, RSI, in->a);
            int gi = 0, si = 0;
            for (int j = 0; j < c.n; j++) {
                if (c.eb[j] == EB_INT) emit(e->g, "mov %s, qword ptr [rsi + %d]", gi++ ? "rdx" : "rax", 8 * j);
                else emit(e->g, "movsd xmm%d, qword ptr [rsi + %d]", si++, 8 * j);
            }
        } else if (rt->kind == TY_FLOAT) {
            load_f(e, 0, in->a);
        } else {
            load_int(e, RAX, in->a);
        }
    }
    if (e->nsaved) {
        emit(e->g, "lea rsp, [rbp - %d]", 8 * e->nsaved);
        for (int k = e->nsaved - 1; k >= 0; k--) emit(e->g, "pop %s", R64[e->saved[k]]);
        emit(e->g, "pop rbp");
    } else {
        emit(e->g, "leave");
    }
    emit(e->g, "ret");
}

static void emit_ins(Em *e, size_t i) {
    const Ins *in = &e->f->code[i];
    int d = in->d;
    switch (in->op) {
    case I_NOP:
        return;
    case I_CONST: {
        int t = target(e, d, RAX);
        if (in->imm == 0) emit(e->g, "xor %s, %s", R32[t], R32[t]);
        else if (in->imm >= INT32_MIN && in->imm <= INT32_MAX) emit(e->g, "mov %s, %lld", R64[t], (long long)in->imm);
        else emit(e->g, "movabs %s, %lld", R64[t], (long long)in->imm);
        store_int(e, d, t);
        return;
    }
    case I_FCONST: {
        int x = ftarget(e, d, 0);
        if (in->imm == 0) {
            emit(e->g, "xorps xmm%d, xmm%d", x, x);
        } else {
            emit(e->g, "movabs rax, %lld", (long long)in->imm);
            emit(e->g, "%s xmm%d, %s", f32(e, d) ? "movd" : "movq", x, f32(e, d) ? "eax" : "rax");
        }
        store_f(e, d, x);
        return;
    }
    case I_MOV:
        if (e->f->vk[d] != VK_INT) {
            if (reg_of(e, d) >= 16) load_f(e, reg_of(e, d) - 16, in->a);
            else if (reg_of(e, in->a) >= 16) store_f(e, d, reg_of(e, in->a) - 16);
            else {
                load_f(e, 0, in->a);
                store_f(e, d, 0);
            }
        } else if (reg_of(e, d) >= 0) {
            load_int(e, reg_of(e, d), in->a);
        } else if (reg_of(e, in->a) >= 0 || is_imm(e, in->a)) {
            emit(e->g, "mov %s, %s", opnd(e, d, 8), opnd(e, in->a, 8));
        } else {
            load_int(e, RAX, in->a);
            store_int(e, d, RAX);
        }
        return;
    case I_BIN:
        emit_bin(e, in);
        return;
    case I_NEG:
    case I_NOT: {
        int t = target(e, d, RAX);
        load_int(e, t, in->a);
        emit(e->g, "%s %s", in->op == I_NEG ? "neg" : "not", R64[t]);
        store_int(e, d, t);
        return;
    }
    case I_EXT:
        emit_ext(e, in);
        return;
    case I_FBIN:
        emit_fbin(e, in);
        return;
    case I_FNEG: {
        int x = ftarget(e, d, 0);
        load_f(e, x, in->a);
        if (f32(e, d)) {
            emit(e->g, "movd eax, xmm%d", x);
            emit(e->g, "btc eax, 31");
            emit(e->g, "movd xmm%d, eax", x);
        } else {
            emit(e->g, "movq rax, xmm%d", x);
            emit(e->g, "btc rax, 63");
            emit(e->g, "movq xmm%d, rax", x);
        }
        store_f(e, d, x);
        return;
    }
    case I_CMP: {
        emit_cmp(e, in->a, in->b);
        emit(e->g, "set%s al", cc_suffix(in->sub));
        int t = target(e, d, RAX);
        emit(e->g, "movzx %s, al", R32[t]);
        store_int(e, d, t);
        return;
    }
    case I_FCMP: {
        size_t j = i + 1;
        while (j < e->f->n && e->f->code[j].op == I_NOP) j++;
        const Ins *br = j < e->f->n ? &e->f->code[j] : NULL;
        if (br && br->op == I_CBR && br->a == d && is_imm(e, br->b) && imm(e, br->b) == 0 &&
            (br->sub == CC_NE || br->sub == CC_EQ) && e->uses[d] == 1) {
            emit_fcmp_branch(e, in, br->sub == CC_NE ? br->l1 : br->l2, br->sub == CC_NE ? br->l2 : br->l1, j);
            e->skip = j;
            return;
        }
        emit_fcmp(e, in);
        return;
    }
    case I_I2F: case I_U2F: case I_F2I: case I_F2U: case I_F2F:
        emit_convert(e, in);
        return;
    case I_LOAD:
        emit_load(e, in);
        return;
    case I_STORE:
        emit_store(e, in);
        return;
    case I_LEA: {
        const char *addr = address(e, in, RAX, RCX);
        int t = target(e, d, RAX);
        emit(e->g, "lea %s, %s", R64[t], addr);
        store_int(e, d, t);
        return;
    }
    case I_GOT: {
        int t = target(e, d, RAX);
        emit(e->g, "mov %s, qword ptr [rip + %s@GOTPCREL]", R64[t], in->sym);
        store_int(e, d, t);
        return;
    }
    case I_STR: {
        int t = target(e, d, RAX);
        emit(e->g, "lea %s, [rip + .Lstr%d]", R64[t], in->l1);
        store_int(e, d, t);
        return;
    }
    case I_COPY:
        load_int(e, RDI, in->a);
        load_int(e, RSI, in->b);
        copy_bytes(e, (uint32_t)in->imm);
        return;
    case I_ZERO: {
        load_int(e, RDI, in->a);
        uint32_t n = (uint32_t)in->imm;
        if (n > 64) {
            emit(e->g, "xor eax, eax");
            emit(e->g, "mov ecx, %u", n);
            emit(e->g, "rep stosb");
            return;
        }
        for (uint32_t k = 0; k < n;) {
            uint32_t c = n - k >= 8 ? 8 : n - k >= 4 ? 4 : n - k >= 2 ? 2 : 1;
            emit(e->g, "mov %s ptr [rdi + %u], 0", size_ptr(c), k);
            k += c;
        }
        return;
    }
    case I_CALL:
        emit_call(e, in);
        return;
    case I_JMP:
        if (!next_is_label(e, i, in->l1)) emit(e->g, "jmp .L%d", in->l1);
        return;
    case I_CBR: {
        emit_cmp(e, in->a, in->b);
        if (next_is_label(e, i, in->l1)) {
            emit(e->g, "j%s .L%d", cc_suffix(invert(in->sub)), in->l2);
        } else {
            emit(e->g, "j%s .L%d", cc_suffix(in->sub), in->l1);
            if (!next_is_label(e, i, in->l2)) emit(e->g, "jmp .L%d", in->l2);
        }
        return;
    }
    case I_LABEL:
        emit_label(e->g, in->l1);
        return;
    case I_RET:
        emit_ret(e, in);
        return;
    case I_PANIC:
        emit_panic_call(e->g, in->l1);
        return;
    case I_ASM:
        emit_asm(e, in);
        return;
    case I_PARAMS:
        emit_params(e);
        return;
    }
}

void ir_emit_function(Fn *f) {
    Em e = {0};
    e.f = f;
    e.g = f->g;
    static const int callee_saved[5] = {3, 12, 13, 14, 15};
    for (int k = 0; k < 5; k++)
        if (f->used_reg[callee_saved[k]]) e.saved[e.nsaved++] = callee_saved[k];

    /* Slot offsets, below the saved registers, each aligned. */
    int cursor = 8 * e.nsaved;
    for (int s = 0; s < f->nslots; s++) {
        Slot *sl = &f->slots[s];
        if (sl->incoming) continue;
        cursor = (int)(((uint32_t)cursor + sl->size + sl->align - 1) & ~(sl->align - 1));
        sl->off = cursor;
    }
    int frame = ((cursor + 15) & ~15) - 8 * e.nsaved;

    const char *name = f->fn->asm_name;
    FILE *out = f->g->out;
    fputc('\n', out);
    if (f->fn->is_export) fprintf(out, "    .globl %s\n", name);
    fprintf(out, "    .type %s, @function\n%s:\n", name, name);
    emit(e.g, "push rbp");
    emit(e.g, "mov rbp, rsp");
    for (int k = 0; k < e.nsaved; k++) emit(e.g, "push %s", R64[e.saved[k]]);
    if (frame) emit(e.g, "sub rsp, %d", frame);
    static int serial;
    e.serial = serial++;
    e.skip = (size_t)-1;
    e.uses = calloc((size_t)f->nv + 1, sizeof *e.uses);
    for (size_t i = 0; i < f->n; i++) {
        int *ptrs[3], n;
        Ins *in = &f->code[i];
        if (in->op == I_NOP) continue;
        ir_operand_ptrs(in, ptrs, &n);
        for (int k = 0; k < n; k++) e.uses[*ptrs[k]]++;
        if (in->call)
            for (size_t k = 0; k < in->call->n; k++) e.uses[in->call->args[k]]++;
        if (in->as)
            for (size_t k = 0; k < in->as->stmt->u.asm_.nops; k++)
                if (in->as->ins[k] >= 0) e.uses[in->as->ins[k]]++;
    }
    for (size_t i = 0; i < f->n; i++)
        if (i != e.skip) emit_ins(&e, i);
    fprintf(out, "    .size %s, .-%s\n", name, name);
    free(e.uses);

    /* The float constants it reads. */
    bool any = false;
    for (int v = 0; v < f->nv; v++) {
        if (!f->is_imm[v] || f->vk[v] == VK_INT) continue;
        if (!any) fputs("    .section .rodata\n", out);
        any = true;
        if (f->vk[v] == VK_F32)
            fprintf(out, "    .p2align 2\n.LF%d_%d:\n    .long %u\n", e.serial, v, (unsigned)(uint32_t)f->imm_val[v]);
        else
            fprintf(out, "    .p2align 3\n.LF%d_%d:\n    .quad %lld\n", e.serial, v, (long long)f->imm_val[v]);
    }
    if (any) fputs("    .text\n", out);
}

/* ---- debugging: WATR_DUMP_IR=1 prints each function's IR to stderr ------------------- */

static void dump(const Fn *f, const char *when) {
    static const char *const names[] = {"nop", "const", "fconst", "mov", "bin", "neg", "not", "ext", "fbin",
                                        "fneg", "cmp", "fcmp", "i2f", "u2f", "f2i", "f2u", "f2f", "load",
                                        "store", "lea", "got", "str", "copy", "zero", "call", "jmp", "cbr",
                                        "label", "ret", "panic", "asm", "params"};
    fprintf(stderr, "== %s (%s): %zu instructions, %d vregs\n", f->fn->name, when, f->n, f->nv);
    for (size_t i = 0; i < f->n; i++) {
        const Ins *in = &f->code[i];
        fprintf(stderr, "  %4zu %-6s", i, names[in->op]);
        if (in->d >= 0) fprintf(stderr, " v%d =", in->d);
        if (in->a >= 0) fprintf(stderr, " %sv%d", f->is_imm && f->is_imm[in->a] ? "#" : "", in->a);
        if (in->b >= 0) fprintf(stderr, " %sv%d", f->is_imm && f->is_imm[in->b] ? "#" : "", in->b);
        if (in->idx >= 0) fprintf(stderr, " [idx v%d*%d]", in->idx, in->scale);
        if (in->slot >= 0) fprintf(stderr, " [slot %d]", in->slot);
        if (in->sym) fprintf(stderr, " [%s]", in->sym);
        fprintf(stderr, " imm=%lld sub=%d l=%d/%d", (long long)in->imm, in->sub, in->l1, in->l2);
        if (in->call) fprintf(stderr, " call %s/%zu", in->call->name, in->call->n);
        fputc('\n', stderr);
    }
}

static void free_fn(Fn *f) {
    for (size_t i = 0; i < f->n; i++) {
        Ins *in = &f->code[i];
        if (in->call) {
            free(in->call->args);
            free(in->call->types);
            free(in->call);
        }
        if (in->as) {
            free(in->as->ins);
            free(in->as->outs);
            free(in->as->in_slots);
            free(in->as->out_slots);
            free(in->as);
        }
    }
    free(f->code);
    free(f->vk);
    free(f->slots);
    free(f->defers.items);
    free(f->scopes);
    free(f->loops);
    free(f->params);
    free(f->panic_jumps.items);
    free(f->is_imm);
    free(f->imm_val);
    free(f->loc);
    free(f);
}

struct OptProgram {
    Gen *g;
    Fn **fns;           /* per prog->funcs[i]; NULL if it isn't compiled */
    size_t n;
    bool dumping;
};

/* Lowers and optimizes every function that will be compiled, then inlines
 * small functions into their callers (which needs them all at once). */
OptProgram *opt_prepare(Gen *g) {
    const Program *prog = g->prog;
    OptProgram *p = xmalloc(sizeof *p);
    p->g = g;
    p->n = prog->nfuncs;
    p->fns = calloc(p->n + 1, sizeof *p->fns);
    p->dumping = getenv("WATR_DUMP_IR") != NULL;
    for (size_t i = 0; i < p->n; i++) {
        const Symbol *fn = prog->funcs[i];
        if (!fn->reachable || !fn->decl->u.func.body) continue;
        Fn *f = calloc(1, sizeof *f);
        f->g = g;
        f->fn = fn;
        ir_lower_function(f);
        if (p->dumping) dump(f, "lowered");
        ir_optimize(f);
        p->fns[i] = f;
    }
    if (!getenv("WATR_NO_INLINE")) ir_inline_all(p->fns, p->n);   /* WATR_NO_INLINE=1: for debugging */
    return p;
}

void opt_emit_function(OptProgram *p, size_t i) {
    Fn *f = p->fns[i];
    if (!f) return;
    ir_allocate(f);
    if (p->dumping) dump(f, "optimized");
    ir_emit_function(f);
    free_fn(f);
    p->fns[i] = NULL;
}

void opt_free(OptProgram *p) {
    for (size_t i = 0; i < p->n; i++)
        if (p->fns[i]) free_fn(p->fns[i]);
    free(p->fns);
    free(p);
}
