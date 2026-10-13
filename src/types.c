#include "types.h"

#include <stdio.h>
#include <string.h>

#include "vec.h"

static Arena types_arena;
static PtrVec derived;   /* interned pointer and array types */

static Type builtin_storage[16];
Type *ty_error, *ty_void, *ty_bool, *ty_null;
Type *ty_int, *ty_i8, *ty_i16, *ty_i32, *ty_i64;
Type *ty_u8, *ty_u16, *ty_u32, *ty_u64, *ty_usize;
Type *ty_float, *ty_f32, *ty_f64;

static Type *mk(int i, TypeKind kind, const char *name, uint32_t size, bool is_signed) {
    Type *t = &builtin_storage[i];
    memset(t, 0, sizeof *t);
    t->kind = kind;
    t->name = name;
    t->size = size;
    t->align = size ? size : 1;
    t->is_signed = is_signed;
    return t;
}

void types_init(void) {
    ty_error = mk(0, TY_ERROR, "{error}", 8, false);
    ty_void  = mk(1, TY_VOID, "void", 0, false);
    ty_bool  = mk(2, TY_BOOL, "bool", 1, false);
    ty_null  = mk(3, TY_NULL, "null", 8, false);
    ty_int   = mk(4, TY_INT, "int", 8, true);
    ty_i8    = mk(5, TY_INT, "i8", 1, true);
    ty_i16   = mk(6, TY_INT, "i16", 2, true);
    ty_i32   = mk(7, TY_INT, "i32", 4, true);
    ty_i64   = mk(8, TY_INT, "i64", 8, true);
    ty_u8    = mk(9, TY_INT, "u8", 1, false);
    ty_u16   = mk(10, TY_INT, "u16", 2, false);
    ty_u32   = mk(11, TY_INT, "u32", 4, false);
    ty_u64   = mk(12, TY_INT, "u64", 8, false);
    ty_usize = mk(13, TY_INT, "usize", 8, false);
    ty_float = mk(14, TY_FLOAT, "float", 8, true);
    ty_f32   = mk(15, TY_FLOAT, "f32", 4, true);
    ty_f64   = ty_float;  /* replaced below so `f64` keeps its own name */
    static Type f64_storage;
    f64_storage = *ty_float;
    f64_storage.name = "f64";
    ty_f64 = &f64_storage;
}

void types_free(void) {
    free(derived.items);
    derived.items = NULL;
    derived.len = derived.cap = 0;
    arena_free(&types_arena);
}

Type *type_builtin(TokenKind kw) {
    switch (kw) {
    case TOK_KW_VOID:    return ty_void;
    case TOK_KW_BOOL:    return ty_bool;
    case TOK_KW_INT_T:   return ty_int;
    case TOK_KW_FLOAT_T: return ty_float;
    case TOK_KW_I8:      return ty_i8;
    case TOK_KW_I16:     return ty_i16;
    case TOK_KW_I32:     return ty_i32;
    case TOK_KW_I64:     return ty_i64;
    case TOK_KW_U8:      return ty_u8;
    case TOK_KW_U16:     return ty_u16;
    case TOK_KW_U32:     return ty_u32;
    case TOK_KW_U64:     return ty_u64;
    case TOK_KW_USIZE:   return ty_usize;
    case TOK_KW_F32:     return ty_f32;
    case TOK_KW_F64:     return ty_f64;
    default:             return NULL;   /* string, array: later milestones */
    }
}

Type *type_ptr(Type *base, bool base_const, bool base_volatile) {
    for (size_t i = 0; i < derived.len; i++) {
        Type *t = derived.items[i];
        if (t->kind == TY_PTR && t->base == base && t->base_const == base_const && t->base_volatile == base_volatile)
            return t;
    }
    Type *t = ARENA_NEW(&types_arena, Type);
    t->kind = TY_PTR;
    t->size = t->align = 8;
    t->base = base;
    t->base_const = base_const;
    t->base_volatile = base_volatile;
    ptrvec_push(&derived, t);
    return t;
}

Type *type_array(Type *elem, uint64_t len) {
    for (size_t i = 0; i < derived.len; i++) {
        Type *t = derived.items[i];
        if (t->kind == TY_ARRAY && t->base == elem && t->len == len) return t;
    }
    Type *t = ARENA_NEW(&types_arena, Type);
    t->kind = TY_ARRAY;
    t->base = elem;
    t->len = len;
    uint64_t size = elem->size * len;
    t->size = size > UINT32_MAX ? UINT32_MAX : (uint32_t)size;
    t->align = elem->align ? elem->align : 1;
    ptrvec_push(&derived, t);
    return t;
}

