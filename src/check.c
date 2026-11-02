/* Name resolution and type checking. Stops at the first error.
 *
 * Besides checking, this pass rewrites the tree in a few places so that later passes see simpler nodes:
 * intrinsic calls become operators or N_PTROP nodes, `len(a)` and named constants become literals, and any
 * expression made only of literals is folded to one literal (with overflow reported at compile time). */
#include "noge.h"

#define PRIM(k, nm, b, sg, cn, nl) \
    {.kind = (k), .name = (nm), .bits = (b), .is_signed = (sg), .cname = (cn), .is_copy = true, .nleaves = (nl)}
static Type prims[] = {
    PRIM(TY_UNIT, "unit", 0, false, "void", 0),
    PRIM(TY_BOOL, "bool", 1, false, "bool", 1),
    PRIM(TY_INT, "i8", 8, true, "int8_t", 1),
    PRIM(TY_INT, "i16", 16, true, "int16_t", 1),
    PRIM(TY_INT, "i32", 32, true, "int32_t", 1),
    PRIM(TY_INT, "i64", 64, true, "int64_t", 1),
    PRIM(TY_INT, "u8", 8, false, "uint8_t", 1),
    PRIM(TY_INT, "u16", 16, false, "uint16_t", 1),
    PRIM(TY_INT, "u32", 32, false, "uint32_t", 1),
    PRIM(TY_INT, "u64", 64, false, "uint64_t", 1),
    PRIM(TY_INT, "usize", 0, false, "uintptr_t", 1), /* bits: the target's pointer width, set below */
    PRIM(TY_INT, "isize", 0, true, "intptr_t", 1),
};
#define NPRIMS ((int)(sizeof prims / sizeof *prims))
Type *ty_unit = &prims[0], *ty_bool = &prims[1], *ty_i32 = &prims[4], *ty_usize = &prims[10], *ty_isize = &prims[11];
static Type *const ty_u8 = &prims[6];

Type *g_array_types; /* every array type, in creation order */
Type *g_ptr_types;   /* every raw-pointer type, in creation order */
bool g_freestanding;
int g_ptr_bits = (int)(sizeof(void *) * 8);

static Node *prog;
static Node builtin_print, builtin_print_param;

/* ---- name tables: structs, functions and constants are looked up by name all the time ---- */
typedef struct { Str key; Node *val; } Slot;
typedef struct { Slot *slots; size_t cap; } Map;
static Map struct_map, fn_map, const_map;

static uint32_t hash_str(Str s) { /* FNV-1a */
    uint32_t h = 2166136261u;
    for (int i = 0; i < s.n; i++) h = (h ^ (unsigned char)s.p[i]) * 16777619u;
    return h;
}

static void map_init(Map *m, size_t expected) {
    size_t cap = 16;
    while (cap < expected * 2) cap *= 2;
    m->cap = cap;
    m->slots = arena_alloc(&g_arena, cap * sizeof(Slot));
}

static Node *map_get(Map *m, Str key) {
    for (size_t i = hash_str(key) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        if (!m->slots[i].val) return NULL;
        if (str_eq(m->slots[i].key, key)) return m->slots[i].val;
    }
}

/* false if the name is already there */
static bool map_put(Map *m, Str key, Node *val) {
    for (size_t i = hash_str(key) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        if (!m->slots[i].val) {
            m->slots[i].key = key;
            m->slots[i].val = val;
            return true;
        }
        if (str_eq(m->slots[i].key, key)) return false;
    }
}

/* ---- locals ---- */
typedef struct {
    Str name;
    Type *type;
    int id, depth, is_mut, by_ptr;
} Local;
#define MAX_LOCALS 8192
static Local locals[MAX_LOCALS];
static int nlocals, depth, next_id;
static int unsafe_depth; /* > 0 inside an unsafe block */
static int const_ctx;    /* > 0 while evaluating a constant: no locals, no calls */
static Node *cur_fn;

static Local *find_local(Str name) {
    for (int i = nlocals - 1; i >= 0; i--)
        if (str_eq(locals[i].name, name)) return &locals[i];
    return NULL;
}

static int declare(int line, Str name, Type *t, int is_mut, int by_ptr) {
    for (int i = nlocals - 1; i >= 0 && locals[i].depth == depth; i--)
        if (str_eq(locals[i].name, name))
            die(line, "'%.*s' is already declared in this scope", SF(name));
    if (nlocals == MAX_LOCALS) die(line, "too many local variables");
    Local *l = &locals[nlocals++];
    l->name = name;
    l->type = t;
    l->id = next_id++;
    l->depth = depth;
    l->is_mut = is_mut;
    l->by_ptr = by_ptr;
    return l->id;
}

static void require_unsafe(Node *n, const char *what) {
    if (!unsafe_depth) die(n->line, "%s requires an unsafe block", what);
}

/* ---- constants ---- */
static Type *check_expr(Node *n, Type *expect);

static void const_resolve(Node *c);

/* An array length is a constant expression: 8, BUF_LEN, 4 * WORDS + 1. It cannot see local variables. */
static uint64_t const_length(Node *e) {
    int saved_unsafe = unsafe_depth;
    unsafe_depth = 0;
    const_ctx++;
    Type *t = check_expr(e, ty_usize);
    const_ctx--;
    unsafe_depth = saved_unsafe;
    if (e->kind != N_INT || t->kind != TY_INT)
        die(e->line, "an array length must be a compile-time integer constant");
    if (e->op && e->ival) die(e->line, "an array length cannot be negative");
    return e->ival;
}

