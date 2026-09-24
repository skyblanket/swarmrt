# Security Policy

## Supported versions

SwarmRT is pre-1.0 and ships from `main`. Security fixes land on `main`;
there are no separately maintained release branches.

## Reporting a vulnerability

Please **do not open a public issue** for a security vulnerability.

Use GitHub's private reporting instead: go to the **Security** tab of
this repository and choose **"Report a vulnerability"**. That opens a
private advisory visible only to the maintainers.

When reporting, include:

- the affected component (runtime, compiler, a specific builtin),
- a minimal `.sw` program or C reproducer,
- the platform and compiler (`uname -a`, `cc --version`),
- the impact you observed.

We aim to acknowledge a report within a few days and will keep you
updated as we work on a fix.

## Scope

SwarmRT runs untrusted *input* but is not a sandbox for untrusted
*code* — a compiled `.sw` program has the full authority of the user
who runs it (it can shell out, open sockets, and read the filesystem).
Treat `swc build` of an untrusted `.sw` file the same as running any
untrusted program. Reports about a compiled program doing what its
source plainly says are out of scope.

In scope: memory-safety bugs in the runtime or compiler, crashes
reachable from well-formed `sw` source, and `shell_sandboxed` failing
to contain what it documents.

## Security-relevant defaults

- **Distribution** (`node_start`) listens on `127.0.0.1` unless `SW_NODE_BIND`
  says otherwise. Set the same `SW_NODE_COOKIE` on every node to authenticate
  frames (SHA-256 MAC per frame; frames are not encrypted or replay-protected,
  so run clusters on a private network or through a tunnel). A remote peer can
  only deliver plain value messages — never runtime-internal signals.
- **LLM builtins** send a provider key only to that provider: `LLM_API_KEY` to
  any URL, `OPENAI_API_KEY` only to `api.openai.com`, `OTONOMY_API_KEY` only to
  the Otonomy endpoint.
- **`shell_sandboxed`** execs the sandbox tool directly; the command string is
  interpreted only by the shell *inside* the sandbox.
- **The HTTP server** (`http_listen`) binds all interfaces (it is meant to be
  reachable, e.g. for health checks); WebSocket upgrades do not check `Origin`.

## Known issues

Non-security stability bugs are tracked openly in
[docs/notes/KNOWN_ISSUES.md](docs/notes/KNOWN_ISSUES.md). Those are
already public; this policy is for *undisclosed* vulnerabilities.
