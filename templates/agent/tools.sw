module Tools

# The tools an agent may call. Each takes the text after the tool name and
# returns a string. Add one by adding a case below.

export [run, word_count]

fun run(call) {
    parts = string_split(call, " ")
    name = hd(parts)
    input = string_trim(string_sub(call, string_length(name), string_length(call) - string_length(name)))
    case name {
        "word_count" -> word_count(input)
        _ -> "unknown tool: " ++ name
    }
}

fun word_count(text) {
    words = filter(string_split(string_trim(text), " "), fn(w) { w != "" })
    to_string(length(words))
}
