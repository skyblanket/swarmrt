module Test_wsc_invm_loopback

# Regression for the in-VM wsc_connect scheduler-yield deadlock the voice-agent
# port flagged (/Users/sky/voice-agent/docs/RUNBOOK.md, gap table; src/Bridge.sw
# ga_connect). It MUST be run under SW_SCHEDULERS=1 — that's the configuration
# that pins the bug. The driver in tests/sw/run_tests.sh runs this fixture with
# SW_SCHEDULERS=1 under a perl alarm; a regression re-introduces the hang, the
# alarm fires, and the file FAILs.
#
# Topology (exactly the bridge shape that deadlocked), all in ONE VM:
#   server T (Telnyx stand-in)  on :9311 — one handler process.
#   server O (OpenAI stand-in)  on :9312 — echoes WS frames.
#   driver = Telnyx: connects to T and sends a "start" frame, then stays alive
#            so T's incoming WS connection is LIVE.
#   T's handler, on the FIRST ws_message, BLOCKS in wsc_connect -> O while T's
#   conn is live (mirrors Bridge.call_init calling ga_connect before its receive
#   loop). With a single scheduler, the OLD blocking connect/handshake froze the
#   only scheduler thread, so O could never send its 101 -> deadlock. The fix
#   makes connect()/handshake yield the scheduler (sw_yield), so O runs and the
#   dial completes.
#
# Success = the bridge reaches O, exchanges a frame, prints DONE, exits 0.

fun tx_server() {
    http_listen(9311)
    tx_handler('false')
}

fun tx_handler(done) {
    receive {
        {'ws_message', conn, _text} -> {
            case done {
                'false' -> {
                    # The blocking dial-while-conn-is-live that used to deadlock.
                    oa = connect_retry("ws://127.0.0.1:9312/", 200)
                    case oa {
                        'nil' -> { print("FAIL: bridge could not reach O") sys_exit(1) }
                        _     -> {
                            wsc_send(oa, "session.update")
                            r = wsc_recv(oa, 5000)
                            case r {
                                "session.update" -> {
                                    print("PASS wsc_invm_dial_while_conn_live")
                                    print("OK test_wsc_invm_loopback 1/1")
                                    sys_exit(0)
                                }
                                _ -> {
                                    print("FAIL: O echo mismatch, got " ++ to_string(r))
                                    sys_exit(1)
                                }
                            }
                        }
                    }
                }
                _ -> tx_handler(done)
            }
        }
        {'ws_close', _c} -> tx_handler(done)
        after 25000 { print("FAIL: tx_handler idle (deadlock?)") sys_exit(2) }
    }
}

fun oa_server() {
    http_listen(9312)
    oa_loop()
}
fun oa_loop() {
    receive {
        {'ws_message', conn, text} -> ws_send(conn, text) oa_loop()
        {'ws_close', _c}           -> oa_loop()
        after 25000 { 0 }
    }
}

fun connect_retry(url, attempts) {
    h = wsc_connect(url)
    case h {
        'nil' -> if (attempts <= 1) { 'nil' } else { sleep(25) connect_retry(url, attempts - 1) }
        _     -> h
    }
}

fun main() {
    spawn(fun() { tx_server() })
    spawn(fun() { oa_server() })

    h = connect_retry("ws://127.0.0.1:9311/", 200)
    case h {
        'nil' -> { print("FAIL: driver could not connect to T") sys_exit(1) }
        _     -> { wsc_send(h, "start") }
    }
    # Keep the driver (and thus T's incoming conn) alive while the bridge dials O.
    receive { after 25000 { print("FAIL: driver idle (deadlock?)") sys_exit(3) } }
}
