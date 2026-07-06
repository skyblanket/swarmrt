module Conform_input_builtins

# Conformance: the Wave-2C input builtins must behave IDENTICALLY
# interpreter-vs-compiled.
#   - stdin ring: UTF-8 multi-byte bytes survive push/take verbatim
#     (t13 proves ASCII accumulate; this proves multi-byte payloads).
#   - rl_history_load: newest-1000 cap on a 1005-line file, nil on a
#     missing file, 'false'/nil on bad args.
#   - rl_history_append: appends one line (creating the parent dir),
#     rejects blank lines and embedded newlines.
# All file state is reset at startup so both paths see the same world.

# 1005 lines built by straight-line doubling — NOT recursion: the
# tree-walking interpreter has no TCO and a ~1000-frame depth cap, so a
# per-line recursive builder would panic on the interp path only.
fun hist_lines_1005() {
    l1 = "cmd line\n"
    l5 = l1 ++ l1 ++ l1 ++ l1 ++ l1
    l25 = l5 ++ l5 ++ l5 ++ l5 ++ l5
    l125 = l25 ++ l25 ++ l25 ++ l25 ++ l25
    l500 = l125 ++ l125 ++ l125 ++ l125
    l500 ++ l500 ++ l5
}

fun main() {
    # --- UTF-8 ring round-trip ---
    stdin_take_pending()
    u = "héllo→世界🌏"
    p = stdin_pending_push(u)
    got = stdin_take_pending()
    same = if (got == u) { 'yes' } else { 'no' }
    empty = stdin_take_pending()
    print(f"ring_push={p} ring_same={same} ring_empty={empty}")

    # --- rl_history_load: 1000-line cap ---
    hp = "/tmp/sw_conform_t14_hist.txt"
    file_delete(hp)
    file_write(hp, hist_lines_1005())
    n_cap = rl_history_load(hp)
    n_missing = rl_history_load("/tmp/sw_conform_t14_missing.txt")
    n_bad = rl_history_load(42)
    print(f"load_cap={n_cap} load_missing={n_missing} load_badarg={n_bad}")
    file_delete(hp)

    # --- rl_history_append: parent-dir creation + rejection rules ---
    ap = "/tmp/sw_conform_t14_dir/history"
    file_delete(ap)
    a1 = rl_history_append(ap, "cmd one")
    a2 = rl_history_append(ap, "bad\nline")
    a3 = rl_history_append(ap, "")
    body = file_read(ap)
    round = if (body == "cmd one\n") { 'yes' } else { 'no' }
    n_one = rl_history_load(ap)
    print(f"append={a1} newline_rejected={a2} empty_rejected={a3} body_ok={round} reload={n_one}")
    file_delete(ap)
}
