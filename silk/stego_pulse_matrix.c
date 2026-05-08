/***********************************************************************
 Gain index matrix steganography over SILK subframes.
***********************************************************************/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "main.h"
#include "stego_pulse_matrix.h"

static opus_int silk_stego_gain_symbol(
    const opus_int8             gains[ MAX_NB_SUBFR ],
    opus_int                    nbits
)
{
    opus_int mask = ( 1 << nbits ) - 1;
    return ( gains[ 0 ] + 3 * gains[ 1 ] + 9 * gains[ 2 ] + 27 * gains[ 3 ] ) & mask;
}

static void silk_stego_gain_bounds(
    opus_int                    condCoding,
    opus_int                    *gain_min,
    opus_int                    *gain_max
)
{
    *gain_min = 0;
    *gain_max = ( condCoding == CODE_CONDITIONALLY ) ?
        ( MAX_DELTA_GAIN_QUANT - MIN_DELTA_GAIN_QUANT ) : ( N_LEVELS_QGAIN - 1 );
}

static opus_int silk_stego_legacy_lsb_embed(
    SideInfoIndices             *psIndices,
    opus_int                    nb_subfr,
    opus_int                    condCoding,
    opus_int                    stego_bits,
    opus_int                    stego_nbits,
    opus_int                    *applied_bits_out,
    opus_int                    *applied_nbits_out
)
{
    opus_int gain_min, gain_max, applied = 0, n = 0;
    silk_stego_gain_bounds( condCoding, &gain_min, &gain_max );

    for( opus_int k = 0; k < nb_subfr && n < stego_nbits; k++ ) {
        opus_int target = ( stego_bits >> n ) & 0x1;
        opus_int cur = psIndices->GainsIndices[ k ];
        opus_int best = cur;
        opus_int best_cost = 1 << 30;

        for( opus_int d = -1; d <= 1; d++ ) {
            opus_int cand = cur + d;
            if( cand < gain_min || cand > gain_max ) {
                continue;
            }
            if( ( cand & 1 ) == target ) {
                opus_int cost = silk_abs( d );
                if( cost < best_cost || ( cost == best_cost && cand < best ) ) {
                    best_cost = cost;
                    best = cand;
                }
            }
        }
        if( best_cost >= ( 1 << 30 ) ) {
            break;
        }
        psIndices->GainsIndices[ k ] = (opus_int8)best;
        applied |= target << n;
        n++;
    }

    *applied_bits_out = applied;
    *applied_nbits_out = n;
    return n == stego_nbits;
}

static void silk_stego_legacy_lsb_extract(
    const SideInfoIndices       *psIndices,
    opus_int                    nb_subfr,
    opus_int                    *bits_out,
    opus_int                    *nbits_out
)
{
    opus_int bits = 0;
    opus_int n = 0;
    for( opus_int k = 0; k < nb_subfr && n < SILK_STEGO_MAX_BITS; k++ ) {
        bits |= ( psIndices->GainsIndices[ k ] & 1 ) << n;
        n++;
    }
    *bits_out = bits;
    *nbits_out = n;
}

