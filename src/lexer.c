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
    {"as", T_AS},       {"true", T_TRUE},     {"false", T_FALSE}, {"unsafe", T_UNSAFE},
    {"extern", T_EXTERN}, {"export", T_EXPORT}, {"const", T_CONST}, {"mut", T_MUT},
    {"enum", T_ENUM},   {"match", T_MATCH},
};

/* two-character operators first so they win over their prefixes */
static const struct { const char *s; TokKind k; } puncts[] = {
    {"->", T_ARROW}, {"==", T_EQ},  {"!=", T_NE},     {"<=", T_LE},    {">=", T_GE},
    {"&&", T_ANDAND}, {"||", T_OROR}, {"<<", T_SHL},  {">>", T_SHR},
    {"=>", T_FATARROW}, {"::", T_COLONCOLON},
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

        if (*p == '"' || (*p == 'c' && p[1] == '"')) { /* "text" or c"text" */
            const char *s = p;
            tk.kind = *p == 'c' ? T_CSTR : T_STR;
            if (*p == 'c') p++;
            p++; /* opening quote */
            char *buf = arena_alloc(&g_arena, (size_t)(strlen(p) < 4096 ? strlen(p) : 4096) + 1);
            int n = 0;
            for (;; p++) {
                if (!*p || *p == '\n') die(line, "unterminated string literal");
                if (*p == '"') break;
                if (n >= 4095) die(line, "string literal is too long");
                if (*p != '\\') {
                    buf[n++] = *p;
                    continue;
                }
                p++;
                switch (*p) {
                case 'n': buf[n++] = '\n'; break;
                case 'r': buf[n++] = '\r'; break;
                case 't': buf[n++] = '\t'; break;
                case '0': buf[n++] = '\0'; break;
                case '\\': buf[n++] = '\\'; break;
                case '"': buf[n++] = '"'; break;
                case 'x':
                    if (digit_val((unsigned char)p[1]) > 15 || digit_val((unsigned char)p[2]) > 15)
                        die(line, "'\\x' needs two hexadecimal digits");
                    buf[n++] = (char)(digit_val((unsigned char)p[1]) * 16 + digit_val((unsigned char)p[2]));
                    p += 2;
                    break;
                default: die(line, "unknown escape '\\%c' in string literal", *p ? *p : ' ');
                }
            }
            p++; /* closing quote */
            tk.text.n = (int)(p - s);
            tk.str.p = buf;
            tk.str.n = n;
        } else if (*p == '\'') { /* 'a': an integer literal holding the character's code (ASCII only) */
            const char *s = p++;
            uint64_t v;
            if (*p == '\'') die(line, "empty character literal");
            if (!*p || *p == '\n') die(line, "unterminated character literal");
            if ((unsigned char)*p >= 0x80) die(line, "a character literal holds one ASCII character (write other bytes as \\xHH)");
            if (*p != '\\') v = (unsigned char)*p++;
            else {
                p++;
                switch (*p) {
                case 'n': v = '\n'; break;
                case 'r': v = '\r'; break;
                case 't': v = '\t'; break;
                case '0': v = 0; break;
                case '\\': v = '\\'; break;
                case '\'': v = '\''; break;
                case '"': v = '"'; break;
                case 'x':
                    if (digit_val((unsigned char)p[1]) > 15 || digit_val((unsigned char)p[2]) > 15)
                        die(line, "'\\x' needs two hexadecimal digits");
                    v = (uint64_t)(digit_val((unsigned char)p[1]) * 16 + digit_val((unsigned char)p[2]));
                    p += 2;
                    break;
                default: die(line, "unknown escape '\\%c' in character literal", *p ? *p : ' ');
                }
                p++;
            }
            if (*p != '\'') die(line, "a character literal holds exactly one character (it is not closed after it)");
            p++;
            tk.kind = T_INT;
            tk.ival = v;
            tk.text.n = (int)(p - s);
        } else if (isalpha((unsigned char)*p) || *p == '_') {
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
            else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) { base = 2; p += 2; }
            const char *digits = p;
            for (;;) {
                if (*p == '_' && p != digits && (digit_val((unsigned char)p[1]) < (int)base || p[1] == '_')) {
                    p++; /* digit separator: 0x4002_1018 */
                    continue;
                }
                if (digit_val((unsigned char)*p) >= (int)base) break;
                uint64_t d = (uint64_t)digit_val((unsigned char)*p);
                if (v > (UINT64_MAX - d) / base) die(line, "integer literal too large");
                v = v * base + d;
                p++;
            }
            if (p == digits) die(line, "expected digits after '%.2s'", s);
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
