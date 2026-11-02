/* C99 backend: walks the checked AST and prints plain C. */
#include <limits.h>

#include "noge.h"

static FILE *out;

static void indent(int d) {
    for (int i = 0; i < d; i++) fputs("    ", out);
}

static const char *c_op(int op) {
    switch (op) {
    case T_PLUS: return "+";
    case T_MINUS: return "-";
    case T_STAR: return "*";
    case T_SLASH: return "/";
    case T_PERCENT: return "%";
    case T_EQ: return "==";
    case T_NE: return "!=";
    case T_LT: return "<";
    case T_LE: return "<=";
    case T_GT: return ">";
    case T_GE: return ">=";
    case T_ANDAND: return "&&";
    case T_OROR: return "||";
    default: return "?";
    }
}

static void emit_local(int id, Str name) { fprintf(out, "v%d_%.*s", id, SF(name)); }

static void emit_expr(Node *n);

static void emit_int(Node *n) {
    Type *t = n->type;
    if (!t->is_signed) {
        fprintf(out, "((%s)%lluULL)", t->cname, (unsigned long long)n->ival);
        return;
    }
    uint64_t u = n->op ? (0 - n->ival) : n->ival;
    long long v = (long long)u;
    if (v == LLONG_MIN) fputs("(-9223372036854775807LL - 1)", out);
    else if (v < 0) fprintf(out, "((%s)(%lldLL))", t->cname, v);
    else fprintf(out, "((%s)%lldLL)", t->cname, v);
}

static void emit_addr(Node *place) {
    if (place->kind == N_NAME && place->by_ptr) { /* already a pointer */
        emit_local(place->local, place->name);
    } else {
        fputs("(&", out);
        emit_expr(place);
        fputc(')', out);
    }
}

static void emit_expr(Node *n) {
    switch (n->kind) {
    case N_INT:
        emit_int(n);
        break;
    case N_BOOL:
        fputs(n->ival ? "true" : "false", out);
        break;
    case N_NAME:
        if (n->by_ptr) fputs("(*", out);
        emit_local(n->local, n->name);
        if (n->by_ptr) fputc(')', out);
        break;
    case N_MEMBER:
        fputc('(', out);
        emit_expr(n->a);
        fprintf(out, ".m_%.*s)", SF(n->name));
        break;
    case N_UN:
        fprintf(out, "(%s", n->op == T_BANG ? "!" : "-");
        emit_expr(n->a);
        fputc(')', out);
        break;
    case N_BIN:
        fputc('(', out);
        emit_expr(n->a);
        fprintf(out, " %s ", c_op(n->op));
        emit_expr(n->b);
        fputc(')', out);
        break;
    case N_CAST:
        fprintf(out, "((%s)", n->type->cname);
        emit_expr(n->a);
        fputc(')', out);
        break;
    case N_CALL:
        fprintf(out, "f_%.*s(", SF(n->name));
        for (Node *a = n->a; a; a = a->next) {
            if (a != n->a) fputs(", ", out);
            if (a->kind == N_ADDR) emit_addr(a->a);
            else emit_expr(a);
        }
        fputc(')', out);
        break;
    case N_LIT:
        fprintf(out, "((%s){ ", n->type->cname);
        for (Node *i = n->a; i; i = i->next) {
            if (i != n->a) fputs(", ", out);
            fprintf(out, ".m_%.*s = ", SF(i->name));
            emit_expr(i->a);
        }
        fputs(" })", out);
        break;
    default:
        fprintf(stderr, "noge: internal error: cannot emit expression kind %d\n", n->kind);
        exit(1);
    }
}

static void emit_stmt(Node *s, int d);

static void emit_stmts(Node *list, int d) {
    for (Node *s = list; s; s = s->next) emit_stmt(s, d);
}

