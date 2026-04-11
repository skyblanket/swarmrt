# Video Studio — Product Announcement Generator
# Multi-agent pipeline: Brief → Script → Visuals → Scenes → Video Prompts → Review
# Generates LTX-2 ready prompts + shell commands for sushi GPU rendering.
# SwarmRT / otonomy.ai — 2026

module VideoStudio

export [main, producer, scriptwriter, art_director, scene_composer, prompt_engineer, critic, scene_collector, keeper]

# ── LLM Helpers ──

fun call_orc(prompt) {
    llm_complete(prompt, %{model: "otonomy-orc", max_tokens: 4096, temperature: 0.7})
}

fun call_swarm(prompt) {
    llm_complete(prompt, %{model: "otonomy-swarm", max_tokens: 4096, temperature: 0.7})
}

fun call_swarm_long(prompt) {
    llm_complete(prompt, %{model: "otonomy-swarm", max_tokens: 16384, temperature: 0.7})
}

# ── The Producer ──
# Receives product brief, coordinates the entire pipeline.

fun producer(tables) {
    receive {
        {'brief', project_id, brief_text} ->
            print("[producer] Brief received for project " ++ to_string(project_id))
            print("[producer] " ++ string_truncate(brief_text, 120))

            # Fan out to scriptwriter + art director in parallel
            send(whereis("scriptwriter"), {'write', project_id, brief_text})
            send(whereis("art_director"), {'direct', project_id, brief_text})
            ets_put(elem(tables, 0), project_id, {'writing', 0})
            producer(tables)

        {'specs_ready', project_id} ->
            print("[producer] Script + visuals ready. Composing scenes...")
            ets_put(elem(tables, 0), project_id, {'composing', 0})
            send(whereis("scene_composer"), {'compose', project_id, tables})
            producer(tables)

        {'scenes_done', project_id} ->
            print("[producer] Scenes composed. Generating video prompts...")
            ets_put(elem(tables, 0), project_id, {'prompting', 0})
            send(whereis("prompt_engineer"), {'generate', project_id, tables})
            producer(tables)

        {'prompts_done', project_id} ->
            print("[producer] Video prompts ready. Sending to critic...")
            ets_put(elem(tables, 0), project_id, {'reviewing', 0})
            send(whereis("critic"), {'review', project_id, tables})
            producer(tables)

        {'review_done', project_id, score, passed} ->
            if (passed == 'true') {
                print("")
                print("[producer] ════════════════════════════════════════")
                print("[producer]  SHIPPED — project " ++ to_string(project_id))
                print("[producer]  Score: " ++ to_string(score))
                print("[producer]  Output: studio/output/video_" ++ to_string(project_id) ++ "/")
                print("[producer] ════════════════════════════════════════")
                print("")
                ets_put(elem(tables, 0), project_id, {'shipped', 0})
            } else {
                cycle = 0
                prev = ets_get(elem(tables, 0), project_id)
                if (prev != nil) {
                    cycle = elem(prev, 1)
                }
                if (cycle >= 2) {
                    print("[producer] Max revisions. Shipping project " ++ to_string(project_id))
                    ets_put(elem(tables, 0), project_id, {'shipped', cycle})
                } else {
                    print("[producer] Revision " ++ to_string(cycle + 1) ++ " — re-composing scenes...")
                    ets_put(elem(tables, 0), project_id, {'revision', cycle + 1})
                    send(whereis("scene_composer"), {'revise', project_id, tables})
                }
            }
            producer(tables)
    }
}

# ── The Scriptwriter ──
# Writes the announcement script: hook, key features, CTA.

