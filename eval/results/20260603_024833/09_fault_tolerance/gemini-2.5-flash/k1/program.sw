module Main

fun child_process() {
    panic("boom")
}

fun main() {
    trap_exit('true')
    child_pid = spawn(fun() { child_process() })
    link(child_pid)

    receive {
        {'EXIT', from_pid, reason} ->
            print("parent_survived")
    }
}