/* ---- types ---- */
static Node *find_struct(Str name) { return map_get(&struct_map, name); }

/* Array and pointer types are interned, so two spellings of [i32; 4] are the same Type. */
static Type *array_type(Type *elem, uint64_t len) {
    Type **tail = &g_array_types;
    for (; *tail; tail = &(*tail)->next_array)
        if ((*tail)->elem == elem && (*tail)->len == len) return *tail;
    Type *t = ARENA_NEW(&g_arena, Type);
    t->kind = TY_ARRAY;
    t->elem = elem;
    t->len = len;
    t->is_copy = elem->is_copy;
    t->nleaves = 1; /* flow treats an array as one unit: elements are reached by a run-time index */
    size_t n = strlen(elem->name) + strlen(elem->cname) + 64;
    char *name = arena_alloc(&g_arena, n), *cname = arena_alloc(&g_arena, n);
    snprintf(name, n, "[%s; %llu]", elem->name, (unsigned long long)len);
    snprintf(cname, n, "arr_%s_%llu", elem->kind == TY_ARRAY ? elem->cname : elem->name, (unsigned long long)len);
    t->name = name;
    t->cname = cname;
    *tail = t;
    return t;
}

static Type *ptr_type(Type *pointee, bool is_mut) {
    Type **tail = &g_ptr_types;
    for (; *tail; tail = &(*tail)->next_ptr)
        if ((*tail)->elem == pointee && (*tail)->is_mut == is_mut) return *tail;
    Type *t = ARENA_NEW(&g_arena, Type);
    t->kind = TY_PTR;
    t->elem = pointee;
    t->is_mut = is_mut;
    t->is_copy = true;
    t->nleaves = 1;
    size_t n = strlen(pointee->name) + strlen(pointee->cname) + 32;
    char *name = arena_alloc(&g_arena, n), *cname = arena_alloc(&g_arena, n);
    snprintf(name, n, "*%s %s", is_mut ? "mut" : "const", pointee->name);
    snprintf(cname, n, "ptr_%s_%s", is_mut ? "mut" : "const",
             (pointee->kind == TY_ARRAY || pointee->kind == TY_PTR) ? pointee->cname : pointee->name);
    t->name = name;
    t->cname = cname;
    *tail = t;
    return t;
}

static Type *resolve_type(Node *ty) {
    if (ty->op == 2) /* *mut T, *const T */
        return ptr_type(resolve_type(ty->a), ty->ival != 0);
    if (ty->op == 1) { /* [T; N] */
        Type *elem = resolve_type(ty->a);
        uint64_t len = const_length(ty->b);
        if (len == 0) die(ty->line, "an array needs at least one element");
        if (len > 0x7fffffffu) die(ty->line, "array length %llu is too large", (unsigned long long)len);
        return array_type(elem, len);
    }
    for (int i = 1; i < NPRIMS; i++)
        if (str_is(ty->name, prims[i].name)) return &prims[i];
    Node *s = find_struct(ty->name);
    if (s) return s->type;
    die(ty->line, "unknown type '%.*s'", SF(ty->name));
}

static void expect_type(int line, Type *want, Type *got) {
    if (want != got)
        die(line, "type mismatch: expected '%s', found '%s'", want->name, got->name);
}

static Node *find_field(Node *s, Str name) {
    for (Node *f = s->a; f; f = f->next)
        if (str_eq(f->name, name)) return f;
    return NULL;
}

static bool is_intrinsic(Str name) {
    return str_is(name, "len") || str_is(name, "wrapping_add") || str_is(name, "wrapping_sub") ||
           str_is(name, "wrapping_mul") || str_is(name, "ptr_read") || str_is(name, "ptr_write") ||
           str_is(name, "ptr_offset") || str_is(name, "volatile_read") || str_is(name, "volatile_write");
}

static Node *lookup_fn(Str name) {
    Node *f = map_get(&fn_map, name);
    if (f) return f;
    if (!g_freestanding && str_eq(name, builtin_print.name)) return &builtin_print;
    return NULL;
}

/* ---- places ---- */
static bool is_place(Node *n) {
    while (n->kind == N_MEMBER || n->kind == N_INDEX) n = n->a;
    return n->kind == N_NAME;
}

static void require_mutable_place(Node *n, const char *action) {
    if (!is_place(n)) die(n->line, "only a variable or field can be %s", action);
    Node *root = n;
    while (root->kind == N_MEMBER || root->kind == N_INDEX) root = root->a;
    if (!root->mut)
        die(n->line, "'%.*s' is read-only (declare it with 'var', or take it as an 'inout' parameter), so it cannot be %s",
            SF(root->name), action);
}

/* ---- constant folding ----
 * An expression whose operands are all literals is replaced by one literal. The rules are the run-time
 * rules (checked arithmetic, shift counts, division), so what would trap at run time is an error here. */
static uint64_t mask_bits(int bits) { return bits >= 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1; }

static int64_t sext(uint64_t v, int bits) {
    v &= mask_bits(bits);
    if (bits < 64 && (v >> (bits - 1))) v |= ~mask_bits(bits);
    return (int64_t)v;
}

