# {{name}}

An agent project made with `swc new`. Each task runs as its own supervised
process with a deadline, so an agent that crashes or hangs costs its own
result, not the run.

```sh
make test                                   # offline: a local server plays the model
LLM_PROVIDER=ollama LLM_MODEL=qwen2.5 make run   # or LLM_URL=... LLM_API_KEY=...
```

| File | What it is |
|---|---|
| `agent.sw` | One agent: asks the model, runs a tool when asked, returns the answer. |
| `tools.sw` | The tools an agent may call (`word_count`). Add yours here. |
| `main.sw` | Fans `tasks.txt` out over `Std.task_stream`: 8 at a time, 30 s each. |
| `agent_test.sw` | Offline tests against a mock model, including a crashing and a hanging agent. |

Nothing is sent anywhere until you set `LLM_URL` or `LLM_PROVIDER`.
