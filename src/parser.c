#include "parser.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "vec.h"

/*
 * Recursive-descent parser with Pratt parsing for expressions.
 *
 * The whole file is lexed up front so the parser can look ahead freely;
 * that's what lets it decide "is this statement a declaration?" without
 * a symbol table (see looks_like_decl).
 *
 * Error recovery is panic-mode: the first error in a statement is
 * reported, further errors are suppressed until the parser resyncs at a
 * `;`, a `}`, or the next top-level declaration.
 */

typedef struct {
    Arena *arena;
    const SrcFile *file;
    Token *toks;
    size_t ntoks;       /* the last token is always TOK_EOF */
    size_t pos;
    bool panic;
    bool no_as;         /* inside `foreach (expr as ...)`, `as` ends the expression */
    bool in_interp;     /* sub-parser for a "{...}" inside a string */
    int depth;          /* recursion depth, to fail cleanly instead of overflowing the stack */
} Parser;

#define MAX_NESTING 256

/* Binding powers, lowest to highest. Unlike C, bitwise operators bind
 * tighter than comparisons, so `x & 1 == 0` means `(x & 1) == 0`. */
enum {
    BP_NONE = 0,
    BP_ASSIGN,      /* = += -= ... (right-assoc) */
    BP_TERNARY,     /* ?: (right-assoc) */
    BP_COALESCE,    /* ?? (right-assoc) */
    BP_OR,          /* || */
    BP_AND,         /* && */
    BP_CMP,         /* == != < > <= >= (can't be chained) */
    BP_CONCAT,      /* . */
    BP_BOR,         /* | */
    BP_XOR,         /* ^ */
    BP_BAND,        /* & */
    BP_SHIFT,       /* << >> */
    BP_ADD,         /* + - */
    BP_MUL,         /* * / % */
    BP_AS,          /* as */
    BP_UNARY,       /* - ! ~ & * ++ -- (prefix) */
};

static Expr *parse_expr(Parser *p);
static Expr *parse_expr_bp(Parser *p, int min_bp);
static Stmt *parse_stmt(Parser *p);
static Stmt *parse_block(Parser *p);
static TypeExpr *parse_type(Parser *p, bool in_cast);

/* ---- token helpers --------------------------------------------------------- */

static Token *peek_at(Parser *p, size_t ahead) {
    size_t i = p->pos + ahead;
    return &p->toks[i < p->ntoks ? i : p->ntoks - 1];
}

static Token *cur(Parser *p) {
    return peek_at(p, 0);
}

static TokenKind cur_kind(Parser *p) {
    return cur(p)->kind;
}

static Token *advance(Parser *p) {
    Token *t = cur(p);
    if (p->pos < p->ntoks - 1) p->pos++;
    return t;
}

static bool check(Parser *p, TokenKind k) {
    return cur_kind(p) == k;
}

static bool accept(Parser *p, TokenKind k) {
    if (!check(p, k)) return false;
    advance(p);
    return true;
}

static const char *tok_name(Parser *p, const Token *t) {
    return arena_strndup(p->arena, t->start, t->loc.len);
}

/* Human description of a token for "expected X, found Y" messages. */
static const char *describe(const Parser *p, const Token *t) {
    static char buf[96];
    switch (t->kind) {
    case TOK_EOF:
        return p->in_interp ? "the end of the interpolation" : "end of file";
    case TOK_IDENT:
        snprintf(buf, sizeof buf, "identifier `%.*s`", t->loc.len > 40 ? 40 : (int)t->loc.len, t->start);
        return buf;
    case TOK_INT: case TOK_FLOAT: case TOK_CHAR: case TOK_STRING: case TOK_ERROR:
        if (t->loc.len > 24) snprintf(buf, sizeof buf, "`%.21s...`", t->start);
        else snprintf(buf, sizeof buf, "`%.*s`", (int)t->loc.len, t->start);
        /* Keep multi-line string literals on one line in the message. */
        for (char *c = buf; *c; c++) if (*c == '\n') { strcpy(c, "...`"); break; }
        return buf;
    default:
        if (t->kind >= TOK_KW_FUNCTION) snprintf(buf, sizeof buf, "keyword `%s`", token_kind_spelling(t->kind));
        else snprintf(buf, sizeof buf, "`%s`", token_kind_spelling(t->kind));
        return buf;
    }
}

/* Starts an error at `loc` and enters panic mode. Returns NULL (and
 * reports nothing) if the parser is already panicking, so only the first
 * error of a broken statement is shown. The caller adds labels and calls
 * diag_end; every diag_* function accepts NULL. */
static Diag *perr(Parser *p, const char *code, Loc loc, const char *fmt, ...) WATR_PRINTF(4, 5);
static Diag *perr(Parser *p, const char *code, Loc loc, const char *fmt, ...) {
    if (p->panic) return NULL;
    p->panic = true;
    va_list ap;
    va_start(ap, fmt);
    Diag *d = diag_vstart(DIAG_ERROR, code, loc, fmt, ap);
    va_end(ap);
    return d;
}

/* Like perr, at token `t`. Returns NULL for ERROR tokens: the lexer already
 * explained those. */
static Diag *perr_tok(Parser *p, const char *code, const Token *t, const char *fmt, ...) WATR_PRINTF(4, 5);
static Diag *perr_tok(Parser *p, const char *code, const Token *t, const char *fmt, ...) {
    if (p->panic) return NULL;
    p->panic = true;
    if (t->kind == TOK_ERROR) return NULL;
    va_list ap;
    va_start(ap, fmt);
    Diag *d = diag_vstart(DIAG_ERROR, code, t->loc, fmt, ap);
    va_end(ap);
    return d;
}

static TokenKind opener_of(TokenKind close) {
    switch (close) {
    case TOK_RPAREN:   return TOK_LPAREN;
    case TOK_RBRACKET: return TOK_LBRACKET;
    case TOK_RBRACE:   return TOK_LBRACE;
    default:           return TOK_EOF;
    }
}

/* The unmatched `(`/`[`/`{` that a missing closer belongs to, found by
 * walking back over the tokens already parsed. */
static const Token *unmatched_opener(Parser *p, TokenKind close) {
    TokenKind open = opener_of(close);
    if (open == TOK_EOF) return NULL;
    int depth = 0;
    for (size_t i = p->pos; i-- > 0;) {
        TokenKind k = p->toks[i].kind;
        if (k == close) depth++;
        else if (k == open && depth-- == 0) return &p->toks[i];
    }
    return NULL;
}

static Token *expect(Parser *p, TokenKind k, const char *context) {
    if (check(p, k)) return advance(p);
    const Token *t = cur(p);
    const Token *open = unmatched_opener(p, k);
    Diag *d;
    if (t->kind == TOK_EOF && open) {
        d = perr(p, "E0109", open->loc, "unclosed delimiter `%s`", token_kind_spelling(open->kind));
        diag_label(d, "this `%s` is never closed", token_kind_spelling(open->kind));
        diag_note(d, "the file ended before the matching `%s`", token_kind_spelling(k));
    } else {
        d = perr_tok(p, "E0100", t, "expected `%s` %s, found %s", token_kind_spelling(k), context, describe(p, t));
        diag_label(d, "expected `%s`", token_kind_spelling(k));
        if (open) diag_span(d, open->loc, "unclosed `%s` starts here", token_kind_spelling(open->kind));
    }
    diag_end(d);
    return NULL;
}

/* A missing `;` is reported right after the previous token, where it
 * belongs, rather than at the next token (often on the next line). */
static bool expect_semi(Parser *p, const char *context) {
    if (accept(p, TOK_SEMI)) return true;
    if (p->panic) return false;
    const Token *next = cur(p);
    if (next->kind == TOK_ERROR) {
        diag_end(perr_tok(p, "E0101", next, "expected `;` %s", context));
        return false;
    }
    const Token *prev = &p->toks[p->pos - 1];
    Loc loc = prev->loc;
    if (memchr(prev->start, '\n', prev->loc.len) == NULL) {
        loc.offset += loc.len;
        loc.col += loc.len;
    }
    loc.len = 0;
    /* No panic: carry on as if the `;` were there. That's almost always
     * right, and it keeps the next statement from being skipped. */
    Diag *d = diag_start(DIAG_ERROR, "E0101", loc, "expected `;` %s, found %s", context, describe(p, next));
    diag_label(d, "add `;` here");
    if (next->kind != TOK_EOF) diag_span(d, next->loc, "unexpected %s", describe(p, next));
    diag_end(d);
    return false;
}

static Token *expect_ident(Parser *p, const char *what) {
    if (check(p, TOK_IDENT)) return advance(p);
    Token *t = cur(p);
    if (t->kind >= TOK_KW_FUNCTION) {
        /* Keep going with the keyword as the name; the error is clear enough
         * that resyncing would only cause follow-on errors. */
        if (!p->panic) {
            const char *word = token_kind_spelling(t->kind);
            Diag *d = diag_start(DIAG_ERROR, "E0102", t->loc, "`%s` is a reserved word and can't be used as %s", word, what);
            diag_label(d, t->kind >= TOK_KW_VOID ? "this is a built-in type" : "this is a keyword");
            char renamed[64];
            snprintf(renamed, sizeof renamed, "%s_", word);
            diag_fix(d, t->loc, renamed, "pick a different name, for example");
            diag_end(d);
        }
        return advance(p);
    }
    Diag *d = perr_tok(p, "E0100", t, "expected %s, found %s", what, describe(p, t));
    diag_label(d, "expected %s", what);
    diag_end(d);
    return NULL;
}