static bool is_litnode(const Node *n) { return n->kind == N_INT || n->kind == N_BOOL; }

/* the two's complement bit pattern of an integer or bool literal, truncated to its own type */
static uint64_t lit_pattern(const Node *n) {
    if (n->kind == N_BOOL) return n->ival;
    return (n->op ? (uint64_t)0 - n->ival : n->ival) & mask_bits(n->type->bits);
}

/* rewrite n, in place, into an integer literal of type t holding the low t->bits of `pattern` */
static void make_int(Node *n, Type *t, uint64_t pattern) {
    pattern &= mask_bits(t->bits);
    n->kind = N_INT;
    n->a = n->b = NULL;
    if (t->is_signed) {
        int64_t v = sext(pattern, t->bits);
        n->op = v < 0;
        n->ival = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    } else {
        n->op = 0;
        n->ival = pattern;
    }
}

static void make_bool(Node *n, bool v) {
    n->kind = N_BOOL;
    n->a = n->b = NULL;
    n->op = 0;
    n->ival = v;
}

static int64_t min_signed(int bits) { return bits >= 64 ? INT64_MIN : -(INT64_C(1) << (bits - 1)); }
static int64_t max_signed(int bits) { return bits >= 64 ? INT64_MAX : (INT64_C(1) << (bits - 1)) - 1; }

static void fold_bin(Node *n) {
    Type *t = n->a->type;
    int op = n->op;
    if (t == ty_bool) {
        bool x = n->a->ival, y = n->b->ival;
        make_bool(n, op == T_EQ ? x == y : op == T_NE ? x != y : op == T_ANDAND ? (x && y) : (x || y));
        return;
    }
    int bits = t->bits;
    uint64_t xp = lit_pattern(n->a), yp = lit_pattern(n->b);
    int64_t xs = sext(xp, bits), ys = sext(yp, bits);
    bool sg = t->is_signed;
    switch (op) {
    case T_EQ: make_bool(n, xp == yp); return;
    case T_NE: make_bool(n, xp != yp); return;
    case T_LT: make_bool(n, sg ? xs < ys : xp < yp); return;
    case T_LE: make_bool(n, sg ? xs <= ys : xp <= yp); return;
    case T_GT: make_bool(n, sg ? xs > ys : xp > yp); return;
    case T_GE: make_bool(n, sg ? xs >= ys : xp >= yp); return;
    case T_AMP: make_int(n, t, xp & yp); return;
    case T_PIPE: make_int(n, t, xp | yp); return;
    case T_CARET: make_int(n, t, xp ^ yp); return;
    case OP_WADD: make_int(n, t, xp + yp); return;
    case OP_WSUB: make_int(n, t, xp - yp); return;
    case OP_WMUL: make_int(n, t, xp * yp); return;
    case T_SHL:
    case T_SHR: {
        if ((sg && ys < 0) || (sg ? (uint64_t)ys : yp) >= (uint64_t)bits)
            die(n->line, "shift amount is out of range for '%s' (0 to %d)", t->name, bits - 1);
        unsigned c = (unsigned)yp;
        make_int(n, t, op == T_SHL ? xp << c : sg ? (uint64_t)(xs >> c) : xp >> c);
        return;
    }
    default:
        break;
    }
    bool ovf = false;
    if (sg) {
        int64_t r = 0;
        switch (op) {
        case T_PLUS: ovf = __builtin_add_overflow(xs, ys, &r); break;
        case T_MINUS: ovf = __builtin_sub_overflow(xs, ys, &r); break;
        case T_STAR: ovf = __builtin_mul_overflow(xs, ys, &r); break;
        default: /* / and % */
            if (ys == 0) die(n->line, "division by zero");
            if (xs == min_signed(bits) && ys == -1) ovf = true;
            else r = op == T_SLASH ? xs / ys : xs % ys;
            break;
        }
        if (ovf || r < min_signed(bits) || r > max_signed(bits))
            die(n->line, "constant expression overflows '%s'", t->name);
        make_int(n, t, (uint64_t)r);
    } else {
        uint64_t r = 0;
        switch (op) {
        case T_PLUS: ovf = __builtin_add_overflow(xp, yp, &r); break;
        case T_MINUS: ovf = xp < yp; r = xp - yp; break;
        case T_STAR: ovf = __builtin_mul_overflow(xp, yp, &r); break;
        default:
            if (yp == 0) die(n->line, "division by zero");
            r = op == T_SLASH ? xp / yp : xp % yp;
            break;
        }
        if (ovf || r > mask_bits(bits)) die(n->line, "constant expression overflows '%s'", t->name);
        make_int(n, t, r);
    }
}

static void fold_un(Node *n) {
    Type *t = n->a->type;
    if (n->op == T_BANG) {
        make_bool(n, !n->a->ival);
    } else if (n->op == T_TILDE) {
        make_int(n, t, ~lit_pattern(n->a));
    } else { /* unary minus */
        int64_t x = sext(lit_pattern(n->a), t->bits);
        if (x == min_signed(t->bits)) die(n->line, "constant expression overflows '%s'", t->name);
        make_int(n, t, (uint64_t)(-x));
    }
}

