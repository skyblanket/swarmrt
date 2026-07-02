module Conform_bool

# Conformance: && / || short-circuit + truthiness must agree
# interpreter-vs-compiled. boom() must NOT run when the operator
# short-circuits; if either path evaluated it, that path would panic
# (exit 1) and the gate's exit-code/stdout diff would catch the drift.

fun boom() { panic("short-circuit broken: RHS evaluated") }

fun main() {
    print(f"and={(1 < 2) && (2 < 3)} or={(1 > 2) || (2 < 3)}")
    print(f"sc_and={'false' && boom()}")
    print(f"sc_or={'true' || boom()}")
    print(f"truthy={(1 < 2)} falsy={(2 < 1)}")
}
