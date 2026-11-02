#include "noge.h"

static Token *toks;
static int pos;
static int nolit; /* struct literals are not allowed in if/while conditions */

static Token *cur(void) { return &toks[pos]; }
static Token *peek(int k) { return &toks[pos + k]; }
static Token *advance(void) {
    Token *t = &toks[pos];
    if (t->kind != T_EOF) pos++;
    return t;
}
static bool accept(TokKind k) {
    if (cur()->kind == k) { pos++; return true; }
    return false;
}
static const char *describe(Token *t) {
    static char buf[128];
    if (t->kind == T_EOF) return "end of file";
    snprintf(buf, sizeof buf, "'%.*s'", t->text.n > 100 ? 100 : t->text.n, t->text.p);
    return buf;
}
static Token *expect(TokKind k) {
    if (cur()->kind != k)
        die(cur()->line, "expected %s, found %s", tok_name(k), describe(cur()));
    return advance();
}

static Node *mk(NodeKind k, int line) {
    Node *n = ARENA_NEW(&g_arena, Node);
    n->kind = k;
    n->line = line;
    n->local = -1;
    return n;
}

static Str parse_type_name(void) { return expect(T_IDENT)->text; }

static Node *parse_expr(void);
static Node *parse_block(void);

static Node *parse_primary(void) {
    Token *t = cur();
    switch (t->kind) {
    case T_INT: {
        advance();
        Node *n = mk(N_INT, t->line);
        n->ival = t->ival;
        return n;
    }
    case T_TRUE:
    case T_FALSE: {
        advance();
        Node *n = mk(N_BOOL, t->line);
        n->ival = t->kind == T_TRUE;
        return n;
    }
    case T_LPAREN: {
        advance();
        int save = nolit;
        nolit = 0;
        Node *e = parse_expr();
        nolit = save;
        expect(T_RPAREN);
        return e;
    }
    case T_IDENT: {
        advance();
        if (cur()->kind == T_LPAREN) { /* call */
            advance();
            Node *n = mk(N_CALL, t->line);
            n->name = t->text;
            int save = nolit;
            nolit = 0;
            Node **tail = &n->a;
            while (cur()->kind != T_RPAREN) {
                *tail = parse_expr();
                tail = &(*tail)->next;
                if (!accept(T_COMMA)) break;
            }
            nolit = save;
            expect(T_RPAREN);
            return n;
        }
        if (!nolit && cur()->kind == T_LBRACE &&
            (peek(1)->kind == T_RBRACE ||
             (peek(1)->kind == T_IDENT && peek(2)->kind == T_COLON))) { /* struct literal */
            advance();
            Node *n = mk(N_LIT, t->line);
            n->name = t->text;
            int save = nolit;
            nolit = 0;
            Node **tail = &n->a;
            while (cur()->kind != T_RBRACE) {
                Token *f = expect(T_IDENT);
                expect(T_COLON);
                Node *i = mk(N_INITF, f->line);
                i->name = f->text;
                i->a = parse_expr();
                *tail = i;
                tail = &i->next;
                if (!accept(T_COMMA)) break;
            }
            nolit = save;
            expect(T_RBRACE);
            return n;
        }
        Node *n = mk(N_NAME, t->line);
        n->name = t->text;
        return n;
    }
    default:
        die(t->line, "expected an expression, found %s", describe(t));
    }
}

static Node *parse_postfix(void) {
    Node *e = parse_primary();
    while (cur()->kind == T_DOT) {
        int line = advance()->line;
        Token *f = expect(T_IDENT);
        Node *n = mk(N_MEMBER, line);
        n->a = e;
        n->name = f->text;
        e = n;
    }
    return e;
}

static Node *parse_unary(void) {
    Token *t = cur();
    if (t->kind == T_MINUS || t->kind == T_BANG) {
        advance();
        Node *n = mk(N_UN, t->line);
        n->op = t->kind;
        n->a = parse_unary();
        return n;
    }
    if (t->kind == T_AMP) {
        advance();
        Node *n = mk(N_ADDR, t->line);
        n->a = parse_unary();
        return n;
    }
    return parse_postfix();
}

static Node *parse_cast(void) {
    Node *e = parse_unary();
    while (cur()->kind == T_AS) {
        int line = advance()->line;
        Node *n = mk(N_CAST, line);
        n->a = e;
        n->tyname = parse_type_name();
        e = n;
    }
    return e;
}

static int prec(TokKind k) {
    switch (k) {
    case T_OROR: return 1;
    case T_ANDAND: return 2;
    case T_EQ: case T_NE: return 3;
    case T_LT: case T_LE: case T_GT: case T_GE: return 4;
    case T_PLUS: case T_MINUS: return 5;
    case T_STAR: case T_SLASH: case T_PERCENT: return 6;
    default: return 0;
    }
}

