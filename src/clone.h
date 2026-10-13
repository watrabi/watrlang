#ifndef WATR_CLONE_H
#define WATR_CLONE_H

#include "ast.h"

/* A deep copy of generic declaration `tmpl` (functions, structs, classes)
 * with its type parameters replaced by `types` (tmpl->ntparams of them).
 * The copy has no type parameters. */
Decl *clone_instance(Arena *arena, const Decl *tmpl, struct Type **types);

#endif
