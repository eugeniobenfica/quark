/* Quark — the formatter: one canonical text for every program. Four spaces, parentheses only where
 * precedence needs them, comments where they were, a method chain with several lambdas one call per line.
 * Node graphs print through it too, so text -> graph -> text gives the same text. */
#include "quark.h"

#include <stdio.h>

enum { P_LOWEST, P_OR, P_AND, P_NOT, P_COMPARE, P_RANGE, P_ADD, P_MUL, P_UNARY, P_POSTFIX, P_PRIMARY };

static int precedence(const Expr* e) {
    switch (e->kind) {
    case E_BINARY:
        switch (e->op) {
        case T_OR: return P_OR;
        case T_AND: return P_AND;
        case T_PLUS: case T_MINUS: return P_ADD;
        case T_STAR: case T_SLASH: case T_PERCENT: return P_MUL;
        default: return P_COMPARE;
        }
    case E_RANGE: return P_RANGE;
    case E_UNARY: return e->op == T_NOT ? P_NOT : P_UNARY;
    case E_LAMBDA: case E_IF: case E_MATCH: return P_LOWEST;
    case E_INT: case E_FLOAT: return e->raw && e->raw[0] == '-' ? P_UNARY : P_PRIMARY;
    default: return P_PRIMARY;
    }
}

static const Expr* visible(const Expr* e) {
    while (e->kind == E_CALL && e->implicit) e = e->args[0];
    if (e->kind == E_LAMBDA && e->sugar) return e->sugar;
    return e;
}

static void indentTo(QkBuf* out, int indent) {
    for (int i = 0; i < indent; i++) qkBufPuts(out, "    ");
}

static void comments(QkBuf* out, const char* text, int indent) {
    if (!text) return;
    const char* p = text;
    for (;;) {
        const char* nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        indentTo(out, indent);
        qkBufPuts(out, n ? "// " : "//");
        qkBufAdd(out, p, n);
        qkBufPuts(out, "\n");
        if (!nl) break;
        p = nl + 1;
    }
}

void qkFormatTypeExpr(const TypeExpr* t, QkBuf* out) {
    if (!t->name) {
        qkBufPuts(out, "[");
        qkFormatTypeExpr(t->elem, out);
        qkBufPuts(out, "]");
    } else {
        qkBufPuts(out, t->name);
    }
}

static void expr(const Expr* e, QkBuf* out, int indent, int minPrec);
static void block(const Block* b, QkBuf* out, int indent);

static void textLiteral(const Expr* e, QkBuf* out) {
    qkBufPuts(out, "\"");
    for (int i = 0; i < e->textLen; i++) {
        char c = e->text[i];
        if (c == '\n') qkBufPuts(out, "\\n");
        else if (c == '\t') qkBufPuts(out, "\\t");
        else if (c == '"') qkBufPuts(out, "\\\"");
        else if (c == '\\') qkBufPuts(out, "\\\\");
        else qkBufByte(out, (uint8_t)c);
    }
    qkBufPuts(out, "\"");
}

static int lambdaArgs(const Expr* call) {
    int n = 0;
    for (int i = 0; i < call->argCount; i++)
        if (visible(call->args[i])->kind == E_LAMBDA) n++;
    return n;
}

/* a.f(x).g(y): how many calls in the chain carry a lambda */
static int chainLambdas(const Expr* e) {
    int n = 0;
    while (e->kind == E_CALL && e->method && !e->implicit) {
        n += lambdaArgs(e) > 0;
        e = visible(e->args[0]);
    }
    return n;
}

static void args(const Expr* e, int from, QkBuf* out, int indent) {
    qkBufPuts(out, "(");
    for (int i = from; i < e->argCount; i++) {
        if (i > from) qkBufPuts(out, ", ");
        if (e->argNames && e->argNames[i]) qkBufPrintf(out, "%s: ", e->argNames[i]);
        expr(e->args[i], out, indent, P_LOWEST);
    }
    qkBufPuts(out, ")");
}

