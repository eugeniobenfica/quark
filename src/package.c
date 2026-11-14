/* Quark — packages: code from other people's git repositories.
 *
 * quark.toml, at a project's root, names the project and what it depends on, each by git URL and a pinned
 * tag or commit:
 *
 *     [package]
 *     name = "app"
 *     version = "0.1.0"
 *
 *     [dependencies]
 *     geom = { git = "https://example.org/geom.git", tag = "v1.2.0" }
 *     util = { git = "../util", rev = "3f2a9c1" }
 *
 * `quark get` fetches every package (and the packages they name, in their own quark.toml) with the system
 * git into .quark/pkg/<name>/, one folder per name for the whole project, and writes quark.lock: the exact
 * commit and a SHA-256 of the files of each. Then `use geom.shapes` is .quark/pkg/geom/shapes.qk (and `use
 * geom` is .quark/pkg/geom/geom.qk) with no network: a build checks each package it uses against the lock.
 * quark.lock wins over a moved tag: once locked, the same code comes back until quark.toml asks for another.
 *
 * The manifest is TOML (the subset below: tables, arrays of tables, strings, inline tables of strings), not
 * Quark: it is data, read before any code is, by quark get and by tools that know nothing of Quark. */
#define _DEFAULT_SOURCE
#include "qk.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* ---- small things --------------------------------------------------------------------------------- */

/* QK_PUSH grows in the compile's arena; what is here outlives a compile (the language server's), so: */
#define PUSH(items, count, cap, value)                                                      \
    do {                                                                                    \
        if ((count) == (cap)) {                                                             \
            (cap) = (cap) ? (cap) * 2 : 8;                                                  \
            (items) = realloc((items), sizeof(*(items)) * (size_t)(cap));                   \
        }                                                                                   \
        (items)[(count)++] = (value);                                                       \
    } while (0)

static char* dupN(const char* s, size_t n) {
    char* d = malloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}
static char* dup(const char* s) { return s ? dupN(s, strlen(s)) : NULL; }
static bool same(const char* a, const char* b) { return (!a && !b) || (a && b && !strcmp(a, b)); }
static bool isSep(char c) { return c == '/' || c == '\\'; }

static char* joinf(const char* fmt, ...) { /* a new string, printf's way */
    char small[1024];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(small, sizeof small, fmt, args);
    va_end(args);
    if (n < (int)sizeof small) return dupN(small, (size_t)n);
    char* big = malloc((size_t)n + 1);
    va_start(args, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, args);
    va_end(args);
    return big;
}

static bool exists(const char* path) {
#ifdef _WIN32
    wchar_t w[4096];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 4096)) return false;
    return GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

/* ---- SHA-256 -------------------------------------------------------------------------------------- */

typedef struct Sha256 {
    uint32_t h[8];
    uint8_t block[64];
    uint64_t total;
    size_t used;
} Sha256;

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void shaBlock(Sha256* s, const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d, s->h[4] += e, s->h[5] += f, s->h[6] += g, s->h[7] += h;
}

static void shaInit(Sha256* s) {
    static const uint32_t h0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, h0, sizeof h0);
    s->total = 0;
    s->used = 0;
}

static void shaAdd(Sha256* s, const void* data, size_t n) {
    const uint8_t* p = data;
    s->total += n;
    while (n) {
        size_t take = 64 - s->used < n ? 64 - s->used : n;
        memcpy(s->block + s->used, p, take);
        s->used += take, p += take, n -= take;
        if (s->used == 64) {
            shaBlock(s, s->block);
            s->used = 0;
        }
    }
}

