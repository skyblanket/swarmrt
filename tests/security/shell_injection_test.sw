module Main

# Regression test: http_post / http_post_stream must NOT pass caller
# strings through a shell. Before the argv conversion, the URL and header
# values were single-quoted into a `/bin/sh -c "curl ..."` string with no
# escaping, so an embedded `'` broke out and any injected command ran.
#
# Each case fires a request whose URL or header carries a shell payload
# that would `touch` a canary file IF a shell ever parsed it. curl just
# gets a nonsense argv element instead. We run the request in a spawned
# process (it blocks on connect retries to a dead port) and check the
# canary after a short delay — an injected `touch` would fire on the very
# first attempt, well within the window. If the canary exists, we panic
# (non-zero exit) so CI catches the regression.

fun check_clean(canary, label) {
    if (file_exists(canary) == 'true') {
        file_delete(canary)
        panic("SHELL INJECTION via " ++ label ++ " — canary file was created")
    } else {
        print("PASS: " ++ label ++ " injection neutralized")
    }
}

fun fire(canary) {
    if (file_exists(canary) == 'true') { file_delete(canary) }
    # payload closes the single-quote curl used to wrap the value, runs
    # touch, then reopens a quote so the old shell string stayed valid.
    "'; touch " ++ canary ++ "; echo '"
}

fun main() {
    c1 = "/tmp/sw_inj_url_canary"
    p1 = fire(c1)
    spawn(fun() { http_post("http://127.0.0.1:9/x" ++ p1, [], "{}") })

    c2 = "/tmp/sw_inj_hdr_canary"
    p2 = fire(c2)
    spawn(fun() { http_post("http://127.0.0.1:9/y", [{"X-Evil", p2}], "{}") })

    c3 = "/tmp/sw_inj_stream_canary"
    p3 = fire(c3)
    spawn(fun() { http_post_stream("http://127.0.0.1:9/z" ++ p3, [], "{}") })

    # Give each spawned request time to fire its first curl attempt.
    sleep(2000)

    check_clean(c1, "http_post URL")
    check_clean(c2, "http_post header")
    check_clean(c3, "http_post_stream URL")
    print("ALL PASS: no shell injection path in the curl builtins")
}
