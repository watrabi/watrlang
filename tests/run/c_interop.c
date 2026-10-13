// C half of c_interop.watr: calls into watrlang and back.
#include <stdint.h>

typedef struct { float x, y; } Vec2;              // two floats: one SSE eightbyte
typedef struct { int64_t a; double b; } Mixed;     // INTEGER + SSE
typedef struct { int32_t v[5]; } Big;              // 20 bytes: passed in memory

int64_t c_counter = 100;                           // an extern variable for watrlang

Vec2 c_scale(Vec2 v, float k) { Vec2 r = {v.x * k, v.y * k}; return r; }
Mixed c_mix(int a, Mixed m, double d, int b) { Mixed r = {m.a + a + b, m.b * d}; return r; }
Big c_big(Big b, int add) { for (int i = 0; i < 5; i++) b.v[i] += add; return b; }

/* Exported from watrlang. */
int64_t w_triple(int64_t x);
Vec2 w_swap(Vec2 v);

int64_t c_calls_watr(void) {
    Vec2 s = w_swap((Vec2){1.0f, 2.0f});
    return w_triple(14) + (int64_t)(s.x * 10) + (int64_t)s.y;   // 42 + 20 + 1
}
