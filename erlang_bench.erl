#!/usr/bin/env escript
%% Erlang Benchmark for comparison with SwarmRT

-module(erlang_bench).
-export([main/1]).

main(_) ->
    io:format("~n=== Erlang/BEAM Benchmark ===~n"),
    io:format("Erlang/OTP ~s~n", [erlang:system_info(otp_release)]),
    
    benchmark_spawn(100),
    benchmark_spawn(1000),
    benchmark_spawn(10000),
    
    io:format("~n=== Complete ===~n").

benchmark_spawn(N) ->
    Parent = self(),
    
    Start = erlang:monotonic_time(microsecond),
    
    lists:foreach(fun(_) -> 
        spawn(fun() -> Parent ! done end)
    end, lists:seq(1, N)),
    
    lists:foreach(fun(_) -> receive done -> ok end end, lists:seq(1, N)),
    
    End = erlang:monotonic_time(microsecond),
    
    TotalUs = End - Start,
    PerProc = TotalUs / N,
    Rate = N / (TotalUs / 1000000.0),
    
    io:format("Spawn ~p: ~p us total, ~p us/proc, ~p/sec~n", 
              [N, TotalUs, round(PerProc), round(Rate)]).
