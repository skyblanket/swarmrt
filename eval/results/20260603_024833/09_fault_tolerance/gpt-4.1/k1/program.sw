module Main

fun boom() {
    panic("boom")
}

fun main() {
    trap_exit('true')
    pid = spawn(fun() { boom() })
    link(pid)
    receive {
        {'EXIT', _, _} -> print("parent_survived")
    }
}
