# Patent Research Lab v0.3 — SwarmRT Multi-Agent Pipeline
# 16 agents, 4 stages + real-time web tools
# Agents can SEARCH the web, FETCH pages, and search PATENTS autonomously.
# Research → Validate → Patent → Commercialize
# otonomy.ai — 2026
#
# v0.3 fixes: anti-CoT prompts, retry wrapper, clean_json, orc_long for drafter,
#   versioned patent files, increased truncation, targeted revision loop,
#   stronger validator gate, final dashboard summary.

module PatentLab

export [main, director, tool_agent, search_worker, wait_all_3, research_agent,
        researcher_tech, researcher_market, researcher_prior_art,
        findings_collector, analyst, research_editor,
        peer_reviewer, novelty_assessor, validation_collector, validator,
        patent_strategist, patent_drafter, patent_reviewer,
        market_analyst, pitch_writer, keeper]

# ── LLM Helpers ──
# strip_html(), clean_json(), string_truncate() are now runtime builtins.
# llm_complete() now supports retries & min_chars in opts map.

fun call_orc(prompt) {
    llm_complete(prompt, %{model: "otonomy-orc", max_tokens: 4096, temperature: 0.7})
}

fun call_swarm(prompt) {
    llm_complete(prompt, %{model: "otonomy-swarm", max_tokens: 4096, temperature: 0.7})
}

fun call_llm(prompt, model) {
    result = ""
    if (model == 'orc') { result = call_orc(prompt) }
    if (model == 'swarm') { result = call_swarm(prompt) }
    if (model == 'orc_long') { result = llm_complete(prompt, %{model: "otonomy-orc", max_tokens: 16384, temperature: 0.7}) }
    if (model == 'swarm_long') { result = llm_complete(prompt, %{model: "otonomy-swarm", max_tokens: 16384, temperature: 0.7}) }
    result
}

fun call_with_retry(prompt, model, retries) {
    result = ""
    if (model == 'orc') { result = llm_complete(prompt, %{model: "otonomy-orc", max_tokens: 4096, retries: retries}) }
    if (model == 'swarm') { result = llm_complete(prompt, %{model: "otonomy-swarm", max_tokens: 4096, retries: retries}) }
    if (model == 'orc_long') { result = llm_complete(prompt, %{model: "otonomy-orc", max_tokens: 16384, retries: retries}) }
    if (model == 'swarm_long') { result = llm_complete(prompt, %{model: "otonomy-swarm", max_tokens: 16384, retries: retries}) }
    result
}

# ══════════════════════════════════════════════════════════════════════════════
# WEB TOOL HELPERS
# ══════════════════════════════════════════════════════════════════════════════

fun web_search(query) {
    print("[tool] SEARCH: " ++ string_truncate(to_string(query), 60))
    encoded = string_replace(string_replace(string_replace(to_string(query), " ", "+"), "&", "%26"), "#", "%23")
    url = "https://html.duckduckgo.com/html/?q=" ++ encoded
    raw = http_get(url)
    result = strip_html(raw)
    if (string_length(result) < 20) {
        result = "Search returned no usable results."
    }
    print("[tool] Search returned " ++ to_string(string_length(result)) ++ " chars")
    result
}

fun fetch_url(url) {
    print("[tool] FETCH: " ++ string_truncate(to_string(url), 80))
    safe_url = string_replace(string_replace(to_string(url), "'", ""), "\"", "")
    raw = http_get(safe_url)
    result = strip_html(raw)
    if (string_length(result) < 20) {
        result = "Page could not be fetched or was empty."
    }
    print("[tool] Fetched " ++ to_string(string_length(result)) ++ " chars")
    result
}

fun search_patents(query) {
    print("[tool] PATENTS: " ++ string_truncate(to_string(query), 60))
    web_search(to_string(query) ++ " patent site:patents.google.com OR site:uspto.gov")
}

fun search_scholar(query) {
    print("[tool] SCHOLAR: " ++ string_truncate(to_string(query), 60))
    web_search(to_string(query) ++ " research paper site:arxiv.org OR site:scholar.google.com")
}

# ══════════════════════════════════════════════════════════════════════════════
# AGENTIC TOOL LOOP — ReAct-style recursive tool use
# ══════════════════════════════════════════════════════════════════════════════

fun extract_tool_arg(text, marker) {
    parts = string_split(to_string(text), marker)
    result = ""
    if (length(parts) > 1) {
        rest = hd(tl(parts))
        lines = string_split(rest, "\n")
        result = string_trim(hd(lines))
    }
    result
}

fun tool_agent(system, task, depth, max_depth, model) {
    tools_prompt = "\n\nYou have real-time web research tools. To use one, write the command on its own line:\n  SEARCH: your search query\n  FETCH: https://full-url-to-read\n  PATENTS: patent search terms\n  SCHOLAR: academic paper search terms\n\nUse ONE tool per response. After seeing results, you'll get another turn.\nWhen you have enough data, write your FINAL analysis directly — no tool command, no planning notes, no meta-commentary. Output ONLY the final content."

    prompt = system ++ tools_prompt ++ "\n\nTask:\n" ++ task
    response = call_llm(prompt, model)

    result = response
    has_search = string_contains(response, "SEARCH: ")
    has_fetch = string_contains(response, "FETCH: ")
    has_patents = string_contains(response, "PATENTS: ")
    has_scholar = string_contains(response, "SCHOLAR: ")
    used_tool = 'false'

    query = ""
    tool_result = ""
    new_task = ""
    url = ""

    if (has_search == 'true') {
        if (depth < max_depth) {
            query = extract_tool_arg(response, "SEARCH: ")
            tool_result = web_search(query)
            new_task = task ++ "\n\n--- Your previous response ---\n" ++ string_truncate(response, 500) ++ "\n\n--- Search results for '" ++ string_truncate(query, 40) ++ "' ---\n" ++ string_truncate(tool_result, 3000)
            result = tool_agent(system, new_task, depth + 1, max_depth, model)
            used_tool = 'true'
        }
    }

    if (has_fetch == 'true') {
        if (used_tool != 'true') {
            if (depth < max_depth) {
                url = extract_tool_arg(response, "FETCH: ")
                tool_result = fetch_url(url)
                new_task = task ++ "\n\n--- Your previous response ---\n" ++ string_truncate(response, 500) ++ "\n\n--- Content from " ++ string_truncate(url, 60) ++ " ---\n" ++ string_truncate(tool_result, 3000)
                result = tool_agent(system, new_task, depth + 1, max_depth, model)
                used_tool = 'true'
            }
        }
    }

    if (has_patents == 'true') {
        if (used_tool != 'true') {
            if (depth < max_depth) {
                query = extract_tool_arg(response, "PATENTS: ")
                tool_result = search_patents(query)
                new_task = task ++ "\n\n--- Your previous response ---\n" ++ string_truncate(response, 500) ++ "\n\n--- Patent results for '" ++ string_truncate(query, 40) ++ "' ---\n" ++ string_truncate(tool_result, 3000)
                result = tool_agent(system, new_task, depth + 1, max_depth, model)
                used_tool = 'true'
            }
        }
    }

    if (has_scholar == 'true') {
        if (used_tool != 'true') {
            if (depth < max_depth) {
                query = extract_tool_arg(response, "SCHOLAR: ")
                tool_result = search_scholar(query)
                new_task = task ++ "\n\n--- Your previous response ---\n" ++ string_truncate(response, 500) ++ "\n\n--- Academic results for '" ++ string_truncate(query, 40) ++ "' ---\n" ++ string_truncate(tool_result, 3000)
                result = tool_agent(system, new_task, depth + 1, max_depth, model)
            }
        }
    }

    result
}

