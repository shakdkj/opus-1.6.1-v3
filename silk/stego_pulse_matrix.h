/***********************************************************************
 SILK steganography: gain index matrix embedding.
***********************************************************************/
#ifndef SILK_STEGO_PULSE_MATRIX_H
#define SILK_STEGO_PULSE_MATRIX_H

#include "typedef.h"
#include "structs.h"

#define SILK_STEGO_MAX_BITS 6
#define SILK_STEGO_MAX_COST_DATA 2

/* Gain index matrix embedding over SILK subframes. */
void silk_stego_embed_mixed(
    opus_int8                   *pulses,
    SideInfoIndices             *psIndices,
    opus_int                    frame_length,
    opus_int                    fs_kHz,
    opus_int                    nb_subfr,
    opus_int                    signalType,
    opus_int                    condCoding,
    opus_int                    lag,
    opus_int                    stego_bits,
    opus_int                    stego_nbits,
    opus_int                    *applied_bits_out,
    opus_int                    *applied_nbits_out
);

void silk_stego_extract_mixed(
    const void                  *pulses,
    const SideInfoIndices       *psIndices,
    opus_int                    frame_length,
    opus_int                    fs_kHz,
    opus_int                    nb_subfr,
    opus_int                    signalType,
    opus_int                    *bits_out,
    opus_int                    *nbits_out
);

#endif /* SILK_STEGO_PULSE_MATRIX_H */
