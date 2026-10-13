# Workflow

## Before writing code

- Read `SECURITY_RULES.md` and the relevant section of
  `docs/cardman-compat/`.
- Verify assumptions in the code or a primary source before relying on
  them. If something is unknown (terminal behavior, CARDMAN contract),
  say so and record it as an open item rather than guessing.
- If a task needs a decision listed in `COMPATIBILITY_PLAN.md` section 2
  that is still open, stop and ask the owner.

## Branches and PRs

- Work on a feature branch; never push to `main` directly.
- One concern per PR (e.g. redaction, CDR durability, tests). Keep the
  upstream delta small and isolated.
- Open PRs only when the owner asks. PR description: what changed, why,
  how it was verified, what remains unverified.

## Commits

- Every commit message explains **what** changed and **why**, and names
  any security-relevant effect.
- Do not commit generated build output, databases (`mm_manager.db`),
  real captures, or credentials.

## Verification before push

1. Full CMake build clean with `-Werror` (all targets).
2. `ctest` passes.
3. For card-path changes: run a playback session against
   `tools/cardman_stub_bridge.py` in approve, `--decline`, and
   bridge-stopped modes, then decode every output (stdout, `-l` hex lines
   reassembled into bytes, pcap payloads, `mm_manager.db` rows) and
   search for the test PAN and PIN in both ASCII and packed-BCD form. A
   plain grep misses packed and line-split data.
4. Re-read the diff against `SECURITY_RULES.md`'s checklist.

Report results honestly: if something was not tested (no hardware, no
live CARDMAN), say so in the PR.

## Upstream sync

Merge the default branch of `hharte/mm_manager` into a feature branch, resolve conflicts
keeping CARDMAN call sites intact, rebuild, rerun the verification
above. Re-review the CARDMAN delta whenever upstream touches
`process_mm_table()`, `mm_accounting.c`, or `mm_proto.c`.
