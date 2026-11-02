/* AST -> IR. Fixes the order of evaluation and spells out every runtime check.
 *
 * Evaluation order (what the language promises):
 *   - operands of a binary operator, call arguments, array elements and
 *     struct-literal fields are evaluated left to right, in source order;
 *   - `a && b` / `a || b` evaluate `b` only when needed;
 *   - an assignment evaluates its right side first, then the left place
 *     (its index expressions and bounds checks), then stores.
 * A value that is just a variable is not copied when nothing that follows can
 * change it; if a later sibling contains a call, it is first copied to a
 * temporary so the call cannot affect the value already read.
 */
#include "ir.h"

static Block *blk;      /* where new instructions go */
static bool fn_has_match; /* the function being lowered contains a match */
static IrVar **vars;    /* local id -> variable of the function being lowered */
static int next_temp;

static void *zalloc(size_t n) { return arena_alloc(&g_arena, n); }

static Instr *emit(IrOp op, int line) {
    Instr *i = ARENA_NEW(&g_arena, Instr);
    i->op = op;
    i->line = line;
    if (blk->last) blk->last->next = i;
    else blk->first = i;
    blk->last = i;
    return i;
}

static IrVar *new_temp(Type *t) {
    IrVar *v = ARENA_NEW(&g_arena, IrVar);
    v->id = next_temp++;
    v->type = t;
    v->is_temp = true;
    return v;
}

static Operand op_var(IrVar *v) {
    Operand o = {0};
    o.var = v;
    o.type = v->type;
    return o;
}

static Operand op_const(Type *t, uint64_t c) {
    Operand o = {0};
    o.c = c;
    o.type = t;
    return o;
}

static Operand op_unit(void) { return op_const(ty_unit, 0); }

static bool is_place(Node *n) {
    while (n->kind == N_MEMBER || n->kind == N_INDEX) n = n->a;
    return n->kind == N_NAME;
}

/* ---------- which expressions can change a variable? only calls ---------- */

static bool has_call(Node *n) {
    if (!n) return false;
    switch (n->kind) {
    case N_CALL:
        return true;
    case N_BIN:
    case N_INDEX:
        return has_call(n->a) || has_call(n->b);
    case N_PTROP:
        return has_call(n->a) || has_call(n->b);
    case N_VARIANT:
        for (Node *a = n->a; a; a = a->next)
            if (has_call(a)) return true;
        return false;
    case N_MATCH: /* the arms of a match used as a value are expressions; a block arm is assumed to have a call */
        if (has_call(n->a)) return true;
        for (Node *arm = n->b; arm; arm = arm->next)
            if (arm->op || has_call(arm->b)) return true;
        return false;
    case N_UN:
    case N_CAST:
    case N_MEMBER:
    case N_ADDR:
    case N_REPEAT:
        return has_call(n->a);
    case N_LIT:
        for (Node *i = n->a; i; i = i->next)
            if (has_call(i->a)) return true;
        return false;
    case N_ARRAY:
        for (Node *e = n->a; e; e = e->next)
            if (has_call(e)) return true;
        return false;
    default:
        return false;
    }
}

static bool list_has_call(Node *list) { /* expressions chained through ->next */
    for (; list; list = list->next)
        if (has_call(list)) return true;
    return false;
}

static bool inits_have_call(Node *init) { /* N_INITF chain */
    for (; init; init = init->next)
        if (has_call(init->a)) return true;
    return false;
}

/* Copy a user variable into a temporary so later code cannot change what we read. */
static Operand snap(Operand o) {
    if (!o.var || o.var->is_temp) return o;
    IrVar *t = new_temp(o.type);
    Instr *i = emit(I_DECL, 0);
    i->dst = t;
    i->a = o;
    i->has_init = true;
    return op_var(t);
}

static IrVar *to_var(Operand o, int line) {
    if (o.var) return o.var;
    IrVar *t = new_temp(o.type);
    Instr *i = emit(I_DECL, line);
    i->dst = t;
    i->a = o;
    i->has_init = true;
    return t;
}

