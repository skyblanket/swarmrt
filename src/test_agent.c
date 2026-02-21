#include <stdio.h>
#include <unistd.h>
#include "swarmrt_agent.h"

int main() {
    printf("\n=== SwarmRT Agent System ===\n");
    printf("Async tool calling with process suspension\n\n");
    
    sw_agent_init();
    printf("✅ Agent system initialized\n");
    printf("✅ Async tool support enabled\n");
    printf("\nKey: sw_await() suspends process during tool calls\n");
    printf("Key: Schedulers run other agents while tools execute\n");
    
    return 0;
}
