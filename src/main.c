#include "ir.h"

static void usage(void) {
    fprintf(stderr, "usage: nogec [options] [-o out.c] input.nge\n"
                    "  --no-builtins     do not use __builtin_*_overflow; emit portable C99 checks instead\n"
                    "  --freestanding    no C library: no print, no stdio; run-time checks call noge_trap_hook\n"
                    "  --ptr-bits=N      width of usize, isize and pointers on the target: 32 or 64 (the host's by default)\n"
                    "  --target=cortex-m shorthand for --freestanding --ptr-bits=32\n"
                    "  --emit-dir=DIR    write DIR/noge_types.h, a C file per module and a Makefile instead of one C file (files that did not change are left alone)\n"
                    "  --std-dir=DIR     where the standard library (std/) is; also the NOGE_STD variable\n"
                    "  --no-prelude      do not import std/prelude.nge (Option and Result) into every module\n");
}

int main(int argc, char **argv) {
    const char *in = NULL, *outpath = NULL, *emit_dir = NULL;
    EmitOpts opts = {0};
    const char *std_dir = getenv("NOGE_STD");
    bool prelude = true;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) outpath = argv[++i];
        else if (strcmp(argv[i], "--no-builtins") == 0) opts.portable = true;
        else if (strncmp(argv[i], "--std-dir=", 10) == 0) std_dir = argv[i] + 10;
        else if (strncmp(argv[i], "--emit-dir=", 11) == 0) emit_dir = argv[i] + 11;
        else if (strcmp(argv[i], "--no-prelude") == 0) prelude = false;
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
    if (!std_dir) { /* std/ next to the folder nogec is in: build/nogec, std/ */
        const char *slash = strrchr(argv[0], '/');
        if (slash) {
            size_t n = (size_t)(slash - argv[0]) + 16;
            char *guess = malloc(n + 16);
            if (guess) {
                snprintf(guess, n + 16, "%.*s/../std", (int)(slash - argv[0]), argv[0]);
                char probe[4096];
                snprintf(probe, sizeof probe, "%s/prelude.nge", guess);
                FILE *f = fopen(probe, "rb");
                if (f) {
                    fclose(f);
                    std_dir = guess;
                }
            }
        }
    }
    g_std_dir = std_dir;
    g_file = in;
    FILE *probe_in = fopen(in, "rb"); /* a missing program is not an error in the program */
    if (!probe_in) {
        fprintf(stderr, "noge: cannot open '%s'\n", in);
        return 1;
    }
    fclose(probe_in);
    Node *prog = load_program(in, prelude);
    prog = check_program(prog);
    flow_program(prog);
    IrFunc *ir = lower_program(prog);

    if (emit_dir) {
        if (outpath) {
            fprintf(stderr, "nogec: -o and --emit-dir cannot be used together\n");
            return 2;
        }
        emit_c_dir(emit_dir, prog, ir, &opts);
        return 0;
    }
    FILE *out = outpath ? fopen(outpath, "w") : stdout;
    if (!out) {
        fprintf(stderr, "noge: cannot write '%s'\n", outpath);
        return 1;
    }
    emit_c(out, prog, ir, &opts);
    if (outpath) fclose(out);
    return 0;
}
