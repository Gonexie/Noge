/* C99 backend: prints the IR as plain C.
 *
 * Every checked operation becomes a few explicit statements that end in
 * NOGE_TRAP(kind, line) when the check fails. The program may define
 * NOGE_TRAP before including this file's output (for example with
 * -DNOGE_TRAP=...) to reset the chip or log instead of exiting. The hook must
 * not return.
 */
#include "ir.h"

static FILE *out;
static const EmitOpts *opts;

enum { TR_OVERFLOW = 1, TR_DIV_ZERO, TR_BOUNDS, TR_SHIFT };
static const char *const trap_names[] = {"", "NOGE_TRAP_OVERFLOW", "NOGE_TRAP_DIV_ZERO",
                                         "NOGE_TRAP_BOUNDS", "NOGE_TRAP_SHIFT"};

static void ind(int d) {
    for (int i = 0; i < d; i++) fputs("    ", out);
}

/* ---------- names and constants ---------- */

static void put_var(IrVar *v) {
    if (v->gname) fputs(v->gname, out);
    else if (v->is_temp) fprintf(out, "t%d", v->id);
    else fprintf(out, "v%d_%.*s", v->id, SF(v->name));
}

static void put_operand(Operand o) {
    if (o.var) {
        put_var(o.var);
        return;
    }
    Type *t = o.type;
    if (t == ty_bool) {
        fputs(o.c ? "true" : "false", out);
    } else if (t->is_signed) {
        int64_t v = (int64_t)o.c;
        if (v == INT64_MIN) fputs("INT64_MIN", out);
        else if (v < 0) fprintf(out, "((%s)(%lldLL))", t->cname, (long long)v);
        else fprintf(out, "((%s)%lldLL)", t->cname, (long long)v);
    } else {
        fprintf(out, "((%s)%lluULL)", t->cname, (unsigned long long)o.c);
    }
}

static bool is_const(Operand o) { return !o.var; }

static void put_place(const IrPlace *p) {
    if (p->base->by_ptr) {
        fputs("(*", out);
        put_var(p->base);
        fputc(')', out);
    } else {
        put_var(p->base);
    }
    for (Proj *pr = p->proj; pr; pr = pr->next) {
        switch (pr->kind) {
        case PK_FIELD:
            fprintf(out, ".m_%.*s", SF(pr->field->name));
            break;
        case PK_INDEX:
            fputs(".v[", out);
            put_operand(pr->idx);
            fputc(']', out);
            break;
        case PK_TAG:
            fputs(".tag", out);
            break;
        default: /* PK_PAYLOAD */
            fprintf(out, ".u.v_%.*s.f%d", SF(pr->variant->name), pr->fidx);
            break;
        }
    }
}

static void put_addr(const IrPlace *p) {
    if (!p->proj && p->base->by_ptr) { /* already a pointer */
        put_var(p->base);
        return;
    }
    fputs("(&", out);
    put_place(p);
    fputc(')', out);
}

/* the C name of a function: f_name, x_name for one defined in C, or f<id>_name for an instance of a generic one */
static void put_fn_ident(Node *fn) {
    if (fn->inst) fprintf(out, "f%x_%.*s", fn->inst->id, SF(fn->name));
    else if (fn->mod != 0 && fn->ext != EXT_EXTERN) fprintf(out, "fm%s_%.*s", g_modules[fn->mod]->tag, SF(fn->name)); /* another module */
    else fprintf(out, "%c_%.*s", fn->ext == EXT_EXTERN ? 'x' : 'f', SF(fn->name));
}

static void put_zero(Type *t) { fputs(is_aggregate(t) ? "{0}" : "0", out); }

static void put_min(Type *t) {
    if (t->is_signed) fprintf(out, "INT%d_MIN", t->bits);
    else fputs("0", out);
}

static void put_max(Type *t) { fprintf(out, "%sINT%d_MAX", t->is_signed ? "" : "U", t->bits); }

static void put_trap(int kind, int line) { fprintf(out, "NOGE_TRAP(%s, %d)", trap_names[kind], line); }

/* "T name = " when the instruction declares its result, otherwise "name = " */
static void put_def(Instr *i, int d) {
    ind(d);
    if (i->fresh) fprintf(out, "%s ", i->dst->type->cname);
    put_var(i->dst);
    fputs(" = ", out);
}

static void put_decl_only(Instr *i, int d) {
    if (!i->fresh) return;
    ind(d);
    fprintf(out, "%s ", i->dst->type->cname);
    put_var(i->dst);
    fputs(";\n", out);
}

/* the unsigned type used for wrapping arithmetic and shifts of t */
static const char *wide_unsigned(Type *t) { return t->bits <= 32 ? "uint32_t" : "uint64_t"; }

/* ---------- checked arithmetic ---------- */