/* Skips to the end of the current statement: past a `;`, past a whole
 * `{ ... }` block, or up to (not past) the `}` closing the enclosing block. */
static void sync_stmt(Parser *p) {
    int depth = 0;
    while (!check(p, TOK_EOF)) {
        TokenKind k = cur_kind(p);
        if (k == TOK_SEMI && depth == 0) {
            advance(p);
            break;
        }
        if (k == TOK_LBRACE) {
            depth++;
        } else if (k == TOK_RBRACE) {
            if (depth == 0) break;
            if (--depth == 0) {
                advance(p);
                break;
            }
        }
        advance(p);
    }
    p->panic = false;
}

static bool is_top_level_start(TokenKind k) {
    return k == TOK_KW_FUNCTION || k == TOK_KW_STRUCT || k == TOK_KW_CLASS || k == TOK_KW_ENUM || k == TOK_KW_PRIVATE ||
           k == TOK_KW_IMPORT || k == TOK_KW_EXPORT || k == TOK_KW_EXTERN;
}

/* Skips to the next thing that looks like a top-level declaration. */
static void sync_top(Parser *p) {
    int depth = 0;
    while (!check(p, TOK_EOF)) {
        TokenKind k = cur_kind(p);
        if (depth == 0 && is_top_level_start(k)) break;
        if (k == TOK_LBRACE) depth++;
        else if (k == TOK_RBRACE && depth > 0) depth--;
        else if (k == TOK_SEMI && depth == 0) {
            advance(p);
            break;
        }
        advance(p);
    }
    p->panic = false;
}

/* ---- node constructors ----------------------------------------------------- */

static Expr *new_expr(Parser *p, ExprKind kind, Loc loc) {
    Expr *e = ARENA_NEW(p->arena, Expr);
    e->kind = kind;
    e->loc = loc;
    return e;
}

static Stmt *new_stmt(Parser *p, StmtKind kind, Loc loc) {
    Stmt *s = ARENA_NEW(p->arena, Stmt);
    s->kind = kind;
    s->loc = loc;
    return s;
}

static TypeExpr *new_type(Parser *p, TypeExprKind kind, Loc loc) {
    TypeExpr *t = ARENA_NEW(p->arena, TypeExpr);
    t->kind = kind;
    t->loc = loc;
    return t;
}

/* Location spanning from `start` to the end of the previous token. */
static Loc span_to_prev(Parser *p, Loc start) {
    if (p->pos == 0) return start;
    const Token *prev = &p->toks[p->pos - 1];
    uint32_t end = prev->loc.offset + prev->loc.len;
    if (end > start.offset) start.len = end - start.offset;
    return start;
}

/* ---- strings ----------------------------------------------------------------- */

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

/* Decodes escapes in raw[0..len). The lexer already validated them. */
static StrBuf decode_string(Parser *p, const char *raw, size_t len) {
    char *out = arena_alloc(p->arena, len + 1);
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (raw[i] != '\\' || i + 1 >= len) {
            out[n++] = raw[i];
            continue;
        }
        char c = raw[++i];
        switch (c) {
        case 'n': out[n++] = '\n'; break;
        case 't': out[n++] = '\t'; break;
        case 'r': out[n++] = '\r'; break;
        case '0': out[n++] = '\0'; break;
        case 'x':
            out[n++] = (char)(hex_val(raw[i + 1]) * 16 + hex_val(raw[i + 2]));
            i += 2;
            break;
        default: out[n++] = c; break;  /* \\ \" \' \{ \} */
        }
    }
    out[n] = '\0';
    StrBuf s = {out, n};
    return s;
}

/* Lexes the whole range into a malloc'd token array ending in TOK_EOF. */
static Token *tokenize(Lexer *lx, size_t *out_n) {
    size_t cap = 256, n = 0;
    Token *toks = xmalloc(cap * sizeof *toks);
    for (;;) {
        if (n == cap) {
            cap *= 2;
            Token *grown = realloc(toks, cap * sizeof *toks);
            if (!grown) fatal("out of memory");
            toks = grown;
        }
        toks[n] = lexer_next(lx);
        if (toks[n++].kind == TOK_EOF) break;
    }
    *out_n = n;
    return toks;
}

/* Parses the expression inside a {interpolation} by re-lexing that part of
 * the source with a sub-parser. */
static Expr *parse_interp_expr(Parser *p, size_t start, size_t end, uint32_t line, uint32_t col) {
    Lexer lx;
    lexer_init_range(&lx, p->file, start, end, line, col);
    Parser sub = {p->arena, p->file, NULL, 0, 0, false, false, true, p->depth};
    sub.toks = tokenize(&lx, &sub.ntoks);

    /* Errors stay inside the string: the outer parser is still in sync. */
    Expr *e = parse_expr(&sub);
    if (e && !check(&sub, TOK_EOF)) {
        Diag *d = perr_tok(&sub, "E0100", cur(&sub), "unexpected %s in interpolation", describe(&sub, cur(&sub)));
        diag_label(d, "expected `}` to end the interpolation");
        diag_span(d, e->loc, "this is a complete expression");
        diag_end(d);
    }
    free(sub.toks);
    return e;
}

/* Builds EXPR_STRING, or EXPR_INTERP for strings with {expressions}. */
static Expr *parse_string_token(Parser *p, const Token *t) {
    const char *raw = t->start + 1;      /* skip opening quote */
    size_t len = t->loc.len - 2;         /* and closing quote */

    if (!(t->flags & TOKF_INTERP)) {
        Expr *e = new_expr(p, EXPR_STRING, t->loc);
        e->u.str = decode_string(p, raw, len);
        return e;
    }

    Expr *e = new_expr(p, EXPR_INTERP, t->loc);
    PtrVec parts = {0};
    uint32_t line = t->loc.line, col = t->loc.col + 1;
    size_t seg = 0;  /* start of the current literal segment */
    size_t i = 0;

    while (i < len) {
        char c = raw[i];
        if (c == '\\') {
            i += 2;
            col += 2;
            continue;
        }
        if (c != '{') {
            if (c == '\n') { line++; col = 1; } else col++;
            i++;
            continue;
        }

        if (i > seg) {
            Expr *lit = new_expr(p, EXPR_STRING, t->loc);
            lit->u.str = decode_string(p, raw + seg, i - seg);
            ptrvec_push(&parts, lit);
        }

        /* Find the matching `}`, skipping nested strings and char literals.
         * The lexer guarantees it exists and is on this line. */
        size_t expr_start = i + 1;
        uint32_t expr_col = col + 1;
        int depth = 1;
        size_t j = expr_start;
        while (j < len) {
            char d = raw[j];
            if (d == '"' || d == '\'') {
                char q = d;
                for (j++; j < len && raw[j] != q; j++)
                    if (raw[j] == '\\') j++;
            } else if (d == '{') {
                depth++;
            } else if (d == '}' && --depth == 0) {
                break;
            }
            j++;
        }

        size_t base = (size_t)(raw - p->file->text);
        Expr *inner = parse_interp_expr(p, base + expr_start, base + j, line, expr_col);
        if (inner) ptrvec_push(&parts, inner);

        col += (uint32_t)(j + 1 - i);
        i = j + 1;
        seg = i;
    }
    if (len > seg) {
        Expr *lit = new_expr(p, EXPR_STRING, t->loc);
        lit->u.str = decode_string(p, raw + seg, len - seg);
        ptrvec_push(&parts, lit);
    }
    e->u.interp.parts = ptrvec_finish(&parts, p->arena, &e->u.interp.n);
    return e;
}

/* A plain string where interpolation makes no sense (import paths, asm). */
static bool parse_plain_string(Parser *p, const char *what, StrBuf *out) {
    Token *t = cur(p);
    if (t->kind != TOK_STRING) {
        Diag *d = perr_tok(p, "E0100", t, "expected a string for %s, found %s", what, describe(p, t));
        diag_label(d, "expected a string literal");
        diag_end(d);
        return false;
    }
    advance(p);
    if (t->flags & TOKF_INTERP) {
        Diag *d = perr_tok(p, "E0114", t, "%s can't use {interpolation}", what);
        diag_label(d, "must be known at compile time");
        diag_help(d, "for a literal brace, write `\\{`");
        diag_end(d);
        return false;
    }
    *out = decode_string(p, t->start + 1, t->loc.len - 2);
    return true;
}

/* ---- types ------------------------------------------------------------------- */

static bool is_builtin_type(TokenKind k) {
    return k >= TOK_KW_VOID && k <= TOK_KW_USIZE;
}

static bool can_start_expr(TokenKind k) {
    switch (k) {
    case TOK_INT: case TOK_FLOAT: case TOK_CHAR: case TOK_STRING: case TOK_IDENT:
    case TOK_LPAREN: case TOK_LBRACKET:
    case TOK_MINUS: case TOK_BANG: case TOK_TILDE: case TOK_AMP: case TOK_STAR:
    case TOK_PLUSPLUS: case TOK_MINUSMINUS:
    case TOK_KW_TRUE: case TOK_KW_FALSE: case TOK_KW_NULL: case TOK_KW_THIS:
    case TOK_KW_NEW: case TOK_KW_ALLOC: case TOK_KW_SIZEOF:
        return true;
    default:
        return false;
    }
}

