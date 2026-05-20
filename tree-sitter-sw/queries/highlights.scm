; Tree-sitter highlights for sw. Drop into your editor's tree-sitter
; runtime (e.g. ~/.config/helix/runtime/queries/sw/ or VS Code).
;
; Theme groups picked to match the names every editor's theme paints
; consistently — @keyword, @function, @string, etc.

; Keywords
[
  "module"
  "import"
  "export"
  "fun"
  "spawn"
  "send"
  "receive"
  "after"
  "self"
  "if"
  "else"
  "case"
  "when"
  "try"
  "catch"
] @keyword

; Punctuation
[
  "->"
  "|>"
  "=>"
  "++"
  "&&"
  "||"
  "=="
  "!="
  "<="
  ">="
  "<"
  ">"
  "="
  ".."
  "+"
  "-"
  "*"
  "/"
  "%"
] @operator

[
  "(" ")"
  "{" "}"
  "[" "]"
  "%{"
  ","
  ":"
  ";"
  "|"
] @punctuation.delimiter

; Modules + imports
(module_decl name: (module_name) @namespace)
(import_decl name: (module_name) @namespace)
(export_decl (identifier) @function)
(module_call module: (module_name) @namespace name: (identifier) @function)

; Function definitions + calls
(function_definition name: (identifier) @function)
(parameter name: (identifier) @parameter)
(call callee: (identifier) @function.call)

; Literals
(integer) @number
(float) @number
(string) @string
(fstring) @string
(string_escape) @string.escape
(atom) @constant
(atom_shorthand_key) @property
(boolean) @constant.builtin
(nil) @constant.builtin
(wildcard) @variable.parameter

; Patterns + bindings — variables on the left of `=` and inside pattern
; positions read as bindings; otherwise an identifier is a reference.
(assignment name: (identifier) @variable)
(case_clause pattern: (identifier) @variable)
(case_clause error: (identifier) @variable.parameter)

(line_comment) @comment

; Special highlighting for common process / runtime calls
((call callee: (identifier) @function.builtin)
 (#match? @function.builtin "^(print|print_inline|length|hd|tl|elem|to_string|format|panic|expect|map|filter|reduce|spawn|self|send|register|whereis|sleep|timestamp|getenv|sys_exit|shell|file_read|file_write|json_encode|json_decode|http_get|http_post|http_post_stream|http_listen|http_respond|subprocess_spawn|subprocess_send_line|subprocess_recv_line|subprocess_close|db_open|db_close|db_exec|db_query|ets_new|ets_get|ets_put|ets_delete|ets_list|ets_count|map_get|map_put|map_keys|map_values|map_merge|map_has_key|map_size|map_remove|wsc_connect|wsc_send|wsc_recv|wsc_close|chrome_launch|base64_encode|base64_decode|string_length|string_sub|string_split|string_replace|string_contains|string_starts_with|string_ends_with|string_index_of|string_trim|string_upper|string_lower|read_line|read_char|read_choice|term_cols)$"))
