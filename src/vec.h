#ifndef WATR_VEC_H
#define WATR_VEC_H

#include <stddef.h>
#include <string.h>

#include "arena.h"
#include "diag.h"

/* Growable array of pointers, used while building AST lists. Call
 * ptrvec_finish to move the result into an arena and free the buffer. */
typedef struct {
    void **items;
    size_t len;
    size_t cap;
} PtrVec;

static inline void ptrvec_push(PtrVec *v, void *item) {
    if (v->len == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 8;
        void **items = xmalloc(cap * sizeof *items);
        if (v->len) memcpy(items, v->items, v->len * sizeof *items);
        free(v->items);
        v->items = items;
        v->cap = cap;
    }
    v->items[v->len++] = item;
}

/* Copies the items into the arena, frees the vector, returns the arena array. */
static inline void *ptrvec_finish(PtrVec *v, Arena *a, size_t *out_len) {
    void **out = NULL;
    if (v->len) {
        out = arena_alloc(a, v->len * sizeof *out);
        memcpy(out, v->items, v->len * sizeof *out);
    }
    *out_len = v->len;
    free(v->items);
    v->items = NULL;
    v->len = v->cap = 0;
    return out;
}

#endif
