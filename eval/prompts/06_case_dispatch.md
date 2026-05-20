## Task

Write a function `classify(value)` that uses `case` to return:
- `"int"` for any integer
- `"empty_list"` for the empty list `[]`
- `"list:N"` where N is the length, for non-empty lists
- `"ok:V"` where V is the inner value, for `{'ok', V}` tuples
- `"other"` for anything else

Call it with these values in order and print the result of each:
- `42`
- `[]`
- `[1, 2, 3]`
- `{'ok', "hello"}`
- `"a string"`

## Expected output

```
int
empty_list
list:3
ok:hello
other
```

## Notes

Exercises `case` pattern matching with guards, tuple binds, list
patterns, and the catchall.
