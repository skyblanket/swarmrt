module ErrorTest3

fun main() {
    # Wrong syntax for receive
    receive {
        msg -> print(msg)
    }
}
