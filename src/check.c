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
static Type *const ty_i64 = &prims[5];

Type *g_array_types; /* every array type, in creation order */
Type *g_ptr_types;   /* every raw-pointer type, in creation order */
bool g_freestanding;
int g_ptr_bits = (int)(sizeof(void *) * 8);

static Node *prog;
static Node builtin_print, builtin_print_param;

/* ---- name tables: structs, functions and constants are looked up by name all the time ---- */
typedef struct { Str key; Node *val; } Slot;
typedef struct { Slot *slots; size_t cap; } Map;
typedef struct { Map types, fns, consts; } ModScope; /* the names a module declares */
static ModScope *scopes;
static int cur_mod; /* the module whose names the code being checked sees */
static void enter_module(int m) {
    cur_mod = m;
    g_file = g_modules[m]->path;
}

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

/* every local of the function being checked, by id: the flow pass reads this instead of searching the tree */
static LocalInfo *fn_locals;
static int fn_locals_cap;

static int declare(int line, Str name, Type *t, int is_mut, int by_ptr, int kind) {
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
    if (l->id >= fn_locals_cap) {
        int cap = fn_locals_cap ? fn_locals_cap * 2 : 64;
        LocalInfo *bigger = arena_alloc(&g_arena, sizeof(LocalInfo) * (size_t)cap);
        if (fn_locals) memcpy(bigger, fn_locals, sizeof(LocalInfo) * (size_t)fn_locals_cap);
        fn_locals = bigger;
        fn_locals_cap = cap;
    }
    fn_locals[l->id].name = name;
    fn_locals[l->id].type = t;
    fn_locals[l->id].kind = kind;
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

/* A discriminant: a constant integer expression, possibly negative. */
static int64_t const_int_value(Node *e) {
    int saved_unsafe = unsafe_depth;
    unsafe_depth = 0;
    const_ctx++;
    Type *t = check_expr(e, ty_i64);
    const_ctx--;
    unsafe_depth = saved_unsafe;
    if (e->kind != N_INT || t->kind != TY_INT) die(e->line, "a discriminant must be a compile-time integer constant");
    if (e->op) return (int64_t)((uint64_t)0 - e->ival);
    if (e->ival > (uint64_t)INT64_MAX) die(e->line, "a discriminant must fit in a signed 64-bit integer");
    return (int64_t)e->ival;
}

/* ---- types ---- */
/* ---- scopes: what a name means depends on the module it is written in ----
 * A module sees its own declarations, then those of the modules it imports without a name (public ones only; the
 * standard prelude last, so anything else beats it), and reaches the others as name::item. */
enum { K_TYPE, K_FN, K_CONST };
static Map *scope_map(Module *m, int kind) {
    ModScope *s = &scopes[m->id];
    return kind == K_TYPE ? &s->types : kind == K_FN ? &s->fns : &s->consts;
}

static Module *find_alias(Str alias) {
    for (Import *i = g_modules[cur_mod]->imports; i; i = i->next)
        if (i->alias.n && str_eq(i->alias, alias)) return i->mod;
    return NULL;
}

static Node *scope_find(int kind, Str qual, Str name, int line) {
    if (qual.n) {
        Module *m = find_alias(qual);
        if (!m) die(line, "unknown module '%.*s'", SF(qual));
        Node *d = map_get(scope_map(m, kind), name);
        if (d && !d->pub) die(line, "'%.*s' is private to module '%.*s': declare it 'pub' there", SF(name), SF(qual));
        return d;
    }
    Node *own = map_get(scope_map(g_modules[cur_mod], kind), name);
    if (own) return own;
    Node *found = NULL, *from_prelude = NULL;
    for (Import *i = g_modules[cur_mod]->imports; i; i = i->next) {
        if (i->alias.n) continue;
        Node *d = map_get(scope_map(i->mod, kind), name);
        if (!d || !d->pub) continue;
        if (i->prelude) from_prelude = d;
        else if (found && found != d) die(line, "'%.*s' comes from two imported modules; write module::%.*s", SF(name), SF(name));
        else found = d;
    }
    return found ? found : from_prelude;
}

/* structs and enums share one namespace */
static Node *find_decl(Str qual, Str name, int line) { return scope_find(K_TYPE, qual, name, line); }
static Node *find_struct(Str qual, Str name, int line) {
    Node *d = find_decl(qual, name, line);
    return d && d->kind == N_STRUCT ? d : NULL;
}
static Node *find_enum(Str qual, Str name, int line) {
    Node *d = find_decl(qual, name, line);
    return d && d->kind == N_ENUM ? d : NULL;
}
static Node *find_variant(Node *en, Str name) {
    for (Node *v = en->a; v; v = v->next)
        if (str_eq(v->name, name)) return v;
    return NULL;
}
static int list_len(Node *l) {
    int n = 0;
    for (; l; l = l->next) n++;
    return n;
}
/* an enum whose variants carry no values: a plain list of named integers */
static bool enum_fieldless(Type *t) {
    for (Node *v = t->decl->a; v; v = v->next)
        if (v->a) return false;
    return true;
}

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
    snprintf(cname, n, "arr_%s_%llu", (elem->kind == TY_INT || elem->kind == TY_BOOL) ? elem->name : elem->cname,
             (unsigned long long)len);
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
             (pointee->kind == TY_INT || pointee->kind == TY_BOOL || pointee->kind == TY_UNIT) ? pointee->name : pointee->cname);
    t->name = name;
    t->cname = cname;
    *tail = t;
    return t;
}

static Type *resolve_type(Node *ty);
/* ================================================================ generics ==
 * A generic struct, enum or function is a template: it is never checked itself. Each use with particular
 * arguments makes an instance, a copy of the declaration with the parameters replaced, which is registered and
 * checked exactly like hand-written code. So every safety check runs on every instance, and a generic function
 * that nothing uses is not checked at all. Instances are appended to the program, after the declarations. */
#define MAX_INSTS 20000
#define MAX_INST_DEPTH 48
static Node *prog_tail;   /* the last declaration: instances are appended here */
static int n_insts, inst_depth;
static Node *pending[MAX_INSTS]; /* struct and enum instances made but not yet checked for containing themselves */
static int npending;

static void resolve_enum(Node *d);
static void visit_decl(Node *s);
static int layout_type(Type *t);
static void expect_type(int line, Type *want, Type *got);
static bool is_lit(Node *n);
static uint64_t mask_bits(int bits);
static int64_t sext(uint64_t v, int bits);
static int64_t min_signed(int bits);
static int64_t max_signed(int bits);

static void *gzalloc(size_t n) { return arena_alloc(&g_arena, n ? n : 1); }

static int gen_index(Node *gen, Str name) {
    int i = 0;
    for (Node *p = gen; p; p = p->next, i++)
        if (str_eq(p->name, name)) return i;
    return -1;
}

typedef struct {
    Node *gen;       /* the parameters being replaced */
    Type **types;    /* per parameter: its type argument, or NULL for a const parameter */
    uint64_t *vals;  /* per parameter: the value of a const argument */
    Type **ctypes;   /* per parameter: the type of a const parameter */
} Subst;

static Node *mk_node(NodeKind k, int line) {
    Node *n = ARENA_NEW(&g_arena, Node);
    n->kind = k;
    n->line = line;
    n->local = -1;
    return n;
}

/* a type that is already known, standing where a written type would be */
static Node *mk_type_ref(int line, Type *t) {
    Node *n = mk_node(N_TYPE, line);
    n->type = t;
    return n;
}

/* a constant with a type of its own (flag): it does not take its type from the context like a plain literal */
static Node *mk_const_lit(int line, Type *t, uint64_t bits) {
    Node *n = mk_node(N_INT, line);
    n->flag = 1;
    n->type = t;
    int64_t v = t->is_signed ? sext(bits, t->bits) : 0;
    if (t->is_signed && v < 0) {
        n->op = 1;
        n->ival = (uint64_t)0 - (uint64_t)v;
    } else {
        n->ival = t->is_signed ? (uint64_t)v : (bits & mask_bits(t->bits));
    }
    return n;
}

