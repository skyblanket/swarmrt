module Test_shell_utf8

# shell/shell_managed kept only printable ASCII, so `echo café` came back
# as "caf" and every CJK file name vanished from tool output. Valid UTF-8
# must survive, invalid bytes become U+FFFD, ANSI escapes are removed whole,
# binary becomes a placeholder, and a trailing `# comment` / heredoc / `&`
# must not break the wrapper.

fun check(name, cond) {
    if (cond == 'true') { print("PASS " ++ name) ; 0 }
    else { print("FAIL " ++ name) ; 1 }
}

fun main() {
    f = 0
    f = f + check("utf8_kept", if (elem(shell("printf 'café 漢字\\n'"), 1) == "café 漢字\n") { 'true' } else { 'false' })
    f = f + check("utf8_kept_managed", if (elem(shell_managed("printf 'naïve résumé'", 5000), 1) == "naïve résumé") { 'true' } else { 'false' })
    f = f + check("ansi_stripped", if (elem(shell("printf '\\033[31mred\\033[0m'"), 1) == "red") { 'true' } else { 'false' })
    f = f + check("invalid_is_fffd", if (elem(shell("printf 'a\\377\\376b'"), 1) == "a�b") { 'true' } else { 'false' })
    f = f + check("binary_placeholder", string_starts_with(elem(shell("head -c 4096 /dev/urandom"), 1), "[binary output"))
    f = f + check("trailing_comment", if (elem(shell("echo hi # say hi"), 1) == "hi\n") { 'true' } else { 'false' })
    f = f + check("heredoc", if (elem(shell("cat <<'EOF'\nbody\nEOF"), 1) == "body\n") { 'true' } else { 'false' })
    t0 = timestamp()
    bad = shell("echo ( broken 2>/dev/null")
    f = f + check("syntax_error_fails_fast", if (timestamp() - t0 < 5000 && elem(bad, 0) != 0) { 'true' } else { 'false' })
    if (f == 0) { print("OK shell_utf8 8/8") ; sys_exit(0) } else { print("FAIL shell_utf8") ; sys_exit(1) }
}