static void fold_cast(Node *n) {
    Type *from = n->a->type;
    uint64_t v = lit_pattern(n->a);
    if (from->kind == TY_INT && from->is_signed) v = (uint64_t)sext(v, from->bits);
    make_int(n, n->type, v);
}

static void fold(Node *n) {
    switch (n->kind) {
    case N_UN:
        if (is_litnode(n->a)) fold_un(n);
        break;
    case N_CAST:
        if (is_litnode(n->a) && n->type->kind == TY_INT) fold_cast(n);
        break;
    case N_BIN:
        if (is_litnode(n->a) && is_litnode(n->b)) fold_bin(n);
        break;
    default:
        break;
    }
}

/* ---- expressions ---- */
static bool is_lit(Node *n) { /* a constant whose integer type comes from context */
    switch (n->kind) {
    case N_INT:
        return true;
    case N_UN:
        return (n->op == T_MINUS || n->op == T_TILDE) && is_lit(n->a);
    case N_BIN:
        switch (n->op) {
        case T_PLUS: case T_MINUS: case T_STAR: case T_SLASH: case T_PERCENT:
        case T_AMP: case T_PIPE: case T_CARET: case T_SHL: case T_SHR:
            return is_lit(n->a) && is_lit(n->b);
        default:
            return false;
        }
    case N_CALL:
        return str_is(n->name, "len");
    default:
        return false;
    }
}

static Type *check_int_literal(Node *n, Type *expect) {
    Type *t = (expect && expect->kind == TY_INT) ? expect : ty_i32;
    uint64_t max = t->is_signed ? ((UINT64_C(1) << (t->bits - 1)) - 1)
                                : (t->bits == 64 ? UINT64_MAX : (UINT64_C(1) << t->bits) - 1);
    uint64_t limit = max;
    if (n->op) { /* negated literal */
        if (!t->is_signed && n->ival) die(n->line, "cannot negate a literal of unsigned type '%s'", t->name);
        limit = t->is_signed ? max + 1 : 0;
    }
    if (n->ival > limit)
        die(n->line, "literal %s%llu does not fit in '%s'", n->op ? "-" : "", (unsigned long long)n->ival, t->name);
    return t;
}

/* Can a value of this type cross the C boundary? Scalars and pointers by value, structs by value or by
 * pointer, arrays only by pointer (to their first element). */
static void check_c_signature(Node *fn) {
    for (Node *p = fn->a; p; p = p->next)
        if (p->type->kind == TY_ARRAY && p->op == M_SINK)
            die(p->line, "parameter '%.*s': an array cannot be passed to C by value; use let, inout or set", SF(p->name));
    if (fn->type->kind == TY_ARRAY)
        die(fn->line, "function '%.*s': an array cannot be returned across the C boundary", SF(fn->name));
}

static Type *check_call(Node *n) {
    Node *fn = lookup_fn(n->name);
    if (!fn && g_freestanding && str_is(n->name, "print"))
        die(n->line, "'print' needs a hosted C library (it uses stdio), but this is a freestanding build");
    if (!fn) die(n->line, "unknown function '%.*s'", SF(n->name));
    if (const_ctx)
        die(n->line, "'%.*s' cannot be called here: the value must be known at compile time", SF(n->name));
    if (fn->ext == EXT_EXTERN) {
        char what[160];
        snprintf(what, sizeof what, "calling the extern function '%.*s'", SF(n->name));
        require_unsafe(n, what);
    }
    int want = 0, got = 0;
    for (Node *p = fn->a; p; p = p->next) want++;
    for (Node *a = n->a; a; a = a->next) got++;
    if (want != got)
        die(n->line, "function '%.*s' takes %d argument%s, got %d", SF(n->name), want, want == 1 ? "" : "s", got);
    Node *p = fn->a;
    for (Node *a = n->a; a; a = a->next, p = p->next) {
        Type *at;
        if (p->op == M_INOUT || p->op == M_SET) {
            const char *mode = p->op == M_SET ? "set" : "inout";
            if (a->kind != N_ADDR)
                die(a->line, "argument for %s parameter '%.*s' must be written '&place'", mode, SF(p->name));
            at = check_expr(a->a, p->type);
            require_mutable_place(a->a, p->op == M_SET ? "passed as set" : "passed as inout");
            a->type = at;
        } else {
            if (a->kind == N_ADDR)
                die(a->line, "'&' is only for inout or set parameters, but '%.*s' is not inout or set", SF(p->name));
            at = check_expr(a, p->type);
        }
        expect_type(a->line, p->type, at);
    }
    n->decl = fn;
    return fn->type;
}

static Type *check_expr_inner(Node *n, Type *expect);