static void arith_portable(Instr *i, int d) {
    Type *t = i->type;
    int op = i->bop;
    bool s = t->is_signed;
    Operand a = i->a, b = i->b;
    const char *sym = op == T_PLUS ? "+" : op == T_MINUS ? "-" : "*";
    ind(d);
    if (t->bits <= 32 && !(op == T_STAR && !s && t->bits == 32)) { /* widen: cannot overflow int64 */
        fputs("{ int64_t w_ = (int64_t)", out);
        put_operand(a);
        fprintf(out, " %s (int64_t)", sym);
        put_operand(b);
        fputs("; if (w_ < ", out);
        put_min(t);
        fputs(" || w_ > ", out);
        put_max(t);
        fputs(") ", out);
        put_trap(TR_OVERFLOW, i->line);
        fputs("; ", out);
        put_var(i->dst);
        fprintf(out, " = (%s)w_; }\n", t->cname);
        return;
    }
    if (t->bits <= 32) { /* u32 * u32 fits in uint64 */
        fputs("{ uint64_t w_ = (uint64_t)", out);
        put_operand(a);
        fputs(" * (uint64_t)", out);
        put_operand(b);
        fputs("; if (w_ > UINT32_MAX) ", out);
        put_trap(TR_OVERFLOW, i->line);
        fputs("; ", out);
        put_var(i->dst);
        fprintf(out, " = (%s)w_; }\n", t->cname);
        return;
    }
    /* 64-bit: test before (or, for unsigned add, after) the operation. The guard and the
       assignment go on separate lines so the output stays free of misleading-indentation warnings. */
    if (!s && op == T_PLUS) {
        put_var(i->dst), fputs(" = ", out), put_operand(a), fputs(" + ", out), put_operand(b), fputs(";\n", out);
        ind(d);
        fputs("if (", out), put_var(i->dst), fputs(" < ", out), put_operand(a), fputs(") ", out);
    } else if (!s && op == T_MINUS) {
        fputs("if (", out), put_operand(a), fputs(" < ", out), put_operand(b), fputs(") ", out);
    } else if (!s) {
        fputs("if (", out), put_operand(a), fputs(" != 0 && ", out), put_operand(b);
        fputs(" > UINT64_MAX / ", out), put_operand(a), fputs(") ", out);
    } else if (op == T_PLUS || op == T_MINUS) {
        bool add = op == T_PLUS;
        fputs("if ((", out), put_operand(b), fputs(" > 0 && ", out), put_operand(a);
        fputs(add ? " > INT64_MAX - " : " < INT64_MIN + ", out), put_operand(b);
        fputs(") || (", out), put_operand(b), fputs(" < 0 && ", out), put_operand(a);
        fputs(add ? " < INT64_MIN - " : " > INT64_MAX + ", out), put_operand(b);
        fputs(")) ", out);
    } else {
        fputs("if (", out), put_operand(a), fputs(" > 0 ? (", out), put_operand(b);
        fputs(" > 0 ? ", out), put_operand(a), fputs(" > INT64_MAX / ", out), put_operand(b);
        fputs(" : ", out), put_operand(b), fputs(" < INT64_MIN / ", out), put_operand(a);
        fputs(") : (", out), put_operand(b), fputs(" > 0 ? ", out), put_operand(a);
        fputs(" < INT64_MIN / ", out), put_operand(b), fputs(" : (", out), put_operand(a);
        fputs(" != 0 && ", out), put_operand(b), fputs(" < INT64_MAX / ", out), put_operand(a);
        fputs("))) ", out);
    }
    put_trap(TR_OVERFLOW, i->line);
    fputs(";\n", out);
    if (s || op != T_PLUS) { /* the operation itself, once the guard has passed */
        ind(d);
        put_var(i->dst), fputs(" = ", out), put_operand(a), fprintf(out, " %s ", sym), put_operand(b);
        fputs(";\n", out);
    }
}

static void emit_arith(Instr *i, int d) {
    put_decl_only(i, d);
    if (opts->portable) {
        arith_portable(i, d);
        return;
    }
    ind(d);
    fprintf(out, "if (__builtin_%s_overflow(", i->bop == T_PLUS ? "add" : i->bop == T_MINUS ? "sub" : "mul");
    put_operand(i->a);
    fputs(", ", out);
    put_operand(i->b);
    fputs(", &", out);
    put_var(i->dst);
    fputs(")) ", out);
    put_trap(TR_OVERFLOW, i->line);
    fputs(";\n", out);
}

static void emit_divrem(Instr *i, int d) {
    Type *t = i->type;
    if (!(is_const(i->b) && i->b.c != 0)) {
        ind(d);
        fputs("if (", out);
        put_operand(i->b);
        fputs(" == 0) ", out);
        put_trap(TR_DIV_ZERO, i->line);
        fputs(";\n", out);
    }
    if (t->is_signed && !(is_const(i->b) && (int64_t)i->b.c != -1)) { /* MIN / -1 does not fit */
        ind(d);
        fputs("if (", out);
        put_operand(i->a);
        fprintf(out, " == INT%d_MIN && ", t->bits);
        put_operand(i->b);
        fputs(" == -1) ", out);
        put_trap(TR_OVERFLOW, i->line);
        fputs(";\n", out);
    }
    put_def(i, d);
    fprintf(out, "(%s)(", t->cname);
    put_operand(i->a);
    fputs(i->bop == T_SLASH ? " / " : " % ", out);
    put_operand(i->b);
    fputs(");\n", out);
}

static void emit_shift(Instr *i, int d) {
    Type *t = i->type;
    if (!is_const(i->b)) { /* a negative count wraps to a huge unsigned value and fails too */
        ind(d);
        fputs("if ((uint64_t)", out);
        put_operand(i->b);
        fprintf(out, " >= %dU) ", t->bits);
        put_trap(TR_SHIFT, i->line);
        fputs(";\n", out);
    }
    put_def(i, d);
    if (i->bop == T_SHL) { /* shift the unsigned bit pattern: no signed-overflow UB */
        fprintf(out, "(%s)((%s)", t->cname, wide_unsigned(t));
        put_operand(i->a);
        fputs(" << ", out);
        put_operand(i->b);
        fputs(");\n", out);
    } else {
        fprintf(out, "(%s)(", t->cname);
        put_operand(i->a);
        fputs(" >> ", out);
        put_operand(i->b);
        fputs(");\n", out);
    }
}

static const char *c_cmp(int op) {
    switch (op) {
    case T_EQ: return "==";
    case T_NE: return "!=";
    case T_LT: return "<";
    case T_LE: return "<=";
    case T_GT: return ">";
    default: return ">=";
    }
}

