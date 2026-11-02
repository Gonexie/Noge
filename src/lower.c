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
    pr->field = field;
    pr->idx = idx;
    Proj **tail = &p->proj;
    while (*tail) tail = &(*tail)->next;
    *tail = pr;
}

static Operand lower_expr(Node *n);

static IrPlace lower_place(Node *n) {
    IrPlace p = {0};
    switch (n->kind) {
    case N_NAME:
        p.base = vars[n->local];
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
            if (is_place(a)) {
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
    lower_stmts(fn->b->a);
    return f;
}

IrFunc *lower_program(Node *prog) {
    IrFunc *head = NULL, **tail = &head;
    for (Node *d = prog; d; d = d->next) {
        if (d->kind != N_FN) continue;
        *tail = lower_fn(d);
        tail = &(*tail)->next;
    }
    return head;
}