/* ---------- places ---------- */

static void add_proj(IrPlace *p, Node *field, Operand idx) {
    Proj *pr = ARENA_NEW(&g_arena, Proj);
    pr->kind = field ? PK_FIELD : PK_INDEX;
    pr->field = field;
    pr->idx = idx;
    Proj **tail = &p->proj;
    while (*tail) tail = &(*tail)->next;
    *tail = pr;
}

static Operand lower_expr(Node *n);

/* A static is one variable of the whole program: the same IrVar every time, named for C. */
static struct { Node *decl; IrVar *var; } *static_vars;
static int n_static_vars, cap_static_vars;

static IrVar *static_var(Node *d) {
    for (int i = 0; i < n_static_vars; i++)
        if (static_vars[i].decl == d) return static_vars[i].var;
    if (n_static_vars == cap_static_vars) {
        cap_static_vars = cap_static_vars ? cap_static_vars * 2 : 16;
        void *bigger = zalloc(sizeof *static_vars * (size_t)cap_static_vars);
        if (n_static_vars) memcpy(bigger, static_vars, sizeof *static_vars * (size_t)n_static_vars);
        static_vars = bigger;
    }
    IrVar *v = ARENA_NEW(&g_arena, IrVar);
    v->id = -1;
    v->type = d->type;
    v->name = d->name;
    char *c = arena_alloc(&g_arena, (size_t)d->name.n + 24);
    if (d->mod == 0) snprintf(c, (size_t)d->name.n + 24, "g_%.*s", SF(d->name));
    else snprintf(c, (size_t)d->name.n + 24, "gm%s_%.*s", g_modules[d->mod]->tag, SF(d->name));
    v->gname = c;
    static_vars[n_static_vars].decl = d;
    static_vars[n_static_vars].var = v;
    n_static_vars++;
    return v;
}

static bool is_static_name(const Node *n) { return n->kind == N_NAME && n->decl && n->decl->kind == N_STATIC; }

/* the variable a name stands for */
static IrVar *name_var(Node *n) { return is_static_name(n) ? static_var(n->decl) : vars[n->local]; }

/* the static a place starts at, if it does */
static Node *static_root(Node *n) {
    while (n->kind == N_MEMBER || n->kind == N_INDEX) n = n->a;
    return is_static_name(n) ? n->decl : NULL;
}

static IrPlace lower_place(Node *n) {
    IrPlace p = {0};
    switch (n->kind) {
    case N_NAME:
        p.base = name_var(n);
        p.type = n->type;
        return p;
    case N_MEMBER:
        if (is_place(n->a)) {
            p = lower_place(n->a);
        } else { /* a field of a temporary, such as f().x */
            IrVar *v = to_var(lower_expr(n->a), n->line);
            p.base = v;
        }
        add_proj(&p, n->decl, (Operand){0});
        p.type = n->type;
        return p;
    case N_INDEX: {
        p = lower_place(n->a);
        Operand idx = snap(lower_expr(n->b)); /* the place may be used after later arguments run */
        if (idx.var) {
            Instr *i = emit(I_BOUNDS, n->line);
            i->a = idx;
            i->len = n->a->type->len;
        } /* a constant index was checked at compile time */
        add_proj(&p, NULL, idx);
        p.type = n->type;
        return p;
    }
    default:
        die(n->line, "internal error: not a place");
    }
}

/* ---------- expressions ---------- */