static opus_int silk_stego_matrix_embed_gains(
    SideInfoIndices             *psIndices,
    opus_int                    condCoding,
    opus_int                    stego_bits,
    opus_int                    stego_nbits,
    opus_int                    *applied_bits_out,
    opus_int                    *applied_nbits_out
)
{
    opus_int gain_min, gain_max;
    opus_int8 original[ MAX_NB_SUBFR ];
    opus_int8 best_gains[ MAX_NB_SUBFR ];
    opus_int target = stego_bits & ( ( 1 << stego_nbits ) - 1 );
    opus_int best_cost = 1 << 30;
    opus_int best_cost2 = 1 << 30;
    opus_int found = 0;

    silk_stego_gain_bounds( condCoding, &gain_min, &gain_max );
    for( opus_int k = 0; k < MAX_NB_SUBFR; k++ ) {
        original[ k ] = psIndices->GainsIndices[ k ];
        best_gains[ k ] = psIndices->GainsIndices[ k ];
    }

    for( opus_int d0 = -2; d0 <= 2; d0++ ) {
        opus_int c0 = original[ 0 ] + d0;
        if( c0 < gain_min || c0 > gain_max ) continue;
        for( opus_int d1 = -2; d1 <= 2; d1++ ) {
            opus_int c1 = original[ 1 ] + d1;
            if( c1 < gain_min || c1 > gain_max ) continue;
            for( opus_int d2 = -2; d2 <= 2; d2++ ) {
                opus_int c2 = original[ 2 ] + d2;
                if( c2 < gain_min || c2 > gain_max ) continue;
                for( opus_int d3 = -2; d3 <= 2; d3++ ) {
                    opus_int c3 = original[ 3 ] + d3;
                    opus_int8 cand[ MAX_NB_SUBFR ];
                    opus_int cost, cost2;
                    if( c3 < gain_min || c3 > gain_max ) continue;

                    cand[ 0 ] = (opus_int8)c0;
                    cand[ 1 ] = (opus_int8)c1;
                    cand[ 2 ] = (opus_int8)c2;
                    cand[ 3 ] = (opus_int8)c3;
                    if( silk_stego_gain_symbol( cand, stego_nbits ) != target ) {
                        continue;
                    }

                    cost = silk_abs( d0 ) + silk_abs( d1 ) + silk_abs( d2 ) + silk_abs( d3 );
                    cost2 = d0 * d0 + d1 * d1 + d2 * d2 + d3 * d3;
                    if( !found || cost < best_cost ||
                        ( cost == best_cost && cost2 < best_cost2 ) ||
                        ( cost == best_cost && cost2 == best_cost2 &&
                          ( c0 < best_gains[ 0 ] ||
                            ( c0 == best_gains[ 0 ] && c1 < best_gains[ 1 ] ) ||
                            ( c0 == best_gains[ 0 ] && c1 == best_gains[ 1 ] && c2 < best_gains[ 2 ] ) ||
                            ( c0 == best_gains[ 0 ] && c1 == best_gains[ 1 ] && c2 == best_gains[ 2 ] && c3 < best_gains[ 3 ] ) ) ) ) {
                        found = 1;
                        best_cost = cost;
                        best_cost2 = cost2;
                        best_gains[ 0 ] = (opus_int8)c0;
                        best_gains[ 1 ] = (opus_int8)c1;
                        best_gains[ 2 ] = (opus_int8)c2;
                        best_gains[ 3 ] = (opus_int8)c3;
                    }
                }
            }
        }
    }

    if( !found || best_cost > SILK_STEGO_MAX_COST_DATA ) {
        *applied_bits_out = 0;
        *applied_nbits_out = 0;
        return 0;
    }

    for( opus_int k = 0; k < MAX_NB_SUBFR; k++ ) {
        psIndices->GainsIndices[ k ] = best_gains[ k ];
    }
    *applied_bits_out = target;
    *applied_nbits_out = stego_nbits;
    return 1;
}

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
)
{
    (void)pulses;
    (void)frame_length;
    (void)fs_kHz;
    (void)lag;
    (void)signalType;
    *applied_bits_out = 0;
    *applied_nbits_out = 0;

    if( psIndices == NULL || stego_nbits <= 0 ) {
        return;
    }
    stego_nbits = silk_min( stego_nbits, SILK_STEGO_MAX_BITS );

    if( nb_subfr >= MAX_NB_SUBFR ) {
        silk_stego_matrix_embed_gains(
            psIndices,
            condCoding,
            stego_bits,
            stego_nbits,
            applied_bits_out,
            applied_nbits_out
        );
        return;
    }

    stego_nbits = silk_min( stego_nbits, nb_subfr );
    silk_stego_legacy_lsb_embed(
        psIndices,
        nb_subfr,
        condCoding,
        stego_bits,
        stego_nbits,
        applied_bits_out,
        applied_nbits_out
    );
}

void silk_stego_extract_mixed(
    const void                  *pulses,
    const SideInfoIndices       *psIndices,
    opus_int                    frame_length,
    opus_int                    fs_kHz,
    opus_int                    nb_subfr,
    opus_int                    signalType,
    opus_int                    *bits_out,
    opus_int                    *nbits_out
)
{
    (void)pulses;
    (void)frame_length;
    (void)fs_kHz;
    (void)signalType;

    *bits_out = 0;
    *nbits_out = 0;
    if( psIndices == NULL ) {
        return;
    }

    if( nb_subfr >= MAX_NB_SUBFR ) {
        *bits_out = silk_stego_gain_symbol( psIndices->GainsIndices, SILK_STEGO_MAX_BITS );
        *nbits_out = SILK_STEGO_MAX_BITS;
        return;
    }

    silk_stego_legacy_lsb_extract( psIndices, nb_subfr, bits_out, nbits_out );
}
