#include <stdbool.h>
#include <stdint.h>

struct pt { int32_t x, y; };

int32_t noge_triple(int32_t v);   /* exported by the Noge program */
void noge_bump(int32_t *p);

int32_t c_double(int32_t v) { return v * 2; }
int32_t c_call_back(int32_t v) { return noge_triple(v) + 1; }
int32_t c_call_back_ptr(void) { int32_t v = 40; noge_bump(&v); noge_bump(&v); return v; }
void c_fill(uint8_t *buf, int32_t n) { for (int32_t i = 0; i < n; i++) buf[i] = (uint8_t)(i * 3); }
int32_t c_sum(const uint8_t *buf, int32_t n) { int32_t s = 0; for (int32_t i = 0; i < n; i++) s += buf[i]; return s; }
void c_get_pair(int32_t *a, int32_t *b) { *a = 11; *b = 22; }
void c_move(struct pt *p, int32_t dx, int32_t dy) { p->x += dx; p->y += dy; }
int32_t c_dist(const struct pt *p) { return p->x * p->x + p->y * p->y; }
struct pt c_make_pt(int32_t x, int32_t y) { struct pt p = {x, y}; return p; }
struct pt c_flip(struct pt p) { struct pt q = {p.y, p.x}; return q; }
bool c_is_even(int32_t v) { return (v & 1) == 0; }
const uint8_t *c_message(void) { return (const uint8_t *)"hi from C"; }