/* the raw-pointer operations: each becomes an N_PTROP node */
static Type *check_ptr_intrinsic(Node *n) {
    int argc = 0;
    for (Node *a = n->a; a; a = a->next) argc++;
    bool is_offset = str_is(n->name, "ptr_offset");
    bool is_write = str_is(n->name, "ptr_write") || str_is(n->name, "volatile_write");
    bool is_vol = str_is(n->name, "volatile_read") || str_is(n->name, "volatile_write");
    int want = (is_write || is_offset) ? 2 : 1;
    if (argc != want) die(n->line, "'%.*s' takes %d argument%s", SF(n->name), want, want == 1 ? "" : "s");
    char what[96];
    snprintf(what, sizeof what, "'%.*s'", SF(n->name));
    require_unsafe(n, what);

    Node *p = n->a, *v = p->next;
    p->next = NULL;
    Type *pt = check_expr(p, NULL);
    if (pt->kind != TY_PTR) die(p->line, "'%.*s' needs a pointer, found '%s'", SF(n->name), pt->name);
    Type *pointee = pt->elem;
    if (is_write && !pt->is_mut) die(p->line, "'%.*s' needs a '*mut' pointer, found '%s'", SF(n->name), pt->name);
    if (is_vol && pointee->kind != TY_INT && pointee != ty_bool && pointee->kind != TY_PTR)
        die(p->line, "'%.*s' works on integers, bool and pointers, not '%s'", SF(n->name), pointee->name);
    if (!is_write && !is_offset && !pointee->is_copy)
        die(p->line, "'%.*s' would duplicate a value of type '%s', which is not copy", SF(n->name), pointee->name);

    n->kind = N_PTROP;
    n->a = p;
    n->b = v;
    Type *result;
    if (is_offset) {
        Type *nt = check_expr(v, ty_isize);
        if (nt->kind != TY_INT) die(v->line, "a pointer offset must be an integer, found '%s'", nt->name);
        n->op = PO_OFFSET;
        result = pt;
    } else if (is_write) {
        expect_type(v->line, pointee, check_expr(v, pointee));
        n->op = is_vol ? PO_VWRITE : PO_WRITE;
        result = ty_unit;
    } else {
        n->op = is_vol ? PO_VREAD : PO_READ;
        result = pointee;
    }
    return result;
}

/* len(a) is a compile-time constant; wrapping_add/sub/mul are plain operators with modular results. */
static Type *check_intrinsic(Node *n, Type *expect) {
    if (str_is(n->name, "ptr_read") || str_is(n->name, "ptr_write") || str_is(n->name, "ptr_offset") ||
        str_is(n->name, "volatile_read") || str_is(n->name, "volatile_write"))
        return check_ptr_intrinsic(n);
    int argc = 0;
    for (Node *a = n->a; a; a = a->next) argc++;
    if (str_is(n->name, "len")) {
        if (argc != 1) die(n->line, "'len' takes one argument, an array");
        Node *a = n->a;
        if (!is_place(a)) die(a->line, "'len' takes an array variable, field or element");
        Type *t = check_expr(a, NULL);
        if (t->kind != TY_ARRAY) die(a->line, "'len' needs an array, found '%s'", t->name);
        n->kind = N_INT; /* folded: the array itself is not evaluated */
        n->ival = t->len;
        n->op = 0;
        n->a = NULL;
        return check_int_literal(n, expect);
    }
    if (argc != 2) die(n->line, "'%.*s' takes two arguments", SF(n->name));
    Node *x = n->a, *y = x->next;
    x->next = NULL;
    n->kind = N_BIN;
    n->op = str_is(n->name, "wrapping_add") ? OP_WADD : str_is(n->name, "wrapping_sub") ? OP_WSUB : OP_WMUL;
    n->a = x;
    n->b = y;
    return check_expr_inner(n, expect);
}

