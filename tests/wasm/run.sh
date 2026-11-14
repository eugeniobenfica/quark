#!/usr/bin/env bash
# The tests/run programs built for WebAssembly (quark build --target wasm32), run by Node.js when it is here, else
# by a headless Edge or Chrome (one page runs them all), against their recorded output; and the panics against
# theirs. Prints "PASS name" / "FAIL name" lines and a last line "wasm: P passed, F failed"; with no runtime to
# run Wasm, prints "wasm: skipped (...)" and passes.
#   tests/wasm/run.sh [path/to/quark.exe] [program.qk...]   (no programs: the list below; a program with no .out
#                                                            is checked against what quark run prints)
cd "$(dirname "$0")/../.."
Q=${1:-build/quark.exe}
shift
# the programs that run on Wasm (those left out need what Wasm has no way to give yet: tasks, threads)
PROGRAMS=(aliases bits closures constants divide features fnValues general genericTypes graphKeys interfaces language
    loops memory modern modules named parallel patterns random recordKeys recursive stdlib textBuild typedErrors types
    unicode values)
PANICS=(assert divide fuel index mean panic parallel slice stack textCut toInt trace tryMain)
if [ $# -gt 0 ]; then
    files=("$@")
else
    files=()
    for p in "${PROGRAMS[@]}"; do files+=("tests/run/$p.qk"); done
    for p in "${PANICS[@]}"; do files+=("tests/panics/$p.qk"); done
fi

runner=""
if command -v node > /dev/null 2>&1; then
    runner=node
else
    for b in "${QUARK_BROWSER:-}" "/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe" \
        "/c/Program Files/Microsoft/Edge/Application/msedge.exe" "/c/Program Files/Google/Chrome/Application/chrome.exe" \
        "$(command -v chromium 2> /dev/null)" "$(command -v google-chrome 2> /dev/null)" "$(command -v microsoft-edge 2> /dev/null)"; do
        if [ -n "$b" ] && [ -x "$b" ]; then
            browser=$b
            runner=browser
            break
        fi
    done
fi
if [ -z "$runner" ]; then
    echo "wasm: skipped (no Node.js, Edge or Chrome to run WebAssembly)"
    exit 0
fi

tmp=$(mktemp -d)
pass=0
fail=0
# what each program should say: its .out; a panic, all the native program says (its message and its stack of calls)
want() {
    case $1 in
        tests/panics/*) "$Q" run "$1" --fuel 1000000 2>&1 | tr -d '\r' ;;
        *) if [ -f "${1%.qk}.out" ]; then cat "${1%.qk}.out"; else "$Q" run "$1" 2>&1 | tr -d '\r'; fi ;;
    esac
}
built=()
for f in "${files[@]}"; do
    name=$(basename "$f" .qk)
    kind=run
    case $f in tests/panics/*) kind=panic ;; esac
    fuel=""
    [ $kind = panic ] && fuel="--fuel 1000000"
    if "$Q" build "$f" --target wasm32 $fuel -o "$tmp/$kind-$name.wasm" > "$tmp/build.txt" 2>&1; then
        built+=("$f")
    else
        echo "FAIL $f (build)"
        sed 's/^/    /' "$tmp/build.txt" | head -5
        fail=$((fail + 1))
    fi
done

if [ $runner = node ]; then
    for f in "${built[@]}"; do
        name=$(basename "$f" .qk)
        case $f in tests/panics/*) kind=panic ;; *) kind=run ;; esac
        node "$tmp/$kind-$name.js" > "$tmp/out" 2>&1
        status=$?
        if [ $kind = panic ]; then
            if [ $status -ne 0 ] && want "$f" | cmp -s - "$tmp/out"; then pass=$((pass + 1)); echo "PASS $f"; else fail=$((fail + 1)); echo "FAIL $f"; want "$f" | diff - "$tmp/out" | head -5; fi
        elif [ $status -eq 0 ] && want "$f" | cmp -s - "$tmp/out"; then
            pass=$((pass + 1))
            echo "PASS $f"
        else
            fail=$((fail + 1))
            echo "FAIL $f"
            want "$f" | diff - "$tmp/out" | head -5
        fi
    done
else
    # one page: the loader, then each module and what it should print, run one after the other
    page="$tmp/wasm.html"
    {
        echo '<!doctype html><meta charset="utf-8"><pre id="results">no results</pre><script>'
        cat "$tmp/$(ls "$tmp" | grep '\.js$' | head -1)"
        echo '</script><script>'
        echo 'const tests = ['
        for f in "${built[@]}"; do
            name=$(basename "$f" .qk)
            case $f in tests/panics/*) kind=panic ;; *) kind=run ;; esac
            printf '{name: "%s", kind: "%s", wasm: "%s", want: "%s"},\n' "$f" "$kind" "$(base64 -w0 "$tmp/$kind-$name.wasm")" "$(want "$f" | base64 -w0)"
        done
        cat << 'EOF'
];
const bytes = b64 => Uint8Array.from(atob(b64), c => c.charCodeAt(0));
const text = b => new TextDecoder().decode(b);
const lines = [];
for (const t of tests) {
    const chunks = [];
    let status;
    try {
        status = Quark.runSync(bytes(t.wasm), {write: (fd, b) => chunks.push(b.slice()), args: []});
    } catch (e) {
        chunks.push(new TextEncoder().encode('loader error: ' + e + '\n' + (e && e.stack) + '\n'));
        status = -1;
    }
    let got = '';
    for (const c of chunks) got += text(c);
    const want = text(bytes(t.want));
    const ok = (t.kind === 'panic' ? status !== 0 : status === 0) && got === want;
    lines.push((ok ? 'PASS ' : 'FAIL ') + t.name);
    if (!ok) {
        const g = got.split('\n'), w = want.split('\n');
        let i = 0;
        while (i < g.length && i < w.length && g[i] === w[i]) i++;
        lines.push('    status ' + status + ', line ' + (i + 1) + ': want ' + JSON.stringify(w[i]) + ', got ' + JSON.stringify(g.slice(i, i + 3).join('\n')));
    }
}
document.getElementById('results').textContent = lines.join('\n');
EOF
        echo '</script>'
    } > "$page"
    url="file:///$(cygpath -m "$page" 2> /dev/null || echo "$page")"
    profile="$tmp/profile"
    if command -v powershell.exe > /dev/null 2>&1; then # Windows: the browser's output reaches a pipe only through PowerShell
        powershell.exe -NoProfile -Command "& '$(cygpath -w "$browser")' --headless --disable-gpu --no-first-run --user-data-dir='$(cygpath -w "$profile")' --dump-dom '$url' | Out-String -Width 100000" > "$tmp/dom.txt" 2> /dev/null
    else
        "$browser" --headless --disable-gpu --no-first-run --user-data-dir="$profile" --dump-dom "$url" > "$tmp/dom.txt" 2> /dev/null
    fi
    sed -n '/<pre id="results">/,/<\/pre>/p' "$tmp/dom.txt" | sed 's/.*<pre id="results">//; s/<\/pre>.*//' | tr -d '\r' |
        sed 's/&quot;/"/g; s/&lt;/</g; s/&gt;/>/g; s/&amp;/\&/g' > "$tmp/results.txt"
    cat "$tmp/results.txt"
    pass=$((pass + $(grep -c '^PASS' "$tmp/results.txt")))
    fail=$((fail + $(grep -c '^FAIL' "$tmp/results.txt")))
    [ "$(grep -c '^PASS\|^FAIL' "$tmp/results.txt")" -eq ${#built[@]} ] || { echo "FAIL (the browser ran $(grep -c '^PASS\|^FAIL' "$tmp/results.txt") of ${#built[@]})"; fail=$((fail + 1)); }
fi
rm -rf "$tmp"
echo "wasm: $pass passed, $fail failed ($runner)"
[ "$fail" -eq 0 ]
