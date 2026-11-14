/* Quark — modules: `use geometry` in a file brings in the pub declarations of geometry.qk, the file next to it
 * (`use shapes.round`: shapes/round.qk), or of a package quark.toml names (`use geom.shapes`: package.c).
 * Every file of a program is parsed into the one Program, each declaration marked with the file (module) it
 * comes from; the checker lets a file see its own declarations, the host's, and the pub ones of the files it
 * uses (not the files those use). */
#include "qk.h"

#include <stdio.h>

extern const char* const qkStdNames[];
extern const char* const qkStdTexts[];
/* package.c: `use geom.shapes` with geom in quark.toml is .quark/pkg/geom/shapes.qk (NULL: not a package) */
char* qkPackageFile(const char* from, const char* name, const char** error);

/* std.json, std.math: the standard library, written in Quark, kept in the compiler (build.sh puts std/ there) */
static char* stdModule(const char* path, size_t* len) {
    const char* at = strstr(path, "std/");
    if (!at || at[4] == 0) return NULL;
    for (int i = 0; qkStdNames[i]; i++) {
        size_t n = strlen(qkStdNames[i]);
        if (!strncmp(at + 4, qkStdNames[i], n) && !strcmp(at + 4 + n, ".qk")) {
            *len = strlen(qkStdTexts[i]);
            return qkDup(qkStdTexts[i], *len);
        }
    }
    return NULL;
}

static char* readFromDisk(const char* path, size_t* len) { return qkReadFile(path, len); }
char* (*qkModuleReader)(const char* path, size_t* len) = readFromDisk;

/* dir of `from` + name with its dots as folders + ".qk" */
static char* modulePath(const char* from, const char* name) {
    QkBuf b = {0};
    const char* slash = NULL;
    for (const char* q = from; *q; q++)
        if (*q == '/' || *q == '\\') slash = q;
    if (slash) qkBufAdd(&b, from, (size_t)(slash - from + 1));
    for (const char* q = name; *q; q++) qkBufByte(&b, *q == '.' ? '/' : (uint8_t)*q);
    qkBufPuts(&b, ".qk");
    char* path = qkDup(b.data, b.len);
    qkBufFree(&b);
    return path;
}

static int addModule(Program* p, const char* path, QkSource* src) {
    QkFile m = {path, src, NULL, NULL, 0, 0, 0};
    QK_PUSH(p->modules, p->moduleCount, p->moduleCap, m);
    return p->moduleCount - 1;
}

bool qkLoadUses(Program* p, const char* path) {
    int errorsBefore = qkErrorCount;
    if (!p->moduleCount) addModule(p, path, &p->src);
    for (int m = 0; m < p->moduleCount; m++) { /* the list grows as files name more files */
        for (int i = 0; i < p->count; i++) {
            Decl* d = p->decls[i];
            if (d->kind != D_USE || d->module != m) continue;
            const QkSource* from = d->src ? d->src : &p->src;
            bool std = !strncmp(d->name, "std.", 4);
            const char* problem = NULL;
            char* file = std ? modulePath("", d->name) /* std.json: "std/json.qk" */
                             : qkPackageFile(p->modules[m].path, d->name, &problem); /* geom.shapes: .quark/pkg/geom/shapes.qk */
            if (problem) {
                qkError(from, d->line, d->col, "use %s: %s", d->name, problem);
                continue;
            }
            if (!file) file = modulePath(p->modules[m].path, d->name);
            int found = -1;
            for (int k = 0; k < p->moduleCount && found < 0; k++)
                if (!strcmp(p->modules[k].path, file)) found = k;
            if (found == m) {
                qkError(from, d->line, d->col, "a file cannot use itself");
                continue;
            }
            if (found < 0) {
                size_t len = 0;
                char* text = std ? stdModule(file, &len) : qkModuleReader ? qkModuleReader(file, &len) : NULL;
                if (!text) {
                    qkError(from, d->line, d->col, "use %s: cannot read %s", d->name, file);
                    continue;
                }
                QkSource* src = qkNew(sizeof *src);
                *src = (QkSource){file, text, len};
                Program sub;
                if (!qkParse(src, &sub)) continue;
                found = addModule(p, file, src);
                for (int k = 0; k < sub.count; k++) {
                    Decl* x = sub.decls[k];
                    x->module = found;
                    x->src = src;
                    x->index = p->count;
                    QK_PUSH(p->decls, p->count, p->cap, x);
                }
            }
            bool already = false;
            for (int k = 0; k < p->modules[m].useCount; k++) already |= p->modules[m].uses[k] == found;
            if (already) qkError(from, d->line, d->col, "%s is used twice", d->name);
            else {
                QkFile* f = &p->modules[m];
                if (f->useCount == f->aliasCap) {
                    int cap = f->aliasCap ? f->aliasCap * 2 : 8;
                    const char** grown = qkNew(sizeof(char*) * (size_t)cap);
                    if (f->useCount) memcpy(grown, f->aliases, sizeof(char*) * (size_t)f->useCount);
                    f->aliases = grown;
                    f->aliasCap = cap;
                }
                f->aliases[f->useCount] = d->alias;
                QK_PUSH(f->uses, f->useCount, f->useCap, found);
            }
        }
    }
    return qkErrorCount == errorsBefore;
}