static void emit_bin(Instr *i, int d) {
    Type *t = i->type;
    switch (i->bop) {
    case T_EQ: case T_NE: case T_LT: case T_LE: case T_GT: case T_GE:
        put_def(i, d);
        fputc('(', out);
        put_operand(i->a);
        fprintf(out, " %s ", c_cmp(i->bop));
        put_operand(i->b);
        fputs(");\n", out);
        break;
    case T_PLUS: case T_MINUS: case T_STAR:
        emit_arith(i, d);
        break;
    case T_SLASH: case T_PERCENT:
        emit_divrem(i, d);
        break;
    case T_SHL: case T_SHR:
        emit_shift(i, d);
        break;
    case T_AMP: case T_PIPE: case T_CARET:
        put_def(i, d);
        fprintf(out, "(%s)(", t->cname);
        put_operand(i->a);
        fprintf(out, " %c ", i->bop == T_AMP ? '&' : i->bop == T_PIPE ? '|' : '^');
        put_operand(i->b);
        fputs(");\n", out);
        break;
    default: /* wrapping_add / sub / mul: unsigned arithmetic, then truncate */
        put_def(i, d);
        fprintf(out, "(%s)((%s)", t->cname, wide_unsigned(t));
        put_operand(i->a);
        fprintf(out, " %c (%s)", i->bop == OP_WADD ? '+' : i->bop == OP_WSUB ? '-' : '*', wide_unsigned(t));
        put_operand(i->b);
        fputs(");\n", out);
        break;
    }
}

static void emit_un(Instr *i, int d) {
    Type *t = i->type;
    switch (i->bop) {
    case T_BANG:
        put_def(i, d);
        fputs("!", out);
        put_operand(i->a);
        fputs(";\n", out);
        break;
    case T_TILDE:
        put_def(i, d);
        fprintf(out, "(%s)(~", t->cname);
        put_operand(i->a);
        fputs(");\n", out);
        break;
    default: /* negation overflows only for the most negative value */
        ind(d);
        fputs("if (", out);
        put_operand(i->a);
        fprintf(out, " == INT%d_MIN) ", t->bits);
        put_trap(TR_OVERFLOW, i->line);
        fputs(";\n", out);
        put_def(i, d);
        fprintf(out, "(%s)(-", t->cname);
        put_operand(i->a);
        fputs(");\n", out);
        break;
    }
}

/* ---------- instructions ---------- */

static void emit_block(Block *b, int d);

static void emit_operands(Operand *ops, int n) {
    for (int k = 0; k < n; k++) {
        if (k) fputs(", ", out);
        put_operand(ops[k]);
    }
}

