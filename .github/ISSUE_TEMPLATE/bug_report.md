---
name: Bug report
about: A crash, wrong behaviour, or build failure in the runtime or compiler
title: ''
labels: bug
assignees: ''
---

## What happened

<!-- What you observed, including any crash output or stack trace. -->

## What you expected

<!-- What should have happened instead. -->

## Reproducer

<!-- The smallest .sw program (or C snippet) that triggers it, and the
     exact commands you ran. -->

```sw

```

```
$ ./bin/swc build repro.sw -o repro && ./repro
```

## Environment

- OS / arch: <!-- e.g. macOS 15 arm64, Ubuntu 24.04 x86_64 -->
- Compiler: <!-- output of `cc --version` first line -->
- SwarmRT commit: <!-- output of `git rev-parse --short HEAD` -->

## Anything else

<!-- Does it reproduce every time? Only under stress / many processes?
     Already listed in docs/notes/KNOWN_ISSUES.md? -->
