#ifndef WATR_LEXER_H
#define WATR_LEXER_H

#include <stdbool.h>
#include <stdint.h>

#include "diag.h"

/* X(name, spelling): every non-keyword token kind. */
#define WATR_TOKENS(X)          \
    X(EOF,       "end of file") \
    X(ERROR,     "invalid token") \
    X(IDENT,     "identifier")  \
    X(INT,       "integer literal") \
    X(FLOAT,     "float literal") \
    X(STRING,    "string literal") \
    X(CHAR,      "char literal") \
    /* punctuation */           \
    X(LPAREN,    "(")           \
    X(RPAREN,    ")")           \
    X(LBRACE,    "{")           \
    X(RBRACE,    "}")           \
    X(LBRACKET,  "[")           \
    X(RBRACKET,  "]")           \
    X(COMMA,     ",")           \
    X(SEMI,      ";")           \
    X(COLON,     ":")           \
    X(COLONCOLON, "::")         \
    X(QUESTION,  "?")           \
    X(ARROW,     "->")          \
    X(FATARROW,  "=>")          \
    X(ELLIPSIS,  "...")         \
    /* operators */             \
    X(PLUS,      "+")           \
    X(MINUS,     "-")           \
    X(STAR,      "*")           \
    X(SLASH,     "/")           \
    X(PERCENT,   "%")           \
    X(DOT,       ".")           \
    X(AMP,       "&")           \
    X(PIPE,      "|")           \
    X(CARET,     "^")           \
    X(TILDE,     "~")           \
    X(BANG,      "!")           \
    X(SHL,       "<<")          \
    X(SHR,       ">>")          \
    X(ANDAND,    "&&")          \
    X(OROR,      "||")          \
    X(PLUSPLUS,  "++")          \
    X(MINUSMINUS, "--")         \
    X(EQ,        "==")          \
    X(NE,        "!=")          \
    X(LT,        "<")           \
    X(GT,        ">")           \
    X(LE,        "<=")          \
    X(GE,        ">=")          \
    /* assignment */            \
    X(ASSIGN,    "=")           \
    X(PLUS_ASSIGN,    "+=")     \
    X(MINUS_ASSIGN,   "-=")     \
    X(STAR_ASSIGN,    "*=")     \
    X(SLASH_ASSIGN,   "/=")     \
    X(PERCENT_ASSIGN, "%=")     \
    X(DOT_ASSIGN,     ".=")     \
    X(AMP_ASSIGN,     "&=")     \
    X(PIPE_ASSIGN,    "|=")     \
    X(CARET_ASSIGN,   "^=")     \
    X(SHL_ASSIGN,     "<<=")    \
    X(SHR_ASSIGN,     ">>=")

/* X(name, spelling): reserved words. Built-in type names are keywords too,
 * so `int x = 5;` is recognizably a declaration without a symbol table. */
#define WATR_KEYWORDS(X)        \
    X(FUNCTION,  "function")    \
    X(RETURN,    "return")      \
    X(IF,        "if")          \
    X(ELSE,      "else")        \
    X(WHILE,     "while")       \
    X(DO,        "do")          \
    X(FOR,       "for")         \
    X(FOREACH,   "foreach")     \
    X(AS,        "as")          \
    X(BREAK,     "break")       \
    X(CONTINUE,  "continue")    \
    X(STRUCT,    "struct")      \
    X(CLASS,     "class")       \
    X(ENUM,      "enum")        \
    X(PUBLIC,    "public")      \
    X(PRIVATE,   "private")     \
    X(IMPORT,    "import")      \
    X(EXTERN,    "extern")      \
    X(EXPORT,    "export")      \
    X(CONST,     "const")       \
    X(VOLATILE,  "volatile")    \
    X(DEFER,     "defer")       \
    X(NEW,       "new")         \
    X(ALLOC,     "alloc")       \
    X(SIZEOF,    "sizeof")      \
    X(ECHO,      "echo")        \
    X(ASM,       "asm")         \
    X(THIS,      "this")        \
    X(TRUE,      "true")        \
    X(FALSE,     "false")       \
    X(NULL,      "null")        \
    /* built-in types */        \
    X(VOID,      "void")        \
    X(BOOL,      "bool")        \
    X(INT_T,     "int")         \
    X(FLOAT_T,   "float")       \
    X(STRING_T,  "string")      \
    X(ARRAY_T,   "array")       \
    X(I8,        "i8")          \
    X(I16,       "i16")         \
    X(I32,       "i32")         \
    X(I64,       "i64")         \
    X(U8,        "u8")          \
    X(U16,       "u16")         \
    X(U32,       "u32")         \
    X(U64,       "u64")         \
    X(F32,       "f32")         \
    X(F64,       "f64")         \
    X(USIZE,     "usize")

typedef enum {
#define X(name, spelling) TOK_##name,
    WATR_TOKENS(X)
#undef X
#define X(name, spelling) TOK_KW_##name,
    WATR_KEYWORDS(X)
#undef X
    TOK_COUNT
} TokenKind;

/* Set on string tokens that contain {interpolation}. */
#define TOKF_INTERP 0x1

typedef struct {
    TokenKind kind;
    uint32_t flags;
    Loc loc;            /* loc.len is the token's length in bytes */
    const char *start;  /* points into the source text; not NUL-terminated */
    union {
        uint64_t int_val;   /* TOK_INT, TOK_CHAR */
        double float_val;   /* TOK_FLOAT */
    } v;
} Token;

typedef struct {
    const SrcFile *file;
    const char *src;
    size_t pos;
    size_t len;
    uint32_t line;
    uint32_t col;
} Lexer;

void  lexer_init(Lexer *l, const SrcFile *file);
Token lexer_next(Lexer *l);

/* "IDENT", "KW_RETURN", "ARROW"... (for --tokens dumps) */
const char *token_kind_name(TokenKind k);
/* "identifier", "return", "->"... (for error messages) */
const char *token_kind_spelling(TokenKind k);

#endif