/* Base type plus qualifiers: `int`, `Point`, `const volatile u8`. */
static TypeExpr *parse_base_type(Parser *p) {
    Loc start = cur(p)->loc;
    bool is_const = false, is_volatile = false;
    for (;;) {
        if (accept(p, TOK_KW_CONST)) is_const = true;
        else if (accept(p, TOK_KW_VOLATILE)) is_volatile = true;
        else break;
    }

    Token *t = cur(p);
    TypeExpr *ty;
    if (is_builtin_type(t->kind)) {
        advance(p);
        ty = new_type(p, TYPEX_BUILTIN, t->loc);
        ty->u.builtin = t->kind;
    } else if (t->kind == TOK_IDENT) {
        advance(p);
        ty = new_type(p, TYPEX_NAMED, t->loc);
        ty->u.name = tok_name(p, t);
        if (check(p, TOK_COLONCOLON) && peek_at(p, 1)->kind == TOK_IDENT) {   /* module::Type */
            advance(p);
            Token *member = advance(p);
            size_t n = strlen(ty->u.name) + member->loc.len + 3;
            char *full = arena_alloc(p->arena, n);
            snprintf(full, n, "%s::%.*s", ty->u.name, (int)member->loc.len, member->start);
            ty->u.name = full;
        }
    } else {
        Diag *d = perr_tok(p, "E0100", t, "expected a type, found %s", describe(p, t));
        diag_label(d, "expected a type");
        diag_end(d);
        return NULL;
    }
    ty->is_const = is_const;
    ty->is_volatile = is_volatile;
    ty->loc = span_to_prev(p, start);
    return ty;
}

/* Full type: base followed by any of `*`, `[N]`, `[]`, `!Error`.
 * In a cast (`x as u8* ...`) a `*` followed by something that can start an
 * expression is multiplication, not a pointer: `n as int * 2`. */
static TypeExpr *parse_type(Parser *p, bool in_cast) {
    Loc start = cur(p)->loc;
    TypeExpr *ty = parse_base_type(p);
    if (!ty) return NULL;

    for (;;) {
        if (check(p, TOK_STAR)) {
            if (in_cast && can_start_expr(peek_at(p, 1)->kind) && peek_at(p, 1)->kind != TOK_STAR) break;
            advance(p);
            TypeExpr *ptr = new_type(p, TYPEX_PTR, span_to_prev(p, start));
            ptr->u.pointee = ty;
            ty = ptr;
        } else if (check(p, TOK_LBRACKET)) {
            advance(p);
            if (accept(p, TOK_RBRACKET)) {
                TypeExpr *slice = new_type(p, TYPEX_SLICE, span_to_prev(p, start));
                slice->u.array.elem = ty;
                ty = slice;
            } else {
                Expr *len = parse_expr(p);
                if (!len || !expect(p, TOK_RBRACKET, "after array length")) return NULL;
                TypeExpr *arr = new_type(p, TYPEX_ARRAY, span_to_prev(p, start));
                arr->u.array.elem = ty;
                arr->u.array.len = len;
                ty = arr;
            }
        } else if (check(p, TOK_BANG) &&
                   (peek_at(p, 1)->kind == TOK_IDENT || is_builtin_type(peek_at(p, 1)->kind))) {
            advance(p);
            TypeExpr *err = parse_base_type(p);
            if (!err) return NULL;
            TypeExpr *eu = new_type(p, TYPEX_ERROR_UNION, span_to_prev(p, start));
            eu->u.error_union.value = ty;
            eu->u.error_union.error = err;
            ty = eu;
        } else {
            break;
        }
    }
    return ty;
}

/*
 * "If a statement can parse as a declaration, it is one."
 * Built-in types and qualifiers always start a declaration. Otherwise we
 * need  IDENT (* | [INT] | [IDENT] | [] | !IDENT)* IDENT (= | ;)
 * so `Point p;`, `Foo* f = x;` and `a * b;` are declarations, while
 * `a * b + c;` and `arr[i] = 5;` are expressions.
 */
static bool looks_like_decl(Parser *p) {
    TokenKind k = cur_kind(p);
    if (k == TOK_KW_CONST || k == TOK_KW_VOLATILE || is_builtin_type(k)) return true;
    if (k != TOK_IDENT) return false;

    size_t i = 1;
    if (peek_at(p, 1)->kind == TOK_COLONCOLON && peek_at(p, 2)->kind == TOK_IDENT) i = 3;   /* module::Type */
    for (;;) {
        k = peek_at(p, i)->kind;
        if (k == TOK_STAR) {
            i++;
        } else if (k == TOK_LBRACKET) {
            TokenKind inner = peek_at(p, i + 1)->kind;
            if (inner == TOK_RBRACKET) {
                i += 2;
            } else if ((inner == TOK_INT || inner == TOK_IDENT) && peek_at(p, i + 2)->kind == TOK_RBRACKET) {
                i += 3;
            } else {
                return false;
            }
        } else if (k == TOK_BANG) {
            TokenKind next = peek_at(p, i + 1)->kind;
            if (next != TOK_IDENT && !is_builtin_type(next)) return false;
            i += 2;
        } else {
            break;
        }
    }
    if (peek_at(p, i)->kind != TOK_IDENT) return false;
    k = peek_at(p, i + 1)->kind;
    return k == TOK_ASSIGN || k == TOK_SEMI;
}

/* ---- expressions ------------------------------------------------------------- */

/* Parses an expression in a bracketed context where `as` casts are allowed again. */
static Expr *parse_expr_nested(Parser *p) {
    bool saved = p->no_as;
    p->no_as = false;
    Expr *e = parse_expr(p);
    p->no_as = saved;
    return e;
}

/* Comma-separated expressions up to `close`, which is consumed. */
static Expr **parse_expr_list(Parser *p, TokenKind close, const char *context, size_t *out_n) {
    PtrVec items = {0};
    if (!check(p, close)) {
        do {
            if (check(p, close)) break;  /* trailing comma */
            Expr *e = parse_expr_nested(p);
            if (!e) break;
            ptrvec_push(&items, e);
        } while (accept(p, TOK_COMMA));
    }
    if (!p->panic) expect(p, close, context);
    return ptrvec_finish(&items, p->arena, out_n);
}

static Expr *parse_array_lit(Parser *p) {
    Token *open = advance(p); /* [ */
    Expr *e = new_expr(p, EXPR_ARRAY_LIT, open->loc);
    PtrVec entries = {0};
    if (!check(p, TOK_RBRACKET)) {
        do {
            if (check(p, TOK_RBRACKET)) break;
            Expr *first = parse_expr_nested(p);
            if (!first) break;
            ArrayEntry *entry = ARENA_NEW(p->arena, ArrayEntry);
            if (accept(p, TOK_FATARROW)) {
                entry->key = first;
                entry->value = parse_expr_nested(p);
                if (!entry->value) break;
            } else {
                entry->value = first;
            }
            ptrvec_push(&entries, entry);
        } while (accept(p, TOK_COMMA));
    }
    if (!p->panic) expect(p, TOK_RBRACKET, "to close array literal");
    e->u.array.entries = ptrvec_finish(&entries, p->arena, &e->u.array.n);
    e->loc = span_to_prev(p, open->loc);
    return e;
}

/* Vec2 { x: 1.0, y: 2.0 } -- the caller checked the lookahead. */
static Expr *parse_struct_lit(Parser *p) {
    Token *name = advance(p);
    advance(p); /* { */
    Expr *e = new_expr(p, EXPR_STRUCT_LIT, name->loc);
    e->u.struct_lit.type_name = tok_name(p, name);
    PtrVec fields = {0};
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        Token *field = expect_ident(p, "a field name");
        if (!field || !expect(p, TOK_COLON, "after field name")) break;
        FieldInit *fi = ARENA_NEW(p->arena, FieldInit);
        fi->name = tok_name(p, field);
        fi->loc = field->loc;
        fi->value = parse_expr_nested(p);
        if (!fi->value) break;
        ptrvec_push(&fields, fi);
        if (!accept(p, TOK_COMMA)) break;
    }
    if (!p->panic) expect(p, TOK_RBRACE, "to close struct literal");
    e->u.struct_lit.fields = ptrvec_finish(&fields, p->arena, &e->u.struct_lit.n);
    e->loc = span_to_prev(p, name->loc);
    return e;
}

