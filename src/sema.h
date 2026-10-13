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
    size_t ncallees, callees_cap;
    bool reachable;
    bool is_runtime;            /* from the runtime prelude */

    /* globals */
    unsigned char *init;        /* initial bytes (type->size of them); NULL = all zero */
    struct StrReloc *relocs;    /* string literal addresses inside `init` */
    size_t nrelocs;
    bool has_value;             /* const scalar global whose value folds into constants */
    uint64_t value_bits;        /* int/bool/pointer bits, or the float's bits */

    /* locals and parameters */
    int param_index;
    int offset;                 /* rbp-relative; assigned by codegen */

    int reads;                  /* for the unused-variable warning */
    int writes;
} Symbol;

typedef struct {
    Symbol **funcs;
    size_t nfuncs;
    Symbol **globals;
    size_t nglobals;
    bool needs_libc;            /* uses extern symbols or alloc/free: link with the C runtime */
} Program;

/* Returns false if errors were reported. */
bool sema_check(Arena *arena, Module *m, Program *prog);

#endif
