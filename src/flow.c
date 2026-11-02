/* Flow analysis: definite initialization, moves, and exclusivity.
 *
 * Runs on the type-checked AST. Noge only has structured control flow
 * (if / while / return), so the analysis walks the tree directly instead of
 * building a CFG.
 *
 * Every local is flattened into scalar "leaves" (a struct becomes its scalar
 * fields, recursively). Each leaf is UNINIT, INIT or MOVED. A place such as
 * `r.size` covers a contiguous range of leaves, so partial moves and partial
 * initialization need no special cases.
 *
 * Rules enforced here:
 *   - a place must be fully initialized when it is read, moved or passed;
 *   - using a non-copy place by value moves it; copy places are copied;
 *   - nothing may be moved out of a `let` parameter;
 *   - inout and set parameters must be fully initialized whenever the
 *     function returns (so a caller never sees a hole);
 *   - within one expression, accesses that overlap may only both be reads
 *     (see "exclusivity" below).
 */
#include "noge.h"

enum { LS_UNINIT = 0, LS_INIT = 1, LS_MOVED = 2 };
enum { LK_VAR, LK_LET, LK_LETPARAM, LK_SINK, LK_INOUT, LK_SET };

typedef struct { bool live; unsigned char *s; } State; /* live=false: unreachable */
typedef struct { Str name; Type *type; int base, kind; } FLocal;
typedef struct { int local, lo, hi; } Place;           /* leaf range [lo, hi) */
typedef struct { int lo, hi; char kind; Node *node; } Access; /* 'r' read, 'w' inout/set, 'm' move */

static Node *cur_fn;
static FLocal *locs;
static int nleaves;
static State cur;
static bool reporting; /* false while a loop is iterated to a fixpoint */
static Access *accs;
static int nacc, cacc;

/* ---------- states ---------- */

static State st_new(void) {
    State s;
    s.live = true;
    s.s = arena_alloc(&g_arena, (size_t)(nleaves ? nleaves : 1));
    return s;
}

static State st_clone(const State *a) {
    State s = st_new();
    s.live = a->live;
    memcpy(s.s, a->s, (size_t)nleaves);
    return s;
}

static unsigned char meet(unsigned char a, unsigned char b) {
    if (a == LS_INIT && b == LS_INIT) return LS_INIT;
    if (a == LS_MOVED || b == LS_MOVED) return LS_MOVED;
    return LS_UNINIT;
}

/* dst = dst meet src; an unreachable state is the identity */
static void st_meet(State *dst, const State *src) {
    if (!src->live) return;
    if (!dst->live) {
        *dst = st_clone(src);
        return;
    }
    for (int i = 0; i < nleaves; i++) dst->s[i] = meet(dst->s[i], src->s[i]);
}

static bool st_equal(const State *a, const State *b) {
    return a->live == b->live && memcmp(a->s, b->s, (size_t)nleaves) == 0;
}

static void set_range(int lo, int hi, unsigned char v) { memset(cur.s + lo, v, (size_t)(hi - lo)); }

/* ---------- layout ---------- */

static void layout_local(int id, Str name, Type *t, int kind) {
    locs[id].name = name;
    locs[id].type = t;
    locs[id].base = nleaves;
    locs[id].kind = kind;
    nleaves += t->nleaves;
}

static void layout_block(Node *b);

static void layout_stmt(Node *s) {
    switch (s->kind) {
    case N_LET:
        layout_local(s->local, s->name, s->type, s->op ? LK_VAR : LK_LET);
        break;
    case N_IF:
        layout_block(s->b);
        if (s->c) {
            if (s->c->kind == N_IF) layout_stmt(s->c);
            else layout_block(s->c);
        }
        break;
    case N_WHILE:
        layout_block(s->b);
        break;
    case N_BLOCK:
        layout_block(s);
        break;
    default:
        break;
    }
}

static void layout_block(Node *b) {
    for (Node *s = b->a; s; s = s->next) layout_stmt(s);
}

/* ---------- places ---------- */

static bool is_place(Node *n) {
    while (n->kind == N_MEMBER) n = n->a;
    return n->kind == N_NAME;
}