static Expr *parse_primary(Parser *p) {
    Token *t = cur(p);
    Expr *e;

    switch (t->kind) {
    case TOK_INT:
        advance(p);
        e = new_expr(p, EXPR_INT, t->loc);
        e->u.int_val = t->v.int_val;
        return e;
    case TOK_FLOAT:
        advance(p);
        e = new_expr(p, EXPR_FLOAT, t->loc);
        e->u.float_val = t->v.float_val;
        return e;
    case TOK_CHAR:
        advance(p);
        e = new_expr(p, EXPR_CHAR, t->loc);
        e->u.int_val = t->v.int_val;
        return e;
    case TOK_STRING:
        advance(p);
        return parse_string_token(p, t);
    case TOK_KW_TRUE:
    case TOK_KW_FALSE:
        advance(p);
        e = new_expr(p, EXPR_BOOL, t->loc);
        e->u.bool_val = t->kind == TOK_KW_TRUE;
        return e;
    case TOK_KW_NULL:
        advance(p);
        return new_expr(p, EXPR_NULL, t->loc);
    case TOK_KW_THIS:
        advance(p);
        return new_expr(p, EXPR_THIS, t->loc);

    case TOK_IDENT: {
        TokenKind next = peek_at(p, 1)->kind;
        /* module::Type { field: value } */
        if (next == TOK_COLONCOLON && peek_at(p, 2)->kind == TOK_IDENT && peek_at(p, 3)->kind == TOK_LBRACE &&
            (peek_at(p, 4)->kind == TOK_RBRACE ||
             (peek_at(p, 4)->kind == TOK_IDENT && peek_at(p, 5)->kind == TOK_COLON))) {
            advance(p);
            advance(p);
            Expr *lit = parse_struct_lit(p);
            if (lit) {
                size_t n = t->loc.len + strlen(lit->u.struct_lit.type_name) + 3;
                char *full = arena_alloc(p->arena, n);
                snprintf(full, n, "%.*s::%s", (int)t->loc.len, t->start, lit->u.struct_lit.type_name);
                lit->u.struct_lit.type_name = full;
                lit->loc = span_to_prev(p, t->loc);
            }
            return lit;
        }
        if (next == TOK_COLONCOLON) {
            advance(p);
            advance(p);
            Token *member = expect_ident(p, "a name after '::'");
            if (!member) return NULL;
            e = new_expr(p, EXPR_SCOPE, span_to_prev(p, t->loc));
            e->u.scope.scope = tok_name(p, t);
            e->u.scope.member = tok_name(p, member);
            if (check(p, TOK_COLONCOLON) && peek_at(p, 1)->kind == TOK_IDENT) {
                /* module::Enum::Variant */
                advance(p);
                Token *variant = advance(p);
                size_t n = strlen(e->u.scope.scope) + strlen(e->u.scope.member) + 3;
                char *full = arena_alloc(p->arena, n);
                snprintf(full, n, "%s::%s", e->u.scope.scope, e->u.scope.member);
                e->u.scope.scope = full;
                e->u.scope.member = tok_name(p, variant);
                e->loc = span_to_prev(p, t->loc);
            }
            return e;
        }
        if (next == TOK_LBRACE &&
            (peek_at(p, 2)->kind == TOK_RBRACE ||
             (peek_at(p, 2)->kind == TOK_IDENT && peek_at(p, 3)->kind == TOK_COLON)))
            return parse_struct_lit(p);
        advance(p);
        e = new_expr(p, EXPR_IDENT, t->loc);
        e->u.name = tok_name(p, t);
        return e;
    }

    case TOK_LPAREN: {
        advance(p);
        e = parse_expr_nested(p);
        if (!e) return NULL;
        if (!expect(p, TOK_RPAREN, "to close `(`")) return NULL;
        e->parenthesized = true;
        return e;
    }

    case TOK_LBRACKET:
        return parse_array_lit(p);

    case TOK_KW_NEW: {
        advance(p);
        TypeExpr *ty = parse_base_type(p);
        if (!ty) return NULL;
        e = new_expr(p, EXPR_NEW, t->loc);
        e->u.new_.type = ty;
        if (accept(p, TOK_LPAREN))
            e->u.new_.args = parse_expr_list(p, TOK_RPAREN, "to close constructor arguments", &e->u.new_.nargs);
        e->loc = span_to_prev(p, t->loc);
        return e;
    }

    case TOK_KW_ALLOC: {
        advance(p);
        if (!expect(p, TOK_LPAREN, "after `alloc`")) return NULL;
        TypeExpr *ty = parse_type(p, false);
        if (!ty) return NULL;
        e = new_expr(p, EXPR_ALLOC, t->loc);
        e->u.alloc.type = ty;
        if (accept(p, TOK_COMMA)) {
            e->u.alloc.count = parse_expr_nested(p);
            if (!e->u.alloc.count) return NULL;
        }
        if (!expect(p, TOK_RPAREN, "to close `alloc(`")) return NULL;
        e->loc = span_to_prev(p, t->loc);
        return e;
    }

    case TOK_KW_SIZEOF: {
        advance(p);
        if (!expect(p, TOK_LPAREN, "after `sizeof`")) return NULL;
        TypeExpr *ty = parse_type(p, false);
        if (!ty || !expect(p, TOK_RPAREN, "to close `sizeof(`")) return NULL;
        e = new_expr(p, EXPR_SIZEOF, span_to_prev(p, t->loc));
        e->u.sizeof_type = ty;
        return e;
    }

    case TOK_KW_STRING_T:
    case TOK_KW_MIXED_T:
        /* string { ptr: p, len: n } builds a string from raw parts; the
         * runtime builds mixed values the same way. */
        if (peek_at(p, 1)->kind == TOK_LBRACE) return parse_struct_lit(p);
        /* fall through */
    default:
        if (is_builtin_type(t->kind)) {
            Diag *d = perr_tok(p, "E0105", t, "expected a value, found the type `%s`", token_kind_spelling(t->kind));
            diag_label(d, "this is a type, not a value");
            diag_help(d, "to convert a value to `%s`, use `as`: `value as %s`", token_kind_spelling(t->kind),
                      token_kind_spelling(t->kind));
            diag_end(d);
        } else {
            Diag *d = perr_tok(p, "E0118", t, "expected an expression, found %s", describe(p, t));
            diag_label(d, "expected an expression");
            if (p->pos > 0) {
                const Token *prev = &p->toks[p->pos - 1];
                if (prev->kind >= TOK_PLUS && prev->kind <= TOK_SHR_ASSIGN)
                    diag_span(d, prev->loc, "`%s` needs a value after it", token_kind_spelling(prev->kind));
            }
            diag_end(d);
        }
        return NULL;
    }
}

static Expr *parse_postfix(Parser *p, Expr *e) {
    for (;;) {
        Token *t = cur(p);
        switch (t->kind) {
        case TOK_LPAREN: {
            advance(p);
            Expr *call = new_expr(p, EXPR_CALL, e->loc);
            call->u.call.callee = e;
            call->u.call.args = parse_expr_list(p, TOK_RPAREN, "to close argument list", &call->u.call.nargs);
            if (p->panic) return NULL;
            call->loc = span_to_prev(p, e->loc);
            e = call;
            break;
        }
        case TOK_LBRACKET: {
            advance(p);
            Expr *idx = new_expr(p, EXPR_INDEX, e->loc);
            idx->u.index.base = e;
            if (!check(p, TOK_RBRACKET)) {
                idx->u.index.index = parse_expr_nested(p);
                if (!idx->u.index.index) return NULL;
            }
            if (!expect(p, TOK_RBRACKET, "to close index")) return NULL;
            idx->loc = span_to_prev(p, e->loc);
            e = idx;
            break;
        }
        case TOK_ARROW: {
            advance(p);
            Token *name = expect_ident(p, "a member name after '->'");
            if (!name) return NULL;
            Expr *m = new_expr(p, EXPR_MEMBER, span_to_prev(p, e->loc));
            m->u.member.object = e;
            m->u.member.name = tok_name(p, name);
            m->u.member.name_loc = name->loc;
            e = m;
            break;
        }
        case TOK_PLUSPLUS:
        case TOK_MINUSMINUS: {
            advance(p);
            Expr *post = new_expr(p, EXPR_POSTFIX, span_to_prev(p, e->loc));
            post->u.unary.op = t->kind;
            post->u.unary.operand = e;
            e = post;
            break;
        }
        case TOK_QUESTION:
            /* `f()?` propagates errors; `c ? a : b` is a ternary. It's the
             * former when nothing that could start an expression follows. */
            if (can_start_expr(peek_at(p, 1)->kind)) return e;
            advance(p);
            {
                Expr *try_ = new_expr(p, EXPR_TRY, span_to_prev(p, e->loc));
                try_->u.try_expr = e;
                e = try_;
            }
            break;
        default:
            return e;
        }
    }
}

static Expr *parse_unary(Parser *p) {
    Token *t = cur(p);
    switch (t->kind) {
    case TOK_MINUS: case TOK_BANG: case TOK_TILDE: case TOK_AMP: case TOK_STAR:
    case TOK_PLUSPLUS: case TOK_MINUSMINUS: {
        advance(p);
        Expr *operand = parse_expr_bp(p, BP_UNARY);
        if (!operand) return NULL;
        Expr *e = new_expr(p, EXPR_UNARY, span_to_prev(p, t->loc));
        e->u.unary.op = t->kind;
        e->u.unary.operand = operand;
        return e;
    }
    default: {
        Expr *e = parse_primary(p);
        return e ? parse_postfix(p, e) : NULL;
    }
    }
}

static bool is_assign_op(TokenKind k) {
    return k >= TOK_ASSIGN && k <= TOK_SHR_ASSIGN;
}

static bool is_cmp_op(TokenKind k) {
    return k == TOK_EQ || k == TOK_NE || k == TOK_LT || k == TOK_GT || k == TOK_LE || k == TOK_GE;
}

