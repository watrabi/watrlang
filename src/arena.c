#include "arena.h"
#include "diag.h"

#include <stdint.h>
#include <string.h>

#define ARENA_ALIGN      16
#define ARENA_CHUNK_SIZE (64 * 1024)

struct ArenaChunk {
    ArenaChunk *next;
    size_t used;
    size_t cap;
    unsigned char data[];
};

static ArenaChunk *chunk_new(size_t cap) {
    ArenaChunk *c = xmalloc(sizeof(ArenaChunk) + cap);
    c->next = NULL;
    c->used = 0;
    c->cap = cap;
    return c;
}

/* Offset into c->data where an aligned block would start. */
static size_t aligned_offset(const ArenaChunk *c) {
    uintptr_t p = (uintptr_t)(c->data + c->used);
    uintptr_t aligned = (p + (ARENA_ALIGN - 1)) & ~(uintptr_t)(ARENA_ALIGN - 1);
    return c->used + (size_t)(aligned - p);
}

void *arena_alloc(Arena *a, size_t size) {
    if (size == 0) size = 1;

    ArenaChunk *c = a->head;
    if (c) {
        size_t off = aligned_offset(c);
        if (off <= c->cap && size <= c->cap - off) {
            c->used = off + size;
            return memset(c->data + off, 0, size);
        }
    }

    /* Big requests get their own chunk so they don't waste a whole default one. */
    size_t cap = size + ARENA_ALIGN > ARENA_CHUNK_SIZE ? size + ARENA_ALIGN : ARENA_CHUNK_SIZE;
    c = chunk_new(cap);
    c->next = a->head;
    a->head = c;

    size_t off = aligned_offset(c);
    c->used = off + size;
    return memset(c->data + off, 0, size);
}

char *arena_strndup(Arena *a, const char *s, size_t n) {
    char *p = arena_alloc(a, n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void arena_free(Arena *a) {
    ArenaChunk *c = a->head;
    while (c) {
        ArenaChunk *next = c->next;
        free(c);
        c = next;
    }
    a->head = NULL;
}
