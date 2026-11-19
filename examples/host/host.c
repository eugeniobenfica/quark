/* A small application that embeds Quark: its API is a prelude of extern fns backed by C functions, each
 * item's behavior is a script whose events (start, tick, meet) the application calls on every tick, the item's
 * variables live in the application between calls, a script can be swapped while it runs, a script that fails
 * stops only itself, and a node editor's graphs compile like text.
 *
 *   build/host.exe [examples/host/app.qk] [examples/host/item.qk] */
#include "quark.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the application's items --------------------------------------------------------------------- */

typedef struct Obj {
    const char* name;
    double x, y;
    double r, g, b, a;
    int alive;
} Obj;

static Obj items[3] = {{"nobody"}, {"item1", 0, 10}, {"item2", 100, 50}};
static int fastOn;

/* The C side of the prelude's extern fns: one argument per slot, Float slots as double. */
typedef struct Point {
    double x, y;
} Point;

static Point position(int64_t i) {
    Point p = {items[i].x, items[i].y};
    return p; /* two slots: Quark passes the hidden pointer Windows x64 uses for it */
}
static void moveBy(int64_t i, double dx, double dy) {
    items[i].x += dx;
    items[i].y += dy;
}
static void moveTo(int64_t i, double x, double y) {
    items[i].x = x;
    items[i].y = y;
}
static void setColor(int64_t i, double r, double g, double b, double a) { /* five arguments: the fifth on the stack */
    items[i].r = r;
    items[i].g = g;
    items[i].b = b;
    items[i].a = a;
}
static void note(int64_t i, const char* text, int64_t len) { printf("  %s notes \"%.*s\"\n", items[i].name, (int)len, text); }
static void delete(int64_t i) {
    items[i].alive = 0;
    printf("  %s is deleted\n", items[i].name);
}
static int64_t flag(const char* name, int64_t len) { return fastOn && len == 4 && !memcmp(name, "fast", 4); }

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

/* Each item's variables (its Mover record: speed, rounds), kept by the application between calls. */
static QkValue state[3][2];

static void ticks(QkVm* vm, QkModule* m, int count, int fastFrom) {
    int tick = qkFind(m, "tick");
    for (int t = 0; t < count; t++) {
        fastOn = t >= fastFrom;
        for (int i = 1; i <= 2; i++) {
            if (!items[i].alive) continue;
            QkValue args[4] = {{.i = i}, state[i][0], state[i][1], {.f = 1.0 / 60}};
            QkValue next[2];
            if (!qkCall(m, tick, args, 4, next, 2)) {
                printf("  %s stopped: %s\n", items[i].name, qkLastError(vm));
                continue;
            }
            state[i][0] = next[0];
            state[i][1] = next[1];
        }
    }
    for (int i = 1; i <= 2; i++)
        printf("  %s: x %.2f, speed %.1f, rounds %lld%s\n", items[i].name, items[i].x, state[i][0].f, (long long)state[i][1].i,
               items[i].alive ? "" : " (deleted)");
}