static Place place_of(Node *n) {
    Place p;
    if (n->kind == N_NAME) {
        FLocal *l = &locs[n->local];
        p.local = n->local;
        p.lo = l->base;
        p.hi = l->base + l->type->nleaves;
        return p;
    }
    p = place_of(n->a);
    p.lo += n->decl->leaf_off;
    p.hi = p.lo + n->type->nleaves;
    return p;
}

/* "a.b.c" for a place expression; buf must start empty */
static void pname(Node *n, char *buf, size_t cap) {
    size_t len;
    if (n->kind == N_MEMBER) {
        pname(n->a, buf, cap);
        len = strlen(buf);
        snprintf(buf + len, cap - len, ".%.*s", SF(n->name));
    } else {
        len = strlen(buf);
        snprintf(buf + len, cap - len, "%.*s", SF(n->name));
    }
}

/* append ".f.g" for the scalar leaf at offset `off` inside type t */
static void leaf_path(Type *t, int off, char *buf, size_t cap) {
    while (t->kind == TY_STRUCT) {
        Node *f;
        for (f = t->decl->a; f; f = f->next)
            if (off >= f->leaf_off && off < f->leaf_off + f->type->nleaves) break;
        if (!f) return;
        size_t len = strlen(buf);
        snprintf(buf + len, cap - len, ".%.*s", SF(f->name));
        off -= f->leaf_off;
        t = f->type;
    }
}

/* ---------- init / move ---------- */

static void bad_use(Node *n, Place p) {
    int first = p.lo;
    while (cur.s[first] == LS_INIT) first++; /* caller guarantees one exists */
    bool uniform = true;
    for (int i = p.lo; i < p.hi; i++)
        if (cur.s[i] != cur.s[first]) uniform = false;
    char nm[160] = "";
    pname(n, nm, sizeof nm);
    if (!uniform) leaf_path(n->type, first - p.lo, nm, sizeof nm); /* name the missing part */
    if (cur.s[first] == LS_MOVED) die(n->line, "use of moved value '%s'", nm);
    die(n->line, "use of uninitialized value '%s'", nm);
}

/* Read, copy or move the place `n`. consume=true means "take the value":
 * a copy type is copied, anything else is moved out and left MOVED. */
static void use_place(Node *n, bool consume) {
    Place p = place_of(n);
    if (reporting && cur.live)
        for (int i = p.lo; i < p.hi; i++)
            if (cur.s[i] != LS_INIT) bad_use(n, p);
    if (consume && !n->type->is_copy) {
        if (reporting && locs[p.local].kind == LK_LETPARAM) {
            char nm[160] = "";
            pname(n, nm, sizeof nm);
            die(n->line,
                "cannot move out of '%s': '%.*s' is a read-only (let) parameter; "
                "make the type 'copy', or take the parameter as 'sink' or 'inout'",
                nm, SF(locs[p.local].name));
        }
        set_range(p.lo, p.hi, LS_MOVED);
    }
}

static void check_exit(int line) {
    if (!reporting || !cur.live) return;
    for (Node *p = cur_fn->a; p; p = p->next) {
        if (p->op != M_INOUT && p->op != M_SET) continue;
        FLocal *l = &locs[p->local];
        for (int i = l->base; i < l->base + l->type->nleaves; i++) {
            if (cur.s[i] == LS_INIT) continue;
            char nm[160];
            snprintf(nm, sizeof nm, "%.*s", SF(p->name));
            leaf_path(l->type, i - l->base, nm, sizeof nm);
            die(line, "%s parameter '%.*s' must be fully initialized when the function returns, but '%s' was %s",
                p->op == M_SET ? "set" : "inout", SF(p->name), nm,
                cur.s[i] == LS_MOVED ? "moved out" : "never initialized");
        }
    }
}

/* ---------- exclusivity ----------
 * C does not fix the order in which operands, call arguments or initializer
 * fields are evaluated, and arguments are live together for the whole call.
 * So within one such group, two accesses to overlapping storage are only
 * allowed when both are plain reads. Moves, inout and set accesses must not
 * overlap anything else in the group, however deeply it is nested.
 */

static void push(Place p, char kind, Node *n) {
    if (nacc == cacc) {
        cacc = cacc ? cacc * 2 : 64;
        accs = realloc(accs, (size_t)cacc * sizeof *accs);
        if (!accs) {
            fprintf(stderr, "noge: out of memory\n");
            exit(1);
        }
    }
    accs[nacc].lo = p.lo;
    accs[nacc].hi = p.hi;
    accs[nacc].kind = kind;
    accs[nacc].node = n;
    nacc++;
}

