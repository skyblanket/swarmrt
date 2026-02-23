# Autonomous Research Lab — SwarmRT
# 5 AI agents: Director, 2 Researchers, Analyst, Editor
# Takes a question, investigates in parallel, synthesizes, produces a polished report.
# otonomy.ai — 2026

module ResearchLab

export [main, director, researcher_a, researcher_b, analyst, editor, findings_collector, keeper]

# ── LLM Helpers ──
# string_truncate() is now a runtime builtin.

fun call_orc(prompt) {
    llm_complete(prompt, %{model: "otonomy-orc", max_tokens: 4096, temperature: 0.7})
}

fun call_swarm(prompt) {
    llm_complete(prompt, %{model: "otonomy-swarm", max_tokens: 4096, temperature: 0.7})
}

# ── The Director ──
# Receives question, decomposes into 2 sub-questions, coordinates pipeline.

fun director(tables) {
    receive {
        {'question', project_id, question} ->
            print("[director] Research question received for project " ++ to_string(project_id))
            print("[director] Q: " ++ string_truncate(question, 120))

            decompose_prompt = "You are a research director. Given this research question, decompose it into exactly 2 focused sub-questions for parallel investigation by two researchers. The sub-questions should cover different complementary angles. Output ONLY valid JSON with no markdown fences: {\"sub_q1\": \"...\", \"sub_q2\": \"...\"}\n\nQuestion: " ++ question
            raw = call_orc(decompose_prompt)
            print("[director] Decomposition complete")

            sub_q1 = json_get(raw, "sub_q1")
            sub_q2 = json_get(raw, "sub_q2")

            # Fallback if JSON parsing fails
            if (sub_q1 == nil) {
                sub_q1 = "Analyze the key factors and mechanisms related to: " ++ question
            }
            if (sub_q2 == nil) {
                sub_q2 = "Analyze the implications and future outlook for: " ++ question
            }

            print("[director] Sub-Q1: " ++ string_truncate(to_string(sub_q1), 100))
            print("[director] Sub-Q2: " ++ string_truncate(to_string(sub_q2), 100))

            ets_put(elem(tables, 0), project_id, {'researching', 0})

            # Dispatch to researchers in parallel
            send(whereis("researcher_a"), {'research', project_id, sub_q1, question})
            send(whereis("researcher_b"), {'research', project_id, sub_q2, question})
            director(tables)

        {'findings_ready', project_id} ->
            print("[director] Both findings received. Sending to analyst.")
            ets_put(elem(tables, 0), project_id, {'analyzing', 0})
            send(whereis("analyst"), {'analyze', project_id})
            director(tables)

        {'analysis_done', project_id} ->
            print("[director] Analysis complete. Sending to editor.")
            ets_put(elem(tables, 0), project_id, {'editing', 0})
            send(whereis("editor"), {'edit', project_id})
            director(tables)

        {'report_done', project_id, out_file} ->
            print("")
            print("[director] ════════════════════════════════════════")
            print("[director]  SHIPPED — project " ++ to_string(project_id))
            print("[director]  Report: " ++ to_string(out_file))
            print("[director] ════════════════════════════════════════")
            print("")
            ets_put(elem(tables, 0), project_id, {'shipped', 0})
            director(tables)
    }
}

# ── Researcher A ──

fun researcher_a(tables) {
    receive {
        {'research', project_id, sub_question, original_q} ->
            print("[researcher_a] Investigating: " ++ string_truncate(to_string(sub_question), 80))

            prompt = "You are a deep research analyst (Researcher A). Provide a thorough 300-500 word analysis of the following sub-question. Include key findings, evidence, reasoning, and specific examples.\n\nOriginal research question: " ++ to_string(original_q) ++ "\n\nYour assigned sub-question: " ++ to_string(sub_question)
            findings = call_swarm(prompt)

            print("[researcher_a] Findings ready (" ++ to_string(string_length(findings)) ++ " chars)")
            ets_put(elem(tables, 1), {'finding_a', project_id}, findings)
            send(whereis("findings_collector"), {'finding_done', project_id, 'a'})
            researcher_a(tables)
    }
}

# ── Researcher B ──

fun researcher_b(tables) {
    receive {
        {'research', project_id, sub_question, original_q} ->
            print("[researcher_b] Investigating: " ++ string_truncate(to_string(sub_question), 80))

            prompt = "You are a deep research analyst (Researcher B). Provide a thorough 300-500 word analysis of the following sub-question. Include key findings, evidence, reasoning, and specific examples.\n\nOriginal research question: " ++ to_string(original_q) ++ "\n\nYour assigned sub-question: " ++ to_string(sub_question)
            findings = call_swarm(prompt)

            print("[researcher_b] Findings ready (" ++ to_string(string_length(findings)) ++ " chars)")
            ets_put(elem(tables, 1), {'finding_b', project_id}, findings)
            send(whereis("findings_collector"), {'finding_done', project_id, 'b'})
            researcher_b(tables)
    }
}

# ── Findings Collector (barrier on 2 researchers) ──

