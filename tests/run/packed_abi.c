#include <stdint.h>

struct __attribute__((packed)) Packed {
    uint8_t tag;
    uint32_t value;   /* offset 1 */
    uint16_t extra;
};

struct __attribute__((packed)) Aligned {   /* packed but every field is naturally aligned */
    uint32_t a;
    uint32_t b;
};

struct Big {
    uint64_t x;
} __attribute__((aligned(32)));

int64_t c_sum_packed(struct Packed p) { return p.tag + p.value + p.extra; }
struct Packed c_make_packed(uint8_t t, uint32_t v, uint16_t e) { struct Packed p = {t, v, e}; return p; }
int64_t c_sum_aligned(struct Aligned a) { return a.a * 10 + a.b; }
int c_size_packed(void) { return sizeof(struct Packed); }
