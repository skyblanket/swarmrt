# Atelier Mally — Autonomous AI Creative Studio
# 7 AI agents on SwarmRT. French design aesthetic.
# otonomy.ai — 2026

module Atelier

export [main, directeur, concepteur, ingenieur_front, ingenieur_back, typographe, mouvement, critique, specs_collector, keeper]

# ── LLM Helpers (Otonomy Inference Proxy — OpenAI-compatible) ──

fun otonomy_url() {
    "https://otonomy-inference-production.up.railway.app/v1/chat/completions"
}

fun otonomy_headers() {
    key = getenv("OTONOMY_API_KEY")
    [{'Authorization', "Bearer " ++ key}, {'Content-Type', 'application/json'}]
}

fun extract_reply(resp) {
    result = "error: unknown"
    if (resp == nil) {
        print("[llm] ERROR: http_post returned nil")
        result = "error: no response"
    }
    if (resp != nil) {
        err = json_get(resp, "error")
        if (err != nil) {
            print("[llm] API ERROR: " ++ to_string(err))
        }
        choices = json_get(resp, "choices")
        if (choices == nil) {
            print("[llm] ERROR: no choices in response")
            print("[llm] raw resp: " ++ to_string(string_sub(resp, 0, 200)))
            result = "error: no choices"
        }
        if (choices != nil) {
            message = json_get(choices, "message")
            content = json_get(message, "content")
            if (content == nil) {
                content = json_get(message, "reasoning")
            }
            if (content == "") {
                content = json_get(message, "reasoning")
            }
            if (content == nil) {
                print("[llm] WARNING: content and reasoning both nil")
                result = "error: empty response"
            }
            if (content != nil) {
                result = content
            }
        }
    }
    result
}

# Truncate string to max_len chars
fun trunc(s, max_len) {
    result = s
    if (string_length(s) > max_len) {
        result = string_sub(s, 0, max_len)
    }
    result
}

fun call_opus(sys, msg) {
    body = "{\"model\":\"otonomy-orc\",\"max_tokens\":4096,\"messages\":[{\"role\":\"system\",\"content\":" ++ json_escape(sys) ++ "},{\"role\":\"user\",\"content\":" ++ json_escape(msg) ++ "}]}"
    print("[llm] body size: " ++ to_string(string_length(body)))
    resp = http_post(otonomy_url(), otonomy_headers(), body)
    extract_reply(resp)
}

fun call_sonnet(sys, msg) {
    body = "{\"model\":\"otonomy-swarm\",\"max_tokens\":4096,\"messages\":[{\"role\":\"system\",\"content\":" ++ json_escape(sys) ++ "},{\"role\":\"user\",\"content\":" ++ json_escape(msg) ++ "}]}"
    print("[llm] body size: " ++ to_string(string_length(body)))
    resp = http_post(otonomy_url(), otonomy_headers(), body)
    extract_reply(resp)
}

fun call_sonnet_long(sys, msg) {
    body = "{\"model\":\"otonomy-swarm\",\"max_tokens\":65536,\"messages\":[{\"role\":\"system\",\"content\":" ++ json_escape(sys) ++ "},{\"role\":\"user\",\"content\":" ++ json_escape(msg) ++ "}]}"
    print("[llm] body size: " ++ to_string(string_length(body)))
    resp = http_post(otonomy_url(), otonomy_headers(), body)
    extract_reply(resp)
}

# ── System Prompts (kept short to stay under proxy payload limits) ──

fun prompt_directeur() {
    "French design director. Output CONCISE direction under 150 words: concept, mood (3 words), 3 hex colors (no #000), 1 serif + 1 sans font, layout notes."
}

fun prompt_concepteur() {
    "Design system architect. Output compact JSON: colors (hex), fonts (families, 3 sizes rem), spacing, grid. No #000000."
}

fun prompt_typographe() {
    "Luxury brand copywriter. Output: headline (max 6 words), subheadline, 3 section headers, short body text, 2 CTAs, tagline."
}

