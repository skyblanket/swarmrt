module Test_prompt_utf8

import Prompt

# Prompt.render copies literal text around placeholders; string_sub never
# splits UTF-8 characters, so non-ASCII literals must survive intact.

fun main() {
    out = Prompt.render("Café — {{name}} ✓", %{"name" => "Zoë"})
    if (out == "Café — Zoë ✓") { print("OK prompt_utf8") ; sys_exit(0) }
    else { print("FAIL prompt_utf8: got " ++ out) ; sys_exit(1) }
}
