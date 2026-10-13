#include "clone.h"

#include <string.h>

/*
 * Deep copies of syntax trees, for generics: each instantiation gets its
 * own copy of the template (sema annotates and rewrites trees in place),
 * with the type parameters replaced by the types they stand for. Strings,
 * names and source locations are shared; they never change.
 */

typedef struct {
    Arena *arena;
    const char **names;
    struct Type **types;
    size_t n;
} Cloner;

static Expr *clone_expr(Cloner *c, const Expr *e);
static Stmt *clone_stmt(Cloner *c, const Stmt *s);
static TypeExpr *clone_type(Cloner *c, const TypeExpr *t);

static void *dup(Cloner *c, const void *p, size_t size) {
    void *q = arena_alloc(c->arena, size);
    memcpy(q, p, size);
    return q;
}

#define DUP(c, T, p) ((T *)dup((c), (p), sizeof(T)))

static Expr **clone_exprs(Cloner *c, Expr *const *v, size_t n) {
    if (!v) return NULL;
    Expr **out = arena_alloc(c->arena, (n ? n : 1) * sizeof *out);
    for (size_t i = 0; i < n; i++) out[i] = clone_expr(c, v[i]);
    return out;
}

static TypeExpr **clone_types(Cloner *c, TypeExpr *const *v, size_t n) {
    if (!v) return NULL;
    TypeExpr **out = arena_alloc(c->arena, (n ? n : 1) * sizeof *out);
    for (size_t i = 0; i < n; i++) out[i] = clone_type(c, v[i]);
    return out;
}

static TypeExpr *clone_type(Cloner *c, const TypeExpr *t) {
    if (!t) return NULL;
    TypeExpr *o = DUP(c, TypeExpr, t);
    o->targs = clone_types(c, t->targs, t->ntargs);
    switch (t->kind) {
    case TYPEX_NAMED:
        if (t->ntargs == 0) {
            for (size_t i = 0; i < c->n; i++) {
                if (strcmp(t->u.name, c->names[i]) == 0) {
                    o->kind = TYPEX_RESOLVED;
                    o->u.resolved = c->types[i];
                    break;
                }
            }
        }
        break;
    case TYPEX_PTR: o->u.pointee = clone_type(c, t->u.pointee); break;
    case TYPEX_ARRAY:
    case TYPEX_SLICE:
        o->u.array.elem = clone_type(c, t->u.array.elem);
        o->u.array.len = t->u.array.len ? clone_expr(c, t->u.array.len) : NULL;
        break;
    case TYPEX_ERROR_UNION:
        o->u.error_union.value = clone_type(c, t->u.error_union.value);
        o->u.error_union.error = clone_type(c, t->u.error_union.error);
        break;
    case TYPEX_BUILTIN:
    case TYPEX_RESOLVED:
        break;
    }
    return o;
}

