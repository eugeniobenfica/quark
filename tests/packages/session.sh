#!/usr/bin/env bash
# Packages: quark get from git repositories this script makes (no network), use pkg.module, quark.lock, and
# the errors: not fetched, hash mismatch, a cycle, a package asked for twice, a missing tag; semantic versions
# (minimal version selection) and a registry.
#   tests/packages/session.sh path/to/quark.exe     (its output is tests/packages/session.out)
Q=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
# sed in place, the same with GNU's sed and BSD's (macOS)
sedIn() { sed "$1" "$2" > "$2.new" && mv "$2.new" "$2"; }
work=$(mktemp -d)
cd "$work" || exit 1
# git as on a clean machine, with fixed dates: the same commits, so the same output, everywhere
: > gitconfig
export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL="$work/gitconfig" GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@t GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@t
export GIT_AUTHOR_DATE="2026-01-01T00:00:00Z" GIT_COMMITTER_DATE="2026-01-01T00:00:00Z"
repo() { mkdir -p "$1" && git -C "$1" init -q -b main; }
commit() { git -C "$1" add -A && git -C "$1" commit -q -m "$2"; }
say() { echo "\$ $*"; }
# only Quark's own lines: git's wording differs between versions
quark() { "$Q" "$@" 2>&1 | grep -av '^\(fatal\|error\|warning\|hint\):' | sed "s|$work/||g"; }

repo geom
printf '[package]\nname = "geom"\nversion = "1.0.0"\n' > geom/quark.toml
printf 'pub fn area(w: Int, h: Int) -> Int = w * h\n' > geom/geom.qk
printf 'pub fn square(s: Int) -> Int = s * s\n' > geom/shapes.qk
commit geom one && git -C geom tag v1.0
printf 'pub fn area(w: Int, h: Int) -> Int = w * h * 1\n\npub fn twice(x: Int) -> Int = 2 * x\n' > geom/geom.qk
commit geom two && git -C geom tag v2.0

repo util
printf '[package]\nname = "util"\nversion = "0.3.0"\n\n[dependencies]\ngeom = { git = "../geom", tag = "v1.0" }\n' > util/quark.toml
printf 'use geom\n\npub fn room(n: Int) -> Int = area(n, n + 1)\n' > util/util.qk
commit util one
rev=$(git -C util rev-parse --short=10 HEAD)

mkdir app
cat > app/quark.toml <<TOML
# the app's packages
[package]
name = "app"
version = "0.1.0"

[dependencies]
geom = { git = "../geom", tag = "v1.0" }
util = { git = "../util", rev = "$rev" }
TOML
printf 'use geom\nuse geom.shapes\nuse util\n\nfn main() {\n    print(area(3, 4), square(5), room(2))\n}\n' > app/main.qk

say "quark run app/main.qk  (before quark get)"
quark run app/main.qk
say quark get app
quark get app
cat app/quark.lock
say quark run app/main.qk
quark run app/main.qk
say "a file in a folder of the project, run from here and from its folder"
mkdir app/tools
printf 'use geom.shapes

fn main() {
    print(square(7))
}
' > app/tools/seven.qk
quark run app/tools/seven.qk
(cd app/tools && quark run seven.qk)

say "quark run, the repositories gone (no network needed)"
mv geom geom.away && mv util util.away
quark run app/main.qk
say "quark get, the repositories gone: all locked, all there"
quark get app
mv geom.away geom && mv util.away util

say "a package's file changed by hand"
printf '// changed\n' >> app/.quark/pkg/geom/geom.qk
quark run app/main.qk
say "quark get puts it back"
quark get app > /dev/null && quark run app/main.qk

say "a tag moved: quark.lock still says the commit"
git -C geom tag -f v1.0 v2.0 > /dev/null
quark get app
git -C geom tag -f v1.0 HEAD~1 > /dev/null