int main(int argc, char** argv) {
    const char* preludePath = argc > 1 ? argv[1] : "examples/host/app.qk";
    const char* scriptPath = argc > 2 ? argv[2] : "examples/host/item.qk";
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
    qkVmHost(vm, "note", (void*)note);
    qkVmHost(vm, "delete", (void*)delete);
    qkVmHost(vm, "flag", (void*)flag);
    qkVmFuel(vm, 1000000); /* a script that loops forever stops, the application does not */

    printf("compile %s\n", scriptPath);
    char* text = readFile(scriptPath, &len);
    QkModule* m = qkCompile(vm, "item.qk", text, len);
    if (!m) {
        showDiagnostics(vm);
        return 1;
    }
    for (int i = 0; i < qkFunctionCount(m); i++)
        printf("  %s  [%s]\n", qkFunctionSignature(m, i), qkSlotKinds(m, i));

    printf("start\n");
    for (int i = 1; i <= 2; i++) {
        items[i].alive = 1;
        QkValue item = {.i = i};
        if (!qkCall(m, qkFind(m, "start"), &item, 1, state[i], 2)) printf("  %s\n", qkLastError(vm));
    }
    printf("  item1 color %.1f %.1f %.1f %.1f\n", items[1].r, items[1].g, items[1].b, items[1].a);

    printf("120 ticks (fast from tick 60)\n");
    ticks(vm, m, 120, 60);

    printf("meet\n");
    QkValue pair[2] = {{.i = 1}, {.i = 2}};
    qkCall(m, qkFind(m, "meet"), pair, 2, NULL, 0);

    printf("hot reload: rounds no longer speed the item up\n");
    char* steady = replaced(text, "mover.speed * 1.5", "mover.speed");
    QkModule* m2 = qkCompile(vm, "item.qk", steady, strlen(steady));
    if (!m2) {
        showDiagnostics(vm);
        return 1;
    }
    qkModuleFree(m);
    m = m2;
    ticks(vm, m, 240, 1000);

    printf("a script that fails\n");
    char* broken = replaced(steady, "    let at = position(item)",
                            "    let at = position(item)\n    let xs = [1, 2, 3]\n    moveBy(item, toFloat(xs[mover.rounds + 5]), 0)");
    QkModule* bad = qkCompile(vm, "broken.qk", broken, strlen(broken));
    if (!bad) {
        showDiagnostics(vm);
        return 1;
    }
    ticks(vm, bad, 1, 1000);
    printf("  the application goes on with the good one:\n");
    ticks(vm, m, 1, 1000);
    qkModuleFree(bad);

    printf("a script that never stops\n");
    const char* forever = "fn tick(item: Item, mover: Mover, dt: Float) -> Mover {\n"
                          "    while true {\n"
                          "        moveBy(item, 0, 0)\n"
                          "    }\n"
                          "    mover\n"
                          "}\n"
                          "\n"
                          "type Mover { speed: Float, rounds: Int }\n";
    QkModule* loop = qkCompile(vm, "forever.qk", forever, strlen(forever));
    if (!loop) {
        showDiagnostics(vm);
        return 1;
    }
    ticks(vm, loop, 1, 1000);
    qkModuleFree(loop);

    printf("from the node editor: text -> graph -> compiled\n");
    char* graph = qkTextToGraph(vm, steady, strlen(steady), "{\"tick/at\": [40, 120]}");
    if (!graph) {
        showDiagnostics(vm);
        return 1;
    }
    printf("  tick/at placed at (40, 120): %s\n", strstr(graph, "\"pos\": [40, 120]") ? "yes" : "no");
    QkModule* fromGraph = qkCompileGraph(vm, "item.graph", graph, strlen(graph));
    if (!fromGraph) {
        showDiagnostics(vm);
        return 1;
    }
    QkValue item = {.i = 1};
    QkValue fresh[2];
    qkCall(fromGraph, qkFind(fromGraph, "start"), &item, 1, fresh, 2);
    printf("  start from the graph gives speed %.1f, rounds %lld\n", fresh[0].f, (long long)fresh[1].i);
    qkModuleFree(fromGraph);

    printf("a graph with a mistake: errors point at nodes\n");
    char* wrong = replaced(graph, "\"fn\": \"moveBy\"", "\"fn\": \"moveByy\"");
    QkModule* none = qkCompileGraph(vm, "item.graph", wrong, strlen(wrong));
    if (!none) showDiagnostics(vm);

    char* catalog = qkCatalog(vm, steady, strlen(steady));
    printf("catalog: %s, %s, %s\n", strstr(catalog, "\"name\": \"split\"") ? "built-ins" : "-",
           strstr(catalog, "\"name\": \"moveBy\", \"kind\": \"extern fn\", \"host\": true") ? "the application's externs" : "-",
           strstr(catalog, "\"name\": \"tick\", \"kind\": \"fn\", \"host\": false") ? "the script's events" : "-");

    printf("a plugin with fewer capabilities: it may read items and print, nothing else\n");
    qkVmCapabilities(vm, "position flag print");
    QkModule* plugin = qkCompile(vm, "plugin.qk", steady, strlen(steady));
    if (plugin) {
        printf("  the plugin compiled, but it should not have\n");
        qkModuleFree(plugin);
    } else {
        printf("  refused: %s\n", qkDiagnosticAt(vm, 0)->message);
    }
    qkVmCapabilities(vm, NULL);

    printf("a task beside calls: each call's memory goes back when the next one starts, the task keeps its own\n");
    const char* jobs = "fn job(name: Text) {\n"
                       "    var log = \"\"\n"
                       "    for round in 0..3 {\n"
                       "        log = log + \"{name} round {round}; \"\n"
                       "        yield()\n"
                       "    }\n"
                       "    print(\"  \" + trim(log))\n"
                       "}\n"
                       "\n"
                       "fn work(n: Int) -> Int {\n"
                       "    var s = \"\"\n"
                       "    for i in 0..n {\n"
                       "        s = s + \"line {i}\\n\"\n"
                       "    }\n"
                       "    len(s)\n"
                       "}\n";
    QkModule* jm = qkCompile(vm, "jobs.qk", jobs, strlen(jobs));
    if (!jm) {
        showDiagnostics(vm);
        return 1;
    }
    static const char name[] = "import"; /* the host's memory: it outlives the task */
    QkValue jobArgs[2] = {{.p = name}, {.i = 6}};
    QkTaskHandle* job = qkTaskStart(jm, qkFind(jm, "job"), jobArgs, 2);
    int calls = 0;
    int64_t made = 0;
    while (job) {
        QkValue n = {.i = 100000}, len;
        if (qkCall(jm, qkFind(jm, "work"), &n, 1, &len, 1)) made = len.i;
        calls++;
        if (qkTaskResume(job) != 1) {
            qkTaskEnd(job);
            job = NULL;
        }
    }
    printf("  %d calls of work between its steps, each making a text of %lld bytes\n", calls, (long long)made);
    qkModuleFree(jm);

    qkFree(catalog);
    qkFree(graph);
    free(wrong);
    free(broken);
    free(steady);
    free(text);
    free(prelude);
    qkModuleFree(m);
    qkVmFree(vm);
    return 0;
}
