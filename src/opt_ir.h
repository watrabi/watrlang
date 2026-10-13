#ifndef WATR_OPT_IR_H
#define WATR_OPT_IR_H

/*
 * The -O1 backend's intermediate representation: one function at a time,
 * a list of three-address instructions over virtual registers (vregs),
 * with labels and branches. Scalars (ints, bools, pointers, enums, object
 * references, floats) are vreg values; aggregates (structs, arrays,
 * strings, mixed, error unions) are handled by address, as in the -O0
 * generator.
 *
 *   opt_lower.c  AST -> IR (and the defer/loop bookkeeping)
 *   opt_ra.c     cleanup passes, liveness, linear-scan register allocation
 *   opt_emit.c   IR -> x86-64 assembly
 */

#include "codegen_internal.h"

typedef enum { VK_INT, VK_F64, VK_F32 } VKind;

/* I_BIN operations */
enum { B_ADD, B_SUB, B_MUL, B_AND, B_OR, B_XOR, B_SHL, B_SHR, B_SAR, B_SDIV, B_UDIV, B_SREM, B_UREM };
/* I_FBIN operations */
enum { F_ADD, F_SUB, F_MUL, F_DIV };
/* conditions: CC_EQ..CC_GE are signed for ints (and the only ones for floats) */
enum { CC_EQ, CC_NE, CC_LT, CC_LE, CC_GT, CC_GE, CC_B, CC_BE, CC_A, CC_AE };

typedef enum {
    I_NOP,
    I_CONST,   /* d = imm */
    I_FCONST,  /* d = the float whose bits are imm (f32: low 32 bits) */
    I_MOV,     /* d = a */
    I_BIN,     /* d = a <sub> b */
    I_NEG,     /* d = -a */
    I_NOT,     /* d = ~a */
    I_EXT,     /* d = a truncated to sub bytes and sign/zero-extended (sgn) */
    I_FBIN,    /* d = a <sub> b, floats */
    I_FNEG,    /* d = -a, floats */
    I_CMP,     /* d = (a <sub> b) ? 1 : 0 */
    I_FCMP,    /* d = (a <sub> b) ? 1 : 0, floats (false for NaN, except !=) */
    I_I2F,     /* d = (float) a, a signed */
    I_U2F,     /* d = (float) a, a a u64 */
    I_F2I,     /* d = (i64) a, truncating */
    I_F2U,     /* d = (u64) a, truncating */
    I_F2F,     /* d = a converted between f32 and f64 */
    I_LOAD,    /* d = [address] (sub bytes, sgn); see Ins for addressing */
    I_STORE,   /* [address] = b (sub bytes) */
    I_LEA,     /* d = address */
    I_GOT,     /* d = address of extern symbol `sym` */
    I_STR,     /* d = address of a string literal: the {ptr, len} value if imm, else the bytes */
    I_COPY,    /* copy imm bytes from address b to address a */
    I_ZERO,    /* zero imm bytes at address a */
    I_CALL,    /* d = call (d = -1 for void) */
    I_JMP,     /* goto l1 */
    I_CBR,     /* if (a <sub> b) goto l1 else goto l2 */
    I_LABEL,   /* l1: */
    I_RET,     /* return a (-1 for void); the function's exit */
    I_PANIC,   /* report panic site l1 and stop */
    I_ASM,     /* inline asm */
    I_PARAMS,  /* the function entry: parameters move from where the caller put them */
} IrOp;

/* Addresses (LOAD, STORE, LEA): base + idx * scale + off, where the base
 * is a vreg (a), a frame slot (slot >= 0) or a global symbol (sym). */

typedef struct IrCall {
    const char *name;
    bool variadic;
    size_t n;
    int *args;              /* vregs: scalar values, or the addresses of aggregates */
    const Type **types;     /* the types they're passed as */
    const Type *ret;
    int ret_slot;           /* aggregate results land in this slot, else -1 */
    int vslot;              /* 0: call `name`; k: virtual call through slot k - 1 (args[0] is the receiver) */
    const char *iface;      /* with vslot: the interface whose table to use, or NULL for the vtable */
} IrCall;

typedef struct IrAsm {
    const Stmt *stmt;
    int *ins;               /* per operand: the input vreg, or -1 */
    int *outs;              /* per operand: the output vreg, or -1 */
    int *in_slots, *out_slots;   /* frame temporaries the values pass through */
} IrAsm;

typedef struct {
    IrOp op;
    int d, a, b;            /* vregs, -1 if unused */
    int idx, scale;         /* address index vreg (-1 if none) and its scale (1, 2, 4, 8) */
    int slot;               /* address base slot, or -1 */
    const char *sym;        /* address base symbol, or I_GOT's symbol */
    int64_t imm;            /* CONST value, FCONST bits, address offset, COPY/ZERO size, STR kind */
    int sub;                /* BIN/FBIN op, CMP/FCMP/CBR condition, EXT/LOAD/STORE size */
    bool sgn;               /* EXT, LOAD: sign-extend */
    int l1, l2;             /* labels; PANIC: the panic site */
    IrCall *call;
    IrAsm *as;
} Ins;

/* Where a parameter arrives (filled in by lowering, used by I_PARAMS). */
typedef struct {
    int vreg;               /* the vreg it goes in, or -1 */
    int slot;               /* the slot it goes in (or already is in, for INCOMING slots), or -1 */
    const Type *type;
    bool on_stack;
    int stack_off;          /* from rbp + 16 */
    int reg[2];             /* per eightbyte: GPR or XMM argument register */
    Classification c;
} ParamPlace;

typedef struct {
    uint32_t size, align;
    bool incoming;          /* a parameter the caller left on the stack */
    int off;                /* frame: rbp - off (set when emitting); incoming: rbp + off */
} Slot;

typedef struct {
    Gen *g;
    const Symbol *fn;

    Ins *code;
    size_t n, cap;
    VKind *vk;              /* per vreg */
    int nv, vcap;
    Slot *slots;
    int nslots, slots_cap;

    /* lowering */
    PtrVec defers;
    size_t *scopes;
    size_t nscopes, scopes_cap;
    Loop *loops;
    size_t nloops, loops_cap;
    int exit_label;
    int retv;               /* scalar results go here */
    int ret_slot;           /* small aggregate results go here (-1 if not) */
    int hidden;             /* MEMORY-class results: the caller's buffer, a vreg (-1 if not) */
    ParamPlace *params;     /* the `this` parameter first for methods, then the declared ones */
    size_t nparams;
    PtrVec panic_jumps;     /* (label, site) pairs, as two ints in an allocated array */

    /* allocation */
    bool *is_imm;           /* per vreg: an int32 constant used directly as an operand */
    int64_t *imm_val;
    int *loc;               /* per vreg: >= 0 a register (0-15 GPR, 16-31 xmm), < 0: -(spill slot + 1) */
    bool used_reg[32];      /* registers the function writes (callee-saved ones get saved) */
} Fn;

/* opt_lower.c */
int ir_new_vreg(Fn *f, VKind k);
int ir_new_slot(Fn *f, uint32_t size, uint32_t align);
Ins *ir_emit(Fn *f, IrOp op);
void ir_lower_function(Fn *f);

/* opt_ra.c */
void ir_optimize(Fn *f);
void ir_allocate(Fn *f);
int ir_defs_uses(const Ins *in, int *defs, int *uses);   /* returns #uses written; defs gets d or -1 */
void ir_operand_ptrs(Ins *in, int **ptrs, int *n);       /* every vreg operand field that's a use */

/* opt_emit.c */
void ir_emit_function(Fn *f);

#endif
