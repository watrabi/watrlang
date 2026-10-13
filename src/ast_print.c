#include <string.h>

#include "ast.h"

/*
 * Debug printer for `watrc --ast`. Declarations and statements print as an
 * indented outline; expressions print as one-line S-expressions so that
 * precedence is explicit: `1 + 2 * 3` prints as (+ 1 (* 2 3)).
 */

static void print_expr(FILE *out, const Expr *e);
static void print_stmt(FILE *out, const Stmt *s, int indent);

static void indent_to(FILE *out, int indent) {
    for (int i = 0; i < indent; i++) fputs("  ", out);
}

static void print_escaped(FILE *out, const char *data, size_t len, char quote) {
    fputc(quote, out);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        switch (c) {
        case '\n': fputs("\\n", out); break;
        case '\t': fputs("\\t", out); break;
        case '\r': fputs("\\r", out); break;
        case '\0': fputs("\\0", out); break;
        case '\\': fputs("\\\\", out); break;
        default:
            if (c == (unsigned char)quote) fprintf(out, "\\%c", quote);
            else if (c < 0x20 || c == 0x7f) fprintf(out, "\\x%02X", c);
            else fputc(c, out);
        }
    }
    fputc(quote, out);
}

static void print_type(FILE *out, const TypeExpr *t) {
    if (!t) {
        fputs("<?>", out);
        return;
    }
    switch (t->kind) {
    case TYPEX_BUILTIN:
    case TYPEX_NAMED:
        if (t->is_const) fputs("const ", out);
        if (t->is_volatile) fputs("volatile ", out);
        fputs(t->kind == TYPEX_BUILTIN ? token_kind_spelling(t->u.builtin) : t->u.name, out);
        break;
    case TYPEX_PTR:
        print_type(out, t->u.pointee);
        fputc('*', out);
        break;
    case TYPEX_ARRAY:
        print_type(out, t->u.array.elem);
        fputc('[', out);
        print_expr(out, t->u.array.len);
        fputc(']', out);
        break;
    case TYPEX_RESOLVED:   /* only made by sema, after printing */
        fputs("<resolved>", out);
        break;
    case TYPEX_SLICE:
        print_type(out, t->u.array.elem);
        fputs("[]", out);
        break;
    case TYPEX_ERROR_UNION:
        print_type(out, t->u.error_union.value);
        fputc('!', out);
        print_type(out, t->u.error_union.error);
        break;
    }
}

static void print_float(FILE *out, double v) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.17g", v);
    /* Make sure floats never look like ints in the dump. */
    if (!strpbrk(buf, ".eEn")) strcat(buf, ".0");
    fputs(buf, out);
}

static void print_list(FILE *out, Expr **items, size_t n) {
    for (size_t i = 0; i < n; i++) {
        fputc(' ', out);
        print_expr(out, items[i]);
    }
}

