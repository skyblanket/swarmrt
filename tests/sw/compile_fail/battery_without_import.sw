module Cf_battery_without_import

# Battery builtins (PDF, Chrome, the audio codecs) are compiled into a
# program only when it imports their module, so calling one without the
# import is a compile error on both paths, naming the import to add.
# expect-error: pdf_text() is in the Pdf battery, which this module doesn't import — add `import Pdf` to module Cf_battery_without_import
# expect-error: chrome_launch() is in the Chrome battery
# expect-error: audio_resample() is in the Audio battery

fun main() {
    t = pdf_text("x.pdf")
    p = chrome_launch()
    a = audio_resample("", 8000, 16000)
    print(t)
}
