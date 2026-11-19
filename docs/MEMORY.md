# Memory: the arena, and the counted heap tried beside it

Quark's memory is an arena with loop collection ([LANGUAGE.md](LANGUAGE.md#memory)): allocating bumps a pointer,
a call's memory goes at once when it ends, and a loop that makes things copies out what its vars still hold. That
is the model every program uses.

Beside it, behind `QUARK_COUNTED=1`, is a counted heap (reference counting, after Perceus): what a value points to
has a count of its holders, a value is shared and copied only when a holder changes it in place while others hold it
(copy on write), and what nobody holds goes back to the heap at once. It was built to decide, by measurement,
whether it should replace the arena.

## The decision (2026-11-17)

The arena stays. Measured on `bench/` (`bash tools/bench.sh`, milliseconds, `--release`):

| benchmark | arena | counted | C -O2 |
| --- | --- | --- | --- |
| records (arrays in records, changed in place) | 271 | 105 | 63 |
| trees (allocation, recursion) | 466 | 479 | 1611 |
| json | 420 | 446 | |
| counting, sorting, pipeline, mandelbrot | the same | the same | |
| strings | 971 | 1026 | 477 |
| words (many small texts, slices) | 758 | 1206 | 359 |

Counting wins clearly only where loops change long-lived data in place (records), loses where many small texts are
made and dropped, and ties elsewhere. Its code is about four times bigger (the counts are read and written in the
code), and the compiler itself, compiled counted, still goes wrong on some inputs: the test suite passes counted
(845 of 847, two leaks under `--memory`), but a program of the compiler's size finds more. So it does not win
clearly, and the plan's rule keeps the arena.

What came out of the work for everyone: a `let` read out of a `var` is copied only while it is still used after a
change in place (records went from 7.7 to 4.1 times C's time on the arena), and descriptions of types that hold
themselves through a box no longer send the collector round for ever (`skipDesc`).

Later (2026-11-18), the arena's loop collections got cheaper on their own: what a collection copied is found again
through a side table indexed by address (16 bytes for each 16 of a young range, kept by the arena from one
collection to the next, each entry marked with its collection's epoch), not a hash table scattered over memory.
records went from 264 to 127 ms (C: 59), json from 465 to about 437.

## The counted heap, as it is

- `compiler/runtime/heap.qk`: one heap for each thread, a range of memory reserved at once and handed out in 64 KB
  pages; a small page holds blocks of one size, with their counts beside them (a slice that points inside a block
  finds its count); a large block takes pages of its own. Blocks given back go on their size's list.
- `compiler/runtime/counted.qk`: one holder more (`rtDup`), one fewer (`rtRelease`: what reaches 0 lets go of what
  it holds, by the type's shape, from a list rather than by deeper calls), copy on write (`rtOwnArray`, `rtOwnMap`).
- `compiler/lower.qk` (`LowerOptions.counted`): values held where they are kept (`lowerHeld`), what a statement
  makes let go of when it ends (`temps`), names let go of where their block is left (`DeferAt.release`), writes that
  let go of the old value, a `drop` function for each type, copy on write before every change in place, the check
  made once before a loop that never shares the var (`hoistOwned`).
- `QUARK_VALIDATE=1` (with `QUARK_COUNTED=1`): every count goes through the runtime, blocks given back are never
  handed out again, and the first block counted or let go of after it went back stops the program with the line
  that let go of it and the line that used it.

To try it: `QUARK_COUNTED=1 quark run --release program.qk`. What is open: the compiler compiled counted (its
failures depend on the optimizing tier: the code's own reads of the heap's words and the optimizers' assumptions
about what a runtime call changes), the size of the code, and the leaks under `--memory` (maps' old blocks, tasks).
