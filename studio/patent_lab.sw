# Patent Research Lab — SwarmRT Multi-Agent Pipeline
# 16 agents, 4 stages + real-time web tools
# Agents can SEARCH the web, FETCH pages, and search PATENTS autonomously.
# Research → Validate → Patent → Commercialize
# otonomy.ai — 2026

module PatentLab

export [main, director, tool_agent,
        researcher_tech, researcher_market, researcher_prior_art,
        findings_collector, analyst, research_editor,
        peer_reviewer, novelty_assessor, validation_collector, validator,
        patent_strategist, patent_drafter, patent_reviewer,
        market_analyst, pitch_writer, keeper]

# ── LLM Helpers ──

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
    result
}

fun trunc(s, max_len) {
    result = s
    if (string_length(s) > max_len) {
        result = string_sub(s, 0, max_len)
    }
    result
}

# ══════════════════════════════════════════════════════════════════════════════
# WEB TOOL HELPERS — real-time search, fetch, patent lookup
# Uses http_get + shell for HTML stripping. No shell injection — URLs go
# through http_get, only controlled temp filenames hit shell.
# ══════════════════════════════════════════════════════════════════════════════

fun strip_html(html) {
    result = ""
    if (html != nil) {
        text = to_string(html)
        if (string_length(text) > 10) {
            tmp = "/tmp/sw_" ++ to_string(random_int(100000, 999999))
            file_write(tmp, text)
            stripped = shell("sed 's/<[^>]*>//g' " ++ tmp ++ " | tr -s '[:space:]' ' ' | head -c 6000; rm -f " ++ tmp)
            result = elem(stripped, 1)
        }
    }
    result
}

fun web_search(query) {
    print("[tool] SEARCH: " ++ trunc(to_string(query), 60))
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
    print("[tool] FETCH: " ++ trunc(to_string(url), 80))
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
    print("[tool] PATENTS: " ++ trunc(to_string(query), 60))
    web_search(to_string(query) ++ " patent site:patents.google.com OR site:uspto.gov")
}

fun search_scholar(query) {
    print("[tool] SCHOLAR: " ++ trunc(to_string(query), 60))
    web_search(to_string(query) ++ " research paper site:arxiv.org OR site:scholar.google.com")
}

# ══════════════════════════════════════════════════════════════════════════════
# AGENTIC TOOL LOOP — ReAct-style recursive tool use
# LLM decides which tool to call. Supports SEARCH, FETCH, PATENTS, SCHOLAR.
# Max depth prevents runaway recursion. One tool call per iteration.
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
    tools_prompt = "\n\nYou have real-time web research tools. To use one, write the command on its own line:\n  SEARCH: your search query\n  FETCH: https://full-url-to-read\n  PATENTS: patent search terms\n  SCHOLAR: academic paper search terms\n\nUse ONE tool per response. After seeing results, you'll get another turn.\nWhen you have enough data, just write your final analysis (no tool command).\nAlways use at least one tool before giving your final answer."

    prompt = system ++ tools_prompt ++ "\n\nTask:\n" ++ task
    response = call_llm(prompt, model)

    result = response
    has_search = string_contains(response, "SEARCH: ")
    has_fetch = string_contains(response, "FETCH: ")
    has_patents = string_contains(response, "PATENTS: ")
    has_scholar = string_contains(response, "SCHOLAR: ")
    used_tool = 'false'

    # Hoist variables — SwarmRT scopes vars inside if blocks
    query = ""
    tool_result = ""
    new_task = ""
    url = ""

    # Priority: SEARCH > FETCH > PATENTS > SCHOLAR (only one per turn)

    if (has_search == 'true') {
        if (depth < max_depth) {
            query = extract_tool_arg(response, "SEARCH: ")
            tool_result = web_search(query)
            new_task = task ++ "\n\n--- Your previous response ---\n" ++ trunc(response, 500) ++ "\n\n--- Search results for '" ++ trunc(query, 40) ++ "' ---\n" ++ trunc(tool_result, 2500)
            result = tool_agent(system, new_task, depth + 1, max_depth, model)
            used_tool = 'true'
        }
    }

    if (has_fetch == 'true') {
        if (used_tool != 'true') {
            if (depth < max_depth) {
                url = extract_tool_arg(response, "FETCH: ")
                tool_result = fetch_url(url)
                new_task = task ++ "\n\n--- Your previous response ---\n" ++ trunc(response, 500) ++ "\n\n--- Content from " ++ trunc(url, 60) ++ " ---\n" ++ trunc(tool_result, 2500)
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
                new_task = task ++ "\n\n--- Your previous response ---\n" ++ trunc(response, 500) ++ "\n\n--- Patent results for '" ++ trunc(query, 40) ++ "' ---\n" ++ trunc(tool_result, 2500)
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
                new_task = task ++ "\n\n--- Your previous response ---\n" ++ trunc(response, 500) ++ "\n\n--- Academic results for '" ++ trunc(query, 40) ++ "' ---\n" ++ trunc(tool_result, 2500)
                result = tool_agent(system, new_task, depth + 1, max_depth, model)
            }
        }
    }

    result
}

# ══════════════════════════════════════════════════════════════════════════════
# THE DIRECTOR — Pure state machine
# Receives completion messages from each stage, dispatches the next.
# Tracks validation revisions (max 2) and patent revisions (max 2).
# ══════════════════════════════════════════════════════════════════════════════

