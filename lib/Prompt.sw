# Prompt.sw — tiny prompt template engine.
#
# Substitutes `{{name}}` placeholders in a template string with values
# from a vars map. Keys are looked up by name; map_get already treats
# atom and string keys as interchangeable, so the caller can use
# either style.
#
#   Prompt.render("Hello {{name}}, you have {{count}} messages",
#                 %{name: "Alice", count: 3})
#   -> "Hello Alice, you have 3 messages"
#
#   Prompt.from_file("prompts/system.md", %{user: "Sky", role: "agent"})
#   -> loads the file, substitutes, returns the result
#
# Missing variables render as empty strings (use `Prompt.render_strict`
# if you want a panic on missing instead).

module Prompt
export [render, render_strict, from_file]

fun render(template, vars) {
    walk(template, vars, 0, "", 'false')
}

fun render_strict(template, vars) {
    walk(template, vars, 0, "", 'true')
}

fun from_file(path, vars) {
    content = expect(file_read(path), f"Prompt: file not found: {path}")
    render(content, vars)
}

# Internal: scan the template char by char, copying literals into acc.
# When we hit `{{`, find the matching `}}`, look up the trimmed var
# name in vars, and append its rendered value. Unmatched `{{` is
# emitted verbatim (so braces don't have to be escaped if the user
# just wants curly braces in their prompt).
fun walk(t, vars, i, acc, strict) {
    if (i >= string_length(t)) { acc }
    else {
        if (i + 2 <= string_length(t) && string_sub(t, i, 2) == "{{") {
            rest_after = string_sub(t, i + 2, string_length(t) - (i + 2))
            close_at = string_index_of(rest_after, "}}")
            if (close_at < 0) {
                # No closing — emit rest verbatim and stop.
                acc ++ string_sub(t, i, string_length(t) - i)
            } else {
                var_name = trim_ws(string_sub(rest_after, 0, close_at))
                v = map_get(vars, var_name)
                rendered = case v {
                    nil ->
                        if (strict == 'true') {
                            panic(f"Prompt: missing variable '{var_name}'")
                        } else { "" }
                    _   -> to_string(v)
                }
                walk(t, vars, i + 2 + close_at + 2, acc ++ rendered, strict)
            }
        } else {
            walk(t, vars, i + 1, acc ++ string_sub(t, i, 1), strict)
        }
    }
}

# Trim spaces/tabs around a key name. `string_trim` already exists as
# a builtin but it handles \n\r too — for placeholder names we only
# care about the typical " name " case.
fun trim_ws(s) { string_trim(s) }
