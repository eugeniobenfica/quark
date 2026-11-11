/* A tiny engine that runs Quark the way Artifice would: the engine's API is a prelude of extern fns backed by
 * C functions, each object's behavior is a script whose events (start, update, hit) the engine calls every
 * frame, the object's variables live in the engine between calls, a script can be swapped while the game
 * runs, a script that fails stops only itself, and the node editor's graphs compile like text.
 *
 *   build/host.exe [examples/host/engine.qk] [examples/host/ship.qk] */
#include "quark.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the engine's objects ------------------------------------------------------------------------ */

typedef struct Obj {
    const char* name;
    double x, y;
    double r, g, b, a;
    int alive;
} Obj;

static Obj objects[3] = {{"nobody"}, {"ship1", 0, 10}, {"ship2", 100, 50}};
static int spaceDown;

/* The C side of the prelude's extern fns: one argument per slot, Float slots as double. */
typedef struct V2 {
    double x, y;
} V2;

static V2 position(int64_t o) {
    V2 v = {objects[o].x, objects[o].y};
    return v; /* two slots: Quark passes the hidden pointer Windows x64 uses for it */
}
static void moveBy(int64_t o, double dx, double dy) {
    objects[o].x += dx;
    objects[o].y += dy;
}
static void moveTo(int64_t o, double x, double y) {
    objects[o].x = x;
    objects[o].y = y;
}
static void setColor(int64_t o, double r, double g, double b, double a) { /* five arguments: the fifth on the stack */
    objects[o].r = r;
    objects[o].g = g;
    objects[o].b = b;
    objects[o].a = a;
}
static void say(int64_t o, const char* text, int64_t len) { printf("  %s says \"%.*s\"\n", objects[o].name, (int)len, text); }
static void destroy(int64_t o) {
    objects[o].alive = 0;
    printf("  %s is destroyed\n", objects[o].name);
}
static int64_t keyDown(const char* key, int64_t len) { return spaceDown && len == 5 && !memcmp(key, "space", 5); }

/* ---- helpers ------------------------------------------------------------------------------------- */

static char* readFile(const char* path, size_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* s = malloc((size_t)n + 1);
    size_t got = fread(s, 1, (size_t)n, f);
    fclose(f);
    s[got] = '\0';
    *len = got;
    return s;
}

static char* replaced(const char* text, const char* from, const char* to) {
    const char* at = strstr(text, from);
    size_t n = strlen(text), a = strlen(from), b = strlen(to);
    char* s = malloc(n - a + b + 1);
    size_t head = (size_t)(at - text);
    memcpy(s, text, head);
    memcpy(s + head, to, b);
    strcpy(s + head + b, at + a);
    return s;
}

static void showDiagnostics(QkVm* vm) {
    for (int i = 0; i < qkDiagnosticCount(vm); i++) {
        const QkDiagnostic* d = qkDiagnosticAt(vm, i);
        if (d->key[0]) printf("  error at node %s: %s\n", d->key, d->message);
        else printf("  error: %s\n", d->message);
    }
}

/* Each ship's variables (its Ship record: speed, laps), kept by the engine between calls. */
static QkValue state[3][2];

static void frames(QkVm* vm, QkModule* m, int count, int spaceFrom) {
    int update = qkFind(m, "update");
    for (int f = 0; f < count; f++) {
        spaceDown = f >= spaceFrom;
        for (int o = 1; o <= 2; o++) {
            if (!objects[o].alive) continue;
            QkValue args[4] = {{.i = o}, state[o][0], state[o][1], {.f = 1.0 / 60}};
            QkValue next[2];
            if (!qkCall(m, update, args, 4, next, 2)) {
                printf("  %s stopped: %s\n", objects[o].name, qkLastError(vm));
                continue;
            }
            state[o][0] = next[0];
            state[o][1] = next[1];
        }
    }
    for (int o = 1; o <= 2; o++)
        printf("  %s: x %.2f, speed %.1f, laps %lld%s\n", objects[o].name, objects[o].x, state[o][0].f, (long long)state[o][1].i,
               objects[o].alive ? "" : " (destroyed)");
}