static void emit_instr(Instr *i, int d) {
    switch (i->op) {
    case I_DECL:
        ind(d);
        fprintf(out, "%s ", i->dst->type->cname);
        put_var(i->dst);
        fputs(" = ", out);
        if (i->has_init) put_operand(i->a);
        else put_zero(i->dst->type);
        fputc(';', out);
        if (!i->dst->is_temp) {
            fputs(" (void)", out);
            put_var(i->dst);
            fputc(';', out);
        }
        fputc('\n', out);
        break;
    case I_STORE:
        ind(d);
        put_place(&i->place);
        fputs(" = ", out);
        put_operand(i->a);
        fputs(";\n", out);
        break;
    case I_LOAD:
        put_def(i, d);
        put_place(&i->place);
        fputs(";\n", out);
        break;
    case I_BIN:
        emit_bin(i, d);
        break;
    case I_UN:
        emit_un(i, d);
        break;
    case I_CAST: {
        Type *to = i->dst->type, *from = i->a.type;
        put_def(i, d);
        if ((to->kind == TY_PTR) != (from->kind == TY_PTR)) /* integer <-> pointer goes through uintptr_t */
            fprintf(out, "(%s)(uintptr_t)", to->cname);
        else
            fprintf(out, "(%s)", to->cname);
        put_operand(i->a);
        fputs(";\n", out);
        break;
    }
    case I_STR: {
        put_def(i, d);
        fprintf(out, "(%s)\"", i->dst->type->cname);
        for (int k = 0; k < i->str.n; k++) {
            unsigned char c = (unsigned char)i->str.p[k];
            if (c == '"' || c == '\\' || c == '?') fprintf(out, "\\%c", c); /* \? keeps trigraphs out */
            else if (c >= 32 && c < 127) fputc(c, out);
            else fprintf(out, "\\%03o", c);
        }
        fputs("\";\n", out);
        break;
    }
    case I_PREAD:
        put_def(i, d);
        if (i->vol) fprintf(out, "*(volatile %s *)", i->dst->type->cname);
        else fputc('*', out);
        put_operand(i->a);
        fputs(";\n", out);
        break;
    case I_PWRITE:
        ind(d);
        if (i->vol) fprintf(out, "*(volatile %s *)", i->type->cname);
        else fputc('*', out);
        put_operand(i->a);
        fputs(" = ", out);
        put_operand(i->b);
        fputs(";\n", out);
        break;
    case I_POFFSET:
        put_def(i, d);
        put_operand(i->a);
        fputs(" + ", out);
        put_operand(i->b);
        fputs(";\n", out);
        break;
    case I_BOUNDS:
        ind(d);
        fputs("if ((uint64_t)", out);
        put_operand(i->a);
        fprintf(out, " >= %lluULL) ", (unsigned long long)i->len);
        put_trap(TR_BOUNDS, i->line);
        fputs(";\n", out);
        break;
    case I_MKSTRUCT: {
        put_def(i, d);
        fprintf(out, "(%s){ ", i->type->cname);
        int k = 0;
        for (Node *f = i->type->decl->a; f; f = f->next, k++) {
            if (k) fputs(", ", out);
            fprintf(out, ".m_%.*s = ", SF(f->name));
            put_operand(i->ops[k]);
        }
        fputs(" };\n", out);
        break;
    }
    case I_MKENUM: {
        Operand tag = {0};
        tag.c = i->variant->ival;
        tag.type = i->type->tag_type;
        put_def(i, d);
        fprintf(out, "(%s){ .tag = ", i->type->cname);
        put_operand(tag);
        if (i->nops) {
            fprintf(out, ", .u.v_%.*s = { ", SF(i->variant->name));
            for (int k = 0; k < i->nops; k++) {
                if (k) fputs(", ", out);
                fprintf(out, ".f%d = ", k);
                put_operand(i->ops[k]);
            }
            fputs(" }", out);
        }
        fputs(" };\n", out);
        break;
    }
    case I_MKARRAY:
        put_def(i, d);
        fprintf(out, "(%s){ { ", i->type->cname);
        emit_operands(i->ops, i->nops);
        fputs(" } };\n", out);
        break;
    case I_FILL:
        if (is_const(i->a) && i->a.c == 0) { /* the common [0; N] */
            put_def(i, d);
            fputs("{0};\n", out);
            break;
        }
        put_decl_only(i, d);
        ind(d);
        fputs("{ uint64_t k_; for (k_ = 0; k_ < ", out);
        fprintf(out, "%lluULL; k_++) ", (unsigned long long)i->len);
        put_var(i->dst);
        fputs(".v[k_] = ", out);
        put_operand(i->a);
        fputs("; }\n", out);
        break;
    case I_CALL:
        if (i->dst) put_def(i, d);
        else ind(d);
        put_fn_ident(i->callee);
        fputc('(', out);
        for (int k = 0; k < i->nargs; k++) {
            if (k) fputs(", ", out);
            if (!i->args[k].addr) {
                put_operand(i->args[k].val);
            } else if (i->callee->ext == EXT_EXTERN && i->args[k].place.type->kind == TY_ARRAY) {
                put_place(&i->args[k].place); /* C wants a pointer to the first element */
                fputs(".v", out);
            } else {
                put_addr(&i->args[k].place);
            }
        }
        fputs(");\n", out);
        break;
    case I_USE:
        ind(d);
        fputs("(void)", out);
        put_operand(i->a);
        fputs(";\n", out);
        break;
    case I_IF:
        ind(d);
        if (!i->then.first && i->els.first) { /* from `||`: run the right side when false */
            fputs("if (!", out);
            put_operand(i->a);
            fputs(") {\n", out);
            emit_block(&i->els, d + 1);
            ind(d);
            fputs("}\n", out);
            break;
        }
        fputs("if (", out);
        put_operand(i->a);
        fputs(") {\n", out);
        emit_block(&i->then, d + 1);
        ind(d);
        if (i->els.first) {
            fputs("} else {\n", out);
            emit_block(&i->els, d + 1);
            ind(d);
        }
        fputs("}\n", out);
        break;
    case I_WHILE:
        ind(d);
        fputs("for (;;) {\n", out);
        emit_block(&i->cond, d + 1);
        ind(d + 1);
        fputs("if (!", out);
        put_operand(i->a);
        fputs(") break;\n", out);
        emit_block(&i->body, d + 1);
        ind(d);
        fputs("}\n", out);
        break;
    case I_RETURN:
        ind(d);
        if (i->a.type == ty_unit) {
            fputs("return;\n", out);
        } else {
            fputs("return ", out);
            put_operand(i->a);
            fputs(";\n", out);
        }
        break;
    case I_BLOCK:
        ind(d);
        fputs("{\n", out);
        emit_block(&i->then, d + 1);
        ind(d);
        fputs("}\n", out);
        break;
    }
}

static void emit_block(Block *b, int d) {
    for (Instr *i = b->first; i; i = i->next) emit_instr(i, d);
}

/* ---------- types, functions, prelude ---------- */

static void emit_type_def(Type *t) { /* components first, so by-value members are complete */
    if (t->mark) return;
    if (t->kind != TY_STRUCT && t->kind != TY_ARRAY && t->kind != TY_ENUM) return;
    t->mark = 1;
    if (t->kind == TY_ENUM) { /* a tag, and a union with one member per variant that carries values */
        bool any = false;
        for (Node *v = t->decl->a; v; v = v->next) {
            for (Node *f = v->a; f; f = f->next) emit_type_def(f->type);
            any |= v->a != NULL;
        }
        fprintf(out, "struct %s {\n    %s tag;\n", t->cname, t->tag_type->cname);
        if (any) {
            fputs("    union {\n", out);
            for (Node *v = t->decl->a; v; v = v->next) {
                if (!v->a) continue;
                fputs("        struct {", out);
                int k = 0;
                for (Node *f = v->a; f; f = f->next, k++) fprintf(out, " %s f%d;", f->type->cname, k);
                fprintf(out, " } v_%.*s;\n", SF(v->name));
            }
            fputs("    } u;\n", out);
        }
        fputs("};\n\n", out);
        return;
    }
    if (t->kind == TY_ARRAY) {
        emit_type_def(t->elem);
        fprintf(out, "struct %s {\n    %s v[%lluULL];\n};\n\n", t->cname, t->elem->cname, (unsigned long long)t->len);
        return;
    }
    for (Node *f = t->decl->a; f; f = f->next) emit_type_def(f->type);
    fprintf(out, "struct %s {\n", t->cname);
    for (Node *f = t->decl->a; f; f = f->next) fprintf(out, "    %s m_%.*s;\n", f->type->cname, SF(f->name));
    fputs("};\n\n", out);
}

