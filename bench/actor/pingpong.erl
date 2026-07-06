-module(pingpong).
-export([main/0, pong/0]).
pong() ->
    receive
        {ping, From} -> From ! pong, pong();
        stop -> ok
    end.
ping_loop(P, 0) -> P ! stop, ok;
ping_loop(P, N) ->
    P ! {ping, self()},
    receive pong -> ok end,
    ping_loop(P, N-1).
main() ->
    P = spawn(fun pong/0),
    ping_loop(P, 2000000),
    io:format("done~n").