static void shaHex(Sha256* s, char out[65]) {
    uint64_t bits = s->total * 8;
    uint8_t pad = 0x80, zero = 0;
    shaAdd(s, &pad, 1);
    while (s->used != 56) shaAdd(s, &zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    shaAdd(s, len, 8);
    for (int i = 0; i < 8; i++) snprintf(out + 8 * i, 9, "%08x", s->h[i]);
}

/* ---- a package's content hash: its files (not .git), in path order ------------------------------------ */

typedef struct Names {
    char** items;
    int count, cap;
} Names;

static void listFiles(const char* dir, const char* rel, Names* out) {
#ifdef _WIN32
    char* pattern = joinf("%s/%s*", dir, rel);
    wchar_t w[4096];
    MultiByteToWideChar(CP_UTF8, 0, pattern, -1, w, 4096);
    free(pattern);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char name[1024];
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL);
        bool isDir = fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
#else
    char* path = joinf("%s/%s", dir, rel);
    DIR* d = opendir(path);
    free(path);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d))) {
        const char* name = e->d_name;
        char* full = joinf("%s/%s%s", dir, rel, name);
        struct stat st;
        bool isDir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        free(full);
#endif
        if (!strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, ".git")) continue;
        char* sub = joinf("%s%s", rel, name);
        if (isDir) {
            char* subDir = joinf("%s/", sub);
            listFiles(dir, subDir, out);
            free(subDir);
            free(sub);
        } else
            PUSH(out->items, out->count, out->cap, sub);
#ifdef _WIN32
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    }
    closedir(d);
#endif
}

static int byName(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }

/* "sha256-<hex>" over each file's path, a 0, its length (8 bytes, little end first) and its bytes; NULL: no files */
static char* hashDir(const char* dir) {
    Names files = {0};
    listFiles(dir, "", &files);
    if (!files.count) return NULL;
    qsort(files.items, (size_t)files.count, sizeof(char*), byName);
    Sha256 s;
    shaInit(&s);
    for (int i = 0; i < files.count; i++) {
        char* full = joinf("%s/%s", dir, files.items[i]);
        size_t len = 0;
        char* text = qkReadFile(full, &len);
        free(full);
        uint8_t n[8];
        for (int k = 0; k < 8; k++) n[k] = (uint8_t)((uint64_t)len >> (8 * k));
        shaAdd(&s, files.items[i], strlen(files.items[i]) + 1);
        shaAdd(&s, n, 8);
        if (text) shaAdd(&s, text, len);
        free(text);
        free(files.items[i]);
    }
    free(files.items);
    char hex[65];
    shaHex(&s, hex);
    return joinf("sha256-%s", hex);
}

/* ---- TOML, the part a manifest and a lock need ---------------------------------------------------------- */

typedef struct Entry {
    char* table; /* "package", "dependencies"; "" before any */
    int index;   /* [[package]]: which one (0, 1, ...); -1 for a [table] */
    char* key;
    char* sub;   /* key = { sub = "value" }: the inner key; NULL for key = "value" */
    char* value;
    int line;
} Entry;

typedef struct Toml {
    Entry* items;
    int count, cap;
} Toml;

static char* tomlError; /* the first error, "file:line: message" */

static void failAt(const char* file, int line, const char* message) {
    if (!tomlError) tomlError = joinf("%s:%d: %s", file, line, message);
}

static void skipSpace(const char** p) {
    while (**p == ' ' || **p == '\t') (*p)++;
}

static char* parseKey(const char** p) {
    skipSpace(p);
    const char* start = *p;
    if (**p == '"') {
        (*p)++;
        start = *p;
        while (**p && **p != '"' && **p != '\n') (*p)++;
        if (**p != '"') return NULL;
        char* k = dupN(start, (size_t)(*p - start));
        (*p)++;
        return k;
    }
    while ((**p >= 'a' && **p <= 'z') || (**p >= 'A' && **p <= 'Z') || (**p >= '0' && **p <= '9') || **p == '_' || **p == '-') (*p)++;
    return *p > start ? dupN(start, (size_t)(*p - start)) : NULL;
}

static char* parseString(const char** p) {
    skipSpace(p);
    if (**p != '"') return NULL;
    (*p)++;
    QkBuf b = {0};
    qkBufAdd(&b, "", 0);
    while (**p != '"') {
        char c = *(*p)++;
        if (!c || c == '\n') {
            qkBufFree(&b);
            return NULL;
        }
        if (c == '\\') {
            char e = *(*p)++;
            c = e == 'n' ? '\n' : e == 't' ? '\t' : e == '"' ? '"' : e == '\\' ? '\\' : 0;
            if (!c) {
                qkBufFree(&b);
                return NULL;
            }
        }
        qkBufByte(&b, (uint8_t)c);
    }
    (*p)++;
    return b.data;
}

static bool lineEnds(const char** p) {
    skipSpace(p);
    if (**p == '#')
        while (**p && **p != '\n') (*p)++;
    if (**p == '\r') (*p)++;
    return **p == '\n' || **p == 0;
}