# ══════════════════════════════════════════════════════════════════════════════
# PARALLEL RESEARCH AGENT — Plan → Parallel Search → Analyze
# 2 LLM calls instead of 3-4. Searches run as spawned processes in parallel.
# Much faster than sequential ReAct for data-gathering tasks.
# ══════════════════════════════════════════════════════════════════════════════

# Worker process: executes one search, writes result to ETS
fun search_worker(table, key, query) {
    result = ""
    if (string_length(to_string(query)) > 5) {
        result = web_search(to_string(query))
    }
    ets_put(table, key, result)
}

# Poll ETS until all 3 search results are ready
fun wait_all_3(table) {
    v1 = ets_get(table, 'r1')
    v2 = ets_get(table, 'r2')
    v3 = ets_get(table, 'r3')
    all_done = 'false'
    if (v1 != nil) {
        if (v2 != nil) {
            if (v3 != nil) {
                all_done = 'true'
            }
        }
    }
    if (all_done != 'true') {
        sleep(1000)
        wait_all_3(table)
    }
}

# Plan-Search-Analyze: 1 LLM (plan queries) → 3 parallel searches → 1 LLM (analyze)
fun research_agent(system, task, model) {
    # Phase 1: LLM generates 3 targeted search queries
    plan_prompt = system ++ "\n\nBefore researching, generate exactly 3 focused search queries that will find the most relevant real-time data. Vary the queries: one broad, one technical/specific, one targeting companies or patents.\n\nOutput ONLY valid JSON:\n{\"q1\": \"...\", \"q2\": \"...\", \"q3\": \"...\"}\n\nTask: " ++ task
    raw = call_llm(plan_prompt, model)

    q1 = json_get(raw, "q1")
    q2 = json_get(raw, "q2")
    q3 = json_get(raw, "q3")
    if (q1 == nil) { q1 = task }
    if (q2 == nil) { q2 = "" }
    if (q3 == nil) { q3 = "" }

    print("[research_agent] 3 parallel searches:")
    print("[research_agent]   Q1: " ++ string_truncate(to_string(q1), 60))
    print("[research_agent]   Q2: " ++ string_truncate(to_string(q2), 60))
    print("[research_agent]   Q3: " ++ string_truncate(to_string(q3), 60))

    # Phase 2: Execute all 3 searches as spawned parallel processes
    t_search = ets_new()
    spawn(search_worker(t_search, 'r1', q1))
    spawn(search_worker(t_search, 'r2', q2))
    spawn(search_worker(t_search, 'r3', q3))

    wait_all_3(t_search)
    print("[research_agent] All 3 searches complete. Analyzing...")

    r1 = ets_get(t_search, 'r1')
    r2 = ets_get(t_search, 'r2')
    r3 = ets_get(t_search, 'r3')
    if (r1 == nil) { r1 = "" }
    if (r2 == nil) { r2 = "" }
    if (r3 == nil) { r3 = "" }

    # Phase 3: Single LLM call with all gathered data
    analysis_prompt = system ++ "\n\nUse these web research results to inform your analysis. Cite specific findings and data.\n\nSearch 1 (" ++ string_truncate(to_string(q1), 40) ++ "):\n" ++ string_truncate(to_string(r1), 2000) ++ "\n\nSearch 2 (" ++ string_truncate(to_string(q2), 40) ++ "):\n" ++ string_truncate(to_string(r2), 2000) ++ "\n\nSearch 3 (" ++ string_truncate(to_string(q3), 40) ++ "):\n" ++ string_truncate(to_string(r3), 2000) ++ "\n\nTask:\n" ++ task
    call_llm(analysis_prompt, model)
}

# ══════════════════════════════════════════════════════════════════════════════
# THE DIRECTOR
# ══════════════════════════════════════════════════════════════════════════════