static Node *clone_list(Node *n, const Subst *s);

/* A copy of n (not of its siblings), with the generic parameters of s replaced by their arguments. */
static Node *clone_node(Node *n, const Subst *s) {
    int k;
    if (n->kind == N_TYPE && n->op == 0 && !n->gen && !n->type && (k = gen_index(s->gen, n->name)) >= 0)
        return s->types[k] ? mk_type_ref(n->line, s->types[k]) : mk_const_lit(n->line, s->ctypes[k], s->vals[k]);
    if (n->kind == N_NAME && (k = gen_index(s->gen, n->name)) >= 0 && !s->types[k])
        return mk_const_lit(n->line, s->ctypes[k], s->vals[k]);
    if ((n->kind == N_LET || n->kind == N_PARAM || (n->kind == N_PAT && n->op == PAT_BIND)) &&
        (k = gen_index(s->gen, n->name)) >= 0 && !s->types[k])
        die(n->line, "'%.*s' is a const parameter of this declaration, so it cannot also name a variable", SF(n->name));
    Node *c = ARENA_NEW(&g_arena, Node);
    *c = *n;
    c->next = NULL;
    c->a = clone_list(n->a, s);
    c->b = clone_list(n->b, s);
    c->c = clone_list(n->c, s);
    c->ty = clone_list(n->ty, s);
    c->gen = clone_list(n->gen, s);
    return c;
}

static Node *clone_list(Node *n, const Subst *s) {
    Node *head = NULL, **tail = &head;
    for (; n; n = n->next) {
        *tail = clone_node(n, s);
        tail = &(*tail)->next;
    }
    return head;
}

/* the instance of a declaration: its parts are copies, and it is no longer generic */
static Node *clone_decl(Node *tpl, const Subst *s) {
    Node *c = ARENA_NEW(&g_arena, Node);
    *c = *tpl;
    c->next = NULL;
    c->gen = NULL;
    c->inst = NULL;
    c->a = clone_list(tpl->a, s);
    c->b = clone_list(tpl->b, s);
    c->c = clone_list(tpl->c, s);
    c->ty = clone_list(tpl->ty, s);
    return c;
}

/* does the tree mention this name (as a type or as a value)? */
static bool mentions(Node *n, Str name);
static bool mentions_list(Node *n, Str name) {
    for (; n; n = n->next)
        if (mentions(n, name)) return true;
    return false;
}
static bool mentions(Node *n, Str name) {
    if ((n->kind == N_NAME || (n->kind == N_TYPE && n->op == 0 && !n->type)) && str_eq(n->name, name)) return true;
    return mentions_list(n->a, name) || mentions_list(n->b, name) || mentions_list(n->c, name) ||
           mentions_list(n->ty, name) || mentions_list(n->gen, name);
}

/* ---- names for messages ---- */
typedef struct { char *p; size_t n, cap; } Sb;
static void sb_add(Sb *b, const char *s) {
    size_t l = strlen(s);
    if (b->n + l + 1 > b->cap) {
        b->cap = (b->n + l + 1) * 2;
        b->p = realloc(b->p, b->cap);
        if (!b->p) die(1, "out of memory");
    }
    memcpy(b->p + b->n, s, l + 1);
    b->n += l;
}

static const char *inst_display(Node *tpl, Type **types, uint64_t *vals, Type **ctypes, int line) {
    Sb b = {0};
    char num[40];
    sb_add(&b, arena_strn(&g_arena, tpl->name.p, tpl->name.n));
    sb_add(&b, "<");
    int i = 0;
    for (Node *p = tpl->gen; p; p = p->next, i++) {
        if (i) sb_add(&b, ", ");
        if (types[i]) {
            sb_add(&b, types[i]->name);
        } else if (ctypes[i]->is_signed) {
            snprintf(num, sizeof num, "%lld", (long long)sext(vals[i], ctypes[i]->bits));
            sb_add(&b, num);
        } else {
            snprintf(num, sizeof num, "%llu", (unsigned long long)vals[i]);
            sb_add(&b, num);
        }
    }
    sb_add(&b, ">");
    if (b.n > 2000)
        die(line, "the type '%.40s...' is too big to name (more than 2000 characters): does a generic function "
                  "call itself with a bigger type each time?", b.p);
    const char *r = arena_strn(&g_arena, b.p, (int)b.n);
    free(b.p);
    return r;
}

/* ---- the instances of one declaration ---- */
static Inst *find_inst(Node *tpl, Type **types, uint64_t *vals, int n) {
    for (Inst *r = tpl->inst; r; r = r->next) {
        bool same = true;
        for (int k = 0; k < n && same; k++)
            same = r->types[k] == types[k] && (types[k] || r->vals[k] == vals[k]);
        if (same) return r;
    }
    return NULL;
}

static Inst *new_inst(Node *tpl, Type **types, uint64_t *vals, Type **ctypes, int n, int line) {
    if (n_insts >= MAX_INSTS)
        die(line, "more than %d generic instances: does a generic function call itself with a bigger type each time?",
            MAX_INSTS);
    if (inst_depth >= MAX_INST_DEPTH) die(line, "generic instances nest more than %d deep", MAX_INST_DEPTH);
    Inst *r = ARENA_NEW(&g_arena, Inst);
    r->origin = tpl;
    r->n = n;
    r->types = gzalloc(sizeof(Type *) * (size_t)n);
    r->vals = gzalloc(sizeof(uint64_t) * (size_t)n);
    r->ctypes = gzalloc(sizeof(Type *) * (size_t)n);
    memcpy(r->types, types, sizeof(Type *) * (size_t)n);
    memcpy(r->vals, vals, sizeof(uint64_t) * (size_t)n);
    memcpy(r->ctypes, ctypes, sizeof(Type *) * (size_t)n);
    r->id = ++n_insts;
    r->line = line;
    r->name = inst_display(tpl, types, vals, ctypes, line);
    r->next = tpl->inst;
    tpl->inst = r;
    return r;
}

/* Instances made while others are still being built wait until the outermost one is done: only then are all
 * the field types they depend on known, so they can be checked for containing themselves and laid out. */
static void finish_pending(void) {
    int n = npending;
    npending = 0;
    int saved_mod = cur_mod;
    for (int i = 0; i < n; i++) {
        enter_module(pending[i]->mod);
        visit_decl(pending[i]);
    }
    enter_module(saved_mod);
    for (int i = 0; i < n; i++)
        if (pending[i]->kind == N_STRUCT) layout_type(pending[i]->type);
}

static Type *instantiate_type(Node *tpl, Type **types, uint64_t *vals, Type **ctypes, int line) {
    int n = list_len(tpl->gen);
    Inst *r = find_inst(tpl, types, vals, n);
    if (r) return r->decl->type;
    r = new_inst(tpl, types, vals, ctypes, n, line);
    Subst s = {tpl->gen, types, vals, ctypes};
    Node *d = clone_decl(tpl, &s);
    d->inst = r;
    r->decl = d;
    bool is_enum = tpl->kind == N_ENUM;
    Type *t = ARENA_NEW(&g_arena, Type);
    t->kind = is_enum ? TY_ENUM : TY_STRUCT;
    t->nleaves = is_enum ? 1 : 0;
    t->name = r->name;
    char *cn = arena_alloc(&g_arena, (size_t)tpl->name.n + 24);
    snprintf(cn, (size_t)tpl->name.n + 24, "%c%d_%.*s", is_enum ? 'e' : 's', r->id, SF(tpl->name));
    t->cname = cn;
    t->decl = d;
    t->is_copy = d->op != 0;
    d->type = t;
    prog_tail->next = d;
    prog_tail = d;

    note_push("in '%s', which was asked for on line %d", note_name(r->name), line);
    inst_depth++;
    int saved_mod = cur_mod;
    enter_module(tpl->mod);
    if (is_enum) {
        resolve_enum(d);
    } else {
        for (Node *f = d->a; f; f = f->next) {
            for (Node *g = d->a; g != f; g = g->next)
                if (str_eq(g->name, f->name)) die(f->line, "field '%.*s' declared twice", SF(f->name));
            f->type = resolve_type(f->ty);
        }
    }
    /* a generic type is copy when its declaration says so and every value it holds is copy */
    bool all_copy = true;
    if (is_enum) {
        for (Node *v = d->a; v; v = v->next)
            for (Node *f = v->a; f; f = f->next) all_copy = all_copy && f->type->is_copy;
    } else {
        for (Node *f = d->a; f; f = f->next) all_copy = all_copy && f->type->is_copy;
    }
    t->is_copy = d->op != 0 && all_copy;
    enter_module(saved_mod);
    pending[npending++] = d;
    inst_depth--;
    note_pop();
    if (inst_depth == 0) finish_pending();
    return t;
}