static void call(const Expr* e, QkBuf* out, int indent, bool broken) {
    if (e->method && e->argCount > 0) {
        const Expr* recv = visible(e->args[0]);
        bool recvChain = recv->kind == E_CALL && recv->method;
        if (recvChain) call(recv, out, indent, broken);
        else expr(e->args[0], out, indent, P_POSTFIX);
        if (broken) {
            qkBufPuts(out, "\n");
            indentTo(out, indent + 1);
        }
        qkBufPrintf(out, ".%s", e->name);
        args(e, 1, out, indent + (broken ? 1 : 0));
        return;
    }
    qkBufPuts(out, e->name);
    args(e, 0, out, indent);
}

/* if c { a } else { b } on one line, when both sides are one short value */
static bool shortBranch(const Block* b) {
    return b->count == 1 && b->stmts[0]->kind == S_EXPR && !b->stmts[0]->leading && !b->stmts[0]->trailing && !b->closingComment &&
           b->stmts[0]->value->kind != E_IF && b->stmts[0]->value->kind != E_MATCH && b->stmts[0]->value->kind != E_BLOCK;
}

static void ifExpr(const Expr* e, QkBuf* out, int indent) {
    if (shortBranch(e->thenBlock) && e->elseExpr && e->elseExpr->kind == E_BLOCK && shortBranch(e->elseExpr->block)) {
        QkBuf line = {0};
        qkBufPuts(&line, "if ");
        expr(e->a, &line, indent, P_LOWEST);
        qkBufPuts(&line, " { ");
        expr(e->thenBlock->stmts[0]->value, &line, indent, P_LOWEST);
        qkBufPuts(&line, " } else { ");
        expr(e->elseExpr->block->stmts[0]->value, &line, indent, P_LOWEST);
        qkBufPuts(&line, " }");
        if (line.len <= 70 && !memchr(line.data, '\n', line.len)) {
            qkBufAdd(out, line.data, line.len);
            qkBufFree(&line);
            return;
        }
        qkBufFree(&line);
    }
    qkBufPuts(out, "if ");
    expr(e->a, out, indent, P_LOWEST);
    qkBufPuts(out, " ");
    block(e->thenBlock, out, indent);
    if (e->elseExpr) {
        qkBufPuts(out, " else ");
        if (e->elseExpr->kind == E_IF) ifExpr(e->elseExpr, out, indent);
        else block(e->elseExpr->block, out, indent);
    }
}

static void expr(const Expr* e, QkBuf* out, int indent, int minPrec) {
    e = visible(e);
    int prec = precedence(e);
    bool parens = prec < minPrec;
    if (parens) qkBufPuts(out, "(");
    switch (e->kind) {
    case E_INT: case E_FLOAT:
        if (e->raw) qkBufPuts(out, e->raw);
        else if (e->kind == E_INT) qkBufPrintf(out, "%lld", (long long)e->i);
        else qkBufPrintf(out, "%.17g", e->f);
        break;
    case E_TEXT: textLiteral(e, out); break;
    case E_BOOL: qkBufPuts(out, e->i ? "true" : "false"); break;
    case E_NAME: qkBufPuts(out, e->name); break;
    case E_CASE: qkBufPrintf(out, "%s.%s", e->decl->name, e->name); break;
    case E_FIELD:
        expr(e->a, out, indent, P_POSTFIX);
        qkBufPrintf(out, ".%s", e->name);
        break;
    case E_INDEX:
        expr(e->a, out, indent, P_POSTFIX);
        qkBufPuts(out, "[");
        expr(e->b, out, indent, P_LOWEST);
        qkBufPuts(out, "]");
        break;
    case E_UNARY:
        qkBufPuts(out, e->op == T_NOT ? "not " : "-");
        expr(e->a, out, indent, e->op == T_NOT ? P_NOT : P_UNARY);
        break;
    case E_BINARY: case E_RANGE:
        /* left-associative: the right side binds one level tighter */
        expr(e->a, out, indent, prec == P_COMPARE || prec == P_RANGE ? prec + 1 : prec);
        qkBufPrintf(out, e->kind == E_RANGE ? "%s" : " %s ", qkTokName((TokKind)e->op));
        expr(e->b, out, indent, prec + 1);
        break;
    case E_IF: ifExpr(e, out, indent); break;
    case E_MATCH:
        qkBufPuts(out, "match ");
        expr(e->a, out, indent, P_LOWEST);
        qkBufPuts(out, " {\n");
        for (int i = 0; i < e->armCount; i++) {
            const Arm* a = &e->arms[i];
            comments(out, a->comment, indent + 1);
            indentTo(out, indent + 1);
            if (a->pattern) qkBufPuts(out, a->pattern);
            else if (a->intPattern) qkBufPuts(out, a->intPattern->raw);
            else qkBufPuts(out, "_");
            qkBufPuts(out, " => ");
            if (a->body->kind == E_BLOCK) block(a->body->block, out, indent + 1);
            else expr(a->body, out, indent + 1, P_LOWEST);
            qkBufPuts(out, "\n");
        }
        indentTo(out, indent);
        qkBufPuts(out, "}");
        break;
    case E_LAMBDA:
        if (e->paramCount == 1) qkBufPuts(out, e->params[0].name);
        else {
            qkBufPuts(out, "(");
            for (int i = 0; i < e->paramCount; i++) qkBufPrintf(out, "%s%s", i ? ", " : "", e->params[i].name);
            qkBufPuts(out, ")");
        }
        qkBufPuts(out, " => ");
        expr(e->body, out, indent, P_LOWEST);
        break;
    case E_ARRAY:
        qkBufPuts(out, "[");
        for (int i = 0; i < e->argCount; i++) {
            if (i) qkBufPuts(out, ", ");
            expr(e->args[i], out, indent, P_LOWEST);
        }
        qkBufPuts(out, "]");
        break;
    case E_BLOCK: block(e->block, out, indent); break;
    case E_CALL: {
        /* a chain of calls with lambdas goes one call per line when it is long */
        bool broken = false;
        int lambdas = e->method ? chainLambdas(e) : 0;
        if (lambdas >= 3) {
            broken = true;
        } else if (lambdas == 2) {
            QkBuf line = {0};
            call(e, &line, indent, false);
            broken = line.len > 60 || memchr(line.data, '\n', line.len);
            qkBufFree(&line);
        }
        call(e, out, indent, broken);
        break;
    }
    }
    if (parens) qkBufPuts(out, ")");
}