static int infix_bp(Parser *p, TokenKind k) {
    if (is_assign_op(k)) return BP_ASSIGN;
    if (is_cmp_op(k)) return BP_CMP;
    switch (k) {
    case TOK_QUESTION:   return BP_TERNARY;
    case TOK_QQ:         return BP_COALESCE;
    case TOK_OROR:       return BP_OR;
    case TOK_ANDAND:     return BP_AND;
    case TOK_DOT:        return BP_CONCAT;
    case TOK_PIPE:       return BP_BOR;
    case TOK_CARET:      return BP_XOR;
    case TOK_AMP:        return BP_BAND;
    case TOK_SHL: case TOK_SHR: return BP_SHIFT;
    case TOK_PLUS: case TOK_MINUS: return BP_ADD;
    case TOK_STAR: case TOK_SLASH: case TOK_PERCENT: return BP_MUL;
    case TOK_KW_AS:      return p->no_as ? BP_NONE : BP_AS;
    default:             return BP_NONE;
    }
}

static bool enter(Parser *p) {
    if (++p->depth <= MAX_NESTING) return true;
    Diag *d = perr_tok(p, "E0110", cur(p), "code is nested too deeply");
    diag_label(d, "more than %d levels deep here", MAX_NESTING);
    diag_help(d, "split the expression up using variables");
    diag_end(d);
    return false;
}

static Expr *parse_expr_bp_inner(Parser *p, int min_bp);

static Expr *parse_expr_bp(Parser *p, int min_bp) {
    Expr *e = enter(p) ? parse_expr_bp_inner(p, min_bp) : NULL;
    p->depth--;
    return e;
}

/* Source text of `loc`, or NULL if it spans lines. */
static char *loc_text(Parser *p, Loc loc) {
    const char *s = p->file->text + loc.offset;
    if (memchr(s, '\n', loc.len)) return NULL;
    return arena_strndup(p->arena, s, loc.len);
}

/* `a < b < c`: suggests `a < b && b < c`. */
static void report_chained_comparison(Parser *p, const Expr *lhs, const Token *op) {
    Diag *d = perr_tok(p, "E0103", op, "comparison operators can't be chained");
    diag_label(d, "second comparison");
    diag_span(d, lhs->loc, "this is already a comparison");
    diag_note(d, "in C, `a < b < c` compares the true/false result of `a < b` with `c`");

    /* Peek at the right operand to build the suggestion. */
    size_t saved = p->pos;
    bool saved_panic = p->panic;
    p->panic = true;  /* any errors while peeking stay quiet */
    Expr *rhs = parse_expr_bp(p, BP_CMP + 1);  /* the operator is already consumed */
    p->pos = saved;
    p->panic = saved_panic;

    char *a = loc_text(p, lhs->loc);
    char *b = loc_text(p, lhs->u.binary.rhs->loc);
    char *c = rhs ? loc_text(p, rhs->loc) : NULL;
    if (a && b && c) {
        size_t n = strlen(a) + strlen(b) + strlen(c) + 16;
        char *fixed = arena_alloc(p->arena, n);
        snprintf(fixed, n, "%s && %s %s %s", a, b, token_kind_spelling(op->kind), c);
        Loc whole = lhs->loc;
        whole.len = rhs->loc.offset + rhs->loc.len - lhs->loc.offset;
        diag_fix(d, whole, fixed, "split it into two comparisons");
    } else {
        diag_help(d, "split it into two comparisons joined by `&&`");
    }
    diag_end(d);
}

static Expr *parse_expr_bp_inner(Parser *p, int min_bp) {
    Expr *lhs = parse_unary(p);
    if (!lhs) return NULL;

    for (;;) {
        Token *t = cur(p);
        int bp = infix_bp(p, t->kind);
        if (bp == BP_NONE || bp < min_bp) break;
        advance(p);

        if (bp == BP_ASSIGN) {
            Expr *rhs = parse_expr_bp(p, BP_ASSIGN);  /* right-assoc */
            if (!rhs) return NULL;
            Expr *e = new_expr(p, EXPR_ASSIGN, span_to_prev(p, lhs->loc));
            e->u.binary.op = t->kind;
            e->u.binary.lhs = lhs;
            e->u.binary.rhs = rhs;
            lhs = e;
        } else if (bp == BP_TERNARY) {
            Expr *then_expr = parse_expr_nested(p);
            if (!then_expr) return NULL;
            if (!check(p, TOK_COLON)) {
                Diag *d = perr_tok(p, "E0104", cur(p), "expected `:` in `?:` expression, found %s", describe(p, cur(p)));
                diag_label(d, "expected `:` and a value for when the condition is false");
                diag_span(d, t->loc, "this `?` starts a `cond ? a : b` expression");
                diag_help(d, "if you meant error propagation, wrap it in parentheses: `(expr?)`");
                diag_end(d);
                return NULL;
            }
            advance(p);
            Expr *else_expr = parse_expr_bp(p, BP_TERNARY);  /* right-assoc */
            if (!else_expr) return NULL;
            Expr *e = new_expr(p, EXPR_TERNARY, span_to_prev(p, lhs->loc));
            e->u.ternary.cond = lhs;
            e->u.ternary.then_expr = then_expr;
            e->u.ternary.else_expr = else_expr;
            lhs = e;
        } else if (bp == BP_COALESCE) {
            Expr *rhs = parse_expr_bp(p, BP_COALESCE);  /* right-assoc: a ?? b ?? c */
            if (!rhs) return NULL;
            Expr *e = new_expr(p, EXPR_BINARY, span_to_prev(p, lhs->loc));
            e->u.binary.op = TOK_QQ;
            e->u.binary.lhs = lhs;
            e->u.binary.rhs = rhs;
            lhs = e;
        } else if (bp == BP_AS) {
            TypeExpr *ty = parse_type(p, true);
            if (!ty) return NULL;
            Expr *e = new_expr(p, EXPR_CAST, span_to_prev(p, lhs->loc));
            e->u.cast.expr = lhs;
            e->u.cast.type = ty;
            lhs = e;
        } else {
            if (bp == BP_CMP && lhs->kind == EXPR_BINARY && is_cmp_op(lhs->u.binary.op) && !lhs->parenthesized) {
                report_chained_comparison(p, lhs, t);
                return NULL;
            }
            Expr *rhs = parse_expr_bp(p, bp + 1);  /* left-assoc */
            if (!rhs) return NULL;
            Expr *e = new_expr(p, EXPR_BINARY, span_to_prev(p, lhs->loc));
            e->u.binary.op = t->kind;
            e->u.binary.lhs = lhs;
            e->u.binary.rhs = rhs;
            lhs = e;
        }
    }
    return lhs;
}

static Expr *parse_expr(Parser *p) {
    return parse_expr_bp(p, BP_ASSIGN);
}

/* ---- statements -------------------------------------------------------------- */

/* `Type name [= init]` without the trailing `;`. */
static VarDecl *parse_var_decl(Parser *p) {
    TypeExpr *ty = parse_type(p, false);
    if (!ty) return NULL;
    Token *name = expect_ident(p, "a variable name");
    if (!name) return NULL;
    VarDecl *v = ARENA_NEW(p->arena, VarDecl);
    v->type = ty;
    v->name = tok_name(p, name);
    v->loc = name->loc;
    if (accept(p, TOK_ASSIGN)) {
        v->init = parse_expr(p);
        if (!v->init) return NULL;
    }
    return v;
}

/* A statement used as the body of if/while/for/defer. */
static Stmt *parse_body(Parser *p, const char *what) {
    if (looks_like_decl(p) && !check(p, TOK_LBRACE)) {
        Diag *d = perr_tok(p, "E0106", cur(p), "a variable declaration can't be the whole body of %s", what);
        diag_label(d, "the variable would go out of scope immediately");
        diag_help(d, "if this is intentional, wrap it in braces: `{ ... }`");
        diag_end(d);
        return NULL;
    }
    return parse_stmt(p);
}

static Expr *parse_paren_cond(Parser *p, const char *what) {
    char context[48];
    snprintf(context, sizeof context, "after `%s`", what);
    if (!expect(p, TOK_LPAREN, context)) return NULL;
    Expr *cond = parse_expr_nested(p);
    if (!cond) return NULL;
    snprintf(context, sizeof context, "to close `%s` condition", what);
    if (!expect(p, TOK_RPAREN, context)) return NULL;
    return cond;
}

/* foreach binding: `name` or `Type name`. */
static VarDecl *parse_foreach_binding(Parser *p) {
    VarDecl *v = ARENA_NEW(p->arena, VarDecl);
    TokenKind next = peek_at(p, 1)->kind;
    if (!(check(p, TOK_IDENT) && (next == TOK_FATARROW || next == TOK_RPAREN))) {
        v->type = parse_type(p, false);
        if (!v->type) return NULL;
    }
    Token *name = expect_ident(p, "a loop variable name");
    if (!name) return NULL;
    v->name = tok_name(p, name);
    v->loc = name->loc;
    return v;
}

