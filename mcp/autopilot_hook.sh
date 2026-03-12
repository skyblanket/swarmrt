#!/bin/bash
# SwarmRT Autopilot Hook for Claude Code
#
# This hook checks if autopilot is active. If yes, it tells Claude
# to continue working on the next step instead of stopping.
#
# Install: Add to ~/.claude/settings.json under hooks.
# See install.sh for automatic setup.

SWARMRT_DIR="${SWARMRT_PROJECT_ROOT:-.}/.swarmrt"
AUTOPILOT_FILE="$SWARMRT_DIR/autopilot.json"

# Check if autopilot state file exists
if [ ! -f "$AUTOPILOT_FILE" ]; then
    exit 0
fi

# Check if autopilot is active (and not paused)
ACTIVE=$(python3 -c "
import json, sys
try:
    with open('$AUTOPILOT_FILE') as f:
        data = json.load(f)
    if data.get('active', False):
        if data.get('paused', False):
            print('PAUSED')
        else:
            goal = data.get('goal', '')
            current = data.get('current_step', 0)
            steps = data.get('steps', [])
            total = len(steps)
            done = sum(1 for s in steps if s.get('done', False))

            if current < total:
                next_step = steps[current].get('text', 'continue')
                print(f'CONTINUE|{goal}|{done}/{total}|{next_step}')
            else:
                print('DONE')
    else:
        print('INACTIVE')
except:
    print('INACTIVE')
" 2>/dev/null)

case "$ACTIVE" in
    CONTINUE*)
        IFS='|' read -r _ goal progress next_step <<< "$ACTIVE"
        echo ""
        echo "Autopilot active: $goal ($progress steps done)"
        echo "Next: $next_step"
        echo ""
        echo "Call autopilot_status to get full context, then work on the next step."
        echo "When done, call autopilot_step with a summary."
        ;;
    DONE)
        echo "All autopilot steps complete. Call autopilot_stop."
        ;;
    PAUSED)
        # Paused — don't re-feed, let Claude stop naturally
        exit 0
        ;;
    *)
        # Not active, do nothing
        exit 0
        ;;
esac