static Type *check_expr_inner(Node *n, Type *expect) {
    switch (n->kind) {
    case N_INT:
        return check_int_literal(n, expect);
    case N_BOOL:
        return ty_bool;
    case N_CSTR:
        return ptr_type(ty_u8, false);
    case N_NAME: {
        Local *l = const_ctx ? NULL : find_local(n->name);
        if (l) {
            n->local = l->id;
            n->by_ptr = l->by_ptr;
            n->mut = l->is_mut;
            return l->type;
        }
        Node *c = map_get(&const_map, n->name);
        if (!c && const_ctx && find_local(n->name))
            die(n->line, "'%.*s' is a variable, but a compile-time constant is needed here", SF(n->name));
        if (!c) die(n->line, "unknown name '%.*s'", SF(n->name));
        const_resolve(c);
        n->kind = c->type == ty_bool ? N_BOOL : N_INT; /* a constant is just its value */
        n->ival = c->ival;
        n->op = c->op;
        return c->type;
    }
    case N_MEMBER: {
        Type *st = check_expr(n->a, NULL);
        if (st->kind != TY_STRUCT) die(n->line, "type '%s' has no fields", st->name);
        Node *f = find_field(st->decl, n->name);
        if (!f) die(n->line, "type '%s' has no field '%.*s'", st->name, SF(n->name));
        n->decl = f;
        return f->type;
    }
    case N_CALL:
        return is_intrinsic(n->name) ? check_intrinsic(n, expect) : check_call(n);
    case N_INDEX: {
        if (!is_place(n->a)) die(n->line, "only an array variable, field or element can be indexed");
        Type *at = check_expr(n->a, NULL);
        if (at->kind != TY_ARRAY) die(n->line, "cannot index a value of type '%s'", at->name);
        Type *it = check_expr(n->b, NULL);
        if (it->kind != TY_INT) die(n->b->line, "an array index must be an integer, found '%s'", it->name);
        if (n->b->kind == N_INT && ((n->b->op && n->b->ival) || n->b->ival >= at->len))
            die(n->b->line, "index %s%llu is out of bounds for an array of length %llu", n->b->op ? "-" : "",
                (unsigned long long)n->b->ival, (unsigned long long)at->len);
        return at->elem;
    }
    case N_ARRAY: {
        Type *want = (expect && expect->kind == TY_ARRAY) ? expect->elem : NULL;
        Type *elem = want;
        uint64_t cnt = 0;
        for (Node *e = n->a; e; e = e->next, cnt++) {
            Type *t = check_expr(e, elem);
            if (!elem) elem = t;
            expect_type(e->line, elem, t);
        }
        if (elem == ty_unit) die(n->line, "an array cannot hold 'unit' values");
        return array_type(elem, cnt);
    }
    case N_REPEAT: {
        Type *want = (expect && expect->kind == TY_ARRAY) ? expect->elem : NULL;
        Type *t = check_expr(n->a, want);
        if (want) expect_type(n->a->line, want, t);
        if (t == ty_unit) die(n->line, "an array cannot hold 'unit' values");
        if (!t->is_copy)
            die(n->line, "'[value; N]' copies the value N times, but '%s' is not copy", t->name);
        uint64_t len = const_length(n->b);
        if (len == 0) die(n->line, "an array needs at least one element");
        if (len > 0x7fffffffu) die(n->line, "array length %llu is too large", (unsigned long long)len);
        n->ival = len;
        return array_type(t, len);
    }
    case N_LIT: {
        Node *s = find_struct(n->name);
        if (!s) die(n->line, "unknown struct '%.*s'", SF(n->name));
        for (Node *i = n->a; i; i = i->next) {
            Node *f = find_field(s, i->name);
            if (!f) die(i->line, "struct '%.*s' has no field '%.*s'", SF(s->name), SF(i->name));
            for (Node *j = n->a; j != i; j = j->next)
                if (str_eq(j->name, i->name)) die(i->line, "field '%.*s' given twice", SF(i->name));
            i->decl = f;
            expect_type(i->line, f->type, check_expr(i->a, f->type));
        }
        for (Node *f = s->a; f; f = f->next) {
            bool given = false;
            for (Node *i = n->a; i; i = i->next) given |= str_eq(i->name, f->name);
            if (!given) die(n->line, "missing field '%.*s' in '%.*s' literal", SF(f->name), SF(s->name));
        }
        n->decl = s;
        return s->type;
    }
    case N_CAST: {
        Type *to = resolve_type(n->ty);
        /* an address literal takes the pointer-sized integer type: 0xE000_E010 as *mut u32 */
        Type *from = check_expr(n->a, (to->kind == TY_PTR && is_lit(n->a)) ? ty_usize : NULL);
        bool ok = (from == to) || ((from->kind == TY_INT || from == ty_bool) && to->kind == TY_INT) ||
                  (from->kind == TY_INT && to->kind == TY_PTR) || (from->kind == TY_PTR && to->kind == TY_INT) ||
                  (from->kind == TY_PTR && to->kind == TY_PTR);
        if (!ok) die(n->line, "cannot cast '%s' to '%s'", from->name, to->name);
        return to;
    }
    case N_UN: {
        if (n->op == T_MINUS && n->a->kind == N_INT) { /* fold -literal */
            n->a->op = 1;
            Type *t = check_expr(n->a, expect);
            n->kind = N_INT;
            n->ival = n->a->ival;
            n->op = 1;
            n->a = NULL;
            return t;
        }
        Type *t = check_expr(n->a, n->op == T_BANG ? ty_bool : expect);
        if (n->op == T_BANG) {
            if (t != ty_bool) die(n->line, "'!' needs 'bool', found '%s'", t->name);
        } else if (n->op == T_TILDE) {
            if (t->kind != TY_INT) die(n->line, "'~' needs an integer, found '%s'", t->name);
        } else if (t->kind != TY_INT || !t->is_signed) {
            die(n->line, "unary '-' needs a signed integer, found '%s'", t->name);
        }
        return t;
    }
    case N_ADDR:
        die(n->line, "'&' is only allowed on arguments for inout or set parameters");
    case N_BIN: {
        int op = n->op;
        bool logic = op == T_ANDAND || op == T_OROR;
        bool cmp = op == T_EQ || op == T_NE || op == T_LT || op == T_LE || op == T_GT || op == T_GE;
        Type *ex = logic ? ty_bool : (cmp ? NULL : expect);
        Type *lt, *rt;
        if (is_lit(n->a) && !is_lit(n->b)) { /* let the typed side decide the literal's type */
            rt = check_expr(n->b, ex);
            lt = check_expr(n->a, rt);
        } else {
            lt = check_expr(n->a, ex);
            rt = check_expr(n->b, lt);
        }
        expect_type(n->b->line, lt, rt);
        if (logic) {
            if (lt != ty_bool) die(n->line, "'%s' needs 'bool' operands, found '%s'", op_name(op), lt->name);
            return ty_bool;
        }
        if (op == T_EQ || op == T_NE) {
            if (lt->kind != TY_INT && lt != ty_bool && lt->kind != TY_PTR)
                die(n->line, "cannot compare values of type '%s'", lt->name);
            return ty_bool;
        }
        if (lt->kind != TY_INT) die(n->line, "operator %s needs integer operands, found '%s'", op_name(op), lt->name);
        if ((op == T_SLASH || op == T_PERCENT) && n->b->kind == N_INT && n->b->ival == 0)
            die(n->line, "division by zero");
        if ((op == T_SHL || op == T_SHR) && n->b->kind == N_INT &&
            ((n->b->op && n->b->ival) || n->b->ival >= (uint64_t)lt->bits))
            die(n->line, "shift amount %s%llu is out of range for '%s' (0 to %d)", n->b->op ? "-" : "",
                (unsigned long long)n->b->ival, lt->name, lt->bits - 1);
        return cmp ? ty_bool : lt;
    }
    default:
        die(n->line, "internal error: not an expression");
    }
}