/* NULL (and tomlError) if it is not TOML of this kind */
static Toml* parseToml(const char* file, const char* text) {
    Toml* t = calloc(1, sizeof *t);
    char* table = dup("");
    int index = -1, line = 1;
    Names arrays = {0}; /* how many of each [[name]] so far */
    for (const char* p = text; *p; line++) {
        skipSpace(&p);
        if (*p == '[') {
            bool array = p[1] == '[';
            p += array ? 2 : 1;
            char* name = parseKey(&p);
            if (!name || *p != ']' || (array && p[1] != ']')) {
                failAt(file, line, "expected [table] or [[table]]");
                free(name);
                return NULL;
            }
            p += array ? 2 : 1;
            free(table);
            table = name;
            index = -1;
            if (array) {
                index = 0;
                for (int i = 0; i < arrays.count; i++)
                    if (!strcmp(arrays.items[i], name)) index++;
                PUSH(arrays.items, arrays.count, arrays.cap, dup(name));
            }
        } else if (*p != '\n' && *p != '\r' && *p != '#' && *p) {
            char* key = parseKey(&p);
            skipSpace(&p);
            if (!key || *p != '=') {
                failAt(file, line, "expected key = value");
                return NULL;
            }
            p++;
            skipSpace(&p);
            if (*p == '{') { /* key = { a = "x", b = "y" } */
                p++;
                bool any = false;
                for (;;) {
                    skipSpace(&p);
                    if (*p == '}' && !any) break;
                    char* sub = parseKey(&p);
                    skipSpace(&p);
                    if (!sub || *p != '=') {
                        failAt(file, line, "expected { key = \"value\", ... }");
                        return NULL;
                    }
                    p++;
                    char* value = parseString(&p);
                    if (!value) {
                        failAt(file, line, "expected a string in quotes");
                        return NULL;
                    }
                    Entry e = {dup(table), index, dup(key), sub, value, line};
                    PUSH(t->items, t->count, t->cap, e);
                    any = true;
                    skipSpace(&p);
                    if (*p == ',') p++;
                    else if (*p == '}') break;
                    else {
                        failAt(file, line, "expected , or } in the { }");
                        return NULL;
                    }
                }
                p++;
            } else {
                char* value = parseString(&p);
                if (!value) {
                    failAt(file, line, "expected a string in quotes");
                    return NULL;
                }
                Entry e = {dup(table), index, key, NULL, value, line};
                PUSH(t->items, t->count, t->cap, e);
            }
        }
        if (!lineEnds(&p)) {
            failAt(file, line, "expected the end of the line");
            return NULL;
        }
        if (*p) p++;
    }
    return t;
}

/* ---- the manifest and the lock -------------------------------------------------------------------------- */

typedef struct Dep {
    char* name;
    char* git;
    char* tag; /* one of tag and rev */
    char* rev;
    char* commit; /* the lock's, and what quark get found */
    char* hash;
    char* by; /* quark get: the package that asked for it */
} Dep;

typedef struct Manifest {
    char* name;
    char* version;
    Dep* deps;
    int count, cap;
} Manifest;