/* the symbol an exported or extern function has in C, as an assembler label */
static void put_label(Node *fn) { fprintf(out, " __asm__(\"%.*s\")", SF(fn->name)); }

/* how a parameter looks to C: arrays are passed as a pointer to their first element */
static void put_c_param_type(Node *p) {
    Type *t = p->type;
    bool by_ptr = p->op == M_INOUT || p->op == M_SET || (p->op == M_LET && is_aggregate(t));
    if (!by_ptr) {
        fputs(t->cname, out);
        return;
    }
    fprintf(out, "%s%s *", p->op == M_LET ? "const " : "", t->kind == TY_ARRAY ? t->elem->cname : t->cname);
}

static void emit_extern_proto(Node *fn) {
    fprintf(out, "%s x_%.*s(", fn->type->cname, SF(fn->name));
    if (!fn->a) fputs("void", out);
    int k = 0;
    for (Node *p = fn->a; p; p = p->next, k++) {
        if (k) fputs(", ", out);
        put_c_param_type(p);
        fprintf(out, " p%d", k);
    }
    fputc(')', out);
    put_label(fn);
    fputs(";\n", out);
}

static void emit_sig(IrFunc *f) {
    fprintf(out, "%s ", f->ret->cname);
    put_fn_ident(f->decl);
    fputc('(', out);
    if (!f->nparams) fputs("void", out);
    for (int k = 0; k < f->nparams; k++) {
        IrVar *p = f->params[k];
        if (k) fputs(", ", out);
        if (p->by_ptr) fprintf(out, "%s%s *", p->is_const ? "const " : "", p->type->cname);
        else fprintf(out, "%s ", p->type->cname);
        put_var(p);
    }
    fputc(')', out);
}

static void emit_prelude_freestanding(void) {
    fputs("/* generated by nogec --freestanding: no C library is used */\n"
          "#include <stdbool.h>\n#include <stdint.h>\n\n"
          "enum { NOGE_TRAP_OVERFLOW = 1, NOGE_TRAP_DIV_ZERO, NOGE_TRAP_BOUNDS, NOGE_TRAP_SHIFT };\n"
          "\n"
          "/* A failed run-time check calls NOGE_TRAP(kind, line): 1 overflow, 2 division by zero, 3 bounds,\n"
          "   4 shift. By default that calls noge_trap_hook, which your board code may define (reset, log,\n"
          "   halt...) and which must not return. This weak default just hangs. */\n"
          "#ifndef NOGE_TRAP\n"
          "#if defined(__GNUC__)\n"
          "void noge_trap_hook(int kind, int line) __attribute__((noreturn));\n"
          "__attribute__((weak)) void noge_trap_hook(int kind, int line) {\n"
          "    (void)kind;\n"
          "    (void)line;\n"
          "    for (;;) {\n"
          "    }\n"
          "}\n"
          "#else\n"
          "void noge_trap_hook(int kind, int line); /* you must define this */\n"
          "#endif\n"
          "#define NOGE_TRAP(kind, line) noge_trap_hook((kind), (line))\n"
          "#endif\n\n",
          out);
}

static void emit_prelude(const char *file) {
    if (opts->freestanding) {
        emit_prelude_freestanding();
        return;
    }
    fputs("/* generated by nogec */\n"
          "#include <stdbool.h>\n#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n\n"
          "#define NOGE_FILE \"",
          out);
    for (const char *p = file; *p; p++) {
        if (*p == '"' || *p == '\\') fputc('\\', out);
        fputc(*p, out);
    }
    fputs("\"\n"
          "enum { NOGE_TRAP_OVERFLOW = 1, NOGE_TRAP_DIV_ZERO, NOGE_TRAP_BOUNDS, NOGE_TRAP_SHIFT };\n"
          "\n"
          "/* A failed runtime check calls NOGE_TRAP(kind, line). Define your own to reset or log\n"
          "   instead (it must not return); the default reports and exits with status 101. */\n"
          "#ifndef NOGE_TRAP\n"
          "#if defined(__GNUC__)\n"
          "#define NOGE_NORETURN __attribute__((noreturn))\n"
          "#define NOGE_UNUSED __attribute__((unused)) /* clang warns about an unused static inline */\n"
          "#else\n"
          "#define NOGE_NORETURN\n"
          "#define NOGE_UNUSED\n"
          "#endif\n"
          "static inline NOGE_NORETURN NOGE_UNUSED void noge_trap_default(int kind, int line, const char *file) {\n"
          "    static const char *const what[] = {\"\", \"integer overflow\", \"division by zero\",\n"
          "                                       \"array index out of bounds\", \"shift amount out of range\"};\n"
          "    fflush(stdout);\n"
          "    fprintf(stderr, \"%s:%d: runtime error: %s\\n\", file, line, what[kind]);\n"
          "    exit(101);\n"
          "}\n"
          "#define NOGE_TRAP(kind, line) noge_trap_default((kind), (line), NOGE_FILE)\n"
          "#endif\n\n",
          out);
}

