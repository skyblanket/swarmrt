module Conform_split

# Conformance: string_split must agree interpreter-vs-compiled. The
# interpreter previously (a) split an empty separator per-byte (compiled
# returns the whole string as one element) and (b) silently capped the
# result at 256 items. Both fixed (whole-string empty-sep + growable
# buffer). Depth kept shallow so no interpreter stack-ceiling divergence.

fun main() {
    empty = string_split("hello", "")
    parts = string_split("a,b,c,d", ",")
    none = string_split("nodelim", ",")
    trail = string_split("x,", ",")
    print(f"empty_len={length(empty)} first={hd(empty)}")
    print(f"parts_len={length(parts)} p0={hd(parts)}")
    print(f"none_len={length(none)} n0={hd(none)}")
    print(f"trail_len={length(trail)}")
}
