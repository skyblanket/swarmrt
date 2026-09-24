# Chrome.sw — start a local Chromium for CDP automation. A battery: the
# browser launcher is compiled into a program only when it imports this
# module.
#
#   import Chrome
#   port = Chrome.launch()               # headless on 9222 → port, or nil
#   port = Chrome.launch_on(9333, 'false')   # a visible window on 9333
#
# Finds Chrome / Chromium / Brave / Edge / Arc (or a Playwright cache),
# starts it with --remote-debugging-port and an isolated profile, and waits
# for the port; a browser already listening there is reused. Drive it with
# the core WebSocket client (wsc_connect on the page's webSocketDebuggerUrl).
#
# A module that imports Chrome may also call chrome_launch directly; one
# that doesn't is rejected at compile time.

module Chrome

export [launch, launch_on]

fun launch() { chrome_launch() }
fun launch_on(port, headless) { chrome_launch(port, headless) }
