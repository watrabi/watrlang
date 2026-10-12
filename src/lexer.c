#include "lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const kind_names[TOK_COUNT] = {
#define X(name, spelling) #name,
    WATR_TOKENS(X)
#undef X
#define X(name, spelling) "KW_" #name,
    WATR_KEYWORDS(X)
#undef X
};

static const char *const kind_spellings[TOK_COUNT] = {
#define X(name, spelling) spelling,
    WATR_TOKENS(X)
    WATR_KEYWORDS(X)
#undef X
};

const char *token_kind_name(TokenKind k) {
    return k < TOK_COUNT ? kind_names[k] : "?";
}

const char *token_kind_spelling(TokenKind k) {
    return k < TOK_COUNT ? kind_spellings[k] : "?";
}

/* Longest spellings first so "<<=" wins over "<<" wins over "<". */
static const struct {
    const char *text;
    TokenKind kind;
} puncts[] = {
    {"<<=", TOK_SHL_ASSIGN}, {">>=", TOK_SHR_ASSIGN}, {"...", TOK_ELLIPSIS},

    {"->", TOK_ARROW}, {"=>", TOK_FATARROW}, {"::", TOK_COLONCOLON},
    {"++", TOK_PLUSPLUS}, {"--", TOK_MINUSMINUS},
    {"+=", TOK_PLUS_ASSIGN}, {"-=", TOK_MINUS_ASSIGN}, {"*=", TOK_STAR_ASSIGN},
    {"/=", TOK_SLASH_ASSIGN}, {"%=", TOK_PERCENT_ASSIGN}, {".=", TOK_DOT_ASSIGN},
    {"&=", TOK_AMP_ASSIGN}, {"|=", TOK_PIPE_ASSIGN}, {"^=", TOK_CARET_ASSIGN},
    {"==", TOK_EQ}, {"!=", TOK_NE}, {"<=", TOK_LE}, {">=", TOK_GE},
    {"&&", TOK_ANDAND}, {"||", TOK_OROR}, {"<<", TOK_SHL}, {">>", TOK_SHR},

    {"(", TOK_LPAREN}, {")", TOK_RPAREN}, {"{", TOK_LBRACE}, {"}", TOK_RBRACE},
    {"[", TOK_LBRACKET}, {"]", TOK_RBRACKET}, {",", TOK_COMMA}, {";", TOK_SEMI},
    {":", TOK_COLON}, {"?", TOK_QUESTION}, {"+", TOK_PLUS}, {"-", TOK_MINUS},
    {"*", TOK_STAR}, {"/", TOK_SLASH}, {"%", TOK_PERCENT}, {".", TOK_DOT},
    {"&", TOK_AMP}, {"|", TOK_PIPE}, {"^", TOK_CARET}, {"~", TOK_TILDE},
    {"!", TOK_BANG}, {"<", TOK_LT}, {">", TOK_GT}, {"=", TOK_ASSIGN},
};

static bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_ident_char(char c) {
    return is_ident_start(c) || is_digit(c);
}

/* Value of `c` as a digit in `base`, or -1. */
static int digit_value(char c, int base) {
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
    else return -1;
    return d < base ? d : -1;
}

void lexer_init(Lexer *l, const SrcFile *file) {
    l->file = file;
    l->src = file->text;
    l->len = file->len;
    l->pos = 0;
    l->line = 1;
    l->col = 1;
    /* Skip a UTF-8 byte order mark. */
    if (l->len >= 3 && memcmp(l->src, "\xEF\xBB\xBF", 3) == 0) l->pos = 3;
}

static char peek(const Lexer *l, size_t ahead) {
    return l->pos + ahead < l->len ? l->src[l->pos + ahead] : '\0';
}

static bool at_end(const Lexer *l) {
    return l->pos >= l->len;
}

static char advance(Lexer *l) {
    char c = l->src[l->pos++];
    if (c == '\n') {
        l->line++;
        l->col = 1;
    } else {
        l->col++;
    }
    return c;
}

static Loc here(const Lexer *l) {
    Loc loc = {l->file, (uint32_t)l->pos, l->line, l->col, 0};
    return loc;
}

static Loc span_from(const Lexer *l, Loc start) {
    start.len = (uint32_t)(l->pos - start.offset);
    return start;
}

static void skip_trivia(Lexer *l) {
    for (;;) {
        char c = peek(l, 0);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(l);
        } else if (c == '/' && peek(l, 1) == '/') {
            while (!at_end(l) && peek(l, 0) != '\n') advance(l);
        } else if (c == '/' && peek(l, 1) == '*') {
            Loc start = here(l);
            advance(l);
            advance(l);
            for (;;) {
                if (at_end(l)) {
                    start.len = 2;
                    diag_error(start, "unterminated /* comment");
                    return;
                }
                if (peek(l, 0) == '*' && peek(l, 1) == '/') {
                    advance(l);
                    advance(l);
                    break;
                }
                advance(l);
            }
        } else {
            return;
        }
    }
}