static Node *parse_binary(int minp) {
    Node *l = parse_cast();
    for (;;) {
        TokKind k = cur()->kind;
        int p = prec(k);
        if (p == 0 || p < minp) break;
        int line = advance()->line;
        Node *r = parse_binary(p + 1);
        Node *n = mk(N_BIN, line);
        n->op = k;
        n->a = l;
        n->b = r;
        l = n;
    }
    return l;
}

static Node *parse_expr(void) { return parse_binary(1); }

static Node *parse_cond(void) {
    int save = nolit;
    nolit = 1;
    Node *c = parse_expr();
    nolit = save;
    return c;
}

static Node *parse_if(void) {
    int line = advance()->line; /* 'if' */
    Node *n = mk(N_IF, line);
    n->a = parse_cond();
    n->b = parse_block();
    if (accept(T_ELSE)) n->c = cur()->kind == T_IF ? parse_if() : parse_block();
    return n;
}

static Node *parse_stmt(void) {
    Token *t = cur();
    switch (t->kind) {
    case T_LET:
    case T_VAR: {
        advance();
        Node *n = mk(N_LET, t->line);
        n->op = t->kind == T_VAR;
        n->name = expect(T_IDENT)->text;
        if (accept(T_COLON)) n->tyname = parse_type_name();
        expect(T_ASSIGN);
        n->a = parse_expr();
        expect(T_SEMI);
        return n;
    }
    case T_IF:
        return parse_if();
    case T_WHILE: {
        advance();
        Node *n = mk(N_WHILE, t->line);
        n->a = parse_cond();
        n->b = parse_block();
        return n;
    }
    case T_RETURN: {
        advance();
        Node *n = mk(N_RETURN, t->line);
        if (cur()->kind != T_SEMI) n->a = parse_expr();
        expect(T_SEMI);
        return n;
    }
    case T_LBRACE:
        return parse_block();
    default: {
        Node *e = parse_expr();
        if (accept(T_ASSIGN)) {
            Node *n = mk(N_ASSIGN, t->line);
            n->a = e;
            n->b = parse_expr();
            expect(T_SEMI);
            return n;
        }
        Node *n = mk(N_EXPR, t->line);
        n->a = e;
        expect(T_SEMI);
        return n;
    }
    }
}

static Node *parse_block(void) {
    Node *b = mk(N_BLOCK, expect(T_LBRACE)->line);
    Node **tail = &b->a;
    while (cur()->kind != T_RBRACE) {
        if (cur()->kind == T_EOF) die(cur()->line, "unterminated block, expected '}'");
        *tail = parse_stmt();
        tail = &(*tail)->next;
    }
    advance();
    return b;
}

static Node *parse_struct(void) {
    int line = advance()->line;
    Node *s = mk(N_STRUCT, line);
    s->name = expect(T_IDENT)->text;
    expect(T_LBRACE);
    Node **tail = &s->a;
    while (cur()->kind != T_RBRACE) {
        Token *f = expect(T_IDENT);
        expect(T_COLON);
        Node *fd = mk(N_FDECL, f->line);
        fd->name = f->text;
        fd->tyname = parse_type_name();
        *tail = fd;
        tail = &fd->next;
        if (!accept(T_COMMA)) break;
    }
    expect(T_RBRACE);
    if (!s->a) die(line, "struct '%.*s' must have at least one field", SF(s->name));
    return s;
}

static Node *parse_fn(void) {
    int line = advance()->line;
    Node *f = mk(N_FN, line);
    f->name = expect(T_IDENT)->text;
    expect(T_LPAREN);
    Node **tail = &f->a;
    while (cur()->kind != T_RPAREN) {
        Token *pn = expect(T_IDENT);
        expect(T_COLON);
        Node *p = mk(N_PARAM, pn->line);
        p->name = pn->text;
        p->op = M_LET;
        if (accept(T_INOUT)) p->op = M_INOUT;
        else if (accept(T_SINK)) p->op = M_SINK;
        else accept(T_LET);
        p->tyname = parse_type_name();
        *tail = p;
        tail = &p->next;
        if (!accept(T_COMMA)) break;
    }
    expect(T_RPAREN);
    if (accept(T_ARROW)) f->tyname = parse_type_name();
    f->b = parse_block();
    return f;
}

Node *parse_program(Token *tokens) {
    toks = tokens;
    pos = 0;
    nolit = 0;
    Node *head = NULL, **tail = &head;
    while (cur()->kind != T_EOF) {
        Node *d;
        if (cur()->kind == T_FN) d = parse_fn();
        else if (cur()->kind == T_STRUCT) d = parse_struct();
        else die(cur()->line, "expected 'fn' or 'struct', found %s", describe(cur()));
        *tail = d;
        tail = &d->next;
    }
    return head;
}
