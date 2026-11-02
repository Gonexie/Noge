#include "ir.h"

static void usage(void) {
    fprintf(stderr, "usage: nogec [options] [-o out.c] input.nge\n"
                    "  --no-builtins     do not use __builtin_*_overflow; emit portable C99 checks instead\n"
                    "  --freestanding    no C library: no print, no stdio; run-time checks call noge_trap_hook\n"
                    "  --ptr-bits=N      width of usize, isize and pointers on the target: 32 or 64 (the host's by default)\n"
                    "  --target=cortex-m shorthand for --freestanding --ptr-bits=32\n");
}

int main(int argc, char **argv) {
    const char *in = NULL, *outpath = NULL;
    EmitOpts opts = {0};
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) outpath = argv[++i];
        else if (strcmp(argv[i], "--no-builtins") == 0) opts.portable = true;
        else if (strcmp(argv[i], "--freestanding") == 0) g_freestanding = true;
        else if (strncmp(argv[i], "--ptr-bits=", 11) == 0) {
            g_ptr_bits = atoi(argv[i] + 11);
            if (g_ptr_bits != 32 && g_ptr_bits != 64) {
                fprintf(stderr, "nogec: --ptr-bits must be 32 or 64 (16-bit targets are not supported)\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--target=cortex-m") == 0) { /* 32-bit, no hosted library */
            g_freestanding = true;
            g_ptr_bits = 32;
        } else if (strcmp(argv[i], "--target=host") == 0) {
            g_freestanding = false;
        }
        else if (!in && argv[i][0] != '-') in = argv[i];
        else {
            usage();
            return 2;
        }
    }
    opts.freestanding = g_freestanding;
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
