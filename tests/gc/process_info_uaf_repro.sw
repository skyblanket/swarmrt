# process_info_uaf_repro.sw — regression gate for the process_info() reg_entry UAF.
#
# _builtin_process_info() must read proc->reg_entry->name UNDER registry.lock:
# registry_remove_proc() free()s the entry under the wrlock when a registered
# process exits, so an unlocked read in process_info is a use-after-free of a
# malloc'd block (NOT the 0xDE-poisoned varena — which is why it slipped past
# every other gc-stress repro). 64 named permanent guards are crash-restarted in
# a tight loop (free+realloc of reg_entry) while 8 fibers hammer process_info()
# on every pid. Pre-fix: ASan reports heap-use-after-free in strlen within ~4s.
# Post-fix: runs the bounded churn and prints "OK process_info_uaf", exits 0.
module Process_info_uaf

fun guard() { receive { 'boom' -> panic("boom") _ -> guard() } }

fun start_guards(sup, n, names) {
    if (n <= 0) { names }
    else {
        nm = "piu_guard_" ++ to_string(n)
        sup_start_child(sup, {nm, fun() { guard() }, 'permanent'})
        start_guards(sup, n - 1, [nm | names])
    }
}
fun crash_all(names) {
    if (length(names) == 0) { 'ok' }
    else { p = whereis(hd(names)); if (p != nil) { send(p, 'boom') } else { 'x' }; crash_all(tl(names)) }
}
fun chaos_loop(names, n) { if (n <= 0) { 'ok' } else { crash_all(names); sleep(1); chaos_loop(names, n - 1) } }

fun sample_all(pids) { if (length(pids) == 0) { 'ok' } else { process_info(hd(pids)); sample_all(tl(pids)) } }
fun sampler_loop(n) { if (n <= 0) { 'ok' } else { sample_all(process_list()); sampler_loop(n - 1) } }
fun spawn_samplers(k) { if (k <= 0) { 'ok' } else { spawn(sampler_loop(100000)); spawn_samplers(k - 1) } }

fun main() {
    sup = dyn_supervisor(1000000, 60)
    names = start_guards(sup, 64, [])
    sleep(30)
    spawn_samplers(8)
    chaos_loop(names, 6000)   # ~6s of free/realloc churn racing concurrent process_info reads
    print("OK process_info_uaf")
}
