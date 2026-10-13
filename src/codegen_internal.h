#ifndef WATR_CODEGEN_INTERNAL_H
#define WATR_CODEGEN_INTERNAL_H

/*
 * Shared by the two code generators: codegen_x64.c (the module, data and
 * the simple -O0 stack-machine functions) and opt_*.c (-O1: an IR with
 * register allocation).
 */

#include <stdio.h>

#include "codegen.h"
#include "vec.h"

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
    int opt;                /* optimization level: 0 = stack machine, 1 = IR + register allocation */

    /* per function (-O0) */
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
    bool freestanding;      /* no OS: the program supplies watr_write (and maybe watr_panic) */
    const Program *prog;    /* the whole program (classes, for devirtualizing calls) */
    bool need_panic;        /* emit w.panic even without check sites */
    PtrVec panics;          /* PanicSite*: runtime check failure messages */
    PtrVec strings;         /* StringLit*: literals for .rodata */
    PtrVec strobjs;         /* StringLit*: {ptr, len} string values for .data.rel.ro (label is the object's) */
} Gen;

typedef enum { CLS_GPR, CLS_SSE, CLS_MEM, CLS_NONE } ValClass;

/* System V classification of a value passed in registers: up to two
 * eightbytes, each INTEGER or SSE; larger structs go in MEMORY. */
typedef enum { EB_NONE, EB_INT, EB_SSE } Eightbyte;

typedef struct {
    bool memory;
    int n;              /* eightbytes */
    Eightbyte eb[2];
} Classification;

void emit(Gen *g, const char *fmt, ...) WATR_PRINTF(2, 3);
int new_label(Gen *g);
void emit_label(Gen *g, int label);
ValClass cls(const Type *t);
Classification classify(const Type *t);
const char *size_ptr(uint32_t size);
int string_label(Gen *g, StrBuf bytes);
int string_obj_label(Gen *g, StrBuf bytes);   /* a `string` value {ptr, len} in read-only data */
int add_panic(Gen *g, const Expr *at, const char *what);
void emit_panic_call(Gen *g, int label);
const Field *eu_field(const Type *t, const char *name);
void emit_virtual_call(Gen *g, const char *self, int slot, const char *iface);

/* opt_*.c: one function at -O1. */
/* -O1 (opt_emit.c): prepare every function, then emit them one by one. */
typedef struct OptProgram OptProgram;
OptProgram *opt_prepare(Gen *g);
void opt_emit_function(OptProgram *p, size_t i);   /* prog->funcs[i] */
void opt_free(OptProgram *p);

#endif
