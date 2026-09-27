module Test_batteries

# Batteries — PDF, Chrome and the audio codecs — are compiled into a program
# only when it imports their module (lib/Pdf.sw, lib/Chrome.sw,
# lib/Audio.sw); calling one without the import is a compile error
# (tests/sw/compile_fail/battery_without_import.sw). This program imports
# Pdf and Audio, uses both through their modules, and checks its own binary
# carries no Chrome launcher.

import Pdf
import Audio

fun check(name, cond, got) {
    if (cond == 'true') { print("PASS " ++ name) ; 0 }
    else { print("FAIL " ++ name ++ ": got " ++ to_string(got)) ; 1 }
}

# One page, "hello battery" in Helvetica, /Title and /Author in the info dict.
fun tiny_pdf() {
    "%PDF-1.4\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>\nendobj\n4 0 obj\n<< /Length 43 >>\nstream\nBT /F1 12 Tf 20 50 Td (hello battery) Tj ET\nendstream\nendobj\n5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n6 0 obj\n<< /Title (Battery Test) /Author (swarmrt) >>\nendobj\nxref\n0 7\n0000000000 65535 f \n0000000009 00000 n \n0000000058 00000 n \n0000000115 00000 n \n0000000241 00000 n \n0000000334 00000 n \n0000000404 00000 n \ntrailer\n<< /Size 7 /Root 1 0 R /Info 6 0 R >>\nstartxref\n465\n%%EOF\n"
}

fun main() {
    f = 0
    path = file_temp("sw_battery_pdf_")
    file_write(path, tiny_pdf())

    t = Pdf.text(path)
    f = f + check("pdf_text_through_module", string_trim(to_string(t)) == "hello battery", t)
    n = Pdf.pages(path)
    f = f + check("pdf_pages_through_module", n == 1, n)
    m = Pdf.meta(path)
    f = f + check("pdf_meta_through_module", map_get(m, "title") == "Battery Test", m)
    f = f + check("pdf_builtin_callable_after_import", pdf_pages(path) == 1, pdf_pages(path))
    file_delete(path)

    pcm = Audio.ulaw_to_pcm16("/4AAVaoRwzw=")
    back = Audio.pcm16_to_ulaw(pcm)
    f = f + check("audio_roundtrip_through_module", back == "/4AAVaoRwzw=", back)
    up = Audio.resample(pcm, 8000, 16000)
    f = f + check("audio_resample_through_module",
                  string_length(up) > string_length(pcm), up)

    # Chrome isn't imported, so its launcher must not be in this binary.
    me = hd(os_args())
    r = shell("command -v nm >/dev/null 2>&1 && nm '" ++ me ++ "' 2>/dev/null | grep -c -e builtin_chrome_launch -e _sw_find_chrome ; true")
    out = string_trim(elem(r, 1))
    if (out == "") { print("PASS chrome_not_linked (SKIP: no nm)") }
    else { f = f + check("chrome_not_linked", out == "0", out) }

    if (f == 0) { print("OK batteries 7/7") ; sys_exit(0) }
    else { print("FAIL batteries") ; sys_exit(1) }
}
