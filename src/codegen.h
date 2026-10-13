#ifndef WATR_CODEGEN_H
#define WATR_CODEGEN_H

#include <stdio.h>

#include "ast.h"

/* Checks the module and writes x86-64 assembly (GNU as, Intel syntax) to
 * `out`. Returns false if any errors were reported; `out` then holds
 * partial output and should be discarded. */
bool codegen_x64(const Module *m, FILE *out);

#endif
