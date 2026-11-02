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

static Node *parse_type(void);

/* the '>' that closes a list of generic parameters or arguments; `>>` closes two lists at once */
static void expect_gt(void) {
    Token *t = cur();
    if (t->kind == T_SHR) { /* Option<Option<i32>>: split the token, take the first '>' */
        t->kind = T_GT;
        t->text.p += 1;
        t->text.n = 1;
        return;
    }
    expect(T_GT);
}

static bool is_prim_name(Str s) {
    static const char *const prims[] = {"i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "usize", "isize", "bool"};
    for (size_t i = 0; i < sizeof prims / sizeof *prims; i++)
        if (str_is(s, prims[i])) return true;
    return false;
}

/* Generic parameters of a declaration:  <T, U, const N: usize>   (none: NULL) */
static Node *parse_tparams(void) {
    if (!accept(T_LT)) return NULL;
    Node *head = NULL, **tail = &head;
    while (cur()->kind != T_GT && cur()->kind != T_SHR) {
        Node *p = mk(N_TPARAM, cur()->line);
        if (accept(T_CONST)) {
            p->op = 1;
            p->name = expect(T_IDENT)->text;
            expect(T_COLON);
            p->ty = parse_type();
        } else {
            p->name = expect(T_IDENT)->text;
            if (cur()->kind == T_COLON)
                die(cur()->line, "bounds on type parameters are not supported: generic code is checked for each type it is used with");
        }
        *tail = p;
        tail = &p->next;
        if (!accept(T_COMMA)) break;
    }
    if (!head) die(cur()->line, "a list of generic parameters needs at least one parameter");
    expect_gt();
    return head;
}

/* Generic arguments, after the '<':  i32, [u8; 4], 8, BUF_LEN   (a type, or a constant for a const parameter) */
static Node *parse_targs(void) {
    Node *head = NULL, **tail = &head;
    while (cur()->kind != T_GT && cur()->kind != T_SHR) {
        Node *a;
        if (cur()->kind == T_INT) {
            a = mk(N_INT, cur()->line);
            a->ival = cur()->ival;
            advance();
        } else {
            a = parse_type(); /* a name here may turn out to be a constant: decided when the parameter is known */
        }
        *tail = a;
        tail = &a->next;
        if (!accept(T_COMMA)) break;
    }
    if (!head) die(cur()->line, "a list of generic arguments needs at least one argument");
    expect_gt();
    return head;
}

/* Type  ::=  Name [ "<" Args ">" ]  |  "[" Type ";" ConstExpr "]"  |  "*" ("mut" | "const") Type
 * N_TYPE: op 0 named (gen = the arguments), 1 array (a = element, b = length expression), 2 pointer (a = pointee, ival = is mut) */
static Node *parse_expr(void);
static Node *parse_type(void) {
    Token *t = cur();
    Node *n = mk(N_TYPE, t->line);
    if (t->kind == T_LBRACKET) {
        advance();
        n->op = 1;
        n->a = parse_type();
        expect(T_SEMI);
        n->b = parse_expr(); /* a constant expression: 8, BUF_LEN, 4 * WORDS */
        expect(T_RBRACKET);
        return n;
    }
    if (t->kind == T_STAR) {
        advance();
        n->op = 2;
        if (accept(T_MUT)) n->ival = 1;
        else if (!accept(T_CONST)) die(cur()->line, "expected 'mut' or 'const' after '*', found %s", describe(cur()));
        n->a = parse_type();
        return n;
    }
    n->name = expect(T_IDENT)->text;
    /* a primitive never has arguments, so `x as i32 < y` stays a comparison */
    if (cur()->kind == T_LT && !is_prim_name(n->name)) {
        advance();
        n->gen = parse_targs();
    }
    return n;
}

static Node *parse_block(void);
static Node *parse_match(void);

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
    case T_LBRACKET: { /* [a, b, c]  or  [value; N] */
        advance();
        if (cur()->kind == T_RBRACKET) die(t->line, "an array needs at least one element");
        int save = nolit;
        nolit = 0;
        Node *first = parse_expr();
        Node *n;
        if (accept(T_SEMI)) {
            n = mk(N_REPEAT, t->line);
            n->a = first;
            n->b = parse_expr(); /* the length: a constant expression */
        } else {
            n = mk(N_ARRAY, t->line);
            n->a = first;
            Node **tail = &first->next;
            while (accept(T_COMMA) && cur()->kind != T_RBRACKET) {
                *tail = parse_expr();
                tail = &(*tail)->next;
            }
        }
        nolit = save;
        expect(T_RBRACKET);
        return n;
    }
    case T_CSTR: { /* c"text": a pointer to NUL-terminated bytes */
        advance();
        Node *n = mk(N_CSTR, t->line);
        n->name = t->str;
        return n;
    }
    case T_STR:
        die(t->line, "a string literal needs the c prefix, as in c\"text\"");
    case T_LPAREN: {
        advance();
        int save = nolit;
        nolit = 0;
        Node *e = parse_expr();
        nolit = save;
        expect(T_RPAREN);
        return e;
    }
    case T_MATCH:
        return parse_match();
    case T_IDENT: {
        advance();
        Node *targs = NULL; /* name::<A, B>: generic arguments written out */
        if (cur()->kind == T_COLONCOLON && peek(1)->kind == T_LT) {
            advance();
            advance();
            targs = parse_targs();
        }
        if (cur()->kind == T_COLONCOLON) { /* Enum::Variant or Enum::Variant(values) */
            advance();
            Token *v = expect(T_IDENT);
            Node *n = mk(N_VARIANT, t->line);
            n->tyname = t->text;
            n->name = v->text;
            n->gen = targs;
            if (cur()->kind == T_LPAREN) {
                advance();
                n->flag = 1;
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
            }
            return n;
        }
        if (cur()->kind == T_LPAREN) { /* call */
            advance();
            Node *n = mk(N_CALL, t->line);
            n->name = t->text;
            n->gen = targs;
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
            n->gen = targs;
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
        if (targs) die(t->line, "generic arguments must be followed by '::Variant', a call '(...)' or a struct literal '{ ... }'");
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
    while (cur()->kind == T_DOT || cur()->kind == T_LBRACKET) {
        int line = cur()->line;
        Node *n;
        if (advance()->kind == T_DOT) {
            n = mk(N_MEMBER, line);
            n->a = e;
            n->name = expect(T_IDENT)->text;
        } else {
            int save = nolit;
            nolit = 0;
            n = mk(N_INDEX, line);
            n->a = e;
            n->b = parse_expr();
            nolit = save;
            expect(T_RBRACKET);
        }
        e = n;
    }
    return e;
}

static Node *parse_unary(void) {
    Token *t = cur();
    if (t->kind == T_MINUS || t->kind == T_BANG || t->kind == T_TILDE) {
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
        n->ty = parse_type();
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
    case T_PIPE: return 5;
    case T_CARET: return 6;
    case T_AMP: return 7;
    case T_SHL: case T_SHR: return 8;
    case T_PLUS: case T_MINUS: return 9;
    case T_STAR: case T_SLASH: case T_PERCENT: return 10;
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
        if (accept(T_COLON)) n->ty = parse_type();
        if (!n->ty) {
            expect(T_ASSIGN);
            n->a = parse_expr();
        } else if (accept(T_ASSIGN)) {
            n->a = parse_expr();
        } /* else: declared now, assigned later; the flow pass checks that */
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
    case T_MATCH: { /* a match used as a statement needs no semicolon */
        Node *m = parse_match();
        m->flag = 1;
        return m;
    }
    case T_UNSAFE: { /* unsafe { ... }: an ordinary block whose op is 1 */
        advance();
        Node *b = parse_block();
        b->op = 1;
        return b;
    }
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
    b->end_line = advance()->line;
    return b;
}

/* ---- patterns ----
 *   Pattern ::= Atom ("|" Atom)*          alternatives (they may not bind names)
 *   Atom    ::= "_"  |  name  |  literal  |  Enum "::" Variant [ "(" Pattern, ... ")" ]
 * N_PAT: op is PAT_WILD, PAT_BIND (name), PAT_LIT (a = the literal), PAT_VARIANT (tyname::name, a = sub-patterns)
 * or PAT_OR (a = the alternatives). */
static Node *mk_pat(int kind, int line) {
    Node *p = mk(N_PAT, line);
    p->op = kind;
    return p;
}

static Node *parse_pattern(void);

static Node *parse_pattern_atom(void) {
    Token *t = cur();
    switch (t->kind) {
    case T_INT:
    case T_MINUS:
    case T_TRUE:
    case T_FALSE: {
        Node *p = mk_pat(PAT_LIT, t->line);
        Node *e;
        if (t->kind == T_MINUS) { /* a negative number: the same tree as the expression -5 */
            advance();
            Token *i = expect(T_INT);
            e = mk(N_UN, t->line);
            e->op = T_MINUS;
            e->a = mk(N_INT, i->line);
            e->a->ival = i->ival;
        } else if (t->kind == T_INT) {
            advance();
            e = mk(N_INT, t->line);
            e->ival = t->ival;
        } else {
            advance();
            e = mk(N_BOOL, t->line);
            e->ival = t->kind == T_TRUE;
        }
        p->a = e;
        return p;
    }
    case T_IDENT: {
        advance();
        if (cur()->kind == T_COLONCOLON) { /* Enum::Variant, maybe with sub-patterns */
            advance();
            Token *v = expect(T_IDENT);
            Node *p = mk_pat(PAT_VARIANT, t->line);
            p->tyname = t->text;
            p->name = v->text;
            if (accept(T_LPAREN)) {
                p->flag = 1;
                Node **tail = &p->a;
                while (cur()->kind != T_RPAREN) {
                    *tail = parse_pattern();
                    tail = &(*tail)->next;
                    if (!accept(T_COMMA)) break;
                }
                expect(T_RPAREN);
            }
            return p;
        }
        if (str_is(t->text, "_")) return mk_pat(PAT_WILD, t->line);
        Node *p = mk_pat(PAT_BIND, t->line);
        p->name = t->text;
        return p;
    }
    default:
        die(t->line, "expected a pattern, found %s", describe(t));
    }
}

static Node *parse_pattern(void) {
    Node *first = parse_pattern_atom();
    if (cur()->kind != T_PIPE) return first;
    Node *alt = mk_pat(PAT_OR, first->line);
    alt->a = first;
    Node **tail = &first->next;
    while (accept(T_PIPE)) {
        *tail = parse_pattern_atom();
        tail = &(*tail)->next;
    }
    return alt;
}

/* match scrutinee { pattern => { statements }   pattern => expression,  ... } */
static Node *parse_match(void) {
    Token *m = advance();
    Node *n = mk(N_MATCH, m->line);
    int save = nolit;
    nolit = 1; /* `match x {` : the brace opens the arms, not a struct literal */
    n->a = parse_expr();
    nolit = 0; /* inside the arms, struct literals are fine again */
    expect(T_LBRACE);
    Node **tail = &n->b;
    while (cur()->kind != T_RBRACE) {
        Node *arm = mk(N_ARM, cur()->line);
        arm->a = parse_pattern();
        expect(T_FATARROW);
        if (cur()->kind == T_LBRACE) { /* a block arm: statements, no value */
            arm->op = 1;
            arm->b = parse_block();
            accept(T_COMMA);
        } else { /* an expression arm */
            arm->b = parse_expr();
            if (cur()->kind != T_RBRACE) expect(T_COMMA);
            else accept(T_COMMA);
        }
        *tail = arm;
        tail = &arm->next;
    }
    nolit = save;
    expect(T_RBRACE);
    if (!n->b) die(m->line, "a match needs at least one arm");
    return n;
}

/* enum Name { Plain, WithValues(T, U), Numbered = 7, ... }   (also: copy enum) */
static Node *parse_enum(bool is_copy) {
    int line = advance()->line;
    Node *e = mk(N_ENUM, line);
    e->op = is_copy;
    e->name = expect(T_IDENT)->text;
    e->gen = parse_tparams();
    expect(T_LBRACE);
    Node **tail = &e->a;
    int index = 0;
    while (cur()->kind != T_RBRACE) {
        Token *v = expect(T_IDENT);
        Node *vd = mk(N_VDECL, v->line);
        vd->name = v->text;
        vd->index = index++;
        if (accept(T_LPAREN)) {
            vd->flag = 1;
            Node **ft = &vd->a;
            while (cur()->kind != T_RPAREN) {
                Node *fd = mk(N_FDECL, cur()->line);
                fd->ty = parse_type();
                *ft = fd;
                ft = &fd->next;
                if (!accept(T_COMMA)) break;
            }
            expect(T_RPAREN);
        }
        if (accept(T_ASSIGN)) vd->b = parse_expr(); /* an explicit discriminant */
        *tail = vd;
        tail = &vd->next;
        if (!accept(T_COMMA)) break;
    }
    expect(T_RBRACE);
    if (!e->a) die(line, "enum '%.*s' needs at least one variant", SF(e->name));
    return e;
}

static Node *parse_struct(bool is_copy) {
    int line = advance()->line;
    Node *s = mk(N_STRUCT, line);
    s->op = is_copy;
    s->name = expect(T_IDENT)->text;
    s->gen = parse_tparams();
    expect(T_LBRACE);
    Node **tail = &s->a;
    while (cur()->kind != T_RBRACE) {
        Token *f = expect(T_IDENT);
        expect(T_COLON);
        Node *fd = mk(N_FDECL, f->line);
        fd->name = f->text;
        fd->ty = parse_type();
        *tail = fd;
        tail = &fd->next;
        if (!accept(T_COMMA)) break;
    }
    expect(T_RBRACE);
    if (!s->a) die(line, "struct '%.*s' must have at least one field", SF(s->name));
    return s;
}

/* inout, sink, set and copy are keywords only where they can be: in front of a parameter's type, and in
 * front of `struct`. Anywhere else they are ordinary names (a register helper may have a parameter `set`). */
static bool starts_type(const Token *t) { return t->kind == T_IDENT || t->kind == T_LBRACKET || t->kind == T_STAR; }
static bool mode_word(const char *w) { return cur()->kind == T_IDENT && str_is(cur()->text, w) && starts_type(peek(1)); }

static Node *parse_fn(int ext) {
    int line = advance()->line;
    Node *f = mk(N_FN, line);
    f->ext = ext;
    f->name = expect(T_IDENT)->text;
    f->gen = parse_tparams();
    expect(T_LPAREN);
    Node **tail = &f->a;
    while (cur()->kind != T_RPAREN) {
        Token *pn = expect(T_IDENT);
        expect(T_COLON);
        Node *p = mk(N_PARAM, pn->line);
        p->name = pn->text;
        p->op = M_LET;
        if (mode_word("inout")) { advance(); p->op = M_INOUT; }
        else if (mode_word("sink")) { advance(); p->op = M_SINK; }
        else if (mode_word("set")) { advance(); p->op = M_SET; }
        else accept(T_LET);
        p->ty = parse_type();
        *tail = p;
        tail = &p->next;
        if (!accept(T_COMMA)) break;
    }
    expect(T_RPAREN);
    if (accept(T_ARROW)) f->ty = parse_type();
    if (ext == EXT_EXTERN) expect(T_SEMI); /* defined in C */
    else f->b = parse_block();
    return f;
}

/* const NAME: Type = expression;   (a compile-time value) */
static Node *parse_const(void) {
    int line = advance()->line;
    Node *c = mk(N_CONST, line);
    c->name = expect(T_IDENT)->text;
    expect(T_COLON);
    c->ty = parse_type();
    expect(T_ASSIGN);
    c->a = parse_expr();
    expect(T_SEMI);
    return c;
}

Node *parse_program(Token *tokens) {
    toks = tokens;
    pos = 0;
    nolit = 0;
    Node *head = NULL, **tail = &head;
    while (cur()->kind != T_EOF) {
        Node *d;
        if (cur()->kind == T_FN) d = parse_fn(EXT_NONE);
        else if (cur()->kind == T_EXTERN || cur()->kind == T_EXPORT) { /* extern "C" fn f(..); / export "C" fn f(..) {..} */
            int ext = cur()->kind == T_EXTERN ? EXT_EXTERN : EXT_EXPORT;
            const char *what = ext == EXT_EXTERN ? "extern" : "export";
            advance();
            Token *abi = expect(T_STR);
            if (!str_is(abi->str, "C")) die(abi->line, "only the \"C\" calling convention is supported");
            if (cur()->kind != T_FN) die(cur()->line, "expected 'fn' after '%s \"C\"', found %s", what, describe(cur()));
            d = parse_fn(ext);
        }
        else if (cur()->kind == T_CONST) d = parse_const();
        else if (cur()->kind == T_STRUCT) d = parse_struct(false);
        else if (cur()->kind == T_ENUM) d = parse_enum(false);
        else if (cur()->kind == T_IDENT && str_is(cur()->text, "copy")) {
            advance();
            if (cur()->kind == T_ENUM) d = parse_enum(true);
            else if (cur()->kind == T_STRUCT) d = parse_struct(true);
            else die(cur()->line, "expected 'struct' or 'enum' after 'copy', found %s", describe(cur()));
        } else die(cur()->line, "expected 'fn', 'struct', 'enum', 'copy struct', 'const', 'extern' or 'export', found %s", describe(cur()));
        *tail = d;
        tail = &d->next;
    }
    return head;
}
