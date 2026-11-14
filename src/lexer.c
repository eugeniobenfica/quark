/* Quark — the lexer: text to tokens. Names are camelCase (values, functions) or PascalCase (types, enum
 * cases); a name with an underscore is an error, which keeps every program in one style. A line break ends a
 * statement after something that can end one, except inside ( ) and [ ], and before a line that starts
 * with .name (a chained call) or with else. */
#include "qk.h"

#include <stdlib.h>

const char* const qkKeywords[] = {"fn",  "let",   "var",   "type",     "enum", "if",  "else", "match", "for",  "in",
                                  "while", "return", "break", "continue", "and", "or", "not",  "true",  "false", "extern", "with", "effect",
                                  "use",   "pub",    "const", "defer"};
const int qkKeywordCount = (int)(sizeof qkKeywords / sizeof qkKeywords[0]);

const char* qkTokName(TokKind k) {
    static const char* const names[] = {
        "end of file", "end of line", "name", "type name", "number", "number", "text",
        "fn", "let", "var", "type", "enum", "if", "else", "match", "for", "in", "while", "return", "break", "continue",
        "and", "or", "not", "true", "false", "extern", "with", "effect", "use", "pub", "const", "defer",
        "(", ")", "{", "}", "[", "]", ",", ".", "..", ":", "->", "=>", "=", "+=", "-=", "*=", "/=", "+", "-", "*",
        "/", "%", "==", "!=", "<", "<=", ">", ">=", "_", "??", "?", "!", "&", "|", "^", "~", "<<", ">>"};
    return names[k];
}

static bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool isDigit(char c) { return c >= '0' && c <= '9'; }

static bool endsStatement(TokKind k) {
    switch (k) {
    case T_NAME: case T_TYPENAME: case T_INT: case T_FLOAT: case T_TEXT: case T_TRUE: case T_FALSE:
    case T_RPAREN: case T_RBRACKET: case T_RBRACE: case T_RETURN: case T_BREAK: case T_CONTINUE: case T_UNDERSCORE:
    case T_QUESTION: case T_BANG:
        return true;
    default:
        return false;
    }
}

typedef struct Lexer {
    const QkSource* src;
    const char* p;
    const char* end;
    int line;
    const char* lineStart;
    Lexed* out;
    int depth;         /* ( and [ open since the innermost { */
    int saved[256];    /* the depth outside each open { */
    int braces;
    bool tokenOnLine;  /* a token was already read on this line */
    bool blank;        /* an empty line since the last token */
} Lexer;

static void add(Lexer* lx, TokKind kind, const char* start, int len) {
    Token t = {kind, start, len, lx->line, (int)(start - lx->lineStart) + 1, lx->blank};
    QK_PUSH(lx->out->tokens, lx->out->count, lx->out->cap, t);
    lx->blank = false;
    lx->tokenOnLine = true;
}