fun scriptwriter(tables) {
    receive {
        {'write', project_id, brief} ->
            print("[scriptwriter] Writing script for project " ++ to_string(project_id))

            prompt = "You are a world-class product announcement scriptwriter. Write a 30-60 second video script for this product. Output ONLY the script in this exact format:\n\nHOOK: (1 punchy opening line, max 10 words)\n\nNARRATION:\n[Scene 1] (3-4 sentences introducing the product)\n[Scene 2] (3-4 sentences on key features/benefits)\n[Scene 3] (2-3 sentences on the wow factor / differentiator)\n[Scene 4] (2-3 sentences CTA + closing)\n\nTAGLINE: (max 6 words)\n\nKeep it punchy, cinematic, modern. No fluff.\n\nProduct brief: " ++ brief

            script = call_orc(prompt)
            print("[scriptwriter] Script ready (" ++ to_string(string_length(script)) ++ " chars)")
            ets_put(elem(tables, 1), project_id, script)
            send(whereis("scene_collector"), {'spec_done', project_id, 'script'})
            scriptwriter(tables)
    }
}

# ── The Art Director ──
# Creates visual style guide: color palette, mood, camera style, transitions.

fun art_director(tables) {
    receive {
        {'direct', project_id, brief} ->
            print("[art_director] Creating visual direction for project " ++ to_string(project_id))

            prompt = "You are a cinematic art director for product videos. Create a visual style guide for this product announcement video. Output ONLY in this format:\n\nMOOD: (3 words)\nCOLOR PALETTE: (5 hex colors)\nLIGHTING: (1 sentence)\nCAMERA STYLE: (e.g., slow dolly, handheld, drone, macro)\nTRANSITIONS: (e.g., whip pan, dissolve, morph cut)\nTEXTURE: (e.g., matte, glossy, grain, clean)\nTYPOGRAPHY: (font style + animation)\nREFERENCE VIBE: (1 sentence — like \"Apple WWDC meets Blade Runner\")\n\nProduct brief: " ++ brief

            visuals = call_swarm(prompt)
            print("[art_director] Visual direction ready")
            ets_put(elem(tables, 2), project_id, visuals)
            send(whereis("scene_collector"), {'spec_done', project_id, 'visuals'})
            art_director(tables)
    }
}

# ── Scene Collector (barrier) ──
# Waits for both script + visuals before proceeding.

fun scene_collector(tables) {
    receive {
        {'spec_done', project_id, spec_type} ->
            print("[collector] " ++ to_string(spec_type) ++ " done for project " ++ to_string(project_id))
            counter_key = {'spec_count', project_id}
            current = ets_get(elem(tables, 6), counter_key)
            count = 1
            if (current != nil) {
                count = elem(current, 0) + 1
            }
            ets_put(elem(tables, 6), counter_key, {count})
            if (count >= 2) {
                print("[collector] Script + visuals ready for project " ++ to_string(project_id))
                send(whereis("producer"), {'specs_ready', project_id})
            }
            scene_collector(tables)
    }
}

# ── The Scene Composer ──
# Merges script + visuals into detailed scene-by-scene breakdown.

fun scene_composer(tables) {
    receive {
        {'compose', project_id, tbls} ->
            print("[scene_composer] Composing scenes for project " ++ to_string(project_id))

            script = ets_get(elem(tbls, 1), project_id)
            visuals = ets_get(elem(tbls, 2), project_id)

            prompt = "You are a video scene composer. Merge this script and visual direction into exactly 4 detailed scenes. For each scene output:\n\nSCENE N: (title)\nDURATION: (seconds)\nVISUAL: (detailed description of what appears on screen — objects, environment, lighting, camera movement. Be VERY specific for AI video generation.)\nAUDIO: (narration text + any SFX notes)\nTEXT OVERLAY: (any on-screen text, or 'none')\nTRANSITION OUT: (how this scene ends)\n\nScript:\n" ++ string_truncate(to_string(script), 2000) ++ "\n\nVisual Direction:\n" ++ string_truncate(to_string(visuals), 1000)

            scenes = call_swarm_long(prompt)
            print("[scene_composer] 4 scenes composed (" ++ to_string(string_length(scenes)) ++ " chars)")
            ets_put(elem(tbls, 3), project_id, scenes)
            send(whereis("producer"), {'scenes_done', project_id})
            scene_composer(tables)

        {'revise', project_id, tbls} ->
            print("[scene_composer] Revising scenes for project " ++ to_string(project_id))

            feedback = ets_get(elem(tbls, 5), project_id)
            old_scenes = ets_get(elem(tbls, 3), project_id)

            prompt = "Revise these video scenes based on the feedback. Keep the same 4-scene format.\n\nFeedback: " ++ string_truncate(to_string(feedback), 500) ++ "\n\nCurrent scenes:\n" ++ string_truncate(to_string(old_scenes), 3000)

            scenes = call_swarm_long(prompt)
            ets_put(elem(tbls, 3), project_id, scenes)
            print("[scene_composer] Revision done")
            send(whereis("producer"), {'scenes_done', project_id})
            scene_composer(tables)
    }
}