fun findings_collector(tables) {
    receive {
        {'finding_done', project_id, researcher} ->
            print("[collector] Finding from researcher " ++ to_string(researcher) ++ " for project " ++ to_string(project_id))
            counter_key = {'finding_count', project_id}
            current = ets_get(elem(tables, 1), counter_key)
            count = 1
            if (current != nil) {
                count = elem(current, 0) + 1
            }
            ets_put(elem(tables, 1), counter_key, {count})
            if (count >= 2) {
                print("[collector] Both findings ready for project " ++ to_string(project_id))
                send(whereis("director"), {'findings_ready', project_id})
            }
            findings_collector(tables)
    }
}

# ── The Analyst ──
# Synthesizes findings from both researchers.

fun analyst(tables) {
    receive {
        {'analyze', project_id} ->
            print("[analyst] Synthesizing findings for project " ++ to_string(project_id))

            finding_a = ets_get(elem(tables, 1), {'finding_a', project_id})
            finding_b = ets_get(elem(tables, 1), {'finding_b', project_id})

            prompt = "You are a research analyst. Synthesize these two independent research findings into a unified analysis of 400-600 words. Identify patterns, contradictions, complementary insights, and draw connections. Highlight the most important takeaways.\n\nResearcher A's findings:\n" ++ string_truncate(to_string(finding_a), 1500) ++ "\n\nResearcher B's findings:\n" ++ string_truncate(to_string(finding_b), 1500)
            analysis = call_orc(prompt)

            print("[analyst] Synthesis complete (" ++ to_string(string_length(analysis)) ++ " chars)")
            ets_put(elem(tables, 2), project_id, analysis)
            send(whereis("director"), {'analysis_done', project_id})
            analyst(tables)
    }
}

# ── The Editor ──
# Polishes synthesis into a final markdown report and writes to file.

fun editor(tables) {
    receive {
        {'edit', project_id} ->
            print("[editor] Polishing report for project " ++ to_string(project_id))

            finding_a = ets_get(elem(tables, 1), {'finding_a', project_id})
            finding_b = ets_get(elem(tables, 1), {'finding_b', project_id})
            analysis = ets_get(elem(tables, 2), project_id)

            prompt = "You are a professional research report editor. Using the synthesis and raw findings below, produce a polished final research report in markdown format with these sections:\n\n# Executive Summary\n(2-3 concise sentences)\n\n# Key Findings\n(Bulleted list of 4-6 key findings)\n\n# Detailed Analysis\n(The core synthesis, 400-600 words, well-structured with subheadings)\n\n# Conclusions & Implications\n(3-4 sentences on what this means going forward)\n\n---\n\nSynthesis:\n" ++ string_truncate(to_string(analysis), 2000) ++ "\n\nRaw Finding A:\n" ++ string_truncate(to_string(finding_a), 800) ++ "\n\nRaw Finding B:\n" ++ string_truncate(to_string(finding_b), 800)
            report = call_swarm(prompt)

            print("[editor] Report polished (" ++ to_string(string_length(report)) ++ " chars)")

            out_dir = "studio/output/research"
            file_mkdir(out_dir)
            out_file = out_dir ++ "/report_" ++ to_string(project_id) ++ ".md"
            file_write(out_file, report)
            print("[editor] Written to " ++ out_file)

            ets_put(elem(tables, 3), project_id, report)
            send(whereis("director"), {'report_done', project_id, out_file})
            editor(tables)
    }
}

# ── Keeper (prevents VM exit) ──

fun keeper() {
    sleep(60000)
    keeper()
}

# ── Main ──

fun main() {
    print("")
    print("  ╔═══════════════════════════════════════════╗")
    print("  ║   AUTONOMOUS RESEARCH LAB  v0.1           ║")
    print("  ║   5 agents — parallel investigation       ║")
    print("  ║   SwarmRT / otonomy.ai                    ║")
    print("  ╚═══════════════════════════════════════════╝")
    print("")

    # Create ETS tables
    t_status   = ets_new()
    t_findings = ets_new()
    t_analysis = ets_new()
    t_reports  = ets_new()

    tables = {t_status, t_findings, t_analysis, t_reports}
    print("[main] 4 ETS tables created")

    file_mkdir("studio/output")
    file_mkdir("studio/output/research")

    # Spawn all agents
    pid_d  = spawn(director(tables))
    pid_ra = spawn(researcher_a(tables))
    pid_rb = spawn(researcher_b(tables))
    pid_an = spawn(analyst(tables))
    pid_ed = spawn(editor(tables))
    pid_fc = spawn(findings_collector(tables))

    register("director", pid_d)
    register("researcher_a", pid_ra)
    register("researcher_b", pid_rb)
    register("analyst", pid_an)
    register("editor", pid_ed)
    register("findings_collector", pid_fc)

    print("[main] 5 agents + collector spawned:")
    print("  Director        (otonomy-orc)")
    print("  Researcher A    (otonomy-swarm)")
    print("  Researcher B    (otonomy-swarm)")
    print("  Analyst         (otonomy-orc)")
    print("  Editor          (otonomy-swarm)")
    print("")

    question = "What are the most promising actuator technologies and ideas for next-generation robotics? Consider novel materials, bio-inspired designs, soft actuators, electrohydraulic systems, shape-memory alloys, and any emerging approaches that could dramatically improve dexterity, force density, or energy efficiency in robotic systems."

    print("[main] Research question:")
    print("  " ++ question)
    print("")
    print("[main] Dispatching to director...")
    send(pid_d, {'question', 1, question})

    spawn(keeper())
    keeper()
}