/* what every file of a program in several C files includes: like emit_prelude, but the file name is each file's own */
static void emit_prelude_header(void) {
    fputs("#ifndef NOGE_TYPES_H\n#define NOGE_TYPES_H\n", out);
    if (opts->freestanding) {
        fputs("/* generated by nogec --freestanding: no C library is used */\n"
              "#include <stdbool.h>\n#include <stdint.h>\n\n"
              "enum { NOGE_TRAP_OVERFLOW = 1, NOGE_TRAP_DIV_ZERO, NOGE_TRAP_BOUNDS, NOGE_TRAP_SHIFT };\n\n"
              "/* A failed run-time check calls NOGE_TRAP(kind, line): 1 overflow, 2 division by zero, 3 bounds,\n"
              "   4 shift. By default that calls noge_trap_hook, which your board code may define and which must\n"
              "   not return; the program's own file has a weak default that hangs. */\n"
              "#ifndef NOGE_TRAP\n"
              "#if defined(__GNUC__)\n"
              "void noge_trap_hook(int kind, int line) __attribute__((noreturn));\n"
              "#else\n"
              "void noge_trap_hook(int kind, int line); /* you must define this */\n"
              "#endif\n"
              "#define NOGE_TRAP(kind, line) noge_trap_hook((kind), (line))\n"
              "#endif\n\n",
              out);
        return;
    }
    fputs("/* generated by nogec */\n"
          "#include <stdbool.h>\n#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n\n"
          "#ifndef NOGE_FILE\n#define NOGE_FILE \"?\" /* each .c file says which file it comes from */\n#endif\n"
          "enum { NOGE_TRAP_OVERFLOW = 1, NOGE_TRAP_DIV_ZERO, NOGE_TRAP_BOUNDS, NOGE_TRAP_SHIFT };\n"
          "\n"
          "/* A failed runtime check calls NOGE_TRAP(kind, line). Define your own to reset or log\n"
          "   instead (it must not return); the default reports and exits with status 101. */\n"
          "#ifndef NOGE_TRAP\n"
          "#if defined(__GNUC__)\n"
          "#define NOGE_NORETURN __attribute__((noreturn))\n"
          "#define NOGE_UNUSED __attribute__((unused)) /* clang warns about an unused static inline */\n"
          "#else\n"
          "#define NOGE_NORETURN\n"
          "#define NOGE_UNUSED\n"
          "#endif\n"
          "static inline NOGE_NORETURN NOGE_UNUSED void noge_trap_default(int kind, int line, const char *file) {\n"
          "    static const char *const what[] = {\"\", \"integer overflow\", \"division by zero\",\n"
          "                                       \"array index out of bounds\", \"shift amount out of range\"};\n"
          "    fflush(stdout);\n"
          "    fprintf(stderr, \"%s:%d: runtime error: %s\\n\", file, line, what[kind]);\n"
          "    exit(101);\n"
          "}\n"
          "#define NOGE_TRAP(kind, line) noge_trap_default((kind), (line), NOGE_FILE)\n"
          "#endif\n\n",
          out);
}

/* the default for a freestanding program's noge_trap_hook, in the program's own file */
static void emit_trap_hook_definition(void) {
    fputs("#ifndef NOGE_TRAP\n"
          "#if defined(__GNUC__)\n"
          "__attribute__((weak)) void noge_trap_hook(int kind, int line) {\n"
          "    (void)kind;\n"
          "    (void)line;\n"
          "    for (;;) {\n"
          "    }\n"
          "}\n"
          "#endif\n"
          "#endif\n\n",
          out);
}

/* ---- statics ---- */
static void put_static_ident(Node *d) {
    if (d->mod == 0) fprintf(out, "g_%.*s", SF(d->name));
    else fprintf(out, "gm%s_%.*s", g_modules[d->mod]->tag, SF(d->name));
}

/* is this initial value all zero bits (then `{0}` does) */
static bool init_is_zero(Node *e) {
    switch (e->kind) {
    case N_INT: return e->ival == 0;
    case N_BOOL: return e->ival == 0;
    case N_LIT:
        for (Node *i = e->a; i; i = i->next)
            if (!init_is_zero(i->a)) return false;
        return true;
    case N_ARRAY:
        for (Node *x = e->a; x; x = x->next)
            if (!init_is_zero(x)) return false;
        return true;
    case N_REPEAT: return init_is_zero(e->a);
    case N_VARIANT:
        if (e->decl->ival != 0) return false;
        for (Node *x = e->a; x; x = x->next)
            if (!init_is_zero(x)) return false;
        return true;
    default: return false;
    }
}

static void emit_static_init(Node *e, Type *t) {
    if (init_is_zero(e)) {
        fputs(is_aggregate(t) ? "{0}" : "0", out);
        return;
    }
    switch (e->kind) {
    case N_INT: {
        Operand o = {0};
        o.type = t;
        o.c = e->op ? (uint64_t)0 - e->ival : e->ival;
        put_operand(o);
        break;
    }
    case N_BOOL:
        fputs(e->ival ? "true" : "false", out);
        break;
    case N_LIT:
        fputs("{ ", out);
        for (Node *i = e->a; i; i = i->next) {
            fprintf(out, ".m_%.*s = ", SF(i->name));
            emit_static_init(i->a, i->decl->type);
            fputs(i->next ? ", " : " ", out);
        }
        fputc('}', out);
        break;
    case N_ARRAY: /* an array is a struct around the C array */
        fputs("{ { ", out);
        for (Node *x = e->a; x; x = x->next) {
            emit_static_init(x, t->elem);
            fputs(x->next ? ", " : " ", out);
        }
        fputs("} }", out);
        break;
    case N_REPEAT:
        fputs("{ { ", out);
        for (uint64_t k = 0; k < e->ival; k++) {
            emit_static_init(e->a, t->elem);
            fputs(k + 1 < e->ival ? ", " : " ", out);
        }
        fputs("} }", out);
        break;
    case N_VARIANT: {
        Operand tag = {0};
        tag.c = e->decl->ival;
        tag.type = t->tag_type;
        fputs("{ .tag = ", out);
        put_operand(tag);
        if (e->a) {
            fprintf(out, ", .u.v_%.*s = { ", SF(e->decl->name));
            int k = 0;
            Node *f = e->decl->a;
            for (Node *x = e->a; x; x = x->next, f = f->next, k++) {
                fprintf(out, "%s.f%d = ", k ? ", " : "", k);
                emit_static_init(x, f->type);
            }
            fputs(" }", out);
        }
        fputs(" }", out);
        break;
    }
    default: break;
    }
}