# ── The Prompt Engineer ──
# Converts scenes into LTX-2 video generation prompts + render commands.

fun prompt_engineer(tables) {
    receive {
        {'generate', project_id, tbls} ->
            print("[prompt_engineer] Generating LTX-2 prompts for project " ++ to_string(project_id))

            scenes = ets_get(elem(tbls, 3), project_id)
            visuals = ets_get(elem(tbls, 2), project_id)

            prompt = "You are an expert at writing prompts for AI video generation models (LTX-Video). Convert these 4 scenes into LTX-2 compatible prompts. For each scene output:\n\nPROMPT_N: (A single detailed paragraph describing the video clip. Include: subject, action, environment, lighting, camera angle, camera movement, style, mood. Be cinematic and specific. 50-100 words.)\nNEGATIVE: (what to avoid — e.g., 'blurry, low quality, text, watermark')\nFRAMES: (number of frames at 25fps for the duration)\nGUIDANCE: (CFG scale 1-20, recommend 7-12)\n\nAlso output a RENDER section at the end:\nRENDER:\n```\n# Scene N\npython inference.py --prompt \"<prompt>\" --negative \"<negative>\" --num-frames <frames> --guidance-scale <guidance> --output scene_N.mp4\n```\n\nVisual Direction:\n" ++ string_truncate(to_string(visuals), 800) ++ "\n\nScenes:\n" ++ string_truncate(to_string(scenes), 3000)

            prompts = call_swarm_long(prompt)
            print("[prompt_engineer] LTX-2 prompts ready (" ++ to_string(string_length(prompts)) ++ " chars)")
            ets_put(elem(tbls, 4), project_id, prompts)

            # Write all outputs to files
            out_dir = "studio/output/video_" ++ to_string(project_id)
            file_mkdir(out_dir)

            script = ets_get(elem(tbls, 1), project_id)
            visuals_text = ets_get(elem(tbls, 2), project_id)
            scenes_text = ets_get(elem(tbls, 3), project_id)

            file_write(out_dir ++ "/script.md", to_string(script))
            file_write(out_dir ++ "/visual_direction.md", to_string(visuals_text))
            file_write(out_dir ++ "/scenes.md", to_string(scenes_text))
            file_write(out_dir ++ "/ltx2_prompts.md", prompts)
            print("[prompt_engineer] Files written to " ++ out_dir)

            send(whereis("producer"), {'prompts_done', project_id})
            prompt_engineer(tables)
    }
}

# ── The Critic ──
# Reviews the full package: script, visuals, scenes, prompts.