static Node *instantiate_fn(Node *tpl, Type **types, uint64_t *vals, Type **ctypes, int line) {
    int n = list_len(tpl->gen);
    Inst *r = find_inst(tpl, types, vals, n);
    if (r) return r->decl;
    r = new_inst(tpl, types, vals, ctypes, n, line);
    Subst s = {tpl->gen, types, vals, ctypes};
    Node *d = clone_decl(tpl, &s);
    d->inst = r;
    r->decl = d;
    note_push("in '%s', which was asked for on line %d", note_name(r->name), line);
    inst_depth++;
    int saved_mod = cur_mod;
    enter_module(tpl->mod);
    for (Node *p = d->a; p; p = p->next) {
        for (Node *q = d->a; q != p; q = q->next)
            if (str_eq(q->name, p->name)) die(p->line, "parameter '%.*s' declared twice", SF(p->name));
        p->type = resolve_type(p->ty);
    }
    d->type = d->ty ? resolve_type(d->ty) : ty_unit;
    enter_module(saved_mod);
    inst_depth--;
    note_pop();
    prog_tail->next = d;
    prog_tail = d;
    if (inst_depth == 0) finish_pending();
    return d;
}

/* ---- working out the arguments ---- */
typedef struct {
    Node *tpl;
    int n;
    Type **types;
    uint64_t *vals;
    Type **ctypes;
    bool *bound;
} Bind;

static void bind_init(Bind *b, Node *tpl) {
    b->tpl = tpl;
    b->n = list_len(tpl->gen);
    b->types = gzalloc(sizeof(Type *) * (size_t)b->n);
    b->vals = gzalloc(sizeof(uint64_t) * (size_t)b->n);
    b->ctypes = gzalloc(sizeof(Type *) * (size_t)b->n);
    b->bound = gzalloc((size_t)b->n);
    int i = 0, saved_mod = cur_mod;
    enter_module(tpl->mod);
    for (Node *p = tpl->gen; p; p = p->next, i++) {
        if (p->op != 1) continue;
        Type *t = resolve_type(p->ty);
        if (t->kind != TY_INT)
            die(p->line, "const parameter '%.*s' must have an integer type, not '%s'", SF(p->name), t->name);
        b->ctypes[i] = t;
    }
    enter_module(saved_mod);
}

static Str param_name(Bind *b, int k) {
    Node *p = b->tpl->gen;
    while (k-- > 0) p = p->next;
    return p->name;
}

/* an argument that is a type parameter's name or a const parameter's name: which one is it? */
static int param_of(Bind *b, Node *n) {
    if ((n->kind == N_NAME || (n->kind == N_TYPE && n->op == 0 && !n->gen && !n->type))) return gen_index(b->tpl->gen, n->name);
    return -1;
}

static void bind_type(Bind *b, int k, Type *t, int line, bool soft) {
    if (b->bound[k]) {
        if (!soft && b->types[k] != t)
            die(line, "'%.*s' is both '%s' and '%s'", SF(param_name(b, k)), b->types[k]->name, t->name);
        return;
    }
    b->types[k] = t;
    b->bound[k] = true;
}

static void bind_val(Bind *b, int k, uint64_t v, int line, bool soft) {
    if (b->bound[k]) {
        if (!soft && b->vals[k] != v)
            die(line, "'%.*s' is both %llu and %llu", SF(param_name(b, k)), (unsigned long long)b->vals[k], (unsigned long long)v);
        return;
    }
    Type *ct = b->ctypes[k];
    bool fits = ct->is_signed ? ((int64_t)v >= min_signed(ct->bits) && (int64_t)v <= max_signed(ct->bits))
                              : v <= mask_bits(ct->bits);
    if (!fits)
        die(line, "%llu does not fit in '%s', the type of const parameter '%.*s'", (unsigned long long)v, ct->name,
            SF(param_name(b, k)));
    b->vals[k] = v;
    b->bound[k] = true;
}

/* Match the written type `pat` (in the template, with its parameters in it) against a real type, and bind the
 * parameters it names. `soft`: only fill parameters that are still open. */
static void unify_r(Bind *b, Node *pat, Type *actual, bool soft) {
    if (pat->kind != N_TYPE || pat->type) return;
    if (pat->op == 1) { /* [X; N] */
        if (actual->kind != TY_ARRAY) return;
        unify_r(b, pat->a, actual->elem, soft);
        int k = param_of(b, pat->b);
        if (k >= 0 && b->ctypes[k]) bind_val(b, k, actual->len, pat->line, soft);
        return;
    }
    if (pat->op == 2) { /* *mut X */
        if (actual->kind == TY_PTR) unify_r(b, pat->a, actual->elem, soft);
        return;
    }
    int k = pat->gen ? -1 : gen_index(b->tpl->gen, pat->name);
    if (k >= 0) {
        if (!b->ctypes[k]) bind_type(b, k, actual, pat->line, soft);
        return;
    }
    if (pat->gen) { /* Name<args>: an instance of that generic type */
        Node *g = find_decl(pat->tyname, pat->name, pat->line);
        Inst *r = actual->decl ? actual->decl->inst : NULL;
        if (!g || !g->gen || !r || r->origin != g) return;
        int i = 0;
        for (Node *a = pat->gen; a && i < r->n; a = a->next, i++) {
            if (r->types[i]) {
                unify_r(b, a, r->types[i], soft);
            } else {
                int k2 = param_of(b, a);
                if (k2 >= 0 && b->ctypes[k2]) bind_val(b, k2, r->vals[i], a->line, soft);
            }
        }
    }
}

static void unify(Bind *b, Node *pat, Type *actual, bool soft) {
    int saved_mod = cur_mod;
    enter_module(b->tpl->mod); /* the names in the template's types are the template's module's */
    unify_r(b, pat, actual, soft);
    enter_module(saved_mod);
}

static Type *type_arg(Node *a) {
    if (a->kind == N_INT) die(a->line, "expected a type, found a number");
    return resolve_type(a);
}

/* the value of a const argument: a number or the name of a constant, of the const parameter's type */
static uint64_t const_arg_value(Node *arg, Type *pt) {
    Node *e = ARENA_NEW(&g_arena, Node);
    *e = *arg;
    e->next = NULL;
    if (arg->kind == N_TYPE) {
        if (arg->op != 0 || arg->gen || arg->type) die(arg->line, "expected a constant number here, found a type");
        if (!scope_find(K_CONST, (Str){0}, arg->name, arg->line))
            die(arg->line, "expected a constant number here, but '%.*s' is not a constant", SF(arg->name));
        e->kind = N_NAME;
    }
    int saved = unsafe_depth;
    unsafe_depth = 0;
    const_ctx++;
    Type *t = check_expr(e, pt);
    const_ctx--;
    unsafe_depth = saved;
    if (e->kind != N_INT) die(arg->line, "a generic argument for a const parameter must be a compile-time constant");
    expect_type(arg->line, pt, t);
    return e->op ? (uint64_t)0 - e->ival : e->ival;
}

static void apply_explicit(Bind *b, Node *args, int line) {
    int given = list_len(args);
    if (given > b->n)
        die(line, "'%.*s' takes %d generic argument%s, but %d given", SF(b->tpl->name), b->n, b->n == 1 ? "" : "s", given);
    int k = 0;
    for (Node *a = args; a; a = a->next, k++) {
        if (b->ctypes[k]) bind_val(b, k, const_arg_value(a, b->ctypes[k]), a->line, false);
        else bind_type(b, k, type_arg(a), a->line, false);
    }
}

