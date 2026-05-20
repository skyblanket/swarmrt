# sw eval — baseline run 20260520_233118

_2026-05-20 23:50:47 IST · 10 prompts × 3 models · single-shot, temperature 1, no retries_

## Pass rate by model

| Model | Pass | Fail | Rate |
|---|---:|---:|---:|
| Kimi K2.6 (reasoning) | 2 | 8 | 20% |
| Kimi K2.5 (reasoning) | 2 | 8 | 20% |
| Moonshot v1 32K (non-reasoning baseline) | 1 | 9 | 10% |

## Per-prompt × per-model

| Prompt | Kimi K2.6 | Kimi K2.5 | Moonshot v1 32K |
|---|:---:|:---:|:---:|
| 01_hello_world | ✓ | ✓ | ✓ |
| 02_fizzbuzz | ✗ | ✗ | ✗ |
| 03_list_sum | ✗ | ✗ | ✗ |
| 04_json_parse | ✓ | ✓ | ✗ |
| 05_actor_counter | ✗ | ✗ | ✗ |
| 06_case_dispatch | ✗ | ✗ | ✗ |
| 07_sqlite_crud | ✗ | ✗ | ✗ |
| 08_pipe_filter | ✗ | ✗ | ✗ |
| 09_fault_tolerance | ✗ | ✗ | ✗ |
| 10_http_pipeline | ✗ | ✗ | ✗ |

_See `results/20260520_233118/<prompt>/<model>/` for each attempt: `program.sw`, `compile.log`, `actual.txt`, `expected.txt`, `diff.txt`._

## Headline finding

**Pass rate is low.** Three models, ten prompts, only two prompts cleared
any model: the hello-world smoke test (3/3) and the basic JSON parse
(2/3). Everything that touched `case` expressions, processes, the pipe
operator with module-prefixed functions, or list patterns failed across
the board.

This is the honest floor. Reasoning models hover around 20%, the
non-reasoning baseline around 10%. The bottleneck is **not** model
capability for the easy prompts — all three nailed hello-world. It's
that sw has language quirks and codegen bugs that aren't visible from
the docs, and a single-shot generation gives the model no chance to
recover.

## Failure taxonomy

The failures cluster cleanly into six causes, each tied to a real
language or codegen gap. The first three are LLM hallucinations the
docs should preempt; the last three are bugs the eval exposed.

### Docs-fix issues (LLM hallucinated reasonable-sounding syntax)

1. **F-string `f` prefix omitted.** Models wrote `print("hi {x}")`
   expecting interpolation. sw requires the explicit `f"hi {x}"` prefix
   (or `format("hi {}", x)`). System prompt was ambiguous —
   fixed in the next revision. Tripped up `03_list_sum`, `07_sqlite`,
   most prompts that produced output via interpolation.

2. **Erlang head-tail list pattern `[h | t]`.** Models trained on
   Elixir/Erlang reach for this; sw doesn't support it. Supported
   patterns are `[]`, `[a, b, c]` (fixed-length), and `_`. Killed
   `06_case_dispatch` for the reasoning models.

3. **Erlang BIF guards (`is_integer`, `is_atom`, etc.).** Not in sw —
   use `typeof(x) == "int"`. Same prompt as above.

### Language/codegen bugs surfaced

4. **Nested `case` as RHS.** `out = case x { _ -> 1 }` parses fine at
   the top of a fun body but fails inside any nested block (case arm,
   bare block). Repro at `/tmp/test_nested_case.sw`. Killed
   `02_fizzbuzz` for kimi-k2.6, which wrote idiomatic-looking
   nested-case code.

5. **Pipe + module-prefix codegen.** `x |> Std.sum()` after a pipe is
   codegened as `Main_Std.sum(...)` — the current module name is
   prepended to the qualified call. Killed `08_pipe_filter` for
   kimi-k2.6.

6. **`Std.map` lambda arity mismatch.** When a lambda passed to
   `Std.map` doesn't return a value, codegen emits the wrong arity
   (`Std_map(_t27, 2)` — int 2 where a fn pointer is expected).
   Killed `07_sqlite_crud` for kimi-k2.6.

## Model observations

- **Reasoning models (`kimi-k2.6`, `kimi-k2.5`)** burn 60–110 sec per
  generation on the chain-of-thought, often producing well-structured
  code that hits one of the six issues above. Pass rate is the same
  between K2.6 and K2.5 in this run — the language quirks dominate, not
  the model.
- **Non-reasoning baseline (`moonshot-v1-32k`)** is ~30× faster (1–3 sec
  per call). One pass (hello-world); even json_parse fell to a
  hallucinated `(1 == 2)` placeholder list. Confirms reasoning helps
  for trivial prompts but doesn't rescue any model from unfamiliar
  language pitfalls.

## What this implies

The eval has done its job. Two new bugs are filed (parser nested case,
codegen pipe + module-prefix) and one codegen miss (`Std.map` lambda)
is queued. The next revision of `system_prompt.md` will lead with the
f-string `f` prefix, call out the unsupported list patterns, and
pre-empt the Erlang BIF guards.

Re-running the same suite against the patched system prompt + fixed
parser/codegen should move the reasoning models from 20% → meaningfully
higher, and is the right way to track progress between revisions of
the language.

## Re-run baseline

```bash
export MOONSHOT_KEY=...                    # or whichever endpoint
cd /Users/sky/swarmrt/eval
./runner.sh                                # all 10 × all 3, ~20 min
./runner.sh prompts/03_list_sum.md         # subset
```

Results land in `results/<run_id>/` with per-attempt artifacts; the
latest summary is mirrored to `results.md` (this file).
