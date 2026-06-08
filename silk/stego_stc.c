/***********************************************************************
 SILK steganography: STC (Syndrome-Trellis Codes) over gain symbols.

 Binary STC with diagonal H. h=STC_H, w=STC_W.  Rate alpha=h/w=50%.
 State = accumulated syndrome He (XOR of selected H columns).
***********************************************************************/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "main.h"
#include "stego_stc.h"

#define STC_POLY 0x43
#define INF_COST (1 << 28)


/* H_hat column j = LFSR state after j shifts from init=1 */
static opus_int stc_hat_col( opus_int j )
{
    opus_int s = 1;
    for( opus_int i = 0; i < j; i++ ) {
        s <<= 1;
        if( s & ( 1 << STC_H ) ) s ^= STC_POLY;
    }
    return s & ( ( 1 << STC_H ) - 1 );
}


opus_int silk_stc_encode(
    const opus_int          frame_cost[][ 64 ],
    opus_int                K,
    const opus_uint8        *msg,
    opus_int                msg_bits,
    opus_int8               stego_syms_out[]
)
{
    opus_int n_bits    = K * 6;
    opus_int n_sub     = ( n_bits + STC_W - 1 ) / STC_W;
    opus_int cols      = n_sub * STC_W;
    opus_int total_msg = n_sub * STC_H;
    opus_int STATES    = 1 << STC_H;

    /* Pad message bytes */
    VARDECL( opus_uint8, mp );
    ALLOC( mp, ( total_msg + 7 ) / 8 );
    for( opus_int i = 0; i < ( total_msg + 7 ) / 8; i++ )
        mp[ i ] = ( i < ( msg_bits + 7 ) / 8 ) ? msg[ i ] : 0;

    /* Per-bit info */
    VARDECL( opus_int, fc );   /* flip cost */
    VARDECL( opus_int, ob );   /* original bit */
    ALLOC( fc, cols );
    ALLOC( ob, cols );

    /* Init all to padding default (can't flip) */
    for( opus_int i = 0; i < cols; i++ ) {
        fc[ i ] = INF_COST;
        ob[ i ] = 0;
    }

    for( opus_int k = 0; k < K; k++ ) {
        opus_int orig = 0, best = INF_COST;
        for( opus_int s = 0; s < 64; s++ ) {
            if( frame_cost[ k ][ s ] < best ) {
                best = frame_cost[ k ][ s ]; orig = s;
            }
        }
        stego_syms_out[ k ] = (opus_int8)orig;

        for( opus_int b = 0; b < 6; b++ ) {
            opus_int p = k * 6 + b;
            ob[ p ] = ( orig >> b ) & 1;
            opus_int fs = orig ^ ( 1 << b );
            opus_int c  = frame_cost[ k ][ fs ];
            if( c >= INF_COST ) c = 8;
            if( c > 8 ) c = 8;
            fc[ p ] = c;
        }
    }

    /* Precompute H_hat columns */
    opus_int hc[ STC_W ];
    for( opus_int j = 0; j < STC_W; j++ ) hc[ j ] = stc_hat_col( j );

    /* Per-submatrix Viterbi */
    for( opus_int sub = 0; sub < n_sub; sub++ ) {
        opus_int sc = ( sub + 1 == n_sub && n_bits % STC_W )
                      ? n_bits % STC_W : STC_W;
        opus_int c0   = sub * STC_W;
        opus_int m0   = sub * STC_H;
        opus_int ms   = silk_min( STC_H, total_msg - m0 );

        /* Target syndrome: m_i XOR H_i * x_i */
        opus_int target = 0, hx_syn = 0;
        for( opus_int i = 0; i < ms; i++ ) {
            target |= ( ( mp[ ( m0 + i ) >> 3 ] >> ( ( m0 + i ) & 7 ) ) & 1 ) << i;
        }
        for( opus_int j = 0; j < sc; j++ ) {
            if( ob[ c0 + j ] ) hx_syn ^= hc[ j ];
        }
        target ^= hx_syn;

        /* Forward pass */
        VARDECL( opus_int, dp );
        ALLOC( dp, ( sc + 1 ) * STATES );
        for( opus_int s = 0; s < STATES; s++ )
            dp[ 0 * STATES + s ] = INF_COST;
        dp[ 0 * STATES + 0 ] = 0;

        /* pred[col][state] = (prev_state << 1) | flipped */
        VARDECL( opus_int, pred );
        ALLOC( pred, sc * STATES );

        for( opus_int col = 0; col < sc; col++ ) {
            opus_int bi = c0 + col;
            opus_int flip_c = fc[ bi ];
            opus_int col_val = hc[ col ];

            for( opus_int s = 0; s < STATES; s++ )
                dp[ ( col + 1 ) * STATES + s ] = INF_COST;

            for( opus_int s = 0; s < STATES; s++ ) {
                opus_int cur = dp[ col * STATES + s ];
                if( cur >= INF_COST ) continue;

                /* e=0: state unchanged, cost unchanged */
                if( cur < dp[ ( col + 1 ) * STATES + s ] ) {
                    dp[ ( col + 1 ) * STATES + s ] = cur;
                    pred[ col * STATES + s ] = ( s << 1 ) | 0;
                }

                /* e=1: state XORs the column, cost + flip_cost */
                opus_int ns = s ^ col_val;
                opus_int nc = cur + flip_c;
                if( nc < dp[ ( col + 1 ) * STATES + ns ] ) {
                    dp[ ( col + 1 ) * STATES + ns ] = nc;
                    pred[ col * STATES + ns ] = ( s << 1 ) | 1;
                }
            }
        }

        /* Backtrack */
        if( dp[ sc * STATES + target ] < INF_COST ) {
            opus_int state = target;
            for( opus_int col = sc - 1; col >= 0; col-- ) {
                opus_int bi = c0 + col;
                opus_int entry  = pred[ col * STATES + state ];
                opus_int prev_s = entry >> 1;
                opus_int flipped = entry & 1;
                if( flipped && bi < n_bits ) {
                    opus_int fi = bi / 6, fb = bi % 6;
                    stego_syms_out[ fi ] ^= ( 1 << fb );
                }
                state = prev_s;
            }
        }
    }

    return 0;
}


opus_int silk_stc_extract(
    const opus_int8         stego_syms[],
    opus_int                K,
    opus_uint8              *msg_out
)
{
    opus_int n_bits    = K * 6;
    opus_int n_sub     = ( n_bits + STC_W - 1 ) / STC_W;
    opus_int total_msg = n_sub * STC_H;

    for( opus_int i = 0; i < ( total_msg + 7 ) / 8; i++ ) msg_out[ i ] = 0;

    for( opus_int sub = 0; sub < n_sub; sub++ ) {
        opus_int c0 = sub * STC_W;
        opus_int sc = ( sub + 1 == n_sub && n_bits % STC_W )
                      ? n_bits % STC_W : STC_W;
        opus_int m0 = sub * STC_H;

        opus_int syn = 0;
        for( opus_int j = 0; j < sc; j++ ) {
            opus_int col = c0 + j;
            opus_int fi  = col / 6, fb = col % 6;
            if( ( (opus_int)stego_syms[ fi ] >> fb ) & 1 ) {
                syn ^= stc_hat_col( j );
            }
        }

        for( opus_int i = 0; i < STC_H; i++ ) {
            if( m0 + i >= total_msg ) break;
            if( syn & ( 1 << i ) ) {
                msg_out[ ( m0 + i ) >> 3 ] |= ( 1 << ( ( m0 + i ) & 7 ) );
            }
        }
    }

    return total_msg;
}