static void bind_from_inst(Bind *b, Inst *r) {
    for (int k = 0; k < b->n && k < r->n; k++) {
        if (r->types[k]) bind_type(b, k, r->types[k], 0, true);
        else bind_val(b, k, r->vals[k], 0, true);
    }
}

/* The real type for a written type of the template, if every parameter it names is known yet. */
static Type *subst_type(Bind *b, Node *pat) {
    int k = 0;
    for (Node *p = b->tpl->gen; p; p = p->next, k++)
        if (!b->bound[k] && mentions(pat, p->name)) return NULL;
    Subst s = {b->tpl->gen, b->types, b->vals, b->ctypes};
    int saved_mod = cur_mod;
    enter_module(b->tpl->mod); /* the names in the template's types are the template's module's */
    Type *t = resolve_type(clone_node(pat, &s));
    enter_module(saved_mod);
    return t;
}

/* Check the values of a call, struct literal or variant while working out the parameters from their types.
 * pats[i] is the written type the template gives value i, exprs[i] the expression, and pre[i] gets its type.
 * Values that are plain numbers go last, so another value can decide their type: f(1, x) with x a u8. */
static void infer_values(Bind *b, Node **pats, Node **exprs, int nv, Type **pre, Node *ret_pat, Type *expect) {
    if (ret_pat && expect) unify(b, ret_pat, expect, true); /* the type the result is wanted as fills what is still open */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < nv; i++) {
            if (is_lit(exprs[i]) != (pass == 1)) continue;
            Type *want = subst_type(b, pats[i]);
            pre[i] = check_expr(exprs[i], want);
            if (!want) unify(b, pats[i], pre[i], false);
        }
    }
}

static void require_bound(Bind *b, int line, const char *how) {
    int k = 0;
    for (Node *p = b->tpl->gen; p; p = p->next, k++)
        if (!b->bound[k])
            die(line, "cannot work out the %s '%.*s' of '%.*s': %s", p->op ? "const parameter" : "type parameter",
                SF(p->name), SF(b->tpl->name), how);
}

/* the struct or enum a literal or variant makes, when the declaration is generic */
static Type *infer_instance(Node *tpl, Node *targs, int line, Type *expect, Node **pats, Node **exprs, int nv, Type **pre) {
    Bind b;
    bind_init(&b, tpl);
    apply_explicit(&b, targs, line);
    if (expect && expect->decl && expect->decl->inst && expect->decl->inst->origin == tpl) bind_from_inst(&b, expect->decl->inst);
    infer_values(&b, pats, exprs, nv, pre, NULL, NULL);
    require_bound(&b, line, "write the arguments out, as in 'Name::<...>', or give the variable its type");
    return instantiate_type(tpl, b.types, b.vals, b.ctypes, line);
}

/* A written type with generic arguments: Pair<i32, bool>, Vec<u8, 8>. */
static Type *instantiate_written(Node *tpl, Node *ty) {
    int np = list_len(tpl->gen), na = list_len(ty->gen);
    if (na != np)
        die(ty->line, "'%.*s' takes %d generic argument%s, but %d given", SF(tpl->name), np, np == 1 ? "" : "s", na);
    Bind b;
    bind_init(&b, tpl);
    apply_explicit(&b, ty->gen, ty->line);
    return instantiate_type(tpl, b.types, b.vals, b.ctypes, ty->line);
}

