/* Quark — documentation: the pub declarations of a program's files (the file given and the ones its uses name),
 * each with its signature and the comment above it, as Markdown or as one HTML page. A type's own functions
 * (fn Vec2.length) are listed under the type. */
#include "qk.h"

#include <stdio.h>
#include <stdlib.h>

static void fnSignature(const Decl* d, QkBuf* out) {
    qkBufPrintf(out, "%s%sfn %s", d->isExtern ? "extern " : "", d->isExtern && d->isEffect ? "effect " : "", d->name);
    int typeParams = d->impliedTypes ? 0 : d->typeParamCount;
    for (int i = 0; i < typeParams; i++) qkBufPrintf(out, "%s%s", i ? ", " : "<", d->typeParams[i]);
    qkBufPuts(out, typeParams ? ">(" : "(");
    for (int i = 0; i < d->paramCount; i++) {
        if (!i && (!d->params[0].type || d->impliedSelf)) { /* fn Vec2.length(self) */
            qkBufPuts(out, d->params[0].name);
            continue;
        }
        qkBufPrintf(out, "%s%s: ", i ? ", " : "", d->params[i].name);
        qkFormatTypeExpr(d->params[i].type, out);
        if (d->params[i].fallback) {
            qkBufPuts(out, " = ");
            qkFormatExpr(d->params[i].fallback, out, 0);
        }
    }
    qkBufPuts(out, ")");
    if (d->ret) {
        qkBufPuts(out, " -> ");
        qkFormatTypeExpr(d->ret, out);
    } else if (d->retType && d->retType->kind != TY_VOID && d->retType->kind != TY_ERROR) { /* worked out from its body */
        qkBufPuts(out, " -> ");
        qkTypeName(d->retType, out);
    }
}

/* a declaration as the formatter writes it, without its comments (and a function without its body) */
static void signature(const Decl* d, QkBuf* out) {
    qkBufPuts(out, "pub ");
    if (d->kind == D_FN) {
        fnSignature(d, out);
        return;
    }
    Decl bare = *d;
    bare.leading = bare.trailing = NULL;
    bare.isPub = false;
    bare.module = 0;
    if (d->kind == D_CONST && d->body && d->body->kind == E_LAMBDA) bare.body = NULL;
    Decl* one = &bare;
    Program p = {0};
    p.decls = &one;
    p.count = 1;
    QkBuf b = {0};
    qkFormat(&p, &b);
    while (b.len && b.data[b.len - 1] == '\n') b.len--;
    qkBufAdd(out, b.data, b.len);
    qkBufFree(&b);
}

static const char* kindWord(const Decl* d) {
    if (d->kind == D_FN) return d->owner ? "method" : "fn";
    if (d->kind == D_ENUM) return "enum";
    if (d->kind == D_CONST) return "const";
    if (d->isInterface) return "interface";
    return d->isExtern ? "extern type" : "type";
}

static void escapeHtml(QkBuf* out, const char* s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '<') qkBufPuts(out, "&lt;");
        else if (c == '>') qkBufPuts(out, "&gt;");
        else if (c == '&') qkBufPuts(out, "&amp;");
        else if (c == '"') qkBufPuts(out, "&quot;");
        else qkBufByte(out, c);
    }
}

static void html(QkBuf* out, const char* s) { escapeHtml(out, s, strlen(s)); }

/* an anchor: the name with its dot made a dash (Vec2.length -> Vec2-length) */
static void anchor(QkBuf* out, const char* name) {
    for (const char* c = name; *c; c++) qkBufByte(out, *c == '.' ? '-' : *c);
}

/* the comment above it: its lines as paragraphs (an empty line between them) */
static void comment(QkBuf* out, const char* text, bool asHtml) {
    if (!text) return;
    if (!asHtml) {
        qkBufPuts(out, text);
        qkBufPuts(out, "\n\n");
        return;
    }
    qkBufPuts(out, "<p>");
    for (const char* c = text; *c; c++) {
        if (*c == '\n' && c[1] == '\n') {
            qkBufPuts(out, "</p>\n<p>");
            c++;
        } else if (*c == '\n') {
            qkBufByte(out, ' ');
        } else {
            escapeHtml(out, c, 1);
        }
    }
    qkBufPuts(out, "</p>\n");
}

static void entry(QkBuf* out, const Decl* d, bool asHtml, int level) {
    QkBuf sig = {0};
    signature(d, &sig);
    if (asHtml) {
        qkBufPrintf(out, "<section class=\"decl\" id=\"");
        anchor(out, d->name);
        qkBufPrintf(out, "\">\n<h%d><span class=\"kind\">%s</span> ", level, kindWord(d));
        html(out, d->name);
        qkBufPrintf(out, "</h%d>\n<pre><code>", level);
        escapeHtml(out, sig.data, sig.len);
        qkBufPuts(out, "</code></pre>\n");
        comment(out, d->leading, true);
        qkBufPuts(out, "</section>\n");
    } else {
        for (int i = 0; i < level; i++) qkBufByte(out, '#');
        qkBufPrintf(out, " %s %s\n\n```quark\n", kindWord(d), d->name);
        qkBufAdd(out, sig.data, sig.len);
        qkBufPuts(out, "\n```\n\n");
        comment(out, d->leading, false);
    }
    qkBufFree(&sig);
}

