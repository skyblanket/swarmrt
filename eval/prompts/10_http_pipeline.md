## Task

Build a tiny data-pipeline program. The input is a hardcoded JSON
string representing a list of events:

```
[{"type":"click","count":3},{"type":"view","count":10},{"type":"click","count":7},{"type":"signup","count":1}]
```

Decode it, group by `type`, sum `count` per type, and print one line
per type in the format `TYPE: TOTAL`, sorted alphabetically by TYPE.

## Expected output

```
click: 10
signup: 1
view: 10
```

## Notes

Tests JSON parsing, list iteration, map accumulation, sort. May use
Std.sort with a key function or sort the keys before printing. The
expected output is alphabetical by type.