static Type *resolve_type(Node *ty) {
    if (ty->kind != N_TYPE) die(ty->line, "expected a type, found a number");
    if (ty->type) return ty->type; /* a generic parameter that has been replaced by its argument */
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
    Node *s = find_decl(ty->tyname, ty->name, ty->line);
    if (s) {
        if (s->gen) {
            if (!ty->gen) die(ty->line, "'%.*s' is generic: write its arguments, as in %.*s<...>", SF(s->name), SF(s->name));
            return instantiate_written(s, ty);
        }
        if (ty->gen) die(ty->line, "'%.*s' is not generic, so it takes no type arguments", SF(s->name));
        return s->type;
    }
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

static Node *lookup_fn(Str qual, Str name, int line) {
    Node *f = scope_find(K_FN, qual, name, line);
    if (f) return f;
    if (!qual.n && !g_freestanding && str_eq(name, builtin_print.name)) return &builtin_print;
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
        return !n->flag; /* a const generic argument has a type of its own */
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

/* Does a value of this type contain an enum (itself, or in a field or element)? */
static bool holds_enum(const Type *t) {
    switch (t->kind) {
    case TY_ENUM:
        return true;
    case TY_ARRAY:
        return holds_enum(t->elem);
    case TY_STRUCT:
        for (Node *f = t->decl->a; f; f = f->next)
            if (holds_enum(f->type)) return true;
        return false;
    default:
        return false;
    }
}

/* Can a value of this type cross the C boundary? Scalars and pointers by value, structs by value or by
 * pointer, arrays only by pointer (to their first element). An enum never does, not even inside a struct:
 * C code could write a tag that names no variant, and match would then read the wrong values. */
static void check_c_signature(Node *fn) {
    for (Node *p = fn->a; p; p = p->next)
        if (holds_enum(p->type))
            die(p->line, "parameter '%.*s': an enum cannot cross the C boundary (nor can a struct or array holding one); "
                         "pass an integer (value as u32)", SF(p->name));
    if (holds_enum(fn->type))
        die(fn->line, "function '%.*s': an enum cannot cross the C boundary (nor can a struct or array holding one); "
                      "return an integer (value as u32)", SF(fn->name));
    for (Node *p = fn->a; p; p = p->next)
        if (p->type->kind == TY_ARRAY && p->op == M_SINK)
            die(p->line, "parameter '%.*s': an array cannot be passed to C by value; use let, inout or set", SF(p->name));
    if (fn->type->kind == TY_ARRAY)
        die(fn->line, "function '%.*s': an array cannot be returned across the C boundary", SF(fn->name));
}

/* A call of a generic function: work out the instance from the arguments (and the expected type), and say what
 * type each argument turned out to have, so that they are not checked a second time. */
static Node *infer_call(Node *tpl, Node *n, Type *expect, Type ***pre_out) {
    int nv = list_len(n->a), np = list_len(tpl->a);
    if (np != nv)
        die(n->line, "function '%.*s' takes %d argument%s, got %d", SF(n->name), np, np == 1 ? "" : "s", nv);
    Bind b;
    bind_init(&b, tpl);
    apply_explicit(&b, n->gen, n->line);
    Node **pats = gzalloc(sizeof(Node *) * (size_t)nv), **exprs = gzalloc(sizeof(Node *) * (size_t)nv);
    Type **pre = gzalloc(sizeof(Type *) * (size_t)nv);
    int i = 0;
    for (Node *p = tpl->a, *a = n->a; a; p = p->next, a = a->next, i++) {
        pats[i] = p->ty;
        if (p->op == M_INOUT || p->op == M_SET) {
            if (a->kind != N_ADDR)
                die(a->line, "argument for %s parameter '%.*s' must be written '&place'", p->op == M_SET ? "set" : "inout",
                    SF(p->name));
            exprs[i] = a->a;
        } else {
            if (a->kind == N_ADDR)
                die(a->line, "'&' is only for inout or set parameters, but '%.*s' is not inout or set", SF(p->name));
            exprs[i] = a;
        }
    }
    infer_values(&b, pats, exprs, nv, pre, tpl->ty, expect);
    require_bound(&b, n->line, "write the arguments out, as in 'name::<...>(...)', or give the result its type");
    *pre_out = pre;
    return instantiate_fn(tpl, b.types, b.vals, b.ctypes, n->line);
}

static Type *check_call(Node *n, Type *expect) {
    Node *fn = lookup_fn(n->qual, n->name, n->line);
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
    Type **pre = NULL; /* the argument types, when a generic call has already checked them */
    if (fn->gen) fn = infer_call(fn, n, expect, &pre);
    else if (n->gen) die(n->line, "function '%.*s' is not generic, so it takes no type arguments", SF(n->name));
    int want = 0, got = 0;
    for (Node *p = fn->a; p; p = p->next) want++;
    for (Node *a = n->a; a; a = a->next) got++;
    if (want != got)
        die(n->line, "function '%.*s' takes %d argument%s, got %d", SF(n->name), want, want == 1 ? "" : "s", got);
    Node *p = fn->a;
    int ai = 0;
    for (Node *a = n->a; a; a = a->next, p = p->next, ai++) {
        Type *at;
        if (p->op == M_INOUT || p->op == M_SET) {
            const char *mode = p->op == M_SET ? "set" : "inout";
            if (a->kind != N_ADDR)
                die(a->line, "argument for %s parameter '%.*s' must be written '&place'", mode, SF(p->name));
            at = pre ? pre[ai] : check_expr(a->a, p->type);
            require_mutable_place(a->a, p->op == M_SET ? "passed as set" : "passed as inout");
            a->type = at;
        } else {
            if (a->kind == N_ADDR)
                die(a->line, "'&' is only for inout or set parameters, but '%.*s' is not inout or set", SF(p->name));
            at = pre ? pre[ai] : check_expr(a, p->type);
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

/* ---- enums: constructors, patterns and match ---- */

static Node wild_pat = {.kind = N_PAT, .op = PAT_WILD}; /* padding for the rows of the exhaustiveness check */

static bool irrefutable(const Node *p) { return p->op == PAT_WILD || p->op == PAT_BIND; }

static void check_block(Node *b);

/* Check pattern p against a value of type t, declaring the names it binds. */
static void check_pattern(Node *p, Type *t, bool in_alternative) {
    p->type = t;
    switch (p->op) {
    case PAT_WILD:
        break;
    case PAT_BIND:
        if (in_alternative) die(p->line, "alternatives joined with '|' cannot bind names");
        if (t->kind == TY_ENUM && find_variant(t->decl, p->name))
            die(p->line, "'%.*s' is also a variant of '%s'; write '%s::%.*s' to match it (a bare name is a new variable)",
                SF(p->name), t->name, t->name, SF(p->name));
        p->local = declare(p->line, p->name, t, 0, 0, LK_LET);
        break;
    case PAT_LIT:
        if (t == ty_bool) {
            if (p->a->kind != N_BOOL) die(p->line, "a bool is matched with true or false");
        } else if (t->kind == TY_INT) {
            if (p->a->kind == N_BOOL) die(p->line, "a number is matched with a number, not true or false");
            expect_type(p->line, t, check_expr(p->a, t)); /* folds -5 and range-checks the literal */
        } else {
            die(p->line, "a literal cannot match a value of type '%s'", t->name);
        }
        break;
    case PAT_VARIANT: {
        if (t->kind != TY_ENUM)
            die(p->line, "'%.*s::%.*s' is a variant of an enum, but the value being matched has type '%s'", SF(p->tyname),
                SF(p->name), t->name);
        if (p->qual.n && !find_alias(p->qual)) die(p->line, "unknown module '%.*s'", SF(p->qual));
        if (!str_eq(t->decl->name, p->tyname))
            die(p->line, "the pattern '%.*s::%.*s' does not match the type '%s'", SF(p->tyname), SF(p->name), t->name);
        Node *v = find_variant(t->decl, p->name);
        if (!v) die(p->line, "enum '%s' has no variant '%.*s'", t->name, SF(p->name));
        int want = list_len(v->a), got = list_len(p->a);
        if (want == 0 && p->flag) die(p->line, "'%s::%.*s' has no values; write the pattern without parentheses", t->name, SF(v->name));
        if (want != got)
            die(p->line, "'%s::%.*s' has %d value%s, but the pattern has %d", t->name, SF(v->name), want, want == 1 ? "" : "s", got);
        Node *f = v->a;
        for (Node *s = p->a; s; s = s->next, f = f->next) check_pattern(s, f->type, in_alternative);
        p->decl = v;
        break;
    }
    case PAT_OR:
        for (Node *alt = p->a; alt; alt = alt->next) check_pattern(alt, t, true);
        break;
    }
}

/* ---- exhaustiveness ----
 * `rows` is a matrix of patterns, one row per match arm, `ncols` patterns wide, column i matching a value of
 * types[i]. covers() answers: does every possible value match some row? For an enum column, each variant is
 * tried in turn: the rows that can match it keep their other columns and gain one column per value the variant
 * carries (a wildcard row gets wildcards). For bool, true and false are tried. For an integer, only a catch-all
 * row covers every value. */
static void *zalloc_(size_t n) { return arena_alloc(&g_arena, n); }

static Node ***expand_alternatives(Node ***rows, int *nrows, int ncols) {
    int total = 0;
    for (int r = 0; r < *nrows; r++) total += rows[r][0]->op == PAT_OR ? list_len(rows[r][0]->a) : 1;
    Node ***out = zalloc_(sizeof(Node **) * (size_t)(total ? total : 1));
    int k = 0;
    for (int r = 0; r < *nrows; r++) {
        Node *p = rows[r][0];
        if (p->op != PAT_OR) {
            out[k++] = rows[r];
            continue;
        }
        for (Node *alt = p->a; alt; alt = alt->next) {
            Node **nr = zalloc_(sizeof(Node *) * (size_t)ncols);
            memcpy(nr, rows[r], sizeof(Node *) * (size_t)ncols);
            nr[0] = alt;
            out[k++] = nr;
        }
    }
    *nrows = k;
    return out;
}

static bool covers(Node ***rows, int nrows, Type **types, int ncols);

/* Does this row fit every value (every pattern in it is a wildcard or a name)? Then the rows below it do not
 * matter, and the search can stop instead of trying every combination of the remaining columns. */
static bool row_fits_all(Node **row, int ncols) {
    for (int c = 0; c < ncols; c++)
        if (!irrefutable(row[c])) return false;
    return true;
}

/* The rows that can match variant v, with the first column replaced by the variant's values: a row for v
 * contributes its sub-patterns, a catch-all row contributes a wildcard for each value. *ts gets the new column types. */
static Node ***specialize(Node ***rows, int nrows, Type **types, int ncols, Node *v, int *out_rows, Type ***ts) {
    int ar = list_len(v->a), width = ar + ncols - 1;
    Node ***sub = zalloc_(sizeof(Node **) * (size_t)(nrows ? nrows : 1));
    int m = 0;
    for (int r = 0; r < nrows; r++) {
        Node *p = rows[r][0];
        Node **nr = NULL;
        if (p->op == PAT_VARIANT && p->decl == v) {
            nr = zalloc_(sizeof(Node *) * (size_t)(width ? width : 1));
            int i = 0;
            for (Node *s = p->a; s; s = s->next) nr[i++] = s;
            for (int c = 1; c < ncols; c++) nr[i++] = rows[r][c];
        } else if (irrefutable(p)) {
            nr = zalloc_(sizeof(Node *) * (size_t)(width ? width : 1));
            int i = 0;
            for (; i < ar; i++) nr[i] = &wild_pat;
            for (int c = 1; c < ncols; c++) nr[i++] = rows[r][c];
        }
        if (nr) sub[m++] = nr;
    }
    *ts = zalloc_(sizeof(Type *) * (size_t)(width ? width : 1));
    int i = 0;
    for (Node *f = v->a; f; f = f->next) (*ts)[i++] = f->type;
    for (int c = 1; c < ncols; c++) (*ts)[i++] = types[c];
    *out_rows = m;
    return sub;
}

static bool variant_covered(Node ***rows, int nrows, Type **types, int ncols, Node *v) {
    int m;
    Type **ts;
    Node ***sub = specialize(rows, nrows, types, ncols, v, &m, &ts);
    return covers(sub, m, ts, list_len(v->a) + ncols - 1);
}

static bool covers(Node ***rows, int nrows, Type **types, int ncols) {
    if (ncols == 0) return nrows > 0;
    rows = expand_alternatives(rows, &nrows, ncols);
    for (int r = 0; r < nrows; r++)
        if (row_fits_all(rows[r], ncols)) return true;
    Type *t = types[0];
    if (t->kind == TY_ENUM) {
        for (Node *v = t->decl->a; v; v = v->next)
            if (!variant_covered(rows, nrows, types, ncols, v)) return false;
        return true;
    }
    for (int value = 0; value < (t == ty_bool ? 2 : 1); value++) { /* bool: false, true; others: just "everything else" */
        Node ***rest = zalloc_(sizeof(Node **) * (size_t)(nrows ? nrows : 1));
        int m = 0;
        for (int r = 0; r < nrows; r++) {
            Node *p = rows[r][0];
            bool takes = irrefutable(p) || (t == ty_bool && p->op == PAT_LIT && (int)p->a->ival == value);
            if (takes) rest[m++] = rows[r] + 1;
        }
        if (!covers(rest, m, types + 1, ncols - 1)) return false;
    }
    return true;
}

/* ---- unreachable arms ----
 * The same walk answers the opposite question: can the pattern vector q match some value that none of the
 * rows (the arms above) matches? If not, the arm is dead code, which is almost certainly a mistake. For
 * integers only a catch-all row is counted, so an arm is never wrongly called unreachable. */
static uint64_t lit_bits(const Node *lit) {
    return lit->kind == N_BOOL ? lit->ival : (lit->op ? (uint64_t)0 - lit->ival : lit->ival);
}

static bool useful(Node ***rows, int nrows, Node **q, Type **types, int ncols) {
    if (ncols == 0) return nrows == 0;
    rows = expand_alternatives(rows, &nrows, ncols);
    for (int r = 0; r < nrows; r++)
        if (row_fits_all(rows[r], ncols)) return false; /* that row already fits whatever q could */
    Node *p = q[0];
    Type *t = types[0];
    if (p->op == PAT_OR) {
        for (Node *alt = p->a; alt; alt = alt->next) {
            Node **q2 = zalloc_(sizeof(Node *) * (size_t)ncols);
            memcpy(q2, q, sizeof(Node *) * (size_t)ncols);
            q2[0] = alt;
            if (useful(rows, nrows, q2, types, ncols)) return true;
        }
        return false;
    }
    if (t->kind == TY_ENUM) {
        for (Node *v = t->decl->a; v; v = v->next) {
            if (p->op == PAT_VARIANT && p->decl != v) continue;
            int m, ar = list_len(v->a), width = ar + ncols - 1;
            Type **ts;
            Node ***sub = specialize(rows, nrows, types, ncols, v, &m, &ts);
            Node **q2 = zalloc_(sizeof(Node *) * (size_t)(width ? width : 1));
            int i = 0;
            if (p->op == PAT_VARIANT) {
                for (Node *s = p->a; s; s = s->next) q2[i++] = s;
            } else {
                for (; i < ar; i++) q2[i] = &wild_pat;
            }
            for (int c = 1; c < ncols; c++) q2[i++] = q[c];
            if (useful(sub, m, q2, ts, width)) return true;
        }
        return false;
    }
    int values = (p->op != PAT_LIT && t == ty_bool) ? 2 : 1; /* a catch-all on a bool is tried as true, then false */
    for (int value = 0; value < values; value++) {
        Node ***rest = zalloc_(sizeof(Node **) * (size_t)(nrows ? nrows : 1));
        int m = 0;
        for (int r = 0; r < nrows; r++) {
            Node *rp = rows[r][0];
            bool takes;
            if (irrefutable(rp)) takes = true;
            else if (p->op == PAT_LIT) takes = lit_bits(rp->a) == lit_bits(p->a);
            else takes = t == ty_bool && lit_bits(rp->a) == (uint64_t)value;
            if (takes) rest[m++] = rows[r] + 1;
        }
        if (useful(rest, m, q + 1, types + 1, ncols - 1)) return true;
    }
    return false;
}

static void check_exhaustive(Node *m, Type *st) {
    int n = list_len(m->b), i = 0;
    Node ***rows = zalloc_(sizeof(Node **) * (size_t)n);
    for (Node *arm = m->b; arm; arm = arm->next, i++) {
        rows[i] = zalloc_(sizeof(Node *));
        rows[i][0] = arm->a;
    }
    Type *types[1] = {st};
    if (covers(rows, n, types, 1)) return;
    if (st->kind == TY_ENUM) { /* say which variants are not fully covered */
        int rn = n;
        Node ***ex = expand_alternatives(rows, &rn, 1);
        char list[512] = "";
        int missing = 0;
        for (Node *v = st->decl->a; v; v = v->next) {
            if (variant_covered(ex, rn, types, 1, v)) continue;
            size_t len = strlen(list);
            snprintf(list + len, sizeof list - len, "%s'%s::%.*s'", missing++ ? ", " : "", st->name, SF(v->name));
        }
        die(m->line, "match is not exhaustive: %s %s not fully covered (add %s, or a '_' arm)", list,
            missing == 1 ? "is" : "are", missing == 1 ? "an arm for it" : "arms for them");
    }
    if (st == ty_bool) die(m->line, "match is not exhaustive: it needs arms for both true and false, or a '_' arm");
    die(m->line, "match is not exhaustive: numbers need a final '_' arm (or a name) that matches everything else");
}

static bool always_returns(Node *s);

static Type *check_match(Node *m, Type *expect) {
    Type *st = check_expr(m->a, NULL);
    if (st->kind != TY_ENUM && st != ty_bool && st->kind != TY_INT)
        die(m->a->line, "cannot match on a value of type '%s' (match works on enums, integers and bool)", st->name);
    bool stmt = m->flag; /* a statement: the arms' values, if any, are dropped */
    Type *result = ty_unit;
    bool have = false;
    for (Node *arm = m->b; arm; arm = arm->next) {
        int save = nlocals;
        depth++;
        check_pattern(arm->a, st, false);
        if (arm->op) { /* a block */
            if (!stmt && !always_returns(arm->b))
                die(arm->line,
                    "a match used as a value needs an expression in every arm; a block arm is only allowed when it ends in 'return'");
            check_block(arm->b);
        } else {
            Type *at = check_expr(arm->b, stmt ? NULL : (have ? result : expect));
            if (!stmt) {
                if (at == ty_unit) die(arm->b->line, "this arm has no value, but the match is used as a value");
                if (!have) result = at;
                else expect_type(arm->b->line, result, at);
                have = true;
            }
        }
        depth--;
        nlocals = save;
    }
    if (!stmt && !have) die(m->line, "this match has no arm with a value, but it is used as a value");
    check_exhaustive(m, st);
    int n = list_len(m->b), i = 0;
    Node ***rows = zalloc_(sizeof(Node **) * (size_t)n);
    for (Node *arm = m->b; arm; arm = arm->next, i++) {
        rows[i] = zalloc_(sizeof(Node *));
        rows[i][0] = arm->a;
        Type *types[1] = {st};
        if (i > 0 && !useful(rows, i, &arm->a, types, 1))
            die(arm->line, "this arm can never match: the arms above already match every value it could match");
    }
    return stmt ? ty_unit : result;
}

static Type *check_expr_inner(Node *n, Type *expect) {
    switch (n->kind) {
    case N_INT:
        if (n->flag) return n->type;
        return check_int_literal(n, expect);
    case N_BOOL:
        return ty_bool;
    case N_CSTR:
        return ptr_type(ty_u8, false);
    case N_VARIANT: {
        if (!n->qual.n && find_alias(n->tyname) && !find_decl((Str){0}, n->tyname, n->line)) { /* module::name */
            n->qual = n->tyname;
            n->tyname = (Str){0};
            n->kind = n->flag ? N_CALL : N_NAME;
            return check_expr_inner(n, expect);
        }
        Node *ed = find_enum(n->qual, n->tyname, n->line);
        if (!ed) die(n->line, n->qual.n ? "unknown enum '%.*s'" : "unknown enum or module '%.*s'", SF(n->tyname));
        Node *v = find_variant(ed, n->name);
        if (!v) die(n->line, "enum '%.*s' has no variant '%.*s'", SF(ed->name), SF(n->name));
        int want = list_len(v->a), got = list_len(n->a);
        if (want == 0 && n->flag)
            die(n->line, "'%.*s::%.*s' has no values; write it without parentheses", SF(ed->name), SF(v->name));
        if (want != got)
            die(n->line, "'%.*s::%.*s' needs %d value%s, got %d", SF(ed->name), SF(v->name), want, want == 1 ? "" : "s", got);
        Type **pre = NULL;
        if (ed->gen) { /* a generic enum: the values (or the expected type) decide which instance */
            Node **pats = gzalloc(sizeof(Node *) * (size_t)got), **exprs = gzalloc(sizeof(Node *) * (size_t)got);
            pre = gzalloc(sizeof(Type *) * (size_t)got);
            int k = 0;
            for (Node *f = v->a, *a = n->a; a; f = f->next, a = a->next, k++) {
                pats[k] = f->ty;
                exprs[k] = a;
            }
            Type *et = infer_instance(ed, n->gen, n->line, expect, pats, exprs, got, pre);
            ed = et->decl;
            v = find_variant(ed, n->name);
        } else if (n->gen) {
            die(n->line, "enum '%.*s' is not generic, so it takes no type arguments", SF(ed->name));
        }
        Node *f = v->a;
        int k = 0;
        for (Node *a = n->a; a; a = a->next, f = f->next, k++)
            expect_type(a->line, f->type, pre ? pre[k] : check_expr(a, f->type));
        n->decl = v;
        return ed->type;
    }
    case N_MATCH:
        return check_match(n, expect);
    case N_NAME: {
        Local *l = (const_ctx || n->qual.n) ? NULL : find_local(n->name);
        if (l) {
            n->local = l->id;
            n->by_ptr = l->by_ptr;
            n->mut = l->is_mut;
            return l->type;
        }
        Node *c = scope_find(K_CONST, n->qual, n->name, n->line);
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
        return !n->qual.n && is_intrinsic(n->name) ? check_intrinsic(n, expect) : check_call(n, expect);
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
        Node *s = find_struct(n->qual, n->name, n->line);
        if (!s) die(n->line, "unknown struct '%.*s'", SF(n->name));
        Type **pre = NULL;
        if (s->gen) { /* a generic struct: the values (or the expected type) decide which instance */
            int nv = list_len(n->a), k = 0;
            Node **pats = gzalloc(sizeof(Node *) * (size_t)nv), **exprs = gzalloc(sizeof(Node *) * (size_t)nv);
            pre = gzalloc(sizeof(Type *) * (size_t)nv);
            for (Node *i = n->a; i; i = i->next, k++) {
                Node *f = find_field(s, i->name);
                if (!f) die(i->line, "struct '%.*s' has no field '%.*s'", SF(s->name), SF(i->name));
                for (Node *j = n->a; j != i; j = j->next)
                    if (str_eq(j->name, i->name)) die(i->line, "field '%.*s' given twice", SF(i->name));
                pats[k] = f->ty;
                exprs[k] = i->a;
            }
            s = infer_instance(s, n->gen, n->line, expect, pats, exprs, nv, pre)->decl;
        } else if (n->gen) {
            die(n->line, "struct '%.*s' is not generic, so it takes no type arguments", SF(s->name));
        }
        int vi = 0;
        for (Node *i = n->a; i; i = i->next, vi++) {
            Node *f = find_field(s, i->name);
            if (!f) die(i->line, "struct '%.*s' has no field '%.*s'", SF(s->name), SF(i->name));
            for (Node *j = n->a; j != i; j = j->next)
                if (str_eq(j->name, i->name)) die(i->line, "field '%.*s' given twice", SF(i->name));
            i->decl = f;
            expect_type(i->line, f->type, pre ? pre[vi] : check_expr(i->a, f->type));
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
                  (from->kind == TY_PTR && to->kind == TY_PTR) ||
                  (from->kind == TY_ENUM && enum_fieldless(from) && to->kind == TY_INT);
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
            if (lt->kind != TY_INT && lt != ty_bool && lt->kind != TY_PTR && !(lt->kind == TY_ENUM && enum_fieldless(lt)))
                die(n->line, "cannot compare values of type '%s'%s", lt->name,
                    lt->kind == TY_ENUM ? " (an enum that carries values is taken apart with match)" : "");
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
    int saved_mod = cur_mod;
    enter_module(c->mod);
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
    enter_module(saved_mod);
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
        s->local = declare(s->line, s->name, t, s->op, 0, s->op ? LK_VAR : LK_LET);
        break;
    }
    case N_ASSIGN: {
        if (s->a->kind == N_NAME && !find_local(s->a->name) && scope_find(K_CONST, s->a->qual, s->a->name, s->a->line))
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
    case N_MATCH:
        check_match(s, NULL);
        s->type = ty_unit;
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
    case N_MATCH: /* a match statement returns if every arm is a block that does */
        if (!s->flag) return false;
        for (Node *arm = s->b; arm; arm = arm->next)
            if (!arm->op || !always_returns(arm->b)) return false;
        return true;
    default: return false;
    }
}

/* ---- program ---- */
/* The values each variant carries, the discriminant of each variant, and the integer type of the tag. */
static void resolve_enum(Node *d) {
    int64_t next = 0, lo = 0, hi = 0;
    bool first = true;
    for (Node *v = d->a; v; v = v->next) {
        for (Node *w = d->a; w != v; w = w->next)
            if (str_eq(w->name, v->name)) die(v->line, "variant '%.*s' is declared twice", SF(v->name));
        for (Node *f = v->a; f; f = f->next) {
            f->type = resolve_type(f->ty);
            if (!d->inst && d->type->is_copy && !f->type->is_copy)
                die(f->line, "'%s' in variant '%.*s' is not copy, so '%.*s' cannot be a copy enum", f->type->name,
                    SF(v->name), SF(d->name));
        }
        int64_t disc = next;
        if (v->b) {
            if (v->a) die(v->line, "variant '%.*s' carries values, so it cannot have an explicit discriminant", SF(v->name));
            disc = const_int_value(v->b);
        }
        for (Node *w = d->a; w != v; w = w->next)
            if ((int64_t)w->ival == disc)
                die(v->line, "variants '%.*s' and '%.*s' have the same discriminant %lld", SF(w->name), SF(v->name),
                    (long long)disc);
        v->ival = (uint64_t)disc;
        next = (int64_t)((uint64_t)disc + 1);
        if (first || disc < lo) lo = disc;
        if (first || disc > hi) hi = disc;
        first = false;
    }
    /* the smallest integer type that holds every discriminant: prims[6] u8, [7] u16, [8] u32, [4] i32, [5] i64 */
    d->type->tag_type = (lo >= 0 && hi <= 255)            ? &prims[6]
                        : (lo >= 0 && hi <= 65535)        ? &prims[7]
                        : (lo >= 0 && hi <= (int64_t)UINT32_MAX) ? &prims[8]
                        : (lo >= INT32_MIN && hi <= INT32_MAX)   ? &prims[4]
                                                                 : &prims[5];
}

static void visit_decl(Node *s);

static void visit_fields(Node *fields) {
    for (Node *f = fields; f; f = f->next) {
        Type *ft = f->type;
        while (ft->kind == TY_ARRAY) ft = ft->elem; /* an array of S inside S is just as infinite */
        if (ft->kind == TY_STRUCT || ft->kind == TY_ENUM) visit_decl(ft->decl);
    } /* a pointer to S inside S is fine: it has a fixed size */
}

static void visit_decl(Node *s) { /* reject structs and enums that contain themselves by value */
    if (s->ival == 1) die(s->line, "%s '%.*s' contains itself", s->kind == N_ENUM ? "enum" : "struct", SF(s->name));
    if (s->ival == 2) return;
    s->ival = 1;
    if (s->kind == N_STRUCT) {
        visit_fields(s->a);
    } else {
        for (Node *v = s->a; v; v = v->next) visit_fields(v->a);
    }
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
    enter_module(fn->mod);
    cur_fn = fn;
    nlocals = 0;
    depth = 1;
    next_id = 0;
    unsafe_depth = 0;
    if (fn->inst) note_push("in '%s', which was asked for on line %d", note_name(fn->inst->name), fn->inst->line);
    for (Node *p = fn->a; p; p = p->next)
        p->local = declare(p->line, p->name, p->type, p->op == M_INOUT || p->op == M_SET, p->op == M_INOUT || p->op == M_SET,
                           p->op == M_INOUT ? LK_INOUT : p->op == M_SET ? LK_SET : p->op == M_SINK ? LK_SINK : LK_LETPARAM);
    check_block(fn->b);
    fn->nlocals = next_id;
    fn->linfo = arena_alloc(&g_arena, sizeof(LocalInfo) * (size_t)(next_id ? next_id : 1));
    if (next_id) memcpy(fn->linfo, fn_locals, sizeof(LocalInfo) * (size_t)next_id);
    if (fn->type != ty_unit && !always_returns(fn->b))
        die(fn->line, "function '%.*s' does not return a value on every path", SF(fn->name));
    if (fn->inst) note_pop();
}

Node *check_program(Node *program) {
    prog = program;
    prog_tail = program;
    while (prog_tail && prog_tail->next) prog_tail = prog_tail->next;
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
    size_t *cnt = arena_alloc(&g_arena, sizeof(size_t) * 3 * (size_t)g_nmodules);
    for (Node *d = prog; d; d = d->next)
        cnt[3 * d->mod + (d->kind == N_STRUCT || d->kind == N_ENUM ? 0 : d->kind == N_CONST ? 2 : 1)]++;
    scopes = arena_alloc(&g_arena, sizeof(ModScope) * (size_t)g_nmodules);
    for (int i = 0; i < g_nmodules; i++) {
        map_init(&scopes[i].types, cnt[3 * i]);
        map_init(&scopes[i].fns, cnt[3 * i + 1]);
        map_init(&scopes[i].consts, cnt[3 * i + 2]);
    }
    for (Node *d = prog; d; d = d->next) {
        enter_module(d->mod);
        bool is_type = d->kind == N_STRUCT || d->kind == N_ENUM;
        Map *m = is_type ? &scopes[d->mod].types : d->kind == N_CONST ? &scopes[d->mod].consts : &scopes[d->mod].fns;
        if (!map_put(m, d->name, d)) die(d->line, "'%.*s' is defined twice", SF(d->name));
        if (d->gen) {
            for (Node *p = d->gen; p; p = p->next)
                for (Node *q = d->gen; q != p; q = q->next)
                    if (str_eq(p->name, q->name)) die(p->line, "generic parameter '%.*s' declared twice", SF(p->name));
            for (Node *p = d->gen; p; p = p->next)
                for (int i = 0; i < NPRIMS; i++)
                    if (str_is(p->name, prims[i].name))
                        die(p->line, "'%.*s' is a builtin type, so it cannot be a generic parameter", SF(p->name));
            if (d->kind == N_FN && d->ext != EXT_NONE)
                die(d->line, "a function that is shared with C cannot be generic: C has no generics");
            if (d->kind == N_FN && str_is(d->name, "main")) die(d->line, "'main' cannot be generic");
        }
        if (is_type) {
            for (int i = 0; i < NPRIMS; i++)
                if (str_is(d->name, prims[i].name)) die(d->line, "'%.*s' is a builtin type", SF(d->name));
            if (d->gen) continue; /* a template: its instances get their own types */
            Type *t = ARENA_NEW(&g_arena, Type);
            t->kind = d->kind == N_ENUM ? TY_ENUM : TY_STRUCT;
            t->nleaves = d->kind == N_ENUM ? 1 : 0; /* flow treats an enum as one unit, like an array */
            t->name = arena_strn(&g_arena, d->name.p, d->name.n);
            char *cn = arena_alloc(&g_arena, (size_t)d->name.n + 24);
            if (d->mod == 0) snprintf(cn, (size_t)d->name.n + 24, "%c_%.*s", d->kind == N_ENUM ? 'e' : 's', SF(d->name));
            else snprintf(cn, (size_t)d->name.n + 24, "%cm%d_%.*s", d->kind == N_ENUM ? 'e' : 's', d->mod, SF(d->name));
            t->cname = cn;
            t->decl = d;
            t->is_copy = d->op != 0;
            d->type = t;
        } else if (d->kind == N_FN && (str_eq(d->name, builtin_print.name) || is_intrinsic(d->name))) {
            die(d->line, "'%.*s' is a builtin function", SF(d->name));
        }
    }
    for (int i = 0; i < g_nmodules; i++) { /* a module name cannot also be the name of one of the module's own things */
        enter_module(i);
        for (Import *im = g_modules[i]->imports; im; im = im->next)
            if (im->alias.n && (map_get(&scopes[i].types, im->alias) || map_get(&scopes[i].fns, im->alias) ||
                                map_get(&scopes[i].consts, im->alias)))
                die(im->line, "'%.*s' is both the name of an import and of a declaration in this module", SF(im->alias));
    }

    /* 2. struct fields. Generic instances made on the way wait (inst_depth) until every plain type is resolved. */
    inst_depth = 1;
    for (Node *d = prog; d; d = d->next) {
        if (d->kind != N_STRUCT || d->gen || d->inst) continue;
        enter_module(d->mod);
        for (Node *f = d->a; f; f = f->next) {
            for (Node *g = d->a; g != f; g = g->next)
                if (str_eq(g->name, f->name)) die(f->line, "field '%.*s' declared twice", SF(f->name));
            f->type = resolve_type(f->ty);
        }
    }
    for (Node *d = prog; d; d = d->next)
        if (d->kind == N_ENUM && !d->gen && !d->inst) {
            enter_module(d->mod);
            resolve_enum(d);
        }
    inst_depth = 0;
    finish_pending();
    for (Node *d = prog; d; d = d->next)
        if ((d->kind == N_STRUCT || d->kind == N_ENUM) && !d->gen && !d->inst) {
            enter_module(d->mod);
            visit_decl(d);
        }
    for (Node *d = prog; d; d = d->next) {
        if (d->kind != N_STRUCT || d->gen || d->inst) continue;
        enter_module(d->mod);
        layout_type(d->type);
        if (!d->type->is_copy) continue;
        for (Node *f = d->a; f; f = f->next)
            if (!f->type->is_copy)
                die(f->line, "field '%.*s' of copy struct '%.*s' has type '%s', which is not copy",
                    SF(f->name), SF(d->name), f->type->name);
    }

    /* 3. constants, then function signatures */
    for (Node *d = prog; d; d = d->next)
        if (d->kind == N_CONST) {
            enter_module(d->mod);
            const_resolve(d);
        }
    Node *main_fn = NULL;
    for (Node *d = prog; d; d = d->next) {
        if (d->kind != N_FN || d->gen || d->inst) continue;
        enter_module(d->mod);
        for (Node *p = d->a; p; p = p->next) {
            for (Node *q = d->a; q != p; q = q->next)
                if (str_eq(q->name, p->name)) die(p->line, "parameter '%.*s' declared twice", SF(p->name));
            p->type = resolve_type(p->ty);
        }
        d->type = d->ty ? resolve_type(d->ty) : ty_unit;
        if (d->ext != EXT_NONE) check_c_signature(d);
        if (str_is(d->name, "main") && d->mod == 0) main_fn = d;
    }
    enter_module(0);
    if (!main_fn) die(1, "no 'main' function");
    if (main_fn->a || main_fn->type != ty_i32 || main_fn->ext != EXT_NONE)
        die(main_fn->line, "'main' must be declared as 'fn main() -> i32'");

    /* 4. bodies (extern functions have none) */
    for (Node *d = prog; d; d = d->next) /* instances made by calls are appended, and checked in their turn */
        if (d->kind == N_FN && d->ext != EXT_EXTERN && !d->gen) check_fn(d);

    enter_module(0);
    /* the later passes see instances only, not the generic templates */
    Node *head = NULL, **tail = &head;
    for (Node *d = prog; d;) {
        Node *next = d->next;
        if (!d->gen) {
            *tail = d;
            tail = &d->next;
        }
        d = next;
    }
    *tail = NULL;
    return head;
}
