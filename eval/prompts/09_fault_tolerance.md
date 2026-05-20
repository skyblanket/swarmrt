## Task

Demonstrate fault tolerance: spawn a process that immediately calls
`panic("boom")`, but trap the exit so the main process survives. After
the child dies, print `parent_survived`.

Use `trap_exit('true')` and `link(pid)`. After the linked child
panics, the parent receives an exit message via `receive`. Print
`parent_survived` after observing the death.

## Expected output

```
parent_survived
```

## Notes

Tests `link`, `trap_exit`, `panic`, `receive` for exit messages. The
exit message arrives as a tuple — the exact shape is documented in
the BUILDING_AGENTS guide and SW_LANGUAGE reference.
