-module(spawn).
-export([main/0]).
spawn_n(0) -> ok;
spawn_n(N) -> spawn(fun() -> ok end), spawn_n(N-1).
main() -> spawn_n(1000000), io:format("done~n").
