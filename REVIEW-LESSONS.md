# Review lessons

Read this before sending a change of this repository to review, and run `make check` on a fresh
clone first. The reviewer is the last check, not the first. Each kind below was found by reviewers
more than once; each now has a gate, so the next case fails the build instead of a review.

## Root cause and corrective action, 2026-11-19 (branch fix/gb20b-sec2-reply)

Review runs of that change: 1 (head 4955ab8, 14 findings), 2 (head 3a45c36, the reviewer program
exited on a credit error before reading anything), 3 (head 3a45c36, 11 findings). A fourth run was
due, so the findings of all runs were sorted into kinds, with why each got past its author.

| Kind | Findings | Why it reached review | Gate that catches it now |
|---|---|---|---|
| A limit that no test presses on | r1: 1, 11. r3: 3 | The scenarios and the mutants were chosen by hand, around the cases the author thought of. A hand-picked list has the author's blind spots. | `tests/host/boundary.sh`: for every comparison line of a unit, every literal is changed by +1 and by -1 and a test must fail. A survivor is a missing test, or is listed in `tests/host/boundary_equivalent.txt` with its reason; a stale entry fails too. First run on `om/boot.om`: 18 of 94 survived, 16 more than review had found. |
| A statement nothing kept backs | r1: 5, 6, 7, 9, 12, 13. r3: 1, 2, 4, 9 | Wording was checked by rereading. When one instance was corrected the others of its kind were not searched for (the "every piece is pinned" claim was fixed in a document and left in the script header; "Wolfram verified" was removed in one sentence and left in another). | `tools/claims-lint.sh`: quoted repository paths must exist; receipts named in `docs/STATUS.md` must be files of `docs/calc-receipts/`; `docs/evidence/` must match its SHA256SUMS; a new line that credits Wolfram with a result fails unless it says the result was not kept. `--selftest` plants one fault of each kind and requires a failure. Evidence a document cites is copied into `docs/evidence/`. |
| A gate that leans on something outside the clone | r1: 4, 14. r3: 1, 11 | The gates were run in the author's working copy, where the oracle clone, the file modes and NVIDIA's tree already existed. | The oracle is a branch of this repository (`oracle/aienux-c9678d7b`); `tools/nvsrc-pin.sh` pins the 58 files of NVIDIA's tree the gates read; `tools/package.sh` pins every input, the system information included, with a control in `tests/host/plan_test.sh`. The pre-review run is on a fresh clone, not the working copy. |
| A control that does not do what its description says | r1: 9 | The description was written from intent. The control was never made to fail on purpose. | Each control is run both ways before it is described: `tools/ko-units.sh` proves its own flip is one bit; `tools/claims-lint.sh --selftest` plants its faults. Rule for authors: run the failing case and paste its output into the commit message. |
| One reading of the source | r1: 12. r3: 5 | Reference, simulator and port come from one lane. | No gate can close this. It is stated in `docs/STATUS.md`; reviewers are asked to read the driver themselves; NVIDIA's own log on the chip is kept in `docs/evidence/` where it speaks to the question. |

Findings not of a repeating kind, each answered where it was raised: the interface version (r1: 3),
decision logic in the C builder (r1: 8), the mutant line that moved (r1: 10), and the notes of r3
(6, 7, 8, 10).

## Before review

1. `git clone` the branch somewhere new and run `make OMC=<pinned omc> check` there. Nothing copied in.
2. For every changed comparison in an Omega unit, the unit must be in `BOUNDARY` of the Makefile.
3. For every claim added to a document or a commit message: where is the file, the line or the
   command that shows it? If it is outside the repository, copy it into `docs/evidence/`.
4. For every control added to a test or a script: run it against the fault it is for, and against no
   fault.
5. A finding names one case. Search for the others of its kind (`git grep`), fix them all, and say in
   the commit how the search was done.
6. Read the result of a check before the next command uses it. (5ef7577 was pushed with a failing
   `make static` because the command went on to commit without stopping.)
7. Pick the reviewer with `aien-pick`; it knows which seats answer. (Review run 2 was lost to a seat
   that the board had shown as down for an hour.)

## Known debt, with the gates

- `tests/host/boundary.sh` runs on `om/boot.om` and `om/ident.om`. On `om/abi.om`, `om/rmarg.om` and
  `om/rmchk.om` it reports 14, 16 and 29 survivors (measured 2026-11-19), all on lines older than
  this change; the lines this change wrote in them are killed. They are not in the gate until tests
  for them exist. Ledger item on spark-b87b: L1336.
- `tools/claims-baseline.txt` lists the 11 lines, older than this change, that credit Wolfram with a
  constant without a kept result. They are tolerated, not correct: replace each with a receipt and
  delete its line from the baseline.
- sparse is not installed on spark-b87b; `make static` says `STATIC PASS-PARTIAL sparse NOT_RUN`.
