module Main

fun main() {
    trap_exit('true')
    pid = spawn(fun() { panic("boom") })
    link(pid)
    receive {
        {'EXIT', from, reason} -> print("parent_survived")
    }
}
