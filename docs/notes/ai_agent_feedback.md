# SwarmRT AI Agent Feedback Report — RETIRED

**Original date:** 2026-03-02
**Retired:** 2026-06-02

This report has been retired. Its headline findings ("all compiled programs
hang forever after `main()`", "DX Grade C+", "too unreliable for production",
"no REPL", "print adds a literal 'n' instead of a newline", panicking programs
hang) were accurate against the March commit but are **stale and false against
current `main`**:

- Compiled programs exit cleanly when `main` returns.
- A panicking process now exits non-zero with a stack trace (verified
  2026-06-02: `panic("boom")` exits 1).
- `swc repl` ships (`swc` lists `build`, `emit`, `repl`, `test`).
- `print` emits a real newline.

The two diagnoses that were **still true** when re-verified against the shipped
`bin/swc` have been folded into the honest limitations list in
[KNOWN_ISSUES.md](KNOWN_ISSUES.md):

- Undefined variables compile to atoms rather than erroring (no static checking).
- There is no `swc run` subcommand.

For the full external-review hardening narrative, see
[REVIEW_HARDENING.md](REVIEW_HARDENING.md).