static bool shown(const Decl* d) {
    return d->isPub && !d->fromPrelude && !d->templateOf && !d->lifted && !d->isTest && d->kind != D_USE;
}

static const char* const kStyle =
    "<style>\n"
    ":root { color-scheme: light dark; --fg: #1d1d1f; --bg: #fff; --muted: #6e6e73; --code: #f5f5f7; --accent: #5b4bdb; }\n"
    "@media (prefers-color-scheme: dark) { :root { --fg: #e8e8ed; --bg: #161618; --muted: #9a9aa2; --code: #232326; --accent: #a99cff; } }\n"
    "body { font: 16px/1.55 system-ui, sans-serif; color: var(--fg); background: var(--bg); max-width: 52rem; margin: 0 auto; padding: 2rem 1rem; }\n"
    "nav { font-size: 14px; } nav ul { columns: 2; padding-left: 1.2rem; } a { color: var(--accent); }\n"
    "h1 { margin-bottom: .2rem; } h2.file { border-bottom: 1px solid var(--muted); padding-bottom: .3rem; margin-top: 2.5rem; }\n"
    ".kind { color: var(--muted); font-weight: normal; font-size: .8em; margin-right: .3em; }\n"
    "pre { background: var(--code); padding: .7rem 1rem; border-radius: 6px; overflow-x: auto; }\n"
    "code { font: 14px/1.45 ui-monospace, Consolas, monospace; }\n"
    ".methods { margin-left: 1.2rem; }\n"
    "</style>\n";

void qkDoc(const Program* p, bool asHtml, QkBuf* out) {
    int files = p->moduleCount ? p->moduleCount : 1;
    const char* path0 = p->moduleCount ? p->modules[0].path : p->src.path;
    const char* base = path0; /* the title: the file's name, without its folder and .qk */
    for (const char* c = path0; *c; c++)
        if (*c == '/' || *c == '\\') base = c + 1;
    size_t n = strlen(base);
    if (n > 3 && !strcmp(base + n - 3, ".qk")) n -= 3;
    const char* title = qkDup(base, n);
    if (asHtml) {
        qkBufPuts(out, "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
                       "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>");
        html(out, title);
        qkBufPuts(out, "</title>\n");
        qkBufPuts(out, kStyle);
        qkBufPuts(out, "</head>\n<body>\n<h1>");
        html(out, title);
        qkBufPuts(out, "</h1>\n<nav><ul>\n");
        for (int i = 0; i < p->count; i++) {
            const Decl* d = p->decls[i];
            if (!shown(d) || d->owner) continue;
            qkBufPuts(out, "<li><a href=\"#");
            anchor(out, d->name);
            qkBufPuts(out, "\">");
            html(out, d->name);
            qkBufPuts(out, "</a></li>\n");
        }
        qkBufPuts(out, "</ul></nav>\n");
    } else {
        qkBufPrintf(out, "# %s\n\n", title);
    }
    for (int m = 0; m < files; m++) {
        int count = 0;
        for (int i = 0; i < p->count; i++) count += shown(p->decls[i]) && p->decls[i]->module == m;
        if (!count) continue;
        const char* path = p->moduleCount ? p->modules[m].path : p->src.path;
        if (asHtml) {
            qkBufPuts(out, "<h2 class=\"file\">");
            html(out, path);
            qkBufPuts(out, "</h2>\n");
        } else {
            qkBufPrintf(out, "## %s\n\n", path);
        }
        for (int i = 0; i < p->count; i++) {
            const Decl* d = p->decls[i];
            if (!shown(d) || d->module != m) continue;
            bool ownerHere = false; /* a type's function goes under its type, when the type is shown */
            for (int k = 0; d->owner && k < p->count; k++)
                if (p->decls[k]->kind != D_FN && shown(p->decls[k]) && !strcmp(p->decls[k]->name, d->owner)) ownerHere = true;
            if (ownerHere) continue;
            entry(out, d, asHtml, 3);
            if (d->kind == D_FN) continue;
            bool any = false;
            for (int k = 0; k < p->count; k++) {
                const Decl* f = p->decls[k];
                if (!shown(f) || !f->owner || strcmp(f->owner, d->name)) continue;
                if (asHtml && !any) qkBufPuts(out, "<div class=\"methods\">\n");
                any = true;
                entry(out, f, asHtml, 4);
            }
            if (asHtml && any) qkBufPuts(out, "</div>\n");
        }
    }
    if (asHtml) qkBufPuts(out, "</body>\n</html>\n");
}
