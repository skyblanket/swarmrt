module Test_shell_exit

# `exit` in a shell() command used to end the wrapper's shell before it wrote
# the exit status, so shell() waited the full 120s and returned -1 with no
# output. The command now runs in a subshell: `exit N` returns N right away,
# output printed before the exit is kept, and a failing `|| exit N` works too.

fun check(name, cond) {
    if (cond == 'true') { print("PASS " ++ name) ; 0 }
    else { print("FAIL " ++ name) ; 1 }
}

fun main() {
    f = 0
    t0 = timestamp()
    a = shell("exit 1")
    f = f + check("exit_1_status", if (elem(a, 0) == 1 && timestamp() - t0 < 5000) { 'true' } else { 'false' })
    b = shell("exit 0")
    f = f + check("exit_0_status", if (elem(b, 0) == 0) { 'true' } else { 'false' })
    c = shell("echo before; exit 3")
    f = f + check("output_before_exit", if (elem(c, 0) == 3 && elem(c, 1) == "before\n") { 'true' } else { 'false' })
    d = shell("false || exit 1")
    f = f + check("or_exit_status", if (elem(d, 0) == 1) { 'true' } else { 'false' })
    if (f == 0) { print("OK shell_exit 4/4") ; sys_exit(0) } else { print("FAIL shell_exit") ; sys_exit(1) }
}
