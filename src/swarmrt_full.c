/* SwarmRT Full - Core */
#include "swarmrt_full.h"

/* Reduction counting - decrement on every operation */
#define CHECK_REDS(p) do { if (--(p)->fcalls <= 0) sw_reschedule(p); } while(0)

void sw_reschedule(sw_process_t *p) {
    p->reds += SWARM_REDUCTIONS;
    p->fcalls = SWARM_REDUCTIONS;
    sw_yield();
}

/* Copying message passing */
void sw_send(uint64_t to, sw_term_t msg) {
    sw_process_t *dst = sw_lookup_pid(to);
    sw_term_t copied = sw_copy_term(msg, dst);
    sw_enqueue_msg(dst, copied);
}

/* Preemptive bytecode interpreter */
void sw_interpret(sw_process_t *p) {
    while (1) {
        uint8_t op = *p->pc++;
        switch (op) {
            case SW_OP_MOVE: sw_op_move(p); CHECK_REDS(p); break;
            case SW_OP_CALL: sw_op_call(p); CHECK_REDS(p); break;
            case SW_OP_SEND: sw_op_send(p); CHECK_REDS(p); break;
            case SW_OP_RECV: sw_op_recv(p); CHECK_REDS(p); break;
            case SW_OP_ADD: sw_op_add(p); CHECK_REDS(p); break;
            case SW_OP_RETURN: return;
        }
    }
}

/* Generational GC */
void sw_gc_minor(sw_process_t *p) {
    sw_cheney_copy(p->heap, p->htop, p->old_heap, &p->old_htop);
    p->htop = p->heap;
}