fun director(tables) {
    receive {
        {'question', project_id, question} ->
            print("[director] Research question received for project " ++ to_string(project_id))
            print("[director] Q: " ++ string_truncate(question, 120))
            ets_put(elem(tables, 0), {'question', project_id}, question)
            ets_put(elem(tables, 0), {'val_rev', project_id}, {0})
            ets_put(elem(tables, 0), {'pat_rev', project_id}, {0})

            decompose_prompt = "You are a research director preparing a patent investigation. Given this research question, decompose it into exactly 3 focused sub-questions for parallel investigation:\n1. Technical/engineering angle\n2. Commercial/market angle\n3. Prior art angle\n\nOutput ONLY valid JSON with no markdown fences:\n{\"tech_q\": \"...\", \"market_q\": \"...\", \"prior_art_q\": \"...\"}\n\nQuestion: " ++ question
            raw = clean_json(call_orc(decompose_prompt))
            print("[director] Decomposition complete")

            tech_q = json_get(raw, "tech_q")
            market_q = json_get(raw, "market_q")
            prior_art_q = json_get(raw, "prior_art_q")
            if (tech_q == nil) { tech_q = "Analyze the key technical mechanisms and engineering challenges related to: " ++ question }
            if (market_q == nil) { market_q = "Analyze the commercial opportunity and market landscape for: " ++ question }
            if (prior_art_q == nil) { prior_art_q = "Survey existing patents, academic literature, and prior art related to: " ++ question }

            print("[director] Tech Q: " ++ string_truncate(to_string(tech_q), 80))
            print("[director] Market Q: " ++ string_truncate(to_string(market_q), 80))
            print("[director] Prior Art Q: " ++ string_truncate(to_string(prior_art_q), 80))

            ets_put(elem(tables, 0), project_id, {'researching'})
            ets_put(elem(tables, 1), {'finding_count', project_id}, {0})
            send(whereis("researcher_tech"), {'research', project_id, tech_q, question})
            send(whereis("researcher_market"), {'research', project_id, market_q, question})
            send(whereis("researcher_prior_art"), {'research', project_id, prior_art_q, question})
            director(tables)

        {'findings_ready', project_id} ->
            print("[director] All 3 findings received. Sending to analyst.")
            ets_put(elem(tables, 0), project_id, {'analyzing'})
            send(whereis("analyst"), {'analyze', project_id})
            director(tables)

        {'analysis_done', project_id} ->
            print("[director] Analysis complete. Sending to research editor.")
            ets_put(elem(tables, 0), project_id, {'editing_research'})
            send(whereis("research_editor"), {'edit', project_id})
            director(tables)

        {'research_report_done', project_id} ->
            print("[director] Research report written. Starting validation (Stage 2).")
            ets_put(elem(tables, 0), project_id, {'validating'})
            ets_put(elem(tables, 3), {'val_count', project_id}, {0})
            send(whereis("peer_reviewer"), {'review', project_id})
            send(whereis("novelty_assessor"), {'assess', project_id})
            director(tables)

        {'validations_ready', project_id} ->
            print("[director] Both validations received. Sending to validator.")
            ets_put(elem(tables, 0), project_id, {'checking_validation'})
            send(whereis("validator"), {'validate', project_id})
            director(tables)

        {'validation_complete', project_id, decision} ->
            if (decision == 'go') {
                print("[director] Validation PASSED. Starting patent strategy (Stage 3).")
                ets_put(elem(tables, 0), project_id, {'strategizing'})
                send(whereis("patent_strategist"), {'strategize', project_id})
            } else {
                current = ets_get(elem(tables, 0), {'val_rev', project_id})
                val_rev = 0
                if (current != nil) { val_rev = elem(current, 0) }
                if (val_rev < 2) {
                    print("[director] Validation REJECTED. Revision " ++ to_string(val_rev + 1) ++ " — targeted report revision.")
                    ets_put(elem(tables, 0), {'val_rev', project_id}, {val_rev + 1})
                    ets_put(elem(tables, 0), project_id, {'revising_research'})
                    send(whereis("research_editor"), {'revise_report', project_id})
                } else {
                    print("[director] Validation REJECTED — max revisions. Proceeding to patent.")
                    ets_put(elem(tables, 0), project_id, {'strategizing'})
                    send(whereis("patent_strategist"), {'strategize', project_id})
                }
            }
            director(tables)

        {'strategy_done', project_id} ->
            print("[director] Patent strategy complete. Sending to drafter.")
            ets_put(elem(tables, 0), project_id, {'drafting_patent'})
            send(whereis("patent_drafter"), {'draft', project_id})
            director(tables)

        {'patent_drafted', project_id} ->
            print("[director] Patent draft complete. Sending to reviewer.")
            ets_put(elem(tables, 0), project_id, {'reviewing_patent'})
            send(whereis("patent_reviewer"), {'review_patent', project_id})
            director(tables)

        {'patent_reviewed', project_id, avg_score, passed} ->
            if (passed == 'true') {
                print("[director] Patent APPROVED (" ++ to_string(avg_score) ++ "). Starting commercialization (Stage 4).")
                ets_put(elem(tables, 0), project_id, {'analyzing_market'})
                send(whereis("market_analyst"), {'analyze_market', project_id})
            } else {
                current = ets_get(elem(tables, 0), {'pat_rev', project_id})
                pat_rev = 0
                if (current != nil) { pat_rev = elem(current, 0) }
                if (pat_rev < 2) {
                    print("[director] Patent score " ++ to_string(avg_score) ++ "/10. Revision " ++ to_string(pat_rev + 1) ++ ".")
                    ets_put(elem(tables, 0), {'pat_rev', project_id}, {pat_rev + 1})
                    ets_put(elem(tables, 0), project_id, {'drafting_patent'})
                    send(whereis("patent_drafter"), {'revise_patent', project_id})
                } else {
                    print("[director] Patent score " ++ to_string(avg_score) ++ "/10. Max revisions. Proceeding.")
                    ets_put(elem(tables, 0), project_id, {'analyzing_market'})
                    send(whereis("market_analyst"), {'analyze_market', project_id})
                }
            }
            director(tables)

        {'market_done', project_id} ->
            print("[director] Market analysis complete. Sending to pitch writer.")
            ets_put(elem(tables, 0), project_id, {'writing_pitch'})
            send(whereis("pitch_writer"), {'write_pitch', project_id})
            director(tables)

        {'pitch_done', project_id} ->
            pat_review = ets_get(elem(tables, 4), {'review', project_id})
            pat_rev_entry = ets_get(elem(tables, 0), {'pat_rev', project_id})
            val_rev_entry = ets_get(elem(tables, 0), {'val_rev', project_id})
            novelty_s = "N/A"
            breadth_s = "N/A"
            defensibility_s = "N/A"
            avg_s = "N/A"
            if (pat_review != nil) {
                cleaned = clean_json(pat_review)
                novelty_s = to_string(json_get(cleaned, "novelty"))
                breadth_s = to_string(json_get(cleaned, "breadth"))
                defensibility_s = to_string(json_get(cleaned, "defensibility"))
                avg_s = to_string(json_get(cleaned, "average"))
            }
            val_revs = 0
            if (val_rev_entry != nil) { val_revs = elem(val_rev_entry, 0) }
            pat_revs = 0
            if (pat_rev_entry != nil) { pat_revs = elem(pat_rev_entry, 0) }
            out_dir = "studio/output/patents/project_" ++ to_string(project_id)
            print("")
            print("[director] ════════════════════════════════════════════════════")
            print("[director]  SHIPPED — Patent Pipeline Complete")
            print("[director]  Project " ++ to_string(project_id))
            print("[director] ────────────────────────────────────────────────────")
            print("[director]  Patent Scores:")
            print("[director]    Novelty:        " ++ novelty_s ++ "/10")
            print("[director]    Breadth:        " ++ breadth_s ++ "/10")
            print("[director]    Defensibility:  " ++ defensibility_s ++ "/10")
            print("[director]    Average:        " ++ avg_s ++ "/10")
            print("[director] ────────────────────────────────────────────────────")
            print("[director]  Revisions: validation " ++ to_string(val_revs) ++ "/2, patent " ++ to_string(pat_revs) ++ "/2")
            print("[director] ────────────────────────────────────────────────────")
            print("[director]  Output: " ++ out_dir ++ "/")
            print("[director]    research_report.md, validation_report.md,")
            print("[director]    patent_draft.md (+ versioned), patent_review.md,")
            print("[director]    market_analysis.md, pitch.md")
            print("[director] ════════════════════════════════════════════════════")
            print("")
            ets_put(elem(tables, 0), project_id, {'shipped'})
            director(tables)
    }
}