static TokenKind keyword_or_ident(const char *s, size_t n) {
    static const struct {
        const char *text;
        TokenKind kind;
    } keywords[] = {
#define X(name, spelling) {spelling, TOK_KW_##name},
        WATR_KEYWORDS(X)
#undef X
    };
    for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++) {
        if (strlen(keywords[i].text) == n && memcmp(keywords[i].text, s, n) == 0)
            return keywords[i].kind;
    }
    return TOK_IDENT;
}

/* Lexes digits in `base`, allowing `_` separators between digits.
 * Accumulates into *val and sets *overflow if it doesn't fit in u64.
 * Returns the number of digits consumed (not counting underscores). */
static size_t lex_digits(Lexer *l, int base, uint64_t *val, bool *overflow, bool *bad_underscore) {
    size_t ndigits = 0;
    bool last_underscore = false;
    for (;;) {
        char c = peek(l, 0);
        if (c == '_') {
            if (ndigits == 0 || last_underscore) *bad_underscore = true;
            last_underscore = true;
            advance(l);
            continue;
        }
        int d = digit_value(c, base);
        if (d < 0) break;
        if (*val > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) *overflow = true;
        *val = *val * (uint64_t)base + (uint64_t)d;
        ndigits++;
        last_underscore = false;
        advance(l);
    }
    if (last_underscore) *bad_underscore = true;
    return ndigits;
}

static Token lex_number(Lexer *l) {
    Loc start = here(l);
    Token t = {TOK_INT, 0, start, l->src + l->pos, {0}};
    uint64_t val = 0;
    bool overflow = false, bad_underscore = false, is_float = false;

    int base = 10;
    if (peek(l, 0) == '0') {
        char p = peek(l, 1);
        if (p == 'x' || p == 'X') base = 16;
        else if (p == 'b' || p == 'B') base = 2;
        else if (p == 'o' || p == 'O') base = 8;
    }

    if (base != 10) {
        advance(l);
        advance(l);
        if (lex_digits(l, base, &val, &overflow, &bad_underscore) == 0) {
            /* Report the bad digit itself if there is one, e.g. 0xZZ. */
            while (is_ident_char(peek(l, 0))) advance(l);
            diag_error(span_from(l, start), "expected digits after '%.2s'", t.start);
            t.kind = TOK_ERROR;
            t.loc = span_from(l, start);
            return t;
        }
    } else {
        lex_digits(l, 10, &val, &overflow, &bad_underscore);
        size_t int_len = l->pos - start.offset;

        /* `1.5` is a float; `1.` followed by a non-digit is `1` then `.` (concat). */
        if (peek(l, 0) == '.' && is_digit(peek(l, 1))) {
            is_float = true;
            advance(l);
            uint64_t ignored = 0;
            bool ignored_overflow = false;
            lex_digits(l, 10, &ignored, &ignored_overflow, &bad_underscore);
        }
        if (peek(l, 0) == 'e' || peek(l, 0) == 'E') {
            size_t sign = (peek(l, 1) == '+' || peek(l, 1) == '-') ? 1 : 0;
            if (is_digit(peek(l, 1 + sign))) {
                is_float = true;
                advance(l);
                if (sign) advance(l);
                uint64_t ignored = 0;
                bool ignored_overflow = false;
                lex_digits(l, 10, &ignored, &ignored_overflow, &bad_underscore);
            }
        }

        if (int_len > 1 && t.start[0] == '0' && is_digit(t.start[1])) {
            Loc zl = start;
            zl.len = (uint32_t)int_len;
            diag_error(zl, "leading zeros are not allowed (use 0o for octal)");
            t.kind = TOK_ERROR;
        }
    }

    if (is_ident_char(peek(l, 0))) {
        Loc bad = here(l);
        while (is_ident_char(peek(l, 0))) advance(l);
        bad = span_from(l, bad);
        diag_error(bad, "invalid character '%c' in number", t.start[bad.offset - start.offset]);
        t.kind = TOK_ERROR;
    }

    t.loc = span_from(l, start);
    if (t.kind == TOK_ERROR) return t;

    if (bad_underscore) {
        diag_error(t.loc, "'_' must sit between digits");
        t.kind = TOK_ERROR;
        return t;
    }

    if (is_float) {
        /* strtod doesn't understand '_', so copy the literal without them. */
        char *buf = xmalloc(t.loc.len + 1);
        size_t n = 0;
        for (uint32_t i = 0; i < t.loc.len; i++)
            if (t.start[i] != '_') buf[n++] = t.start[i];
        buf[n] = '\0';
        t.kind = TOK_FLOAT;
        t.v.float_val = strtod(buf, NULL);
        free(buf);
    } else if (overflow) {
        diag_error(t.loc, "integer literal is too large (max is 2^64 - 1)");
        t.kind = TOK_ERROR;
    } else {
        t.v.int_val = val;
    }
    return t;
}