static void print_expr(FILE *out, const Expr *e) {
    if (!e) {
        fputs("<?>", out);
        return;
    }
    switch (e->kind) {
    case EXPR_INT:
        fprintf(out, "%llu", (unsigned long long)e->u.int_val);
        break;
    case EXPR_FLOAT:
        print_float(out, e->u.float_val);
        break;
    case EXPR_CHAR: {
        char c = (char)e->u.int_val;
        print_escaped(out, &c, 1, '\'');
        break;
    }
    case EXPR_STRING:
        print_escaped(out, e->u.str.data, e->u.str.len, '"');
        break;
    case EXPR_INTERP:
        fputs("(interp", out);
        print_list(out, e->u.interp.parts, e->u.interp.n);
        fputc(')', out);
        break;
    case EXPR_BOOL:
        fputs(e->u.bool_val ? "true" : "false", out);
        break;
    case EXPR_NULL:
        fputs("null", out);
        break;
    case EXPR_THIS:
        fputs("this", out);
        break;
    case EXPR_IDENT:
        fputs(e->u.name, out);
        break;
    case EXPR_SCOPE:
        fprintf(out, "%s::%s", e->u.scope.scope, e->u.scope.member);
        break;
    case EXPR_UNARY:
    case EXPR_POSTFIX: {
        TokenKind op = e->u.unary.op;
        const char *prefix = "";
        if (op == TOK_PLUSPLUS || op == TOK_MINUSMINUS) prefix = e->kind == EXPR_UNARY ? "pre" : "post";
        fprintf(out, "(%s%s ", prefix, token_kind_spelling(op));
        print_expr(out, e->u.unary.operand);
        fputc(')', out);
        break;
    }
    case EXPR_BINARY:
    case EXPR_ASSIGN:
        fprintf(out, "(%s ", token_kind_spelling(e->u.binary.op));
        print_expr(out, e->u.binary.lhs);
        fputc(' ', out);
        print_expr(out, e->u.binary.rhs);
        fputc(')', out);
        break;
    case EXPR_TERNARY:
        fputs("(?: ", out);
        print_expr(out, e->u.ternary.cond);
        fputc(' ', out);
        print_expr(out, e->u.ternary.then_expr);
        fputc(' ', out);
        print_expr(out, e->u.ternary.else_expr);
        fputc(')', out);
        break;
    case EXPR_CALL:
        fputs("(call ", out);
        print_expr(out, e->u.call.callee);
        print_list(out, e->u.call.args, e->u.call.nargs);
        fputc(')', out);
        break;
    case EXPR_INDEX:
        if (e->u.index.index) {
            fputs("(index ", out);
            print_expr(out, e->u.index.base);
            fputc(' ', out);
            print_expr(out, e->u.index.index);
        } else {
            fputs("(append ", out);
            print_expr(out, e->u.index.base);
        }
        fputc(')', out);
        break;
    case EXPR_MEMBER:
        fputs("(-> ", out);
        print_expr(out, e->u.member.object);
        fprintf(out, " %s)", e->u.member.name);
        break;
    case EXPR_CAST:
        fputs("(as ", out);
        print_expr(out, e->u.cast.expr);
        fputc(' ', out);
        print_type(out, e->u.cast.type);
        fputc(')', out);
        break;
    case EXPR_NEW:
        fputs("(new ", out);
        print_type(out, e->u.new_.type);
        print_list(out, e->u.new_.args, e->u.new_.nargs);
        fputc(')', out);
        break;
    case EXPR_ALLOC:
        fputs("(alloc ", out);
        print_type(out, e->u.alloc.type);
        if (e->u.alloc.count) {
            fputc(' ', out);
            print_expr(out, e->u.alloc.count);
        }
        fputc(')', out);
        break;
    case EXPR_SIZEOF:
        fputs("(sizeof ", out);
        print_type(out, e->u.sizeof_type);
        fputc(')', out);
        break;
    case EXPR_TRY:
        fputs("(try ", out);
        print_expr(out, e->u.try_expr);
        fputc(')', out);
        break;
    case EXPR_ARRAY_LIT:
        fputs("(array", out);
        for (size_t i = 0; i < e->u.array.n; i++) {
            const ArrayEntry *entry = e->u.array.entries[i];
            fputc(' ', out);
            if (entry->key) {
                fputs("(=> ", out);
                print_expr(out, entry->key);
                fputc(' ', out);
                print_expr(out, entry->value);
                fputc(')', out);
            } else {
                print_expr(out, entry->value);
            }
        }
        fputc(')', out);
        break;
    case EXPR_STRUCT_LIT:
        fprintf(out, "(struct %s", e->u.struct_lit.type_name);
        for (size_t i = 0; i < e->u.struct_lit.n; i++) {
            fprintf(out, " (%s ", e->u.struct_lit.fields[i]->name);
            print_expr(out, e->u.struct_lit.fields[i]->value);
            fputc(')', out);
        }
        fputc(')', out);
        break;
    }
}

static void print_var(FILE *out, const VarDecl *v) {
    if (v->type) {
        print_type(out, v->type);
        fputc(' ', out);
    }
    fputs(v->name, out);
    if (v->init) {
        fputs(" = ", out);
        print_expr(out, v->init);
    }
}

/* Body of if/while/etc: a block's statements are printed directly. */
static void print_body(FILE *out, const Stmt *s, int indent) {
    if (s->kind == STMT_BLOCK) {
        if (s->u.block.n == 0) {
            indent_to(out, indent);
            fputs("(empty)\n", out);
        }
        for (size_t i = 0; i < s->u.block.n; i++) print_stmt(out, s->u.block.stmts[i], indent);
    } else {
        print_stmt(out, s, indent);
    }
}

/* for-loop init: a var decl or an expression, on one line. */
static void print_inline_stmt(FILE *out, const Stmt *s) {
    if (!s) return;
    if (s->kind == STMT_VAR) {
        fputs("var ", out);
        print_var(out, s->u.var);
    } else if (s->kind == STMT_EXPR) {
        print_expr(out, s->u.expr);
    }
}