# ══════════════════════════════════════════════════════════════════════════════
# STAGE 1: RESEARCH
# ══════════════════════════════════════════════════════════════════════════════

fun researcher_tech(tables) {
    receive {
        {'research', project_id, sub_question, original_q} ->
            print("[researcher_tech] Investigating: " ++ string_truncate(to_string(sub_question), 80))
            system = "You are a deep technical research analyst. Provide a thorough 400-600 word analysis citing specific data from the web research provided. Include technical details, mechanisms, materials, and feasibility."
            task = "Original question: " ++ to_string(original_q) ++ "\n\nSub-question: " ++ to_string(sub_question)
            findings = research_agent(system, task, 'swarm')
            print("[researcher_tech] Findings ready (" ++ to_string(string_length(findings)) ++ " chars)")
            ets_put(elem(tables, 1), {'finding_tech', project_id}, findings)
            send(whereis("findings_collector"), {'finding_done', project_id, 'tech'})
            researcher_tech(tables)
    }
}

fun researcher_market(tables) {
    receive {
        {'research', project_id, sub_question, original_q} ->
            print("[researcher_market] Investigating: " ++ string_truncate(to_string(sub_question), 80))
            system = "You are a market research analyst. Provide a thorough 400-600 word analysis citing real market data from the web research provided. Include market size, key players, growth trends, competitive dynamics."
            task = "Original question: " ++ to_string(original_q) ++ "\n\nSub-question: " ++ to_string(sub_question)
            findings = research_agent(system, task, 'swarm')
            print("[researcher_market] Findings ready (" ++ to_string(string_length(findings)) ++ " chars)")
            ets_put(elem(tables, 1), {'finding_market', project_id}, findings)
            send(whereis("findings_collector"), {'finding_done', project_id, 'market'})
            researcher_market(tables)
    }
}

fun researcher_prior_art(tables) {
    receive {
        {'research', project_id, sub_question, original_q} ->
            print("[researcher_prior_art] Investigating: " ++ string_truncate(to_string(sub_question), 80))
            system = "You are a patent research specialist. Provide a thorough 400-600 word analysis citing real patent numbers, assignees, and academic references from the web research provided. Identify gaps in existing IP landscape."
            task = "Original question: " ++ to_string(original_q) ++ "\n\nSub-question: " ++ to_string(sub_question)
            findings = research_agent(system, task, 'swarm')
            print("[researcher_prior_art] Findings ready (" ++ to_string(string_length(findings)) ++ " chars)")
            ets_put(elem(tables, 1), {'finding_prior_art', project_id}, findings)
            send(whereis("findings_collector"), {'finding_done', project_id, 'prior_art'})
            researcher_prior_art(tables)
    }
}

fun findings_collector(tables) {
    receive {
        {'finding_done', project_id, researcher} ->
            print("[collector] " ++ to_string(researcher) ++ " done for project " ++ to_string(project_id))
            counter_key = {'finding_count', project_id}
            current = ets_get(elem(tables, 1), counter_key)
            count = 1
            if (current != nil) { count = elem(current, 0) + 1 }
            ets_put(elem(tables, 1), counter_key, {count})
            if (count >= 3) {
                print("[collector] All 3 findings ready for project " ++ to_string(project_id))
                send(whereis("director"), {'findings_ready', project_id})
            }
            findings_collector(tables)
    }
}

fun analyst(tables) {
    receive {
        {'analyze', project_id} ->
            print("[analyst] Synthesizing findings for project " ++ to_string(project_id))
            finding_tech = ets_get(elem(tables, 1), {'finding_tech', project_id})
            finding_market = ets_get(elem(tables, 1), {'finding_market', project_id})
            finding_prior_art = ets_get(elem(tables, 1), {'finding_prior_art', project_id})
            prompt = "You are a senior research analyst. Synthesize these three findings into a unified 600-800 word analysis. Preserve specific numbers, patent references, and data points. Identify patterns across technical, market, and prior art dimensions.\n\nCRITICAL: Output ONLY the final synthesis. No planning notes, no thinking, no word counts. Start directly with the analysis.\n\nTechnical:\n" ++ string_truncate(to_string(finding_tech), 2000) ++ "\n\nMarket:\n" ++ string_truncate(to_string(finding_market), 2000) ++ "\n\nPrior art:\n" ++ string_truncate(to_string(finding_prior_art), 2000)
            analysis = call_with_retry(prompt, 'orc', 1)
            print("[analyst] Synthesis complete (" ++ to_string(string_length(analysis)) ++ " chars)")
            ets_put(elem(tables, 2), project_id, analysis)
            send(whereis("director"), {'analysis_done', project_id})
            analyst(tables)
    }
}