fun director(tables) {
    receive {
        # ── Stage 1 entry: decompose question into 3 sub-questions ──
        {'question', project_id, question} ->
            print("[director] Research question received for project " ++ to_string(project_id))
            print("[director] Q: " ++ trunc(question, 120))

            # Store original question + init revision counters
            ets_put(elem(tables, 0), {'question', project_id}, question)
            ets_put(elem(tables, 0), {'val_rev', project_id}, {0})
            ets_put(elem(tables, 0), {'pat_rev', project_id}, {0})

            decompose_prompt = "You are a research director preparing a patent investigation. Given this research question, decompose it into exactly 3 focused sub-questions for parallel investigation:\n1. Technical/engineering angle — mechanisms, materials, implementation\n2. Commercial/market angle — market size, buyers, competitive landscape\n3. Prior art angle — existing patents, academic literature, known solutions\n\nOutput ONLY valid JSON with no markdown fences:\n{\"tech_q\": \"...\", \"market_q\": \"...\", \"prior_art_q\": \"...\"}\n\nQuestion: " ++ question
            raw = call_orc(decompose_prompt)
            print("[director] Decomposition complete")

            tech_q = json_get(raw, "tech_q")
            market_q = json_get(raw, "market_q")
            prior_art_q = json_get(raw, "prior_art_q")

            if (tech_q == nil) {
                tech_q = "Analyze the key technical mechanisms and engineering challenges related to: " ++ question
            }
            if (market_q == nil) {
                market_q = "Analyze the commercial opportunity and market landscape for: " ++ question
            }
            if (prior_art_q == nil) {
                prior_art_q = "Survey existing patents, academic literature, and prior art related to: " ++ question
            }

            print("[director] Tech Q: " ++ trunc(to_string(tech_q), 80))
            print("[director] Market Q: " ++ trunc(to_string(market_q), 80))
            print("[director] Prior Art Q: " ++ trunc(to_string(prior_art_q), 80))

            ets_put(elem(tables, 0), project_id, {'researching'})
            # Reset findings barrier to 0
            ets_put(elem(tables, 1), {'finding_count', project_id}, {0})

            # Dispatch 3 researchers in parallel
            send(whereis("researcher_tech"), {'research', project_id, tech_q, question})
            send(whereis("researcher_market"), {'research', project_id, market_q, question})
            send(whereis("researcher_prior_art"), {'research', project_id, prior_art_q, question})
            director(tables)

        # ── Stage 1: all 3 findings collected → analyst ──
        {'findings_ready', project_id} ->
            print("[director] All 3 findings received. Sending to analyst.")
            ets_put(elem(tables, 0), project_id, {'analyzing'})
            send(whereis("analyst"), {'analyze', project_id})
            director(tables)

        # ── Stage 1: analysis done → research editor ──
        {'analysis_done', project_id} ->
            print("[director] Analysis complete. Sending to research editor.")
            ets_put(elem(tables, 0), project_id, {'editing_research'})
            send(whereis("research_editor"), {'edit', project_id})
            director(tables)

        # ── Stage 1 complete → Stage 2: parallel validation ──
        {'research_report_done', project_id} ->
            print("[director] Research report written. Starting validation (Stage 2).")
            ets_put(elem(tables, 0), project_id, {'validating'})
            # Reset validation barrier to 0
            ets_put(elem(tables, 3), {'val_count', project_id}, {0})
            send(whereis("peer_reviewer"), {'review', project_id})
            send(whereis("novelty_assessor"), {'assess', project_id})
            director(tables)

        # ── Stage 2: both validations collected → validator ──
        {'validations_ready', project_id} ->
            print("[director] Both validations received. Sending to validator.")
            ets_put(elem(tables, 0), project_id, {'checking_validation'})
            send(whereis("validator"), {'validate', project_id})
            director(tables)

        # ── Stage 2 complete: go/no-go decision ──
        {'validation_complete', project_id, decision} ->
            if (decision == 'go') {
                print("[director] Validation PASSED. Starting patent strategy (Stage 3).")
                ets_put(elem(tables, 0), project_id, {'strategizing'})
                send(whereis("patent_strategist"), {'strategize', project_id})
            } else {
                current = ets_get(elem(tables, 0), {'val_rev', project_id})
                val_rev = 0
                if (current != nil) {
                    val_rev = elem(current, 0)
                }
                if (val_rev < 2) {
                    print("[director] Validation REJECTED. Revision " ++ to_string(val_rev + 1) ++ " — looping back to research.")
                    ets_put(elem(tables, 0), {'val_rev', project_id}, {val_rev + 1})

                    question = ets_get(elem(tables, 0), {'question', project_id})
                    val_feedback = ets_get(elem(tables, 3), {'validator_decision', project_id})
                    revised_q = to_string(question) ++ "\n\n[REVISION NOTE — previous attempt rejected. Feedback: " ++ trunc(to_string(val_feedback), 300) ++ "]"

                    decompose_prompt = "You are a research director. A previous research attempt was rejected during peer validation. Decompose this revised question into 3 focused sub-questions (technical, market, prior art) that address the rejection feedback. Output ONLY valid JSON: {\"tech_q\": \"...\", \"market_q\": \"...\", \"prior_art_q\": \"...\"}\n\nQuestion with feedback:\n" ++ trunc(revised_q, 1500)
                    raw = call_orc(decompose_prompt)

                    tech_q = json_get(raw, "tech_q")
                    market_q = json_get(raw, "market_q")
                    prior_art_q = json_get(raw, "prior_art_q")
                    if (tech_q == nil) { tech_q = "Re-analyze technical aspects of: " ++ to_string(question) }
                    if (market_q == nil) { market_q = "Re-analyze market opportunity for: " ++ to_string(question) }
                    if (prior_art_q == nil) { prior_art_q = "Deeper prior art survey for: " ++ to_string(question) }

                    ets_put(elem(tables, 0), project_id, {'researching'})
                    # Reset findings barrier to 0
                    ets_put(elem(tables, 1), {'finding_count', project_id}, {0})
                    send(whereis("researcher_tech"), {'research', project_id, tech_q, to_string(question)})
                    send(whereis("researcher_market"), {'research', project_id, market_q, to_string(question)})
                    send(whereis("researcher_prior_art"), {'research', project_id, prior_art_q, to_string(question)})
                } else {
                    print("[director] Validation REJECTED — max revisions reached. Proceeding to patent anyway.")
                    ets_put(elem(tables, 0), project_id, {'strategizing'})
                    send(whereis("patent_strategist"), {'strategize', project_id})
                }
            }
            director(tables)

        # ── Stage 3: strategy done → patent drafter ──
        {'strategy_done', project_id} ->
            print("[director] Patent strategy complete. Sending to drafter.")
            ets_put(elem(tables, 0), project_id, {'drafting_patent'})
            send(whereis("patent_drafter"), {'draft', project_id})
            director(tables)

        # ── Stage 3: draft done → patent reviewer ──
        {'patent_drafted', project_id} ->
            print("[director] Patent draft complete. Sending to reviewer.")
            ets_put(elem(tables, 0), project_id, {'reviewing_patent'})
            send(whereis("patent_reviewer"), {'review_patent', project_id})
            director(tables)

        # ── Stage 3 complete: quality gate ──
        {'patent_reviewed', project_id, avg_score, passed} ->
            if (passed == 'true') {
                print("[director] Patent APPROVED (score: " ++ to_string(avg_score) ++ "). Starting commercialization (Stage 4).")
                ets_put(elem(tables, 0), project_id, {'analyzing_market'})
                send(whereis("market_analyst"), {'analyze_market', project_id})
            } else {
                current = ets_get(elem(tables, 0), {'pat_rev', project_id})
                pat_rev = 0
                if (current != nil) {
                    pat_rev = elem(current, 0)
                }
                if (pat_rev < 2) {
                    print("[director] Patent score " ++ to_string(avg_score) ++ "/10. Revision " ++ to_string(pat_rev + 1) ++ " — sending back to drafter.")
                    ets_put(elem(tables, 0), {'pat_rev', project_id}, {pat_rev + 1})
                    ets_put(elem(tables, 0), project_id, {'drafting_patent'})
                    send(whereis("patent_drafter"), {'revise_patent', project_id})
                } else {
                    print("[director] Patent score " ++ to_string(avg_score) ++ "/10. Max revisions reached. Proceeding to commercialization.")
                    ets_put(elem(tables, 0), project_id, {'analyzing_market'})
                    send(whereis("market_analyst"), {'analyze_market', project_id})
                }
            }
            director(tables)

        # ── Stage 4: market analysis done → pitch writer ──
        {'market_done', project_id} ->
            print("[director] Market analysis complete. Sending to pitch writer.")
            ets_put(elem(tables, 0), project_id, {'writing_pitch'})
            send(whereis("pitch_writer"), {'write_pitch', project_id})
            director(tables)

        # ── Stage 4 complete: SHIPPED ──
        {'pitch_done', project_id} ->
            print("")
            print("[director] ════════════════════════════════════════════════════")
            print("[director]  SHIPPED — Patent Pipeline Complete")
            print("[director]  Project " ++ to_string(project_id))
            print("[director]  Output: studio/output/patents/project_" ++ to_string(project_id) ++ "/")
            print("[director]  Files: research_report.md, validation_report.md,")
            print("[director]         patent_draft.md, patent_review.md,")
            print("[director]         market_analysis.md, pitch.md")
            print("[director] ════════════════════════════════════════════════════")
            print("")
            ets_put(elem(tables, 0), project_id, {'shipped'})
            director(tables)
    }
}