static Stmt *parse_foreach(Parser *p) {
    Token *kw = advance(p);
    Stmt *s = new_stmt(p, STMT_FOREACH, kw->loc);
    if (!expect(p, TOK_LPAREN, "after `foreach`")) return NULL;

    bool saved = p->no_as;
    p->no_as = true;
    s->u.foreach_.iterable = parse_expr(p);
    p->no_as = saved;
    if (!s->u.foreach_.iterable) return NULL;

    if (!expect(p, TOK_KW_AS, "in foreach (write: foreach (items as item))")) return NULL;
    VarDecl *first = parse_foreach_binding(p);
    if (!first) return NULL;
    if (accept(p, TOK_FATARROW)) {
        s->u.foreach_.key = first;
        s->u.foreach_.value = parse_foreach_binding(p);
        if (!s->u.foreach_.value) return NULL;
    } else {
        s->u.foreach_.value = first;
    }
    if (!expect(p, TOK_RPAREN, "to close `foreach (`")) return NULL;
    s->u.foreach_.body = parse_body(p, "a foreach loop");
    return s->u.foreach_.body ? s : NULL;
}

static Stmt *parse_for(Parser *p) {
    Token *kw = advance(p);
    Stmt *s = new_stmt(p, STMT_FOR, kw->loc);
    if (!expect(p, TOK_LPAREN, "after `for`")) return NULL;

    if (!check(p, TOK_SEMI)) {
        Loc start = cur(p)->loc;
        if (looks_like_decl(p)) {
            VarDecl *v = parse_var_decl(p);
            if (!v) return NULL;
            s->u.for_.init = new_stmt(p, STMT_VAR, span_to_prev(p, start));
            s->u.for_.init->u.var = v;
        } else {
            Expr *e = parse_expr_nested(p);
            if (!e) return NULL;
            s->u.for_.init = new_stmt(p, STMT_EXPR, e->loc);
            s->u.for_.init->u.expr = e;
        }
    }
    if (!expect(p, TOK_SEMI, "after for-loop initializer")) return NULL;
    if (!check(p, TOK_SEMI)) {
        s->u.for_.cond = parse_expr_nested(p);
        if (!s->u.for_.cond) return NULL;
    }
    if (!expect(p, TOK_SEMI, "after for-loop condition")) return NULL;
    if (!check(p, TOK_RPAREN)) {
        s->u.for_.step = parse_expr_nested(p);
        if (!s->u.for_.step) return NULL;
    }
    if (!expect(p, TOK_RPAREN, "to close `for (`")) return NULL;
    s->u.for_.body = parse_body(p, "a for loop");
    return s->u.for_.body ? s : NULL;
}

static AsmOperand *parse_asm_operand(Parser *p) {
    Token *dir = cur(p);
    AsmOperand *op = ARENA_NEW(p->arena, AsmOperand);
    op->loc = dir->loc;
    if (dir->kind == TOK_IDENT && dir->loc.len == 2 && memcmp(dir->start, "in", 2) == 0) op->dir = ASM_IN;
    else if (dir->kind == TOK_IDENT && dir->loc.len == 3 && memcmp(dir->start, "out", 3) == 0) op->dir = ASM_OUT;
    else if (dir->kind == TOK_IDENT && dir->loc.len == 5 && memcmp(dir->start, "inout", 5) == 0) op->dir = ASM_INOUT;
    else {
        Diag *d = perr_tok(p, "E0115", dir, "expected `in`, `out` or `inout`, found %s", describe(p, dir));
        diag_label(d, "not an asm operand direction");
        diag_help(d, "operands look like `in(\"rax\") value` or `out(\"rax\") variable`");
        diag_end(d);
        return NULL;
    }
    advance(p);
    if (!expect(p, TOK_LPAREN, "before asm register name")) return NULL;
    if (!parse_plain_string(p, "an asm register", &op->reg)) return NULL;
    if (!expect(p, TOK_RPAREN, "after asm register name")) return NULL;
    op->loc = span_to_prev(p, dir->loc);   /* `in("rax")` */
    op->expr = parse_expr_nested(p);
    return op->expr ? op : NULL;
}

/* asm { "cli"; "hlt"; }   or   asm("out dx, al" : in("dx") port, in("al") val); */
static Stmt *parse_asm(Parser *p) {
    Token *kw = advance(p);
    Stmt *s = new_stmt(p, STMT_ASM, kw->loc);
    PtrVec lines = {0}, ops = {0};

    if (accept(p, TOK_LBRACE)) {
        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            StrBuf *line = ARENA_NEW(p->arena, StrBuf);
            if (!parse_plain_string(p, "an asm instruction", line)) {
                /* Skip the rest of the asm block so its `}` can't be
                 * mistaken for the end of the function. */
                while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) advance(p);
                accept(p, TOK_RBRACE);
                p->panic = false;
                free(lines.items);
                return NULL;
            }
            ptrvec_push(&lines, line);
            accept(p, TOK_SEMI);
        }
        expect(p, TOK_RBRACE, "to close `asm {`");
    } else if (accept(p, TOK_LPAREN)) {
        do {
            StrBuf *line = ARENA_NEW(p->arena, StrBuf);
            if (!parse_plain_string(p, "an asm instruction", line)) break;
            ptrvec_push(&lines, line);
        } while (check(p, TOK_STRING));
        if (!p->panic && accept(p, TOK_COLON)) {
            do {
                AsmOperand *op = parse_asm_operand(p);
                if (!op) break;
                ptrvec_push(&ops, op);
            } while (accept(p, TOK_COMMA));
        }
        if (!p->panic && expect(p, TOK_RPAREN, "to close `asm(`")) expect_semi(p, "after asm statement");
    } else {
        Diag *d = perr_tok(p, "E0100", cur(p), "expected `{` or `(` after `asm`, found %s", describe(p, cur(p)));
        diag_label(d, "expected `{` or `(`");
        diag_help(d, "write `asm { \"cli\"; \"hlt\"; }` or `asm(\"out dx, al\" : in(\"dx\") port, in(\"al\") v);`");
        diag_end(d);
    }

    s->u.asm_.lines = ptrvec_finish(&lines, p->arena, &s->u.asm_.nlines);
    s->u.asm_.ops = ptrvec_finish(&ops, p->arena, &s->u.asm_.nops);
    s->loc = span_to_prev(p, kw->loc);
    return p->panic ? NULL : s;
}

static Stmt *parse_stmt_inner(Parser *p);

static Stmt *parse_stmt(Parser *p) {
    Stmt *s = enter(p) ? parse_stmt_inner(p) : NULL;
    p->depth--;
    return s;
}

static Stmt *parse_stmt_inner(Parser *p) {
    Token *t = cur(p);
    Stmt *s;

    switch (t->kind) {
    case TOK_LBRACE:
        return parse_block(p);

    case TOK_SEMI:
        advance(p);
        return new_stmt(p, STMT_BLOCK, t->loc);  /* empty statement */

    case TOK_KW_IF: {
        advance(p);
        s = new_stmt(p, STMT_IF, t->loc);
        if (!(s->u.if_.cond = parse_paren_cond(p, "if"))) return NULL;
        if (!(s->u.if_.then_stmt = parse_body(p, "an if"))) return NULL;
        if (accept(p, TOK_KW_ELSE) && !(s->u.if_.else_stmt = parse_body(p, "an else"))) return NULL;
        return s;
    }

    case TOK_KW_WHILE:
        advance(p);
        s = new_stmt(p, STMT_WHILE, t->loc);
        if (!(s->u.while_.cond = parse_paren_cond(p, "while"))) return NULL;
        if (!(s->u.while_.body = parse_body(p, "a while loop"))) return NULL;
        return s;

    case TOK_KW_DO:
        advance(p);
        s = new_stmt(p, STMT_DO_WHILE, t->loc);
        if (!(s->u.while_.body = parse_body(p, "a do-while loop"))) return NULL;
        if (!expect(p, TOK_KW_WHILE, "after do-while body")) return NULL;
        if (!(s->u.while_.cond = parse_paren_cond(p, "while"))) return NULL;
        expect_semi(p, "after do-while");
        return s;

    case TOK_KW_FOR:
        return parse_for(p);

    case TOK_KW_FOREACH:
        return parse_foreach(p);

    case TOK_KW_RETURN:
        advance(p);
        s = new_stmt(p, STMT_RETURN, t->loc);
        if (!check(p, TOK_SEMI) && !(s->u.expr = parse_expr(p))) return NULL;
        expect_semi(p, "after return");
        s->loc = span_to_prev(p, t->loc);
        return s;

    case TOK_KW_BREAK:
    case TOK_KW_CONTINUE:
        advance(p);
        s = new_stmt(p, t->kind == TOK_KW_BREAK ? STMT_BREAK : STMT_CONTINUE, t->loc);
        expect_semi(p, t->kind == TOK_KW_BREAK ? "after break" : "after continue");
        return s;

    case TOK_KW_ECHO: {
        advance(p);
        s = new_stmt(p, STMT_ECHO, t->loc);
        PtrVec exprs = {0};
        do {
            Expr *e = parse_expr(p);
            if (!e) break;
            ptrvec_push(&exprs, e);
        } while (accept(p, TOK_COMMA));
        s->u.echo.exprs = ptrvec_finish(&exprs, p->arena, &s->u.echo.n);
        if (p->panic) return NULL;
        expect_semi(p, "after echo");
        s->loc = span_to_prev(p, t->loc);
        return s;
    }

    case TOK_KW_DEFER:
        advance(p);
        s = new_stmt(p, STMT_DEFER, t->loc);
        if (check(p, TOK_KW_DEFER) || check(p, TOK_KW_RETURN)) {
            Diag *d = perr_tok(p, "E0108", cur(p), "`defer %s` is not allowed", token_kind_spelling(cur_kind(p)));
            diag_label(d, "can't be deferred");
            diag_span(d, t->loc, "`defer` runs its statement when the block exits");
            diag_end(d);
            return NULL;
        }
        if (!(s->u.defer_body = parse_body(p, "a defer"))) return NULL;
        return s;

    case TOK_KW_ASM:
        return parse_asm(p);

    case TOK_KW_FUNCTION: case TOK_KW_STRUCT: case TOK_KW_CLASS: case TOK_KW_ENUM:
    case TOK_KW_IMPORT: case TOK_KW_EXPORT: case TOK_KW_EXTERN:
        {
            Diag *d = perr_tok(p, "E0107", t, "`%s` is only allowed at the top level of a file",
                               token_kind_spelling(t->kind));
            diag_label(d, "not allowed inside a function");
            if (t->kind == TOK_KW_FUNCTION)
                diag_help(d, "watrlang has no nested functions; if this should be a separate function, "
                             "check that the one before it ends with `}`");
            diag_end(d);
        }
        return NULL;

    default:
        break;
    }

    if (looks_like_decl(p)) {
        VarDecl *v = parse_var_decl(p);
        if (!v) return NULL;
        s = new_stmt(p, STMT_VAR, span_to_prev(p, t->loc));
        s->u.var = v;
        expect_semi(p, "after variable declaration");
        return s;
    }

    Expr *e = parse_expr(p);
    if (!e) return NULL;
    s = new_stmt(p, STMT_EXPR, e->loc);
    s->u.expr = e;
    expect_semi(p, "after expression");
    return s;
}