fun research_editor(tables) {
    receive {
        {'edit', project_id} ->
            print("[research_editor] Polishing report for project " ++ to_string(project_id))
            finding_tech = ets_get(elem(tables, 1), {'finding_tech', project_id})
            finding_market = ets_get(elem(tables, 1), {'finding_market', project_id})
            finding_prior_art = ets_get(elem(tables, 1), {'finding_prior_art', project_id})
            analysis = ets_get(elem(tables, 2), project_id)
            prompt = "You are a professional research report editor. Produce a polished report in markdown. Preserve all citations, patent numbers, market figures.\n\nSections:\n# Executive Summary\n(3-4 sentences)\n# Technical Landscape\n(200-300 words)\n# Market Opportunity\n(200-300 words)\n# Prior Art & Patent Landscape\n(200-300 words)\n# Synthesis & Key Insights\n(300-400 words)\n# Patentability Assessment\n(100-200 words)\n\nCRITICAL: Output ONLY the final markdown report. No planning notes, no word count checks, no revision drafts, no thinking. Start with '# Executive Summary'.\n\nSynthesis:\n" ++ string_truncate(to_string(analysis), 2500) ++ "\n\nTechnical:\n" ++ string_truncate(to_string(finding_tech), 1200) ++ "\n\nMarket:\n" ++ string_truncate(to_string(finding_market), 1200) ++ "\n\nPrior art:\n" ++ string_truncate(to_string(finding_prior_art), 1200)
            report = call_with_retry(prompt, 'swarm', 1)
            print("[research_editor] Report polished (" ++ to_string(string_length(report)) ++ " chars)")
            out_dir = "studio/output/patents/project_" ++ to_string(project_id)
            file_mkdir(out_dir)
            file_write(out_dir ++ "/research_report.md", report)
            print("[research_editor] Written to " ++ out_dir ++ "/research_report.md")
            ets_put(elem(tables, 2), {'report', project_id}, report)
            send(whereis("director"), {'research_report_done', project_id})
            research_editor(tables)

        {'revise_report', project_id} ->
            print("[research_editor] Targeted revision for project " ++ to_string(project_id))
            old_report = ets_get(elem(tables, 2), {'report', project_id})
            peer_review = ets_get(elem(tables, 3), {'peer_review', project_id})
            novelty = ets_get(elem(tables, 3), {'novelty', project_id})
            val_decision = ets_get(elem(tables, 3), {'validator_decision', project_id})
            prompt = "You are a research report editor performing a TARGETED REVISION. Address every concern from the peer review and novelty assessment. Strengthen weak areas. Fill gaps. Same markdown structure.\n\nSections: Executive Summary, Technical Landscape, Market Opportunity, Prior Art & Patent Landscape, Synthesis & Key Insights, Patentability Assessment.\n\nCRITICAL: Output ONLY the complete revised report. No planning, no thinking, no word counts. Start with '# Executive Summary'.\n\nPeer review:\n" ++ string_truncate(to_string(peer_review), 1500) ++ "\n\nNovelty assessment:\n" ++ string_truncate(to_string(novelty), 1000) ++ "\n\nValidator:\n" ++ string_truncate(to_string(val_decision), 500) ++ "\n\nPrevious report:\n" ++ string_truncate(to_string(old_report), 3000)
            report = call_with_retry(prompt, 'orc', 1)
            print("[research_editor] Revision complete (" ++ to_string(string_length(report)) ++ " chars)")
            out_dir = "studio/output/patents/project_" ++ to_string(project_id)
            file_write(out_dir ++ "/research_report.md", report)
            ets_put(elem(tables, 2), {'report', project_id}, report)
            send(whereis("director"), {'research_report_done', project_id})
            research_editor(tables)
    }
}

# ══════════════════════════════════════════════════════════════════════════════
# STAGE 2: VALIDATION
# ══════════════════════════════════════════════════════════════════════════════

fun peer_reviewer(tables) {
    receive {
        {'review', project_id} ->
            print("[peer_reviewer] Reviewing research for project " ++ to_string(project_id))
            report = ets_get(elem(tables, 2), {'report', project_id})
            prompt = "You are a rigorous peer reviewer. Evaluate:\n1. Methodological soundness\n2. Logical gaps\n3. Unsupported claims\n4. Completeness\n5. Technical accuracy\n6. Document quality — if it contains planning notes, word counts, or draft artifacts, rate confidence LOW immediately.\n\nRate confidence: HIGH, MEDIUM, or LOW. 300-400 words.\n\nCRITICAL: Output ONLY the review.\n\nReport:\n" ++ string_truncate(to_string(report), 3000)
            review = call_with_retry(prompt, 'swarm', 1)
            print("[peer_reviewer] Review complete (" ++ to_string(string_length(review)) ++ " chars)")
            ets_put(elem(tables, 3), {'peer_review', project_id}, review)
            send(whereis("validation_collector"), {'validation_done', project_id, 'peer_review'})
            peer_reviewer(tables)
    }
}

fun novelty_assessor(tables) {
    receive {
        {'assess', project_id} ->
            print("[novelty_assessor] Assessing novelty for project " ++ to_string(project_id))
            report = ets_get(elem(tables, 2), {'report', project_id})
            prior_art = ets_get(elem(tables, 1), {'finding_prior_art', project_id})
            system = "You are a patent novelty examiner. Verify innovations against prior art by searching online. For each: state innovation, SEARCH/PATENTS for closest prior art, rate NOVEL/INCREMENTAL/KNOWN, cite evidence. Overall: HIGH/MEDIUM/LOW. 300-400 words.\n\nCRITICAL: Final output must be ONLY the assessment."
            task = "Report:\n" ++ string_truncate(to_string(report), 2000) ++ "\n\nPrior art:\n" ++ string_truncate(to_string(prior_art), 1500)
            assessment = tool_agent(system, task, 0, 2, 'swarm')
            print("[novelty_assessor] Assessment complete (" ++ to_string(string_length(assessment)) ++ " chars)")
            ets_put(elem(tables, 3), {'novelty', project_id}, assessment)
            send(whereis("validation_collector"), {'validation_done', project_id, 'novelty'})
            novelty_assessor(tables)
    }
}

