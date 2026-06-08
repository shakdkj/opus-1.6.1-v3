/***********************************************************************
 SILK steganography: STC (Syndrome-Trellis Codes) over gain indices.
***********************************************************************/
#ifndef SILK_STEGO_STC_H
#define SILK_STEGO_STC_H

#include "typedef.h"

#define STC_H 6           /* constraint height (64 states) */
#define STC_W 12          /* submatrix width; alpha = h/w = 50% */

/* STC encode a block of frames.
   frame_cost[k][64] = cost per symbol per frame (INF = unreachable).
   K = number of frames (at least 2).
   msg/msg_bits = message to embed.
   stego_syms_out = output symbols (K values, 0-63 each).
   Returns 0 on success. */
opus_int silk_stc_encode(
    const opus_int          frame_cost[][ 64 ],
    opus_int                K,
    const opus_uint8        *msg,
    opus_int                msg_bits,
    opus_int8               stego_syms_out[]
);

/* Extract message from stego symbols.
   stego_syms = K symbol values (0-63).
   K = number of frames.
   msg_out = output buffer (byte-aligned).
   Returns number of message bits extracted. */
opus_int silk_stc_extract(
    const opus_int8         stego_syms[],
    opus_int                K,
    opus_uint8              *msg_out
);

#endif
