module MyTest
export [main]

fun worker(id) {
    print("Worker " ++ to_string(id) ++ " started")
    receive {
        {'compute', n} -> 
            result = n * 2
            print("Worker " ++ to_string(id) ++ " computed: " ++ to_string(result))
            worker(id)
        'stop' -> 
            print("Worker " ++ to_string(id) ++ " stopping")
    }
}

fun main() {
    print("=== My First SwarmRT Program ===")
    
    # Spawn some workers
    w1 = spawn(worker(1))
    w2 = spawn(worker(2))
    
    # Send them work
    send(w1, {'compute', 21})
    send(w2, {'compute', 42})
    
    # Let them work
    sleep(100)
    
    # Stop them
    send(w1, 'stop')
    send(w2, 'stop')
    
    print("=== Done ===")
}
