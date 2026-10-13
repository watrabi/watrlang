#ifndef WATR_AST_H
#define WATR_AST_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "diag.h"
#include "lexer.h"

/*
 * Syntax tree produced by the parser. Every node lives in the parser's arena.
 * Lists are arena arrays of pointers plus a count.
 */

typedef struct TypeExpr TypeExpr;
typedef struct Expr Expr;
struct Type;     /* types.h */
struct Symbol;   /* sema.h */
typedef struct Stmt Stmt;
typedef struct Decl Decl;

/* Decoded string bytes (escapes resolved). May contain NUL; `len` is authoritative. */
typedef struct {
    const char *data;
    size_t len;
} StrBuf;

/* ---- types as written in source ---------------------------------------- */

typedef enum {
    TYPEX_BUILTIN,      /* int, u8, string, ...       u.builtin */
    TYPEX_NAMED,        /* Point, IoError              u.name */
    TYPEX_PTR,          /* T*                          u.pointee */
    TYPEX_ARRAY,        /* T[N]                        u.array */
    TYPEX_SLICE,        /* T[]                         u.array.elem */
    TYPEX_ERROR_UNION,  /* T!E                         u.error_union */
} TypeExprKind;

struct TypeExpr {
    TypeExprKind kind;
    Loc loc;
    bool is_const;      /* `const T` */
    bool is_volatile;   /* `volatile T` (usually seen as `volatile T*`) */
    union {
        TokenKind builtin;
        const char *name;
        TypeExpr *pointee;
        struct { TypeExpr *elem; Expr *len; } array;
        struct { TypeExpr *value; TypeExpr *error; } error_union;
    } u;
};

/* ---- expressions --------------------------------------------------------- */

enum { BUILTIN_NONE, BUILTIN_FREE };

typedef enum {
    EXPR_INT,         /* u.int_val */
    EXPR_FLOAT,       /* u.float_val */
    EXPR_CHAR,        /* u.int_val */
    EXPR_STRING,      /* u.str */
    EXPR_INTERP,      /* "Hi {name}": u.interp, parts are EXPR_STRING or any expr */
    EXPR_BOOL,        /* u.bool_val */
    EXPR_NULL,
    EXPR_THIS,
    EXPR_IDENT,       /* u.name */
    EXPR_SCOPE,       /* Enum::Value: u.scope */
    EXPR_UNARY,       /* -x !x ~x &x *x ++x --x: u.unary */
    EXPR_POSTFIX,     /* x++ x--: u.unary */
    EXPR_BINARY,      /* u.binary */
    EXPR_ASSIGN,      /* = += -= ... .=: u.binary */
    EXPR_TERNARY,     /* u.ternary */
    EXPR_CALL,        /* u.call */
    EXPR_INDEX,       /* a[i], or a[] (index == NULL) for PHP-style append */
    EXPR_MEMBER,      /* obj->name: u.member */
    EXPR_CAST,        /* x as T: u.cast */
    EXPR_NEW,         /* new T(args): u.new_ */
    EXPR_ALLOC,       /* alloc(T) / alloc(T, n): u.alloc */
    EXPR_SIZEOF,      /* sizeof(T): u.sizeof_type */
    EXPR_TRY,         /* expr? (error propagation): u.try_expr */
    EXPR_ARRAY_LIT,   /* [a, b, k => v]: u.array */
    EXPR_STRUCT_LIT,  /* Vec2 { x: 1.0, y: 2.0 }: u.struct_lit */
} ExprKind;

typedef struct {
    Expr *key;    /* NULL for positional entries */
    Expr *value;
} ArrayEntry;

typedef struct {
    const char *name;
    Loc loc;
    Expr *value;
} FieldInit;

struct Expr {
    ExprKind kind;
    Loc loc;
    bool parenthesized;   /* written as (expr); used to allow (a < b) == c */

    /* Filled in by sema: */
    struct Type *type;    /* the expression's type */
    struct Symbol *sym;   /* EXPR_IDENT: the variable; EXPR_CALL: the function */
    bool implicit;        /* EXPR_CAST inserted by sema (e.g. f32 -> f64) */
    uint32_t field_offset;   /* EXPR_MEMBER */
    uint8_t builtin;         /* EXPR_CALL: BUILTIN_* for compiler-provided functions */
    bool via_pointer;        /* EXPR_MEMBER: obj is a pointer to the struct */

