# AI Agent Pattern
# Perfect for the mally-style swarm coordination

module AIAgent

export [main, agent_loop]

# Agent state: {id, tools, context}
fun agent_loop(state) {
    receive {
        {execute, task, from} ->
            # Decompose task into subtasks
            subtasks = decompose(task)
            
            # Spawn swarm to handle subtasks in parallel
            results = swarm map(
                fun(sub) { run_tool(state.tools, sub) },
                subtasks
            )
            
            # Synthesize and respond
            response = synthesize(results)
            send(from, {result, response})
            
            agent_loop(state)
        
        {update_context, new_context} ->
            agent_loop({state.id, state.tools, new_context})
        
        shutdown -> 
            print("Agent " ++ state.id ++ " shutting down")
    }
}

fun main() {
    # Create agent with tools
    tools = [file_tool, search_tool, web_tool]
    agent = spawn(agent_loop({id: "agent_1", tools: tools, context: []}))
    
    # Send task
    send(agent, {execute, "Analyze codebase structure", self()})
    
    # Get result
    receive {
        {result, answer} -> print(answer)
    } after 30000 {
        print("Timeout - agent took too long")
    }
}

# Helper functions (built-ins)
fun decompose(task) {
    # AI decomposes task into steps
    # Returns list of subtasks
}

fun run_tool(tools, subtask) {
    # Execute appropriate tool
}

fun synthesize(results) {
    # Combine results into coherent output
}