static Operand lower_call(Node *n) {
    Node *fn = n->decl;
    int cnt = 0;
    for (Node *a = n->a; a; a = a->next) cnt++;
    Arg *args = zalloc(sizeof(Arg) * (size_t)(cnt ? cnt : 1));
    Node *p = fn->a;
    int k = 0;
    for (Node *a = n->a; a; a = a->next, p = p->next, k++) {
        Arg *g = &args[k];
        if (p->op == M_INOUT || p->op == M_SET) {
            g->addr = true;
            g->place = lower_place(a->a);
        } else if (p->op == M_LET && is_aggregate(p->type)) { /* borrowed: pass its address */
            g->addr = true;
            Node *sr = static_root(a);
            if (is_place(a) && !(sr && sr->op)) {
                g->place = lower_place(a);
            } else {
                IrVar *v = to_var(lower_expr(a), a->line);
                g->place.base = v;
                g->place.type = v->type;
            }
        } else {
            Operand o = lower_expr(a);
            if (list_has_call(a->next)) o = snap(o);
            g->val = o;
        }
    }
    Instr *i = emit(I_CALL, n->line);
    i->callee = fn;
    i->args = args;
    i->nargs = cnt;
    if (n->type == ty_unit) return op_unit();
    IrVar *t = new_temp(n->type);
    i->dst = t;
    i->fresh = true;
    return op_var(t);
}

static Operand lower_struct_lit(Node *n) {
    Node *s = n->decl;
    int nf = 0, ni = 0;
    for (Node *f = s->a; f; f = f->next) nf++;
    for (Node *in = n->a; in; in = in->next) ni++;
    Operand *vals = zalloc(sizeof(Operand) * (size_t)ni);
    int k = 0;
    for (Node *in = n->a; in; in = in->next, k++) { /* source order */
        vals[k] = lower_expr(in->a);
        if (inits_have_call(in->next)) vals[k] = snap(vals[k]);
    }
    Operand *ops = zalloc(sizeof(Operand) * (size_t)nf);
    int j = 0;
    for (Node *f = s->a; f; f = f->next, j++) { /* declaration order */
        int idx = 0;
        for (Node *in = n->a; in; in = in->next, idx++)
            if (str_eq(in->name, f->name)) {
                ops[j] = vals[idx];
                break;
            }
    }
    IrVar *t = new_temp(n->type);
    Instr *i = emit(I_MKSTRUCT, n->line);
    i->dst = t;
    i->fresh = true;
    i->ops = ops;
    i->nops = nf;
    i->type = n->type;
    return op_var(t);
}

static Operand lower_array_lit(Node *n) {
    int cnt = 0;
    for (Node *e = n->a; e; e = e->next) cnt++;
    Operand *ops = zalloc(sizeof(Operand) * (size_t)cnt);
    int k = 0;
    for (Node *e = n->a; e; e = e->next, k++) {
        ops[k] = lower_expr(e);
        if (list_has_call(e->next)) ops[k] = snap(ops[k]);
    }
    IrVar *t = new_temp(n->type);
    Instr *i = emit(I_MKARRAY, n->line);
    i->dst = t;
    i->fresh = true;
    i->ops = ops;
    i->nops = cnt;
    i->type = n->type;
    return op_var(t);
}

/* ---------- enums and match ---------- */

static void lower_stmts(Node *list);

static int list_len(Node *l) {
    int n = 0;
    for (; l; l = l->next) n++;
    return n;
}

static bool irrefutable(const Node *p) { return p->op == PAT_WILD || p->op == PAT_BIND; }

/* A place one step further in. The chain is copied, so the original stays usable for other steps. */
static IrPlace extend(IrPlace p, int kind, Node *variant, int fidx, Type *type) {
    Proj *copy = NULL, **tail = &copy;
    for (Proj *q = p.proj; q; q = q->next) {
        Proj *c = ARENA_NEW(&g_arena, Proj);
        *c = *q;
        c->next = NULL;
        *tail = c;
        tail = &c->next;
    }
    Proj *np = ARENA_NEW(&g_arena, Proj);
    np->kind = kind;
    np->variant = variant;
    np->fidx = fidx;
    *tail = np;
    p.proj = copy;
    p.type = type;
    return p;
}

static IrVar *load_to_temp(IrPlace p, Type *t, int line) {
    IrVar *v = new_temp(t);
    Instr *i = emit(I_LOAD, line);
    i->dst = v;
    i->fresh = true;
    i->place = p;
    return v;
}

