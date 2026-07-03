# SwarmRT — Versioning & Releasing

## Versioning (SemVer)

SwarmRT follows [Semantic Versioning](https://semver.org): `MAJOR.MINOR.PATCH`.
The single source of truth is the top-level [`VERSION`](../VERSION) file; the
build injects it as `-DSWARMRT_VERSION` and `swc --version` reports it.

What each bump means for the two compatibility surfaces — the **`sw` language**
and the **embedder C ABI** (`libswarmrt.a` + public headers):

- **MAJOR** — a breaking change to either surface: `sw` source that compiled on
  the previous major may no longer compile or may change behavior; or the C ABI
  changed incompatibly (a struct layout, a removed/renamed exported symbol, a
  changed signature). Migration notes are required in `docs/CHANGELOG.md`.
- **MINOR** — backward-compatible additions: new builtins, new stdlib modules,
  new C API, new env-var knobs. Existing `sw` programs and embedders keep
  working unchanged.
- **PATCH** — backward-compatible bug fixes and internal changes only.

Pre-release tags use a suffix: `1.0.0-rc.1`, `1.0.0-beta.1`. A pre-release is
not covered by the stability guarantees above and is published as a GitHub
*prerelease*.

### Compatibility notes specific to SwarmRT

- **No package registry is a feature, not a gap.** There is no Hex/npm-style
  dependency resolution; a program is `swc build`-ed from source you control.
  So "dependency compatibility" is not a versioned surface.
- **`sw` has no stable serialization format across majors.** Distribution wire
  frames and the on-disk obfuscation format may change on a MAJOR; a running
  cluster should run one runtime version.
- **SQLite is the durable boundary.** State an agent persists via `db_*`
  survives across runtime versions (it's just SQLite); ETS, mailboxes, and
  live PIDs do not (see `docs/DEPLOYMENT.md`).

## Cutting a release

1. **Green everything on `main`** — the full gate suite must pass (the CI
   `Release Gates` are the authority, but run locally before tagging):
   ```
   make swc libswarmrt          # zero warnings
   make test-sw                 # sw suite + interp/compiled conformance
   make gc-stress gc-slope      # ownership: safety + bounded memory
   make tsan-gate lsan-gate     # races (Linux) + leaks (Linux)
   make alloc-fault             # OOM-injection safety
   make fuzz                    # all untrusted-input boundaries
   make quota-gate msgsize-gate slowloris-gate isolation-gate
   make crashlog-gate health-gate shutdown-gate
   for p in 2 3 4 5 6 7 8 9 10; do make phase$p && ./bin/test-phase$p; done
   ```
   Plus the sign-off items in `docs/PRODUCTION_ROADMAP.md` Phase 5: a **24-hour
   soak** on a dedicated Linux host (`SOAK_SECONDS=86400 SOAK_RSS_BUDGET_MB=512
   ./tests/soak/run_soak.sh`) and an **independent adversarial review**.

2. **Bump `VERSION`** to the release version (drop the `-rc.N` suffix for a
   final), and add a `docs/CHANGELOG.md` entry (newest first) with any
   migration notes. Commit.

3. **Tag and push:**
   ```
   git tag v$(cat VERSION)
   git push origin v$(cat VERSION)
   ```
   The `Release` workflow (`.github/workflows/release.yml`) then builds `swc` +
   `libswarmrt.a` on macOS-arm64, Linux-x86_64, and Linux-arm64, verifies the
   tag matches `VERSION` and that `swc --version` reports it, packages each
   with the public headers, checksums everything, and publishes the GitHub
   Release (marked *prerelease* automatically for `rc`/`alpha`/`beta` tags).

## Release gates (what "1.0.0" requires)

From `docs/PRODUCTION_ROADMAP.md` Phase 5, a final `1.0.0` requires, on top of
the always-green gate suite: no known P0/P1; all memory slopes bounded; the 24h
soak passing on a native Linux host; native-Linux stress passing repeatedly; no
unexplained compiler warnings; and a completed independent runtime/security
review (folding in the deferred clean ETS re-audit). Until those sign-offs
land, the version stays a release candidate (`-rc.N`).
