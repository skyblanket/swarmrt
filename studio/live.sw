module Live

# SwarmRT LiveView — helper module
#
# Provides:
#   render_page(title, body)  → full HTML page with embedded LiveView JS
#   html_escape(s) / h(s)    → escape HTML entities
#   attr(name, val)           → safe HTML attribute
#
# Usage:
#   import Live
#   html = Live.render_page("My App", "<h1>Hello</h1>")

export [render_page, html_escape, h, attr]

fun render_page(title, body) {
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">" ++
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">" ++
    "<title>" ++ html_escape(title) ++ "</title>" ++
    "<style>" ++
    "*{margin:0;padding:0;box-sizing:border-box}" ++
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;" ++
    "background:#0a0a0a;color:#e0e0e0}" ++
    "button{cursor:pointer;border:1px solid #333;background:#1a1a1a;color:#e0e0e0;" ++
    "border-radius:8px;transition:all 0.15s}" ++
    "button:hover{background:#2a2a2a;border-color:#555}" ++
    "button:active{transform:scale(0.97)}" ++
    "input,textarea,select{background:#1a1a1a;color:#e0e0e0;border:1px solid #333;" ++
    "border-radius:6px;padding:8px 12px;outline:none}" ++
    "input:focus,textarea:focus,select:focus{border-color:#666}" ++
    "</style>" ++
    "</head><body><div id=\"live-root\">" ++ body ++ "</div>" ++
    "<script>" ++ live_js() ++ "</script></body></html>"
}

fun html_escape(s) {
    s = string_replace(s, "&", "&amp;")
    s = string_replace(s, "<", "&lt;")
    s = string_replace(s, ">", "&gt;")
    string_replace(s, "\"", "&quot;")
}

fun h(s) {
    html_escape(s)
}

fun attr(name, val) {
    name ++ "=\"" ++ html_escape(to_string(val)) ++ "\""
}
