module ImportTest
import MathLib

fun main() {
    print("square(5):", MathLib.square(5))
    print("double(7):", MathLib.double(7))
    print("clamp(150):", MathLib.clamp(150))
    print("clamp(-5):", MathLib.clamp(-5))
    print("clamp(50):", MathLib.clamp(50))
}
