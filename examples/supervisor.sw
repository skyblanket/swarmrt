# Supervisor pattern - fault tolerance
# Let it crash philosophy

module WorkerPool

export [main]

# Worker that might fail
fun worker(id) {
    receive {
        do_work -> 
            result = risky_operation()
            {ok, result}
        
        crash -> 
            # Intentional crash to test supervisor
            1 / 0
    }
}

# Supervisor spec
supervisor pool_sup {
    # Restart policy: permanent | temporary | transient
    # Max restarts in time window
    
    worker(worker, permanent, max_restarts: 5, window: 60000)
}

fun main() {
    # Start supervised workers
    pool = supervise pool_sup with [worker(1), worker(2), worker(3)]
    
    # If a worker crashes, supervisor restarts it
    # Main code continues unaffected
    
    send(pool[0], do_work)
    
    receive {
        {ok, result} -> print("Success: " ++ result)
        {error, reason} -> print("Failed: " ++ reason)
    }
}
