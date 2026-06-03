# sw eval — run 20260603_024833

_2026-06-03 03:01:54 IST_

## Pass rate by model

_Each (prompt × model) cell sampled K=1 time(s) at temperature 1._
_**pass@1** is the mean ± population-stdev of the per-cell pass@1 fraction across prompts;_
_**Rate** is the raw aggregate over all 1·prompts samples._

| Model | Samples | Pass | Fail | Rate | pass@1 (mean ± stdev) |
|---|---:|---:|---:|---:|:---:|
| Kimi K2.6 (reasoning) | 10 | 9 | 1 | 90% | 90% ± 30% |
| Kimi K2.5 (reasoning) | 10 | 9 | 1 | 90% | 90% ± 30% |
| Moonshot v1 32K (non-reasoning baseline) | 10 | 3 | 7 | 30% | 30% ± 46% |
| OpenAI GPT-4.1 (via OpenRouter) | 10 | 9 | 1 | 90% | 90% ± 30% |
| Anthropic Claude Sonnet 4.5 (via OpenRouter) | 10 | 10 | 0 | 100% | 100% ± 0% |
| Google Gemini 2.5 Flash (via OpenRouter) | 10 | 9 | 1 | 90% | 90% ± 30% |
| DeepSeek V3.1 (open, via OpenRouter) | 10 | 7 | 3 | 70% | 70% ± 46% |
| Qwen 2.5 72B (open, via OpenRouter) | 10 | 7 | 3 | 70% | 70% ± 46% |

## Per-prompt × per-model

| Prompt | kimi-k2.6 | kimi-k2.5 | moonshot-v1-32k | gpt-4.1 | claude-sonnet-4.5 | gemini-2.5-flash | deepseek-v3.1 | qwen-2.5-72b |
|---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| 01_hello_world | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| 02_fizzbuzz | ✓ | ✓ | ✗ | ✓ | ✓ | ✗ | ✓ | ✓ |
| 03_list_sum | ✓ | ✓ | ✗ | ✓ | ✓ | ✓ | ✓ | ✓ |
| 04_json_parse | ✓ | ✓ | ✗ | ✓ | ✓ | ✓ | ✓ | ✗ |
| 05_actor_counter | ✓ | ✓ | ✗ | ✓ | ✓ | ✓ | ✓ | ✓ |
| 06_case_dispatch | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| 07_sqlite_crud | ✓ | ✓ | ✗ | ✓ | ✓ | ✓ | ✗ | ✗ |
| 08_pipe_filter | ✓ | ✗ | ✗ | ✓ | ✓ | ✓ | ✗ | ✗ |
| 09_fault_tolerance | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| 10_http_pipeline | ✗ | ✓ | ✗ | ✗ | ✓ | ✓ | ✗ | ✓ |

_See `results/20260603_024833/<prompt>/<model>/` for each attempt:_
_`program.sw` (generated code), `compile.log`, `actual.txt`, `expected.txt`, `diff.txt` (if mismatch)._