static Expr *clone_expr(Cloner *c, const Expr *e) {
    if (!e) return NULL;
    Expr *o = DUP(c, Expr, e);
    o->targs = clone_types(c, e->targs, e->ntargs);
    switch (e->kind) {
    case EXPR_INT: case EXPR_FLOAT: case EXPR_CHAR: case EXPR_STRING: case EXPR_BOOL:
    case EXPR_NULL: case EXPR_THIS: case EXPR_IDENT: case EXPR_SCOPE:
        break;
    case EXPR_INTERP: o->u.interp.parts = clone_exprs(c, e->u.interp.parts, e->u.interp.n); break;
    case EXPR_UNARY:
    case EXPR_POSTFIX: o->u.unary.operand = clone_expr(c, e->u.unary.operand); break;
    case EXPR_BINARY:
    case EXPR_ASSIGN:
        o->u.binary.lhs = clone_expr(c, e->u.binary.lhs);
        o->u.binary.rhs = clone_expr(c, e->u.binary.rhs);
        break;
    case EXPR_TERNARY:
        o->u.ternary.cond = clone_expr(c, e->u.ternary.cond);
        o->u.ternary.then_expr = clone_expr(c, e->u.ternary.then_expr);
        o->u.ternary.else_expr = clone_expr(c, e->u.ternary.else_expr);
        break;
    case EXPR_CALL:
        o->u.call.callee = clone_expr(c, e->u.call.callee);
        o->u.call.args = clone_exprs(c, e->u.call.args, e->u.call.nargs);
        break;
    case EXPR_INDEX:
        o->u.index.base = clone_expr(c, e->u.index.base);
        o->u.index.index = clone_expr(c, e->u.index.index);
        break;
    case EXPR_MEMBER: o->u.member.object = clone_expr(c, e->u.member.object); break;
    case EXPR_CAST:
    case EXPR_INSTANCEOF:
        o->u.cast.expr = clone_expr(c, e->u.cast.expr);
        o->u.cast.type = clone_type(c, e->u.cast.type);
        break;
    case EXPR_NEW:
        o->u.new_.type = clone_type(c, e->u.new_.type);
        o->u.new_.args = clone_exprs(c, e->u.new_.args, e->u.new_.nargs);
        break;
    case EXPR_ALLOC:
        o->u.alloc.type = clone_type(c, e->u.alloc.type);
        o->u.alloc.count = clone_expr(c, e->u.alloc.count);
        break;
    case EXPR_SIZEOF: o->u.sizeof_type = clone_type(c, e->u.sizeof_type); break;
    case EXPR_TRY: o->u.try_expr = clone_expr(c, e->u.try_expr); break;
    case EXPR_ARRAY_LIT: {
        size_t n = e->u.array.n;
        o->u.array.entries = arena_alloc(c->arena, (n ? n : 1) * sizeof(ArrayEntry *));
        for (size_t i = 0; i < n; i++) {
            ArrayEntry *a = DUP(c, ArrayEntry, e->u.array.entries[i]);
            a->key = clone_expr(c, a->key);
            a->value = clone_expr(c, a->value);
            o->u.array.entries[i] = a;
        }
        break;
    }
    case EXPR_STRUCT_LIT: {
        size_t n = e->u.struct_lit.n;
        o->u.struct_lit.fields = arena_alloc(c->arena, (n ? n : 1) * sizeof(FieldInit *));
        for (size_t i = 0; i < n; i++) {
            FieldInit *f = DUP(c, FieldInit, e->u.struct_lit.fields[i]);
            f->value = clone_expr(c, f->value);
            o->u.struct_lit.fields[i] = f;
        }
        break;
    }
    }
    return o;
}

static VarDecl *clone_var(Cloner *c, const VarDecl *v) {
    if (!v) return NULL;
    VarDecl *o = DUP(c, VarDecl, v);
    o->type = clone_type(c, v->type);
    o->init = clone_expr(c, v->init);
    return o;
}