# ══════════════════════════════════════════════════════════════════════════════
# STAGE 1: RESEARCH — 3 tool-augmented researchers + analyst + editor
# Each researcher runs the agentic tool loop (max 3 web searches each).
# ══════════════════════════════════════════════════════════════════════════════

# ── Researcher: Technical/Engineering ──

fun researcher_tech(tables) {
    receive {
        {'research', project_id, sub_question, original_q} ->
            print("[researcher_tech] Investigating: " ++ trunc(to_string(sub_question), 80))
            print("[researcher_tech] Starting tool-augmented research (max 3 web lookups)...")

            system = "You are a deep technical research analyst specializing in engineering and applied science. You have access to real-time web search. Use tools to find specific technical details, recent papers, material properties, and engineering data. Provide a thorough 400-600 word analysis with citations from your web research. Include specific technical details, mechanisms, materials, and feasibility analysis."
            task = "Original research question: " ++ to_string(original_q) ++ "\n\nYour assigned sub-question: " ++ to_string(sub_question)
            findings = tool_agent(system, task, 0, 3, 'swarm')

            print("[researcher_tech] Findings ready (" ++ to_string(string_length(findings)) ++ " chars)")
            ets_put(elem(tables, 1), {'finding_tech', project_id}, findings)
            send(whereis("findings_collector"), {'finding_done', project_id, 'tech'})
            researcher_tech(tables)
    }
}

# ── Researcher: Commercial/Market ──

