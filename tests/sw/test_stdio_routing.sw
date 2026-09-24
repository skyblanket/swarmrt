module Test_stdio_routing

# eprint writes to stderr; stdout_to_stderr() moves everything printed
# afterwards (print, print_inline, streamed output) onto stderr and returns
# the original stdout's fd, which fd_write can still reach. This is how a CLI keeps a
# machine-readable result (`--json`) alone on stdout.
#
# The test re-runs its own binary with SW_STDIO_CHILD=1 and inspects the two
# streams separately.

fun child() {
    print("T-before")
    eprint("E-diag", 7)
    fd = stdout_to_stderr()
    print("T-after")
    print_inline("I-inline\n")
    fd2 = stdout_to_stderr()
    same = if (fd == fd2) { "same" } else { "differs" }
    fd_write(fd, "J-result " ++ same ++ "\n")
    sys_exit(0)
}

fun main() {
    if (getenv("SW_STDIO_CHILD") == "1") { child() }
    else {
        me = hd(os_args())
        dir = "/tmp/sw_stdio_" ++ to_string(random_int(1, 1000000000))
        file_mkdir(dir)
        shell("SW_STDIO_CHILD=1 '" ++ me ++ "' >" ++ dir ++ "/out 2>" ++ dir ++ "/err")
        out = file_read(dir ++ "/out")
        err = file_read(dir ++ "/err")
        shell("rm -rf " ++ dir)
        fails = 0
        if (out == "T-before\nJ-result same\n") { print("PASS stdout_keeps_only_result") }
        else { print(f"FAIL stdout_keeps_only_result: {out}") ; fails = fails + 1 }
        if (string_contains(err, "E-diag 7") == 'true') { print("PASS eprint_to_stderr") }
        else { print(f"FAIL eprint_to_stderr: {err}") ; fails = fails + 1 }
        if (string_contains(err, "T-after") == 'true' && string_contains(err, "I-inline") == 'true') {
            print("PASS diverted_output_on_stderr")
        } else { print(f"FAIL diverted_output_on_stderr: {err}") ; fails = fails + 1 }
        if (fd_write(987654, "x") == 'error') { print("PASS fd_write_bad_fd") }
        else { print("FAIL fd_write_bad_fd") ; fails = fails + 1 }
        if (fails == 0) { print("OK stdio_routing 4/4") ; sys_exit(0) }
        else { print("FAIL stdio_routing") ; sys_exit(1) }
    }
}