int main(int argc, char** argv) {
    const char* preludePath = argc > 1 ? argv[1] : "examples/host/engine.qk";
    const char* scriptPath = argc > 2 ? argv[2] : "examples/host/ship.qk";
    size_t len;

    QkVm* vm = qkVmNew();
    char* prelude = readFile(preludePath, &len);
    if (!qkVmPrelude(vm, prelude)) {
        showDiagnostics(vm);
        return 1;
    }
    qkVmHost(vm, "position", (void*)position);
    qkVmHost(vm, "moveBy", (void*)moveBy);
    qkVmHost(vm, "moveTo", (void*)moveTo);
    qkVmHost(vm, "setColor", (void*)setColor);
    qkVmHost(vm, "say", (void*)say);
    qkVmHost(vm, "destroy", (void*)destroy);
    qkVmHost(vm, "keyDown", (void*)keyDown);
    qkVmFuel(vm, 1000000); /* a script that loops forever stops, the game does not */

    printf("compile %s\n", scriptPath);
    char* text = readFile(scriptPath, &len);
    QkModule* m = qkCompile(vm, "ship.qk", text, len);
    if (!m) {
        showDiagnostics(vm);
        return 1;
    }
    for (int i = 0; i < qkFunctionCount(m); i++)
        printf("  %s  [%s]\n", qkFunctionSignature(m, i), qkSlotKinds(m, i));

    printf("start\n");
    for (int o = 1; o <= 2; o++) {
        objects[o].alive = 1;
        QkValue self = {.i = o};
        if (!qkCall(m, qkFind(m, "start"), &self, 1, state[o], 2)) printf("  %s\n", qkLastError(vm));
    }
    printf("  ship1 color %.1f %.1f %.1f %.1f\n", objects[1].r, objects[1].g, objects[1].b, objects[1].a);

    printf("120 frames (space held from frame 60)\n");
    frames(vm, m, 120, 60);

    printf("hit\n");
    QkValue pair[2] = {{.i = 1}, {.i = 2}};
    qkCall(m, qkFind(m, "hit"), pair, 2, NULL, 0);

    printf("hot reload: laps no longer speed the ship up\n");
    char* faster = replaced(text, "ship.speed * 1.5", "ship.speed");
    QkModule* m2 = qkCompile(vm, "ship.qk", faster, strlen(faster));
    if (!m2) {
        showDiagnostics(vm);
        return 1;
    }
    qkModuleFree(m);
    m = m2;
    frames(vm, m, 240, 1000);

    printf("a script that fails\n");
    char* broken = replaced(faster, "    let at = position(self)",
                            "    let at = position(self)\n    let xs = [1, 2, 3]\n    moveBy(self, toFloat(xs[ship.laps + 5]), 0)");
    QkModule* bad = qkCompile(vm, "broken.qk", broken, strlen(broken));
    if (!bad) {
        showDiagnostics(vm);
        return 1;
    }
    frames(vm, bad, 1, 1000);
    printf("  the engine goes on with the good one:\n");
    frames(vm, m, 1, 1000);
    qkModuleFree(bad);

    printf("a script that never stops\n");
    const char* forever = "fn update(self: Object, ship: Ship, dt: Float) -> Ship {\n"
                          "    while true {\n"
                          "        moveBy(self, 0, 0)\n"
                          "    }\n"
                          "    ship\n"
                          "}\n"
                          "\n"
                          "type Ship { speed: Float, laps: Int }\n";
    QkModule* loop = qkCompile(vm, "forever.qk", forever, strlen(forever));
    if (!loop) {
        showDiagnostics(vm);
        return 1;
    }
    frames(vm, loop, 1, 1000);
    qkModuleFree(loop);

    printf("from the node editor: text -> graph -> compiled\n");
    char* graph = qkTextToGraph(vm, faster, strlen(faster), "{\"update/at\": [40, 120]}");
    if (!graph) {
        showDiagnostics(vm);
        return 1;
    }
    printf("  update/at placed at (40, 120): %s\n", strstr(graph, "\"pos\": [40, 120]") ? "yes" : "no");
    QkModule* fromGraph = qkCompileGraph(vm, "ship.graph", graph, strlen(graph));
    if (!fromGraph) {
        showDiagnostics(vm);
        return 1;
    }
    QkValue self = {.i = 1};
    QkValue fresh[2];
    qkCall(fromGraph, qkFind(fromGraph, "start"), &self, 1, fresh, 2);
    printf("  start from the graph gives speed %.1f, laps %lld\n", fresh[0].f, (long long)fresh[1].i);
    qkModuleFree(fromGraph);

    printf("a graph with a mistake: errors point at nodes\n");
    char* wrong = replaced(graph, "\"fn\": \"moveBy\"", "\"fn\": \"moveByy\"");
    QkModule* none = qkCompileGraph(vm, "ship.graph", wrong, strlen(wrong));
    if (!none) showDiagnostics(vm);

    char* catalog = qkCatalog(vm, faster, strlen(faster));
    printf("catalog: %s, %s, %s\n", strstr(catalog, "\"name\": \"histogram\"") ? "built-ins" : "-",
           strstr(catalog, "\"name\": \"moveBy\", \"kind\": \"extern fn\", \"host\": true") ? "the engine's externs" : "-",
           strstr(catalog, "\"name\": \"update\", \"kind\": \"fn\", \"host\": false") ? "the script's events" : "-");

    qkFree(catalog);
    qkFree(graph);
    free(wrong);
    free(broken);
    free(faster);
    free(text);
    free(prelude);
    qkModuleFree(m);
    qkVmFree(vm);
    return 0;
}