fun validation_collector(tables) {
    receive {
        {'validation_done', project_id, val_type} ->
            print("[validation_collector] " ++ to_string(val_type) ++ " done for project " ++ to_string(project_id))
            counter_key = {'val_count', project_id}
            current = ets_get(elem(tables, 3), counter_key)
            count = 1
            if (current != nil) { count = elem(current, 0) + 1 }
            ets_put(elem(tables, 3), counter_key, {count})
            if (count >= 2) {
                print("[validation_collector] Both validations ready for project " ++ to_string(project_id))
                send(whereis("director"), {'validations_ready', project_id})
            }
            validation_collector(tables)
    }
}

fun validator(tables) {
    receive {
        {'validate', project_id} ->
            print("[validator] Making go/no-go decision for project " ++ to_string(project_id))
            peer_review = ets_get(elem(tables, 3), {'peer_review', project_id})
            novelty = ets_get(elem(tables, 3), {'novelty', project_id})
            report = ets_get(elem(tables, 2), {'report', project_id})
            prompt = "You are a STRICT patent pipeline gatekeeper.\n\nHARD RULES (automatic NO_GO if ANY true):\n- Peer review confidence is LOW\n- Report contains planning notes, word counts, or draft artifacts\n- ALL novelty elements rated KNOWN\n- Significant factual errors\n\nGO requires ALL:\n- Peer review MEDIUM or HIGH\n- At least one NOVEL element\n- No fatal flaws\n- Report is polished, finished\n\nOutput ONLY valid JSON, no fences:\n{\"decision\": \"go\" or \"no_go\", \"reasoning\": \"2-3 sentences\", \"key_concerns\": \"...\", \"strongest_claims\": \"...\"}\n\nPeer review:\n" ++ string_truncate(to_string(peer_review), 1500) ++ "\n\nNovelty:\n" ++ string_truncate(to_string(novelty), 1500) ++ "\n\nReport (first 500 chars):\n" ++ string_truncate(to_string(report), 500)
            raw = clean_json(call_with_retry(prompt, 'orc', 1))
            decision_str = json_get(raw, "decision")
            reasoning = json_get(raw, "reasoning")
            if (reasoning == nil) { reasoning = to_string(raw) }
            print("[validator] Decision: " ++ to_string(decision_str))
            print("[validator] Reasoning: " ++ string_truncate(to_string(reasoning), 120))
            ets_put(elem(tables, 3), {'validator_decision', project_id}, raw)
            val_report = "# Validation Report\n\n## Peer Review\n" ++ to_string(peer_review) ++ "\n\n## Novelty Assessment\n" ++ to_string(novelty) ++ "\n\n## Decision: " ++ to_string(decision_str) ++ "\n\n" ++ to_string(reasoning)
            out_dir = "studio/output/patents/project_" ++ to_string(project_id)
            file_mkdir(out_dir)
            file_write(out_dir ++ "/validation_report.md", val_report)
            decision_atom = 'no_go'
            if (string_contains(to_string(decision_str), "no") != 'true') { decision_atom = 'go' }
            send(whereis("director"), {'validation_complete', project_id, decision_atom})
            validator(tables)
    }
}

# ══════════════════════════════════════════════════════════════════════════════
# STAGE 3: PATENT — strategist → drafter (orc_long) → reviewer (revision loop)
# ══════════════════════════════════════════════════════════════════════════════

fun patent_strategist(tables) {
    receive {
        {'strategize', project_id} ->
            print("[patent_strategist] Developing strategy for project " ++ to_string(project_id))
            report = ets_get(elem(tables, 2), {'report', project_id})
            val_decision = ets_get(elem(tables, 3), {'validator_decision', project_id})
            strongest = json_get(clean_json(val_decision), "strongest_claims")
            concerns = json_get(clean_json(val_decision), "key_concerns")
            if (strongest == nil) { strongest = "See research report" }
            if (concerns == nil) { concerns = "None identified" }
            system = "You are a patent strategist. Search for comparable patents. Develop: TITLE, ABSTRACT (150 words), 2-3 INDEPENDENT CLAIMS, 4-6 DEPENDENT CLAIMS, CLAIM HIERARCHY, KEY DIFFERENTIATORS, PROSECUTION STRATEGY. 600-800 words.\n\nCRITICAL: Output ONLY the strategy. No planning."
            task = "Strongest: " ++ string_truncate(to_string(strongest), 400) ++ "\nConcerns: " ++ string_truncate(to_string(concerns), 300) ++ "\n\nReport:\n" ++ string_truncate(to_string(report), 2500)
            strategy = tool_agent(system, task, 0, 2, 'orc')
            print("[patent_strategist] Strategy complete (" ++ to_string(string_length(strategy)) ++ " chars)")
            ets_put(elem(tables, 4), {'strategy', project_id}, strategy)
            send(whereis("director"), {'strategy_done', project_id})
            patent_strategist(tables)
    }
}