void qkFormatExpr(const Expr* e, QkBuf* out, int indent) { expr(e, out, indent, P_LOWEST); }

static void stmt(const Stmt* s, QkBuf* out, int indent) {
    comments(out, s->leading, indent);
    indentTo(out, indent);
    switch (s->kind) {
    case S_LET: case S_VAR:
        qkBufPrintf(out, "%s %s", s->kind == S_LET ? "let" : "var", s->name);
        if (s->typeAnn) {
            qkBufPuts(out, ": ");
            qkFormatTypeExpr(s->typeAnn, out);
        }
        qkBufPuts(out, " = ");
        expr(s->value, out, indent, P_LOWEST);
        break;
    case S_ASSIGN:
        expr(s->target, out, indent, P_LOWEST);
        qkBufPrintf(out, " %s ", qkTokName((TokKind)s->op));
        expr(s->value, out, indent, P_LOWEST);
        break;
    case S_EXPR: expr(s->value, out, indent, P_LOWEST); break;
    case S_FOR:
        qkBufPrintf(out, "for %s", s->name);
        if (s->indexName) qkBufPrintf(out, ", %s", s->indexName);
        qkBufPuts(out, " in ");
        expr(s->iter, out, indent, P_LOWEST);
        qkBufPuts(out, " ");
        block(s->body, out, indent);
        break;
    case S_WHILE:
        qkBufPuts(out, "while ");
        expr(s->value, out, indent, P_LOWEST);
        qkBufPuts(out, " ");
        block(s->body, out, indent);
        break;
    case S_RETURN:
        qkBufPuts(out, "return");
        if (s->value) {
            qkBufPuts(out, " ");
            expr(s->value, out, indent, P_LOWEST);
        }
        break;
    case S_BREAK: qkBufPuts(out, "break"); break;
    case S_CONTINUE: qkBufPuts(out, "continue"); break;
    }
    if (s->trailing) qkBufPrintf(out, " // %s", s->trailing);
    qkBufPuts(out, "\n");
}