static Stmt *parse_block(Parser *p) {
    Token *open = cur(p);
    if (!expect(p, TOK_LBRACE, "to start a block")) return NULL;
    Stmt *block = new_stmt(p, STMT_BLOCK, open->loc);
    PtrVec stmts = {0};

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        size_t before = p->pos;
        Stmt *s = parse_stmt(p);
        if (s) ptrvec_push(&stmts, s);
        if (p->panic) sync_stmt(p);
        if (p->pos == before) advance(p);  /* always make progress */
    }
    block->u.block.stmts = ptrvec_finish(&stmts, p->arena, &block->u.block.n);
    if (!check(p, TOK_RBRACE)) {
        Diag *d = perr(p, "E0109", open->loc, "unclosed delimiter `{`");
        diag_label(d, "this `{` is never closed");
        diag_note(d, "the file ended before the matching `}`");
        diag_end(d);
        return block;
    }
    advance(p);
    block->loc = span_to_prev(p, open->loc);
    return block;
}

/* ---- declarations ------------------------------------------------------------ */

static Decl *new_decl(Parser *p, DeclKind kind, Loc loc) {
    Decl *d = ARENA_NEW(p->arena, Decl);
    d->kind = kind;
    d->loc = loc;
    return d;
}

/* function name(Type a, Type b, ...): Ret { body }   (current token: `function`) */
static Decl *parse_function(Parser *p, Loc start, bool is_export, bool is_extern) {
    advance(p); /* function */
    Token *name = expect_ident(p, "a function name");
    if (!name) return NULL;
    Decl *d = new_decl(p, DECL_FUNC, start);
    d->name = tok_name(p, name);
    d->name_loc = name->loc;
    d->is_export = is_export;
    d->is_extern = is_extern;

    if (!expect(p, TOK_LPAREN, "after function name")) return NULL;
    PtrVec params = {0};
    if (!check(p, TOK_RPAREN)) {
        do {
            if (check(p, TOK_ELLIPSIS)) {
                advance(p);
                d->u.func.variadic = true;
                if (!check(p, TOK_RPAREN)) {
                    Diag *e = perr_tok(p, "E0116", cur(p), "`...` must be the last parameter");
                    diag_label(e, "nothing can come after `...`");
                    diag_end(e);
                }
                break;
            }
            TypeExpr *ty = parse_type(p, false);
            if (!ty) break;
            Token *pname = expect_ident(p, "a parameter name");
            if (!pname) break;
            Param *prm = ARENA_NEW(p->arena, Param);
            prm->type = ty;
            prm->name = tok_name(p, pname);
            prm->loc = pname->loc;
            ptrvec_push(&params, prm);
        } while (accept(p, TOK_COMMA));
    }
    d->u.func.params = ptrvec_finish(&params, p->arena, &d->u.func.nparams);
    if (p->panic || !expect(p, TOK_RPAREN, "to close parameter list")) return NULL;

    if (accept(p, TOK_COLON)) {
        d->u.func.ret = parse_type(p, false);
        if (!d->u.func.ret) return NULL;
    }

    if (check(p, TOK_SEMI)) {
        if (!is_extern) {
            Diag *e = perr_tok(p, "E0111", cur(p), "function `%s` has no body", d->name);
            diag_label(e, "expected `{` and a body");
            diag_note(e, "only `extern` functions, defined outside watrlang, end with `;`");
            diag_fix(e, cur(p)->loc, " { }", "add a body");
            diag_end(e);
            return NULL;
        }
        advance(p);
    } else if (is_extern) {
        Diag *e = perr_tok(p, "E0111", cur(p), "extern function `%s` can't have a body", d->name);
        diag_label(e, "unexpected body");
        diag_span(e, start, "`extern` means the function is defined elsewhere, e.g. in C");
        diag_help(e, "end the declaration with `;`, or remove `extern` to define it here");
        diag_end(e);
        return NULL;
    } else {
        if (!check(p, TOK_LBRACE)) {
            Diag *e = perr_tok(p, "E0100", cur(p), "expected `{` to start the body of `%s`, found %s", d->name,
                               describe(p, cur(p)));
            diag_label(e, "expected `{`");
            diag_end(e);
            return NULL;
        }
        d->u.func.body = parse_block(p);
        if (!d->u.func.body) return NULL;
    }
    d->loc = span_to_prev(p, start);
    return d;
}

static Decl *parse_struct(Parser *p, Loc start) {
    advance(p); /* struct */
    Token *name = expect_ident(p, "a struct name");
    if (!name || !expect(p, TOK_LBRACE, "after struct name")) return NULL;
    Decl *d = new_decl(p, DECL_STRUCT, start);
    d->name = tok_name(p, name);
    d->name_loc = name->loc;

    PtrVec fields = {0};
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        size_t before = p->pos;
        VarDecl *f = NULL;
        TypeExpr *ty = parse_type(p, false);
        Token *fname = ty ? expect_ident(p, "a field name") : NULL;
        if (fname) {
            f = ARENA_NEW(p->arena, VarDecl);
            f->type = ty;
            f->name = tok_name(p, fname);
            f->loc = fname->loc;
            if (check(p, TOK_ASSIGN))
            {
                Diag *e = perr_tok(p, "E0113", cur(p), "struct fields can't have default values");
                diag_label(e, "default value not allowed");
                diag_span(e, fname->loc, "field `%s`", f->name);
                diag_help(e, "set fields with a struct literal, `Name { %s: value }`, or use a class with a constructor",
                          f->name);
                diag_end(e);
            }
            else
                expect_semi(p, "after struct field");
        }
        if (f && !p->panic) ptrvec_push(&fields, f);
        if (p->panic) sync_stmt(p);
        if (p->pos == before) advance(p);
    }
    d->u.struct_.fields = ptrvec_finish(&fields, p->arena, &d->u.struct_.n);
    expect(p, TOK_RBRACE, "to close struct");
    d->loc = span_to_prev(p, start);
    return d;
}

static Decl *parse_class(Parser *p, Loc start) {
    advance(p); /* class */
    Token *name = expect_ident(p, "a class name");
    if (!name || !expect(p, TOK_LBRACE, "after class name")) return NULL;
    Decl *d = new_decl(p, DECL_CLASS, start);
    d->name = tok_name(p, name);
    d->name_loc = name->loc;

    PtrVec members = {0};
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        size_t before = p->pos;
        Loc mstart = cur(p)->loc;
        Visibility vis = VIS_DEFAULT;
        if (accept(p, TOK_KW_PUBLIC)) vis = VIS_PUBLIC;
        else if (accept(p, TOK_KW_PRIVATE)) vis = VIS_PRIVATE;
        bool is_static = accept(p, TOK_KW_STATIC);

        Decl *m = NULL;
        if (check(p, TOK_KW_FUNCTION)) {
            m = parse_function(p, mstart, false, false);
            if (m) m->is_static = is_static;
        } else {
            VarDecl *v = parse_var_decl(p);
            if (v) {
                expect_semi(p, "after class field");
                m = new_decl(p, DECL_VAR, span_to_prev(p, mstart));
                m->name = v->name;
                m->name_loc = v->loc;
                m->u.var = v;
            }
        }
        if (m) {
            m->vis = vis;
            m->is_static = is_static;
            ptrvec_push(&members, m);
        }
        if (p->panic) sync_stmt(p);
        if (p->pos == before) advance(p);
    }
    d->u.class_.members = ptrvec_finish(&members, p->arena, &d->u.class_.n);
    expect(p, TOK_RBRACE, "to close class");
    d->loc = span_to_prev(p, start);
    return d;
}