fun patent_drafter(tables) {
    receive {
        {'draft', project_id} ->
            print("[patent_drafter] Drafting patent for project " ++ to_string(project_id))
            strategy = ets_get(elem(tables, 4), {'strategy', project_id})
            report = ets_get(elem(tables, 2), {'report', project_id})
            prompt = "You are a patent attorney drafting a USPTO-style patent application. Write a COMPLETE application:\n\n# [PATENT TITLE]\n## Field of the Invention\n## Background of the Invention\n(200-300 words)\n## Summary of the Invention\n(150-200 words)\n## Detailed Description of Preferred Embodiments\n(400-600 words, 3+ working examples)\n## Claims\n(2-3 independent + 4-6 dependent, each complete single sentence)\n## Abstract\n(150 words max)\n\nEvery claim must be COMPLETE. No truncation.\n\nCRITICAL: Output ONLY the patent. No planning, no thinking. Start with title.\n\nStrategy:\n" ++ string_truncate(to_string(strategy), 3000) ++ "\n\nReport:\n" ++ string_truncate(to_string(report), 2000)
            draft = call_with_retry(prompt, 'orc_long', 1)
            print("[patent_drafter] Draft complete (" ++ to_string(string_length(draft)) ++ " chars)")
            out_dir = "studio/output/patents/project_" ++ to_string(project_id)
            file_mkdir(out_dir)
            file_write(out_dir ++ "/patent_draft.md", draft)
            file_write(out_dir ++ "/patent_draft_v1.md", draft)
            print("[patent_drafter] Written (v1)")
            ets_put(elem(tables, 4), {'draft', project_id}, draft)
            send(whereis("director"), {'patent_drafted', project_id})
            patent_drafter(tables)

        {'revise_patent', project_id} ->
            print("[patent_drafter] Revising patent for project " ++ to_string(project_id))
            old_draft = ets_get(elem(tables, 4), {'draft', project_id})
            review_feedback = ets_get(elem(tables, 4), {'review', project_id})
            pat_rev_entry = ets_get(elem(tables, 0), {'pat_rev', project_id})
            version = 2
            if (pat_rev_entry != nil) { version = elem(pat_rev_entry, 0) + 1 }
            prompt = "You are a patent attorney revising based on reviewer feedback. Address EVERY concern. Strengthen weak claims. Ensure ALL claims complete.\n\nCRITICAL: Output ONLY the revised patent. No planning. Start with title.\n\nFeedback:\n" ++ string_truncate(to_string(review_feedback), 2000) ++ "\n\nCurrent draft:\n" ++ string_truncate(to_string(old_draft), 3500)
            draft = call_with_retry(prompt, 'orc_long', 1)
            print("[patent_drafter] Revision complete (" ++ to_string(string_length(draft)) ++ " chars)")
            out_dir = "studio/output/patents/project_" ++ to_string(project_id)
            file_write(out_dir ++ "/patent_draft.md", draft)
            file_write(out_dir ++ "/patent_draft_v" ++ to_string(version) ++ ".md", draft)
            print("[patent_drafter] Written (v" ++ to_string(version) ++ ")")
            ets_put(elem(tables, 4), {'draft', project_id}, draft)
            send(whereis("director"), {'patent_drafted', project_id})
            patent_drafter(tables)
    }
}

fun patent_reviewer(tables) {
    receive {
        {'review_patent', project_id} ->
            print("[patent_reviewer] Reviewing patent for project " ++ to_string(project_id))
            draft = ets_get(elem(tables, 4), {'draft', project_id})
            strategy = ets_get(elem(tables, 4), {'strategy', project_id})
            prompt = "Score this patent 1-10 each:\n1. NOVELTY\n2. BREADTH\n3. DEFENSIBILITY\n\nAlso check: claim clarity, specification support, completeness (any cut off?).\nPASSES if average >= 7.\n\nOutput ONLY valid JSON, no fences:\n{\"novelty\": N, \"breadth\": N, \"defensibility\": N, \"average\": N.N, \"passed\": true/false, \"strengths\": \"...\", \"weaknesses\": \"...\", \"feedback\": \"...\"}\n\nStrategy:\n" ++ string_truncate(to_string(strategy), 1500) ++ "\n\nDraft:\n" ++ string_truncate(to_string(draft), 3500)
            raw = clean_json(call_with_retry(prompt, 'orc', 1))
            avg = json_get(raw, "average")
            passed = json_get(raw, "passed")
            feedback = json_get(raw, "feedback")
            strengths = json_get(raw, "strengths")
            weaknesses = json_get(raw, "weaknesses")
            novelty_score = json_get(raw, "novelty")
            breadth_score = json_get(raw, "breadth")
            defensibility_score = json_get(raw, "defensibility")
            if (avg == nil) { avg = "N/A" }
            if (feedback == nil) { feedback = to_string(raw) }
            print("[patent_reviewer] Scores — novelty: " ++ to_string(novelty_score) ++ ", breadth: " ++ to_string(breadth_score) ++ ", defensibility: " ++ to_string(defensibility_score))
            print("[patent_reviewer] Average: " ++ to_string(avg) ++ " | Passed: " ++ to_string(passed))
            ets_put(elem(tables, 4), {'review', project_id}, raw)
            review_report = "# Patent Review\n\n## Scores\n- Novelty: " ++ to_string(novelty_score) ++ "/10\n- Breadth: " ++ to_string(breadth_score) ++ "/10\n- Defensibility: " ++ to_string(defensibility_score) ++ "/10\n- **Average: " ++ to_string(avg) ++ "/10**\n- **Passed: " ++ to_string(passed) ++ "**\n\n## Strengths\n" ++ to_string(strengths) ++ "\n\n## Weaknesses\n" ++ to_string(weaknesses) ++ "\n\n## Feedback\n" ++ to_string(feedback)
            out_dir = "studio/output/patents/project_" ++ to_string(project_id)
            file_mkdir(out_dir)
            file_write(out_dir ++ "/patent_review.md", review_report)
            passed_atom = 'false'
            if (string_contains(to_string(passed), "true") == 'true') { passed_atom = 'true' }
            send(whereis("director"), {'patent_reviewed', project_id, avg, passed_atom})
            patent_reviewer(tables)
    }
}

# ══════════════════════════════════════════════════════════════════════════════
# STAGE 4: COMMERCIALIZATION
# ══════════════════════════════════════════════════════════════════════════════

fun market_analyst(tables) {
    receive {
        {'analyze_market', project_id} ->
            print("[market_analyst] Analyzing market for project " ++ to_string(project_id))
            report = ets_get(elem(tables, 2), {'report', project_id})
            draft = ets_get(elem(tables, 4), {'draft', project_id})
            strategy = ets_get(elem(tables, 4), {'strategy', project_id})
            system = "You are a market analyst. Search for real market data. Produce markdown:\n## Target Buyers\n## Market Size (TAM/SAM/SOM)\n## Competitive Landscape\n## Licensing Strategy\n## Risk Factors\n## Revenue Projections (3-year)\n600-800 words.\n\nCRITICAL: Output ONLY the analysis. No planning."
            task = "Strategy:\n" ++ string_truncate(to_string(strategy), 1500) ++ "\n\nDraft:\n" ++ string_truncate(to_string(draft), 1200) ++ "\n\nReport:\n" ++ string_truncate(to_string(report), 1200)
            analysis = research_agent(system, task, 'swarm')
            print("[market_analyst] Analysis complete (" ++ to_string(string_length(analysis)) ++ " chars)")
            out_dir = "studio/output/patents/project_" ++ to_string(project_id)
            file_mkdir(out_dir)
            file_write(out_dir ++ "/market_analysis.md", analysis)
            ets_put(elem(tables, 5), project_id, analysis)
            send(whereis("director"), {'market_done', project_id})
            market_analyst(tables)
    }
}

