## Task

Decode the JSON string `{"name":"akash","items":["a","b","c"]}` and
print two lines: the value of `name`, then the number of items in the
`items` list.

## Expected output

```
akash
3
```

## Notes

Tests `json_decode`, `map_get` with string keys, and list `length`.
After `json_decode`, the items field is a list of strings.
