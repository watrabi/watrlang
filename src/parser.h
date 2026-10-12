#ifndef WATR_PARSER_H
#define WATR_PARSER_H

#include "arena.h"
#include "ast.h"
#include "diag.h"

/* Lexes and parses a whole file. Always returns a module; check
 * diag_error_count() to see whether it is complete. */
Module *parse_file(Arena *arena, const SrcFile *file);

#endif
