#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "diag.h"
#include "lexer.h"
#include "parser.h"
#include "modules.h"
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
          "  -c           write an object file (.o) instead of an executable\n"
          "  -O0, -O1     optimization: -O1 (the default) allocates registers;\n"
          "               -O0 is a simple stack machine, for comparison\n"
          "  --freestanding  no C library, no runtime heap, no entry point: for\n"
          "               kernels and other bare-metal code (see docs/DESIGN.md)\n"
          "  -T <script>  link with this linker script (with --freestanding)\n"
          "  -e <symbol>  the entry point (with --freestanding; default _start)\n"
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
    int emit_obj = 0;
    int verbose = 0;
    bool freestanding = false;
    int opt = 1;
    const char *opt_env = getenv("WATR_OPT");   /* default level, for testing both backends */
    if (opt_env && (opt_env[0] == '0' || opt_env[0] == '1') && !opt_env[1]) opt = opt_env[0] - '0';
    const char *linker_script = NULL, *entry = NULL;
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
        } else if (strcmp(arg, "-c") == 0) {
            emit_obj = 1;
        } else if (strcmp(arg, "-O0") == 0 || strcmp(arg, "-O1") == 0 || strcmp(arg, "-O") == 0) {
            opt = arg[2] == '0' ? 0 : 1;
        } else if (strcmp(arg, "--freestanding") == 0) {
            freestanding = true;
        } else if (strcmp(arg, "-T") == 0 || strcmp(arg, "-e") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: `%s` needs %s\n", arg, arg[1] == 'T' ? "a linker script" : "a symbol name");
                return 2;
            }
            if (arg[1] == 'T') linker_script = argv[++i];
            else entry = argv[++i];
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
        } else if (has_suffix(arg, ".c") || has_suffix(arg, ".o") || has_suffix(arg, ".a") || has_suffix(arg, ".so") ||
                   has_suffix(arg, ".s") || has_suffix(arg, ".S")) {
            link_inputs[nlink++] = arg;
        } else if (path) {
            fprintf(stderr, "error: give watrc one .watr file; it finds the others through `import`\n");
            return 2;
        } else {
            path = arg;
        }
    }

    if (!path) {
        usage(stderr);
        return 2;
    }
    if ((linker_script || entry) && !freestanding) {
        fprintf(stderr, "error: `-T` and `-e` only apply with `--freestanding`\n");
        return 2;
    }

    /* Keep token dumps and error messages in order when both go to a pipe. */
    if (dump_tokens || dump_ast) setvbuf(stdout, NULL, _IOLBF, 0);

    types_init();
    Arena arena = {0};
    SrcFile *file = src_load(&arena, path);
    if (!file) return 2;

    Module *m_root = NULL;
    if (dump_tokens) {
        Lexer lexer;
        lexer_init(&lexer, file);
        for (;;) {
            Token t = lexer_next(&lexer);
            dump_token(&t);
            if (t.kind == TOK_EOF) break;
        }
    } else {
        Program prog = {0};
        prog.freestanding = freestanding;
        prog.opt = opt;
        bool ok;
        if (dump_ast) {
            /* The tree is printed even after errors, to show how the parser recovered. */
            Module *m = parse_file(&arena, file);
            ast_print_module(stdout, m);
            ok = false;
        } else {
            ModuleSet set;
            ok = load_program(&arena, path, &set) && sema_check(&arena, set.mods, set.n, &prog);
            m_root = ok ? set.mods[0] : NULL;
        }
        if (ok && getenv("WATR_GC_STRESS") && strcmp(getenv("WATR_GC_STRESS"), "0") != 0) {
            /* Testing aid: the program collects garbage at every allocation. */
            for (size_t i = 0; i < prog.nglobals; i++) {
                Symbol *g = prog.globals[i];
                if (g->is_runtime && strcmp(g->name, "__rt_gc_stress") == 0) {
                    g->init = arena_alloc(&arena, 1);
                    g->init[0] = 1;
                }
            }
        }
        if (ok) {
            char *asm_text = NULL;
            size_t asm_len = 0;
            FILE *asm_out = open_memstream(&asm_text, &asm_len);
            if (!asm_out) fatal("open_memstream failed");
            codegen_x64(m_root, &prog, asm_out);
            fclose(asm_out);

            char *def = default_output(path, emit_asm ? ".s" : emit_obj ? ".o" : "");
            const char *dest = out_path ? out_path : def;
            BuildOptions opts = {0};
            opts.mode = emit_obj ? BUILD_OBJECT
                        : freestanding ? BUILD_FREESTANDING
                        : prog.needs_libc || nlink > 0 ? BUILD_LIBC
                                                        : BUILD_STATIC;
            opts.verbose = verbose;
            opts.linker_script = linker_script;
            opts.entry = entry;
            opts.extra = link_inputs;
            opts.nextra = nlink;
            bool built = emit_asm ? write_file(dest, asm_text, asm_len) : toolchain_build(asm_text, asm_len, dest, &opts);
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