static Type *check_expr(Node *n, Type *expect) {
    n->type = check_expr_inner(n, expect);
    fold(n);
    return n->type;
}

/* A constant is evaluated when first used (or at the end of the declarations), so order does not matter.
 * Its initializer cannot see local variables. */
static void const_resolve(Node *c) {
    if (c->mut == 2) return;
    if (c->mut == 1) die(c->line, "constant '%.*s' is defined in terms of itself", SF(c->name));
    c->mut = 1;
    int saved_unsafe = unsafe_depth;
    unsafe_depth = 0;
    const_ctx++;
    Type *t = resolve_type(c->ty);
    if (t->kind != TY_INT && t != ty_bool)
        die(c->line, "a constant must be an integer or bool, not '%s'", t->name);
    expect_type(c->a->line, t, check_expr(c->a, t));
    if (!is_litnode(c->a))
        die(c->a->line, "the value of constant '%.*s' is not known at compile time", SF(c->name));
    c->type = t;
    c->ival = c->a->ival;
    c->op = c->a->kind == N_INT ? c->a->op : 0;
    const_ctx--;
    unsafe_depth = saved_unsafe;
    c->mut = 2;
}

/* ---- statements ---- */
static void check_stmt(Node *s);

static void check_block(Node *b) {
    int save = nlocals;
    depth++;
    for (Node *s = b->a; s; s = s->next) check_stmt(s);
    depth--;
    nlocals = save;
}

static void check_stmt(Node *s) {
    switch (s->kind) {
    case N_LET: {
        Type *t;
        if (!s->a) { /* declared now, assigned later (var only) */
            if (!s->op)
                die(s->line, "'let' binding '%.*s' needs an initializer (use 'var' to assign it later)", SF(s->name));
            t = resolve_type(s->ty);
        } else if (s->ty) {
            t = resolve_type(s->ty);
            expect_type(s->a->line, t, check_expr(s->a, t));
        } else {
            t = check_expr(s->a, NULL);
        }
        if (t == ty_unit) die(s->line, "cannot store a value of type 'unit' in '%.*s'", SF(s->name));
        s->type = t;
        s->local = declare(s->line, s->name, t, s->op, 0);
        break;
    }
    case N_ASSIGN: {
        if (s->a->kind == N_NAME && !find_local(s->a->name) && map_get(&const_map, s->a->name))
            die(s->a->line, "'%.*s' is a constant and cannot be assigned to", SF(s->a->name));
        Type *lt = check_expr(s->a, NULL);
        require_mutable_place(s->a, "assigned to");
        expect_type(s->b->line, lt, check_expr(s->b, lt));
        break;
    }
    case N_EXPR:
        check_expr(s->a, NULL);
        break;
    case N_IF:
        expect_type(s->a->line, ty_bool, check_expr(s->a, ty_bool));
        check_block(s->b);
        if (s->c) {
            if (s->c->kind == N_IF) check_stmt(s->c);
            else check_block(s->c);
        }
        break;
    case N_WHILE:
        expect_type(s->a->line, ty_bool, check_expr(s->a, ty_bool));
        check_block(s->b);
        break;
    case N_RETURN:
        if (cur_fn->type == ty_unit) {
            if (s->a) die(s->line, "function '%.*s' returns no value", SF(cur_fn->name));
        } else {
            if (!s->a) die(s->line, "function '%.*s' must return '%s'", SF(cur_fn->name), cur_fn->type->name);
            expect_type(s->a->line, cur_fn->type, check_expr(s->a, cur_fn->type));
        }
        break;
    case N_BLOCK:
        if (s->op) unsafe_depth++; /* unsafe { ... } */
        check_block(s);
        if (s->op) unsafe_depth--;
        break;
    default:
        die(s->line, "internal error: not a statement");
    }
}

static bool always_returns(Node *s) {
    switch (s->kind) {
    case N_RETURN: return true;
    case N_BLOCK:
        for (Node *c = s->a; c; c = c->next)
            if (always_returns(c)) return true;
        return false;
    case N_IF: return s->c && always_returns(s->b) && always_returns(s->c);
    default: return false;
    }
}

/* ---- program ---- */
static void visit_struct(Node *s) { /* reject structs that contain themselves by value */
    if (s->ival == 1) die(s->line, "struct '%.*s' contains itself", SF(s->name));
    if (s->ival == 2) return;
    s->ival = 1;
    for (Node *f = s->a; f; f = f->next) {
        Type *ft = f->type;
        while (ft->kind == TY_ARRAY) ft = ft->elem; /* an array of S inside S is just as infinite */
        if (ft->kind == TY_STRUCT) visit_struct(ft->decl);
    } /* a pointer to S inside S is fine: it has a fixed size */
    s->ival = 2;
}

