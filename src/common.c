/* Quark — buffers, memory, files and error messages. */
#include "quark.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/* fopen with a UTF-8 path, on Windows too */
static FILE* openUtf8(const char* path, const char* mode) {
#ifdef _WIN32
    wchar_t wpath[4096], wmode[8];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 4096)) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 8);
    return _wfopen(wpath, wmode);
#else
    return fopen(path, mode);
#endif
}

void qkBufAdd(QkBuf* b, const void* p, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1) cap *= 2;
        char* grown = realloc(b->data, cap);
        if (!grown) {
            fputs("quark: out of memory\n", stderr);
            exit(2);
        }
        b->data = grown;
        b->cap = cap;
    }
    if (n) memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void qkBufPuts(QkBuf* b, const char* s) { qkBufAdd(b, s, strlen(s)); }

void qkBufByte(QkBuf* b, uint8_t v) { qkBufAdd(b, &v, 1); }

void qkBufPrintf(QkBuf* b, const char* fmt, ...) {
    char small[512];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(small, sizeof small, fmt, args);
    va_end(args);
    if (n < (int)sizeof small) {
        qkBufAdd(b, small, (size_t)n);
        return;
    }
    char* big = malloc((size_t)n + 1);
    va_start(args, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, args);
    va_end(args);
    qkBufAdd(b, big, (size_t)n);
    free(big);
}

void qkBufFree(QkBuf* b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

/* A bump allocator: the compiler runs once and exits. */
static char* chunk;
static size_t chunkLeft;

void* qkNew(size_t bytes) {
    bytes = (bytes + 15) & ~(size_t)15;
    if (bytes > chunkLeft) {
        size_t size = bytes > (1u << 20) ? bytes : (1u << 20);
        chunk = calloc(1, size);
        if (!chunk) {
            fputs("quark: out of memory\n", stderr);
            exit(2);
        }
        chunkLeft = size;
    }
    void* p = chunk;
    chunk += bytes;
    chunkLeft -= bytes;
    return p;
}

char* qkDup(const char* s, size_t n) {
    char* d = qkNew(n + 1);
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

char* qkReadFile(const char* path, size_t* len) {
    FILE* f = openUtf8(path, "rb");
    if (!f) return NULL;
    QkBuf b = {0};
    char block[65536];
    size_t n;
    while ((n = fread(block, 1, sizeof block, f)) > 0) qkBufAdd(&b, block, n);
    fclose(f);
    if (!b.data) qkBufAdd(&b, "", 0);
    *len = b.len;
    return b.data;
}

bool qkWriteFile(const char* path, const void* data, size_t len) {
    for (int attempt = 0; attempt < 2; attempt++) { /* a synced folder may hold the file for a moment */
        FILE* f = openUtf8(path, "wb");
        if (!f) continue;
        bool ok = fwrite(data, 1, len, f) == len;
        ok = fclose(f) == 0 && ok;
        if (ok) return true;
    }
    return false;
}

int qkErrorCount;

void qkError(const QkSource* src, int line, int col, const char* fmt, ...) {
    qkErrorCount++;
    fprintf(stderr, "%s:%d:%d: ", src && src->path ? src->path : "quark", line, col);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    if (!src || !src->text || line <= 0) return;
    /* the line itself, and a caret under the column */
    const char* p = src->text;
    const char* end = src->text + src->len;
    for (int l = 1; l < line && p < end; p++)
        if (*p == '\n') l++;
    const char* stop = p;
    while (stop < end && *stop != '\n' && *stop != '\r') stop++;
    fprintf(stderr, "  %.*s\n  ", (int)(stop - p), p);
    for (int i = 1; i < col && p + i - 1 < stop; i++) fputc(p[i - 1] == '\t' ? '\t' : ' ', stderr);
    fputs("^\n", stderr);
}

static int editDistance(const char* a, const char* b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 40 || lb > 40) return 99;
    int row[41];
    for (int j = 0; j <= lb; j++) row[j] = j;
    for (int i = 1; i <= la; i++) {
        int diag = row[0];
        row[0] = i;
        for (int j = 1; j <= lb; j++) {
            int up = row[j];
            int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            int best = diag + cost;
            if (row[j] + 1 < best) best = row[j] + 1;
            if (row[j - 1] + 1 < best) best = row[j - 1] + 1;
            row[j] = best;
            diag = up;
        }
    }
    return row[lb];
}

/* The closest name, for "did you mean ...?" — or NULL when nothing is close. */
const char* qkClosest(const char* name, const char* const* candidates, int count) {
    const char* best = NULL;
    int bestDistance = (int)strlen(name) / 3 + 1;
    for (int i = 0; i < count; i++) {
        if (!candidates[i]) continue;
        int d = editDistance(name, candidates[i]);
        if (d < bestDistance || (d == bestDistance && !best)) {
            if (d <= bestDistance) {
                best = candidates[i];
                bestDistance = d;
            }
        }
    }
    return best;
}
