/* Flow analysis: definite initialization, moves, and exclusivity.
 *
 * Runs on the type-checked AST. Noge only has structured control flow
 * (if / while / return), so the analysis walks the tree directly instead of
 * building a CFG. It visits expressions in exactly the order lowering
 * evaluates them (left to right; an assignment's right side first).
 *
 * Every local is flattened into scalar "leaves" (a struct becomes its scalar
 * fields, recursively; an array is a single leaf, because its elements are
 * reached by a run-time index). Each leaf is UNINIT, INIT or MOVED. A place
 * such as `r.size` covers a contiguous range of leaves, so partial moves and
 * partial initialization need no special cases.
 *
 * Rules enforced here:
 *   - a place must be fully initialized when it is read, moved or passed;
 *   - using a non-copy place by value moves it; copy places are copied;
 *   - nothing may be moved out of a `let` parameter, or out of an array
 *     element (elements are chosen at run time, so only the whole array moves);
 *   - an array must be whole before one of its elements is written;
 *   - inout and set parameters must be fully initialized whenever the
 *     function returns (so a caller never sees a hole);
 *   - exclusivity: the arguments of one call are evaluated left to right. A
 *     `let`, `inout` or `set` argument that names a place borrows it until the
 *     call returns. Any later access that overlaps a live borrow, including
 *     one inside a later argument, is an error unless both are plain reads.
 */
#include "noge.h"

enum { LS_UNINIT = 0, LS_INIT = 1, LS_MOVED = 2 };

typedef struct { bool live; unsigned char *s; } State; /* live=false: unreachable */
typedef struct { Str name; Type *type; int base, kind; } FLocal;
typedef struct { int local, lo, hi; bool idx; } Place; /* leaf range [lo, hi); idx: reached through an array index */
typedef struct { int lo, hi; bool write; Node *node; } Borrow;

static Node *cur_fn;
static FLocal *locs;
static int nleaves;
static State cur;
static bool reporting; /* false while a loop is iterated to a fixpoint */
static Borrow *live;   /* borrows of the calls being evaluated, innermost last */
static int nlive, clive;

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

/* ---------- places ---------- */

static bool is_place(Node *n) {
    while (n->kind == N_MEMBER || n->kind == N_INDEX) n = n->a;
    return n->kind == N_NAME;
}

static Place place_of(Node *n) {
    Place p;
    if (n->kind == N_NAME) {
        FLocal *l = &locs[n->local];
        p.local = n->local;
        p.lo = l->base;
        p.hi = l->base + l->type->nleaves;
        p.idx = false;
        return p;
    }
    p = place_of(n->a);
    if (n->kind == N_INDEX || p.idx) { /* inside an array: still that array's one leaf */
        p.idx = true;
        return p;
    }
    p.lo += n->decl->leaf_off;
    p.hi = p.lo + n->type->nleaves;
    return p;
}

/* the array that owns the storage of a place reached through an index */
static Node *owning_array(Node *n) {
    if (n->kind == N_NAME) return NULL;
    Node *r = owning_array(n->a);
    if (r) return r;
    return n->kind == N_INDEX ? n->a : NULL;
}

