#include "codegen_internal.h"

#include <stdlib.h>
#include <string.h>

#include "sema.h"
#include "types.h"

/*
 * The Windows target (x86_64-windows).
 *
 * watrlang functions keep calling each other with the System V
 * convention, exactly as on Linux, so neither backend changes. The
 * Microsoft x64 convention only matters where C code is involved, and
 * there this file puts a thunk in between:
 *
 *   extern calls   watrlang -> thunk (System V in, Win64 out) -> C
 *   exports        C -> thunk (Win64 in, System V out) -> watrlang
 *   main           the C runtime's main(argc, argv, envp) -> w.main
 *
 * Win64 in brief: four argument slots in rcx, rdx, r8, r9 (or xmm0-3 for
 * floats, by position), the rest on the stack after 32 bytes of "shadow
 * space" the caller always reserves; structs of 1, 2, 4 or 8 bytes travel
 * as integers, any other struct as a pointer to a copy; results come back
 * in rax or xmm0, or through a hidden pointer in the first slot; variadic
 * floats also go in the integer register. rsi, rdi and xmm6-15 are
 * callee-saved, unlike System V, so a thunk that calls watrlang code
 * saves them. (Win64 callees preserve a superset of what System V callers
 * expect, so the other direction needs no saving.)
 *
 * The runtime's OS primitives call the C runtime (msvcrt) and kernel32;
 * programs are always linked with the C runtime on Windows.
 */

typedef struct {
    const Symbol *fn;
    const Type **types;     /* the arguments as passed at the call */
    size_t n;
    char *name;
} Win64Thunk;

static bool is_agg(const Type *t) {
    return type_in_memory(t);
}

/* A struct Win64 passes and returns in a register, as an integer. */
static bool win_small(const Type *t) {
    return t->size == 1 || t->size == 2 || t->size == 4 || t->size == 8;
}

static uint32_t round8(uint32_t n) {
    return (n + 7) & ~7u;
}

static int align16(int n) {
    return (n + 15) & ~15;
}

static const char *const WIN_GPR[4] = {"rcx", "rdx", "r8", "r9"};
static const char *const SYSV_GPR[6] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};

/* Where System V puts each argument (the same rules as the call sites in
 * codegen_x64.c and opt_emit.c). Returns the stack bytes used. */
typedef struct {
    bool on_stack;
    int stack_off;
    int reg[2];
    Classification c;
    uint32_t size;
} SvPlace;

