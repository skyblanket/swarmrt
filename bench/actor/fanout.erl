-module(fanout).
-export([main/0, sink/1]).
sink(0) -> ok;
sink(N) -> receive m -> ok end, sink(N-1).
send_n(_P, 0) -> ok;
send_n(P, N) -> P ! m, send_n(P, N-1).
main() ->
    N = 5000000,
    P = spawn(fun() -> sink(N) end),
    send_n(P, N),
    io:format("done~n").