say "quark.lock's hash is not what the commit holds"
cp app/quark.lock lock.saved
sedIn 's/hash = "sha256-0/hash = "sha256-f/; s/hash = "sha256-\([1-9a-f]\)/hash = "sha256-0/' app/quark.lock
quark get app
cp lock.saved app/quark.lock

say "quark.toml asks for another tag, not fetched yet"
sedIn 's/tag = "v1.0"/tag = "v2.0"/' app/quark.toml
quark run app/main.qk
say "quark get: util asks for geom v1.0, the app for v2.0"
quark get app
sedIn 's/tag = "v2.0"/tag = "v9"/' app/quark.toml
say "a tag that is not there"
quark get app
sedIn 's/tag = "v9"/tag = "v1.0"/' app/quark.toml

say "a file next to main.qk with a package's name"
printf 'pub fn other() -> Int = 1\n' > app/util.qk
quark check app/main.qk
rm app/util.qk

say "a cycle: util needs app"
printf '[package]\nname = "util"\nversion = "0.4.0"\n\n[dependencies]\ngeom = { git = "../geom", tag = "v1.0" }\napp = { git = "../app", tag = "v1" }\n' > util/quark.toml
commit util two
(cd app && git init -q -b main && git add -A && git commit -q -m app && git tag v1)
sedIn "s/rev = \"$rev\"/rev = \"$(git -C util rev-parse --short=10 HEAD)\"/" app/quark.toml
quark get app

say "versions: each asks for the least it wants; the newest of those is taken"
repo shape
for v in 1.0.0 1.1.0 1.2.0 2.0.0; do
    printf '[package]\nname = "shape"\nversion = "%s"\n' "$v" > shape/quark.toml
    printf 'pub fn sides() -> Text = "%s"\n' "$v" > shape/shape.qk
    commit shape "$v" && git -C shape tag "v$v"
done
repo lib
printf '[package]\nname = "lib"\nversion = "0.1.0"\n\n[dependencies]\nshape = { git = "../shape", version = "1.1" }\n' > lib/quark.toml
printf 'use shape\n\npub fn libSides() -> Text = sides()\n' > lib/lib.qk
commit lib one && git -C lib tag v0.1.0
mkdir proj
printf '[package]\nname = "proj"\nversion = "0.1.0"\n\n[dependencies]\nshape = { git = "../shape", version = "1.0" }\nlib = { git = "../lib", version = "0.1" }\n' > proj/quark.toml
printf 'use shape\nuse lib\n\nfn main() {\n    print(sides(), libSides())\n}\n' > proj/main.qk
quark get proj
grep -A2 'name = "shape"' proj/quark.lock
quark run proj/main.qk
say "the project asks for 1.2: newer than the lock"
sedIn 's/version = "1.0"/version = "1.2"/' proj/quark.toml
quark run proj/main.qk
quark get proj
quark run proj/main.qk
say "2.0 against lib's 1.1"
sedIn 's/version = "1.2"/version = "2.0"/' proj/quark.toml
quark get proj
say "a version with no tag"
sedIn 's/version = "2.0"/version = "1.7"/' proj/quark.toml
quark get proj
sedIn 's/version = "1.7"/version = "x.1"/' proj/quark.toml
quark get proj

say "by version alone: the git URL from a registry"
repo registry
printf '# what each name is\n[packages]\nshape = "../shape"\nlib = "../lib"\n' > registry/index.toml
commit registry one
mkdir proj2
printf '[package]\nname = "proj2"\nversion = "0.2.0"\nregistry = "../registry"\n\n[dependencies]\nlib = "0.1"\nshape = "1.2.0"\n' > proj2/quark.toml
cp proj/main.qk proj2/main.qk
quark get proj2
quark run proj2/main.qk
say "a name the registry lacks, and no registry"
printf 'other = "1.0"\n' >> proj2/quark.toml
quark get proj2
sedIn '/^registry/d' proj2/quark.toml
quark get proj2

say "a manifest with a mistake"
printf '[package]\nname = "bad"\nversion = 1\n' > app/quark.toml
quark get app
cd / && rm -rf "$work"