/* enum Name [: Type] { A, B = 5, C } */
static Decl *parse_enum(Parser *p, Loc start) {
    advance(p); /* enum */
    Token *name = expect_ident(p, "an enum name");
    if (!name) return NULL;
    Decl *d = new_decl(p, DECL_ENUM, start);
    d->name = tok_name(p, name);
    d->name_loc = name->loc;
    if (accept(p, TOK_COLON) && !(d->u.enum_.base = parse_type(p, false))) return NULL;
    if (!expect(p, TOK_LBRACE, "to start enum body")) return NULL;

    PtrVec items = {0};
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        Token *iname = expect_ident(p, "an enum value name");
        if (!iname) break;
        EnumItem *item = ARENA_NEW(p->arena, EnumItem);
        item->name = tok_name(p, iname);
        item->loc = iname->loc;
        if (accept(p, TOK_ASSIGN) && !(item->value = parse_expr(p))) break;
        ptrvec_push(&items, item);
        if (!accept(p, TOK_COMMA)) break;
    }
    d->u.enum_.items = ptrvec_finish(&items, p->arena, &d->u.enum_.n);
    if (p->panic || !expect(p, TOK_RBRACE, "to close enum")) return NULL;
    d->loc = span_to_prev(p, start);
    return d;
}

static Decl *parse_global_var(Parser *p, Loc start, bool is_export, bool is_extern) {
    VarDecl *v = parse_var_decl(p);
    if (!v) return NULL;
    if (is_extern && v->init) {
        Diag *e = perr(p, "E0112", v->init->loc, "extern variable `%s` can't have an initializer", v->name);
        diag_label(e, "unexpected initializer");
        diag_note(e, "`extern` means the variable is defined elsewhere, which also gives it its value");
        diag_end(e);
        return NULL;
    }
    expect_semi(p, "after variable declaration");
    Decl *d = new_decl(p, DECL_VAR, span_to_prev(p, start));
    d->name = v->name;
    d->name_loc = v->loc;
    d->is_export = is_export;
    d->is_extern = is_extern;
    d->u.var = v;
    return d;
}

static Decl *parse_decl_inner(Parser *p, Loc start);

static Decl *parse_decl(Parser *p) {
    Token *t = cur(p);
    Loc start = t->loc;

    if (t->kind == TOK_KW_IMPORT) {
        advance(p);
        Decl *d = new_decl(p, DECL_IMPORT, start);
        if (!parse_plain_string(p, "an import path", &d->u.import.path)) return NULL;
        if (accept(p, TOK_KW_AS)) {   /* import "std/io" as out; */
            Token *alias = expect_ident(p, "a module alias");
            if (!alias) return NULL;
            d->name = tok_name(p, alias);
            d->name_loc = alias->loc;
        }
        expect_semi(p, "after import");
        d->loc = span_to_prev(p, start);
        return d;
    }

    /* #[packed], #[align(16)]: attributes for the next declaration. */
    bool packed = false;
    uint32_t align = 0;
    Loc attr_loc = start;
    while (check(p, TOK_HASHBRACKET)) {
        advance(p);
        do {
            Token *name = expect_ident(p, "an attribute name");
            if (!name) return NULL;
            if (name->loc.len == 6 && memcmp(name->start, "packed", 6) == 0) {
                packed = true;
            } else if (name->loc.len == 5 && memcmp(name->start, "align", 5) == 0) {
                if (!expect(p, TOK_LPAREN, "after `align`")) return NULL;
                Token *n = cur(p);
                uint64_t v = n->kind == TOK_INT ? n->v.int_val : 0;
                if (n->kind != TOK_INT || v == 0 || (v & (v - 1)) != 0 || v > 4096) {
                    Diag *e = perr_tok(p, "E0119", n, "`align` needs a power of two from 1 to 4096");
                    diag_label(e, "not a valid alignment");
                    diag_end(e);
                    return NULL;
                }
                advance(p);
                align = (uint32_t)v;
                if (!expect(p, TOK_RPAREN, "after the alignment")) return NULL;
            } else {
                Diag *e = perr_tok(p, "E0119", name, "unknown attribute `%.*s`", (int)name->loc.len, name->start);
                diag_label(e, "not an attribute");
                diag_note(e, "attributes are `#[packed]` and `#[align(N)]`");
                diag_end(e);
                return NULL;
            }
        } while (accept(p, TOK_COMMA));
        if (!expect(p, TOK_RBRACKET, "to close the attribute")) return NULL;
        attr_loc = span_to_prev(p, start);
    }
    bool has_attrs = packed || align;

    /* `private` keeps a declaration inside its module. */
    bool is_private = accept(p, TOK_KW_PRIVATE);
    if (is_private && (check(p, TOK_KW_EXPORT) || check(p, TOK_KW_EXTERN) || check(p, TOK_KW_IMPORT))) {
        Diag *e = perr_tok(p, "E0112", cur(p), "`private` can't be combined with `%s`", token_kind_spelling(cur_kind(p)));
        diag_label(e, "not allowed with `private`");
        diag_end(e);
        return NULL;
    }
    Decl *d = parse_decl_inner(p, start);
    if (d && is_private) d->vis = VIS_PRIVATE;
    if (d && has_attrs) {
        bool ok = d->kind == DECL_STRUCT || (d->kind == DECL_VAR && !packed);
        if (!ok) {
            /* The declaration itself parsed fine, so no recovery is needed. */
            Diag *e = diag_start(DIAG_ERROR, "E0119", attr_loc, "`#[%s]` can't be used on %s", packed ? "packed" : "align",
                                 d->kind == DECL_FUNC ? "a function" : d->kind == DECL_VAR ? "a variable" : "this declaration");
            diag_label(e, "not allowed here");
            diag_note(e, "`#[packed]` applies to structs; `#[align(N)]` to structs and global variables");
            diag_end(e);
        }
        d->packed = packed;
        d->align = align;
        d->attr_loc = attr_loc;
    }
    return d;
}

static Decl *parse_decl_inner(Parser *p, Loc start) {
    bool is_export = false, is_extern = false;
    if (accept(p, TOK_KW_EXPORT)) is_export = true;
    else if (accept(p, TOK_KW_EXTERN)) is_extern = true;
    if ((is_export || is_extern) && (check(p, TOK_KW_EXPORT) || check(p, TOK_KW_EXTERN))) {
        Diag *e = perr_tok(p, "E0112", cur(p), "a declaration can't be both `export` and `extern`");
        diag_label(e, "second modifier");
        diag_span(e, start, "first modifier");
        diag_note(e, "`export` defines a symbol for others to use; `extern` uses one defined elsewhere");
        diag_end(e);
        return NULL;
    }

    switch (cur_kind(p)) {
    case TOK_KW_FUNCTION:
        return parse_function(p, start, is_export, is_extern);
    case TOK_KW_STRUCT:
    case TOK_KW_CLASS:
    case TOK_KW_ENUM:
        if (is_export || is_extern) {
            Diag *e = perr(p, "E0112", start, "`%s` only applies to functions and variables",
                           is_export ? "export" : "extern");
            diag_label(e, "not allowed here");
            diag_span(e, cur(p)->loc, "a %s can't be `%s`", token_kind_spelling(cur_kind(p)),
                      is_export ? "export" : "extern");
            diag_end(e);
            return NULL;
        }
        if (check(p, TOK_KW_STRUCT)) return parse_struct(p, start);
        if (check(p, TOK_KW_CLASS)) return parse_class(p, start);
        return parse_enum(p, start);
    default:
        break;
    }

    if (looks_like_decl(p) || is_export || is_extern) return parse_global_var(p, start, is_export, is_extern);

    {
        const Token *t2 = cur(p);
        Diag *e = perr_tok(p, "E0117", t2, "expected a declaration, found %s", describe(p, t2));
        diag_label(e, "expected `function`, `struct`, `class`, `enum`, `import` or a variable");
        switch (t2->kind) {
        case TOK_KW_RETURN: case TOK_KW_IF: case TOK_KW_WHILE: case TOK_KW_FOR: case TOK_KW_FOREACH:
        case TOK_KW_DO: case TOK_KW_ECHO: case TOK_KW_BREAK: case TOK_KW_CONTINUE: case TOK_KW_DEFER:
            diag_note(e, "statements have to be inside a function, such as `function main(): int { ... }`");
            break;
        case TOK_RBRACE:
            diag_note(e, "this `}` doesn't close anything; is there an extra one?");
            break;
        default:
            break;
        }
        diag_end(e);
    }
    return NULL;
}

Module *parse_file(Arena *arena, const SrcFile *file) {
    Lexer lx;
    lexer_init(&lx, file);
    Parser p = {arena, file, NULL, 0, 0, false, false, false, 0};
    p.toks = tokenize(&lx, &p.ntoks);

    Module *m = ARENA_NEW(arena, Module);
    m->file = file;
    PtrVec decls = {0};
    while (!check(&p, TOK_EOF)) {
        size_t before = p.pos;
        Decl *d = parse_decl(&p);
        if (d) ptrvec_push(&decls, d);
        if (p.panic) sync_top(&p);
        if (p.pos == before) advance(&p);
    }
    m->decls = ptrvec_finish(&decls, arena, &m->n);
    free(p.toks);
    return m;
}
