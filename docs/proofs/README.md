# Proofs about a model of the GB20B start-up

`AienGpuGb20b.lean` holds 22 Lean 4 theorems about a hand-written model of four pieces of this
repository: the SEC2 reply size with its 32-bit wrap, the EMEM pointer encoding, the GB20B
work-submit token, and the order of the start-up steps (no GSP register before the SEC2 reply is
acknowledged). Three of them are negative controls: the GB10B token has bit 30 clear, a size check
of 1 instead of 4 admits a reply of zero dwords, and a trace that skips the reply breaks the order
invariant.

What it is not: the model is not generated from `om/*.om` or the C, so the proofs do not fail when
the code changes, and they say nothing about the chip. The tests in `tests/host` check the code.

Check it (Lean 4.35.0, core only, no imports, about 2 s):

```sh
lean docs/proofs/AienGpuGb20b.lean
```

Exit status 0 and no `error` or `sorry` line means every theorem checked. The file ends with
`#print axioms` for 19 of the theorems; the output names only `propext`, `Quot.sound` and
`Classical.choice`, or no axiom.
