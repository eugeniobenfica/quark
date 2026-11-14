/* qkDiff and qkMerge from a host: versions given as text or as graphs, the changes and the merge back.
 *   gcc -Iinclude tests/collab/api.c build/libquark.a -lm -o api && ./api tests/collab */
#include "quark.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* readAll(const char* dir, const char* name) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* s = malloc((size_t)n + 1);
    s[fread(s, 1, (size_t)n, f)] = '\0';
    fclose(f);
    return s;
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "tests/collab";
    char* base = readAll(dir, "base.qk");
    char* ours = readAll(dir, "ours.qk");
    char* theirs = readAll(dir, "theirs.qk");
    char* clash = readAll(dir, "conflict.qk");
    if (!base || !ours || !theirs || !clash) return 2;
    QkVm* vm = qkVmNew();

    char* diff = qkDiff(vm, base, strlen(base), ours, strlen(ours), false);
    printf("diff:\n%s", diff);
    char* oursGraph = qkTextToGraph(vm, ours, strlen(ours), NULL); /* a graph and a text side by side */
    char* same = qkDiff(vm, ours, strlen(ours), oursGraph, strlen(oursGraph), true);
    printf("the same: %s", same);

    char* conflicts;
    char* merged = qkMerge(vm, base, strlen(base), ours, strlen(ours), theirs, strlen(theirs), false, &conflicts);
    printf("merged (conflicts %s):\n%s", conflicts, merged);
    qkFree(conflicts);
    char* graph = qkMerge(vm, base, strlen(base), oursGraph, strlen(oursGraph), theirs, strlen(theirs), true, NULL);
    char* back = graph ? qkGraphToText(vm, graph, strlen(graph)) : NULL;
    printf("as a graph: %s\n", back && !strcmp(back, merged) ? "the same program" : "different");
    char* clashed = qkMerge(vm, base, strlen(base), ours, strlen(ours), clash, strlen(clash), false, &conflicts);
    printf("conflicts: %s", conflicts);
    printf("kept ours: %s\n", clashed && strstr(clashed, "let n = 12") ? "yes" : "no");

    char* broken = qkDiff(vm, "fn main( {", 10, base, strlen(base), false);
    printf("not a program: %s (%d diagnostics)\n", broken ? "a diff" : "NULL", qkDiagnosticCount(vm));
    qkFree(diff);
    qkFree(same);
    qkFree(merged);
    qkFree(graph);
    qkFree(back);
    qkFree(clashed);
    qkFree(conflicts);
    qkFree(oursGraph);
    qkVmFree(vm);
    return 0;
}
