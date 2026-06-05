module TestRunner

export [run_tests, parse_output, test_gate, format_result]

# Run tests and return structured results
# shell() returns {exit_code, stdout} as a tuple
fun run_tests(repo_path, command) {
  cmd = if (command == "") { "npm test" } else { command }
  full = "cd " ++ repo_path ++ " && " ++ cmd ++ " 2>&1"
  result = shell(full)
  exit_code = elem(result, 0)
  stdout = elem(result, 1)
  parsed = parse_output(stdout)

  %{
    framework: parsed.framework,
    passed: parsed.passed,
    failed: parsed.failed,
    skipped: parsed.skipped,
    total: parsed.total,
    duration_ms: parsed.duration_ms,
    failures: parsed.failures,
    raw: stdout,
    exit_code: exit_code
  }
}

# Parse test output from multiple frameworks
fun parse_output(output) {
  p = try_lodash_style(output)
  if (p.total > 0) { p }
  else {
    p = try_jest(output)
    if (p.total > 0) { p }
    else {
      p = try_mocha(output)
      if (p.total > 0) { p }
      else {
        p = try_pytest(output)
        if (p.total > 0) { p }
        else {
          p = try_vitest(output)
          if (p.total > 0) { p }
          else {
            %{
              framework: "unknown",
              passed: 0, failed: 0, skipped: 0, total: 0,
              duration_ms: 0, failures: [], raw: output
            }
          }
        }
      }
    }
  }
}

# Lodash: "PASS: 6825  FAIL: 0  TOTAL: 6825"
fun try_lodash_style(output) {
  if (string_contains(output, "PASS:") && string_contains(output, "TOTAL:")) {
    lines = string_split(output, "\n")
    found_line = find_first_line(lines, "PASS:", "TOTAL:")
    if (found_line != "") {
      passed = extract_number_after(found_line, "PASS:")
      failed = extract_number_after(found_line, "FAIL:")
      total = extract_number_after(found_line, "TOTAL:")
      %{
        framework: "custom",
        passed: passed, failed: failed, skipped: 0, total: total,
        duration_ms: 0, failures: []
      }
    } else {
      %{passed: 0, failed: 0, skipped: 0, total: 0}
    }
  } else {
    %{passed: 0, failed: 0, skipped: 0, total: 0}
  }
}

# Jest: "Tests: 5 failed, 10 passed, 15 total" or "Test Suites: 2 passed, 2 total"
fun try_jest(output) {
  if (string_contains(output, "Test Suites:") || string_contains(output, "Tests:")) {
    # Try to find "Tests:" line first
    tests_line = find_line_starting_with(output, "Tests:")
    if (tests_line != "") {
      passed = extract_number_before(tests_line, " passed")
      failed = extract_number_before(tests_line, " failed")
      total = extract_number_before(tests_line, " total")
      skipped = extract_number_before(tests_line, " skipped")
      if (total > 0) {
        %{
          framework: "jest",
          passed: passed, failed: failed, skipped: skipped, total: total,
          duration_ms: 0, failures: []
        }
      } else {
        %{passed: 0, failed: 0, skipped: 0, total: 0}
      }
    } else {
      # Fall back to "Test Suites:"
      suites_passed = extract_number_before(output, " passed")
      suites_total = extract_number_before(output, " total")
      if (suites_total > 0) {
        %{
          framework: "jest",
          passed: suites_passed, failed: suites_total - suites_passed,
          skipped: 0, total: suites_total,
          duration_ms: 0, failures: []
        }
      } else {
        %{passed: 0, failed: 0, skipped: 0, total: 0}
      }
    }
  } else {
    %{passed: 0, failed: 0, skipped: 0, total: 0}
  }
}

# Mocha: "passing (15)" and "failing (5)"
fun try_mocha(output) {
  if (string_contains(output, "passing (")) {
    passed = extract_number_in_parens(output, "passing (")
    failed = extract_number_in_parens(output, "failing (")
    total = passed + failed
    %{
      framework: "mocha",
      passed: passed, failed: failed, skipped: 0, total: total,
      duration_ms: 0, failures: []
    }
  } else {
    %{passed: 0, failed: 0, skipped: 0, total: 0}
  }
}

# pytest: "5 failed, 10 passed in 0.3s"
fun try_pytest(output) {
  if (string_contains(output, " passed in ")) {
    failed = extract_number_before(output, " failed")
    passed = extract_number_before(output, " passed")
    skipped = extract_number_before(output, " skipped")
    total = passed + failed + skipped
    %{
      framework: "pytest",
      passed: passed, failed: failed, skipped: skipped, total: total,
      duration_ms: 0, failures: []
    }
  } else {
    %{passed: 0, failed: 0, skipped: 0, total: 0}
  }
}

# Vitest: "Test Files  5 failed | 10 passed"
fun try_vitest(output) {
  if (string_contains(output, "Test Files")) {
    failed = extract_number_before(output, " failed")
    passed = extract_number_before(output, " passed")
    total = failed + passed
    %{
      framework: "vitest",
      passed: passed, failed: failed, skipped: 0, total: total,
      duration_ms: 0, failures: []
    }
  } else {
    %{passed: 0, failed: 0, skipped: 0, total: 0}
  }
}

