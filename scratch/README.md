# scratch/

One-off `.sw` files used during development — error-message tweaking,
ETS poking, import-resolver shakeout. Kept around because they're handy
when iterating on the same area again, but they're not part of the
test suite and not documented.

If something here graduates into a real test, move it to
`tests/sw/test_<topic>.sw` (compiled path) or
`tests/sw/repl/test_<topic>.sw` (interpreter path) so the harness
picks it up. Otherwise treat as disposable.
