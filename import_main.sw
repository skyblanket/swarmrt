module ImportMain
import Utils

fun main() {
    print(Utils.greet("World"))
    result = Utils.double(21)
    print("Double 21 = " ++ to_string(result))
}