/* the C variable of a static, as a declaration (extern, for other files) or with its initial value */
static void emit_static(Node *d, bool define) {
    if (!define) fputs("extern ", out);
    if (!d->op) fputs("const ", out);
    else if (d->flag) fputs("volatile ", out);
    fprintf(out, "%s ", d->type->cname);
    put_static_ident(d);
    if (define) {
        fputs(" = ", out);
        emit_static_init(d->a, d->type);
    }
    fputs(";\n", out);
}

/* every struct, enum, array and pointer type, components first */
static void emit_types(Node *prog) {
    for (Node *d = prog; d; d = d->next)
        if (d->kind == N_STRUCT || d->kind == N_ENUM) fprintf(out, "typedef struct %s %s;\n", d->type->cname, d->type->cname);
    for (Type *t = g_array_types; t; t = t->next_array) fprintf(out, "typedef struct %s %s;\n", t->cname, t->cname);
    for (Type *t = g_ptr_types; t; t = t->next_ptr) /* inner pointer types come first */
        fprintf(out, "typedef %s%s *%s;\n", t->is_mut ? "" : "const ", t->elem->cname, t->cname);
    fputc('\n', out);
    for (Node *d = prog; d; d = d->next)
        if (d->kind == N_STRUCT || d->kind == N_ENUM) emit_type_def(d->type);
    for (Type *t = g_array_types; t; t = t->next_array) emit_type_def(t);
}

void emit_c(FILE *f, Node *prog, IrFunc *funcs, const EmitOpts *o) {
    out = f;
    opts = o;
    emit_prelude(g_modules[0]->path);
    emit_types(prog);

    if (!opts->freestanding) fputs("void f_print(int32_t x);\n", out);
    for (Node *d = prog; d; d = d->next)
        if (d->kind == N_FN && d->ext == EXT_EXTERN) emit_extern_proto(d);
    for (IrFunc *fn = funcs; fn; fn = fn->next) {
        emit_sig(fn);
        if (fn->decl->ext == EXT_EXPORT) put_label(fn->decl);
        fputs(";\n", out);
    }
    if (!opts->freestanding) fputs("\nvoid f_print(int32_t x) { printf(\"%d\\n\", (int)x); }\n", out);
    fputc('\n', out);
    for (Node *d = prog; d; d = d->next)
        if (d->kind == N_STATIC) emit_static(d, true);
    fputc('\n', out);

    int file_mod = 0; /* the module NOGE_FILE names: a failed check says which file the line is in */
    for (IrFunc *fn = funcs; fn; fn = fn->next) {
        if (!opts->freestanding && fn->decl->mod != file_mod) {
            file_mod = fn->decl->mod;
            fputs("#undef NOGE_FILE\n#define NOGE_FILE \"", out);
            for (const char *p = g_modules[file_mod]->path; *p; p++) {
                if (*p == '"' || *p == '\\') fputc('\\', out);
                fputc(*p, out);
            }
            fputs("\"\n", out);
        }
        emit_sig(fn);
        fputs(" {\n", out);
        for (int k = 0; k < fn->nparams; k++) {
            fputs("    (void)", out);
            put_var(fn->params[k]);
            fputs(";\n", out);
        }
        emit_block(&fn->body, 1);
        fputs("}\n\n", out);
    }
    fputs("int main(void) { return (int)f_main(); }\n", out);
}

/* ======================================================= one C file per module ==
 * nogec --emit-dir=DIR writes DIR/noge_types.h (the types, the run-time check machinery, the declarations of
 * extern functions), DIR/<module>.c for every module that has functions, and DIR/Makefile. The whole program has
 * been checked together, so each .c file holds exactly the functions of its module (instances of generic code in the
 * module of the template) and the prototypes of what it defines and calls. A file is only rewritten when its text
 * changes, so make, or a compiler cache, rebuilds only the modules a change really touches. */

static void write_if_changed(const char *path, FILE *tmp) {
    long n = ftell(tmp);
    rewind(tmp);
    char *buf = malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, tmp) != (size_t)n) {
        fprintf(stderr, "noge: out of memory\n");
        exit(1);
    }
    FILE *old = fopen(path, "rb");
    if (old) {
        bool same = true;
        for (long i = 0; i < n && same; i++) same = fgetc(old) == (unsigned char)buf[i];
        same = same && fgetc(old) == EOF;
        fclose(old);
        if (same) {
            free(buf);
            return;
        }
    }
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "noge: cannot write '%s' (does the folder exist?)\n", path);
        exit(1);
    }
    fclose(f);
    free(buf);
}

/* the file name of a module, without .c: its own name, and for the others a few hex digits from its path */
static void module_stem(char *dst, size_t cap, int m) {
    const char *p = g_modules[m]->path;
    const char *slash = strrchr(p, '/');
    if (slash) p = slash + 1;
    size_t n = strlen(p);
    if (n > 4 && strcmp(p + n - 4, ".nge") == 0) n -= 4;
    size_t w = 0;
    for (size_t i = 0; i < n && w + 12 < cap; i++) {
        char c = p[i];
        dst[w++] = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ? c : '_';
    }
    dst[w] = 0;
    if (m != 0) snprintf(dst + w, cap - w, "_%s", g_modules[m]->tag);
}

static void emit_string_literal_path(const char *path) {
    for (const char *p = path; *p; p++) {
        if (*p == '"' || *p == '\\') fputc('\\', out);
        fputc(*p, out);
    }
}

