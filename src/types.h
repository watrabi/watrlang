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
    TY_CLASS,    /* a reference (pointer) to a heap object; may be null */
    TY_MIXED,    /* any value, tagged: {u64 tag; u64 bits}, 16 bytes */
    TY_PHPARRAY, /* `array`: a reference to a GC'd ordered map; may be null */
} TypeKind;

typedef struct Type Type;
struct Decl;

typedef struct {
    const char *name;
    Loc loc;
    Type *type;
    uint32_t offset;
    struct Expr *init;       /* class fields: the default value, or NULL for zero */
    bool is_private;         /* class fields */
    bool is_protected;
    Type *owner;             /* class fields: the class that declares it */
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

    const struct Decl *decl; /* TY_STRUCT, TY_ENUM, TY_CLASS */
    Field *fields;           /* TY_STRUCT, TY_CLASS; TY_ERRUNION: is_err, err, value */
    size_t nfields;
    Type *err;               /* TY_ERRUNION: the error enum (base is the value type) */
    EnumVariant *variants;   /* TY_ENUM */
    size_t nvariants;
    struct Symbol *name_fn;  /* TY_ENUM: generated function returning a variant's name */
    int layout_state;        /* TY_STRUCT: 0 = not started, 1 = in progress, 2 = done */
    bool packed;             /* TY_STRUCT: #[packed], fields at any offset */
    uint32_t class_id;       /* TY_CLASS: identifies the class inside `mixed` values */
    uint32_t obj_size;       /* TY_CLASS: the object's size and alignment (the value is a pointer) */
    uint32_t obj_align;
    struct Symbol **methods; /* TY_CLASS: the ones it declares itself */
    size_t nmethods;
    Type *base_class;        /* TY_CLASS: `extends` */
    Type **ifaces;           /* TY_CLASS: every interface it implements (classes) or extends (interfaces), flattened */
    size_t nifaces;
    struct Symbol ***itables;   /* classes: per ifaces[i], the methods for that interface's vtable, or NULL */
    struct Symbol **vtable;  /* classes: virtual methods by slot; interfaces: all their methods, in order */
    size_t nvtable;
    bool is_interface, is_abstract;
    const char *desc_name;   /* the runtime descriptor (vtable, parent, interfaces) */
    struct Symbol *desc_sym; /* a pseudo-global for taking the descriptor's address */
    int class_state;         /* hierarchy/layout progress: 0 none, 1 in progress, 2 done */
    const struct Decl *tmpl; /* generic instances (`Box<int>`): the generic declaration */
    Type **targs;            /* ...and its type arguments */
    size_t ntargs;
    Loc inst_loc;            /* where it was first used */
};

extern Type *ty_error, *ty_void, *ty_bool, *ty_null;
extern Type *ty_int, *ty_i8, *ty_i16, *ty_i32, *ty_i64;
extern Type *ty_u8, *ty_u16, *ty_u32, *ty_u64, *ty_usize;
extern Type *ty_float, *ty_f32, *ty_f64;
extern Type *ty_string;
extern Type *ty_mixed, *ty_phparray;

void  types_init(void);
void  types_free(void);
Type *type_builtin(TokenKind keyword);
Type *type_ptr(Type *base, bool base_const, bool base_volatile);
Type *type_array(Type *elem, uint64_t len);
Type *type_new_struct(const char *name, const struct Decl *decl);
Type *type_new_enum(const char *name, const struct Decl *decl, Type *base);
Type *type_new_class(const char *name, const struct Decl *decl);
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
    return type_is_aggregate(t) || t->kind == TY_STRING || t->kind == TY_ERRUNION || t->kind == TY_MIXED;
}
/* Lives in a general-purpose register: ints, enums, bools, pointers, null. */
static inline bool type_in_gpr(const Type *t) {
    return t->kind == TY_INT || t->kind == TY_ENUM || t->kind == TY_BOOL || t->kind == TY_PTR || t->kind == TY_NULL ||
           t->kind == TY_CLASS || t->kind == TY_PHPARRAY;
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
/* Is class/interface `from` the same as `to`, derived from it, or implementing it? */
bool class_subtype(const Type *from, const Type *to);
/* Is `value as to` allowed? */
bool type_castable(const Type *from, const Type *to);

/* Range of an int type, as int64/uint64 bounds. */
int64_t  int_type_min(const Type *t);
uint64_t int_type_max(const Type *t);
/* Truncates/extends a 64-bit value to `t`'s width (two's complement). */
uint64_t int_normalize(const Type *t, uint64_t v);

#endif
