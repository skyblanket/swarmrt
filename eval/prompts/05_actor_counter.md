## Task

Spawn a counter actor that holds an integer state. The main process
sends it five `'incr'` messages, then sends `{'get', self()}` and waits
for the reply. Print the final count.

## Expected output

```
count=5
```

## Notes

Tests `spawn`, `send`, `receive`, `self()`, and tagged-tuple messaging.
The counter actor should `receive` in a loop, recursing with the
updated state.
