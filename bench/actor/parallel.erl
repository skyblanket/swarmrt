-module(parallel).
-export([main/0]).
consumer(0) -> ok;
consumer(N) -> receive m -> ok end, consumer(N-1).
producer(_C, 0, P) -> P ! fin, ok;
producer(C, N, P) -> C ! m, producer(C, N-1, P).
spawn_pairs(0, _M, _P) -> ok;
spawn_pairs(K, M, P) -> C = spawn(fun() -> consumer(M) end), spawn(fun() -> producer(C, M, P) end), spawn_pairs(K-1, M, P).
wait_fins(0) -> ok;
wait_fins(K) -> receive fin -> ok end, wait_fins(K-1).
main() -> spawn_pairs(20, 500000, self()), wait_fins(20), io:format("done~n").
