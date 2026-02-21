# Swarm parallel operations
# AI's favorite - parallel execution as a primitive

module SwarmParallel

export [main, analyze_file]

fun analyze_file(path) {
    # Simulate file analysis
    content = read_file(path)
    {path, analyze(content)}
}

fun main() {
    files = ["src/auth.sw", "src/db.sw", "src/api.sw"]
    
    # Parallel map - spawns process per item
    # The `swarm` keyword tells the runtime to parallelize
    results = swarm map(analyze_file, files)
    
    # Or with pipe syntax (AI-friendly!)
    results = files |> swarm map(analyze_file)
    
    # Reduce in parallel
    total = swarm reduce(
        fun(acc, item) { acc + item.size },
        0,
        results
    )
    
    print("Total size: " ++ total)
}