/* Lexes the escape after a backslash. Returns the byte value or -1 on error. */
static int lex_escape(Lexer *l) {
    Loc start = here(l);
    start.offset--; /* include the backslash */
    start.col--;
    if (at_end(l)) return -1;
    char c = advance(l);
    switch (c) {
    case 'n':  return '\n';
    case 't':  return '\t';
    case 'r':  return '\r';
    case '0':  return '\0';
    case '\\': return '\\';
    case '"':  return '"';
    case '\'': return '\'';
    case '{':  return '{';
    case '}':  return '}';
    case 'x': {
        int hi = digit_value(peek(l, 0), 16);
        int lo = hi >= 0 ? digit_value(peek(l, 1), 16) : -1;
        if (lo < 0) {
            start.len = 2;
            diag_error(start, "\\x needs exactly two hex digits");
            return -1;
        }
        advance(l);
        advance(l);
        return hi * 16 + lo;
    }
    default:
        start.len = 2;
        if (c == '\n')
            diag_error(start, "unknown escape sequence '\\' at end of line");
        else
            diag_error(start, "unknown escape sequence '\\%c'", c);
        return -1;
    }
}

/* Result of lexing a string body. */
typedef enum {
    STR_OK,
    STR_ERROR,     /* an error was already reported */
    STR_NEWLINE,   /* single_line was set and a newline came first; not reported */
} StrResult;

static StrResult lex_string_body(Lexer *l, Loc open, uint32_t *flags, bool single_line);

typedef enum {
    INTERP_OK,
    INTERP_RECOVERED,  /* error reported, but the string can carry on */
    INTERP_FATAL,      /* error reported, end the string token here */
} InterpResult;

/* Skips a {interpolation} inside a string, up to and including its `}`.
 * Nested strings and braces are allowed: "{f("x")}", "{m[{a}]}".
 * An interpolation must fit on one line, so a forgotten `}` is reported
 * at the `{` instead of swallowing the rest of the file. */
static InterpResult lex_interp(Lexer *l, uint32_t *flags) {
    Loc open = here(l);
    open.len = 1;
    advance(l); /* { */
    *flags |= TOKF_INTERP;

    if (peek(l, 0) == '}') {
        advance(l);
        open.len = 2;
        diag_error(open, "empty interpolation (write \\{ for a literal brace)");
        return INTERP_RECOVERED;
    }

    int depth = 1;
    bool ok = true;
    for (;;) {
        char c = peek(l, 0);
        if (at_end(l) || c == '\n') {
            diag_error(open, "unterminated interpolation: missing '}' (write \\{ for a literal brace)");
            return INTERP_FATAL;
        }
        if (c == '{') {
            depth++;
            advance(l);
        } else if (c == '}') {
            advance(l);
            if (--depth == 0) return ok ? INTERP_OK : INTERP_RECOVERED;
        } else if (c == '"') {
            Loc inner = here(l);
            inner.len = 1;
            advance(l);
            uint32_t ignored = 0;
            StrResult r = lex_string_body(l, inner, &ignored, true);
            if (r == STR_NEWLINE) {
                diag_error(open, "unterminated interpolation: missing '}' (write \\{ for a literal brace)");
                return INTERP_FATAL;
            }
            if (r == STR_ERROR) {
                /* Stopped at a line end: the nested string already reported why. */
                if (at_end(l) || peek(l, 0) == '\n') return INTERP_FATAL;
                ok = false;
            }
        } else if (c == '\'') {
            /* A char literal like '}' must not end the interpolation. */
            advance(l);
            if (peek(l, 0) == '\\') {
                advance(l);
                if (lex_escape(l) < 0) ok = false;
            } else if (!at_end(l) && peek(l, 0) != '\n') {
                advance(l);
            }
            if (peek(l, 0) == '\'') advance(l);
        } else {
            advance(l);
        }
    }
}

/* Lexes the rest of a string after its opening quote. */
static StrResult lex_string_body(Lexer *l, Loc open, uint32_t *flags, bool single_line) {
    bool ok = true;
    for (;;) {
        if (at_end(l)) {
            if (single_line) return STR_NEWLINE;
            diag_error(open, "unterminated string literal");
            return STR_ERROR;
        }
        char c = peek(l, 0);
        if (c == '\n' && single_line) {
            return STR_NEWLINE;
        } else if (c == '"') {
            advance(l);
            return ok ? STR_OK : STR_ERROR;
        } else if (c == '\\') {
            advance(l);
            if (lex_escape(l) < 0) ok = false;
        } else if (c == '{') {
            /* A broken interpolation ends the string token here; lexing
             * resumes on the same line instead of hunting for a far-off quote. */
            InterpResult r = lex_interp(l, flags);
            if (r == INTERP_FATAL) return STR_ERROR;
            if (r == INTERP_RECOVERED) ok = false;
        } else {
            advance(l);
        }
    }
}

