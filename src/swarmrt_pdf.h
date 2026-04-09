/*
 * swarmrt_pdf.h — PDF text extraction API
 *
 * Pure C, no external deps except zlib. Zero-copy parsing.
 * otonomy.ai
 */

#ifndef SWARMRT_PDF_H
#define SWARMRT_PDF_H

#include <stdint.h>
#include <stddef.h>

/* === Error Codes === */
#define SW_PDF_OK            0
#define SW_PDF_ERR_PARSE    -1
#define SW_PDF_ERR_ALLOC    -2
#define SW_PDF_ERR_ENCRYPT  -3
#define SW_PDF_ERR_ZLIB     -4
#define SW_PDF_ERR_PAGES    -5

/* === Metadata === */
typedef struct {
    char *title;
    char *author;
    char *subject;
    char *creator;
    char *creation_date;
} sw_pdf_meta_t;

/* === Text Block (for structured extraction) === */
typedef struct {
    char *text;
    float x, y, w, h;
    float font_size;
} sw_pdf_text_block_t;

/* === Public API === */

/* Extract all text from PDF buffer. Returns malloc'd UTF-8 (caller frees). */
int sw_pdf_extract_text(const uint8_t *data, size_t len, char **out_text, size_t *out_len);

/* Extract specific pages (0-indexed, -1 terminated) */
int sw_pdf_extract_pages(const uint8_t *data, size_t len, const int *pages,
                         char **out_text, size_t *out_len);

/* Page count */
int sw_pdf_page_count(const uint8_t *data, size_t len, int *out_count);

/* Metadata (caller frees individual strings + struct fields) */
int sw_pdf_metadata(const uint8_t *data, size_t len, sw_pdf_meta_t *out);

/* Free metadata strings */
void sw_pdf_meta_free(sw_pdf_meta_t *meta);

/* Per-page structured extraction (text + bounding boxes for future OCR) */
int sw_pdf_extract_structured(const uint8_t *data, size_t len, int page_idx,
                              sw_pdf_text_block_t **out_blocks, int *out_count);

/* Free structured blocks */
void sw_pdf_blocks_free(sw_pdf_text_block_t *blocks, int count);

#endif /* SWARMRT_PDF_H */
