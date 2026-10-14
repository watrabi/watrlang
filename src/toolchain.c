#include "toolchain.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "diag.h"
#include "target.h"

bool write_file(const char *path, const char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "watrc: error: cannot write '%s': %s\n", path, strerror(errno));
        return false;
    }
    bool ok = fwrite(data, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    if (!ok) fprintf(stderr, "watrc: error: failed writing '%s'\n", path);
    return ok;
}

/* Runs argv[0] with arguments and waits. No shell is involved, so paths
 * with spaces or quotes are passed through untouched. */
static bool run_tool(char *const argv[], bool verbose, bool assembling) {
    if (verbose) {
        for (int i = 0; argv[i]; i++) fprintf(stderr, "%s%s", i ? " " : "", argv[i]);
        fputc('\n', stderr);
    }
    fflush(NULL);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "watrc: error: fork failed: %s\n", strerror(errno));
        return false;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        /* Only reached if exec failed; 127 is the shell convention for "not found". */
        fprintf(stderr, "error: cannot run `%s`: %s (is %s installed?)\n", argv[0], strerror(errno),
                strstr(argv[0], "mingw") ? "mingw-w64 (set WATR_WINDOWS_CC to use another compiler)"
                : strcmp(argv[0], "as") == 0 || strcmp(argv[0], "ld") == 0 ? "GNU binutils" : "a C compiler");
        _exit(127);
    }

    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "watrc: error: waitpid failed: %s\n", strerror(errno));
            return false;
        }
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) return false;  /* already explained */
    if (assembling)
        fprintf(stderr, "error: the assembler rejected watrc's output; this is a compiler bug, please report it\n");
    else
        fprintf(stderr, "error: linking failed (see the messages above)\n");
    return false;
}

static bool run(char *const argv[], bool verbose) {
    return run_tool(argv, verbose, strcmp(argv[0], "as") == 0);
}

/* Creates an empty temp file and returns its malloc'd path. */
static char *make_temp(void) {
    const char *dir = getenv("TMPDIR");
    if (!dir || !dir[0]) dir = "/tmp";
    size_t n = strlen(dir) + sizeof "/watrc-XXXXXX";
    char *path = xmalloc(n);
    snprintf(path, n, "%s/watrc-XXXXXX", dir);
    int fd = mkstemp(path);
    if (fd < 0) {
        fprintf(stderr, "watrc: error: cannot create a temp file in %s: %s\n", dir, strerror(errno));
        free(path);
        return NULL;
    }
    close(fd);
    return path;
}

static bool has_suffix(const char *s, const char *suffix) {
    size_t n = strlen(s), k = strlen(suffix);
    return n > k && strcmp(s + n - k, suffix) == 0;
}

static const char *c_compiler(void) {
    const char *cc = getenv("CC");
    return cc && cc[0] ? cc : "cc";
}

/* The C compiler that targets Windows: mingw-w64's gcc, which also
 * assembles (GNU as for COFF) and links against msvcrt. */
static const char *windows_cc(void) {
    const char *cc = getenv("WATR_WINDOWS_CC");
    return cc && cc[0] ? cc : "x86_64-w64-mingw32-gcc";
}

/* Windows: always through the C compiler, with the C runtime. */
static bool build_windows(const char *asm_path, const char *obj_path, const char *out_path, const BuildOptions *o) {
    const char *cc = windows_cc();
    char *as_argv[] = {(char *)cc, "-c", "-x", "assembler", "-o", (char *)(o->mode == BUILD_OBJECT ? out_path : obj_path),
                       (char *)asm_path, NULL};
    if (!run_tool(as_argv, o->verbose, true)) return false;
    if (o->mode == BUILD_OBJECT) return true;

    char **argv = xmalloc((o->nextra + 12) * sizeof *argv);
    size_t n = 0;
    argv[n++] = (char *)cc;
    argv[n++] = "-o";
    argv[n++] = (char *)out_path;
    argv[n++] = (char *)obj_path;
    for (size_t i = 0; i < o->nextra; i++) argv[n++] = (char *)o->extra[i];
    /* An 8 MiB stack, like Linux's default, committed up front: watrlang
     * functions don't probe the stack page by page, which a stack that
     * Windows grows on demand would need for frames over 4 KiB. */
    argv[n++] = "-Xlinker";
    argv[n++] = "--stack";
    argv[n++] = "-Xlinker";
    argv[n++] = "8388608,8388608";
    argv[n] = NULL;
    bool ok = run_tool(argv, o->verbose, false);
    free(argv);
    return ok;
}

