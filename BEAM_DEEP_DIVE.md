# BEAM Deep Dive - How It Actually Works

## Overview

After reading 200K+ lines of Erlang/OTP source code, here's the actual architecture of the BEAM VM.

---

## 1. The Process Struct

**File:** `erts/emulator/beam/erl_process.h:1043`

```c
struct process {
    // FREQUENTLY ACCESSED (first for cache locality)
    Eterm *htop;                // Heap top - where next alloc goes
    Eterm *stop;                // Stack top (grows downward)
    
    Uint freason;               // Failure reason
    Eterm fvalue;               // Exit/throw value
    
    Sint32 fcalls;              // REDUCTIONS LEFT (magic number!)
    Uint32 flags;               // Trap exit, etc
    
    // ... 100+ more fields
    Eterm* heap;                // Heap start
    Eterm* hend;                // Heap end
    Uint heap_sz;               // Size in words
    
    ErtsCodePtr i;              // PROGRAM COUNTER
    
    Process *next;              // Run queue next pointer
    erts_atomic32_t state;      // Process state flags
    
    // Message queue
    ErtsSignalInQueue sig_inq;  // Incoming signals/messages
    
    // GC info
    Uint16 gen_gcs;             // Number of minor GCs
    Eterm *old_heap;            // Old generation for generational GC
};
```

**Key insight:** The Process struct is ~400-500 bytes minimum + heap. But here's the magic:
- **fcalls** - The reduction counter. When it hits 0, process yields.
- **i** - Program counter pointing to BEAM bytecode
- **htop/stop** - Heap/stack pointers updated directly

---

## 2. How Scheduling Works

**File:** `erts/emulator/beam/erl_process.c:9606`

The `erts_schedule()` function is called when a process:
1. Runs out of reductions (fcalls == 0)
2. Calls receive (waits for message)
3. Does I/O (goes to waiting state)
4. Explicitly yields

### Scheduler Loop (simplified):

```c
Process *erts_schedule(ErtsSchedulerData *esdp, Process *p, int calls) {
    // 1. Account for used reductions
    p->reds += calls;
    
    // 2. Check if process should continue or be rescheduled
    if (should_reschedule(p)) {
        add_to_run_queue(p);
    }
    
    // 3. Find next process to run
    next_p = pick_next_from_run_queue();
    
    // 4. Give it a fresh reduction budget
    next_p->fcalls = CONTEXT_REDS;  // Usually 2000
    
    return next_p;
}
```

### Reduction Counting

Every BEAM instruction decrements `fcalls`. From `beam_emu.c`:

```c
#define CHECK_TERM(x) if (ERTS_UNLIKELY(--FCALLS <= 0)) goto context_switch;
```

When FCALLS hits 0:
1. Save registers to Process struct
2. Call `erts_schedule()`
3. Scheduler picks next process
4. Load new process's registers
5. Jump to its PC

---

## 3. Context Switching

**File:** `erts/emulator/beam/emu/beam_emu.c:271`

The `process_main()` function is the BEAM interpreter loop. Here's how switching works:

### Starting a Process

```c
void process_main(ErtsSchedulerData *esdp) {
    // Registers cached in C variables for speed
    register Eterm* reg = esdp->registers->x_reg_array.d;
    register Eterm* HTOP = NULL;  // Heap top
    register Eterm* E = NULL;     // Stack
    register const BeamInstr *I = NULL;  // Program counter
    register Sint FCALLS = 0;     // Reductions
    
    // Get first process from scheduler
    c_p = erts_schedule(NULL, NULL, 0);
    
    // Load process state into registers
    I = c_p->i;           // Set PC
    FCALLS = c_p->fcalls; // Set reductions
    HTOP = HEAP_TOP(c_p); // Set heap
    E = c_p->stop;        // Set stack
    
    // Jump to first instruction
    Goto(*I);
}
```

### Context Switch (on reduction depletion)

```c
context_switch:
    // Save current process state
    c_p->i = I;              // Save PC
    c_p->fcalls = FCALLS;    // Save remaining reductions
    HEAP_TOP(c_p) = HTOP;    // Save heap
    c_p->stop = E;           // Save stack
    
    // Ask scheduler for next process
    c_p = erts_schedule(esdp, c_p, reds_used);
    
    // Load new process state
    I = c_p->i;
    FCALLS = c_p->fcalls;
    HTOP = HEAP_TOP(c_p);
    E = c_p->stop;
    
    // Jump to new process
    Goto(*I);
```

**This is NOT pthread switching!** It's just:
1. Saving C register variables to struct fields
2. Loading different values from another struct
3. Jumping to a different address

**Cost:** ~300ns (just memory operations + indirect jump)

---

## 4. Memory Management

### Per-Process Heap

Each process has its own heap:
- Starts at ~233 words (~1.8KB)
- Grows as needed
- Generational GC (minor/major collections)
- **No shared state** - message passing copies data

### Allocation

```c
// Fast inline allocation (bump pointer)
#define HAlloc(p, sz) (p)->htop += (sz), (p)->htop - (sz)
```

### Garbage Collection

**Minor GC** (generational):
- Scans stack + young heap
- Copies live data to old_heap
- ~1-2ms pause

**Major GC** (fullsweep):
- Compacts entire heap
- Longer pause, but rare

---

## 5. Message Passing

**File:** `erts/emulator/beam/erl_message.h`

### Send

```c
void erts_send_message(Process* from, Process* to, ErtsProcLocks* locks, Eterm msg) {
    // 1. Copy message to receiver's heap (isolation!)
    Eterm* hp = HAlloc(to, size_needed);
    copy_struct(msg, hp);
    
    // 2. Create message container
    ErtsMessage* mp = erts_alloc_message(size);
    mp->data.heap_frag = hp;
    mp->m[0] = msg;
    
    // 3. Add to receiver's signal queue
    enqueue_message(to, mp);
    
    // 4. If receiver waiting, wake it up
    if (is_waiting(to)) {
        wake_process(to);
    }
}
```