static void block(const Block* b, QkBuf* out, int indent) {
    if (!b->count && !b->closingComment) {
        qkBufPuts(out, "{}");
        return;
    }
    qkBufPuts(out, "{\n");
    for (int i = 0; i < b->count; i++) {
        if (i && b->stmts[i]->blankBefore) qkBufPuts(out, "\n");
        stmt(b->stmts[i], out, indent + 1);
    }
    comments(out, b->closingComment, indent + 1);
    indentTo(out, indent);
    qkBufPuts(out, "}");
}

static void decl(const Decl* d, QkBuf* out) {
    comments(out, d->leading, 0);
    if (d->kind == D_FN) {
        qkBufPrintf(out, "fn %s(", d->name);
        for (int i = 0; i < d->paramCount; i++) {
            qkBufPrintf(out, "%s%s: ", i ? ", " : "", d->params[i].name);
            qkFormatTypeExpr(d->params[i].type, out);
        }
        qkBufPuts(out, ")");
        if (d->ret) {
            qkBufPuts(out, " -> ");
            qkFormatTypeExpr(d->ret, out);
        }
        if (d->body->kind == E_BLOCK) {
            qkBufPuts(out, " ");
            block(d->body->block, out, 0);
        } else {
            /* = expr on the same line, or indented on the next when the line would pass 100 columns */
            QkBuf line = {0};
            expr(d->body, &line, 0, P_LOWEST);
            size_t lineStart = out->len;
            while (lineStart > 0 && out->data[lineStart - 1] != '\n') lineStart--;
            bool multiline = memchr(line.data, '\n', line.len) != NULL;
            if (!multiline && out->len - lineStart + 3 + line.len > 100) {
                qkBufPuts(out, " =\n    ");
                QkBuf indented = {0};
                expr(d->body, &indented, 1, P_LOWEST);
                qkBufAdd(out, indented.data, indented.len);
                qkBufFree(&indented);
            } else {
                qkBufPuts(out, " = ");
                qkBufAdd(out, line.data, line.len);
            }
            qkBufFree(&line);
        }
    } else {
        QkBuf line = {0};
        qkBufPrintf(&line, "%s %s { ", d->kind == D_TYPE ? "type" : "enum", d->name);
        int n = d->kind == D_TYPE ? d->paramCount : d->caseCount;
        for (int i = 0; i < n; i++) {
            if (i) qkBufPuts(&line, ", ");
            if (d->kind == D_TYPE) {
                qkBufPrintf(&line, "%s: ", d->params[i].name);
                qkFormatTypeExpr(d->params[i].type, &line);
            } else {
                qkBufPuts(&line, d->cases[i]);
            }
        }
        qkBufPuts(&line, " }");
        if (line.len <= 100) {
            qkBufAdd(out, line.data, line.len);
        } else {
            qkBufPrintf(out, "%s %s {\n", d->kind == D_TYPE ? "type" : "enum", d->name);
            for (int i = 0; i < n; i++) {
                indentTo(out, 1);
                if (d->kind == D_TYPE) {
                    qkBufPrintf(out, "%s: ", d->params[i].name);
                    qkFormatTypeExpr(d->params[i].type, out);
                } else {
                    qkBufPuts(out, d->cases[i]);
                }
                qkBufPuts(out, "\n");
            }
            qkBufPuts(out, "}");
        }
        qkBufFree(&line);
    }
    if (d->trailing) qkBufPrintf(out, " // %s", d->trailing);
    qkBufPuts(out, "\n");
}

static bool oneLine(const Decl* d) {
    QkBuf b = {0};
    decl(d, &b);
    bool one = b.len && !memchr(b.data, '\n', b.len - 1);
    qkBufFree(&b);
    return one;
}

void qkFormat(const Program* p, QkBuf* out) {
    for (int i = 0; i < p->count; i++) {
        /* a blank line between declarations, except between one-line types and enums */
        const Decl* d = p->decls[i];
        const Decl* before = i ? p->decls[i - 1] : NULL;
        bool together = before && d->kind != D_FN && before->kind != D_FN && !d->leading && oneLine(before) && oneLine(d);
        if (i && !together) qkBufPuts(out, "\n");
        decl(d, out);
    }
    if (p->closingComment) {
        if (p->count) qkBufPuts(out, "\n");
        comments(out, p->closingComment, 0);
    }
    if (!out->data) qkBufAdd(out, "", 0);
}