fun researcher_market(tables) {
    receive {
        {'research', project_id, sub_question, original_q} ->
            print("[researcher_market] Investigating: " ++ trunc(to_string(sub_question), 80))
            print("[researcher_market] Starting tool-augmented research (max 3 web lookups)...")

            system = "You are a market research analyst specializing in technology commercialization. You have access to real-time web search. Use tools to find current market data, company valuations, industry reports, and competitive intelligence. Provide a thorough 400-600 word analysis with real market data. Include market size estimates, key players, growth trends, and competitive dynamics."
            task = "Original research question: " ++ to_string(original_q) ++ "\n\nYour assigned sub-question: " ++ to_string(sub_question)
            findings = tool_agent(system, task, 0, 3, 'swarm')

            print("[researcher_market] Findings ready (" ++ to_string(string_length(findings)) ++ " chars)")
            ets_put(elem(tables, 1), {'finding_market', project_id}, findings)
            send(whereis("findings_collector"), {'finding_done', project_id, 'market'})
            researcher_market(tables)
    }
}

# ── Researcher: Prior Art/Patents ──

fun researcher_prior_art(tables) {
    receive {
        {'research', project_id, sub_question, original_q} ->
            print("[researcher_prior_art] Investigating: " ++ trunc(to_string(sub_question), 80))
            print("[researcher_prior_art] Starting tool-augmented research (max 4 lookups — patents + scholar)...")

            system = "You are a patent research specialist with access to real-time web search, patent databases, and academic paper search. Use PATENTS: to search patent databases, SCHOLAR: to search academic papers, and SEARCH: for general web search. Find real patent numbers, assignees, filing dates, and academic citations. Identify gaps in existing IP landscape. Provide a thorough 400-600 word analysis citing real patents and papers."
            task = "Original research question: " ++ to_string(original_q) ++ "\n\nYour assigned sub-question: " ++ to_string(sub_question)
            findings = tool_agent(system, task, 0, 4, 'swarm')

            print("[researcher_prior_art] Findings ready (" ++ to_string(string_length(findings)) ++ " chars)")
            ets_put(elem(tables, 1), {'finding_prior_art', project_id}, findings)
            send(whereis("findings_collector"), {'finding_done', project_id, 'prior_art'})
            researcher_prior_art(tables)
    }
}

# ── Findings Collector (barrier: 3 researchers) ──

fun findings_collector(tables) {
    receive {
        {'finding_done', project_id, researcher} ->
            print("[findings_collector] Finding from " ++ to_string(researcher) ++ " for project " ++ to_string(project_id))
            counter_key = {'finding_count', project_id}
            current = ets_get(elem(tables, 1), counter_key)
            count = 1
            if (current != nil) {
                count = elem(current, 0) + 1
            }
            ets_put(elem(tables, 1), counter_key, {count})
            if (count >= 3) {
                print("[findings_collector] All 3 findings ready for project " ++ to_string(project_id))
                send(whereis("director"), {'findings_ready', project_id})
            }
            findings_collector(tables)
    }
}

# ── The Analyst ──
# Synthesizes findings from all 3 researchers into a unified analysis.

fun analyst(tables) {
    receive {
        {'analyze', project_id} ->
            print("[analyst] Synthesizing findings for project " ++ to_string(project_id))

            finding_tech = ets_get(elem(tables, 1), {'finding_tech', project_id})
            finding_market = ets_get(elem(tables, 1), {'finding_market', project_id})
            finding_prior_art = ets_get(elem(tables, 1), {'finding_prior_art', project_id})

            prompt = "You are a senior research analyst. Synthesize these three independent research findings into a unified analysis of 600-800 words. These findings include real web research data and citations — preserve specific numbers, patent references, and data points. Identify patterns across technical, market, and prior art dimensions. Highlight where the technical innovation meets market demand and where gaps in prior art create patenting opportunities.\n\nTechnical findings:\n" ++ trunc(to_string(finding_tech), 1200) ++ "\n\nMarket findings:\n" ++ trunc(to_string(finding_market), 1200) ++ "\n\nPrior art findings:\n" ++ trunc(to_string(finding_prior_art), 1200)
            analysis = call_orc(prompt)

            print("[analyst] Synthesis complete (" ++ to_string(string_length(analysis)) ++ " chars)")
            ets_put(elem(tables, 2), project_id, analysis)
            send(whereis("director"), {'analysis_done', project_id})
            analyst(tables)
    }
}

# ── Research Editor ──
# Polishes synthesis + raw findings into a final research report markdown.

fun research_editor(tables) {
    receive {
        {'edit', project_id} ->
            print("[research_editor] Polishing research report for project " ++ to_string(project_id))

            finding_tech = ets_get(elem(tables, 1), {'finding_tech', project_id})
            finding_market = ets_get(elem(tables, 1), {'finding_market', project_id})
            finding_prior_art = ets_get(elem(tables, 1), {'finding_prior_art', project_id})
            analysis = ets_get(elem(tables, 2), project_id)

            prompt = "You are a professional research report editor. Using the synthesis and raw findings below (which include real web research data), produce a polished research report in markdown. Preserve all specific citations, patent numbers, market figures, and data points.\n\n# Executive Summary\n(3-4 concise sentences)\n\n# Technical Landscape\n(Key technical findings, 200-300 words)\n\n# Market Opportunity\n(Key market findings with real data, 200-300 words)\n\n# Prior Art & Patent Landscape\n(Existing patents cited, identified gaps, 200-300 words)\n\n# Synthesis & Key Insights\n(Cross-cutting analysis, 300-400 words)\n\n# Patentability Assessment\n(Initial assessment of what could be patented, 100-200 words)\n\n---\n\nSynthesis:\n" ++ trunc(to_string(analysis), 1500) ++ "\n\nTechnical findings:\n" ++ trunc(to_string(finding_tech), 600) ++ "\n\nMarket findings:\n" ++ trunc(to_string(finding_market), 600) ++ "\n\nPrior art findings:\n" ++ trunc(to_string(finding_prior_art), 600)
            report = call_swarm(prompt)

            print("[research_editor] Report polished (" ++ to_string(string_length(report)) ++ " chars)")

            out_dir = "studio/output/patents/project_" ++ to_string(project_id)
            file_mkdir(out_dir)
            file_write(out_dir ++ "/research_report.md", report)
            print("[research_editor] Written to " ++ out_dir ++ "/research_report.md")

            ets_put(elem(tables, 2), {'report', project_id}, report)
            send(whereis("director"), {'research_report_done', project_id})
            research_editor(tables)
    }
}

