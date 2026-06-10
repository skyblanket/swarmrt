module Conform_strings

# Conformance: string builtins + f-strings (escapes, composite
# rendering) + ++ concat + byte-length semantics.

fun main() {
    s = "Hello, Swarm"
    print(f"len={string_length(s)} upper={string_upper(s)} lower={string_lower(s)}")
    print(f"sub={string_sub(s, 0, 5)} idx={string_index_of(s, "Swarm")} has={string_contains(s, "Sw")}")
    print(f"rep={string_replace(s, "Swarm", "World")} trim=[{string_trim("  x  ")}]")
    print(f"split={string_split("a,b,c", ",")}")
    print(f"starts={string_starts_with(s, "He")} ends={string_ends_with(s, "rm")}")
    # f-string escapes + composite values
    print(f"braces={{literal}} tuple={ {1, 'two'} } list={[1, 2]} map={%{a: 1}}")
    print("concat: " ++ "a" ++ "b" ++ to_string(3))
    # unicode byte-length (documented: bytes, not codepoints)
    print(f"bytes={string_length("世界")}")
    # codepoint-aware split: string_chars walks UTF-8 sequences
    cs = string_chars("héllo 世界")
    print(f"chars={cs} ncp={length(cs)}")
}
