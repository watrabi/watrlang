#include <stdio.h>
#include <string.h>

#include "arena.h"
#include "diag.h"
#include "lexer.h"
#include "parser.h"
#include "prelude.h"
#include "codegen.h"
#include "sema.h"
#include "types.h"
#include "toolchain.h"

#define WATRC_VERSION "0.0.1"

static void usage(FILE *out) {
    fputs("usage: watrc [options] file.watr [file.c file.o lib.a ...]\n"
          "\n"
          "Compiles file.watr into a native executable (file, without .watr).\n"
          "Extra C sources, objects and libraries are linked in.\n"
          "\n"
          "options:\n"
          "  -o <path>    write the output here\n"
          "  -S           write assembly (.s) instead of an executable\n"
          "  -v           show the assembler and linker commands\n"
          "  --explain <code>  explain an error code, e.g. --explain E0200\n"
          "  --tokens     print the token stream and exit\n"
          "  --ast        print the syntax tree and exit\n"
          "  --version    print the compiler version\n"
          "  -h, --help   show this help\n",
          out);
}

/* "dir/hello.watr" -> "dir/hello" + suffix */
static char *default_output(const char *path, const char *suffix) {
    size_t n = strlen(path);
    if (n > 5 && strcmp(path + n - 5, ".watr") == 0) n -= 5;
    else if (!suffix[0]) suffix = ".out";   /* don't overwrite the source file */
    char *out = xmalloc(n + strlen(suffix) + 1);
    memcpy(out, path, n);
    strcpy(out + n, suffix);
    return out;
}

static bool has_suffix(const char *s, const char *suffix) {
    size_t n = strlen(s), k = strlen(suffix);
    return n > k && strcmp(s + n - k, suffix) == 0;
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
    int emit_asm = 0;
    int verbose = 0;
    const char *out_path = NULL;
    const char *link_inputs[argc > 0 ? argc : 1];   /* extra .c/.o/.a files to link */
    size_t nlink = 0;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--version") == 0) {
            printf("watrc %s\n", WATRC_VERSION);
            return 0;
        } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(arg, "--explain") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: `--explain` needs an error code, like `--explain E0200`\n");
                return 2;
            }
            const char *code = argv[++i];
            const char *text = diag_explain(code);
            if (!text) {
                fprintf(stderr, "error: `%s` is not a watrlang error code\n", code);
                return 2;
            }
            printf("%s\n", text);
            return 0;
        } else if (strcmp(arg, "--tokens") == 0) {
            dump_tokens = 1;
        } else if (strcmp(arg, "--ast") == 0) {
            dump_ast = 1;
        } else if (strcmp(arg, "-S") == 0) {
            emit_asm = 1;
        } else if (strcmp(arg, "-v") == 0) {
            verbose = 1;
        } else if (strcmp(arg, "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: `-o` needs a path\n");
                return 2;
            }
            out_path = argv[++i];
        } else if (arg[0] == '-' && arg[1] != '\0') {
            fprintf(stderr, "error: unknown option `%s`\n\n", arg);
            usage(stderr);
            return 2;
        } else if (has_suffix(arg, ".c") || has_suffix(arg, ".o") || has_suffix(arg, ".a") || has_suffix(arg, ".so")) {
            link_inputs[nlink++] = arg;
        } else if (path) {
            fprintf(stderr, "error: only one .watr file is supported for now (imports are coming in milestone 8)\n");
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

    types_init();
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

        Program prog = {0};
        bool ok = !dump_ast && diag_error_count() == 0;
        if (ok) {
            /* The runtime prelude is compiled along with every program. */
            SrcFile *rt = ARENA_NEW(&arena, SrcFile);
            rt->path = WATR_PRELUDE_PATH;
            rt->text = watr_prelude_source;
            rt->len = strlen(watr_prelude_source);
            Module *rtm = parse_file(&arena, rt);
            Decl **all = ARENA_ARRAY(&arena, Decl *, m->n + rtm->n);
            memcpy(all, m->decls, m->n * sizeof *all);
            memcpy(all + m->n, rtm->decls, rtm->n * sizeof *all);
            m->decls = all;
            m->n += rtm->n;
            ok = diag_error_count() == 0 && sema_check(&arena, m, &prog);
        }
        if (ok) {
            char *asm_text = NULL;
            size_t asm_len = 0;
            FILE *asm_out = open_memstream(&asm_text, &asm_len);
            if (!asm_out) fatal("open_memstream failed");
            codegen_x64(m, &prog, asm_out);
            fclose(asm_out);

            char *def = default_output(path, emit_asm ? ".s" : "");
            const char *dest = out_path ? out_path : def;
            bool built = emit_asm ? write_file(dest, asm_text, asm_len)
                                  : toolchain_build_executable(asm_text, asm_len, dest, verbose,
                                                               prog.needs_libc || nlink > 0, link_inputs, nlink);
            free(def);
            free(asm_text);
            if (!built) {
                arena_free(&arena);
                types_free();
                return 2;
            }
        }
    }

    int errors = diag_error_count();
    diag_print_summary(path);
    fflush(stdout);
    arena_free(&arena);
    types_free();
    return errors ? 1 : 0;
}
