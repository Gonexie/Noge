/* The IR between the checked AST and the C backend.
 *
 * It is "structured three-address code": every instruction does one thing to
 * variables and constants, `if` and `while` stay nested (the language has no
 * other control flow), and every runtime check (bounds, overflow, division,
 * shifts) is spelled out as part of an instruction. Lowering fixes the order
 * of evaluation: operands, call arguments and literal fields are evaluated
 * left to right, an assignment evaluates its right side before its left place.
 */
#ifndef NOGE_IR_H
#define NOGE_IR_H

#include "noge.h"

typedef struct IrVar {
    int id;
    Type *type;
    Str name;      /* user variable; empty for a temporary */
    bool is_temp;
    bool by_ptr;   /* the C variable is a pointer to the value (inout / set / aggregate let) */
    bool is_const; /* ... to const (a `let` parameter) */
} IrVar;

typedef struct {
    IrVar *var;  /* NULL: a constant */
    uint64_t c;  /* constant bits, two's complement for negative values */
    Type *type;
} Operand;

/* one step into a value */
enum { PK_FIELD, PK_INDEX, PK_TAG, PK_PAYLOAD };
typedef struct Proj Proj;
struct Proj {
    int kind;
    Node *field;   /* PK_FIELD: the struct field */
    Operand idx;   /* PK_INDEX: the (already bounds-checked) index */
    Node *variant; /* PK_PAYLOAD: the variant, and which of the values it carries */
    int fidx;
    Proj *next;
};

typedef struct {
    IrVar *base;
    Proj *proj;
    Type *type; /* type of the whole place */
} IrPlace;

typedef struct {
    bool addr;     /* pass the address of `place` instead of the value `val` */
    Operand val;
    IrPlace place;
} Arg;

typedef enum {
    I_DECL,     /* declare dst, initialized from a (has_init) or zeroed */
    I_STORE,    /* place = a */
    I_LOAD,     /* dst = place */
    I_BIN,      /* dst = a bop b   (operands have type `type`) */
    I_UN,       /* dst = bop a */
    I_CAST,     /* dst = (dst->type) a */
    I_BOUNDS,   /* trap unless a < len */
    I_MKSTRUCT, /* dst = struct built from ops, in declaration order */
    I_MKARRAY,  /* dst = array built from ops */
    I_MKENUM,   /* dst = enum value: `variant` with the values in ops */
    I_FILL,     /* dst = array with every element equal to a */
    I_CALL,     /* [dst =] callee(args) */
    I_USE,      /* (void)a: a computed value that is deliberately dropped */
    I_STR,      /* dst = pointer to the NUL-terminated bytes in str */
    I_PREAD,    /* dst = *a   (vol: volatile) */
    I_PWRITE,   /* *a = b     (vol: volatile) */
    I_POFFSET,  /* dst = a + b elements */
    I_IF,       /* if (a) then else els */
    I_WHILE,    /* loop: cond block, leave unless a, body */
    I_RETURN,   /* return [a] */
    I_BLOCK     /* { then } */
} IrOp;

typedef struct Instr Instr;
typedef struct {
    Instr *first, *last;
} Block;

struct Instr {
    IrOp op;
    int line;
    int bop;        /* I_BIN / I_UN: a token kind or an OP_* pseudo operator */
    bool has_init;  /* I_DECL */
    bool fresh;     /* this instruction declares dst */
    bool vol;       /* I_PREAD / I_PWRITE: volatile access */
    Str str;        /* I_STR */
    IrVar *dst;
    Operand a, b;
    Type *type;     /* I_BIN / I_UN: type of the operands */
    uint64_t len;   /* I_BOUNDS / I_FILL */
    IrPlace place;  /* I_LOAD / I_STORE */
    Operand *ops;   /* I_MKSTRUCT / I_MKARRAY */
    int nops;
    Node *callee;   /* I_CALL */
    Node *variant;  /* I_MKENUM */
    Arg *args;
    int nargs;
    Block then, els; /* I_IF; I_BLOCK uses `then` */
    Block cond, body; /* I_WHILE */
    Instr *next;
};

typedef struct IrFunc IrFunc;
struct IrFunc {
    Node *decl;
    IrVar **params;
    int nparams;
    Type *ret;
    Block body;
    IrFunc *next;
};

typedef struct {
    bool portable;     /* do not rely on __builtin_*_overflow */
    bool freestanding; /* no hosted C library: no stdio, no print, a link-time trap hook */
} EmitOpts;

IrFunc *lower_program(Node *prog);
void emit_c(FILE *out, Node *prog, IrFunc *funcs, const EmitOpts *opts);
/* the same program as a header, a C file per module and a Makefile in a folder; a file is only rewritten if it changed */
void emit_c_dir(const char *dir, Node *prog, IrFunc *funcs, const EmitOpts *opts);

#endif