bool toolchain_build(const char *asm_text, size_t asm_len, const char *out_path, const BuildOptions *o) {
    if (target_is_windows()) {
        char *asm_path = make_temp();
        char *obj_path = asm_path ? make_temp() : NULL;
        bool ok = asm_path && obj_path && write_file(asm_path, asm_text, asm_len) &&
                  build_windows(asm_path, obj_path, out_path, o);
        if (asm_path) unlink(asm_path);
        if (obj_path) unlink(obj_path);
        free(asm_path);
        free(obj_path);
        return ok;
    }
    char *asm_path = make_temp();
    char *obj_path = asm_path ? make_temp() : NULL;
    bool ok = asm_path && obj_path && write_file(asm_path, asm_text, asm_len);
    char **temps = xmalloc((o->nextra + 1) * sizeof *temps);   /* objects made from extra .s/.c files */
    size_t ntemps = 0;

    if (ok) {
        char *as_argv[] = {"as", "--64", "-o", (char *)(o->mode == BUILD_OBJECT ? out_path : obj_path), asm_path, NULL};
        ok = run(as_argv, o->verbose);
    }
    if (ok && o->mode == BUILD_STATIC) {
        char *ld_argv[] = {"ld", "-o", (char *)out_path, obj_path, NULL};
        ok = run(ld_argv, o->verbose);
    } else if (ok && o->mode == BUILD_FREESTANDING) {
        char **argv = xmalloc((o->nextra + 16) * sizeof *argv);
        size_t n = 0;
        argv[n++] = "ld";
        argv[n++] = "-static";
        argv[n++] = "-nostdlib";
        argv[n++] = "-z";
        argv[n++] = "noexecstack";
        if (o->linker_script) {
            argv[n++] = "-T";
            argv[n++] = (char *)o->linker_script;
        }
        if (o->entry) {
            argv[n++] = "-e";
            argv[n++] = (char *)o->entry;
        }
        argv[n++] = "-o";
        argv[n++] = (char *)out_path;
        argv[n++] = obj_path;
        for (size_t i = 0; ok && i < o->nextra; i++) {
            const char *in = o->extra[i];
            if (has_suffix(in, ".s") || has_suffix(in, ".S") || has_suffix(in, ".c")) {
                char *tmp = make_temp();
                if (!tmp) {
                    ok = false;
                    break;
                }
                temps[ntemps++] = tmp;
                if (has_suffix(in, ".c")) {   /* C for a kernel: no libc, no red zone, no PIC */
                    char *cc_argv[] = {(char *)c_compiler(), "-c", "-O2", "-ffreestanding", "-fno-pic", "-mno-red-zone",
                                       "-fno-stack-protector", "-o", tmp, (char *)in, NULL};
                    ok = run(cc_argv, o->verbose);
                } else {
                    char *as_argv[] = {"as", "--64", "-o", tmp, (char *)in, NULL};
                    ok = run(as_argv, o->verbose);
                }
                argv[n++] = tmp;
            } else {
                argv[n++] = (char *)in;
            }
        }
        argv[n] = NULL;
        if (ok) ok = run(argv, o->verbose);
        free(argv);
    } else if (ok && o->mode == BUILD_LIBC) {
        char **argv = xmalloc((o->nextra + 8) * sizeof *argv);
        size_t n = 0;
        argv[n++] = (char *)c_compiler();
        argv[n++] = "-o";
        argv[n++] = (char *)out_path;
        argv[n++] = obj_path;
        for (size_t i = 0; i < o->nextra; i++) argv[n++] = (char *)o->extra[i];
        argv[n++] = "-lm";
        argv[n] = NULL;
        ok = run(argv, o->verbose);
        free(argv);
    }

    for (size_t i = 0; i < ntemps; i++) {
        unlink(temps[i]);
        free(temps[i]);
    }
    free(temps);
    if (asm_path) unlink(asm_path);
    if (obj_path) unlink(obj_path);
    free(asm_path);
    free(obj_path);
    return ok;
}
