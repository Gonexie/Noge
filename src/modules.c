/* Loading a program that is made of several files. Each file is a module. The program's own file is module 0.
 * `import "path.nge" as name;` makes the module's public names available as name::item, and `import "path.nge";`
 * merges them into the importing module's own names (which win over them). A file that is imported by several
 * others is loaded once. The standard prelude (std/prelude.nge) is imported, last in line, by every module. */
#include "noge.h"

Module **g_modules;
int g_nmodules;
const char *g_std_dir;

static int cap;
static signed char *state; /* per module: 1 while it is being loaded, 2 when done */

static bool file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

/* "a/./b/../c.nge" -> "a/c.nge", without looking at the file system */
static char *normalize(const char *path) {
    size_t n = strlen(path);
    char *out = arena_alloc(&g_arena, n + 2);
    char **parts = arena_alloc(&g_arena, sizeof(char *) * (n + 2));
    char *copy = arena_strn(&g_arena, path, (int)n);
    int np = 0;
    bool abs = path[0] == '/';
    for (char *p = copy; p && *p;) {
        char *slash = strchr(p, '/');
        if (slash) *slash = 0;
        if (strcmp(p, "..") == 0 && np > 0 && strcmp(parts[np - 1], "..") != 0) np--;
        else if (*p && strcmp(p, ".") != 0) parts[np++] = p;
        p = slash ? slash + 1 : NULL;
    }
    char *w = out;
    if (abs) *w++ = '/';
    for (int i = 0; i < np; i++) {
        if (i) *w++ = '/';
        size_t l = strlen(parts[i]);
        memcpy(w, parts[i], l);
        w += l;
    }
    *w = 0;
    return out;
}

/* `rel` seen from the folder `file` is in */
static char *join_dir(const char *file, const char *rel) {
    if (rel[0] == '/') return normalize(rel);
    const char *slash = strrchr(file, '/');
    size_t dn = slash ? (size_t)(slash - file + 1) : 0;
    char *both = arena_alloc(&g_arena, dn + strlen(rel) + 1);
    memcpy(both, file, dn);
    strcpy(both + dn, rel);
    return normalize(both);
}

static unsigned fnv(const char *s, unsigned h) {
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

/* The path as it will not change with where nogec is run from: relative to the program's own folder, or std/... */
static const char *stable_path(const char *path) {
    if (g_nmodules > 0) {
        const char *root = g_modules[0]->path;
        const char *slash = strrchr(root, '/');
        size_t dn = slash ? (size_t)(slash - root + 1) : 0;
        if (dn && strncmp(path, root, dn) == 0) return path + dn;
        if (g_std_dir) {
            char *sd = normalize(g_std_dir);
            size_t n = strlen(sd);
            if (strncmp(path, sd, n) == 0 && path[n] == '/') {
                char *r = arena_alloc(&g_arena, strlen(path) + 8);
                sprintf(r, "std/%s", path + n + 1);
                return r;
            }
        }
    }
    return path;
}

static Module *new_module(const char *path) {
    if (g_nmodules == cap) {
        int ncap = cap ? cap * 2 : 8;
        Module **m = arena_alloc(&g_arena, sizeof(Module *) * (size_t)ncap);
        signed char *s = arena_alloc(&g_arena, (size_t)ncap);
        if (g_nmodules) {
            memcpy(m, g_modules, sizeof(Module *) * (size_t)g_nmodules);
            memcpy(s, state, (size_t)g_nmodules);
        }
        g_modules = m;
        state = s;
        cap = ncap;
    }
    Module *m = ARENA_NEW(&g_arena, Module);
    m->id = g_nmodules;
    m->path = path;
    unsigned h = fnv(stable_path(path), 2166136261u) & 0xffffffu;
    for (bool clash = true; clash;) { /* six hex digits can clash: then the next number */
        clash = false;
        for (int i = 0; i < g_nmodules; i++) {
            char t[8];
            snprintf(t, sizeof t, "%06x", h);
            if (strcmp(g_modules[i]->tag, t) == 0) clash = true;
        }
        if (clash) h = (h + 1) & 0xffffffu;
    }
    char *tag = arena_alloc(&g_arena, 8);
    snprintf(tag, 8, "%06x", h);
    m->tag = tag;
    g_modules[g_nmodules++] = m;
    return m;
}

static Module *load_module(const char *path, int line);

static char *resolve_import(const char *importer, Str spec, int line) {
    char *s = arena_strn(&g_arena, spec.p, spec.n);
    if (strncmp(s, "std/", 4) == 0) {
        if (!g_std_dir)
            die(line, "cannot find the standard library: tell nogec where it is with --std-dir=DIR or the NOGE_STD variable");
        size_t n = strlen(g_std_dir) + strlen(s);
        char *p = arena_alloc(&g_arena, n + 2);
        snprintf(p, n + 2, "%s/%s", g_std_dir, s + 4);
        return normalize(p);
    }
    return join_dir(importer, s);
}

static Module *load_module(const char *path, int line) {
    for (int i = 0; i < g_nmodules; i++) {
        if (strcmp(g_modules[i]->path, path) != 0) continue;
        if (state[i] == 1) die(line, "modules import each other in a circle (through '%s')", path);
        return g_modules[i];
    }
    if (!file_exists(path)) die(line, "cannot open the imported file '%s'", path);
    Module *m = new_module(path);
    state[m->id] = 1;
    const char *saved = g_file;
    g_file = path;
    Node *decls = parse_program(lex(read_file(path)));
    Node *head = NULL, **tail = &head;
    Import **itail = &m->imports;
    for (Node *d = decls; d;) {
        Node *next = d->next;
        if (d->kind == N_IMPORT) {
            Module *dep = load_module(resolve_import(path, d->tyname, d->line), d->line);
            g_file = path;
            for (Import *i = m->imports; i; i = i->next)
                if (d->name.n && str_eq(i->alias, d->name))
                    die(d->line, "'%.*s' names two different imports", SF(d->name));
            Import *imp = ARENA_NEW(&g_arena, Import);
            imp->alias = d->name;
            imp->mod = dep;
            imp->line = d->line;
            *itail = imp;
            itail = &imp->next;
        } else {
            d->mod = m->id;
            *tail = d;
            tail = &d->next;
        }
        d = next;
    }
    *tail = NULL;
    m->decls = head;
    g_file = saved;
    state[m->id] = 2;
    return m;
}

Node *load_program(const char *path, bool use_prelude) {
    Module *root = load_module(normalize(path), 0);
    if (use_prelude && g_std_dir) {
        char *pp = arena_alloc(&g_arena, strlen(g_std_dir) + 16);
        sprintf(pp, "%s/prelude.nge", g_std_dir);
        pp = normalize(pp);
        if (file_exists(pp)) {
            Module *pre = load_module(pp, 0);
            int n = g_nmodules; /* modules the prelude itself loaded count too, but import nothing implicitly */
            for (int i = 0; i < n; i++) {
                if (g_modules[i] == pre) continue;
                Import *imp = ARENA_NEW(&g_arena, Import);
                imp->mod = pre;
                imp->prelude = true;
                Import **t = &g_modules[i]->imports;
                while (*t) t = &(*t)->next;
                *t = imp;
            }
        }
    }
    (void)root;
    Node *head = NULL, **tail = &head;
    for (int i = 0; i < g_nmodules; i++) {
        for (Node *d = g_modules[i]->decls; d; d = d->next) {
            *tail = d;
            tail = &d->next;
        }
    }
    *tail = NULL;
    return head;
}