static IrVar *decl_var(Type *t, bool has_init, Operand init, int line) {
    IrVar *v = new_temp(t);
    Instr *i = emit(I_DECL, line);
    i->dst = v;
    i->has_init = has_init;
    if (has_init) i->a = init;
    return v;
}

static void store_to(IrVar *v, Operand val, int line) {
    Instr *i = emit(I_STORE, line);
    i->place.base = v;
    i->place.type = v->type;
    i->a = val;
}

static IrVar *compare(int op, Operand a, Operand b, Type *operand_type, int line) {
    IrVar *t = new_temp(ty_bool);
    Instr *i = emit(I_BIN, line);
    i->bop = op;
    i->a = a;
    i->b = b;
    i->type = operand_type;
    i->dst = t;
    i->fresh = true;
    return t;
}

static IrVar *not_of(IrVar *v, int line) {
    IrVar *t = new_temp(ty_bool);
    Instr *i = emit(I_UN, line);
    i->bop = T_BANG;
    i->a = op_var(v);
    i->type = ty_bool;
    i->dst = t;
    i->fresh = true;
    return t;
}

/* the tag of an enum value, loaded into a temporary; it is the discriminant of the variant */
static IrVar *tag_of(Operand o, Type *enum_type, int line) {
    IrVar *v = to_var(o, line);
    IrPlace p = {0};
    p.base = v;
    p.type = v->type;
    return load_to_temp(extend(p, PK_TAG, NULL, 0, enum_type->tag_type), enum_type->tag_type, line);
}

/* Declare the names a pattern binds, each a copy of the part of the value it names. */
static void lower_bind(Node *p, IrPlace place, int line) {
    switch (p->op) {
    case PAT_BIND: {
        IrVar *copy = load_to_temp(place, p->type, line);
        IrVar *u = ARENA_NEW(&g_arena, IrVar);
        u->id = p->local;
        u->type = p->type;
        u->name = p->name;
        vars[p->local] = u;
        Instr *d = emit(I_DECL, line);
        d->dst = u;
        d->a = op_var(copy);
        d->has_init = true;
        break;
    }
    case PAT_VARIANT: {
        int k = 0;
        Node *f = p->decl->a;
        for (Node *s = p->a; s; s = s->next, f = f->next, k++)
            lower_bind(s, extend(place, PK_PAYLOAD, p->decl, k, f->type), line);
        break;
    }
    default:
        break;
    }
}

static bool has_or(const Node *p) {
    if (p->op == PAT_OR) return true;
    if (p->op == PAT_VARIANT)
        for (const Node *s = p->a; s; s = s->next)
            if (has_or(s)) return true;
    return false;
}

typedef struct {
    Node *pat;
    IrPlace place; /* where the value this pattern is matched against lives */
} PatItem;

/* One arm of a match being lowered, and what it does once its pattern has fitted. */
typedef struct {
    Node *arm;
    IrPlace scrutinee;
    IrVar *pending; /* true until an arm has run (none for a match with a single arm) */
    IrVar *res;     /* the value of a match that is used as a value */
    bool stmt;
} ArmCtx;

/* Bind the arm's names, run its body (or compute its value), and note that an arm has run. */
static void run_arm(ArmCtx *c) {
    Node *arm = c->arm;
    lower_bind(arm->a, c->scrutinee, arm->line);
    if (arm->op) {
        lower_stmts(arm->b->a);
    } else {
        Operand v = lower_expr(arm->b);
        if (!c->stmt) {
            store_to(c->res, v, arm->line);
        } else if (v.var && v.type != ty_unit) { /* a statement drops the value */
            Instr *u = emit(I_USE, arm->line);
            u->a = v;
        }
    }
    if (arm->next && !(blk->last && blk->last->op == I_RETURN)) /* the last arm, or one that returns, need not say so */
        store_to(c->pending, op_const(ty_bool, 0), arm->line);
}