static void collect(Node *n, bool consume);

static void collect_arg(Node *a, int mode) {
    switch (mode) {
    case M_LET:
        collect(a, false);
        break;
    case M_SINK:
        collect(a, true);
        break;
    default:
        push(place_of(a->a), 'w', a->a);
        break;
    }
}

static void collect(Node *n, bool consume) {
    switch (n->kind) {
    case N_NAME:
        push(place_of(n), consume && !n->type->is_copy ? 'm' : 'r', n);
        break;
    case N_MEMBER:
        if (is_place(n)) push(place_of(n), consume && !n->type->is_copy ? 'm' : 'r', n);
        else collect(n->a, true);
        break;
    case N_UN:
    case N_CAST:
        collect(n->a, true);
        break;
    case N_BIN:
        collect(n->a, true);
        collect(n->b, true);
        break;
    case N_CALL: {
        Node *p = n->decl->a;
        for (Node *a = n->a; a; a = a->next, p = p->next) collect_arg(a, p->op);
        break;
    }
    case N_LIT:
        for (Node *i = n->a; i; i = i->next) collect(i->a, true);
        break;
    default:
        break;
    }
}

static const char *kind_word(char k) { return k == 'r' ? "read" : k == 'm' ? "moved" : "inout/set"; }

/* start[i]..start[i+1] are the accesses of part i; compare every pair of parts */
static void conflicts(const int *start, int count) {
    for (int i = 0; i < count; i++)
        for (int j = i + 1; j < count; j++)
            for (int x = start[i]; x < start[i + 1]; x++)
                for (int y = start[j]; y < start[j + 1]; y++) {
                    Access *p = &accs[x], *q = &accs[y];
                    if (p->kind == 'r' && q->kind == 'r') continue;
                    if (p->lo >= q->hi || q->lo >= p->hi) continue;
                    char a[160] = "", b[160] = "";
                    pname(p->node, a, sizeof a);
                    pname(q->node, b, sizeof b);
                    die(q->node->line,
                        "conflicting accesses in one expression: '%s' (%s) overlaps '%s' (%s); "
                        "split it into separate statements",
                        a, kind_word(p->kind), b, kind_word(q->kind));
                }
}

static void group_call(Node *n) {
    int cnt = 0;
    for (Node *a = n->a; a; a = a->next) cnt++;
    if (cnt < 2) return;
    int *start = arena_alloc(&g_arena, sizeof(int) * (size_t)(cnt + 1));
    int base = nacc, i = 0;
    Node *p = n->decl->a;
    for (Node *a = n->a; a; a = a->next, p = p->next, i++) {
        start[i] = nacc;
        collect_arg(a, p->op);
    }
    start[cnt] = nacc;
    conflicts(start, cnt);
    nacc = base;
}

static void group_bin(Node *n) {
    int start[3], base = nacc;
    start[0] = nacc;
    collect(n->a, true);
    start[1] = nacc;
    collect(n->b, true);
    start[2] = nacc;
    conflicts(start, 2);
    nacc = base;
}

static void group_lit(Node *n) {
    int cnt = 0;
    for (Node *i = n->a; i; i = i->next) cnt++;
    if (cnt < 2) return;
    int *start = arena_alloc(&g_arena, sizeof(int) * (size_t)(cnt + 1));
    int base = nacc, k = 0;
    for (Node *i = n->a; i; i = i->next, k++) {
        start[k] = nacc;
        collect(i->a, true);
    }
    start[cnt] = nacc;
    conflicts(start, cnt);
    nacc = base;
}

/* ---------- expressions ---------- */

static void expr(Node *n, bool consume);

static void call(Node *n) {
    Node *fn = n->decl;
    if (reporting) group_call(n);
    Node *p = fn->a;
    for (Node *a = n->a; a; a = a->next, p = p->next) {
        switch (p->op) {
        case M_LET:
            expr(a, false); /* borrowed for the call: read, never moved */
            break;
        case M_SINK:
            expr(a, true);
            break;
        case M_INOUT:
            use_place(a->a, false); /* must be fully initialized, stays so */
            break;
        default: /* M_SET: the callee initializes it */
            break;
        }
    }
    p = fn->a;
    for (Node *a = n->a; a; a = a->next, p = p->next)
        if (p->op == M_SET) {
            Place pl = place_of(a->a);
            set_range(pl.lo, pl.hi, LS_INIT);
        }
}

