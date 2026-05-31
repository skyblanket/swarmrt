<!-- Thanks for contributing to SwarmRT. -->

## What this changes

<!-- A short description of the change and the motivation. -->

## Related issue

<!-- e.g. "Closes #12" — or "n/a" -->

## Checklist

- [ ] `make test-sw` passes
- [ ] `make test-phase{2..9}` pass
- [ ] `make stress` completes all runs (if the change touches the scheduler / arena / mailbox path)
- [ ] Touches the scheduler, arena, or mailbox path? Ordering explained below.
- [ ] Compiler ↔ interpreter parity kept (new builtins reachable from both)
- [ ] New runtime behaviour noted in `docs/CHANGELOG.md`

## Notes for reviewers

<!-- Concurrency ordering, platform assumptions, new dependencies, anything subtle. -->