static int sysv_places(const Type *const *types, size_t n, bool ret_memory, SvPlace *p) {
    int ngpr = ret_memory ? 1 : 0, nsse = 0, stack = 0;
    for (size_t i = 0; i < n; i++) {
        p[i].c = classify(types[i]);
        p[i].size = is_agg(types[i]) ? round8(types[i]->size) : 8;
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
    return stack;
}

/* ---- extern calls: System V in, Win64 out ----------------------------------- */

const char *win64_extern_thunk(Gen *g, const Symbol *fn, const Type **types, size_t n) {
    for (size_t i = 0; i < g->thunks.len; i++) {
        Win64Thunk *t = g->thunks.items[i];
        if (t->fn != fn || t->n != n) continue;
        bool same = true;
        for (size_t k = 0; k < n && same; k++) same = t->types[k] == types[k];
        if (same) return t->name;
    }
    Win64Thunk *t = xmalloc(sizeof *t);
    t->fn = fn;
    t->n = n;
    t->types = xmalloc((n ? n : 1) * sizeof *t->types);
    memcpy(t->types, types, n * sizeof *types);
    size_t len = strlen(fn->asm_name) + 48;
    t->name = xmalloc(len);
    snprintf(t->name, len, "w.win64.%s.%zu", fn->asm_name, g->thunks.len);
    ptrvec_push(&g->thunks, t);
    return t->name;
}

/*
 * Frame of an extern thunk:
 *   [rbp + 16 + k]       System V stack arguments, as the caller left them
 *   [rbp - 8 - 8k]       System V integer argument register k (rdi, rsi, ...)
 *   [rbp - 56 - 8k]      xmm k
 *   below that           copies of structs Win64 passes by pointer, the result buffer
 *   [rsp + 8s]           Win64 argument slot s (0-3 are shadow space)
 */
static void sysv_src(char *buf, size_t cap, const SvPlace *p, int j) {
    if (p->on_stack) snprintf(buf, cap, "qword ptr [rbp + %d]", 16 + p->stack_off + 8 * j);
    else if (p->c.eb[j] == EB_INT) snprintf(buf, cap, "qword ptr [rbp - %d]", 8 + 8 * p->reg[j]);
    else snprintf(buf, cap, "qword ptr [rbp - %d]", 56 + 8 * p->reg[j]);
}

static void emit_extern_thunk(Gen *g, const Win64Thunk *t) {
    const Symbol *fn = t->fn;
    const Type *ret = fn->type;
    size_t n = t->n;
    bool ret_agg = ret->kind != TY_VOID && is_agg(ret);
    Classification rc = classify(ret);
    bool win_ret_mem = ret_agg && !win_small(ret);
    size_t slot0 = win_ret_mem ? 1 : 0;

    SvPlace *p = xmalloc((n ? n : 1) * sizeof *p);
    sysv_places(t->types, n, rc.memory, p);

    int cur = 112;
    int *copy = xmalloc((n ? n : 1) * sizeof *copy);
    for (size_t i = 0; i < n; i++) {
        copy[i] = 0;
        if (is_agg(t->types[i]) && !win_small(t->types[i]) && !p[i].on_stack) {
            cur = align16(cur + (int)round8(t->types[i]->size));
            copy[i] = cur;
        }
    }
    int rbuf = 0;
    if (ret_agg) {
        cur = align16(cur + (int)(round8(ret->size) < 16 ? 16 : round8(ret->size)));
        rbuf = cur;
    }
    size_t nslots = n + slot0 < 4 ? 4 : n + slot0;
    int frame = align16(cur + 8 * (int)nslots);

    emit_func_begin(g, t->name, false);
    emit(g, "push rbp");
    emit(g, "mov rbp, rsp");
    emit(g, "sub rsp, %d", frame);
    for (int k = 0; k < 6; k++) emit(g, "mov qword ptr [rbp - %d], %s", 8 + 8 * k, SYSV_GPR[k]);
    for (int k = 0; k < 8; k++) emit(g, "movsd qword ptr [rbp - %d], xmm%d", 56 + 8 * k, k);

    char src[64];
    for (size_t i = 0; i < n; i++) {   /* structs Win64 wants by pointer, from registers into one piece */
        if (!copy[i]) continue;
        for (int j = 0; j < p[i].c.n; j++) {
            sysv_src(src, sizeof src, &p[i], j);
            emit(g, "mov rax, %s", src);
            emit(g, "mov qword ptr [rbp - %d], rax", copy[i] - 8 * j);
        }
    }
    if (win_ret_mem) emit(g, "lea rcx, [rbp - %d]", rbuf);
    for (size_t i = 0; i < n; i++) {
        const Type *ty = t->types[i];
        size_t s = i + slot0;
        if (is_agg(ty) && !win_small(ty)) {   /* by pointer */
            if (p[i].on_stack) emit(g, "lea r11, [rbp + %d]", 16 + p[i].stack_off);
            else emit(g, "lea r11, [rbp - %d]", copy[i]);
            if (s < 4) emit(g, "mov %s, r11", WIN_GPR[s]);
            else emit(g, "mov qword ptr [rsp + %zu], r11", 8 * s);
            continue;
        }
        /* A scalar, or a small struct's bytes: its first eightbyte. */
        sysv_src(src, sizeof src, &p[i], 0);
        bool is_float = !is_agg(ty) && cls(ty) == CLS_SSE;
        if (s >= 4) {
            emit(g, "mov rax, %s", src);
            emit(g, "mov qword ptr [rsp + %zu], rax", 8 * s);
            continue;
        }
        if (is_float) emit(g, "movsd xmm%zu, %s", s, src);
        if (!is_float || (fn->is_variadic && i >= fn->nparams)) emit(g, "mov %s, %s", WIN_GPR[s], src);
    }
    emit(g, "call %s", fn->asm_name);

    if (ret_agg) {
        if (!win_ret_mem) emit(g, "mov qword ptr [rbp - %d], rax", rbuf);
        if (rc.memory) {   /* into the caller's buffer, whose address System V returns */
            emit(g, "mov rdi, qword ptr [rbp - 8]");
            emit(g, "lea rsi, [rbp - %d]", rbuf);
            emit(g, "mov ecx, %u", ret->size);
            emit(g, "rep movsb");
            emit(g, "mov rax, qword ptr [rbp - 8]");
        } else {
            int gi = 0, si = 0;
            for (int j = 0; j < rc.n; j++) {
                if (rc.eb[j] == EB_INT) emit(g, "mov %s, qword ptr [rbp - %d]", gi++ ? "rdx" : "rax", rbuf - 8 * j);
                else emit(g, "movsd xmm%d, qword ptr [rbp - %d]", si++, rbuf - 8 * j);
            }
        }
    }
    emit(g, "leave");
    emit(g, "ret");
    emit_func_end(g, t->name);
    free(copy);
    free(p);
}

void win64_emit_thunks(Gen *g) {
    if (!g->thunks.len) return;
    fputs("\n# Calls into C, converted to the Win64 convention.\n    .text\n", g->out);
    for (size_t i = 0; i < g->thunks.len; i++) {
        Win64Thunk *t = g->thunks.items[i];
        emit_extern_thunk(g, t);
        free(t->types);
        free(t->name);
        free(t);
    }
}

/* ---- exports: Win64 in, System V out ----------------------------------------- */

/* rsi, rdi and xmm6-15 belong to the C caller; watrlang code may use them. */
static void save_win64_callee_saved(Gen *g) {
    emit(g, "mov qword ptr [rbp - 8], rsi");
    emit(g, "mov qword ptr [rbp - 16], rdi");
    for (int k = 0; k < 10; k++) emit(g, "movdqu xmmword ptr [rbp - %d], xmm%d", 32 + 16 * k, 6 + k);
}

static void restore_win64_callee_saved(Gen *g) {
    emit(g, "mov rsi, qword ptr [rbp - 8]");
    emit(g, "mov rdi, qword ptr [rbp - 16]");
    for (int k = 0; k < 10; k++) emit(g, "movdqu xmm%d, xmmword ptr [rbp - %d]", 6 + k, 32 + 16 * k);
}

/*
 * Frame of an export thunk:
 *   [rbp + 16 + 8s]      Win64 argument slot s: the caller's shadow space
 *                        (filled from the registers here) and stack arguments
 *   [rbp - 8 .. - 176]   the caller's rsi, rdi, xmm6-15
 *   below that           struct copies, the result buffer
 *   [rsp + k]            System V stack arguments
 */
void win64_emit_export_thunk(Gen *g, const Symbol *fn) {
    const Type *ret = fn->type;
    const Type *const *types = (const Type *const *)fn->param_types;
    size_t n = fn->nparams;
    bool ret_agg = ret->kind != TY_VOID && is_agg(ret);
    Classification rc = classify(ret);
    bool win_ret_mem = ret_agg && !win_small(ret);
    size_t slot0 = win_ret_mem ? 1 : 0;

    SvPlace *p = xmalloc((n ? n : 1) * sizeof *p);
    int stack = sysv_places(types, n, rc.memory, p);

    int cur = 176;
    int *buf = xmalloc((n ? n : 1) * sizeof *buf);
    for (size_t i = 0; i < n; i++) {
        buf[i] = 0;
        if (is_agg(types[i]) && !win_small(types[i]) && !p[i].on_stack) {
            cur = align16(cur + (int)round8(types[i]->size));
            buf[i] = cur;
        }
    }
    int rbuf = 0;
    if (ret_agg) {
        cur = align16(cur + (int)(round8(ret->size) < 16 ? 16 : round8(ret->size)));
        rbuf = cur;
    }
    int frame = align16(cur + stack);

    fprintf(g->out, "\n# `%s` as C on Windows calls it.\n", fn->name);
    emit_func_begin(g, fn->name, true);
    emit(g, "push rbp");
    emit(g, "mov rbp, rsp");
    emit(g, "sub rsp, %d", frame);
    for (size_t s = 0; s < 4; s++) {   /* the register slots, into the shadow space */
        size_t i = s - slot0;
        bool is_float = s >= slot0 && i < n && !is_agg(types[i]) && cls(types[i]) == CLS_SSE;
        if (is_float) emit(g, "movsd qword ptr [rbp + %zu], xmm%zu", 16 + 8 * s, s);
        else emit(g, "mov qword ptr [rbp + %zu], %s", 16 + 8 * s, WIN_GPR[s]);
    }
    save_win64_callee_saved(g);

    /* Structs first: copying uses rsi, rdi and rcx. */
    for (size_t i = 0; i < n; i++) {
        size_t at = 16 + 8 * (i + slot0);
        const Type *ty = types[i];
        if (!is_agg(ty)) {
            if (p[i].on_stack) {
                emit(g, "mov rax, qword ptr [rbp + %zu]", at);
                emit(g, "mov qword ptr [rsp + %d], rax", p[i].stack_off);
            }
            continue;
        }
        if (!p[i].on_stack && !buf[i]) continue;   /* small: read straight from its slot below */
        if (win_small(ty)) emit(g, "lea rsi, [rbp + %zu]", at);
        else emit(g, "mov rsi, qword ptr [rbp + %zu]", at);
        if (p[i].on_stack) emit(g, "lea rdi, [rsp + %d]", p[i].stack_off);
        else emit(g, "lea rdi, [rbp - %d]", buf[i]);
        emit(g, "mov ecx, %u", ty->size);
        emit(g, "rep movsb");
    }
    for (size_t i = 0; i < n; i++) {
        if (p[i].on_stack) continue;
        size_t at = 16 + 8 * (i + slot0);
        char base[32];
        if (buf[i]) snprintf(base, sizeof base, "rbp - %d", buf[i]);
        else snprintf(base, sizeof base, "rbp + %zu", at);
        for (int j = 0; j < p[i].c.n; j++) {
            /* rbp - buf + 8j: the eightbytes go up from the buffer's start */
            char addr[48];
            if (buf[i]) snprintf(addr, sizeof addr, "rbp - %d", buf[i] - 8 * j);
            else snprintf(addr, sizeof addr, "%s", base);
            if (p[i].c.eb[j] == EB_INT) emit(g, "mov %s, qword ptr [%s]", SYSV_GPR[p[i].reg[j]], addr);
            else emit(g, "movsd xmm%d, qword ptr [%s]", p[i].reg[j], addr);
        }
    }
    if (rc.memory) {
        if (win_ret_mem) emit(g, "mov rdi, qword ptr [rbp + 16]");   /* straight into the caller's buffer */
        else emit(g, "lea rdi, [rbp - %d]", rbuf);
    }
    emit(g, "call %s", fn->asm_name);

    if (ret_agg) {
        if (!rc.memory) {   /* the eightbytes into the buffer */
            int gi = 0, si = 0;
            for (int j = 0; j < rc.n; j++) {
                if (rc.eb[j] == EB_INT) emit(g, "mov qword ptr [rbp - %d], %s", rbuf - 8 * j, gi++ ? "rdx" : "rax");
                else emit(g, "movsd qword ptr [rbp - %d], xmm%d", rbuf - 8 * j, si++);
            }
        }
        if (!win_ret_mem) {
            emit(g, "mov rax, qword ptr [rbp - %d]", rbuf);
        } else {
            if (!rc.memory) {
                emit(g, "mov rdi, qword ptr [rbp + 16]");
                emit(g, "lea rsi, [rbp - %d]", rbuf);
                emit(g, "mov ecx, %u", ret->size);
                emit(g, "rep movsb");
            }
            emit(g, "mov rax, qword ptr [rbp + 16]");
        }
    }
    restore_win64_callee_saved(g);
    emit(g, "leave");
    emit(g, "ret");
    emit_func_end(g, fn->name);
    free(buf);
    free(p);
}

/* ---- process entry and runtime primitives ------------------------------------ */

void win64_emit_entry(Gen *g) {
    /* The C runtime's startup code calls main(argc, argv, envp). stdout and
     * stderr are switched to binary mode, so "\n" is written as is, the
     * same bytes as on Linux (and as Rust and Go programs do). */
    fputs("\n# C entry point: the C runtime calls this, and exits with its result.\n", g->out);
    emit_func_begin(g, "main", true);
    emit(g, "push rbp");
    emit(g, "mov rbp, rsp");
    emit(g, "sub rsp, 208");   /* rsi, rdi, xmm6-15, then 32 bytes of shadow space */
    save_win64_callee_saved(g);
    emit(g, "mov qword ptr [rip + w.__rt_stack_base], rbp");
    emit(g, "mov qword ptr [rip + w.__rt_argc], rcx");
    emit(g, "mov qword ptr [rip + w.__rt_argv], rdx");
    emit(g, "mov qword ptr [rip + w.__rt_envp], r8");
    for (int fd = 1; fd <= 2; fd++) {
        emit(g, "mov ecx, %d", fd);
        emit(g, "mov edx, 0x8000");   /* _O_BINARY */
        emit(g, "call _setmode");
    }
    emit(g, "call w.main");
    restore_win64_callee_saved(g);
    emit(g, "leave");
    emit(g, "ret");
}

/* w.panic(msg = rdi, len = rsi): flush stdout, print to stderr, exit(101). */
void win64_emit_panic(Gen *g) {
    emit(g, "mov rbx, rdi");   /* panic never returns, so rbx and r12 are free */
    emit(g, "mov r12, rsi");
    emit(g, "and rsp, -16");
    emit(g, "sub rsp, 32");
    emit(g, "xor ecx, ecx");
    emit(g, "call fflush");
    emit(g, "mov ecx, 2");
    emit(g, "call __acrt_iob_func");   /* stderr */
    emit(g, "mov rcx, rbx");
    emit(g, "mov edx, 1");
    emit(g, "mov r8, r12");
    emit(g, "mov r9, rax");
    emit(g, "call fwrite");
    emit(g, "mov ecx, 101");
    emit(g, "call exit");
}

bool win64_runtime_primitive(Gen *g, const char *name) {
    char label[96];
    snprintf(label, sizeof label, "w.%s", name);
    if (strcmp(name, "__rt_exit") == 0) {   /* exit() flushes stdio */
        emit_func_begin(g, label, false);
        emit(g, "and rsp, -16");
        emit(g, "sub rsp, 32");
        emit(g, "mov ecx, edi");
        emit(g, "call exit");
    } else if (strcmp(name, "__rt_out") == 0) {   /* fwrite(p, 1, n, stdout), in order with C's printf */
        emit_func_begin(g, label, false);
        emit(g, "push rbp");
        emit(g, "mov rbp, rsp");
        emit(g, "push rdi");
        emit(g, "push rsi");
        emit(g, "sub rsp, 32");
        emit(g, "mov ecx, 1");
        emit(g, "call __acrt_iob_func");   /* stdout */
        emit(g, "mov rcx, qword ptr [rbp - 8]");
        emit(g, "mov edx, 1");
        emit(g, "mov r8, qword ptr [rbp - 16]");
        emit(g, "mov r9, rax");
        emit(g, "call fwrite");
        emit(g, "leave");
        emit(g, "ret");
    } else if (strcmp(name, "__rt_mmap") == 0) {   /* VirtualAlloc: zeroed pages; -ENOMEM like mmap on failure */
        emit_func_begin(g, label, false);
        emit(g, "push rbp");
        emit(g, "mov rbp, rsp");
        emit(g, "sub rsp, 32");
        emit(g, "xor ecx, ecx");
        emit(g, "mov rdx, rdi");
        emit(g, "mov r8d, 0x3000");   /* MEM_COMMIT | MEM_RESERVE */
        emit(g, "mov r9d, 4");        /* PAGE_READWRITE */
        emit(g, "call VirtualAlloc");
        emit(g, "test rax, rax");
        emit(g, "jnz 1f");
        emit(g, "mov rax, -12");
        fputs("1:\n", g->out);
        emit(g, "leave");
        emit(g, "ret");
    } else if (strcmp(name, "__rt_fmt_f64") == 0) {
        /* mingw's C99 snprintf: msvcrt's prints 1e+020 and 1.#INF */
        emit_func_begin(g, label, false);
        emit(g, "push rbp");
        emit(g, "mov rbp, rsp");
        emit(g, "sub rsp, 32");
        emit(g, "mov rcx, rdi");
        emit(g, "mov edx, 32");
        emit(g, "lea r8, [rip + .Lrt_fmt_g]");
        emit(g, "movq r9, xmm0");
        emit(g, "movaps xmm3, xmm0");
        emit(g, "call __mingw_snprintf");
        emit(g, "mov eax, eax");
        emit(g, "leave");
        emit(g, "ret");
        fprintf(g->out, "    %s\n.Lrt_fmt_g:\n    .asciz \"%%.14g\"\n    .text\n", section(SEC_RODATA));
    } else {
        return false;
    }
    return true;
}