# ══════════════════════════════════════════════════════════════════════════════
# STAGE 2: VALIDATION — parallel peer review + novelty check → go/no-go
# ══════════════════════════════════════════════════════════════════════════════

# ── Peer Reviewer ──
# Checks methodology, logical gaps, unsupported claims.

fun peer_reviewer(tables) {
    receive {
        {'review', project_id} ->
            print("[peer_reviewer] Reviewing research for project " ++ to_string(project_id))

            report = ets_get(elem(tables, 2), {'report', project_id})

            prompt = "You are a rigorous peer reviewer for a patent research report. Evaluate the following research report for:\n1. Methodological soundness — are conclusions supported by evidence?\n2. Logical gaps — any leaps in reasoning or missing analysis?\n3. Unsupported claims — assertions without backing?\n4. Completeness — any obvious areas not covered?\n5. Technical accuracy — any factual errors?\n\nProvide a structured review (300-400 words) with specific critiques and an overall assessment. Rate confidence in the research: HIGH, MEDIUM, or LOW.\n\nResearch report:\n" ++ trunc(to_string(report), 2500)
            review = call_swarm(prompt)

            print("[peer_reviewer] Review complete (" ++ to_string(string_length(review)) ++ " chars)")
            ets_put(elem(tables, 3), {'peer_review', project_id}, review)
            send(whereis("validation_collector"), {'validation_done', project_id, 'peer_review'})
            peer_reviewer(tables)
    }
}

# ── Novelty Assessor ──
# Evaluates what's genuinely new vs. already known. Uses tools to verify claims.

fun novelty_assessor(tables) {
    receive {
        {'assess', project_id} ->
            print("[novelty_assessor] Assessing novelty for project " ++ to_string(project_id))
            print("[novelty_assessor] Using web tools to verify novelty claims...")

            report = ets_get(elem(tables, 2), {'report', project_id})
            prior_art = ets_get(elem(tables, 1), {'finding_prior_art', project_id})

            system = "You are a patent novelty examiner with access to real-time web search and patent databases. Verify claimed innovations against actual prior art by searching online. For each potential innovation:\n1. State the claimed innovation\n2. SEARCH or PATENTS to find closest prior art\n3. Rate novelty: NOVEL, INCREMENTAL, or KNOWN\n4. Cite specific evidence\n\nOverall novelty score: HIGH, MEDIUM, or LOW. 300-400 words."
            task = "Research report:\n" ++ trunc(to_string(report), 1800) ++ "\n\nPrior art findings:\n" ++ trunc(to_string(prior_art), 1200)
            assessment = tool_agent(system, task, 0, 2, 'swarm')

            print("[novelty_assessor] Assessment complete (" ++ to_string(string_length(assessment)) ++ " chars)")
            ets_put(elem(tables, 3), {'novelty', project_id}, assessment)
            send(whereis("validation_collector"), {'validation_done', project_id, 'novelty'})
            novelty_assessor(tables)
    }
}

# ── Validation Collector (barrier: 2) ──

fun validation_collector(tables) {
    receive {
        {'validation_done', project_id, val_type} ->
            print("[validation_collector] " ++ to_string(val_type) ++ " done for project " ++ to_string(project_id))
            counter_key = {'val_count', project_id}
            current = ets_get(elem(tables, 3), counter_key)
            count = 1
            if (current != nil) {
                count = elem(current, 0) + 1
            }
            ets_put(elem(tables, 3), counter_key, {count})
            if (count >= 2) {
                print("[validation_collector] Both validations ready for project " ++ to_string(project_id))
                send(whereis("director"), {'validations_ready', project_id})
            }
            validation_collector(tables)
    }
}

# ── Validator ──
# Makes go/no-go decision based on peer review + novelty assessment.