/* After a line break: does the next line continue this statement? */
static bool continues(const Lexer* lx) {
    const char* q = lx->p;
    for (;;) {
        while (q < lx->end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++;
        if (q + 1 < lx->end && q[0] == '/' && q[1] == '/') {
            while (q < lx->end && *q != '\n') q++;
            continue;
        }
        break;
    }
    if (q + 1 < lx->end && q[0] == '.' && q[1] != '.') return true;
    if (q + 4 <= lx->end && memcmp(q, "else", 4) == 0 && (q + 4 == lx->end || !isAlpha(q[4]))) return true;
    return false;
}

void qkLex(const QkSource* src, Lexed* out) {
    memset(out, 0, sizeof *out);
    Lexer lx = {0};
    lx.src = src;
    lx.p = src->text;
    lx.end = src->text + src->len;
    lx.line = 1;
    lx.lineStart = src->text;
    lx.out = out;
    while (lx.p < lx.end) {
        char c = *lx.p;
        if (c == '\n') {
            if (lx.depth == 0 && out->count && endsStatement(out->tokens[out->count - 1].kind)) {
                lx.p++;
                bool goesOn = continues(&lx);
                lx.p--;
                if (!goesOn) add(&lx, T_NEWLINE, lx.p, 0);
            }
            if (!lx.tokenOnLine && lx.p > src->text) {
                /* an empty line (or one with only a comment, handled below) */
                const char* q = lx.lineStart;
                while (q < lx.p && (*q == ' ' || *q == '\t' || *q == '\r')) q++;
                if (q == lx.p) lx.blank = true;
            }
            lx.p++;
            lx.line++;
            lx.lineStart = lx.p;
            lx.tokenOnLine = false;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            lx.p++;
            continue;
        }
        const char* start = lx.p;
        if (c == '/' && lx.p + 1 < lx.end && lx.p[1] == '/') {
            lx.p += 2;
            if (lx.p < lx.end && *lx.p == ' ') lx.p++;
            const char* text = lx.p;
            while (lx.p < lx.end && *lx.p != '\n') lx.p++;
            const char* stop = lx.p;
            while (stop > text && (stop[-1] == '\r' || stop[-1] == ' ')) stop--;
            Comment cm = {lx.line, !lx.tokenOnLine, qkDup(text, (size_t)(stop - text))};
            QK_PUSH(out->comments, out->commentCount, out->commentCap, cm);
            continue;
        }
        if (isAlpha(c) || c == '_') {
            while (lx.p < lx.end && (isAlpha(*lx.p) || isDigit(*lx.p) || *lx.p == '_')) lx.p++;
            int len = (int)(lx.p - start);
            if (len == 1 && c == '_') {
                add(&lx, T_UNDERSCORE, start, 1);
                continue;
            }
            if (memchr(start, '_', (size_t)len)) {
                /* suggest the camelCase spelling */
                char fixed[128];
                int n = 0;
                bool upper = false;
                for (int i = 0; i < len && n < 120; i++) {
                    if (start[i] == '_') {
                        upper = n > 0;
                        continue;
                    }
                    char ch = start[i];
                    if (upper && ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
                    fixed[n++] = ch;
                    upper = false;
                }
                fixed[n] = '\0';
                qkError(src, lx.line, (int)(start - lx.lineStart) + 1,
                        "names are camelCase or PascalCase, without underscores: write \"%s\"", fixed);
            }
            TokKind kind = (c >= 'A' && c <= 'Z') ? T_TYPENAME : T_NAME;
            for (int k = 0; k < qkKeywordCount; k++)
                if ((int)strlen(qkKeywords[k]) == len && memcmp(qkKeywords[k], start, (size_t)len) == 0) kind = (TokKind)(T_FN + k);
            add(&lx, kind, start, len);
            continue;
        }
        if (isDigit(c)) {
            bool isFloat = false;
            while (lx.p < lx.end && (isDigit(*lx.p) || *lx.p == '_')) lx.p++;
            if (lx.p + 1 < lx.end && *lx.p == '.' && isDigit(lx.p[1])) {
                isFloat = true;
                lx.p++;
                while (lx.p < lx.end && (isDigit(*lx.p) || *lx.p == '_')) lx.p++;
            }
            if (lx.p < lx.end && (*lx.p == 'e' || *lx.p == 'E')) {
                const char* q = lx.p + 1;
                if (q < lx.end && (*q == '+' || *q == '-')) q++;
                if (q < lx.end && isDigit(*q)) {
                    isFloat = true;
                    lx.p = q;
                    while (lx.p < lx.end && isDigit(*lx.p)) lx.p++;
                }
            }
            if (lx.p < lx.end && (isAlpha(*lx.p))) {
                qkError(src, lx.line, (int)(lx.p - lx.lineStart) + 1, "a letter right after a number");
                while (lx.p < lx.end && isAlpha(*lx.p)) lx.p++;
            }
            add(&lx, isFloat ? T_FLOAT : T_INT, start, (int)(lx.p - start));
            continue;
        }
        if (c == '"' && lx.p + 2 < lx.end && lx.p[1] == '"' && lx.p[2] == '"') { /* """ a text on lines of its own """ */
            int line = lx.line;
            int col = (int)(start - lx.lineStart) + 1;
            lx.p += 3;
            while (lx.p + 2 < lx.end && !(lx.p[0] == '"' && lx.p[1] == '"' && lx.p[2] == '"')) {
                if (*lx.p == '\\' && lx.p + 1 < lx.end) lx.p++;
                else if (*lx.p == '\n') {
                    lx.line++;
                    lx.lineStart = lx.p + 1;
                }
                lx.p++;
            }
            if (lx.p + 2 >= lx.end) {
                qkError(src, line, col, "this text has no closing \"\"\"");
                lx.p = lx.end;
            } else {
                lx.p += 3;
            }
            add(&lx, T_TEXT, start, (int)(lx.p - start));
            lx.out->tokens[lx.out->count - 1].line = line;
            lx.out->tokens[lx.out->count - 1].col = col;
            continue;
        }
        if (c == '"') { /* text, with {expressions} inside (which may hold texts of their own) */
            lx.p++;
            int depth = 0;
            while (lx.p < lx.end && *lx.p != '\n' && (depth > 0 || *lx.p != '"')) {
                char k = *lx.p;
                if (k == '\\' && lx.p + 1 < lx.end) {
                    lx.p += 2;
                    continue;
                }
                if (k == '{') depth++;
                else if (k == '}' && depth > 0) depth--;
                else if (k == '"') { /* a text inside an expression */
                    lx.p++;
                    while (lx.p < lx.end && *lx.p != '"' && *lx.p != '\n') {
                        if (*lx.p == '\\' && lx.p + 1 < lx.end) lx.p++;
                        lx.p++;
                    }
                }
                lx.p++;
            }
            if (lx.p >= lx.end || *lx.p != '"') {
                qkError(src, lx.line, (int)(start - lx.lineStart) + 1, "this text has no closing \"");
            } else {
                lx.p++;
            }
            add(&lx, T_TEXT, start, (int)(lx.p - start));
            continue;
        }
        /* punctuation: the longest first */
        static const struct { const char* s; TokKind k; } punct[] = {
            {"??", T_QQ}, {"?", T_QUESTION}, {"..", T_DOTDOT}, {"->", T_ARROW}, {"=>", T_FATARROW}, {"==", T_EQ}, {"!=", T_NE}, {"<=", T_LE}, {">=", T_GE},
            {"+=", T_PLUSEQ}, {"-=", T_MINUSEQ}, {"*=", T_STAREQ}, {"/=", T_SLASHEQ},
            {"(", T_LPAREN}, {")", T_RPAREN}, {"{", T_LBRACE}, {"}", T_RBRACE}, {"[", T_LBRACKET}, {"]", T_RBRACKET},
            {",", T_COMMA}, {".", T_DOT}, {":", T_COLON}, {"=", T_ASSIGN}, {"+", T_PLUS}, {"-", T_MINUS}, {"*", T_STAR},
            {"/", T_SLASH}, {"%", T_PERCENT}, {"<", T_LT}, {">", T_GT}, {"!", T_BANG}, {"&", T_AMP}, {"|", T_PIPE},
            {"^", T_CARET}, {"~", T_TILDE}};
        bool found = false;
        for (size_t i = 0; i < sizeof punct / sizeof punct[0]; i++) {
            size_t n = strlen(punct[i].s);
            if (lx.p + n <= lx.end && memcmp(lx.p, punct[i].s, n) == 0) {
                TokKind k = punct[i].k;
                if (k == T_LPAREN || k == T_LBRACKET) lx.depth++;
                if ((k == T_RPAREN || k == T_RBRACKET) && lx.depth > 0) lx.depth--;
                if (k == T_LBRACE) { /* a block inside parentheses (a lambda's) has lines again */
                    if (lx.braces < 256) lx.saved[lx.braces] = lx.depth;
                    lx.braces++;
                    lx.depth = 0;
                }
                if (k == T_RBRACE && lx.braces > 0) {
                    lx.braces--;
                    if (lx.braces < 256) lx.depth = lx.saved[lx.braces];
                }
                lx.p += n;
                add(&lx, k, start, (int)n);
                found = true;
                break;
            }
        }
        if (!found) {
            qkError(src, lx.line, (int)(start - lx.lineStart) + 1, "\"%c\" is not part of Quark", c);
            lx.p++;
        }
    }
    if (out->count && endsStatement(out->tokens[out->count - 1].kind)) add(&lx, T_NEWLINE, lx.end, 0);
    lx.blank = false;
    add(&lx, T_EOF, lx.end, 0);
}
