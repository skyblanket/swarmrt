# SwarmRT AI Agent Feedback Report

**Date:** 2026-03-02  
**Tester:** AI Agent (user_researcher)  
**Method:** Hands-on building, writing code, running tests  

---

## Executive Summary

SwarmRT is a native actor-model runtime with ~25K lines of C code. Excellent language design for AI agents, BUT critical reliability issues discovered during real use.

**Overall DX Grade: C+** (was B+ before hands-on testing)

**CRITICAL BUG:** All compiled programs hang forever after `main()` completes.

---

## 🚨 P0 - Critical Issues (Production Blockers)

### 1. Programs Never Exit

**Problem:** Every program hangs forever after `main()` completes.

```bash
./bin/swc build examples/hello.sw -o hello
./hello
# Prints "Hello, Swarm!" then hangs forever
```

**Impact:** CRITICAL - Makes SwarmRT unusable for CLI tools, scripts, automation.

**Workaround:**
```bash
./program &
PID=$!
sleep 2
kill $PID
```

**Root Cause:** Schedulers keep running, no shutdown signal sent.

**Fix Needed:** Add `exit()` builtin or auto-exit when main completes.

---

### 2. No Static Analysis for Undefined Variables

**Problem:** Undefined variables compile without error.

```sw
print(undefined_var)  # Compiles! Runtime creates atom :undefined_var
```

**Impact:** HIGH - Typos become silent runtime bugs.

**Fix Needed:** Compile-time error on undefined variables.

---

## P1 - High Priority Issues

### 3. No REPL

Cannot interactively test code. Must write→compile→run→kill for every test.

**Fix Needed:**
```bash
swc repl
> x = 42
> print(x)
42
```

---

### 4. Poor Error Messages

```
Parse error: line 7: expected ')', got '}'
```

**Missing:**
- Column number
- Source line snippet
- Suggested fix
- Error code for searching

**Fix Needed:** Rich error messages like Rust:
```
error[E0425]: cannot find value `undefined_var` in this scope
 --> myprogram.sw:3:11
  |
3 |     print(undefined_var)
  |           ^^^^^^^^^^^^^ not found in this scope
```

---

### 5. No `swc run` Command

**Current workflow:**
```bash
./bin/swc build myprogram.sw -o myprogram
./myprogram  # hangs forever
```

**Fix Needed:**
```bash
swc run myprogram.sw  # Compile + run + auto-exit after main
```

---

## P2 - Medium Priority Issues

### 6. Inconsistent Boolean Returns

```sw
string_starts_with("x", "y")  # Returns :false (atom)
```

**Expected:** `false` (keyword/boolean)

**Fix Needed:** Consistent boolean type across stdlib.

---

### 7. Print Adds Literal 'n'

```sw
print("hello")  # Output: hellon (literal 'n', not newline)
```

**Fix Needed:** Print should add `\n` not `n`.

---

### 8. No Package Manager

Want to use a library? Must manually:
1. Find the .sw file
2. Copy to your project
3. Import it

**Fix Needed:**
```bash
swarmrt install json-parser
```

---

### 9. No Watch Mode

**Fix Needed:**
```bash
swc watch myprogram.sw  # Auto-rebuild on file changes
```

---

## P3 - Nice to Have

10. **LSP/IDE Support** - Autocomplete, goto-definition
11. **Test Framework** - Built-in assertions, `swc test`
12. **Debugger** - Process introspection
13. **Profiler** - Performance analysis
14. **Documentation Generator** - `swc doc`
15. **Formatter** - `swc fmt`

---

## What Actually Worked Well

### ✅ Build System
```bash
make swc libswarmrt  # ~10 seconds, clean build
```

### ✅ Fast Compilation
Most programs compile in < 1 second.

### ✅ Language Features
- Pattern matching works well
- Imports auto-resolve (`Utils.sw` or `utils.sw`)
- ETS tables work
- JSON encode/decode works
- Tail call optimization works

### ✅ Test Suite
- Phase 2 tests: 6/6 pass
- Integration tests: All pass

---

## Workflow Comparison

### Current SwarmRT Workflow
```bash
vim myprogram.sw
./bin/swc build myprogram.sw -o myprogram
./myprogram &       # hangs forever
sleep 3
kill %1
```

### Ideal Workflow
```bash
vim myprogram.sw
swc repl            # interactive testing
swc run myprogram.sw  # compile + run + exit cleanly
swc test            # run tests
swc watch           # auto-rebuild
```

---

## Grades

| Category | Static Grade | Real Use Grade |
|----------|--------------|----------------|
| Language Design | A- | A- |
| Build System | B | B+ |
| Documentation | C+ | C+ |
| Testing | C | D+ |
| Reliability | - | D |
| **Overall** | **B+** | **C+** |

---

## Would I Use This Today?

- **AI agent orchestration:** Maybe - if shutdown bug fixed
- **Production services:** No - too unreliable
- **Learning actor model:** Yes - simple syntax, fast feedback

---

## Bottom Line

SwarmRT has excellent technical bones (25K LOC C, 100K+ processes, 10ns sends) and genuinely novel AI agent use case. However, the "programs never exit" bug is a showstopper. Combined with no static analysis, it feels like a promising proof-of-concept rather than production-ready system.

**Fix shutdown + static analysis = solid B+ product.**

---

*Analysis performed by actually building, writing code, and running SwarmRT.*
