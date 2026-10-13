#ifndef WATR_CODEGEN_H
#define WATR_CODEGEN_H

#include <stdio.h>

#include "ast.h"
#include "sema.h"

/* Writes x86-64 assembly (GNU as, Intel syntax) for a module that sema
 * has checked without errors. */
void codegen_x64(const Module *m, const Program *prog, FILE *out);

#endif