fun prompt_mouvement() {
    "Animation director. Output CSS custom properties: easing cubic-bezier(0.16,1,0.3,1), durations 300-500ms, stagger 50-80ms, scroll triggers."
}

fun prompt_ingenieur() {
    "Frontend dev. Output a COMPLETE single HTML file (under 400 lines) with embedded CSS and JS. Keep CSS minimal and elegant — use max 150 lines of CSS. Semantic HTML5, responsive, accessible. Include hero, 3 product cards, about section, footer. Output ONLY raw HTML starting with <!DOCTYPE html> and ending with </html>. No markdown fences, no explanation."
}

fun prompt_ingenieur_back() {
    "Backend dev. Minimal server code. Output working code only."
}

fun prompt_critique() {
    "Critic. Score 1-10: craft, innovation, restraint, emotion, performance, a11y. Output ONLY JSON: {\"craft\":N,\"innovation\":N,\"restraint\":N,\"emotion\":N,\"performance\":N,\"a11y\":N,\"average\":N.N,\"passed\":true/false,\"feedback\":\"...\"}"
}

# ── Le Directeur Creatif ──

fun directeur(tables) {
    receive {
        {'brief', project_id, brief_text} ->
            print("[directeur] Brief received for project " ++ to_string(project_id))
            direction = call_opus(prompt_directeur(), brief_text)
            print("[directeur] Direction set (" ++ to_string(string_length(direction)) ++ " chars)")
            ets_put(elem(tables, 1), project_id, direction)
            ets_put(elem(tables, 0), project_id, {'concepting', 0})
            # Truncate direction for downstream agents to keep payload small
            dir_short = trunc(direction, 400)
            send(whereis("concepteur"), {'design', project_id, dir_short})
            send(whereis("typographe"), {'copy', project_id, dir_short})
            send(whereis("mouvement"), {'motion', project_id, dir_short})
            directeur(tables)

        {'specs_ready', project_id} ->
            print("[directeur] All specs ready. Starting build for project " ++ to_string(project_id))
            ets_put(elem(tables, 0), project_id, {'building', 0})
            send(whereis("ingenieur_front"), {'build', project_id, tables})
            directeur(tables)

        {'build_done', project_id} ->
            print("[directeur] Build complete. Sending to critique.")
            prev = ets_get(elem(tables, 0), project_id)
            prev_cycle = 0
            if (prev != nil) {
                prev_cycle = elem(prev, 1)
            }
            ets_put(elem(tables, 0), project_id, {'review', prev_cycle})
            send(whereis("critique"), {'review', project_id, tables})
            directeur(tables)

        {'review_done', project_id, avg_score, passed} ->
            if (passed == 'true') {
                print("[directeur] ════════════════════════════════")
                print("[directeur] SHIPPED project " ++ to_string(project_id) ++ " — score: " ++ to_string(avg_score))
                print("[directeur] ════════════════════════════════")
                ets_put(elem(tables, 0), project_id, {'shipped', 0})
            } else {
                proj = ets_get(elem(tables, 0), project_id)
                cycle = elem(proj, 1)
                if (cycle >= 3) {
                    print("[directeur] Max revisions reached. Shipping project " ++ to_string(project_id))
                    ets_put(elem(tables, 0), project_id, {'shipped', cycle})
                } else {
                    print("[directeur] Revision " ++ to_string(cycle + 1) ++ " for project " ++ to_string(project_id))
                    ets_put(elem(tables, 0), project_id, {'revision', cycle + 1})
                    send(whereis("ingenieur_front"), {'revise', project_id, tables})
                }
            }
            directeur(tables)
    }
}

# ── Le Concepteur ──

fun concepteur(tables) {
    receive {
        {'design', project_id, direction} ->
            print("[concepteur] Designing system for project " ++ to_string(project_id))
            tokens = call_sonnet(prompt_concepteur(), direction)
            print("[concepteur] Design tokens ready.")
            ets_put(elem(tables, 2), project_id, tokens)
            send(whereis("specs_collector"), {'spec_done', project_id, 'design'})
            concepteur(tables)
    }
}