static Token lex_string(Lexer *l) {
    Loc start = here(l);
    Token t = {TOK_STRING, 0, start, l->src + l->pos, {0}};
    Loc open = start;
    open.len = 1;
    advance(l); /* " */
    StrResult r = lex_string_body(l, open, &t.flags, false);
    t.loc = span_from(l, start);
    if (r != STR_OK) t.kind = TOK_ERROR;
    return t;
}

static Token lex_char(Lexer *l) {
    Loc start = here(l);
    Token t = {TOK_CHAR, 0, start, l->src + l->pos, {0}};
    advance(l); /* ' */

    int value = -1;
    char c = peek(l, 0);
    if (at_end(l) || c == '\n') {
        t.loc = span_from(l, start);
        diag_error(t.loc, "unterminated char literal");
        t.kind = TOK_ERROR;
        return t;
    }
    if (c == '\'') {
        advance(l);
        t.loc = span_from(l, start);
        diag_error(t.loc, "empty char literal");
        t.kind = TOK_ERROR;
        return t;
    }
    if (c == '\\') {
        advance(l);
        value = lex_escape(l);
    } else {
        value = (unsigned char)advance(l);
    }

    if (peek(l, 0) != '\'') {
        /* Find the closing quote on this line so the error covers the whole literal. */
        while (!at_end(l) && peek(l, 0) != '\'' && peek(l, 0) != '\n') advance(l);
        bool closed = peek(l, 0) == '\'';
        if (closed) advance(l);
        t.loc = span_from(l, start);
        diag_error(t.loc, closed ? "char literal must be a single byte (use a string for more)"
                                 : "unterminated char literal");
        t.kind = TOK_ERROR;
        return t;
    }
    advance(l);
    t.loc = span_from(l, start);
    if (value < 0) {
        t.kind = TOK_ERROR;
        return t;
    }
    t.v.int_val = (uint64_t)value;
    return t;
}

Token lexer_next(Lexer *l) {
    skip_trivia(l);

    Loc start = here(l);
    Token t = {TOK_EOF, 0, start, l->src + l->pos, {0}};
    if (at_end(l)) return t;

    char c = peek(l, 0);

    if (is_ident_start(c)) {
        while (is_ident_char(peek(l, 0))) advance(l);
        if ((unsigned char)peek(l, 0) >= 0x80) {
            while ((unsigned char)peek(l, 0) >= 0x80 || is_ident_char(peek(l, 0))) advance(l);
            t.kind = TOK_ERROR;
            t.loc = span_from(l, start);
            diag_error(t.loc, "identifiers must be ASCII (non-ASCII is fine in strings and comments)");
            return t;
        }
        t.loc = span_from(l, start);
        t.kind = keyword_or_ident(t.start, t.loc.len);
        return t;
    }
    if (is_digit(c)) return lex_number(l);
    if (c == '"') return lex_string(l);
    if (c == '\'') return lex_char(l);

    for (size_t i = 0; i < sizeof puncts / sizeof puncts[0]; i++) {
        size_t n = strlen(puncts[i].text);
        if (l->pos + n <= l->len && memcmp(l->src + l->pos, puncts[i].text, n) == 0) {
            for (size_t j = 0; j < n; j++) advance(l);
            t.kind = puncts[i].kind;
            t.loc = span_from(l, start);
            return t;
        }
    }

    advance(l);
    t.kind = TOK_ERROR;
    t.loc = span_from(l, start);
    if (c == '$' && is_ident_start(peek(l, 0))) {
        const char *name = l->src + l->pos;
        size_t n = 0;
        while (is_ident_char(name[n])) n++;
        diag_error(t.loc, "watrlang variables have no '$' sigil: write '%.*s', not '$%.*s'",
                   (int)n, name, (int)n, name);
    } else if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7f) {
        diag_error(t.loc, "unexpected character '%c'", c);
    } else if ((unsigned char)c >= 0x80) {
        /* One error for the whole word (e.g. `été`), not one per byte. */
        while ((unsigned char)peek(l, 0) >= 0x80 || is_ident_char(peek(l, 0))) advance(l);
        t.loc = span_from(l, start);
        diag_error(t.loc, "identifiers must be ASCII (non-ASCII is fine in strings and comments)");
    } else {
        diag_error(t.loc, "unexpected byte 0x%02X", (unsigned char)c);
    }
    return t;
}
