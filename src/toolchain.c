#include "toolchain.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "diag.h"

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
static bool run(char *const argv[], bool verbose) {
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
                strcmp(argv[0], "as") == 0 || strcmp(argv[0], "ld") == 0 ? "GNU binutils" : "a C compiler");
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
    if (strcmp(argv[0], "as") == 0)
        fprintf(stderr, "error: the assembler rejected watrc's output; this is a compiler bug, please report it\n");
    else
        fprintf(stderr, "error: linking failed (see the messages above)\n");
    return false;
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

bool toolchain_build_executable(const char *asm_text, size_t asm_len, const char *out_path, bool verbose,
                                bool libc, const char *const *extra, size_t nextra) {
    char *asm_path = make_temp();
    char *obj_path = asm_path ? make_temp() : NULL;
    bool ok = asm_path && obj_path && write_file(asm_path, asm_text, asm_len);

    if (ok) {
        char *as_argv[] = {"as", "--64", "-o", obj_path, asm_path, NULL};
        ok = run(as_argv, verbose);
    }
    if (ok && !libc) {
        char *ld_argv[] = {"ld", "-o", (char *)out_path, obj_path, NULL};
        ok = run(ld_argv, verbose);
    } else if (ok) {
        const char *cc = getenv("CC");
        if (!cc || !cc[0]) cc = "cc";
        char **argv = xmalloc((nextra + 8) * sizeof *argv);
        size_t n = 0;
        argv[n++] = (char *)cc;
        argv[n++] = "-o";
        argv[n++] = (char *)out_path;
        argv[n++] = obj_path;
        for (size_t i = 0; i < nextra; i++) argv[n++] = (char *)extra[i];
        argv[n++] = "-lm";
        argv[n] = NULL;
        ok = run(argv, verbose);
        free(argv);
    }

    if (asm_path) unlink(asm_path);
    if (obj_path) unlink(obj_path);
    free(asm_path);
    free(obj_path);
    return ok;
}
