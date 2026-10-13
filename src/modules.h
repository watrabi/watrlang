#ifndef WATR_MODULES_H
#define WATR_MODULES_H

#include "arena.h"
#include "ast.h"

/*
 * Loads a program: the main file, everything it imports (transitively,
 * each file once), and the runtime prelude last. Module 0 is the main
 * file. Import paths are relative to the importing file, except paths
 * starting with "std/", which come from the standard library directory
 * ($WATR_STD, or std/ next to the compiler).
 */
typedef struct {
    Module **mods;
    size_t n;
} ModuleSet;

/* Returns false if any file couldn't be read or parsed (errors reported). */
bool load_program(Arena *arena, const char *root_path, ModuleSet *out);

/* The parsed main file alone, for --ast. */
Module *load_root_only(Arena *arena, const char *path);

#endif