fun validator(tables) {
    receive {
        {'validate', project_id} ->
            print("[validator] Making go/no-go decision for project " ++ to_string(project_id))

            peer_review = ets_get(elem(tables, 3), {'peer_review', project_id})
            novelty = ets_get(elem(tables, 3), {'novelty', project_id})
            report = ets_get(elem(tables, 2), {'report', project_id})

            prompt = "You are a patent pipeline gatekeeper. Based on the peer review and novelty assessment, decide whether this research should proceed to patent drafting.\n\nCriteria for GO:\n- Peer review confidence is MEDIUM or HIGH\n- At least one NOVEL element identified\n- No fatal methodological flaws\n\nCriteria for NO-GO:\n- LOW peer review confidence\n- All elements rated KNOWN or INCREMENTAL\n- Significant logical or factual errors\n\nOutput ONLY valid JSON with no markdown fences:\n{\"decision\": \"go\" or \"no_go\", \"reasoning\": \"2-3 sentences\", \"key_concerns\": \"any issues to address in patent\", \"strongest_claims\": \"most patentable elements\"}\n\nPeer review:\n" ++ trunc(to_string(peer_review), 1200) ++ "\n\nNovelty assessment:\n" ++ trunc(to_string(novelty), 1200) ++ "\n\nResearch summary:\n" ++ trunc(to_string(report), 800)
            raw = call_swarm(prompt)

            decision_str = json_get(raw, "decision")
            reasoning = json_get(raw, "reasoning")
            if (reasoning == nil) { reasoning = to_string(raw) }

            print("[validator] Decision: " ++ to_string(decision_str))
            print("[validator] Reasoning: " ++ trunc(to_string(reasoning), 120))

            # Store full decision for revision feedback
            ets_put(elem(tables, 3), {'validator_decision', project_id}, raw)

            # Build validation report
            val_report = "# Validation Report\n\n## Peer Review\n" ++ to_string(peer_review) ++ "\n\n## Novelty Assessment\n" ++ to_string(novelty) ++ "\n\n## Decision: " ++ to_string(decision_str) ++ "\n\n" ++ to_string(reasoning)
            out_dir = "studio/output/patents/project_" ++ to_string(project_id)
            file_mkdir(out_dir)
            file_write(out_dir ++ "/validation_report.md", val_report)
            print("[validator] Written to " ++ out_dir ++ "/validation_report.md")

            decision_atom = 'no_go'
            if (string_contains(to_string(decision_str), "no") != 'true') {
                decision_atom = 'go'
            }
            send(whereis("director"), {'validation_complete', project_id, decision_atom})
            validator(tables)
    }
}

# ══════════════════════════════════════════════════════════════════════════════
# STAGE 3: PATENT DRAFTING — strategist → drafter → reviewer (revision loop)
# ══════════════════════════════════════════════════════════════════════════════

# ── Patent Strategist ──
# Identifies patentable claims, builds claim hierarchy. Uses tools for real examples.

fun patent_strategist(tables) {
    receive {
        {'strategize', project_id} ->
            print("[patent_strategist] Developing patent strategy for project " ++ to_string(project_id))
            print("[patent_strategist] Searching for comparable patents...")

            report = ets_get(elem(tables, 2), {'report', project_id})
            val_decision = ets_get(elem(tables, 3), {'validator_decision', project_id})

            strongest = json_get(val_decision, "strongest_claims")
            concerns = json_get(val_decision, "key_concerns")
            if (strongest == nil) { strongest = "See research report" }
            if (concerns == nil) { concerns = "None identified" }

            system = "You are a patent strategist with access to real-time patent and web search. Search for comparable patents to inform your claim strategy. Develop a filing strategy including:\n1. TITLE — concise patent title\n2. ABSTRACT — 150-word patent abstract\n3. INDEPENDENT CLAIMS (2-3) — broad claims defining the core invention\n4. DEPENDENT CLAIMS (4-6) — narrower claims adding specificity\n5. CLAIM HIERARCHY — which dependent claims hang off which independent claims\n6. KEY DIFFERENTIATORS — what distinguishes this from prior art you found\n7. PROSECUTION STRATEGY — anticipate examiner objections\n\n600-800 words total."
            task = "Strongest patentable elements: " ++ trunc(to_string(strongest), 400) ++ "\nKey concerns: " ++ trunc(to_string(concerns), 300) ++ "\n\nResearch report:\n" ++ trunc(to_string(report), 2000)
            strategy = tool_agent(system, task, 0, 2, 'orc')

            print("[patent_strategist] Strategy complete (" ++ to_string(string_length(strategy)) ++ " chars)")
            ets_put(elem(tables, 4), {'strategy', project_id}, strategy)
            send(whereis("director"), {'strategy_done', project_id})
            patent_strategist(tables)
    }
}

# ── Patent Drafter ──
# Writes full USPTO-style patent application.

fun patent_drafter(tables) {
    receive {
        {'draft', project_id} ->
            print("[patent_drafter] Drafting patent application for project " ++ to_string(project_id))

            strategy = ets_get(elem(tables, 4), {'strategy', project_id})
            report = ets_get(elem(tables, 2), {'report', project_id})

            prompt = "You are a patent attorney drafting a USPTO-style patent application. Using the strategy and research below, write a complete patent application in markdown with these sections:\n\n# [PATENT TITLE]\n\n## Field of the Invention\n(1-2 sentences defining the technical field)\n\n## Background of the Invention\n(Prior art discussion, 200-300 words, identify shortcomings)\n\n## Summary of the Invention\n(Core innovation, 150-200 words)\n\n## Detailed Description of Preferred Embodiments\n(Technical implementation details, 400-600 words, reference to figures as needed)\n\n## Claims\n(Independent and dependent claims in proper patent claim format)\n\n## Abstract\n(150 words max)\n\nUse formal patent language. Claims must be single-sentence, each starting with proper claim formatting.\n\nPatent strategy:\n" ++ trunc(to_string(strategy), 2000) ++ "\n\nResearch report:\n" ++ trunc(to_string(report), 1500)
            draft = call_swarm(prompt)

            print("[patent_drafter] Draft complete (" ++ to_string(string_length(draft)) ++ " chars)")

            out_dir = "studio/output/patents/project_" ++ to_string(project_id)
            file_mkdir(out_dir)
            file_write(out_dir ++ "/patent_draft.md", draft)
            print("[patent_drafter] Written to " ++ out_dir ++ "/patent_draft.md")

            ets_put(elem(tables, 4), {'draft', project_id}, draft)
            send(whereis("director"), {'patent_drafted', project_id})
            patent_drafter(tables)

        {'revise_patent', project_id} ->
            print("[patent_drafter] Revising patent draft for project " ++ to_string(project_id))

            old_draft = ets_get(elem(tables, 4), {'draft', project_id})
            review_feedback = ets_get(elem(tables, 4), {'review', project_id})

            prompt = "You are a patent attorney revising a USPTO-style patent application based on reviewer feedback. Address every concern raised. Strengthen weak claims. Improve specificity where noted. Maintain proper patent formatting.\n\nOutput the COMPLETE revised patent application in the same format.\n\nReviewer feedback:\n" ++ trunc(to_string(review_feedback), 1500) ++ "\n\nCurrent draft:\n" ++ trunc(to_string(old_draft), 2500)
            draft = call_swarm(prompt)

            print("[patent_drafter] Revision complete (" ++ to_string(string_length(draft)) ++ " chars)")

            out_dir = "studio/output/patents/project_" ++ to_string(project_id)
            file_write(out_dir ++ "/patent_draft.md", draft)
            print("[patent_drafter] Revision written to " ++ out_dir ++ "/patent_draft.md")

            ets_put(elem(tables, 4), {'draft', project_id}, draft)
            send(whereis("director"), {'patent_drafted', project_id})
            patent_drafter(tables)
    }
}