static void expr(Node *n, bool consume) {
    switch (n->kind) {
    case N_NAME:
        use_place(n, consume);
        break;
    case N_MEMBER:
        if (is_place(n)) use_place(n, consume);
        else expr(n->a, true);
        break;
    case N_UN:
    case N_CAST:
        expr(n->a, true);
        break;
    case N_BIN:
        if (n->op == T_ANDAND || n->op == T_OROR) { /* the right side runs only sometimes */
            expr(n->a, true);
            State skipped = st_clone(&cur);
            expr(n->b, true);
            st_meet(&cur, &skipped);
        } else {
            if (reporting) group_bin(n);
            expr(n->a, true);
            expr(n->b, true);
        }
        break;
    case N_CALL:
        call(n);
        break;
    case N_LIT:
        if (reporting) group_lit(n);
        for (Node *i = n->a; i; i = i->next) expr(i->a, true);
        break;
    default:
        break;
    }
}

/* ---------- statements ---------- */

static void stmt(Node *s);

static void block(Node *b) {
    for (Node *s = b->a; s; s = s->next) stmt(s);
}

static void stmt(Node *s) {
    switch (s->kind) {
    case N_LET: {
        FLocal *l = &locs[s->local];
        if (s->a) expr(s->a, true);
        /* (re)declaring resets the variable, which matters inside loops */
        set_range(l->base, l->base + l->type->nleaves, s->a ? LS_INIT : LS_UNINIT);
        break;
    }
    case N_ASSIGN: {
        expr(s->b, true);
        Place p = place_of(s->a); /* writing is not a use: it (re)initializes */
        set_range(p.lo, p.hi, LS_INIT);
        break;
    }
    case N_EXPR:
        expr(s->a, true);
        break;
    case N_IF: {
        expr(s->a, true);
        State entry = st_clone(&cur);
        block(s->b);
        State then_end = cur;
        cur = entry;
        if (s->c) {
            if (s->c->kind == N_IF) stmt(s->c);
            else block(s->c);
        }
        st_meet(&cur, &then_end);
        break;
    }
    case N_WHILE: {
        /* Find the state at the loop head: entry meet end-of-body, repeated until stable. */
        State head = st_clone(&cur);
        bool saved = reporting;
        reporting = false;
        for (int iter = 0;; iter++) {
            if (iter > 1000) die(s->line, "internal error: flow analysis did not converge");
            cur = st_clone(&head);
            expr(s->a, true);
            block(s->b);
            State next = st_clone(&head);
            st_meet(&next, &cur);
            if (st_equal(&next, &head)) break;
            head = next;
        }
        reporting = saved;
        /* Final pass from the stable head state, reporting errors. */
        cur = st_clone(&head);
        expr(s->a, true);
        State exit = st_clone(&cur); /* the loop is left when the condition is false */
        block(s->b);
        cur = exit;
        break;
    }
    case N_RETURN:
        if (s->a) expr(s->a, true);
        check_exit(s->line);
        cur.live = false;
        break;
    case N_BLOCK:
        block(s);
        break;
    default:
        break;
    }
}

/* ---------- driver ---------- */

static void flow_fn(Node *fn) {
    cur_fn = fn;
    locs = arena_alloc(&g_arena, sizeof(FLocal) * (size_t)(fn->nlocals ? fn->nlocals : 1));
    nleaves = 0;
    nacc = 0;
    for (Node *p = fn->a; p; p = p->next) {
        int kind = p->op == M_INOUT ? LK_INOUT : p->op == M_SET ? LK_SET : p->op == M_SINK ? LK_SINK : LK_LETPARAM;
        layout_local(p->local, p->name, p->type, kind);
    }
    layout_block(fn->b);
    cur = st_new();
    for (Node *p = fn->a; p; p = p->next) {
        FLocal *l = &locs[p->local];
        if (l->kind != LK_SET) set_range(l->base, l->base + l->type->nleaves, LS_INIT);
    }
    reporting = true;
    block(fn->b);
    check_exit(fn->b->end_line);
}

void flow_program(Node *prog) {
    for (Node *d = prog; d; d = d->next)
        if (d->kind == N_FN) flow_fn(d);
}
