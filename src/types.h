#ifndef WATR_TYPES_H
#define WATR_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#include "diag.h"
#include "lexer.h"

/*
 * Semantic types. Built-in types are singletons; pointer and array types
 * are interned, so two `u8*` types are the same object. Aliases (`int` and
 * `i64`, `float` and `f64`, `usize` and `u64`) are distinct objects with the
 * same representation; type_equiv treats them as interchangeable.
 */

typedef enum {
    TY_ERROR,    /* an error was already reported; suppresses follow-on errors */
    TY_VOID,
    TY_BOOL,
    TY_INT,
    TY_FLOAT,
    TY_NULL,     /* the type of `null`, which converts to any pointer */
    TY_PTR,
    TY_ARRAY,
    TY_STRUCT,
    TY_STRING,   /* immutable {const u8* ptr; usize len}, 16 bytes */
    TY_ENUM,     /* named integer constants; represented as its base int type */
    TY_ERRUNION, /* T!E: {bool is_err; E err; T value} */
} TypeKind;

typedef struct Type Type;
struct Decl;

typedef struct {
    const char *name;
    Loc loc;
    Type *type;
    uint32_t offset;
} Field;

typedef struct {
    const char *name;
    Loc loc;
    int64_t value;
} EnumVariant;

struct Type {
    TypeKind kind;
    uint32_t size;
    uint32_t align;
    bool is_signed;          /* TY_INT */
    const char *name;        /* built-ins and structs */

    Type *base;              /* TY_PTR: pointee; TY_ARRAY: element */
    bool base_const;         /* TY_PTR: `const T*` */
    bool base_volatile;      /* TY_PTR: `volatile T*` */
    uint64_t len;            /* TY_ARRAY */

    const struct Decl *decl; /* TY_STRUCT, TY_ENUM */
    Field *fields;           /* TY_STRUCT; TY_ERRUNION: is_err, err, value */
    size_t nfields;
    Type *err;               /* TY_ERRUNION: the error enum (base is the value type) */
    EnumVariant *variants;   /* TY_ENUM */
    size_t nvariants;
    struct Symbol *name_fn;  /* TY_ENUM: generated function returning a variant's name */
    int layout_state;        /* TY_STRUCT: 0 = not started, 1 = in progress, 2 = done */
};

extern Type *ty_error, *ty_void, *ty_bool, *ty_null;
extern Type *ty_int, *ty_i8, *ty_i16, *ty_i32, *ty_i64;
extern Type *ty_u8, *ty_u16, *ty_u32, *ty_u64, *ty_usize;
extern Type *ty_float, *ty_f32, *ty_f64;
extern Type *ty_string;

void  types_init(void);
void  types_free(void);
Type *type_builtin(TokenKind keyword);   /* NULL for `array` (not yet supported) */
Type *type_ptr(Type *base, bool base_const, bool base_volatile);
Type *type_array(Type *elem, uint64_t len);
Type *type_new_struct(const char *name, const struct Decl *decl);
Type *type_new_enum(const char *name, const struct Decl *decl, Type *base);
Type *type_errunion(Type *value, Type *err);

/* "u8", "const u8*", "Vec2[4]" */
const char *type_name(const Type *t);

static inline bool type_is_int(const Type *t)     { return t->kind == TY_INT; }
static inline bool type_is_float(const Type *t)   { return t->kind == TY_FLOAT; }
static inline bool type_is_numeric(const Type *t) { return t->kind == TY_INT || t->kind == TY_FLOAT; }
static inline bool type_is_ptr(const Type *t)     { return t->kind == TY_PTR; }
static inline bool type_is_aggregate(const Type *t) { return t->kind == TY_ARRAY || t->kind == TY_STRUCT; }
/* Handled by address in codegen: structs, arrays and strings. */
static inline bool type_in_memory(const Type *t) {
    return type_is_aggregate(t) || t->kind == TY_STRING || t->kind == TY_ERRUNION;
}
/* Lives in a general-purpose register: ints, enums, bools, pointers, null. */
static inline bool type_in_gpr(const Type *t) {
    return t->kind == TY_INT || t->kind == TY_ENUM || t->kind == TY_BOOL || t->kind == TY_PTR || t->kind == TY_NULL;
}
/* Integer representation with is_signed/size: ints and enums. */
static inline bool type_is_intlike(const Type *t) { return t->kind == TY_INT || t->kind == TY_ENUM; }

/* Same representation and meaning: `int` ~ `i64`, `int*` ~ `i64*`. */
bool type_equiv(const Type *a, const Type *b);
/* Can every value of int type `from` be represented in `to`? */
bool int_widens(const Type *from, const Type *to);
/* Can a value of type `from` be used where `to` is expected, without `as`?
 * (Literals are handled separately: they adapt to their context.) */
bool type_assignable(const Type *from, const Type *to);
/* Is `value as to` allowed? */
bool type_castable(const Type *from, const Type *to);

/* Range of an int type, as int64/uint64 bounds. */
int64_t  int_type_min(const Type *t);
uint64_t int_type_max(const Type *t);
/* Truncates/extends a 64-bit value to `t`'s width (two's complement). */
uint64_t int_normalize(const Type *t, uint64_t v);

#endif
