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

## Known issues

Non-security stability bugs are tracked openly in
[docs/notes/KNOWN_ISSUES.md](docs/notes/KNOWN_ISSUES.md) — for example a
high-process-count message-send race on Linux x86_64. Those are already
public; this policy is for *undisclosed* vulnerabilities.
