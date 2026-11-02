/* Noge compiler: shared declarations. */
#ifndef NOGE_H
#define NOGE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#define NORETURN __attribute__((noreturn))
#define PRINTF_FMT(a, b) __attribute__((format(printf, a, b)))
#else
#define NORETURN
#define PRINTF_FMT(a, b)
#endif

/* ---- strings (views into the source buffer, not NUL terminated) ---- */
typedef struct { const char *p; int n; } Str;
static inline bool str_eq(Str a, Str b) { return a.n == b.n && memcmp(a.p, b.p, (size_t)a.n) == 0; }
static inline bool str_is(Str a, const char *c) {
    size_t n = strlen(c);
    return (size_t)a.n == n && memcmp(a.p, c, n) == 0;
}
#define SF(s) (int)(s).n, (s).p /* for printf("%.*s") */

/* ---- arena: everything the compiler allocates lives here ---- */
typedef struct Chunk Chunk;
typedef struct { Chunk *head; } Arena;
extern Arena g_arena;
void *arena_alloc(Arena *a, size_t n); /* zeroed */
char *arena_strn(Arena *a, const char *s, int n);
#define ARENA_NEW(a, T) ((T *)arena_alloc((a), sizeof(T)))

/* ---- diagnostics ---- */
extern const char *g_file;
NORETURN PRINTF_FMT(2, 3) void die(int line, const char *fmt, ...);
char *read_file(const char *path);

/* ---- tokens ---- */
#define TOKEN_LIST(X)                                                          \
    X(T_EOF, "end of file") X(T_IDENT, "identifier") X(T_INT, "integer")      \
    X(T_FN, "'fn'") X(T_STRUCT, "'struct'") X(T_LET, "'let'")                  \
    X(T_VAR, "'var'") X(T_IF, "'if'") X(T_ELSE, "'else'")                      \
    X(T_WHILE, "'while'") X(T_RETURN, "'return'") X(T_AS, "'as'")              \
    X(T_TRUE, "'true'") X(T_FALSE, "'false'") X(T_INOUT, "'inout'")            \
    X(T_SINK, "'sink'") X(T_LPAREN, "'('") X(T_RPAREN, "')'")                  \
    X(T_LBRACE, "'{'") X(T_RBRACE, "'}'") X(T_COMMA, "','")                    \
    X(T_COLON, "':'") X(T_SEMI, "';'") X(T_DOT, "'.'") X(T_ARROW, "'->'")      \
    X(T_ASSIGN, "'='") X(T_EQ, "'=='") X(T_NE, "'!='") X(T_LT, "'<'")          \
    X(T_LE, "'<='") X(T_GT, "'>'") X(T_GE, "'>='") X(T_PLUS, "'+'")            \
    X(T_MINUS, "'-'") X(T_STAR, "'*'") X(T_SLASH, "'/'")                       \
    X(T_PERCENT, "'%'") X(T_ANDAND, "'&&'") X(T_OROR, "'||'")                  \
    X(T_BANG, "'!'") X(T_AMP, "'&'")

typedef enum {
#define X(k, s) k,
    TOKEN_LIST(X)
#undef X
    T__COUNT
} TokKind;

typedef struct {
    TokKind kind;
    Str text;
    int line;
    uint64_t ival;
} Token;

const char *tok_name(TokKind k);
Token *lex(const char *src); /* array terminated by several T_EOF tokens */

/* ---- types ---- */
typedef struct Node Node;
typedef enum { TY_UNIT, TY_BOOL, TY_INT, TY_STRUCT } TyKind;
typedef struct Type {
    TyKind kind;
    const char *name;
    int bits;
    bool is_signed;
    Node *decl;        /* struct declaration */
    const char *cname; /* name in the emitted C */
} Type;

extern Type *ty_unit, *ty_bool, *ty_i32;

/* ---- AST ---- */
typedef enum {
    /* expressions */
    N_INT, N_BOOL, N_NAME, N_BIN, N_UN, N_ADDR, N_CALL, N_MEMBER, N_LIT,
    N_INITF, N_CAST,
    /* statements */
    N_LET, N_ASSIGN, N_EXPR, N_IF, N_WHILE, N_RETURN, N_BLOCK,
    /* declarations */
    N_FN, N_PARAM, N_STRUCT, N_FDECL
} NodeKind;

typedef enum { M_LET, M_INOUT, M_SINK } ParamMode;

struct Node {
    NodeKind kind;
    int line;
    Str name;   /* identifier / field / callee */
    Str tyname; /* written type annotation, if any */
    Node *a, *b, *c;
    Node *next; /* sibling in a list */
    int op;     /* operator token, param mode, var-vs-let, literal sign */
    int local;  /* local variable id (function-unique) */
    int by_ptr; /* NAME refers to an inout parameter (a C pointer) */
    int mut;    /* NAME refers to something assignable */
    uint64_t ival;
    Type *type;  /* resolved type */
    Node *decl;  /* resolved callee / field */
};

Node *parse_program(Token *toks); /* list of N_FN / N_STRUCT */
void check_program(Node *prog);
void emit_c(FILE *out, Node *prog);

#endif
