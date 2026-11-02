/* Name resolution and type checking. Stops at the first error. */
#include "noge.h"

static Type prims[] = {
    {TY_UNIT, "unit", 0, false, NULL, "void", true, 0},
    {TY_BOOL, "bool", 1, false, NULL, "bool", true, 1},
    {TY_INT, "i8", 8, true, NULL, "int8_t", true, 1},
    {TY_INT, "i16", 16, true, NULL, "int16_t", true, 1},
    {TY_INT, "i32", 32, true, NULL, "int32_t", true, 1},
    {TY_INT, "i64", 64, true, NULL, "int64_t", true, 1},
    {TY_INT, "u8", 8, false, NULL, "uint8_t", true, 1},
    {TY_INT, "u16", 16, false, NULL, "uint16_t", true, 1},
    {TY_INT, "u32", 32, false, NULL, "uint32_t", true, 1},
    {TY_INT, "u64", 64, false, NULL, "uint64_t", true, 1},
};
#define NPRIMS ((int)(sizeof prims / sizeof *prims))
Type *ty_unit = &prims[0], *ty_bool = &prims[1], *ty_i32 = &prims[4];

static Node *prog;
static Node builtin_print, builtin_print_param;

/* ---- locals ---- */
typedef struct {
    Str name;
    Type *type;
    int id, depth, is_mut, by_ptr;
} Local;
#define MAX_LOCALS 8192
static Local locals[MAX_LOCALS];
static int nlocals, depth, next_id;
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

/* ---- types ---- */
static Node *find_struct(Str name) {
    for (Node *d = prog; d; d = d->next)
        if (d->kind == N_STRUCT && str_eq(d->name, name)) return d;
    return NULL;
}

static Type *resolve_type(Str name, int line) {
    for (int i = 1; i < NPRIMS; i++)
        if (str_is(name, prims[i].name)) return &prims[i];
    Node *s = find_struct(name);
    if (s) return s->type;
    die(line, "unknown type '%.*s'", SF(name));
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

static Node *lookup_fn(Str name) {
    for (Node *d = prog; d; d = d->next)
        if (d->kind == N_FN && str_eq(d->name, name)) return d;
    if (str_eq(name, builtin_print.name)) return &builtin_print;
    return NULL;
}

/* ---- places ---- */
static bool is_place(Node *n) {
    while (n->kind == N_MEMBER) n = n->a;
    return n->kind == N_NAME;
}

static void require_mutable_place(Node *n, const char *action) {
    if (!is_place(n)) die(n->line, "only a variable or field can be %s", action);
    Node *root = n;
    while (root->kind == N_MEMBER) root = root->a;
    if (!root->mut)
        die(n->line, "'%.*s' is read-only (declare it with 'var', or take it as an 'inout' parameter), so it cannot be %s",
            SF(root->name), action);
}

/* ---- expressions ---- */
static bool is_lit(Node *n) {
    return n->kind == N_INT || (n->kind == N_UN && n->op == T_MINUS && n->a->kind == N_INT);
}

static Type *check_expr(Node *n, Type *expect);

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

static Type *check_call(Node *n) {
    Node *fn = lookup_fn(n->name);
    if (!fn) die(n->line, "unknown function '%.*s'", SF(n->name));
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

static Type *check_expr_inner(Node *n, Type *expect) {
    switch (n->kind) {
    case N_INT:
        return check_int_literal(n, expect);
    case N_BOOL:
        return ty_bool;
    case N_NAME: {
        Local *l = find_local(n->name);
        if (!l) die(n->line, "unknown name '%.*s'", SF(n->name));
        n->local = l->id;
        n->by_ptr = l->by_ptr;
        n->mut = l->is_mut;
        return l->type;
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
        return check_call(n);
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
        Type *from = check_expr(n->a, NULL);
        Type *to = resolve_type(n->tyname, n->line);
        bool ok = (from == to) || ((from->kind == TY_INT || from->kind == TY_BOOL) && to->kind == TY_INT);
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
        } else if (t->kind != TY_INT || !t->is_signed) {
            die(n->line, "unary '-' needs a signed integer, found '%s'", t->name);
        }
        return t;
    }
    case N_ADDR:
        die(n->line, "'&' is only allowed on arguments for inout or set parameters");
    case N_BIN: {
        TokKind op = (TokKind)n->op;
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
            if (lt != ty_bool) die(n->line, "'%s' needs 'bool' operands, found '%s'", tok_name(op), lt->name);
            return ty_bool;
        }
        if (op == T_EQ || op == T_NE) {
            if (lt->kind != TY_INT && lt != ty_bool) die(n->line, "cannot compare values of type '%s'", lt->name);
            return ty_bool;
        }
        if (lt->kind != TY_INT) die(n->line, "operator %s needs integer operands, found '%s'", tok_name(op), lt->name);
        return cmp ? ty_bool : lt;
    }
    default:
        die(n->line, "internal error: not an expression");
    }
}

