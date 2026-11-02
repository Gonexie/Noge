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
    if (v->is_temp) fprintf(out, "t%d", v->id);
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
        if (pr->field) {
            fprintf(out, ".m_%.*s", SF(pr->field->name));
        } else {
            fputs(".v[", out);
            put_operand(pr->idx);
            fputc(']', out);
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
    case I_CAST:
        put_def(i, d);
        fprintf(out, "(%s)", i->dst->type->cname);
        put_operand(i->a);
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
        fprintf(out, "f_%.*s(", SF(i->callee->name));
        for (int k = 0; k < i->nargs; k++) {
            if (k) fputs(", ", out);
            if (i->args[k].addr) put_addr(&i->args[k].place);
            else put_operand(i->args[k].val);
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
    if (t->kind != TY_STRUCT && t->kind != TY_ARRAY) return;
    t->mark = 1;
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

static void emit_sig(IrFunc *f) {
    fprintf(out, "%s f_%.*s(", f->ret->cname, SF(f->decl->name));
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

static void emit_prelude(const char *file) {
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
          "#else\n"
          "#define NOGE_NORETURN\n"
          "#endif\n"
          "static inline NOGE_NORETURN void noge_trap_default(int kind, int line) {\n"
          "    static const char *const what[] = {\"\", \"integer overflow\", \"division by zero\",\n"
          "                                       \"array index out of bounds\", \"shift amount out of range\"};\n"
          "    fflush(stdout);\n"
          "    fprintf(stderr, \"%s:%d: runtime error: %s\\n\", NOGE_FILE, line, what[kind]);\n"
          "    exit(101);\n"
          "}\n"
          "#define NOGE_TRAP(kind, line) noge_trap_default((kind), (line))\n"
          "#endif\n\n",
          out);
}

void emit_c(FILE *f, Node *prog, IrFunc *funcs, const EmitOpts *o) {
    out = f;
    opts = o;
    emit_prelude(g_file);

    for (Node *d = prog; d; d = d->next)
        if (d->kind == N_STRUCT) fprintf(out, "typedef struct %s %s;\n", d->type->cname, d->type->cname);
    for (Type *t = g_array_types; t; t = t->next_array) fprintf(out, "typedef struct %s %s;\n", t->cname, t->cname);
    fputc('\n', out);
    for (Node *d = prog; d; d = d->next)
        if (d->kind == N_STRUCT) emit_type_def(d->type);
    for (Type *t = g_array_types; t; t = t->next_array) emit_type_def(t);

    fputs("void f_print(int32_t x);\n", out);
    for (IrFunc *fn = funcs; fn; fn = fn->next) {
        emit_sig(fn);
        fputs(";\n", out);
    }
    fputs("\nvoid f_print(int32_t x) { printf(\"%d\\n\", (int)x); }\n\n", out);

    for (IrFunc *fn = funcs; fn; fn = fn->next) {
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