fun critic(tables) {
    receive {
        {'review', project_id, tbls} ->
            print("[critic] Reviewing project " ++ to_string(project_id))

            script = ets_get(elem(tbls, 1), project_id)
            scenes = ets_get(elem(tbls, 3), project_id)
            prompts = ets_get(elem(tbls, 4), project_id)

            review_msg = "Review this product announcement video package. Score each 1-10 and output ONLY valid JSON:\n{\"script_quality\":N,\"visual_coherence\":N,\"scene_flow\":N,\"prompt_clarity\":N,\"overall_impact\":N,\"average\":N.N,\"passed\":true/false,\"feedback\":\"...\"}\n\nPassed = average >= 7.\n\nScript:\n" ++ string_truncate(to_string(script), 800) ++ "\n\nScenes:\n" ++ string_truncate(to_string(scenes), 1500) ++ "\n\nLTX-2 Prompts:\n" ++ string_truncate(to_string(prompts), 1500)

            review = call_orc(review_msg)
            print("[critic] Review complete")

            avg = json_get(review, "average")
            passed = json_get(review, "passed")
            feedback = json_get(review, "feedback")

            ets_put(elem(tbls, 5), project_id, review)
            print("[critic] Score: " ++ to_string(avg) ++ " | Passed: " ++ to_string(passed))

            passed_atom = 'false'
            if (string_contains(to_string(passed), "true") == 'true') {
                passed_atom = 'true'
            }
            send(whereis("producer"), {'review_done', project_id, avg, passed_atom})
            critic(tables)
    }
}

# ── Keeper ──

fun keeper() {
    sleep(60000)
    keeper()
}

# ── Main ──

fun main() {
    print("")
    print("  ╔═══════════════════════════════════════════╗")
    print("  ║   V I D E O   S T U D I O   v0.1          ║")
    print("  ║   Product Announcement Generator           ║")
    print("  ║   6 agents — LTX-2 ready                   ║")
    print("  ║   SwarmRT / otonomy.ai                      ║")
    print("  ╚═══════════════════════════════════════════╝")
    print("")

    # ETS tables
    t_status   = ets_new()   # 0: project status
    t_scripts  = ets_new()   # 1: scripts
    t_visuals  = ets_new()   # 2: visual direction
    t_scenes   = ets_new()   # 3: scene breakdowns
    t_prompts  = ets_new()   # 4: LTX-2 prompts
    t_reviews  = ets_new()   # 5: critic reviews
    t_counters = ets_new()   # 6: barrier counters

    tables = {t_status, t_scripts, t_visuals, t_scenes, t_prompts, t_reviews, t_counters}
    print("[main] 7 ETS tables created")

    file_mkdir("studio/output")

    # Spawn agents
    pid_prod = spawn(producer(tables))
    pid_sw   = spawn(scriptwriter(tables))
    pid_ad   = spawn(art_director(tables))
    pid_sc   = spawn(scene_composer(tables))
    pid_pe   = spawn(prompt_engineer(tables))
    pid_cr   = spawn(critic(tables))
    pid_col  = spawn(scene_collector(tables))

    register("producer", pid_prod)
    register("scriptwriter", pid_sw)
    register("art_director", pid_ad)
    register("scene_composer", pid_sc)
    register("prompt_engineer", pid_pe)
    register("critic", pid_cr)
    register("scene_collector", pid_col)

    print("[main] 6 agents spawned:")
    print("  Producer          (coordinator)")
    print("  Scriptwriter      (orc)")
    print("  Art Director      (swarm)")
    print("  Scene Composer    (swarm)")
    print("  Prompt Engineer   (swarm)")
    print("  Critic            (orc)")
    print("")

    brief = "Announce SwarmRT — a BEAM-inspired native runtime for AI agents. Written in C, it spawns 100K+ lightweight processes with lock-free message passing at 10ns per send. Features: GenServer, Supervisors, hot code reload, per-process GC, ARM64 assembly context switching. Compiles .sw source to native binaries. Built for autonomous multi-agent systems that need Erlang's fault tolerance at C's speed. By Otonomy."

    print("[main] Product brief:")
    print("  " ++ string_truncate(brief, 120) ++ "...")
    print("")
    print("[main] Dispatching to producer...")
    send(pid_prod, {'brief', 1, brief})

    spawn(keeper())
    keeper()
}
