-module(beam_bench).
-export([run/0]).

%% =====================================================
%% BEAM Benchmark Suite — head-to-head with SwarmRT
%% =====================================================

run() ->
    io:format("~n================================================~n"),
    io:format("  BEAM/OTP Benchmark Suite (OTP ~s)~n", [erlang:system_info(otp_release)]),
    io:format("  Schedulers: ~p~n", [erlang:system_info(schedulers_online)]),
    io:format("================================================~n"),

    bench_spawn(100),
    bench_spawn(1000),
    bench_spawn(10000),

    bench_context_switch(1000),
    bench_context_switch(10000),
    bench_context_switch(100000),

    bench_message_passing(10000),

    bench_memory(),

    bench_ring(10, 100),

    bench_parallel_spawn(10000),

    io:format("~n================================================~n"),
    io:format("  BEAM Benchmark Complete~n"),
    io:format("================================================~n"),
    halt(0).

%% --- Spawn ---
bench_spawn(N) ->
    io:format("~n=== Spawn ~p processes ===~n", [N]),
    Parent = self(),
    T0 = erlang:monotonic_time(microsecond),
    lists:foreach(fun(_) -> spawn(fun() -> Parent ! done end) end, lists:seq(1, N)),
    T1 = erlang:monotonic_time(microsecond),
    %% Wait for all to finish
    drain(N),
    T2 = erlang:monotonic_time(microsecond),
    SpawnUs = T1 - T0,
    TotalUs = T2 - T0,
    io:format("  Spawn time: ~p us (~.2f us/proc)~n", [SpawnUs, SpawnUs / N]),
    io:format("  Total time: ~p us (~.2f ms)~n", [TotalUs, TotalUs / 1000]),
    io:format("  Rate: ~p spawns/sec~n", [round(N / (SpawnUs / 1000000))]).

drain(0) -> ok;
drain(N) -> receive done -> drain(N - 1) after 5000 -> timeout end.

%% --- Context switch (yield) ---
bench_context_switch(N) ->
    io:format("~n=== Context switch ~p yields ===~n", [N]),
    Parent = self(),
    T0 = erlang:monotonic_time(microsecond),
    spawn(fun() -> yield_loop(N, Parent) end),
    receive switches_done -> ok after 10000 -> timeout end,
    T1 = erlang:monotonic_time(microsecond),
    Elapsed = T1 - T0,
    NsPerSwitch = (Elapsed * 1000) / N,
    io:format("  Time: ~p us~n", [Elapsed]),
    io:format("  Per switch: ~.1f ns~n", [NsPerSwitch]),
    io:format("  Rate: ~p switches/sec~n", [round(N / (Elapsed / 1000000))]).

yield_loop(0, Parent) -> Parent ! switches_done;
yield_loop(N, Parent) -> erlang:yield(), yield_loop(N - 1, Parent).

%% --- Message passing (ping-pong) ---
bench_message_passing(N) ->
    io:format("~n=== Message passing ~p round-trips ===~n", [N]),
    Parent = self(),
    Pong = spawn(fun() -> pong_loop(N) end),
    T0 = erlang:monotonic_time(microsecond),
    ping_loop(N, Pong),
    T1 = erlang:monotonic_time(microsecond),
    Parent ! {msg_done, T1 - T0},
    receive {msg_done, Elapsed} ->
        NsPerMsg = (Elapsed * 1000) / (N * 2), %% 2 messages per round-trip
        io:format("  Time: ~p us~n", [Elapsed]),
        io:format("  Per message: ~.1f ns~n", [NsPerMsg]),
        io:format("  Rate: ~p msgs/sec~n", [round(N * 2 / (Elapsed / 1000000))])
    after 10000 -> io:format("  TIMEOUT~n")
    end.

ping_loop(0, _Pong) -> ok;
ping_loop(N, Pong) ->
    Pong ! {ping, self()},
    receive pong -> ok end,
    ping_loop(N - 1, Pong).

pong_loop(0) -> ok;
pong_loop(N) ->
    receive {ping, From} -> From ! pong end,
    pong_loop(N - 1).

%% --- Memory ---
bench_memory() ->
    io:format("~n=== Memory per process ===~n"),
    Before = erlang:memory(processes),
    Pids = [spawn(fun() -> receive stop -> ok end end) || _ <- lists:seq(1, 10000)],
    After = erlang:memory(processes),
    PerProc = (After - Before) / 10000,
    io:format("  10000 processes: ~.2f MB~n", [(After - Before) / (1024*1024)]),
    io:format("  Per process: ~p bytes~n", [round(PerProc)]),
    lists:foreach(fun(P) -> P ! stop end, Pids),
    timer:sleep(100).

%% --- Ring benchmark (classic Erlang benchmark) ---
bench_ring(NumProcs, NumMsgs) ->
    io:format("~n=== Ring ~p procs x ~p messages ===~n", [NumProcs, NumMsgs]),
    T0 = erlang:monotonic_time(microsecond),
    Last = ring_create(NumProcs, self()),
    lists:foreach(fun(I) -> Last ! {msg, I} end, lists:seq(1, NumMsgs)),
    ring_collect(NumMsgs),
    T1 = erlang:monotonic_time(microsecond),
    Elapsed = T1 - T0,
    TotalMsgs = NumProcs * NumMsgs,
    io:format("  Time: ~p us (~.2f ms)~n", [Elapsed, Elapsed / 1000]),
    io:format("  Total messages: ~p~n", [TotalMsgs]),
    io:format("  Rate: ~p msgs/sec~n", [round(TotalMsgs / (Elapsed / 1000000))]).

ring_create(1, Next) -> spawn(fun() -> ring_node(Next) end);
ring_create(N, Next) ->
    Pid = spawn(fun() -> ring_node(Next) end),
    ring_create(N - 1, Pid).

ring_node(Next) ->
    receive
        {msg, _} = M -> Next ! M, ring_node(Next);
        stop -> ok
    end.

ring_collect(0) -> ok;
ring_collect(N) -> receive {msg, _} -> ring_collect(N - 1) after 10000 -> timeout end.

%% --- Parallel spawn (all at once, measure completion) ---
bench_parallel_spawn(N) ->
    io:format("~n=== Parallel spawn+complete ~p processes ===~n", [N]),
    Parent = self(),
    T0 = erlang:monotonic_time(microsecond),
    lists:foreach(fun(I) ->
        spawn(fun() ->
            %% Do some trivial work
            _ = lists:seq(1, 100),
            Parent ! {done, I}
        end)
    end, lists:seq(1, N)),
    par_drain(N),
    T1 = erlang:monotonic_time(microsecond),
    Elapsed = T1 - T0,
    io:format("  Time: ~p us (~.2f ms)~n", [Elapsed, Elapsed / 1000]),
    io:format("  Per process: ~.2f us~n", [Elapsed / N]),
    io:format("  Rate: ~p procs/sec~n", [round(N / (Elapsed / 1000000))]).

par_drain(0) -> ok;
par_drain(N) -> receive {done, _} -> par_drain(N - 1) after 10000 -> timeout end.
