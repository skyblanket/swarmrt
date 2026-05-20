# sw eval — empirical pass rate for LLMs writing sw

This directory holds a small benchmark of code-generation tasks that test
whether an LLM can produce working `sw` programs against the published
docs.  Each prompt is a standalone task with a deterministic check; the
program either passes or it doesn't.

The pitch on the front of the repo is that `sw` is **for AI agents**.
That claim deserves real numbers — `eval/` is how we measure them.

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

```bash
# Set keys:
export OTONOMY_KEY=otonomy-sk-...        # for otonomy-orc/swarm
export GEMMA_KEY=...                      # for gemma.otonomy.ai

cd eval
./runner.sh                               # runs every prompt × every model
./runner.sh prompts/03_*                  # subset
```

Results land in `results/<timestamp>.md` and the latest run is also
mirrored into `results/results.md` for easy linking from the README.

## Methodology

- Single-shot generation per prompt × model.  No multi-turn refinement.
- LLM output is parsed for a ` ```sw ` block.  If absent, the whole
  response is treated as code (and usually fails).
- Code is compiled via `swc build` — compile errors count as failures.
- The compiled binary's stdout is normalized (trim trailing whitespace,
  collapse spaces in the SwarmRT init banner) and compared verbatim
  against the prompt's "Expected output" section.
- Wall-clock time per task is recorded for budget tracking, not scoring.

## Scope (current revision)

- 10 prompts, breadth over depth.
- 3 model endpoints — `kimi-k2.6` (Moonshot, reasoning), `kimi-k2.5`
  (Moonshot, reasoning, prior version), `moonshot-v1-32k` (Moonshot,
  non-reasoning baseline).  See `models.json` to add more.
- No agent harness, no retries, no tool use.  Pure code-gen.

## What this measures (and what it doesn't)

**Measures:** how well an LLM uses the published docs verbatim to write
syntactically correct, semantically correct `sw` that compiles and runs.

**Doesn't measure:** real-world agent workflows (those need multi-turn,
tool use, state); idiomatic style (we only check stdout); creative
problem decomposition.

A future revision will add (a) compile-error feedback loops, (b) longer
multi-file programs, (c) MCP-tool-use prompts.
