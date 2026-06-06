# sw eval — empirical pass rate for LLMs writing sw

This directory holds a small benchmark of code-generation tasks that test
whether an LLM can produce working `sw` programs against the published
docs.  Each prompt is a standalone task with a deterministic check; the
program either passes or it doesn't.

The pitch on the front of the repo is that `sw` is **for AI agents**.
That claim deserves real numbers — `eval/` is how we measure them.

## Cold cross-vendor eval (drop keys + run)

This eval is **cold** and **cross-vendor**: nothing is pre-wired to a single
provider, and the system prompt is *de-leaked* — it teaches the language, not
the answers. To run it you only need to export the API key(s) for whichever
vendor(s) you want to light up, then run one command. Any model whose key env
is unset is skipped cleanly; you never need every key.

```bash
# Export only the vendor(s) you want. Each model in models.json reads its own
# key from the named env var; an unset key => that model is SKIPped (no call,
# no cost). Keys are referenced by env only — none are stored in this repo.
export MOONSHOT_KEY=...        # Kimi K2.6 / K2.5 / moonshot-v1-32k (Moonshot)
export OPENROUTER_API_KEY=...  # GPT-4.1 / Gemini 2.5 Flash / DeepSeek / Qwen (via OpenRouter)

cd eval
./check_leakage.sh             # gate: fail-closed if the prompt leaks answers
EVAL_K=5 ./runner.sh           # the eval — 5 samples/cell, pass@1 mean ± stdev
```

- All vendors are plain **OpenAI chat/completions** drop-ins — the runner sends
  one standard body to every endpoint; no per-vendor request branch.
- `EVAL_K` controls samples per (prompt × model) cell — **default 5**; results
  are reported as **pass@1 = mean ± stdev** across the K draws. `EVAL_K=1` gives
  the legacy single-shot behavior.
- The prompt is **de-leaked** and that invariant is enforced by
  `./check_leakage.sh` (exits nonzero if any task solution appears in
  `system_prompt.md`). Run it before trusting a result.
- Add/remove vendors by editing `models.json` (`id`, `label`, `endpoint`,
  `key_env`, `model_field`). Only OpenAI-compatible endpoints are supported.

## Layout

```
eval/
├── README.md            ← you are here
├── system_prompt.md     ← context fed to every model
├── prompts/
│   ├── 01_*.md          ← one task per file; expected verbatim stdout
│   └── ...
├── models.json          ← endpoints, key env-vars, model IDs
├── runner.sh            ← bash driver
└── results/             ← per-run results (gitignored except for results.md)
```

Each prompt file has three sections:

```
## Task
<one-paragraph problem statement>

## Expected output
<verbatim stdout — what the compiled program should print>

## Notes
<optional hints, constraints, allowed builtins>
```

## How to run

See **Cold cross-vendor eval** above for the keys and the one command. In
short: export only the vendor key(s) you want (`MOONSHOT_KEY`,
`OPENAI_API_KEY`, `DEEPSEEK_API_KEY`), then:

```bash
cd eval
./check_leakage.sh                        # answer-leakage gate (fail-closed)
EVAL_K=5 ./runner.sh                       # every prompt × every keyed model
./runner.sh prompts/03_*                  # subset
```

Results land in `results/<timestamp>.md` and the latest run is also
mirrored into `results/results.md` for easy linking from the README.

## Methodology

- K-sampled generation per prompt × model (`EVAL_K`, default 5); reported as
  pass@1 mean ± stdev.  No multi-turn refinement.
- LLM output is parsed for a ` ```sw ` block.  If absent, the whole
  response is treated as code (and usually fails).
- Code is compiled via `swc build` — compile errors count as failures.
- The compiled binary's stdout is normalized (trim trailing whitespace,
  collapse spaces in the SwarmRT init banner) and compared verbatim
  against the prompt's "Expected output" section.
- Wall-clock time per task is recorded for budget tracking, not scoring.

## Scope (current revision)

- 10 prompts, breadth over depth.
- 5 model endpoints, cross-vendor, all OpenAI chat/completions drop-ins:
  `kimi-k2.6` / `kimi-k2.5` / `moonshot-v1-32k` (Moonshot), `gpt-4.1`
  (OpenAI, frontier-closed), `deepseek-chat` (DeepSeek V4, open).  Each only
  runs if its `key_env` is set.  See `models.json` to add more.
- No agent harness, no retries, no tool use.  Pure code-gen.

## What this measures (and what it doesn't)

**Measures:** how well an LLM uses the published docs verbatim to write
syntactically correct, semantically correct `sw` that compiles and runs.

**Doesn't measure:** real-world agent workflows (those need multi-turn,
tool use, state); idiomatic style (we only check stdout); creative
problem decomposition.

A future revision will add (a) compile-error feedback loops, (b) longer
multi-file programs, (c) MCP-tool-use prompts.