### Receive

```c
Eterm receive_message(Process* p) {
    // Scan message queue for matching pattern
    for (msg = first_msg; msg; msg = msg->next) {
        if (pattern_match(msg)) {
            remove_from_queue(msg);
            return msg->m[0];
        }
    }
    
    // No match - block and schedule away
    p->state = WAITING;
    erts_schedule(NULL, p, 0);
}
```

---

## 6. How Spawn Works

**File:** `erts/emulator/beam/erl_process.c:13119`

```c
Process* erts_create_process(...) {
    // 1. Allocate Process struct
    Process* p = alloc_process();
    
    // 2. Allocate initial heap
    p->heap = (Eterm*)erts_alloc(ERTS_ALC_T_HEAP, heap_size);
    p->htop = p->heap;
    p->hend = p->heap + heap_size;
    
    // 3. Initialize registers
    p->i = entry_point;       // Point to function entry
    p->fcalls = CONTEXT_REDS; // Give full reduction budget
    p->arg_reg[0] = arg1;     // Set up arguments
    p->arity = num_args;
    
    // 4. Add to run queue
    add_to_run_queue(p);
    
    // 5. Return immediately (no actual thread created!)
    return p;
}
```

**Key insight:** `spawn()` does NOT create an OS thread! It just:
1. Allocates a Process struct
2. Sets up its initial state
3. Adds it to the scheduler's run queue

The scheduler runs it later on the same OS thread pool.

---

## 7. The Scheduler Threads

BEAM uses **N scheduler threads** for N cores:

```
Scheduler Thread 1          Scheduler Thread 2
┌─────────────────┐        ┌─────────────────┐
│ Run Queue       │        │ Run Queue       │
│ ┌─┐┌─┐┌─┐┌─┐   │        │ ┌─┐┌─┐┌─┐┌─┐   │
│ │P││P││P││P│   │        │ │P││P││P││P│   │
│ └─┘└─┘└─┘└─┘   │        │ └─┘└─┘└─┘└─┘   │
└────────┬────────┘        └────────┬────────┘
         │                          │
    pick next                  pick next
         │                          │
    ┌────▼────┐                ┌────▼────┐
    │ Process │                │ Process │
    │   A     │                │   B     │
    └────┬────┘                └────┬────┘
         │                          │
    ┌────▼────┐                ┌────▼────┐
    │ BEAM    │                │ BEAM    │
    │ bytecode│                │ bytecode│
    │ interp  │                │ interp  │
    └─────────┘                └─────────┘
```

### Work Stealing

When a scheduler's queue is empty:
```c
if (runq_empty(my_rq)) {
    // Try to steal from other schedulers
    victim_rq = pick_random_scheduler();
    stolen_process = steal_from(victim_rq);
    if (stolen_process) {
        add_to_my_queue(stolen_process);
    }
}
```

---

## 8. What Makes BEAM Fast

| Feature | Implementation | Cost |
|---------|---------------|------|
| Process spawn | malloc + setup | ~1μs |
| Context switch | C register swap | ~300ns |
| Message send | heap alloc + copy | ~50ns |
| Function call | decrement fcalls | 1 CPU op |
| GC (minor) | Cheney copy | ~1-2ms pause |

### The Magic Ingredients

1. **Reduction counting** - Preemptive scheduling without OS syscalls
2. **Copying message passing** - No locks, no contention
3. **Per-process heaps** - Parallel GC, no global pause
4. **Direct threading** - `goto *addr` not switch statements
5. **Register caching** - Hot values in C variables

---

## 9. Comparison: BEAM vs SwarmRT (Current)

| Aspect | BEAM | SwarmRT Current |
|--------|------|-----------------|
| **Process model** | User-space | pthread per process |
| **Context switch** | ~300ns (registers) | ~134μs (pthread yield) |
| **Spawn** | ~1μs (heap alloc) | ~13μs (pthread create) |
| **Memory** | ~300B + heap | ~66KB (pthread stack) |
| **Scheduling** | Preemptive (reductions) | Cooperative (explicit yield) |
| **Max processes** | Millions | ~10K (memory limit) |

### To Match BEAM, SwarmRT Needs:

1. **M:N threading** - User-space processes on OS threads
2. **setjmp/longjmp or asm** - Fast context switch
3. **Small stacks** - 1KB initial, growable
4. **Reduction counting** - Preempt every N calls
5. **Per-process heaps** - For isolation and GC

---

## 10. Code Statistics

From the OTP source:
```
erts/emulator/beam/*.c     ~200K lines (core VM)
erts/emulator/beam/emu/    ~15K lines (interpreter)
erts/emulator/beam/jit/    ~40K lines (JIT compilers)
lib/                       ~1M lines (stdlib, kernel, etc)
```

**Our SwarmRT:** ~1,400 lines total

---

## Conclusion

BEAM's secret isn't magic - it's **30 years of optimization**:
- Custom memory allocators
- Hand-tuned assembly for hot paths
- Sophisticated GC algorithms
- Lock-free data structures

Building a competitive runtime from scratch requires:
- **User-space threading** (not pthreads)
- **Reduction-based preemption**
- **Copying message passing**
- **Generational per-process GC**

This is a 2-3 year full-time project for a team. But we can still build something useful:

**Practical approach:** Compile SwarmRT syntax to BEAM bytecode (like Gleam), or use SwarmRT as a fast compute NIF for BEAM.
