#include <stdarg.h>

#include "noge.h"

struct Chunk {
    Chunk *next;
    size_t used, cap;
    char data[];
};

Arena g_arena;
const char *g_file = "";

void *arena_alloc(Arena *a, size_t n) {
    n = (n + 7) & ~(size_t)7;
    Chunk *c = a->head;
    if (!c || c->cap - c->used < n) {
        size_t cap = n > 65536 ? n : 65536;
        c = malloc(sizeof(Chunk) + cap);
        if (!c) {
            fprintf(stderr, "noge: out of memory\n");
            exit(1);
        }
        c->next = a->head;
        c->used = 0;
        c->cap = cap;
        a->head = c;
    }
    void *p = c->data + c->used;
    c->used += n;
    memset(p, 0, n);
    return p;
}

char *arena_strn(Arena *a, const char *s, int n) {
    char *d = arena_alloc(a, (size_t)n + 1);
    memcpy(d, s, (size_t)n);
    return d;
}

/* Notes name the generic instances being built or checked, so an error inside one says which and where it was
 * asked for. They are printed, innermost first, after the error. */
#define MAX_NOTES 64
static char *notes[MAX_NOTES];
static int nnotes;

void note_push(const char *fmt, ...) {
    char buf[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (nnotes < MAX_NOTES) notes[nnotes] = arena_strn(&g_arena, buf, (int)strlen(buf));
    nnotes++; /* beyond the limit the note is dropped, but the count stays right for note_pop */
}

const char *note_name(const char *name) {
    if (strlen(name) <= 90) return name;
    char *s = arena_strn(&g_arena, name, 90);
    memcpy(s + 87, "...", 4);
    return s;
}

void note_pop(void) {
    if (nnotes > 0) nnotes--;
}

void die(int line, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s:%d: error: ", g_file, line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    for (int i = (nnotes < MAX_NOTES ? nnotes : MAX_NOTES) - 1; i >= 0; i--) fprintf(stderr, "  note: %s\n", notes[i]);
    exit(1);
}

char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "noge: cannot open '%s'\n", path);
        exit(1);
    }
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    size_t got;
    while (buf && (got = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += got;
        if (cap - len < 2) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
    }
    fclose(f);
    if (!buf) {
        fprintf(stderr, "noge: out of memory\n");
        exit(1);
    }
    buf[len] = 0;
    return buf;
}
