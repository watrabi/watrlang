#ifndef WATR_SEMA_H
#define WATR_SEMA_H

#include "ast.h"
#include "types.h"

/*
 * Semantic analysis: resolves names and types, checks the rules, folds
 * constants, and annotates the AST (Expr.type, Expr.sym, VarDecl.sym, ...)
 * for the code generator. All "is this program valid?" errors come from
 * here; codegen assumes a checked tree.
 */

typedef enum { SYM_LOCAL, SYM_PARAM, SYM_GLOBAL, SYM_FUNC } SymKind;

/* A pointer to string literal data at `offset` in a global's initial bytes. */
typedef struct StrReloc {
    uint32_t offset;
    StrBuf str;
} StrReloc;

typedef struct Symbol {
    SymKind kind;
    const char *name;
    Loc loc;
    Type *type;                 /* variables: their type; functions: the return type */
    const TypeExpr *type_expr;  /* as written, for fixes like "remove `const`" */
    bool is_const;
    const Decl *decl;           /* functions and globals */
    const char *asm_name;       /* functions and globals: `w.name`, or the plain name for extern/export */
    bool is_extern;             /* defined outside watrlang (C, libc) */
    bool is_export;
    bool is_variadic;

    /* functions */
    Type **param_types;
    size_t nparams;

    /* functions: the call graph, for dropping code nothing uses */
    struct Symbol **callees;
    Loc *callee_locs;           /* where each callee is first called */
    size_t ncallees, callees_cap;
    bool reachable;
    bool is_runtime;            /* from the runtime prelude */
    int module;                 /* index of the defining module */
    bool is_private;            /* `private`: invisible to importing modules */
    Type *enum_type;            /* generated enum-name functions: the enum (decl is NULL) */
    Type *owner;                /* methods: their class */
    int vindex;                 /* virtual methods: vtable slot (interface methods: position); -1 otherwise */
    bool is_abstract;           /* abstract and interface methods: no body */
    bool is_protected;
    bool is_static;             /* methods without `this` */
    struct Symbol *this_param;  /* methods: the implicit first parameter */

    /* globals */
    unsigned char *init;        /* initial bytes (type->size of them); NULL = all zero */
    struct StrReloc *relocs;    /* string literal addresses inside `init` */
    size_t nrelocs;
    bool has_value;             /* const scalar global whose value folds into constants */
    uint32_t align;             /* #[align(N)] on a global; 0 = its type's alignment */
    uint64_t value_bits;        /* int/bool/pointer bits, or the float's bits */

    /* locals and parameters */
    int param_index;
    int offset;                 /* rbp-relative; assigned by codegen */

    bool addr_taken;            /* locals and parameters: `&x` appears, so x must live in memory */
    int vreg;                   /* -O1 codegen: the virtual register of a local in a register, or -1 */
    int slot;                   /* -O1 codegen: the frame slot of a local in memory */

    int reads;                  /* for the unused-variable warning */
    int writes;
} Symbol;

typedef struct {
    Symbol **funcs;
    size_t nfuncs;
    Symbol **globals;
    size_t nglobals;
    Symbol **enum_fns;          /* generated functions that turn an enum value into its name */
    Type **classes;             /* classes and interfaces, for their runtime descriptors */
    size_t nclasses;
    size_t nenum_fns;
    bool needs_libc;            /* uses extern C symbols or prints floats: link with the C runtime */
    bool freestanding;          /* input: --freestanding (no libc, no heap, no entry point) */
    int opt;                    /* input: optimization level (-O0, -O1) */
} Program;

/* An inline asm operand register: 0-15 for rax..r15 (any width), 16-31
 * for xmm0..xmm15, -1 if it isn't one. */
int asm_register(const char *name, size_t len);

/* Checks a whole program: mods[0] is the main file, the runtime prelude is
 * last. Returns false if errors were reported. */
bool sema_check(Arena *arena, Module **mods, size_t nmods, Program *prog);

#endif