# ── Le Typographe ──

fun typographe(tables) {
    receive {
        {'copy', project_id, direction} ->
            print("[typographe] Crafting copy for project " ++ to_string(project_id))
            copy = call_opus(prompt_typographe(), direction)
            print("[typographe] Copy ready.")
            ets_put(elem(tables, 3), project_id, copy)
            send(whereis("specs_collector"), {'spec_done', project_id, 'copy'})
            typographe(tables)
    }
}

# ── Le Directeur de Mouvement ──

fun mouvement(tables) {
    receive {
        {'motion', project_id, direction} ->
            print("[mouvement] Choreographing for project " ++ to_string(project_id))
            motion = call_sonnet(prompt_mouvement(), direction)
            print("[mouvement] Motion spec ready.")
            ets_put(elem(tables, 4), project_id, motion)
            send(whereis("specs_collector"), {'spec_done', project_id, 'motion'})
            mouvement(tables)
    }
}

# ── Specs Collector ──

fun specs_collector(tables) {
    receive {
        {'spec_done', project_id, spec_type} ->
            print("[collector] " ++ to_string(spec_type) ++ " done for project " ++ to_string(project_id))
            counter_key = {'spec_count', project_id}
            current = ets_get(elem(tables, 5), counter_key)
            count = 1
            if (current != nil) {
                count = elem(current, 0) + 1
            }
            ets_put(elem(tables, 5), counter_key, {count})
            if (count >= 3) {
                print("[collector] All 3 specs ready for project " ++ to_string(project_id))
                send(whereis("directeur"), {'specs_ready', project_id})
            }
            specs_collector(tables)
    }
}

# ── L'Ingenieur Frontend ──

fun ingenieur_front(tables) {
    receive {
        {'build', project_id, tbls} ->
            print("[ingenieur] Building project " ++ to_string(project_id))
            design = ets_get(elem(tbls, 2), project_id)
            copy = ets_get(elem(tbls, 3), project_id)
            motion = ets_get(elem(tbls, 4), project_id)
            direction = ets_get(elem(tbls, 1), project_id)
            print("[ingenieur] Specs loaded — building HTML...")
            # Truncate each spec to keep payload under proxy limit
            prompt = "Build a landing page.\nDirection: " ++ trunc(to_string(direction), 200) ++ "\nDesign: " ++ trunc(to_string(design), 200) ++ "\nCopy: " ++ trunc(to_string(copy), 200) ++ "\nAnimation: " ++ trunc(to_string(motion), 200)
            html = call_sonnet_long(prompt_ingenieur(), prompt)
            print("[ingenieur] Code generated. Writing files...")
            out_dir = "studio/output/project_" ++ to_string(project_id)
            file_mkdir(out_dir)
            file_write(out_dir ++ "/index.html", html)
            ets_put(elem(tbls, 5), {'html', project_id}, html)
            print("[ingenieur] Written to " ++ out_dir)
            send(whereis("directeur"), {'build_done', project_id})
            ingenieur_front(tables)

        {'revise', project_id, tbls} ->
            print("[ingenieur] Revising project " ++ to_string(project_id))
            feedback = ets_get(elem(tbls, 6), project_id)
            old_html = ets_get(elem(tbls, 5), {'html', project_id})
            prompt = "Revise this HTML.\nFeedback: " ++ trunc(to_string(feedback), 200) ++ "\nHTML: " ++ trunc(to_string(old_html), 400)
            html = call_sonnet_long(prompt_ingenieur(), prompt)
            out_dir = "studio/output/project_" ++ to_string(project_id)
            file_write(out_dir ++ "/index.html", html)
            ets_put(elem(tbls, 5), {'html', project_id}, html)
            print("[ingenieur] Revision written.")
            send(whereis("directeur"), {'build_done', project_id})
            ingenieur_front(tables)
    }
}