/* Test the patterns in items[0..n) one after the other. Each test is an `if` nested in the one before, so a
 * failed test skips everything that depends on it. When all have fitted, either `m` is set to true (a pattern
 * with alternatives is tried more than once, and the arm must not be repeated for each) or, with no `m`, the arm
 * itself runs right there inside the innermost `if`. */
static void lower_test(PatItem *items, int n, IrVar *m, ArmCtx *ctx, int line) {
    if (n == 0) {
        if (m) store_to(m, op_const(ty_bool, 1), line);
        else run_arm(ctx);
        return;
    }
    PatItem it = items[0];
    Node *p = it.pat;
    switch (p->op) {
    case PAT_WILD:
    case PAT_BIND:
        lower_test(items + 1, n - 1, m, ctx, line);
        return;
    case PAT_LIT: {
        IrVar *v = load_to_temp(it.place, p->type, line);
        Operand lit = lower_expr(p->a); /* the checker typed and folded it (-5 included) */
        IrVar *eq = compare(T_EQ, op_var(v), lit, p->type, line);
        Instr *c = emit(I_IF, line);
        c->a = op_var(eq);
        Block *save = blk;
        blk = &c->then;
        lower_test(items + 1, n - 1, m, ctx, line);
        blk = save;
        return;
    }
    case PAT_VARIANT: {
        Type *et = p->type;
        IrPlace tp = extend(it.place, PK_TAG, NULL, 0, et->tag_type);
        IrVar *tag = load_to_temp(tp, et->tag_type, line);
        IrVar *eq = compare(T_EQ, op_var(tag), op_const(et->tag_type, p->decl->ival), et->tag_type, line);
        Instr *c = emit(I_IF, line);
        c->a = op_var(eq);
        Block *save = blk;
        blk = &c->then;
        int ns = list_len(p->a), k = 0;
        PatItem *next = zalloc(sizeof(PatItem) * (size_t)(ns + n - 1 ? ns + n - 1 : 1));
        Node *f = p->decl->a;
        for (Node *s = p->a; s; s = s->next, f = f->next, k++) { /* the values come first, then the rest */
            next[k].pat = s;
            next[k].place = extend(it.place, PK_PAYLOAD, p->decl, k, f->type);
        }
        for (int r = 1; r < n; r++) next[k++] = items[r];
        lower_test(next, k, m, ctx, line);
        blk = save;
        return;
    }
    case PAT_OR:
        for (Node *alt = p->a; alt; alt = alt->next) {
            IrVar *open = not_of(m, line); /* only if no earlier alternative has fitted */
            Instr *c = emit(I_IF, line);
            c->a = op_var(open);
            Block *save = blk;
            blk = &c->then;
            PatItem *next = zalloc(sizeof(PatItem) * (size_t)n);
            next[0].pat = alt;
            next[0].place = it.place;
            for (int r = 1; r < n; r++) next[r] = items[r];
            lower_test(next, n, m, ctx, line);
            blk = save;
        }
        return;
    }
}

/* match: the value is looked at once (it is read where it lives, or evaluated into a temporary). Then each arm
 * in turn, while no arm has run, tests its pattern and runs. An expression arm stores its value in the result. */