# Test gate: run tests and return {ok: true/false, reason, details}
fun test_gate(repo_path, command) {
  result = run_tests(repo_path, command)

  if (result.exit_code != 0 && result.total == 0) {
    %{
      ok: 'false',
      reason: "Test command failed (exit " ++ result.exit_code ++ ")",
      details: result
    }
  } else if (result.failed > 0) {
    %{
      ok: 'false',
      reason: result.failed ++ "/" ++ result.total ++ " tests failed",
      details: result
    }
  } else if (result.passed == 0 && result.total == 0) {
    %{
      ok: 'false',
      reason: "No test output detected",
      details: result
    }
  } else {
    %{
      ok: 'true',
      reason: "All " ++ result.passed ++ "/" ++ result.total ++ " tests passed",
      details: result
    }
  }
}

# Format result for display
fun format_result(r) {
  fw = map_get(r, 'framework')
  passed = map_get(r, 'passed')
  failed = map_get(r, 'failed')
  total = map_get(r, 'total')
  "[" ++ fw ++ "] " ++ passed ++ " passed, " ++ failed ++ " failed, " ++ total ++ " total"
}

# --- String parsing helpers (tail-recursive) ---

# Extract number that appears right before a marker string
fun extract_number_before(text, marker) {
  idx = string_index_of(text, marker)
  if (idx == -1) { 0 }
  else {
    # Walk backwards, skip spaces, then read digits
    walk = idx - 1
    walk2 = skip_spaces_back(text, walk)
    read_digits_back(text, walk2, walk2 + 1)
  }
}

# Skip spaces going backwards
fun skip_spaces_back(text, pos) {
  if (pos < 0) { -1 }
  else if (string_sub(text, pos, 1) == " ") { skip_spaces_back(text, pos - 1) }
  else { pos }
}

# Read digits going backwards, return parsed int
fun read_digits_back(text, pos, end_pos) {
  if (pos < 0) { parse_digits(text, pos + 1, end_pos) }
  else {
    ch = string_sub(text, pos, 1)
    if (is_digit_char(ch)) { read_digits_back(text, pos - 1, end_pos) }
    else { parse_digits(text, pos + 1, end_pos) }
  }
}

# Extract substring and parse as int
fun parse_digits(text, start, end_pos) {
  if (start >= end_pos || start < 0) { 0 }
  else {
    num_str = string_sub(text, start, end_pos - start)
    parse_int_safe(num_str)
  }
}

# Extract number right after a marker
fun extract_number_after(text, marker) {
  idx = string_index_of(text, marker)
  if (idx == -1) { 0 }
  else {
    start = idx + string_length(marker)
    start2 = skip_spaces_forward(text, start)
    read_digits_forward(text, start2)
  }
}

# Skip spaces going forward
fun skip_spaces_forward(text, pos) {
  if (pos >= string_length(text)) { string_length(text) }
  else if (string_sub(text, pos, 1) == " ") { skip_spaces_forward(text, pos + 1) }
  else { pos }
}

# Read digits going forward, return parsed int
fun read_digits_forward(text, pos) {
  end_pos = find_non_digit(text, pos)
  parse_digits(text, pos, end_pos)
}

fun find_non_digit(text, pos) {
  if (pos >= string_length(text)) { pos }
  else {
    ch = string_sub(text, pos, 1)
    if (is_digit_char(ch)) { find_non_digit(text, pos + 1) }
    else { pos }
  }
}

fun is_digit_char(ch) {
  ch == "0" || ch == "1" || ch == "2" || ch == "3" || ch == "4" ||
  ch == "5" || ch == "6" || ch == "7" || ch == "8" || ch == "9"
}

# Extract number in parentheses after a marker: "passing (15)"
fun extract_number_in_parens(text, marker) {
  idx = string_index_of(text, marker)
  if (idx == -1) { 0 }
  else {
    start = idx + string_length(marker)
    end_pos = find_non_digit(text, start)
    parse_digits(text, start, end_pos)
  }
}

# Int parser (copied from background.sw pattern)
fun parse_int_safe(s) {
    parse_int_digits(s, 0, 0)
}

fun find_line_starting_with(text, prefix) {
  lines = string_split(text, "\n")
  find_line_with_prefix(lines, prefix)
}

fun find_line_with_prefix(lines, prefix) {
  case lines {
    [] -> ""
    [line | rest] -> if (string_starts_with(line, prefix)) {
      line
    } else {
      find_line_with_prefix(rest, prefix)
    }
  }
}

fun find_first_line(lines, marker1, marker2) {
  case lines {
    [] -> ""
    [line | rest] -> if (string_contains(line, marker1) && string_contains(line, marker2)) {
      line
    } else {
      find_first_line(rest, marker1, marker2)
    }
  }
}

fun parse_int_digits(s, i, acc) {
    if (i >= string_length(s)) { acc }
    else {
        ch = string_sub(s, i, 1)
        d = if (ch == "0") { 0 }
            else { if (ch == "1") { 1 }
            else { if (ch == "2") { 2 }
            else { if (ch == "3") { 3 }
            else { if (ch == "4") { 4 }
            else { if (ch == "5") { 5 }
            else { if (ch == "6") { 6 }
            else { if (ch == "7") { 7 }
            else { if (ch == "8") { 8 }
            else { if (ch == "9") { 9 }
            else { 0 - 1 }}}}}}}}}}
        if (d < 0) { acc }
        else { parse_int_digits(s, i + 1, acc * 10 + d) }
    }
}
