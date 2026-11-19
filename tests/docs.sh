#!/usr/bin/env bash
# The docs' examples compile: every ```quark block of docs/LANGUAGE.md, docs/EMBEDDING.md and README.md is checked
# with quark check. A block with fn main is a program; any other block's declarations (lines from column 0 that start
# one: fn, type, enum, interface, const, extern, pub, use, test, and what is indented under them or closes them: } or
# )) stay at the top and its other lines go into a fn main. A block whose first line is "// name.qk" is that file, for
# the blocks after it that use it. The lines of a comment <!-- given ... --> before a block go in front of it (the
# names an example takes as given), and a block after <!-- not checked --> is not checked (a sketch). Each block's
# lines must be in canonical form too: as quark fmt writes them.
#   tests/docs.sh [path/to/quark.exe]     prints a FAIL line for each block that does not compile, then the count
cd "$(dirname "$0")/.."
Q=${1:-build/quark.exe}
tmp=$(mktemp -d)
checked=0
failed=0
for doc in docs/LANGUAGE.md docs/EMBEDDING.md docs/TUTORIAL.md README.md; do
    dir="$tmp/$(basename "$doc" .md)"
    mkdir -p "$dir"
    # each block as a file (k.qk, or the name its first line gives), and a list of them with the doc's line
    awk -v dir="$dir" '
        function flush(   i, decl, main, name, out, d, s, line) {
            if (skip) { skip = 0; givenCount = 0; return }
            name = ""
            if (lines[1] ~ /^\/\/ [a-zA-Z0-9_]+\.qk$/) name = substr(lines[1], 4)
            out = dir "/" (name != "" ? name : k ".qk")
            d = ""; s = ""; decl = 0; main = 0
            for (i = 1; i <= count; i++) if (lines[i] ~ /^fn main\(/) main = 1
            for (i = 1; i <= givenCount + count; i++) {
                line = i <= givenCount ? given[i] : lines[i - givenCount]
                if (main || name != "") { d = d line "\n"; continue }
                if (line ~ /^(pub |fn |type |enum |interface |const |extern |use |test |opaque )/) decl = 1
                else if (line ~ /^[^ })\/]/ || (line ~ /^\/\// && !decl)) decl = 0
                if (decl) d = d line "\n"; else s = s "    " line "\n"
            }
            if (s ~ /[^ \n]/) d = d "\nfn main() {\n" s "}\n"
            printf "%s", d > out
            close(out)
            print out "\t" start "\t" (name != "" ? 1 : 0) > (dir "/list")
            givenCount = 0
        }
        /^ *<!-- not checked/ { skip = 1; next }
        /^ *<!-- given$/ { giving = 1; givenCount = 0; gindent = match($0, /[^ ]/) - 1; next }
        giving && /^ *-->$/ { giving = 0; next }
        giving { given[++givenCount] = substr($0, gindent + 1); next }
        /^ *```quark$/ { inside = 1; k++; count = 0; start = NR; indent = match($0, /[^ ]/) - 1; next }
        inside && /^ *```$/ { inside = 0; flush(); next }
        inside { lines[++count] = substr($0, indent + 1) }
    ' "$doc"
    [ -f "$dir/list" ] || continue
    while IFS=$'\t' read -r file line module; do
        [ "$module" = 1 ] && [ "$(basename "$file")" != main.qk ] && continue # a file another block uses
        checked=$((checked + 1))
        if ! out=$("$Q" check "$file" 2>&1); then
            failed=$((failed + 1))
            echo "FAIL $doc:$line"
            echo "$out" | head -4
        elif ! diff <(grep -v '^ *$' "$file") <("$Q" fmt "$file" | grep -v '^ *$') > "$tmp/diff"; then
            failed=$((failed + 1)) # its lines as quark fmt writes them (blank lines aside: a fragment has none of its own)
            echo "FAIL $doc:$line is not in canonical form"
            head -6 "$tmp/diff"
        fi
    done < "$dir/list"
done
rm -rf "$tmp"
echo "docs: $checked checked, $failed failed"
[ "$failed" = 0 ]
