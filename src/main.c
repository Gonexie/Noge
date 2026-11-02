#include "noge.h"

int main(int argc, char **argv) {
    const char *in = NULL, *outpath = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) outpath = argv[++i];
        else if (!in && argv[i][0] != '-') in = argv[i];
        else {
            fprintf(stderr, "usage: nogec [-o out.c] input.nge\n");
            return 2;
        }
    }
    if (!in) {
        fprintf(stderr, "usage: nogec [-o out.c] input.nge\n");
        return 2;
    }
    g_file = in;
    char *src = read_file(in);
    Node *prog = parse_program(lex(src));
    check_program(prog);
    flow_program(prog);

    FILE *out = outpath ? fopen(outpath, "w") : stdout;
    if (!out) {
        fprintf(stderr, "noge: cannot write '%s'\n", outpath);
        return 1;
    }
    emit_c(out, prog);
    if (outpath) fclose(out);
    return 0;
}