static Stmt *clone_stmt(Cloner *c, const Stmt *s) {
    if (!s) return NULL;
    Stmt *o = DUP(c, Stmt, s);
    switch (s->kind) {
    case STMT_BLOCK: {
        size_t n = s->u.block.n;
        o->u.block.stmts = arena_alloc(c->arena, (n ? n : 1) * sizeof(Stmt *));
        for (size_t i = 0; i < n; i++) o->u.block.stmts[i] = clone_stmt(c, s->u.block.stmts[i]);
        break;
    }
    case STMT_VAR: o->u.var = clone_var(c, s->u.var); break;
    case STMT_EXPR:
    case STMT_RETURN: o->u.expr = clone_expr(c, s->u.expr); break;
    case STMT_IF:
        o->u.if_.cond = clone_expr(c, s->u.if_.cond);
        o->u.if_.then_stmt = clone_stmt(c, s->u.if_.then_stmt);
        o->u.if_.else_stmt = clone_stmt(c, s->u.if_.else_stmt);
        break;
    case STMT_WHILE:
    case STMT_DO_WHILE:
        o->u.while_.cond = clone_expr(c, s->u.while_.cond);
        o->u.while_.body = clone_stmt(c, s->u.while_.body);
        break;
    case STMT_FOR:
        o->u.for_.init = clone_stmt(c, s->u.for_.init);
        o->u.for_.cond = clone_expr(c, s->u.for_.cond);
        o->u.for_.step = clone_expr(c, s->u.for_.step);
        o->u.for_.body = clone_stmt(c, s->u.for_.body);
        break;
    case STMT_FOREACH:
        o->u.foreach_.iterable = clone_expr(c, s->u.foreach_.iterable);
        o->u.foreach_.key = clone_var(c, s->u.foreach_.key);
        o->u.foreach_.value = clone_var(c, s->u.foreach_.value);
        o->u.foreach_.body = clone_stmt(c, s->u.foreach_.body);
        break;
    case STMT_BREAK:
    case STMT_CONTINUE:
        break;
    case STMT_ECHO: o->u.echo.exprs = clone_exprs(c, s->u.echo.exprs, s->u.echo.n); break;
    case STMT_DEFER: o->u.defer_body = clone_stmt(c, s->u.defer_body); break;
    case STMT_ASM: {
        size_t n = s->u.asm_.nops;
        o->u.asm_.ops = arena_alloc(c->arena, (n ? n : 1) * sizeof(AsmOperand *));
        for (size_t i = 0; i < n; i++) {
            AsmOperand *op = DUP(c, AsmOperand, s->u.asm_.ops[i]);
            op->expr = clone_expr(c, op->expr);
            o->u.asm_.ops[i] = op;
        }
        break;
    }
    }
    return o;
}

static Decl *clone_decl_rec(Cloner *c, const Decl *d) {
    Decl *o = DUP(c, Decl, d);
    o->sym = NULL;
    o->type = NULL;
    switch (d->kind) {
    case DECL_IMPORT: break;
    case DECL_FUNC: {
        size_t n = d->u.func.nparams;
        o->u.func.params = arena_alloc(c->arena, (n ? n : 1) * sizeof(Param *));
        for (size_t i = 0; i < n; i++) {
            Param *p = DUP(c, Param, d->u.func.params[i]);
            p->type = clone_type(c, p->type);
            p->sym = NULL;
            o->u.func.params[i] = p;
        }
        o->u.func.ret = clone_type(c, d->u.func.ret);
        o->u.func.body = clone_stmt(c, d->u.func.body);
        break;
    }
    case DECL_STRUCT: {
        size_t n = d->u.struct_.n;
        o->u.struct_.fields = arena_alloc(c->arena, (n ? n : 1) * sizeof(VarDecl *));
        for (size_t i = 0; i < n; i++) o->u.struct_.fields[i] = clone_var(c, d->u.struct_.fields[i]);
        break;
    }
    case DECL_CLASS: {
        size_t n = d->u.class_.n;
        o->u.class_.members = arena_alloc(c->arena, (n ? n : 1) * sizeof(Decl *));
        for (size_t i = 0; i < n; i++) o->u.class_.members[i] = clone_decl_rec(c, d->u.class_.members[i]);
        o->u.class_.base = clone_type(c, d->u.class_.base);
        o->u.class_.ifaces = clone_types(c, d->u.class_.ifaces, d->u.class_.nifaces);
        break;
    }
    case DECL_ENUM: {
        size_t n = d->u.enum_.n;
        o->u.enum_.base = clone_type(c, d->u.enum_.base);
        o->u.enum_.items = arena_alloc(c->arena, (n ? n : 1) * sizeof(EnumItem *));
        for (size_t i = 0; i < n; i++) {
            EnumItem *it = DUP(c, EnumItem, d->u.enum_.items[i]);
            it->value = clone_expr(c, it->value);
            o->u.enum_.items[i] = it;
        }
        break;
    }
    case DECL_VAR: o->u.var = clone_var(c, d->u.var); break;
    }
    return o;
}

Decl *clone_instance(Arena *arena, const Decl *tmpl, struct Type **types) {
    Cloner c = {arena, tmpl->tparams, types, tmpl->ntparams};
    Decl *d = clone_decl_rec(&c, tmpl);
    d->tparams = NULL;
    d->tparam_locs = NULL;
    d->ntparams = 0;
    return d;
}