/* a name `use` can say: lowercase letters, digits and _, not std */
static bool goodName(const char* s) {
    if (!s || !((*s >= 'a' && *s <= 'z') || *s == '_')) return false;
    for (const char* q = s; *q; q++)
        if (!((*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9') || *q == '_')) return false;
    return strcmp(s, "std") != 0;
}

static Dep* findDep(Dep* deps, int count, const char* name) {
    for (int i = 0; i < count; i++)
        if (!strcmp(deps[i].name, name)) return &deps[i];
    return NULL;
}

/* what tag/rev a package is pinned to, for messages */
static const char* pinOf(const Dep* d) { return d->tag ? d->tag : d->rev; }

/* NULL (and tomlError) if it is wrong; path names it in messages */
static Manifest* parseManifest(const char* path, const char* text) {
    Toml* t = parseToml(path, text);
    if (!t) return NULL;
    Manifest* m = calloc(1, sizeof *m);
    for (int i = 0; i < t->count; i++) {
        Entry* e = &t->items[i];
        if (!strcmp(e->table, "package") && e->index < 0 && !e->sub) {
            if (!strcmp(e->key, "name")) m->name = e->value;
            else if (!strcmp(e->key, "version")) m->version = e->value;
            else failAt(path, e->line, "[package] has name and version");
        } else if (!strcmp(e->table, "dependencies") && e->index < 0) {
            if (!e->sub) {
                failAt(path, e->line, "a dependency is name = { git = \"url\", tag = \"v1.0.0\" } (or rev = \"commit\")");
                continue;
            }
            if (!goodName(e->key)) {
                failAt(path, e->line, "a package's name is lowercase letters, digits and _ (and not std): what use says");
                continue;
            }
            Dep* d = findDep(m->deps, m->count, e->key);
            if (!d) {
                Dep fresh = {e->key};
                PUSH(m->deps, m->count, m->cap, fresh);
                d = &m->deps[m->count - 1];
            }
            char** slot = !strcmp(e->sub, "git") ? &d->git : !strcmp(e->sub, "tag") ? &d->tag : !strcmp(e->sub, "rev") ? &d->rev : NULL;
            if (!slot) failAt(path, e->line, "a dependency has git, and tag or rev");
            else if (*slot) failAt(path, e->line, "said twice");
            else *slot = e->value;
        } else
            failAt(path, e->line, "quark.toml has [package] (name, version) and [dependencies]");
    }
    if (!tomlError && !goodName(m->name)) failAt(path, 1, "[package] needs name = \"...\": lowercase letters, digits and _");
    if (!tomlError && !m->version) failAt(path, 1, "[package] needs version = \"...\"");
    for (int i = 0; i < m->count && !tomlError; i++) {
        Dep* d = &m->deps[i];
        if (!d->git) tomlError = joinf("%s: %s needs git = \"url\"", path, d->name);
        else if (!d->tag == !d->rev) tomlError = joinf("%s: %s needs one of tag = \"...\" and rev = \"commit\"", path, d->name);
        else if (strchr(d->git, '"') || strchr(pinOf(d), '"') || pinOf(d)[0] == '-' || d->git[0] == '-')
            tomlError = joinf("%s: %s: not a git URL and pin", path, d->name);
    }
    return tomlError ? NULL : m;
}

/* NULL with tomlError set if it is wrong; NULL with no error if there is no such file */
static Manifest* readManifest(const char* path) {
    size_t len;
    char* text = qkReadFile(path, &len);
    if (!text) return NULL;
    Manifest* m = parseManifest(path, text);
    free(text);
    return m;
}

/* the lock: its packages; empty with no error if there is none */
static Dep* readLock(const char* path, int* count) {
    *count = 0;
    size_t len;
    char* text = qkReadFile(path, &len);
    if (!text) return NULL;
    Toml* t = parseToml(path, text);
    free(text);
    if (!t) return NULL;
    Dep* deps = NULL;
    int cap = 0;
    for (int i = 0; i < t->count; i++) {
        Entry* e = &t->items[i];
        if (strcmp(e->table, "package") || e->index < 0 || e->sub) {
            failAt(path, e->line, "quark.lock has [[package]] entries (it is written by quark get)");
            return NULL;
        }
        while (*count <= e->index) {
            Dep fresh = {0};
            PUSH(deps, *count, cap, fresh);
        }
        Dep* d = &deps[e->index];
        char** slot = !strcmp(e->key, "name") ? &d->name : !strcmp(e->key, "git") ? &d->git : !strcmp(e->key, "tag") ? &d->tag
                    : !strcmp(e->key, "rev") ? &d->rev : !strcmp(e->key, "commit") ? &d->commit : !strcmp(e->key, "hash") ? &d->hash : NULL;
        if (!slot) failAt(path, e->line, "a locked package has name, git, tag or rev, commit and hash");
        else *slot = e->value;
    }
    for (int i = 0; i < *count && !tomlError; i++)
        if (!deps[i].name || !deps[i].git || !deps[i].commit || !deps[i].hash) failAt(path, 1, "a locked package lacks name, git, commit or hash");
    return tomlError ? NULL : deps;
}

static void addQuoted(QkBuf* b, const char* key, const char* value) {
    qkBufPrintf(b, "%s = \"", key);
    for (const char* q = value; *q; q++) {
        if (*q == '"' || *q == '\\') qkBufByte(b, '\\');
        qkBufByte(b, (uint8_t)*q);
    }
    qkBufPuts(b, "\"\n");
}

static int byDepName(const void* a, const void* b) { return strcmp(((const Dep*)a)->name, ((const Dep*)b)->name); }

static bool writeLock(const char* path, Dep* deps, int count) {
    qsort(deps, (size_t)count, sizeof *deps, byDepName);
    QkBuf b = {0};
    qkBufPuts(&b, "# The exact code of every package, written by quark get: commit it with quark.toml.\n");
    for (int i = 0; i < count; i++) {
        qkBufPuts(&b, "\n[[package]]\n");
        addQuoted(&b, "name", deps[i].name);
        addQuoted(&b, "git", deps[i].git);
        addQuoted(&b, deps[i].tag ? "tag" : "rev", pinOf(&deps[i]));
        addQuoted(&b, "commit", deps[i].commit);
        addQuoted(&b, "hash", deps[i].hash);
    }
    bool ok = qkWriteFile(path, b.data, b.len);
    qkBufFree(&b);
    return ok;
}

/* ---- git ----------------------------------------------------------------------------------------------- */

/* runs `git <args>` (args quoted already); its output, without the last line end, in *out (if out); true if
 * it succeeded. git's own errors go to stderr: they say what went wrong with the network or the repository. */
static bool git(char** out, const char* fmt, ...) {
    char args[8192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(args, sizeof args, fmt, ap);
    va_end(ap);
    char* cmd = joinf("git -c advice.detachedHead=false %s", args);
    fflush(stdout);
#ifdef _WIN32
    wchar_t w[8192], mode[4] = L"rb";
    MultiByteToWideChar(CP_UTF8, 0, cmd, -1, w, 8192);
    FILE* f = _wpopen(w, mode);
#else
    FILE* f = popen(cmd, "r");
#endif
    free(cmd);
    if (!f) return false;
    QkBuf b = {0};
    qkBufAdd(&b, "", 0);
    char block[4096];
    size_t n;
    while ((n = fread(block, 1, sizeof block, f)) > 0) qkBufAdd(&b, block, n);
#ifdef _WIN32
    int status = _pclose(f);
#else
    int status = pclose(f);
#endif
    while (b.len && (b.data[b.len - 1] == '\n' || b.data[b.len - 1] == '\r')) b.data[--b.len] = 0;
    if (out) *out = b.data;
    else qkBufFree(&b);
    return status == 0;
}

#ifdef _WIN32
#define NOWHERE "NUL"
#else
#define NOWHERE "/dev/null"
#endif

/* ---- quark get ----------------------------------------------------------------------------------------- */

typedef struct Get {
    const char* root; /* the project, "." or "dir" */
    Dep* lock;        /* quark.lock as it was */
    int lockCount;
    Dep* got; /* what this run found */
    int count, cap;
    const char* stack[64]; /* the packages being visited: a cycle comes back to one */
    int depth;
} Get;

static bool getError(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("quark get: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    return false;
}

/* the lock's entry for d, if quark.toml still asks for what it was locked for */
static Dep* lockedFor(Get* g, const Dep* d) {
    Dep* locked = findDep(g->lock, g->lockCount, d->name);
    return locked && same(locked->git, d->git) && same(locked->tag, d->tag) && same(locked->rev, d->rev) ? locked : NULL;
}

/* d's commit (the locked one if quark.toml still asks for the same), in the package's own clone, which is
 * made (or fetched into) only when the commit is not there yet. Its files are not touched: see place. */
static bool resolve(Get* g, Dep* d) {
    char* dir = joinf("%s/.quark/pkg/%s", g->root, d->name);
    char* gitDir = joinf("%s/.git", dir);
    Dep* locked = lockedFor(g, d);
    char* want = locked ? joinf("%s^{commit}", locked->commit) : d->tag ? joinf("refs/tags/%s^{commit}", d->tag) : joinf("%s^{commit}", d->rev);
    char* commit = NULL;
    bool ok = false;
    /* line ends exactly as committed: the same files, so the same hash, on every system */
    if (!exists(gitDir) &&
        !git(NULL, "-C \"%s\" clone --quiet --no-checkout -c core.autocrlf=false -c core.eol=lf -- \"%s\" \".quark/pkg/%s\"", g->root, d->git, d->name))
        getError("%s: cannot clone %s", d->name, d->git);
    else if (git(&commit, "-C \"%s\" rev-parse --verify --quiet \"%s\"", dir, want))
        ok = true;
    else { /* not here yet: ask the remote */
        free(commit);
        git(NULL, "-C \"%s\" fetch --quiet --tags origin", dir);
        ok = git(&commit, "-C \"%s\" rev-parse --verify --quiet \"%s\"", dir, want);
        if (!ok && locked) getError("%s: the locked commit %s is not in %s", d->name, locked->commit, d->git);
        else if (!ok) getError("%s: no %s %s in %s", d->name, d->tag ? "tag" : "commit", pinOf(d), d->git);
    }
    if (ok) d->commit = commit;
    else free(commit);
    free(want), free(dir), free(gitDir);
    return ok;
}

/* the package's folder holding the files of its commit, their hash the lock's (if it is locked) */
static bool place(Get* g, Dep* d) {
    char* dir = joinf("%s/.quark/pkg/%s", g->root, d->name);
    Dep* locked = lockedFor(g, d);
    char *head = NULL, *hash = NULL;
    bool ok = false;
    /* locked, checked out and unchanged: nothing to do. Otherwise check it out afresh. */
    if (!locked || !git(&head, "-C \"%s\" rev-parse --verify --quiet HEAD", dir) || strcmp(head, d->commit) || !(hash = hashDir(dir)) ||
        strcmp(hash, locked->hash)) {
        free(hash);
        hash = NULL;
        if (!git(NULL, "-C \"%s\" checkout --quiet --force --detach %s", dir, d->commit) || !git(NULL, "-C \"%s\" clean --quiet -ffdx", dir)) {
            getError("%s: cannot check out %s", d->name, d->commit);
            goto done;
        }
        hash = hashDir(dir);
        if (!hash) hash = dup("sha256-"); /* a package of no files */
    }
    if (locked && strcmp(hash, locked->hash)) {
        getError("%s: hash mismatch: quark.lock has %s, but %s at %.12s hashes to %s", d->name, locked->hash, d->git, d->commit, hash);
        goto done;
    }
    d->hash = hash;
    hash = NULL;
    ok = true;
done:
    free(head), free(hash), free(dir);
    return ok;
}

/* the packages m names, and the ones they name, each resolved once; nothing is checked out yet, so a
 * conflict or a cycle found late leaves every package's files as they were */
static bool visit(Get* g, Manifest* m) {
    for (int i = 0; i < m->count; i++) {
        Dep want = m->deps[i];
        want.by = m->name;
        for (int k = 0; k < g->depth; k++)
            if (!strcmp(g->stack[k], want.name)) {
                QkBuf b = {0};
                for (int j = k; j < g->depth; j++) qkBufPrintf(&b, "%s -> ", g->stack[j]);
                qkBufPuts(&b, want.name);
                getError("a cycle of packages: %s", b.data);
                qkBufFree(&b);
                return false;
            }
        Dep* had = findDep(g->got, g->count, want.name);
        if (had) {
            if (!same(had->git, want.git) || !same(had->tag, want.tag) || !same(had->rev, want.rev))
                return getError("%s is asked for twice: %s %s by %s, %s %s by %s", want.name, had->git, pinOf(had), had->by, want.git,
                                pinOf(&want), want.by);
            continue;
        }
        if (g->depth == 64) return getError("packages nested too deep");
        if (!resolve(g, &want)) return false;
        PUSH(g->got, g->count, g->cap, want);
        char* text = NULL; /* its own quark.toml, at its commit */
        Manifest* sub = NULL;
        if (git(&text, "-C \"%s/.quark/pkg/%s\" show \"%s:quark.toml\" 2>" NOWHERE, g->root, want.name, want.commit)) {
            char* path = joinf("%s/quark.toml", want.name);
            sub = parseManifest(path, text);
            free(path);
            if (!sub) return getError("%s", tomlError);
        }
        free(text);
        if (sub) {
            if (strcmp(sub->name, want.name)) return getError("%s's quark.toml calls it %s: depend on it by its own name", want.name, sub->name);
            g->stack[g->depth++] = want.name;
            bool ok = visit(g, sub);
            g->depth--;
            if (!ok) return false;
        }
    }
    return true;
}

int qkGet(const char* root) {
#ifdef _WIN32
    _putenv("GIT_TERMINAL_PROMPT=0"); /* a failed fetch fails; it never waits for a password */
#else
    setenv("GIT_TERMINAL_PROMPT", "0", 1);
#endif
    tomlError = NULL;
    char* path = joinf("%s/quark.toml", root);
    Manifest* m = readManifest(path);
    if (!m) {
        getError("%s", tomlError ? tomlError : "no quark.toml here: a project names its packages there");
        return 1;
    }
    Get g = {root};
    char* lockPath = joinf("%s/quark.lock", root);
    g.lock = readLock(lockPath, &g.lockCount);
    if (tomlError) {
        getError("%s", tomlError);
        return 1;
    }
    g.stack[g.depth++] = m->name;
    if (!visit(&g, m)) return 1;
    for (int i = 0; i < g.count; i++)
        if (!place(&g, &g.got[i])) return 1;
    char* ignore = joinf("%s/.quark/.gitignore", root); /* fetched code stays out of the project's own repository */
    if (g.count && !exists(ignore)) qkWriteFile(ignore, "*\n", 2);
    free(ignore);
    if (!writeLock(lockPath, g.got, g.count)) {
        getError("cannot write %s", lockPath);
        return 1;
    }
    for (int i = 0; i < g.count; i++) printf("%-16s %-12s %.12s\n", g.got[i].name, pinOf(&g.got[i]), g.got[i].commit);
    printf("%s %s: %d package%s locked\n", m->name, m->version, g.count, g.count == 1 ? "" : "s");
    return 0;
}

/* ---- use pkg.module ---------------------------------------------------------------------------------- */

/* The project and package a file belongs to: a file in <root>.quark/pkg/<name>/ is in that package of that
 * project; any other is in the project whose quark.toml is in its folder or the nearest one above. Both end
 * in a separator (or are "" for the current folder) and are kept as the path names them, so the same file
 * is the same text from every file that uses it. */
typedef struct Place {
    char* dir; /* the file's folder: the key */
    char* root;
    char* pkg;
    Manifest* manifest; /* NULL: no packages here */
    char* error;
} Place;

static Place* places;
static int placeCount, placeCap;

typedef struct Checked {
    char* key; /* the folder, and what is asked of it */
    char* error; /* NULL: it matches quark.lock */
} Checked;
static Checked* checked;
static int checkedCount, checkedCap;

static int depthOf(const char* dir) { /* how many folders lie above dir: whether "../" goes anywhere */
#ifdef _WIN32
    wchar_t w[4096], full[4096];
    MultiByteToWideChar(CP_UTF8, 0, *dir ? dir : ".", -1, w, 4096);
    if (!_wfullpath(full, w, 4096)) return 0;
    int n = 0;
    for (wchar_t* q = full; *q; q++) n += (*q == '\\' || *q == '/') && q[1];
    return n;
#else
    char* full = realpath(*dir ? dir : ".", NULL);
    if (!full) return 0;
    int n = 0;
    for (char* q = full; *q; q++) n += *q == '/' && q[1];
    free(full);
    return n;
#endif
}

static Place* placeOf(const char* file) {
    size_t cut = 0;
    for (size_t i = 0; file[i]; i++)
        if (isSep(file[i])) cut = i + 1;
    char* dir = dupN(file, cut);
    for (int i = 0; i < placeCount; i++)
        if (!strcmp(places[i].dir, dir)) {
            free(dir);
            return &places[i];
        }
    Place pl = {dir};
    tomlError = NULL;
    /* inside .quark/pkg/<name>/ */
    for (size_t i = 0; dir[i]; i++) {
        if ((i == 0 || isSep(dir[i - 1])) && !strncmp(dir + i, ".quark", 6) && isSep(dir[i + 6]) && !strncmp(dir + i + 7, "pkg", 3) &&
            isSep(dir[i + 10])) {
            size_t name = i + 11, end = name;
            while (dir[end] && !isSep(dir[end])) end++;
            if (!dir[end]) break;
            pl.root = dupN(dir, i);
            pl.pkg = dupN(dir, end + 1);
            char* path = joinf("%squark.toml", pl.pkg);
            pl.manifest = readManifest(path);
            free(path);
            break;
        }
    }
    if (!pl.root) { /* the nearest quark.toml above: up the path as written, then on with ../ */
        char* at = dup(dir);
        for (int step = 0; step < 256; step++) {
            char* path = joinf("%squark.toml", at);
            bool here = exists(path);
            if (here) {
                pl.manifest = readManifest(path);
                pl.root = dup(at);
                pl.pkg = dup(at);
            }
            free(path);
            if (here) break;
            size_t n = strlen(at), k = n ? n - 1 : 0;
            while (k > 0 && !isSep(at[k - 1])) k--;
            if ((n == 1 && isSep(at[0])) || (n == 3 && at[1] == ':')) break; /* "/", "C:/": the top */
            if (n == 0 || !strncmp(at + k, "../", 3) || !strncmp(at + k, "..\\", 3)) { /* "", "../": on up, while there is an up */
                if (depthOf(at) == 0) break;
                char* up = joinf("%s../", at);
                free(at);
                at = up;
            } else
                at[k] = 0; /* "a/b/" -> "a/" */
        }
        free(at);
    }
    if (tomlError) pl.error = tomlError;
    tomlError = NULL;
    PUSH(places, placeCount, placeCap, pl);
    return &places[placeCount - 1];
}

/* is the package's folder what quark.lock says, and quark.lock what the asking quark.toml says? (once each) */
static const char* checkPackage(Place* pl, const char* name, const Dep* asked, const char* dir) {
    char* key = joinf("%s %s %s %s", dir, asked->git, asked->tag ? "tag" : "rev", pinOf(asked));
    for (int i = 0; i < checkedCount; i++)
        if (!strcmp(checked[i].key, key)) {
            free(key);
            return checked[i].error;
        }
    char* error = NULL;
    char* lockPath = joinf("%squark.lock", pl->root);
    int count = 0;
    tomlError = NULL;
    Dep* lock = readLock(lockPath, &count);
    Dep* d = findDep(lock, count, name);
    if (tomlError) error = tomlError;
    else if (!d) error = joinf("%s is not in %s: run quark get", name, lockPath);
    else if (!same(d->git, asked->git) || !same(d->tag, asked->tag) || !same(d->rev, asked->rev))
        error = joinf("%squark.toml asks for %s %s, quark.lock has %s: run quark get", pl->pkg, name, pinOf(asked), pinOf(d));
    else {
        char* hash = hashDir(dir);
        if (!hash) error = joinf("the package %s is not fetched: run quark get", name);
        else if (strcmp(hash, d->hash))
            error = joinf("hash mismatch: the files in %s are not the ones quark.lock has for %s (run quark get to restore them)", dir, name);
        free(hash);
    }
    tomlError = NULL;
    free(lockPath);
    Checked c = {key, error};
    PUSH(checked, checkedCount, checkedCap, c);
    return error;
}

/* `use name` in the file `from`: the file of a package, or NULL if name is not a package's (a file next to
 * it, then); *error if it is one but cannot be used */
char* qkPackageFile(const char* from, const char* name, const char** error) {
    *error = NULL;
    Place* pl = placeOf(from);
    if (pl->error) {
        *error = pl->error;
        return NULL;
    }
    if (!pl->manifest) return NULL;
    const char* dot = strchr(name, '.');
    char* first = dot ? dupN(name, (size_t)(dot - name)) : dup(name);
    Dep* d = findDep(pl->manifest->deps, pl->manifest->count, first);
    if (!d) {
        free(first);
        return NULL;
    }
    char* dir = joinf("%s.quark/pkg/%s/", pl->root, first);
    QkBuf b = {0};
    qkBufPuts(&b, dir);
    for (const char* q = dot ? dot + 1 : name; *q; q++) qkBufByte(&b, *q == '.' ? '/' : (uint8_t)*q);
    qkBufPuts(&b, ".qk");
    char* file = qkDup(b.data, b.len);
    qkBufFree(&b);
    /* a file of the same name next to it would be hidden: say so */
    QkBuf local = {0};
    qkBufPuts(&local, pl->dir);
    for (const char* q = name; *q; q++) qkBufByte(&local, *q == '.' ? '/' : (uint8_t)*q);
    qkBufPuts(&local, ".qk");
    if (exists(local.data)) {
        char* both = joinf("%s is both the package %s and the file %s: rename the file", name, first, local.data);
        *error = qkDup(both, strlen(both));
        free(both);
    } else
        *error = checkPackage(pl, first, d, dir);
    qkBufFree(&local);
    free(first);
    free(dir);
    return *error ? NULL : file;
}
