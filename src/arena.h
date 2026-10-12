#ifndef WATR_ARENA_H
#define WATR_ARENA_H

#include <stddef.h>

/*
 * Bump allocator. The compiler allocates everything (tokens, AST, types)
 * out of arenas and frees them all at once, so nothing calls free() on
 * individual objects.
 */

typedef struct ArenaChunk ArenaChunk;

typedef struct {
    ArenaChunk *head;
} Arena;

/* Returns zeroed memory aligned to 16 bytes. Never returns NULL. */
void *arena_alloc(Arena *a, size_t size);
char *arena_strndup(Arena *a, const char *s, size_t n);
void  arena_free(Arena *a);

#define ARENA_NEW(a, T) ((T *)arena_alloc((a), sizeof(T)))
#define ARENA_ARRAY(a, T, n) ((T *)arena_alloc((a), sizeof(T) * (n)))

#endif
