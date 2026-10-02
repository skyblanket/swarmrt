# Security audit summary — September 2026

An internal review of the runtime, the compiler, the builtins and the release,
done before the public launch. Each finding below was reproduced, fixed on
`main`, and has a regression test that fails on the old code. This is not an
external audit; see [SECURITY.md](../SECURITY.md) to report a vulnerability.

## Threat model

SwarmRT runs untrusted **input**, not untrusted **code**: a compiled program has
the authority of the user who runs it. So the questions were:

1. Can input a program handles (HTTP requests, WebSocket frames, distribution
   frames, JSON, PDFs, model output) corrupt memory, crash the runtime or run
   commands?
2. Do the builtins do only what they say, with no hidden network traffic,
   credentials sent to the wrong host, or shell interpolation of data?
3. Are the defaults safe for a program that sets nothing?

## Findings and fixes

| Area | Finding | Fix | Gate |
|---|---|---|---|
| Sandbox | `shell_sandboxed` pasted the command into an outer `sh -c '…'`, so a quote escaped the sandbox. | The sandbox tool is exec'd with the command as one argv element. | `make test-injection` |
| LLM builtins | `llm_stream` built a shell string from the URL and API key, so a quote in either was command injection. | curl is exec'd with an argv; the key goes in a 0600 header file, out of `ps`. | `make test-injection` |
| LLM builtins | A provider key was sent to whatever URL was in effect, and then to any URL *containing* the provider's host (`https://evil.example/?://api.openai.com/`). | Keys go only to the parsed `https` host of their provider, with no userinfo. | `test_llm_endpoint.sw` |
| LLM builtins | With nothing configured, prompts went to a hosted vendor endpoint. | No default endpoint: `LLM_URL` or an explicit provider is required, and the error says so on stderr. | `test_llm_endpoint.sw` |
| Distribution | `node_start` listened on every interface, unauthenticated, and delivered the wire's message tag verbatim (a peer could forge runtime-internal EXIT/DOWN signals). | Binds 127.0.0.1 by default; optional `SW_NODE_COOKIE` SHA-256 MAC per frame; only plain value messages cross the wire. | phase tests, `test_pid_reuse.sw` |
| Processes | A pid held only a slab pointer, so a stale pid reached whatever process reused the slot (`send`, `exit_proc`, `monitor`). | Pids carry the numeric id; a stale pid is dead. | `test_pid_reuse.sw` |
| HTTP server | `http_listen` bound every interface; a malformed bind address silently meant "all". | Binds 127.0.0.1 unless `bind:` / `SW_HTTP_BIND` says otherwise; a bad address is an error. | `test_http_bind.sw` |
| WebSocket | Upgrades ignored `Origin`, so any web page could open a socket to a local server (cross-site WebSocket hijacking). | Non-loopback, cross-origin, non-allowlisted Origins get 403. | `test_ws_origin.sw` |
| HTTP server | A handler-side close freed a connection's buffers while the bridge was parsing them (double free, SIGSEGV under churn); a closed fd could be reused under an in-flight read. | Close-once handoff under the table lock; the IO thread frees fds and port structs between event batches, with refcount pins. | `test_http_port_free.sw`, `tsan-gate`, fuzz |
| HTTP server | `ws_request_path` returned a pointer a concurrent close could free. | Copied under the connection lock. | `test_ws_headers.sw` |
| HTTP server | The bridge process kept ~1.7 KB per request forever (memory exhaustion by request volume). | The bridge rewinds its heap per event. | `test_http_port_free.sw` |
| JSON | `json_decode` accepted truncated input as complete (a cut-off tool call still ran), and `json_encode` could emit invalid JSON. | Strict decode (nil on unterminated input or trailing data); encode always emits valid JSON. | `test_json_strict.sw` |
| Batteries | Every binary carried the hand-written PDF parser (~3.2K lines over untrusted input). | PDF, Chrome and the audio codecs are compiled in only when a module imports them. | `test_batteries.sw` |
| Packages | Dependency fetches run git on user-supplied names, URLs and refs. | argv-only exec (no shell); names, URLs (no leading `-`, no `ext::`/`fd::`), refs and shas validated; `protocol.ext` / `protocol.fd` disabled; the lockfile pins commit shas and the checkout is verified against them. | `make test-pkg` (14 injection attempts refused) |
| Packages | A dependency's internal module could be silently replaced by a same-named project module. | A conflict error naming both files. | `make test-pkg` |

## Continuous checks

Every change to `main` runs: the sw suite and interpreter/compiled conformance,
the C phase suites, the 80K-spawn stress test, gc-stress under ASAN and UBSAN,
LSan and TSan gates, allocation-failure injection, the shell-injection probe,
libFuzzer targets for the parser, HTTP and WebSocket frame parsers, the
package-manager tests, and an install smoke test run from outside the checkout.

## Known limits

- **DNS rebinding.** The WebSocket same-origin rule trusts `Host`. A server that
  must resist rebinding should validate `Host` itself.
- **Distribution is authenticated, not encrypted,** and not replay-protected.
  Run clusters on a private network or through a tunnel.
- **No code sandbox.** A compiled program can do anything its user can;
  `shell_sandboxed` contains only the command passed to it.
- **Durable state** is stored unencrypted in SQLite (`SW_STATE_DB`), in native
  byte order. Treat the file like any application data.
- **HTTP handler fields** (a connection's mode and handler) are read without the
  table lock. Pipelined requests are parsed one per incoming chunk.