    union {
        uint64_t int_val;
        double float_val;
        bool bool_val;
        StrBuf str;
        const char *name;
        struct { const char *scope; const char *member; } scope;
        struct { TokenKind op; Expr *operand; } unary;
        struct { TokenKind op; Expr *lhs; Expr *rhs; } binary;
        struct { Expr *cond; Expr *then_expr; Expr *else_expr; } ternary;
        struct { Expr *callee; Expr **args; size_t nargs; } call;
        struct { Expr *base; Expr *index; } index;
        struct { Expr *object; const char *name; Loc name_loc; } member;
        struct { Expr *expr; TypeExpr *type; } cast;
        struct { TypeExpr *type; Expr **args; size_t nargs; } new_;
        struct { TypeExpr *type; Expr *count; } alloc;
        TypeExpr *sizeof_type;
        Expr *try_expr;
        struct { ArrayEntry **entries; size_t n; } array;
        struct { const char *type_name; FieldInit **fields; size_t n; } struct_lit;
        struct { Expr **parts; size_t n; } interp;
    } u;
};

/* ---- statements ---------------------------------------------------------- */

typedef struct {
    TypeExpr *type;   /* NULL only for untyped foreach bindings */
    const char *name;
    Loc loc;          /* location of the name */
    Expr *init;       /* NULL if none */
    struct Symbol *sym;   /* filled in by sema */
} VarDecl;

typedef enum { ASM_IN, ASM_OUT, ASM_INOUT } AsmDir;

typedef struct {
    AsmDir dir;
    StrBuf reg;
    Expr *expr;
    Loc loc;
} AsmOperand;

typedef enum {
    STMT_BLOCK,
    STMT_VAR,
    STMT_EXPR,
    STMT_IF,
    STMT_WHILE,
    STMT_DO_WHILE,
    STMT_FOR,
    STMT_FOREACH,
    STMT_RETURN,
    STMT_BREAK,
    STMT_CONTINUE,
    STMT_ECHO,
    STMT_DEFER,
    STMT_ASM,
} StmtKind;

struct Stmt {
    StmtKind kind;
    Loc loc;
    union {
        struct { Stmt **stmts; size_t n; } block;
        VarDecl *var;
        Expr *expr;   /* STMT_EXPR; STMT_RETURN (NULL for bare `return;`) */
        struct { Expr *cond; Stmt *then_stmt; Stmt *else_stmt; } if_;
        struct { Expr *cond; Stmt *body; } while_;    /* WHILE and DO_WHILE */
        struct { Stmt *init; Expr *cond; Expr *step; Stmt *body; } for_;  /* any part may be NULL */
        struct { Expr *iterable; VarDecl *key; VarDecl *value; Stmt *body; } foreach_;  /* key may be NULL */
        struct { Expr **exprs; size_t n; } echo;
        Stmt *defer_body;
        struct { StrBuf **lines; size_t nlines; AsmOperand **ops; size_t nops; } asm_;
    } u;
};

/* ---- declarations -------------------------------------------------------- */

typedef enum { VIS_DEFAULT, VIS_PUBLIC, VIS_PRIVATE } Visibility;

typedef struct {
    TypeExpr *type;
    const char *name;
    Loc loc;
    struct Symbol *sym;   /* filled in by sema */
} Param;

typedef struct {
    const char *name;
    Loc loc;
    Expr *value;   /* NULL = previous + 1 */
} EnumItem;

typedef enum {
    DECL_IMPORT,
    DECL_FUNC,
    DECL_STRUCT,
    DECL_CLASS,
    DECL_ENUM,
    DECL_VAR,
} DeclKind;

struct Decl {
    DeclKind kind;
    Loc loc;
    const char *name;      /* NULL for imports */
    Loc name_loc;
    bool is_export;
    bool is_extern;
    Visibility vis;        /* class members only */
    struct Symbol *sym;    /* filled in by sema: functions and globals */
    struct Type *type;     /* filled in by sema: structs */
    union {
        StrBuf import_path;
        struct {
            Param **params;
            size_t nparams;
            bool variadic;      /* trailing `...` */
            TypeExpr *ret;      /* NULL = no return type written (void) */
            Stmt *body;         /* NULL for extern functions */
        } func;
        struct { VarDecl **fields; size_t n; } struct_;
        struct { Decl **members; size_t n; } class_;   /* DECL_VAR fields, DECL_FUNC methods */
        struct { TypeExpr *base; EnumItem **items; size_t n; } enum_;
        VarDecl *var;
    } u;
};

typedef struct {
    const SrcFile *file;
    Decl **decls;
    size_t n;
} Module;

void ast_print_module(FILE *out, const Module *m);

#endif
