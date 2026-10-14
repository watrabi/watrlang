#ifndef WATR_TARGET_H
#define WATR_TARGET_H

#include <stdbool.h>

/*
 * The platform a program is compiled for. Every target is x86-64; they
 * differ in the operating system: how a process starts and ends, how it
 * talks to the OS (system calls or DLLs), the object file format, the C
 * calling convention, and which toolchain links it.
 *
 * watrlang code calls watrlang code with the System V convention on every
 * target. Only where it meets foreign code (extern and export functions,
 * the C runtime's `main`) does a target's own convention apply; on Windows
 * that boundary is crossed through small thunks (codegen_win64.c).
 */

typedef enum {
    OS_LINUX,
    OS_WINDOWS,
} TargetOs;

typedef struct {
    const char *name;        /* canonical: "x86_64-linux" */
    TargetOs os;
    const char *os_name;     /* "linux": std/<os_name>/ holds this OS's versions of std modules */
    const char *exe_suffix;  /* added to the default output name: "" or ".exe" */
} Target;

/* The target being compiled for (the host's, unless --target says otherwise). */
extern const Target *watr_target;

/* A target by name ("x86_64-windows", or just "windows"), or NULL. */
const Target *target_find(const char *name);

/* "x86_64-linux, x86_64-windows", for messages. */
const char *target_names(void);

static inline bool target_is_windows(void) { return watr_target->os == OS_WINDOWS; }

#endif