static Operand lower_match(Node *m) {
    fn_has_match = true;
    bool stmt = m->flag;
    Node *scrut = m->a;
    int line = m->line;
    IrPlace sp = {0};
    if (is_place(scrut)) {
        sp = lower_place(scrut);
    } else {
        IrVar *v = to_var(lower_expr(scrut), line);
        sp.base = v;
        sp.type = v->type;
    }
    ArmCtx ctx = {0};
    ctx.scrutinee = sp;
    ctx.stmt = stmt;
    ctx.pending = m->b->next ? decl_var(ty_bool, true, op_const(ty_bool, 1), line) : NULL;
    ctx.res = stmt ? NULL : decl_var(m->type, false, (Operand){0}, line);
    for (Node *arm = m->b; arm; arm = arm->next) {
        Block *outer = blk;
        if (arm != m->b) { /* the first arm always gets its turn */
            Instr *c = emit(I_IF, arm->line);
            c->a = op_var(ctx.pending);
            blk = &c->then;
        }
        ctx.arm = arm;
        PatItem item = {arm->a, sp};
        if (irrefutable(arm->a)) {
            run_arm(&ctx);
        } else if (!has_or(arm->a)) {
            lower_test(&item, 1, NULL, &ctx, arm->line);
        } else {
            IrVar *matched = decl_var(ty_bool, true, op_const(ty_bool, 0), arm->line);
            lower_test(&item, 1, matched, &ctx, arm->line);
            Instr *hit = emit(I_IF, arm->line);
            hit->a = op_var(matched);
            blk = &hit->then;
            run_arm(&ctx);
        }
        blk = outer;
    }
    return stmt ? op_unit() : op_var(ctx.res);
}

/* Read a place into a fresh temporary. */
static Operand load_place(Node *n) {
    IrPlace p = lower_place(n);
    IrVar *t = new_temp(n->type);
    Instr *i = emit(I_LOAD, n->line);
    i->dst = t;
    i->fresh = true;
    i->place = p;
    return op_var(t);
}

static Operand lower_expr(Node *n) {
    switch (n->kind) {
    case N_INT:
        return op_const(n->type, n->op ? (uint64_t)0 - n->ival : n->ival);
    case N_BOOL:
        return op_const(ty_bool, n->ival);
    case N_NAME:
        if (is_static_name(n)) return load_place(n); /* a static is read where it is, once */
        if (!vars[n->local]->by_ptr) return op_var(vars[n->local]); /* use the variable itself */
        return load_place(n);                                       /* a pointer parameter: read through it */
    case N_MEMBER:
    case N_INDEX:
        return load_place(n);
    case N_UN: {
        Operand a = lower_expr(n->a);
        IrVar *t = new_temp(n->type);
        Instr *i = emit(I_UN, n->line);
        i->bop = n->op;
        i->a = a;
        i->type = n->a->type;
        i->dst = t;
        i->fresh = true;
        return op_var(t);
    }
    case N_CAST: {
        Operand a = lower_expr(n->a);
        if (n->a->type->kind == TY_ENUM) a = op_var(tag_of(a, n->a->type, n->line)); /* its discriminant */
        IrVar *t = new_temp(n->type);
        Instr *i = emit(I_CAST, n->line);
        i->a = a;
        i->dst = t;
        i->fresh = true;
        return op_var(t);
    }
    case N_BIN: {
        if (n->op == T_ANDAND || n->op == T_OROR) {
            Operand a = lower_expr(n->a);
            IrVar *t = new_temp(ty_bool);
            Instr *d = emit(I_DECL, n->line);
            d->dst = t;
            d->a = a;
            d->has_init = true;
            Instr *c = emit(I_IF, n->line);
            c->a = op_var(t);
            Block *save = blk;
            blk = n->op == T_ANDAND ? &c->then : &c->els; /* && continues when true, || when false */
            Operand b = lower_expr(n->b);
            Instr *s = emit(I_STORE, n->line);
            s->place.base = t;
            s->place.type = ty_bool;
            s->a = b;
            blk = save;
            return op_var(t);
        }
        if (n->a->type->kind == TY_ENUM) { /* == and != on a plain enum compare the tags */
            Operand ea = lower_expr(n->a);
            if (has_call(n->b)) ea = snap(ea);
            Operand eb = lower_expr(n->b);
            IrVar *ta = tag_of(ea, n->a->type, n->line), *tb = tag_of(eb, n->a->type, n->line);
            return op_var(compare(n->op, op_var(ta), op_var(tb), n->a->type->tag_type, n->line));
        }
        Operand a = lower_expr(n->a);
        if (has_call(n->b)) a = snap(a);
        Operand b = lower_expr(n->b);
        IrVar *t = new_temp(n->type);
        Instr *i = emit(I_BIN, n->line);
        i->bop = n->op;
        i->a = a;
        i->b = b;
        i->type = n->a->type;
        i->dst = t;
        i->fresh = true;
        return op_var(t);
    }
    case N_CALL:
        return lower_call(n);
    case N_VARIANT: {
        int nf = list_len(n->a), k = 0;
        Operand *ops = zalloc(sizeof(Operand) * (size_t)(nf ? nf : 1));
        for (Node *a = n->a; a; a = a->next, k++) { /* the values, left to right */
            ops[k] = lower_expr(a);
            if (list_has_call(a->next)) ops[k] = snap(ops[k]);
        }
        IrVar *t = new_temp(n->type);
        Instr *i = emit(I_MKENUM, n->line);
        i->dst = t;
        i->fresh = true;
        i->ops = ops;
        i->nops = nf;
        i->type = n->type;
        i->variant = n->decl;
        return op_var(t);
    }
    case N_MATCH:
        return lower_match(n);
    case N_CSTR: {
        IrVar *t = new_temp(n->type);
        Instr *i = emit(I_STR, n->line);
        i->dst = t;
        i->fresh = true;
        i->str = n->name;
        return op_var(t);
    }
    case N_PTROP: {
        Operand p = lower_expr(n->a);
        if (n->b && has_call(n->b)) p = snap(p);
        Operand v = n->b ? lower_expr(n->b) : (Operand){0};
        if (n->op == PO_WRITE || n->op == PO_VWRITE) {
            Instr *i = emit(I_PWRITE, n->line);
            i->vol = n->op == PO_VWRITE;
            i->a = p;
            i->b = v;
            i->type = n->b->type;
            return op_unit();
        }
        IrVar *t = new_temp(n->type);
        Instr *i = emit(n->op == PO_OFFSET ? I_POFFSET : I_PREAD, n->line);
        i->vol = n->op == PO_VREAD;
        i->a = p;
        i->b = v;
        i->dst = t;
        i->fresh = true;
        return op_var(t);
    }
    case N_LIT:
        return lower_struct_lit(n);
    case N_ARRAY:
        return lower_array_lit(n);
    case N_REPEAT: {
        Operand v = lower_expr(n->a);
        IrVar *t = new_temp(n->type);
        Instr *i = emit(I_FILL, n->line);
        i->dst = t;
        i->fresh = true;
        i->a = v;
        i->len = n->type->len;
        i->type = n->type;
        return op_var(t);
    }
    default:
        die(n->line, "internal error: cannot lower expression kind %d", n->kind);
    }
}

