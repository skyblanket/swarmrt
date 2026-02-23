module TestLLM

fun main() {
    print("=== LLM Client Test ===")
    print("")

    # Test 1: Simple completion with defaults
    print("--- Test 1: basic llm_complete ---")
    resp = llm_complete("Say hello in exactly 5 words.")
    print("Response:", resp)
    print("")

    # Test 2: With options map
    print("--- Test 2: with opts ---")
    resp2 = llm_complete("What is 2+2? Reply with just the number.", %{
        model: "otonomy-orc",
        max_tokens: 32,
        temperature: 0.0
    })
    print("Response:", resp2)
    print("")

    # Test 3: JSON round-trip with LLM
    print("--- Test 3: JSON structured output ---")
    resp3 = llm_complete("Return a JSON object with keys: name, age, city. Use made up values. Reply with ONLY the JSON, no markdown.")
    print("Raw:", resp3)
    parsed = json_decode(resp3)
    print("Parsed name:", parsed.name)
    print("Parsed age:", parsed.age)
    print("Parsed city:", parsed.city)
    print("")

    # Test 4: Multi-agent conversation
    print("--- Test 4: two agents talking ---")
    agent1_resp = llm_complete("You are Agent Alpha. Propose a 1-sentence idea for an app.", %{
        model: "otonomy-orc",
        max_tokens: 100,
        temperature: 0.9
    })
    print("Agent Alpha:", agent1_resp)

    prompt2 = "You are Agent Beta. Another agent proposed this idea: " ++ agent1_resp ++ " Give a 1-sentence critique."
    agent2_resp = llm_complete(prompt2, %{
        model: "otonomy-orc",
        max_tokens: 100,
        temperature: 0.7
    })
    print("Agent Beta:", agent2_resp)
    print("")

    # Test 5: Spawn concurrent LLM calls
    print("--- Test 5: parallel LLM via spawn ---")
    me = self()

    spawn(worker(me, "poet", "Write a haiku about code. Just the haiku, nothing else."))
    spawn(worker(me, "chef", "Invent a 1-sentence recipe. Just the recipe, nothing else."))
    spawn(worker(me, "comic", "Tell a 1-sentence programming joke. Just the joke, nothing else."))

    # Collect 3 responses
    collect(3)

    print("")
    print("=== All LLM tests passed! ===")
}

fun worker(parent, role, prompt) {
    resp = llm_complete(prompt, %{model: "otonomy-orc", max_tokens: 100, temperature: 0.9})
    send(parent, {role, resp})
}

fun collect(remaining) {
    if (remaining == 0) {
        print("(all collected)")
    } else {
        receive {
            {role, resp} ->
                print("[" ++ role ++ "]:", resp)
                collect(remaining - 1)
        }
    }
}
