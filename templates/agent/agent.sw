module Agent

# One agent answers one task. It asks the model, and when the model asks
# for a tool (a reply of the form `CALL <tool> <input>`) it runs the tool and
# asks again with the result. A bad reply panics: the caller's
# Std.task_stream turns that into an {'error', reason} cell for this task
# alone, and every other task keeps going.

import Tools

export [answer, instructions]

fun instructions() {
    "Answer the task in one short line. If you need to count the words in a " ++
    "piece of text, reply exactly `CALL word_count <text>` and nothing else; " ++
    "you will get the count back."
}

# `llm` holds llm_complete opts: %{} uses LLM_URL / LLM_PROVIDER / LLM_MODEL
# from the environment, %{url: ...} points somewhere explicit.
fun answer(task, llm) { step(instructions() ++ "\n\nTask: " ++ task, llm, 3) }

fun step(prompt, llm, budget) {
    reply = string_trim(llm_complete(prompt, llm))
    if (string_starts_with(reply, "error:")) { panic(reply) }
    if (string_starts_with(reply, "CALL ")) {
        if (budget == 0) { panic("too many tool calls") }
        result = Tools.run(string_sub(reply, 5, string_length(reply) - 5))
        step(prompt ++ "\n\nTool result: " ++ result, llm, budget - 1)
    } else { reply }
}