# ── Patent Reviewer ──
# Scores patent on novelty, breadth, defensibility. Gates revision loop.

fun patent_reviewer(tables) {
    receive {
        {'review_patent', project_id} ->
            print("[patent_reviewer] Reviewing patent for project " ++ to_string(project_id))

            draft = ets_get(elem(tables, 4), {'draft', project_id})
            strategy = ets_get(elem(tables, 4), {'strategy', project_id})

            prompt = "You are a senior patent examiner reviewing a patent application. Score the following patent on three dimensions (1-10 each):\n\n1. NOVELTY — How new and non-obvious are the claims?\n2. BREADTH — How broad is the protection? Would competitors have to license?\n3. DEFENSIBILITY — How well would these claims survive a challenge?\n\nAlso assess: claim clarity, specification support, prior art differentiation.\n\nA patent PASSES if the average score >= 7.\n\nOutput ONLY valid JSON with no markdown fences:\n{\"novelty\": N, \"breadth\": N, \"defensibility\": N, \"average\": N.N, \"passed\": true/false, \"strengths\": \"...\", \"weaknesses\": \"...\", \"feedback\": \"specific improvements needed\"}\n\nPatent strategy:\n" ++ trunc(to_string(strategy), 1000) ++ "\n\nPatent draft:\n" ++ trunc(to_string(draft), 2500)
            raw = call_orc(prompt)

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

            # Store review for potential revision loop
            ets_put(elem(tables, 4), {'review', project_id}, raw)

            # Write patent review file
            review_report = "# Patent Review\n\n## Scores\n- Novelty: " ++ to_string(novelty_score) ++ "/10\n- Breadth: " ++ to_string(breadth_score) ++ "/10\n- Defensibility: " ++ to_string(defensibility_score) ++ "/10\n- **Average: " ++ to_string(avg) ++ "/10**\n- **Passed: " ++ to_string(passed) ++ "**\n\n## Strengths\n" ++ to_string(strengths) ++ "\n\n## Weaknesses\n" ++ to_string(weaknesses) ++ "\n\n## Feedback\n" ++ to_string(feedback)
            out_dir = "studio/output/patents/project_" ++ to_string(project_id)
            file_mkdir(out_dir)
            file_write(out_dir ++ "/patent_review.md", review_report)
            print("[patent_reviewer] Written to " ++ out_dir ++ "/patent_review.md")

            passed_atom = 'false'
            if (string_contains(to_string(passed), "true") == 'true') {
                passed_atom = 'true'
            }
            send(whereis("director"), {'patent_reviewed', project_id, avg, passed_atom})
            patent_reviewer(tables)
    }
}

# ══════════════════════════════════════════════════════════════════════════════
# STAGE 4: COMMERCIALIZATION — market analysis + sales pitch
# ══════════════════════════════════════════════════════════════════════════════

# ── Market Analyst ──
# Uses tools to gather real market data, competitor info, pricing benchmarks.

fun market_analyst(tables) {
    receive {
        {'analyze_market', project_id} ->
            print("[market_analyst] Analyzing market for project " ++ to_string(project_id))
            print("[market_analyst] Searching for real market data...")

            report = ets_get(elem(tables, 2), {'report', project_id})
            draft = ets_get(elem(tables, 4), {'draft', project_id})
            strategy = ets_get(elem(tables, 4), {'strategy', project_id})

            system = "You are a technology market analyst with access to real-time web search. Search for actual market data, company information, industry reports, and competitive intelligence. Produce a data-driven market analysis in markdown:\n\n# Market Analysis\n\n## Target Buyers\n(5-8 specific company types or named companies found via search)\n\n## Market Size\n(TAM/SAM/SOM with real data from web research)\n\n## Competitive Landscape\n(Real competitors found via search, their IP positions)\n\n## Licensing Strategy\n(Model and pricing with market comparables)\n\n## Risk Factors\n(Market, technology, and IP risks)\n\n## Revenue Projections\n(3-year projection grounded in real market data)\n\n600-800 words."
            task = "Patent strategy:\n" ++ trunc(to_string(strategy), 1000) ++ "\n\nPatent draft summary:\n" ++ trunc(to_string(draft), 800) ++ "\n\nResearch report:\n" ++ trunc(to_string(report), 800)
            analysis = tool_agent(system, task, 0, 3, 'swarm')

            print("[market_analyst] Analysis complete (" ++ to_string(string_length(analysis)) ++ " chars)")

            out_dir = "studio/output/patents/project_" ++ to_string(project_id)
            file_mkdir(out_dir)
            file_write(out_dir ++ "/market_analysis.md", analysis)
            print("[market_analyst] Written to " ++ out_dir ++ "/market_analysis.md")

            ets_put(elem(tables, 5), project_id, analysis)
            send(whereis("director"), {'market_done', project_id})
            market_analyst(tables)
    }
}