/* Flatten a struct into scalar leaves; fields remember where theirs start. */
static int layout_type(Type *t) {
    if (t->nleaves) return t->nleaves;
    int off = 0;
    for (Node *f = t->decl->a; f; f = f->next) {
        f->leaf_off = off;
        off += layout_type(f->type);
    }
    return t->nleaves = off;
}

static void check_fn(Node *fn) {
    cur_fn = fn;
    nlocals = 0;
    depth = 1;
    next_id = 0;
    unsafe_depth = 0;
    for (Node *p = fn->a; p; p = p->next)
        p->local = declare(p->line, p->name, p->type, p->op == M_INOUT || p->op == M_SET, p->op == M_INOUT || p->op == M_SET);
    check_block(fn->b);
    fn->nlocals = next_id;
    if (fn->type != ty_unit && !always_returns(fn->b))
        die(fn->line, "function '%.*s' does not return a value on every path", SF(fn->name));
}

void check_program(Node *program) {
    prog = program;
    prims[10].bits = prims[11].bits = g_ptr_bits; /* usize / isize */

    builtin_print.kind = N_FN;
    builtin_print.name = (Str){"print", 5};
    builtin_print.type = ty_unit;
    builtin_print.ival = 1; /* builtin */
    builtin_print_param.kind = N_PARAM;
    builtin_print_param.name = (Str){"x", 1};
    builtin_print_param.type = ty_i32;
    builtin_print.a = &builtin_print_param;

    /* 1. declare struct, function and constant names */
    size_t nstructs = 0, nfns = 0, nconsts = 0;
    for (Node *d = prog; d; d = d->next) {
        if (d->kind == N_STRUCT) nstructs++;
        else if (d->kind == N_CONST) nconsts++;
        else nfns++;
    }
    map_init(&struct_map, nstructs);
    map_init(&fn_map, nfns);
    map_init(&const_map, nconsts);
    for (Node *d = prog; d; d = d->next) {
        Map *m = d->kind == N_STRUCT ? &struct_map : d->kind == N_CONST ? &const_map : &fn_map;
        if (!map_put(m, d->name, d)) die(d->line, "'%.*s' is defined twice", SF(d->name));
        if (d->kind == N_STRUCT) {
            for (int i = 0; i < NPRIMS; i++)
                if (str_is(d->name, prims[i].name)) die(d->line, "'%.*s' is a builtin type", SF(d->name));
            Type *t = ARENA_NEW(&g_arena, Type);
            t->kind = TY_STRUCT;
            t->name = arena_strn(&g_arena, d->name.p, d->name.n);
            char *cn = arena_alloc(&g_arena, (size_t)d->name.n + 3);
            memcpy(cn, "s_", 2);
            memcpy(cn + 2, d->name.p, (size_t)d->name.n);
            t->cname = cn;
            t->decl = d;
            t->is_copy = d->op != 0;
            d->type = t;
        } else if (d->kind == N_FN && (str_eq(d->name, builtin_print.name) || is_intrinsic(d->name))) {
            die(d->line, "'%.*s' is a builtin function", SF(d->name));
        }
    }

    /* 2. struct fields */
    for (Node *d = prog; d; d = d->next) {
        if (d->kind != N_STRUCT) continue;
        for (Node *f = d->a; f; f = f->next) {
            for (Node *g = d->a; g != f; g = g->next)
                if (str_eq(g->name, f->name)) die(f->line, "field '%.*s' declared twice", SF(f->name));
            f->type = resolve_type(f->ty);
        }
    }
    for (Node *d = prog; d; d = d->next)
        if (d->kind == N_STRUCT) visit_struct(d);
    for (Node *d = prog; d; d = d->next) {
        if (d->kind != N_STRUCT) continue;
        layout_type(d->type);
        if (!d->type->is_copy) continue;
        for (Node *f = d->a; f; f = f->next)
            if (!f->type->is_copy)
                die(f->line, "field '%.*s' of copy struct '%.*s' has type '%s', which is not copy",
                    SF(f->name), SF(d->name), f->type->name);
    }

    /* 3. constants, then function signatures */
    for (Node *d = prog; d; d = d->next)
        if (d->kind == N_CONST) const_resolve(d);
    Node *main_fn = NULL;
    for (Node *d = prog; d; d = d->next) {
        if (d->kind != N_FN) continue;
        for (Node *p = d->a; p; p = p->next) {
            for (Node *q = d->a; q != p; q = q->next)
                if (str_eq(q->name, p->name)) die(p->line, "parameter '%.*s' declared twice", SF(p->name));
            p->type = resolve_type(p->ty);
        }
        d->type = d->ty ? resolve_type(d->ty) : ty_unit;
        if (d->ext != EXT_NONE) check_c_signature(d);
        if (str_is(d->name, "main")) main_fn = d;
    }
    if (!main_fn) die(1, "no 'main' function");
    if (main_fn->a || main_fn->type != ty_i32 || main_fn->ext != EXT_NONE)
        die(main_fn->line, "'main' must be declared as 'fn main() -> i32'");

    /* 4. bodies (extern functions have none) */
    for (Node *d = prog; d; d = d->next)
        if (d->kind == N_FN && d->ext != EXT_EXTERN) check_fn(d);
}