fun pitch_writer(tables) {
    receive {
        {'write_pitch', project_id} ->
            print("[pitch_writer] Writing pitch for project " ++ to_string(project_id))
            market = ets_get(elem(tables, 5), project_id)
            draft = ets_get(elem(tables, 4), {'draft', project_id})
            review = ets_get(elem(tables, 4), {'review', project_id})
            strategy = ets_get(elem(tables, 4), {'strategy', project_id})
            prompt = "Write a compelling 1-page pitch (400-500 words) in markdown for a VP of R&D:\n\n# [Compelling headline]\n**The Opportunity** (2-3 sentences)\n**The Innovation** (3-4 sentences)\n**Market Impact** (key numbers)\n**Patent Strength** (scores, claims)\n**The Ask** (terms, next steps)\n---\n*Generated by Patent Research Lab — SwarmRT*\n\nCRITICAL: Output ONLY the pitch. No planning.\n\nMarket:\n" ++ string_truncate(to_string(market), 1500) ++ "\n\nStrategy:\n" ++ string_truncate(to_string(strategy), 1000) ++ "\n\nReview:\n" ++ string_truncate(to_string(review), 800) ++ "\n\nDraft:\n" ++ string_truncate(to_string(draft), 800)
            pitch = call_with_retry(prompt, 'swarm', 1)
            print("[pitch_writer] Pitch complete (" ++ to_string(string_length(pitch)) ++ " chars)")
            out_dir = "studio/output/patents/project_" ++ to_string(project_id)
            file_mkdir(out_dir)
            file_write(out_dir ++ "/pitch.md", pitch)
            ets_put(elem(tables, 6), project_id, pitch)
            send(whereis("director"), {'pitch_done', project_id})
            pitch_writer(tables)
    }
}

# ══════════════════════════════════════════════════════════════════════════════
# KEEPER + MAIN
# ══════════════════════════════════════════════════════════════════════════════

fun keeper() {
    sleep(60000)
    keeper()
}

fun main() {
    print("")
    print("  ╔═══════════════════════════════════════════════════════╗")
    print("  ║   PATENT RESEARCH LAB  v0.3                           ║")
    print("  ║   16 agents — 4 stages — tool-augmented pipeline      ║")
    print("  ║   Research → Validate → Patent → Commercialize        ║")
    print("  ║   SwarmRT / otonomy.ai                                ║")
    print("  ╚═══════════════════════════════════════════════════════╝")
    print("")

    t_status     = ets_new()
    t_findings   = ets_new()
    t_analysis   = ets_new()
    t_validation = ets_new()
    t_patent     = ets_new()
    t_market     = ets_new()
    t_pitch      = ets_new()
    tables = {t_status, t_findings, t_analysis, t_validation, t_patent, t_market, t_pitch}
    print("[main] 7 ETS tables created")

    file_mkdir("studio/output")
    file_mkdir("studio/output/patents")

    pid_dir  = spawn(director(tables))
    pid_rt   = spawn(researcher_tech(tables))
    pid_rm   = spawn(researcher_market(tables))
    pid_rpa  = spawn(researcher_prior_art(tables))
    pid_fc   = spawn(findings_collector(tables))
    pid_an   = spawn(analyst(tables))
    pid_re   = spawn(research_editor(tables))
    pid_pr   = spawn(peer_reviewer(tables))
    pid_na   = spawn(novelty_assessor(tables))
    pid_vc   = spawn(validation_collector(tables))
    pid_val  = spawn(validator(tables))
    pid_ps   = spawn(patent_strategist(tables))
    pid_pd   = spawn(patent_drafter(tables))
    pid_prv  = spawn(patent_reviewer(tables))
    pid_ma   = spawn(market_analyst(tables))
    pid_pw   = spawn(pitch_writer(tables))

    register("director", pid_dir)
    register("researcher_tech", pid_rt)
    register("researcher_market", pid_rm)
    register("researcher_prior_art", pid_rpa)
    register("findings_collector", pid_fc)
    register("analyst", pid_an)
    register("research_editor", pid_re)
    register("peer_reviewer", pid_pr)
    register("novelty_assessor", pid_na)
    register("validation_collector", pid_vc)
    register("validator", pid_val)
    register("patent_strategist", pid_ps)
    register("patent_drafter", pid_pd)
    register("patent_reviewer", pid_prv)
    register("market_analyst", pid_ma)
    register("pitch_writer", pid_pw)

    print("[main] 16 agents spawned:")
    print("  Stage 1: Director(orc), 3 Researchers(swarm+tools), Analyst(orc+retry), Editor(swarm+retry)")
    print("  Stage 2: Peer Reviewer(swarm+retry), Novelty Assessor(swarm+tools), Validator(orc strict)")
    print("  Stage 3: Strategist(orc+tools), Drafter(orc_long 16k), Reviewer(orc+retry)")
    print("  Stage 4: Market Analyst(swarm+tools), Pitch Writer(swarm+retry)")
    print("")
    print("  v0.3: anti-CoT, retry, clean_json, orc_long drafter, versioned drafts,")
    print("        targeted revision, strict validator, final dashboard")
    print("")

    question = "Biodegradable microelectronics for precision agriculture — sensor networks that dissolve into soil nutrients after their monitoring lifecycle ends. Consider novel materials (silk fibroin substrates, zinc oxide circuits, cellulose encapsulation), power harvesting from soil microbiomes, mesh networking protocols for underground communication, and integration with drone-based deployment systems."

    print("[main] Q: " ++ question)
    print("")
    print("[main] Dispatching to director...")
    print("")
    send(pid_dir, {'question', 1, question})

    spawn(keeper())
    keeper()
}
