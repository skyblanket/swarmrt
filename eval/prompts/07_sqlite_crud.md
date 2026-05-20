## Task

Open an in-memory SQLite database (`db_open(":memory:")`), create a
table `users(id, name)`, insert two rows (1, 'alice') and (2, 'bob'),
query them ordered by id, and print each user as `id=N name=NAME` on
its own line.

## Expected output

```
id=1 name=alice
id=2 name=bob
```

## Notes

Tests `db_open`, `db_exec`, `db_query`, map iteration via `map_get`
with string keys.