/* the prototype of a function from its declaration, the same types and passing as emit_sig */
static void emit_proto(Node *fn) {
    fprintf(out, "%s ", fn->type->cname);
    put_fn_ident(fn);
    fputc('(', out);
    int k = 0;
    for (Node *p = fn->a; p; p = p->next, k++) {
        if (k) fputs(", ", out);
        bool agg_let = p->op == M_LET && is_aggregate(p->type);
        if (p->op == M_INOUT || p->op == M_SET || agg_let) fprintf(out, "%s%s *", agg_let ? "const " : "", p->type->cname);
        else fprintf(out, "%s", p->type->cname);
    }
    if (!k) fputs("void", out);
    fputc(')', out);
    if (fn->ext == EXT_EXPORT) put_label(fn);
    fputs(";\n", out);
}

static int proto_file; /* which file the prototypes being collected are for: a function marked with it has one */

static void protos_in(Block *b) {
    for (Instr *i = b->first; i; i = i->next) {
        if (i->op == I_CALL && i->callee->ext != EXT_EXTERN && i->callee->ival != 1 && i->callee->index != proto_file) {
            i->callee->index = proto_file;
            emit_proto(i->callee);
        }
        if (i->op == I_IF) {
            protos_in(&i->then);
            protos_in(&i->els);
        } else if (i->op == I_WHILE) {
            protos_in(&i->cond);
            protos_in(&i->body);
        } else if (i->op == I_BLOCK) {
            protos_in(&i->then);
        }
    }
}

void emit_c_dir(const char *dir, Node *prog, IrFunc *funcs, const EmitOpts *o) {
    opts = o;
    char path[4096], stem[128];
    FILE *saved = out;

    /* the header */
    out = tmpfile();
    emit_prelude_header();
    emit_types(prog);
    if (!opts->freestanding) fputs("void f_print(int32_t x);\n", out);
    for (Node *d = prog; d; d = d->next)
        if (d->kind == N_FN && d->ext == EXT_EXTERN) emit_extern_proto(d);
    for (Node *d = prog; d; d = d->next)
        if (d->kind == N_STATIC) emit_static(d, false);
    fputs("\n#endif\n", out);
    snprintf(path, sizeof path, "%s/noge_types.h", dir);
    write_if_changed(path, out);
    fclose(out);

    /* one file per module that has functions (the program's own always, for main) */
    char *objs = malloc(64 + (size_t)g_nmodules * 140);
    objs[0] = 0;
    for (int m = 0; m < g_nmodules; m++) {
        bool any = m == 0;
        for (IrFunc *fn = funcs; fn && !any; fn = fn->next) any = fn->decl->mod == m;
        for (Node *d = prog; d && !any; d = d->next) any = d->kind == N_STATIC && d->mod == m;
        if (!any) continue;
        module_stem(stem, sizeof stem, m);
        out = tmpfile();
        fprintf(out, "/* generated by nogec from %s */\n", g_modules[m]->path);
        if (!opts->freestanding) {
            fputs("#define NOGE_FILE \"", out);
            emit_string_literal_path(g_modules[m]->path);
            fputs("\"\n", out);
        }
        fputs("#include \"noge_types.h\"\n\n", out);
        proto_file++;
        for (IrFunc *fn = funcs; fn; fn = fn->next) {
            if (fn->decl->mod != m) continue;
            fn->decl->index = proto_file;
            emit_proto(fn->decl);
        }
        for (IrFunc *fn = funcs; fn; fn = fn->next)
            if (fn->decl->mod == m) protos_in(&fn->body);
        fputc('\n', out);
        for (Node *d = prog; d; d = d->next)
            if (d->kind == N_STATIC && d->mod == m) emit_static(d, true);
        if (m == 0) {
            if (!opts->freestanding) fputs("void f_print(int32_t x) { printf(\"%d\\n\", (int)x); }\n\n", out);
            else emit_trap_hook_definition();
        }
        for (IrFunc *fn = funcs; fn; fn = fn->next) {
            if (fn->decl->mod != m) continue;
            emit_sig(fn);
            fputs(" {\n", out);
            for (int k = 0; k < fn->nparams; k++) {
                fputs("    (void)", out);
                put_var(fn->params[k]);
                fputs(";\n", out);
            }
            emit_block(&fn->body, 1);
            fputs("}\n\n", out);
        }
        if (m == 0) fputs("int main(void) { return (int)f_main(); }\n", out);
        snprintf(path, sizeof path, "%s/%s.c", dir, stem);
        write_if_changed(path, out);
        fclose(out);
        size_t len = strlen(objs);
        snprintf(objs + len, 140, "%s%s.o", len ? " " : "", stem);
    }

    /* the Makefile: an object per file, all of them depending on the header and on the flags in use */
    module_stem(stem, sizeof stem, 0);
    out = tmpfile();
    fprintf(out,
            "# generated by nogec: build with   make   (CC, CFLAGS, LDFLAGS and EXTRA = more C files to link can be set)\n"
            "CC ?= cc\nCFLAGS ?= -O2\nPROG = %s\nOBJS = %s\n\n"
            "$(PROG): $(OBJS) $(EXTRA)\n\t$(CC) $(CFLAGS) -o $@ $(OBJS) $(EXTRA) $(LDFLAGS)\n\n"
            "%%.o: %%.c noge_types.h .flags\n\t$(CC) $(CFLAGS) -c -o $@ $<\n\n"
            ".flags: FORCE\n\t@echo '$(CC) $(CFLAGS)' | cmp -s - $@ || echo '$(CC) $(CFLAGS)' > $@\n"
            "FORCE:\n.PHONY: FORCE\n",
            stem, objs);
    snprintf(path, sizeof path, "%s/Makefile", dir);
    write_if_changed(path, out);
    fclose(out);
    free(objs);
    out = saved;
}
