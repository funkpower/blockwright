/* C ABI over the ans2irc encoder (ffi/ans2irc-ffi). */
#ifndef ANS2IRC_FFI_H
#define ANS2IRC_FFI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One cell: a Unicode scalar value and mIRC colours 0-98. */
typedef struct {
    uint32_t ch;
    uint8_t fg;
    uint8_t bg;
} ans2irc_cell;

/* Bytes owned by the library; release with ans2irc_buf_free. */
typedef struct {
    uint8_t* data;
    size_t len;
} ans2irc_buf;

enum {
    ANS2IRC_OK = 0,
    ANS2IRC_BAD_ARGUMENT = 1, /* null pointer, bad code point or colour */
    ANS2IRC_PANIC = 2
};

/* The shortest IRC line that draws `cells` (no line ending). */
int ans2irc_encode_row(const ans2irc_cell* cells, size_t n, int swap_shades, ans2irc_buf* out);

/* The mIRC colour nearest to 0xRRGGBB. */
uint8_t ans2irc_nearest(uint32_t rgb);

void ans2irc_buf_free(ans2irc_buf* buf);

#ifdef __cplusplus
}
#endif

#endif
