#include "ir.h"

static void usage(void) {
    fprintf(stderr, "usage: nogec [-o out.c] [--no-builtins] input.nge\n"
                    "  --no-builtins   do not use __builtin_*_overflow; emit portable C99 checks instead\n");
}

int main(int argc, char **argv) {
    const char *in = NULL, *outpath = NULL;
    EmitOpts opts = {0};
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) outpath = argv[++i];
        else if (strcmp(argv[i], "--no-builtins") == 0) opts.portable = true;
        else if (!in && argv[i][0] != '-') in = argv[i];
        else {
            usage();
            return 2;
        }
    }
    if (!in) {
        usage();
        return 2;
    }
    g_file = in;
    char *src = read_file(in);
    Node *prog = parse_program(lex(src));
    check_program(prog);
    flow_program(prog);
    IrFunc *ir = lower_program(prog);

    FILE *out = outpath ? fopen(outpath, "w") : stdout;
    if (!out) {
        fprintf(stderr, "noge: cannot write '%s'\n", outpath);
        return 1;
    }
    emit_c(out, prog, ir, &opts);
    if (outpath) fclose(out);
    return 0;
}
