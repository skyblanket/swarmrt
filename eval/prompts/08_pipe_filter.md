## Task

Given the list `[1, 2, 3, 4, 5, 6, 7, 8, 9, 10]`, use the pipe operator
`|>` and Std helpers to: filter to even numbers, square each one, sum
the result. Print the final sum.

## Expected output

```
220
```

## Notes

Tests `import Std`, pipe composition, `Std.sum`, and the global
`filter` / `map` builtins. Note: `filter` and `map` are global (NOT
`Std.filter`/`Std.map` — those don't exist). Lambdas use `fun(x) { body }`.