/* ---------- statements ---------- */

static void lower_stmts(Node *list);

static void lower_stmt(Node *s) {
    switch (s->kind) {
    case N_LET: {
        IrVar *v = ARENA_NEW(&g_arena, IrVar);
        v->id = s->local;
        v->type = s->type;
        v->name = s->name;
        Instr *i;
        if (s->a) {
            Operand init = lower_expr(s->a);
            /* An array that was just built ([a, b, c] or [v; N]) goes straight into the new variable. Declaring the
             * variable from a temporary would keep two copies of it on the stack and copy one into the other:
             * 32 MB of stack and a 16 MB copy for a 16 MB array. */
            Instr *built = blk->last;
            if ((s->a->kind == N_REPEAT || s->a->kind == N_ARRAY) && init.var && init.var->is_temp && built &&
                built->dst == init.var && built->fresh && (built->op == I_FILL || built->op == I_MKARRAY)) {
                built->dst = v;
                Instr *use = emit(I_USE, s->line); /* the C compiler must not warn that the variable is never read */
                use->a = op_var(v);
                vars[s->local] = v;
                break;
            }
            i = emit(I_DECL, s->line);
            i->a = init;
            i->has_init = true;
        } else {
            i = emit(I_DECL, s->line);
        }
        i->dst = v;
        vars[s->local] = v; /* declared after its initializer: `let x = x + 1` sees the old x */
        break;
    }
    case N_ASSIGN: {
        Operand v = lower_expr(s->b); /* right side first */
        IrPlace p = lower_place(s->a);
        Instr *i = emit(I_STORE, s->line);
        i->place = p;
        i->a = v;
        break;
    }
    case N_EXPR: {
        Operand o = lower_expr(s->a);
        if (o.var && o.type != ty_unit) {
            Instr *i = emit(I_USE, s->line);
            i->a = o;
        }
        break;
    }
    case N_IF: {
        Operand c = lower_expr(s->a);
        Instr *i = emit(I_IF, s->line);
        i->a = c;
        Block *save = blk;
        blk = &i->then;
        lower_stmts(s->b->a);
        blk = &i->els;
        if (s->c) {
            if (s->c->kind == N_IF) lower_stmt(s->c);
            else lower_stmts(s->c->a);
        }
        blk = save;
        break;
    }
    case N_WHILE: {
        Instr *i = emit(I_WHILE, s->line);
        Block *save = blk;
        blk = &i->cond;
        i->a = lower_expr(s->a);
        blk = &i->body;
        lower_stmts(s->b->a);
        blk = save;
        break;
    }
    case N_RETURN: {
        Operand o = s->a ? lower_expr(s->a) : op_unit();
        Instr *i = emit(I_RETURN, s->line);
        i->a = o;
        break;
    }
    case N_MATCH:
        lower_match(s);
        break;
    case N_BLOCK: {
        Instr *i = emit(I_BLOCK, s->line);
        Block *save = blk;
        blk = &i->then;
        lower_stmts(s->a);
        blk = save;
        break;
    }
    default:
        die(s->line, "internal error: cannot lower statement kind %d", s->kind);
    }
}

