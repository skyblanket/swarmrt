module Main

fun main() {
    trap_exit('true')  # enable exit trapping
    pid = spawn(fun() { panic("boom") })
    link(pid)
    receive {
        {'EXIT', from, _} -> print("parent_survived")
    }
}