static void emit_if(Node *s, int d) {
    fputs("if (", out);
    emit_expr(s->a);
    fputs(") {\n", out);
    emit_stmts(s->b->a, d + 1);
    indent(d);
    fputc('}', out);
    if (s->c) {
        fputs(" else ", out);
        if (s->c->kind == N_IF) {
            emit_if(s->c, d);
            return;
        }
        fputs("{\n", out);
        emit_stmts(s->c->a, d + 1);
        indent(d);
        fputc('}', out);
    }
    fputc('\n', out);
}

static void emit_stmt(Node *s, int d) {
    indent(d);
    switch (s->kind) {
    case N_LET:
        fprintf(out, "%s ", s->type->cname);
        emit_local(s->local, s->name);
        fputs(" = ", out);
        emit_expr(s->a);
        fputs("; (void)", out);
        emit_local(s->local, s->name);
        fputs(";\n", out);
        break;
    case N_ASSIGN:
        emit_expr(s->a);
        fputs(" = ", out);
        emit_expr(s->b);
        fputs(";\n", out);
        break;
    case N_EXPR:
        emit_expr(s->a);
        fputs(";\n", out);
        break;
    case N_IF:
        emit_if(s, d);
        break;
    case N_WHILE:
        fputs("while (", out);
        emit_expr(s->a);
        fputs(") {\n", out);
        emit_stmts(s->b->a, d + 1);
        indent(d);
        fputs("}\n", out);
        break;
    case N_RETURN:
        fputs("return", out);
        if (s->a) {
            fputc(' ', out);
            emit_expr(s->a);
        }
        fputs(";\n", out);
        break;
    case N_BLOCK:
        fputs("{\n", out);
        emit_stmts(s->a, d + 1);
        indent(d);
        fputs("}\n", out);
        break;
    default:
        fprintf(stderr, "noge: internal error: cannot emit statement kind %d\n", s->kind);
        exit(1);
    }
}

static void emit_struct(Node *s) { /* dependencies first, so by-value fields are complete */
    if (s->mut) return;
    s->mut = 1;
    for (Node *f = s->a; f; f = f->next)
        if (f->type->kind == TY_STRUCT) emit_struct(f->type->decl);
    fprintf(out, "struct %s {\n", s->type->cname);
    for (Node *f = s->a; f; f = f->next) fprintf(out, "    %s m_%.*s;\n", f->type->cname, SF(f->name));
    fputs("};\n\n", out);
}

static void emit_sig(Node *fn) {
    fprintf(out, "%s f_%.*s(", fn->type->cname, SF(fn->name));
    if (!fn->a) fputs("void", out);
    for (Node *p = fn->a; p; p = p->next) {
        if (p != fn->a) fputs(", ", out);
        fprintf(out, "%s%s", p->type->cname, p->op == M_INOUT ? " *" : " ");
        emit_local(p->local, p->name);
    }
    fputc(')', out);
}

void emit_c(FILE *f, Node *prog) {
    out = f;
    fputs("/* generated by nogec */\n"
          "#include <stdbool.h>\n#include <stdint.h>\n#include <stdio.h>\n\n",
          out);

    for (Node *d = prog; d; d = d->next)
        if (d->kind == N_STRUCT) fprintf(out, "typedef struct %s %s;\n", d->type->cname, d->type->cname);
    fputc('\n', out);
    for (Node *d = prog; d; d = d->next)
        if (d->kind == N_STRUCT) emit_struct(d);

    fputs("void f_print(int32_t x);\n", out);
    for (Node *d = prog; d; d = d->next)
        if (d->kind == N_FN) {
            emit_sig(d);
            fputs(";\n", out);
        }
    fputs("\nvoid f_print(int32_t x) { printf(\"%d\\n\", (int)x); }\n\n", out);

    for (Node *d = prog; d; d = d->next) {
        if (d->kind != N_FN) continue;
        emit_sig(d);
        fputs(" {\n", out);
        emit_stmts(d->b->a, 1);
        fputs("}\n\n", out);
    }
    fputs("int main(void) { return (int)f_main(); }\n", out);
}