# ── Pitch Writer ──
# Produces a 1-page executive pitch for patent buyers.

fun pitch_writer(tables) {
    receive {
        {'write_pitch', project_id} ->
            print("[pitch_writer] Writing executive pitch for project " ++ to_string(project_id))

            market = ets_get(elem(tables, 5), project_id)
            draft = ets_get(elem(tables, 4), {'draft', project_id})
            review = ets_get(elem(tables, 4), {'review', project_id})
            strategy = ets_get(elem(tables, 4), {'strategy', project_id})

            prompt = "You are an executive pitch writer specializing in patent sales. Write a compelling 1-page pitch (400-500 words) in markdown that would convince a VP of R&D or corporate development officer to license or acquire this patent.\n\nStructure:\n# [Compelling headline — not the patent title]\n\n**The Opportunity** (2-3 sentences — what problem this solves and why now)\n\n**The Innovation** (3-4 sentences — what makes this patent unique)\n\n**Market Impact** (key numbers — market size, growth, competitive advantage)\n\n**Patent Strength** (scores, key claims, defensibility)\n\n**The Ask** (licensing terms, acquisition range, next steps)\n\n---\n*Generated by Patent Research Lab — SwarmRT*\n\nBe persuasive but accurate. Use concrete numbers from the market analysis. Create urgency.\n\nMarket analysis:\n" ++ trunc(to_string(market), 1200) ++ "\n\nPatent strategy:\n" ++ trunc(to_string(strategy), 800) ++ "\n\nPatent review scores:\n" ++ trunc(to_string(review), 500) ++ "\n\nPatent draft summary:\n" ++ trunc(to_string(draft), 500)
            pitch = call_swarm(prompt)

            print("[pitch_writer] Pitch complete (" ++ to_string(string_length(pitch)) ++ " chars)")

            out_dir = "studio/output/patents/project_" ++ to_string(project_id)
            file_mkdir(out_dir)
            file_write(out_dir ++ "/pitch.md", pitch)
            print("[pitch_writer] Written to " ++ out_dir ++ "/pitch.md")

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
    print("  ║   PATENT RESEARCH LAB  v0.2                           ║")
    print("  ║   16 agents — 4 stages — tool-augmented pipeline      ║")
    print("  ║   Research → Validate → Patent → Commercialize        ║")
    print("  ║   + Web Search, URL Fetch, Patent DB, Scholar Search  ║")
    print("  ║   SwarmRT / otonomy.ai                                ║")
    print("  ╚═══════════════════════════════════════════════════════╝")
    print("")

    # Create 7 ETS tables
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

    # Spawn all 16 agents + collectors
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

    print("[main] 16 agents spawned and registered:")
    print("")
    print("  Stage 1 — Research (tool-augmented):")
    print("    Director              (orc) — decomposes question")
    print("    Researcher Tech       (swarm + tools) — SEARCH, FETCH")
    print("    Researcher Market     (swarm + tools) — SEARCH, FETCH")
    print("    Researcher Prior Art  (swarm + tools) — PATENTS, SCHOLAR, SEARCH")
    print("    Findings Collector    (barrier: 3)")
    print("    Analyst               (orc) — synthesizes findings")
    print("    Research Editor       (swarm) — polishes report")
    print("")
    print("  Stage 2 — Validation:")
    print("    Peer Reviewer         (swarm)")
    print("    Novelty Assessor      (swarm + tools) — PATENTS, SEARCH")
    print("    Validation Collector  (barrier: 2)")
    print("    Validator             (swarm) — go/no-go gate")
    print("")
    print("  Stage 3 — Patent:")
    print("    Patent Strategist     (orc + tools) — PATENTS, SEARCH")
    print("    Patent Drafter        (swarm)")
    print("    Patent Reviewer       (orc) — scores + revision gate")
    print("")
    print("  Stage 4 — Commercialization:")
    print("    Market Analyst        (swarm + tools) — SEARCH, FETCH")
    print("    Pitch Writer          (swarm)")
    print("")
    print("  Tools: web_search, fetch_url, search_patents, search_scholar")
    print("  ReAct loop: agents autonomously decide what to search")
    print("")

    question = "Biodegradable microelectronics for precision agriculture — sensor networks that dissolve into soil nutrients after their monitoring lifecycle ends. Consider novel materials (silk fibroin substrates, zinc oxide circuits, cellulose encapsulation), power harvesting from soil microbiomes, mesh networking protocols for underground communication, and integration with drone-based deployment systems."

    print("[main] Research question:")
    print("  " ++ question)
    print("")
    print("[main] Pipeline: Research (3 parallel + web tools) → Validate → Patent → Commercialize")
    print("[main] Revision loops: validation (max 2), patent quality (max 2)")
    print("[main] Dispatching to director...")
    print("")
    send(pid_dir, {'question', 1, question})

    spawn(keeper())
    keeper()
}