# ── L'Ingenieur Backend ──

fun ingenieur_back(tables) {
    receive {
        {'build_api', project_id, spec} ->
            print("[backend] Building API for project " ++ to_string(project_id))
            code = call_sonnet(prompt_ingenieur_back(), to_string(spec))
            out_dir = "studio/output/project_" ++ to_string(project_id)
            file_mkdir(out_dir)
            file_write(out_dir ++ "/server.js", code)
            ingenieur_back(tables)

        {'ping'} ->
            print("[backend] Standing by.")
            ingenieur_back(tables)
    }
}

# ── Le Critique ──

fun critique(tables) {
    receive {
        {'review', project_id, tbls} ->
            print("[critique] Reviewing project " ++ to_string(project_id))
            html = ets_get(elem(tbls, 5), {'html', project_id})
            direction = ets_get(elem(tbls, 1), project_id)
            review_msg = "Review:\nDirection: " ++ trunc(to_string(direction), 150) ++ "\nHTML: " ++ trunc(to_string(html), 500)
            review = call_opus(prompt_critique(), review_msg)
            print("[critique] Review complete.")
            avg = json_get(review, "average")
            passed = json_get(review, "passed")
            feedback = json_get(review, "feedback")
            ets_put(elem(tbls, 6), project_id, review)
            print("[critique] Score: " ++ to_string(avg) ++ " | Passed: " ++ to_string(passed))
            passed_atom = 'false'
            if (string_contains(to_string(passed), "true") == 'true') {
                passed_atom = 'true'
            }
            send(whereis("directeur"), {'review_done', project_id, avg, passed_atom})
            critique(tables)
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
    print("  ╔═══════════════════════════════════════╗")
    print("  ║       A T E L I E R   M A L L Y       ║")
    print("  ║     Autonomous Creative Studio        ║")
    print("  ║          SwarmRT / otonomy.ai          ║")
    print("  ╚═══════════════════════════════════════╝")
    print("")

    t_projects   = ets_new()
    t_directions = ets_new()
    t_tokens     = ets_new()
    t_copy       = ets_new()
    t_motion     = ets_new()
    t_artifacts  = ets_new()
    t_reviews    = ets_new()

    tables = {t_projects, t_directions, t_tokens, t_copy, t_motion, t_artifacts, t_reviews}
    print("[main] 7 ETS tables created")

    file_mkdir("studio/output")

    pid_d  = spawn(directeur(tables))
    pid_c  = spawn(concepteur(tables))
    pid_if = spawn(ingenieur_front(tables))
    pid_ib = spawn(ingenieur_back(tables))
    pid_t  = spawn(typographe(tables))
    pid_m  = spawn(mouvement(tables))
    pid_cr = spawn(critique(tables))
    pid_sc = spawn(specs_collector(tables))

    register("directeur", pid_d)
    register("concepteur", pid_c)
    register("ingenieur_front", pid_if)
    register("ingenieur_back", pid_ib)
    register("typographe", pid_t)
    register("mouvement", pid_m)
    register("critique", pid_cr)
    register("specs_collector", pid_sc)

    print("[main] 7 agents spawned + registered:")
    print("  Le Directeur Creatif      (Opus)")
    print("  Le Concepteur             (Sonnet)")
    print("  L'Ingenieur Frontend      (Sonnet)")
    print("  L'Ingenieur Backend       (Sonnet)")
    print("  Le Typographe             (Opus)")
    print("  Le Directeur de Mouvement (Sonnet)")
    print("  Le Critique               (Opus)")
    print("")

    brief = "Build a landing page for Maison Lumiere, a French luxury candle brand. Three scents: Jardin de Minuit, Brume d'Automne, Riviere de Soie. Dark background, warm amber accents, minimal. Waitlist signup. Parisian boutique at midnight."

    print("[main] Sending inaugural brief...")
    send(pid_d, {'brief', 1, brief})

    spawn(keeper())
    keeper()
}