static void print_stmt(FILE *out, const Stmt *s, int indent) {
    indent_to(out, indent);
    switch (s->kind) {
    case STMT_BLOCK:
        fputs("block\n", out);
        print_body(out, s, indent + 1);
        break;
    case STMT_VAR:
        fputs("var ", out);
        print_var(out, s->u.var);
        fputc('\n', out);
        break;
    case STMT_EXPR:
        print_expr(out, s->u.expr);
        fputc('\n', out);
        break;
    case STMT_IF:
        fputs("if ", out);
        print_expr(out, s->u.if_.cond);
        fputc('\n', out);
        print_body(out, s->u.if_.then_stmt, indent + 1);
        if (s->u.if_.else_stmt) {
            indent_to(out, indent);
            fputs("else\n", out);
            print_body(out, s->u.if_.else_stmt, indent + 1);
        }
        break;
    case STMT_WHILE:
        fputs("while ", out);
        print_expr(out, s->u.while_.cond);
        fputc('\n', out);
        print_body(out, s->u.while_.body, indent + 1);
        break;
    case STMT_DO_WHILE:
        fputs("do\n", out);
        print_body(out, s->u.while_.body, indent + 1);
        indent_to(out, indent);
        fputs("while ", out);
        print_expr(out, s->u.while_.cond);
        fputc('\n', out);
        break;
    case STMT_FOR:
        fputs("for (", out);
        print_inline_stmt(out, s->u.for_.init);
        fputs("; ", out);
        if (s->u.for_.cond) print_expr(out, s->u.for_.cond);
        fputs("; ", out);
        if (s->u.for_.step) print_expr(out, s->u.for_.step);
        fputs(")\n", out);
        print_body(out, s->u.for_.body, indent + 1);
        break;
    case STMT_FOREACH:
        fputs("foreach ", out);
        print_expr(out, s->u.foreach_.iterable);
        fputs(" as ", out);
        if (s->u.foreach_.key) {
            print_var(out, s->u.foreach_.key);
            fputs(" => ", out);
        }
        print_var(out, s->u.foreach_.value);
        fputc('\n', out);
        print_body(out, s->u.foreach_.body, indent + 1);
        break;
    case STMT_RETURN:
        fputs("return", out);
        if (s->u.expr) {
            fputc(' ', out);
            print_expr(out, s->u.expr);
        }
        fputc('\n', out);
        break;
    case STMT_BREAK:
        fputs("break\n", out);
        break;
    case STMT_CONTINUE:
        fputs("continue\n", out);
        break;
    case STMT_ECHO:
        fputs("echo", out);
        for (size_t i = 0; i < s->u.echo.n; i++) {
            fputs(i ? ", " : " ", out);
            print_expr(out, s->u.echo.exprs[i]);
        }
        fputc('\n', out);
        break;
    case STMT_DEFER:
        fputs("defer\n", out);
        print_body(out, s->u.defer_body, indent + 1);
        break;
    case STMT_ASM: {
        static const char *const dirs[] = {"in", "out", "inout"};
        fputs("asm", out);
        for (size_t i = 0; i < s->u.asm_.nlines; i++) {
            fputs(i ? "; " : " ", out);
            print_escaped(out, s->u.asm_.lines[i]->data, s->u.asm_.lines[i]->len, '"');
        }
        for (size_t i = 0; i < s->u.asm_.nops; i++) {
            const AsmOperand *op = s->u.asm_.ops[i];
            fprintf(out, "%s%s(", i ? ", " : " : ", dirs[op->dir]);
            print_escaped(out, op->reg.data, op->reg.len, '"');
            fputs(") ", out);
            print_expr(out, op->expr);
        }
        fputc('\n', out);
        break;
    }
    }
}

static void print_decl(FILE *out, const Decl *d, int indent) {
    indent_to(out, indent);
    if (d->vis == VIS_PUBLIC) fputs("public ", out);
    if (d->vis == VIS_PRIVATE) fputs("private ", out);
    if (d->is_static) fputs("static ", out);
    if (d->is_export) fputs("export ", out);
    if (d->is_extern) fputs("extern ", out);

    switch (d->kind) {
    case DECL_IMPORT:
        fputs("import ", out);
        print_escaped(out, d->u.import.path.data, d->u.import.path.len, '"');
        if (d->name) fprintf(out, " as %s", d->name);
        fputc('\n', out);
        break;
    case DECL_FUNC:
        fprintf(out, "function %s(", d->name);
        for (size_t i = 0; i < d->u.func.nparams; i++) {
            if (i) fputs(", ", out);
            print_type(out, d->u.func.params[i]->type);
            fprintf(out, " %s", d->u.func.params[i]->name);
        }
        if (d->u.func.variadic) fputs(d->u.func.nparams ? ", ..." : "...", out);
        fputs(") -> ", out);
        if (d->u.func.ret) print_type(out, d->u.func.ret);
        else fputs("void", out);
        fputc('\n', out);
        if (d->u.func.body) print_body(out, d->u.func.body, indent + 1);
        break;
    case DECL_STRUCT:
        fprintf(out, "struct %s\n", d->name);
        for (size_t i = 0; i < d->u.struct_.n; i++) {
            indent_to(out, indent + 1);
            fputs("field ", out);
            print_var(out, d->u.struct_.fields[i]);
            fputc('\n', out);
        }
        break;
    case DECL_CLASS:
        fprintf(out, "class %s\n", d->name);
        for (size_t i = 0; i < d->u.class_.n; i++) print_decl(out, d->u.class_.members[i], indent + 1);
        break;
    case DECL_ENUM:
        fprintf(out, "enum %s", d->name);
        if (d->u.enum_.base) {
            fputs(": ", out);
            print_type(out, d->u.enum_.base);
        }
        fputc('\n', out);
        for (size_t i = 0; i < d->u.enum_.n; i++) {
            const EnumItem *item = d->u.enum_.items[i];
            indent_to(out, indent + 1);
            fputs(item->name, out);
            if (item->value) {
                fputs(" = ", out);
                print_expr(out, item->value);
            }
            fputc('\n', out);
        }
        break;
    case DECL_VAR:
        fputs(indent ? "field " : "var ", out);
        print_var(out, d->u.var);
        fputc('\n', out);
        break;
    }
}

void ast_print_module(FILE *out, const Module *m) {
    for (size_t i = 0; i < m->n; i++) print_decl(out, m->decls[i], 0);
}
