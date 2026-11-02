#include <ctype.h>

#include "noge.h"

static const char *tok_names[] = {
#define X(k, s) s,
    TOKEN_LIST(X)
#undef X
};

const char *tok_name(TokKind k) { return tok_names[k]; }

const char *op_name(int op) {
    switch (op) {
    case OP_WADD: return "wrapping_add";
    case OP_WSUB: return "wrapping_sub";
    case OP_WMUL: return "wrapping_mul";
    default: return tok_names[op];
    }
}

static const struct { const char *s; TokKind k; } keywords[] = {
    {"fn", T_FN},       {"struct", T_STRUCT}, {"let", T_LET},     {"var", T_VAR},
    {"if", T_IF},       {"else", T_ELSE},     {"while", T_WHILE}, {"return", T_RETURN},
    {"as", T_AS},       {"true", T_TRUE},     {"false", T_FALSE}, {"inout", T_INOUT},
    {"sink", T_SINK},   {"set", T_SET},       {"copy", T_COPY},
};

/* two-character operators first so they win over their prefixes */
static const struct { const char *s; TokKind k; } puncts[] = {
    {"->", T_ARROW}, {"==", T_EQ},  {"!=", T_NE},     {"<=", T_LE},    {">=", T_GE},
    {"&&", T_ANDAND}, {"||", T_OROR}, {"<<", T_SHL},  {">>", T_SHR},
    {"(", T_LPAREN}, {")", T_RPAREN}, {"{", T_LBRACE},
    {"}", T_RBRACE}, {",", T_COMMA}, {":", T_COLON},  {";", T_SEMI},   {".", T_DOT},
    {"=", T_ASSIGN}, {"<", T_LT},    {">", T_GT},     {"+", T_PLUS},   {"-", T_MINUS},
    {"*", T_STAR},   {"/", T_SLASH}, {"%", T_PERCENT}, {"!", T_BANG},  {"&", T_AMP},
    {"|", T_PIPE},   {"^", T_CARET}, {"~", T_TILDE},   {"[", T_LBRACKET}, {"]", T_RBRACKET},
};

static int digit_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 99;
}

Token *lex(const char *src) {
    size_t cap = 256, n = 0;
    Token *t = malloc(cap * sizeof *t);
    int line = 1;
    const char *p = src;

    for (;;) {
        while (*p) {
            if (*p == '\n') { line++; p++; }
            else if (*p == ' ' || *p == '\t' || *p == '\r') p++;
            else if (p[0] == '/' && p[1] == '/') { while (*p && *p != '\n') p++; }
            else break;
        }
        if (n + 8 > cap) {
            cap *= 2;
            t = realloc(t, cap * sizeof *t);
        }
        Token tk = {0};
        tk.line = line;
        tk.text.p = p;

        if (!*p) {
            tk.kind = T_EOF;
            for (int i = 0; i < 4; i++) t[n++] = tk; /* room for lookahead */
            return t;
        }

        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *s = p;
            while (isalnum((unsigned char)*p) || *p == '_') p++;
            tk.text.n = (int)(p - s);
            tk.kind = T_IDENT;
            for (size_t i = 0; i < sizeof keywords / sizeof *keywords; i++)
                if (str_is(tk.text, keywords[i].s)) tk.kind = keywords[i].k;
        } else if (isdigit((unsigned char)*p)) {
            const char *s = p;
            uint64_t base = 10, v = 0;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
            const char *digits = p;
            while (digit_val((unsigned char)*p) < (int)base) {
                uint64_t d = (uint64_t)digit_val((unsigned char)*p);
                if (v > (UINT64_MAX - d) / base) die(line, "integer literal too large");
                v = v * base + d;
                p++;
            }
            if (p == digits) die(line, "expected digits after '0x'");
            if (isalnum((unsigned char)*p) || *p == '_')
                die(line, "invalid character '%c' in number", *p);
            tk.kind = T_INT;
            tk.ival = v;
            tk.text.n = (int)(p - s);
        } else {
            bool found = false;
            for (size_t i = 0; i < sizeof puncts / sizeof *puncts; i++) {
                size_t len = strlen(puncts[i].s);
                if (strncmp(p, puncts[i].s, len) == 0) {
                    tk.kind = puncts[i].k;
                    tk.text.n = (int)len;
                    p += len;
                    found = true;
                    break;
                }
            }
            if (!found) die(line, "unexpected character '%c'", *p);
        }
        t[n++] = tk;
    }
}
