#!/usr/bin/env bash
# Writes compiler/stdlib.qk: the standard library (std/*.qk) and the runtime (compiler/runtime/*.qk) as texts kept in
# the compiler written in Quark, as build.sh puts std/ into the C one: `use std.json` reads no file, and quark run and
# quark build compile the runtime from the texts the compiler carries. Run it when either changes (build.sh does).
cd "$(dirname "$0")/.." || exit 1
close='""" + "\n"' # a raw text leaves out its last line break: added back
texts() { # the files of a folder, each a raw text by its name
    for f in "$1"/*.qk; do
        echo "    if name == \"$(basename "$f" .qk)\" {"
        echo '        return r"""'
        cat "$f"
        echo "$close"
        echo "    }"
    done
    echo "    None"
    echo "}"
}
{
    echo "// Made by compiler/embed.sh from std/*.qk and compiler/runtime/*.qk: the standard library and the runtime, as"
    echo "// the compiler keeps them (module.qk reads them; memory.qk compiles the runtime from them)."
    echo
    echo "// compiler/prelude.qk: the built-ins written in Quark"
    echo "pub fn preludeText() -> Text ="
    echo '    r"""'
    cat compiler/prelude.qk
    echo "$close"
    echo
    echo "// std/NAME.qk's text"
    echo "pub fn stdText(name: Text) -> Text? {"
    texts std
    echo
    echo "// the runtime's files"
    echo "pub const runtimeFiles: [Text] = [$(for f in compiler/runtime/*.qk; do printf '"%s", ' "$(basename "$f" .qk)"; done | sed 's/, $//')]"
    echo
    echo "// compiler/runtime/NAME.qk's text"
    echo "pub fn runtimeText(name: Text) -> Text? {"
    texts compiler/runtime
} > compiler/stdlib.qk.tmp && mv compiler/stdlib.qk.tmp compiler/stdlib.qk
