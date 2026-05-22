module Main

# SwarmRT LiveView — counter demo (the diff-protocol tracer bullet)
#
#   bin/swc build studio/counter_lv.sw -o /tmp/counter_lv
#   /tmp/counter_lv      ->  http://localhost:4000
#
# Mount sends the statics + dynamics once. Each +/- click pushes a
# diff carrying ONE changed dynamic — the number — not the page.

import LiveView

# initial assigns
fun mount(params) {
    %{count: 0}
}

# render -> a rendered struct; {{}} is the one dynamic hole.
fun render(assigns) {
    LiveView.h(
        "<div style=\"text-align:center;padding:4rem;font-family:sans-serif\">" ++
        "<h1 style=\"font-size:1.6rem;font-weight:600\">SwarmRT LiveView</h1>" ++
        "<p style=\"color:#888;margin:.5rem 0 2rem\">diff-based — only the number crosses the wire</p>" ++
        "<div style=\"font-size:6rem;font-weight:200;font-variant-numeric:tabular-nums\">{{}}</div>" ++
        "<div style=\"display:flex;gap:1rem;justify-content:center;margin-top:2rem\">" ++
        "<button sw-click=\"dec\" style=\"font-size:1.5rem;padding:.6rem 2rem\">-</button>" ++
        "<button sw-click=\"reset\" style=\"font-size:1.5rem;padding:.6rem 2rem\">reset</button>" ++
        "<button sw-click=\"inc\" style=\"font-size:1.5rem;padding:.6rem 2rem\">+</button>" ++
        "</div></div>",
        [map_get(assigns, 'count')]
    )
}

# events -> new assigns
fun handle_event(name, value, assigns) {
    count = map_get(assigns, 'count')
    next = if (name == "inc") { count + 1 }
           else { if (name == "dec") { count - 1 }
           else { if (name == "reset") { 0 }
           else { count }}}
    LiveView.assign(assigns, 'count', next)
}

fun main() {
    LiveView.start(4000, %{
        title: "Counter",
        mount: mount,
        render: render,
        handle_event: handle_event
    })
}