static void lower_stmts(Node *list) {
    for (Node *s = list; s; s = s->next) lower_stmt(s);
}

static IrFunc *lower_fn(Node *fn) {
    IrFunc *f = ARENA_NEW(&g_arena, IrFunc);
    f->decl = fn;
    f->ret = fn->type;
    int np = 0;
    for (Node *p = fn->a; p; p = p->next) np++;
    f->params = zalloc(sizeof(IrVar *) * (size_t)(np ? np : 1));
    f->nparams = np;
    vars = zalloc(sizeof(IrVar *) * (size_t)(fn->nlocals ? fn->nlocals : 1));
    next_temp = 0;
    int k = 0;
    for (Node *p = fn->a; p; p = p->next, k++) {
        IrVar *v = ARENA_NEW(&g_arena, IrVar);
        bool agg_let = p->op == M_LET && is_aggregate(p->type);
        v->id = p->local;
        v->type = p->type;
        v->name = p->name;
        v->by_ptr = p->op == M_INOUT || p->op == M_SET || agg_let;
        v->is_const = agg_let;
        vars[p->local] = v;
        f->params[k] = v;
    }
    blk = &f->body;
    fn_has_match = false;
    lower_stmts(fn->b->a);
    /* The checker knows that every path returns, but a C compiler cannot follow that through the flags a
     * match uses, and would warn that control reaches the end. Close the function with a return that never runs. */
    if (fn_has_match && f->ret != ty_unit && !(f->body.last && f->body.last->op == I_RETURN)) {
        Operand zero = is_aggregate(f->ret) || f->ret->kind == TY_PTR ? op_var(decl_var(f->ret, false, (Operand){0}, fn->b->end_line))
                                                                       : op_const(f->ret, 0);
        Instr *r = emit(I_RETURN, fn->b->end_line);
        r->a = zero;
    }
    return f;
}

IrFunc *lower_program(Node *prog) {
    IrFunc *head = NULL, **tail = &head;
    for (Node *d = prog; d; d = d->next) {
        if (d->kind != N_FN || d->ext == EXT_EXTERN) continue; /* extern functions live in C */
        *tail = lower_fn(d);
        tail = &(*tail)->next;
    }
    return head;
}
