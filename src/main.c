#include <stdio.h>
#include <string.h>

#include "arena.h"
#include "diag.h"
#include "lexer.h"
#include "parser.h"

#define WATRC_VERSION "0.0.1"

static void usage(FILE *out) {
    fputs("usage: watrc [options] file.watr\n"
          "\n"
          "options:\n"
          "  --tokens     print the token stream and exit\n"
          "  --ast        print the syntax tree and exit\n"
          "  --version    print the compiler version\n"
          "  -h, --help   show this help\n",
          out);
}

static void dump_token(const Token *t) {
    printf("%u:%u %s", t->loc.line, t->loc.col, token_kind_name(t->kind));
    if (t->kind == TOK_EOF) {
        putchar('\n');
        return;
    }
    /* Escape newlines etc. so multi-line strings stay on one dump line. */
    putchar(' ');
    for (uint32_t i = 0; i < t->loc.len; i++) {
        unsigned char c = (unsigned char)t->start[i];
        if (c == '\n') fputs("\\n", stdout);
        else if (c == '\t') fputs("\\t", stdout);
        else if (c == '\r') fputs("\\r", stdout);
        else putchar(c);
    }
    switch (t->kind) {
    case TOK_INT:
    case TOK_CHAR:
        printf(" = %llu", (unsigned long long)t->v.int_val);
        break;
    case TOK_FLOAT:
        printf(" = %.17g", t->v.float_val);
        break;
    case TOK_STRING:
        if (t->flags & TOKF_INTERP) fputs(" (interp)", stdout);
        break;
    default:
        break;
    }
    putchar('\n');
}

int main(int argc, char **argv) {
    const char *path = NULL;
    int dump_tokens = 0;
    int dump_ast = 0;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--version") == 0) {
            printf("watrc %s\n", WATRC_VERSION);
            return 0;
        } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(arg, "--tokens") == 0) {
            dump_tokens = 1;
        } else if (strcmp(arg, "--ast") == 0) {
            dump_ast = 1;
        } else if (arg[0] == '-' && arg[1] != '\0') {
            fprintf(stderr, "watrc: unknown option '%s'\n", arg);
            usage(stderr);
            return 2;
        } else if (path) {
            fprintf(stderr, "watrc: only one input file is supported for now\n");
            return 2;
        } else {
            path = arg;
        }
    }

    if (!path) {
        usage(stderr);
        return 2;
    }

    /* Keep token dumps and error messages in order when both go to a pipe. */
    if (dump_tokens || dump_ast) setvbuf(stdout, NULL, _IOLBF, 0);

    Arena arena = {0};
    SrcFile *file = src_load(&arena, path);
    if (!file) return 2;

    if (dump_tokens) {
        Lexer lexer;
        lexer_init(&lexer, file);
        for (;;) {
            Token t = lexer_next(&lexer);
            dump_token(&t);
            if (t.kind == TOK_EOF) break;
        }
    } else {
        /* The tree is printed even after errors, to show how the parser recovered. */
        Module *m = parse_file(&arena, file);
        if (dump_ast) ast_print_module(stdout, m);
    }

    int errors = diag_error_count();
    if (errors) {
        fprintf(stderr, "%d error%s\n", errors, errors == 1 ? "" : "s");
    } else if (!dump_tokens && !dump_ast) {
        fprintf(stderr, "watrc: parsing succeeded; type checking and code generation are not implemented yet\n");
    }
    fflush(stdout);
    arena_free(&arena);
    return errors ? 1 : 0;
}
