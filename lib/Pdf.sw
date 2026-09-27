# Pdf.sw — PDF text extraction. A battery: the PDF engine is compiled into
# a program only when it imports this module, so programs that don't read
# PDFs don't carry a parser for them.
#
#   import Pdf
#   text  = Pdf.text("report.pdf")     # string, or nil when unreadable
#   n     = Pdf.pages("report.pdf")    # int page count, or nil
#   info  = Pdf.meta("report.pdf")     # %{"title", "author", "subject",
#                                      #   "creator", "creation_date"} (present keys only)
#
# A module that imports Pdf may also call the builtins pdf_text, pdf_pages
# and pdf_meta directly; one that doesn't is rejected at compile time.

module Pdf

export [text, pages, meta]

fun text(path)  { pdf_text(path) }
fun pages(path) { pdf_pages(path) }
fun meta(path)  { pdf_meta(path) }
