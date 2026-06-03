module Main

fun main() {
    trap_exit('true')
    pid = spawn(fun() { panic("boom") })
    link(pid)
    receive {
        {'EXIT', _, _} -> print("parent_survived")
    }
}
