# Packages

`swc` has a small package manager built in. A project lists its dependencies in
`swarm.json`, `swc add` / `swc install` fetch them with git into `.swarm/deps/`,
and `swarm.lock` pins every git dependency to an exact commit so every checkout
of the project builds the same code. `import Foo` then finds `Foo.sw` in an
installed dependency on `swc build` and `swc run` alike.

There's no registry: a package is a git repository (or a local directory) with
`.sw` files in it.

## Quick start

```
$ cd myapp
$ cat swarm.json
{"name": "myapp", "version": "0.1.0"}

$ swc add http_utils https://github.com/acme/sw-http-utils@v1.2.0
swc: http_utils       3f9c2a1b7d04  https://github.com/acme/sw-http-utils@v1.2.0
swc: 1 dependency installed in .swarm/deps/, locked in swarm.lock
swc: added http_utils

$ swc build main.sw          # `import HttpUtils` now resolves from the dependency
```

Commit `swarm.json` and `swarm.lock`. Don't commit `.swarm/`: it's a cache that
`swc install` rebuilds from the lockfile.

## Commands

All of these find the project by walking up from the current directory to the
nearest `swarm.json`, so they work from any subdirectory.

| Command | What it does |
|---|---|
| `swc add <name> <git-url>[@ref]` | Add (or change) a git dependency, fetch it, update the lock. `ref` is a tag, branch or commit; without one the remote's default branch is used. |
| `swc add <name> --path <dir>` | Add a local directory as a dependency (symlinked, so edits show up without reinstalling). |
| `swc install` | Install everything in `swarm.json`. Git dependencies in `swarm.lock` are checked out at exactly the locked commit, whatever the remote's tags and branches say now. Dependencies not in the lock yet (or whose URL/ref changed in the manifest) are resolved and added to it. Alias: `swc deps`. |
| `swc update` | Resolve every dependency's ref again (a branch that moved, a re-pointed tag) and rewrite `swarm.lock`. |
| `swc remove <name>` | Remove a dependency from `swarm.json`, delete its checkout, update the lock. |

If `swc add` or `swc remove` fails (bad ref, network, conflict), `swarm.json` is
put back as it was.

For `url@ref`, the ref is whatever follows the last `@` that comes after the last
`/` and `:`, so the `git@` of `git@github.com:acme/repo.git` is never taken for a
ref.

## `swarm.json`

```json
{
  "name": "myapp",
  "version": "0.1.0",
  "deps": {
    "http_utils": {"git": "https://github.com/acme/sw-http-utils", "ref": "v1.2.0"},
    "shared":     {"path": "../shared"}
  }
}
```

- A dependency is `{"git": "<url>", "ref": "<tag|branch|sha>"}` (`ref` optional)
  or `{"path": "<dir>"}`. A relative `path` is relative to the manifest that
  declares it.
- Dependency names are `[a-z0-9_-]+`, at most 64 characters. The name is the
  directory under `.swarm/deps/`. It doesn't have to match any module name.
- `swc add` / `swc remove` rewrite the file. Fields `swc` doesn't manage
  (`description`, `license`, ...) are kept as they are.

## `swarm.lock`

Written by `swc`, one entry per dependency, sorted by name:

```json
{
  "lock_version": 1,
  "deps": {
    "http_utils": {"git": "https://github.com/acme/sw-http-utils", "ref": "v1.2.0", "sha": "3f9c2a1b7d04..."},
    "shared": {"path": "../shared"},
    "strutil": {"git": "https://github.com/acme/strutil", "ref": "v0.1.0", "sha": "a81e...", "via": "http_utils"}
  }
}
```

`sha` is the full commit id. `via` marks a transitive dependency and names the
package that pulled it in. After checking out a locked commit, `swc install`
checks that `HEAD` is that sha and fails if it isn't.

## How imports find dependencies

For `import Foo`, each directory below is tried as `Foo.sw`, then `foo.sw`:

1. The importing module's own package (its root, then its `src/`) when the
   importer is a dependency's module. Otherwise the directory of the file you're
   building or running.
2. Every installed dependency, `<project>/.swarm/deps/<pkg>/` and its `src/`. If
   two packages both provide `Foo.sw`, that's an error ("import 'Foo' is
   ambiguous"), not a silent pick.
3. The bundled `lib/` (Std, Mcp, Pdf, ...).

The project is the nearest ancestor of the input file that contains
`swarm.json`. A file outside any project resolves imports exactly as before:
its own directory, then `lib/`.

A dependency that ships a module with the same name as a `lib/` module shadows
it (step 2 comes before step 3). `swc install` warns when that happens.

## Transitive dependencies

A dependency with its own `swarm.json` has its dependencies installed too, into
the same flat `.swarm/deps/`: one version per name for the whole project. If
two packages ask for the same name with a different URL, ref or path, install
stops with an error that names both:

```
swc: dependency conflict for 'strutil': http_utils wants https://github.com/acme/strutil@v0.1.0,
     but the project wants https://github.com/acme/strutil@main (dependencies are flat — one version per name)
```

Fix it by making them agree (usually by changing the project's own entry).

## Errors

| Situation | Message |
|---|---|
| no `git` on `PATH` | `git not found in PATH — it is needed to fetch git dependencies` |
| network down, no access, no such repo | `could not fetch '<name>' from <url> — network failure, no access, or no such repository` |
| ref doesn't exist | `bad ref '<ref>' for '<name>': no such tag, branch or commit in <url>` |
| locked commit gone from the remote | `locked commit <sha> of '<name>' is not in <url> any more (history rewritten?) — run swc update to re-resolve` |
| checkout isn't the locked commit | `sha mismatch for '<name>': expected <sha>, checked out <sha>` |
| import not found in a project | `cannot resolve import 'Foo' (...) — if it comes from a dependency, run swc install` |

## Security

- git runs through `fork` + `execvp` with an argument vector, never a shell
  string. Nothing from a manifest or the command line is ever interpreted by a
  shell.
- Before anything reaches git: names must match `[a-z0-9_-]+`; URLs must not
  start with `-`, contain whitespace or control characters, or use the
  `<transport>::` remote-helper syntax (`ext::`, `fd::`); refs must match
  `[A-Za-z0-9._/+-]+`, not start with `-`, and not contain `..`. git is also run
  with `protocol.ext.allow=never` and `protocol.fd.allow=never`.
- `GIT_TERMINAL_PROMPT=0`: a private repository without credentials fails
  instead of waiting for a password.
- Installing a dependency runs none of its code. Only `swc build` / `swc run`
  of a program that imports it does.

## Not yet

- No registry, no semver ranges. A dependency is one git ref.
- One version per name. Conflicts are errors, not resolved.
- Not supported on Windows yet (`swc add`/`install` say so; imports without a
  project work as before).
- `swc test` doesn't resolve imports, with or without packages.
