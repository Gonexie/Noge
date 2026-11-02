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
    X(T_TRUE, "'true'") X(T_FALSE, "'false'")                                  \
    X(T_LPAREN, "'('") X(T_RPAREN, "')'")                  \
    X(T_LBRACE, "'{'") X(T_RBRACE, "'}'") X(T_COMMA, "','")                    \
    X(T_COLON, "':'") X(T_SEMI, "';'") X(T_DOT, "'.'") X(T_ARROW, "'->'")      \
    X(T_ASSIGN, "'='") X(T_EQ, "'=='") X(T_NE, "'!='") X(T_LT, "'<'")          \
    X(T_LE, "'<='") X(T_GT, "'>'") X(T_GE, "'>='") X(T_PLUS, "'+'")            \
    X(T_MINUS, "'-'") X(T_STAR, "'*'") X(T_SLASH, "'/'")                       \
    X(T_PERCENT, "'%'") X(T_ANDAND, "'&&'") X(T_OROR, "'||'")                  \
    X(T_BANG, "'!'") X(T_AMP, "'&'") X(T_PIPE, "'|'") X(T_CARET, "'^'")      \
    X(T_TILDE, "'~'") X(T_SHL, "'<<'") X(T_SHR, "'>>'")                     \
    X(T_LBRACKET, "'['") X(T_RBRACKET, "']'") X(T_STR, "string")           \
    X(T_CSTR, "C string") X(T_UNSAFE, "'unsafe'") X(T_EXTERN, "'extern'")       \
    X(T_EXPORT, "'export'") X(T_CONST, "'const'") X(T_MUT, "'mut'")             \
    X(T_FATARROW, "'=>'") X(T_COLONCOLON, "'::'") X(T_ENUM, "'enum'")          \
    X(T_MATCH, "'match'")

typedef enum {
#define X(k, s) k,
    TOKEN_LIST(X)
#undef X
    T__COUNT
} TokKind;

/* operators that have no token of their own (the wrapping_* intrinsics) */
enum { OP_WADD = T__COUNT, OP_WSUB, OP_WMUL };
const char *op_name(int op);

typedef struct {
    TokKind kind;
    Str text;
    int line;
    uint64_t ival;
    Str str; /* T_STR / T_CSTR: the bytes after escapes are processed */
} Token;

const char *tok_name(TokKind k);
Token *lex(const char *src); /* array terminated by several T_EOF tokens */

/* ---- types ---- */
typedef struct Node Node;
typedef enum { TY_UNIT, TY_BOOL, TY_INT, TY_STRUCT, TY_ARRAY, TY_PTR, TY_ENUM } TyKind;
typedef struct Type {
    TyKind kind;
    const char *name;
    int bits;
    bool is_signed;
    Node *decl;        /* struct declaration */
    const char *cname; /* name in the emitted C */
    bool is_copy;      /* may be duplicated implicitly; otherwise move-only */
    int nleaves;       /* scalar fields after flattening nested structs; an array is one leaf */
    struct Type *elem; /* array element type */
    uint64_t len;      /* array length */
    struct Type *next_array; /* all array types, in creation order */
    bool is_mut;             /* pointer: *mut rather than *const (elem is the pointee) */
    struct Type *next_ptr;   /* all pointer types, in creation order */
    struct Type *tag_type;   /* enum: the integer type of the tag, which holds the discriminant */
    unsigned char mark;      /* emitter: type definition already printed */
} Type;

extern Type *g_array_types; /* head of the list of array types */
extern Type *g_ptr_types;   /* head of the list of pointer types */
extern Type *ty_usize, *ty_isize;
extern bool g_freestanding; /* no hosted C library: no stdio, no print */
extern int g_ptr_bits;      /* width of usize / isize / pointers on the target */
static inline bool is_aggregate(const Type *t) { return t->kind == TY_STRUCT || t->kind == TY_ARRAY || t->kind == TY_ENUM; }

extern Type *ty_unit, *ty_bool, *ty_i32;

/* ---- AST ---- */
typedef enum {
    /* expressions */
    N_INT, N_BOOL, N_NAME, N_BIN, N_UN, N_ADDR, N_CALL, N_MEMBER, N_LIT,
    N_INITF, N_CAST, N_INDEX, N_ARRAY, N_REPEAT, N_CSTR, N_PTROP, N_VARIANT, N_MATCH,
    /* statements */
    N_LET, N_ASSIGN, N_EXPR, N_IF, N_WHILE, N_RETURN, N_BLOCK,
    /* declarations */
    N_FN, N_PARAM, N_STRUCT, N_FDECL, N_TYPE, N_CONST, N_ENUM, N_VDECL,
    /* parts of a match */
    N_ARM, N_PAT
} NodeKind;

/* N_PAT: what a pattern is */
enum { PAT_WILD, PAT_BIND, PAT_LIT, PAT_VARIANT, PAT_OR };

/* How a local variable came to be. The flow pass needs to know which ones are borrowed parameters. */
enum { LK_VAR, LK_LET, LK_LETPARAM, LK_SINK, LK_INOUT, LK_SET };
typedef struct { Str name; Type *type; int kind; } LocalInfo;

typedef enum { M_LET, M_INOUT, M_SINK, M_SET } ParamMode;

/* raw-pointer operations (N_PTROP); all of them need an unsafe block */
enum { PO_READ, PO_WRITE, PO_VREAD, PO_VWRITE, PO_OFFSET };

/* N_FN: how the function relates to C */
enum { EXT_NONE, EXT_EXTERN, EXT_EXPORT };

struct Node {
    NodeKind kind;
    int line;
    Str name;   /* identifier / field / callee */
    Str tyname; /* written type annotation, if any */
    Node *ty;   /* written type annotation as a tree (N_TYPE), if any */
    Node *a, *b, *c;
    Node *next; /* sibling in a list */
    int op;     /* operator token, param mode, var-vs-let, literal sign */
    int local;  /* local variable id (function-unique) */
    int by_ptr; /* NAME refers to an inout parameter (a C pointer) */
    int mut;    /* NAME refers to something assignable */
    int leaf_off;  /* FDECL: first scalar leaf of this field inside its struct */
    int nlocals;   /* FN: number of local ids used (params included) */
    int end_line;  /* BLOCK: line of the closing brace */
    int ext;       /* FN: EXT_NONE, EXT_EXTERN (declared in C) or EXT_EXPORT (named for C) */
    int index;     /* VDECL: position of the variant in its enum */
    int flag;      /* VDECL / VARIANT / PAT: written with parentheses; MATCH: used as a statement */
    LocalInfo *linfo; /* FN: every local variable by id (parameters first) */
    uint64_t ival;
    Type *type;  /* resolved type */
    Node *decl;  /* resolved callee / field */
};

Node *parse_program(Token *toks); /* list of N_FN / N_STRUCT */
void check_program(Node *prog);
void flow_program(Node *prog); /* init / move / exclusivity checks */

#endif
