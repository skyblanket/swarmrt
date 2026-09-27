module Conform_batteries

# The Pdf and Audio batteries give the same answers interpreted and compiled
# (the interpreter's copies live in swarmrt_battery_interp.c).

import Pdf
import Audio

fun main() {
    path = "/tmp/sw_conform_t18_batteries.pdf"   # file_temp is compiled-only
    file_write(path, "%PDF-1.4\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>\nendobj\n4 0 obj\n<< /Length 43 >>\nstream\nBT /F1 12 Tf 20 50 Td (hello battery) Tj ET\nendstream\nendobj\n5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n6 0 obj\n<< /Title (Battery Test) /Author (swarmrt) >>\nendobj\nxref\n0 7\n0000000000 65535 f \n0000000009 00000 n \n0000000058 00000 n \n0000000115 00000 n \n0000000241 00000 n \n0000000334 00000 n \n0000000404 00000 n \ntrailer\n<< /Size 7 /Root 1 0 R /Info 6 0 R >>\nstartxref\n465\n%%EOF\n")
    print(string_trim(to_string(Pdf.text(path))))
    print(to_string(Pdf.pages(path)))
    print(to_string(map_get(Pdf.meta(path), "author")))
    file_delete(path)
    pcm = Audio.ulaw_to_pcm16("/4AAVaoRwzw=")
    print(pcm)
    print(Audio.pcm16_to_ulaw(pcm))
    print(Audio.resample(pcm, 8000, 16000))
}