Type *type_new_struct(const char *name, const struct Decl *decl) {
    Type *t = ARENA_NEW(&types_arena, Type);
    t->kind = TY_STRUCT;
    t->name = name;
    t->decl = decl;
    t->align = 1;
    return t;
}

static void append_name(char *buf, size_t cap, size_t *len, const Type *t) {
    switch (t->kind) {
    case TY_PTR:
        if (t->base_const) *len += (size_t)snprintf(buf + *len, cap - *len, "const ");
        if (t->base_volatile) *len += (size_t)snprintf(buf + *len, cap - *len, "volatile ");
        if (*len >= cap) return;
        append_name(buf, cap, len, t->base);
        if (*len < cap) *len += (size_t)snprintf(buf + *len, cap - *len, "*");
        break;
    case TY_ARRAY:
        append_name(buf, cap, len, t->base);
        if (*len < cap) *len += (size_t)snprintf(buf + *len, cap - *len, "[%llu]", (unsigned long long)t->len);
        break;
    default:
        *len += (size_t)snprintf(buf + *len, cap - *len, "%s", t->name);
        break;
    }
    if (*len > cap) *len = cap;
}

const char *type_name(const Type *t) {
    char buf[256];
    size_t len = 0;
    append_name(buf, sizeof buf, &len, t);
    if (len >= sizeof buf) len = sizeof buf - 1;
    return arena_strndup(&types_arena, buf, len);
}

bool type_equiv(const Type *a, const Type *b) {
    if (a == b) return true;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case TY_INT:
        return a->size == b->size && a->is_signed == b->is_signed;
    case TY_FLOAT:
        return a->size == b->size;
    case TY_PTR:
        return a->base_const == b->base_const && a->base_volatile == b->base_volatile && type_equiv(a->base, b->base);
    case TY_ARRAY:
        return a->len == b->len && type_equiv(a->base, b->base);
    default:
        return false;
    }
}

bool int_widens(const Type *from, const Type *to) {
    if (from->kind != TY_INT || to->kind != TY_INT) return false;
    if (from->is_signed == to->is_signed) return to->size >= from->size;
    if (!from->is_signed && to->is_signed) return to->size > from->size;
    return false;  /* signed -> unsigned can't hold negatives */
}

bool type_assignable(const Type *from, const Type *to) {
    if (from->kind == TY_ERROR || to->kind == TY_ERROR) return true;
    if (type_equiv(from, to)) return true;
    if (from->kind == TY_INT && to->kind == TY_INT) return int_widens(from, to);
    if (from->kind == TY_FLOAT && to->kind == TY_FLOAT) return to->size >= from->size;
    if (from->kind == TY_NULL && to->kind == TY_PTR) return true;
    if (from->kind == TY_PTR && to->kind == TY_PTR) {
        /* Adding qualifiers is fine; dropping them needs a cast. */
        if (from->base_const && !to->base_const) return false;
        if (from->base_volatile && !to->base_volatile) return false;
        if (to->base->kind == TY_VOID) return true;    /* T* -> void* */
        return type_equiv(from->base, to->base);
    }
    return false;
}

bool type_castable(const Type *from, const Type *to) {
    if (from->kind == TY_ERROR || to->kind == TY_ERROR) return true;
    if (type_assignable(from, to)) return true;
    bool from_num = type_is_numeric(from), to_num = type_is_numeric(to);
    if (from_num && to_num) return true;
    if (from->kind == TY_BOOL && to->kind == TY_INT) return true;
    if (from->kind == TY_PTR && to->kind == TY_PTR) return true;
    if (from->kind == TY_PTR && to->kind == TY_INT) return to->size == 8;
    if (from->kind == TY_INT && to->kind == TY_PTR) return from->size == 8;
    if (from->kind == TY_NULL && to->kind == TY_INT) return to->size == 8;
    return false;
}

int64_t int_type_min(const Type *t) {
    if (!t->is_signed) return 0;
    if (t->size == 8) return INT64_MIN;
    return -((int64_t)1 << (t->size * 8 - 1));
}

uint64_t int_type_max(const Type *t) {
    unsigned bits = t->size * 8;
    if (t->is_signed) return ((uint64_t)1 << (bits - 1)) - 1;
    return bits == 64 ? UINT64_MAX : ((uint64_t)1 << bits) - 1;
}

uint64_t int_normalize(const Type *t, uint64_t v) {
    unsigned bits = t->size * 8;
    if (bits >= 64) return v;
    uint64_t mask = ((uint64_t)1 << bits) - 1;
    v &= mask;
    if (t->is_signed && (v >> (bits - 1))) v |= ~mask;  /* sign-extend */
    return v;
}