/* "a.b.c" / "a[_].c" for a place expression; buf must start empty */
static void pname(Node *n, char *buf, size_t cap) {
    size_t len;
    if (n->kind == N_MEMBER) {
        pname(n->a, buf, cap);
        len = strlen(buf);
        snprintf(buf + len, cap - len, ".%.*s", SF(n->name));
    } else if (n->kind == N_INDEX) {
        pname(n->a, buf, cap);
        len = strlen(buf);
        snprintf(buf + len, cap - len, "[_]");
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
    if (p.idx) { /* name the whole array, which is what is missing */
        pname(owning_array(n), nm, sizeof nm);
    } else {
        pname(n, nm, sizeof nm);
        if (!uniform) leaf_path(n->type, first - p.lo, nm, sizeof nm); /* name the missing part */
    }
    if (cur.s[first] == LS_MOVED) die(n->line, "use of moved value '%s'", nm);
    die(n->line, "use of uninitialized value '%s'", nm);
}

static void check_init(Node *n, Place p) {
    if (!reporting || !cur.live) return;
    for (int i = p.lo; i < p.hi; i++)
        if (cur.s[i] != LS_INIT) bad_use(n, p);
}

/* ---------- exclusivity ---------- */

static const char *kind_word(char k) { return k == 'r' ? "read" : k == 'm' ? "moved" : "inout/set"; }

/* An access (a read, a write or a move) must not overlap a live borrow, unless both only read. */
static void access(Node *n, Place p, char kind) {
    if (!reporting) return;
    for (int i = 0; i < nlive; i++) {
        Borrow *b = &live[i];
        if (p.lo >= b->hi || b->lo >= p.hi) continue;
        if (kind == 'r' && !b->write) continue;
        char a[160] = "", c[160] = "";
        pname(n, a, sizeof a);
        pname(b->node, c, sizeof c);
        die(n->line,
            "conflicting accesses: '%s' (%s) overlaps '%s' (%s), which an earlier argument of the same call "
            "borrows until the call returns",
            a, kind_word(kind), c, b->write ? "inout/set" : "read");
    }
}

static void start_borrow(Node *n, Place p, bool write) {
    access(n, p, write ? 'w' : 'r');
    if (nlive == clive) {
        clive = clive ? clive * 2 : 32;
        live = realloc(live, (size_t)clive * sizeof *live);
        if (!live) {
            fprintf(stderr, "noge: out of memory\n");
            exit(1);
        }
    }
    live[nlive].lo = p.lo;
    live[nlive].hi = p.hi;
    live[nlive].write = write;
    live[nlive].node = n;
    nlive++;
}

/* ---------- expressions ---------- */

static void expr(Node *n, bool consume);
static void block(Node *b);

/* Evaluate the index expressions inside a place, outermost first. */
static void path_exprs(Node *n) {
    if (n->kind == N_MEMBER) {
        path_exprs(n->a);
    } else if (n->kind == N_INDEX) {
        path_exprs(n->a);
        expr(n->b, true);
    }
}

/* Read, copy or move the place `n`. consume=true means "take the value":
 * a copy type is copied, anything else is moved out and left MOVED. */
/* Take a non-copy place's value away: only an owned, whole place can be moved out of. */
static void move_place(Node *n, Place p) {
    {
        if (reporting && p.idx) {
            char nm[160] = "";
            pname(owning_array(n), nm, sizeof nm);
            die(n->line,
                "cannot move out of an element of array '%s': its elements are not copy, and only the whole array can be moved",
                nm);
        }
        if (reporting && locs[p.local].kind == LK_LETPARAM) {
            char nm[160] = "";
            pname(n, nm, sizeof nm);
            die(n->line,
                "cannot move out of '%s': '%.*s' is a read-only (let) parameter; "
                "make the type 'copy', or take the parameter as 'sink' or 'inout'",
                nm, SF(locs[p.local].name));
        }
    }
    access(n, p, 'm');
    set_range(p.lo, p.hi, LS_MOVED);
}

static void use_place(Node *n, bool consume) {
    path_exprs(n);
    Place p = place_of(n);
    check_init(n, p);
    if (consume && !n->type->is_copy) move_place(n, p);
    else access(n, p, 'r');
}

/* Borrow a place for the rest of the enclosing call. */
static void place_borrow(Node *n, bool write, bool need_init) {
    path_exprs(n);
    Place p = place_of(n);
    if (need_init || p.idx) check_init(n, p); /* an element can only be reached in a whole array */
    start_borrow(n, p, write);
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

/* The names a pattern binds become initialized locals. Returns, through *moves, whether one of them takes a
 * non-copy value, which moves it out of the value being matched. */
static void pattern_bind(Node *p, bool *moves) {
    switch (p->op) {
    case PAT_BIND: {
        FLocal *l = &locs[p->local];
        set_range(l->base, l->base + l->type->nleaves, LS_INIT);
        if (!p->type->is_copy) *moves = true;
        break;
    }
    case PAT_VARIANT:
        for (Node *s = p->a; s; s = s->next) pattern_bind(s, moves);
        break;
    default: /* wildcards and literals bind nothing; alternatives may not bind */
        break;
    }
}

/* A match is an `if` chain: the scrutinee is evaluated once, each arm starts from the state after it, and the
 * state afterwards is the meet of the arms that fall out of the bottom. An arm that binds a non-copy value takes
 * it out of the scrutinee (an enum is one unit, so the whole thing is gone afterwards). */
static void do_match(Node *m) {
    Node *scrut = m->a;
    bool is_pl = is_place(scrut);
    Place sp = {0};
    if (is_pl) {
        path_exprs(scrut);
        sp = place_of(scrut);
        check_init(scrut, sp);
    } else {
        expr(scrut, true);
    }
    State entry = st_clone(&cur);
    State merged = {0};
    bool have = false;
    for (Node *arm = m->b; arm; arm = arm->next) {
        cur = st_clone(&entry);
        bool moves = false;
        pattern_bind(arm->a, &moves);
        if (is_pl) {
            if (moves) move_place(scrut, sp);
            else access(scrut, sp, 'r');
        }
        if (arm->op) block(arm->b);
        else expr(arm->b, true);
        if (!have) {
            merged = cur;
            have = true;
        } else {
            st_meet(&merged, &cur);
        }
    }
    cur = merged;
}

static void call(Node *n) {
    Node *fn = n->decl;
    int base = nlive;
    Node *p = fn->a;
    for (Node *a = n->a; a; a = a->next, p = p->next) {
        switch (p->op) {
        case M_LET:
            if (is_place(a)) place_borrow(a, false, true); /* borrowed for the whole call */
            else expr(a, false);
            break;
        case M_SINK:
            expr(a, true); /* taken (copied or moved) right now */
            break;
        case M_INOUT:
            place_borrow(a->a, true, true);
            break;
        default: /* M_SET: the callee initializes it */
            place_borrow(a->a, true, false);
            break;
        }
    }
    nlive = base; /* the call is over: its borrows end */
    p = fn->a;
    for (Node *a = n->a; a; a = a->next, p = p->next)
        if (p->op == M_SET) {
            Place pl = place_of(a->a);
            if (!pl.idx) set_range(pl.lo, pl.hi, LS_INIT);
        }
}

static void expr(Node *n, bool consume) {
    switch (n->kind) {
    case N_NAME:
    case N_INDEX:
        use_place(n, consume);
        break;
    case N_MEMBER:
        if (is_place(n)) use_place(n, consume);
        else expr(n->a, true);
        break;
    case N_UN:
    case N_CAST: /* operators only look at their operands (a plain enum compared or cast stays usable) */
        expr(n->a, false);
        break;
    case N_BIN:
        expr(n->a, false);
        if (n->op == T_ANDAND || n->op == T_OROR) { /* the right side runs only sometimes */
            State skipped = st_clone(&cur);
            expr(n->b, false);
            st_meet(&cur, &skipped);
        } else {
            expr(n->b, false);
        }
        break;
    case N_CALL:
        call(n);
        break;
    case N_LIT:
        for (Node *i = n->a; i; i = i->next) expr(i->a, true);
        break;
    case N_ARRAY:
        for (Node *e = n->a; e; e = e->next) expr(e, true);
        break;
    case N_REPEAT:
        expr(n->a, true);
        break;
    case N_VARIANT: /* Enum::Variant(values): the values are taken in order, like a struct literal's fields */
        for (Node *a = n->a; a; a = a->next) expr(a, true);
        break;
    case N_MATCH:
        do_match(n);
        break;
    case N_PTROP: /* raw pointers are plain values; what they point at is not tracked */
        expr(n->a, true);
        if (n->b) expr(n->b, true);
        break;
    default: /* literals, C strings */
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
        expr(s->b, true); /* right side first, then the place */
        path_exprs(s->a);
        Place p = place_of(s->a);
        if (p.idx) check_init(s->a, p); /* writing an element needs a whole array */
        else set_range(p.lo, p.hi, LS_INIT); /* writing a variable or field (re)initializes it */
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
    case N_MATCH:
        do_match(s);
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
    nlive = 0;
    for (int id = 0; id < fn->nlocals; id++) layout_local(id, fn->linfo[id].name, fn->linfo[id].type, fn->linfo[id].kind);
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
        if (d->kind == N_FN && d->ext != EXT_EXTERN) flow_fn(d);
}