static Type *check_expr(Node *n, Type *expect) {
    return n->type = check_expr_inner(n, expect);
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
            t = resolve_type(s->tyname, s->line);
        } else if (s->tyname.n) {
            t = resolve_type(s->tyname, s->line);
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
        check_block(s);
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
    for (Node *f = s->a; f; f = f->next)
        if (f->type->kind == TY_STRUCT) visit_struct(f->type->decl);
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
    for (Node *p = fn->a; p; p = p->next)
        p->local = declare(p->line, p->name, p->type, p->op == M_INOUT || p->op == M_SET, p->op == M_INOUT || p->op == M_SET);
    check_block(fn->b);
    fn->nlocals = next_id;
    if (fn->type != ty_unit && !always_returns(fn->b))
        die(fn->line, "function '%.*s' does not return a value on every path", SF(fn->name));
}

void check_program(Node *program) {
    prog = program;

    builtin_print.kind = N_FN;
    builtin_print.name = (Str){"print", 5};
    builtin_print.type = ty_unit;
    builtin_print.ival = 1; /* builtin */
    builtin_print_param.kind = N_PARAM;
    builtin_print_param.name = (Str){"x", 1};
    builtin_print_param.type = ty_i32;
    builtin_print.a = &builtin_print_param;

    /* 1. declare struct and function names */
    for (Node *d = prog; d; d = d->next) {
        for (Node *e = prog; e != d; e = e->next)
            if (e->kind == d->kind && str_eq(e->name, d->name))
                die(d->line, "'%.*s' is defined twice", SF(d->name));
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
        } else if (str_eq(d->name, builtin_print.name)) {
            die(d->line, "'print' is a builtin function");
        }
    }

    /* 2. struct fields */
    for (Node *d = prog; d; d = d->next) {
        if (d->kind != N_STRUCT) continue;
        for (Node *f = d->a; f; f = f->next) {
            for (Node *g = d->a; g != f; g = g->next)
                if (str_eq(g->name, f->name)) die(f->line, "field '%.*s' declared twice", SF(f->name));
            f->type = resolve_type(f->tyname, f->line);
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

    /* 3. function signatures */
    Node *main_fn = NULL;
    for (Node *d = prog; d; d = d->next) {
        if (d->kind != N_FN) continue;
        for (Node *p = d->a; p; p = p->next) {
            for (Node *q = d->a; q != p; q = q->next)
                if (str_eq(q->name, p->name)) die(p->line, "parameter '%.*s' declared twice", SF(p->name));
            p->type = resolve_type(p->tyname, p->line);
        }
        d->type = d->tyname.n ? resolve_type(d->tyname, d->line) : ty_unit;
        if (str_is(d->name, "main")) main_fn = d;
    }
    if (!main_fn) die(1, "no 'main' function");
    if (main_fn->a || main_fn->type != ty_i32)
        die(main_fn->line, "'main' must be declared as 'fn main() -> i32'");

    /* 4. bodies */
    for (Node *d = prog; d; d = d->next)
        if (d->kind == N_FN) check_fn(d);
}
