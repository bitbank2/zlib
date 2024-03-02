/* inffast.c -- fast decoding
 * Copyright (C) 1995-2017 Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#include "zutil.h"
#include "inftrees.h"
#include "inflate.h"
#include "inffast.h"

#if INTPTR_MAX == INT64_MAX
#define REGISTER_WIDTH 64
typedef uint64_t BIGUINT;
typedef uint32_t SMALLUINT;
#else
#define REGISTER_WIDTH 32
typedef uint32_t BIGUINT;
typedef uint16_t SMALLUINT;
#endif // native register size

#ifdef ASMINF
#  pragma message("Assembler code may have bugs -- use at your own risk")
#else

/*
   Decode literal, length, and distance codes and write out the resulting
   literal and match bytes until either not enough input or output is
   available, an end-of-block is encountered, or a data error is encountered.
   When large enough input and output buffers are supplied to inflate(), for
   example, a 16K input buffer and a 64K output buffer, more than 95% of the
   inflate execution time is spent in this routine.

   Entry assumptions:

        state->mode == LEN
        strm->avail_in >= 6
        strm->avail_out >= 258
        start >= strm->avail_out
        state->bits < 8

   On return, state->mode is one of:

        LEN -- ran out of enough output space or enough available input
        TYPE -- reached end of block code, inflate() to interpret next block
        BAD -- error in block data

   Notes:

    - The maximum input bits used by a length/distance pair is 15 bits for the
      length code, 5 bits for the length extra, 15 bits for the distance code,
      and 13 bits for the distance extra.  This totals 48 bits, or six bytes.
      Therefore if strm->avail_in >= 6, then there is enough input to avoid
      checking for available input while decoding.

    - The maximum bytes that a single length/distance pair can output is 258
      bytes, which is the maximum length that can be coded.  inflate_fast()
      requires strm->avail_out >= 258 for each loop to avoid checking for
      output space.
 */
void ZLIB_INTERNAL inflate_fast(z_streamp strm, unsigned start) {
    struct inflate_state FAR *state;
    z_const unsigned char FAR *in;      /* local strm->next_in */
    z_const unsigned char FAR *last;    /* have enough input while in < last */
    unsigned char FAR *out;     /* local strm->next_out */
    unsigned char FAR *beg;     /* inflate()'s initial strm->next_out */
    unsigned char FAR *end;     /* while out < end, enough space available */
#ifdef INFLATE_STRICT
    unsigned dmax;              /* maximum distance from zlib header */
#endif
    unsigned wsize;             /* window size or zero if not using window */
    unsigned whave;             /* valid bytes in the window */
    unsigned wnext;             /* window write index */
    unsigned char FAR *window;  /* allocated sliding window, if wsize != 0 */
    BIGUINT hold, tmpbits;         /* local strm->hold */
    unsigned bits;              /* local strm->bits */
//    code const FAR *lcode;      /* local strm->lencode */
//    code const FAR *dcode;      /* local strm->distcode */
    unsigned lmask;             /* mask for first level of length codes */
    unsigned dmask;             /* mask for first level of distance codes */
//    code const *here;           /* retrieved table entry */
    uint32_t here32, *plcode, *pdcode;
    unsigned op;                /* code bits, operation, extra bits, or */
                                /*  window position, window bytes to copy */
    unsigned len;               /* match length, unused bytes */
    unsigned dist;              /* match distance */
    unsigned char FAR *from;    /* where to copy match from */

    /* copy state to local variables */
    state = (struct inflate_state FAR *)strm->state;
    in = strm->next_in;
    last = in + (strm->avail_in - 5);
    out = strm->next_out;
    beg = out - (start - strm->avail_out);
    end = out + (strm->avail_out - 257);
#ifdef INFLATE_STRICT
    dmax = state->dmax;
#endif
    wsize = state->wsize;
    whave = state->whave;
    wnext = state->wnext;
    window = state->window;
    hold = state->hold;
    bits = state->bits;
    plcode = (uint32_t *)state->lencode;
    pdcode = (uint32_t *)state->distcode;
//    lcode = state->lencode;
//    dcode = state->distcode;
    lmask = (1U << state->lenbits) - 1;
    dmask = (1U << state->distbits) - 1;

    /* decode literals and length/distances until end-of-block or not enough
       input data or output space */
    // the 32-bit packed structure 'code' is quicker to load as a 32-bit value
    // instead of 3 separate loads
    // 31-------------16 15-----8 7------0 // bit number
    // vvvvvvvvvvvvvvvvv bbbbbbbb oooooooo // val, bits, op
    do {
        if (bits < (REGISTER_WIDTH/2)) { // helps on 32 and 64-bit CPUs
#ifdef UNALIGNED_OK
            tmpbits = *(SMALLUINT *)in;
            hold |= (BIGUINT)(tmpbits << bits);
            in += sizeof(SMALLUINT);
            bits += (REGISTER_WIDTH / 2);
#else
            hold += (unsigned long)(*in++) << bits;
            bits += 8;
            hold += (unsigned long)(*in++) << bits;
            bits += 8;
#endif
        }
       // here = lcode + (hold & lmask);
        here32 = plcode[hold & lmask];
      dolen:
//        op = (unsigned)(here->bits);
        op = (here32 >> 8) & 0xff; // bits
        hold >>= op;
        bits -= op;
//        op = (unsigned)(here->op);
        op = here32 & 0xff; // op
        if (op == 0) {                          /* literal */
//            Tracevv((stderr, here->val >= 0x20 && here->val < 0x7f ?
//                    "inflate:         literal '%c'\n" :
//                    "inflate:         literal 0x%02x\n", here->val));
//            *out++ = (unsigned char)(here->val);
            *out++ = (uint8_t)(here32 >> 16);
        }
        else if (op & 16) {                     /* length base */
//            len = (unsigned)(here->val);
            len = here32 >> 16; // val
            op &= 15;                           /* number of extra bits */
            if (op) {
#if REGISTER_WIDTH == 32
                if (bits < op) {
                    hold += (BIGUINT)(*in++) << bits;
                    bits += 8;
                }
#endif
                len += (unsigned)hold & ((1U << op) - 1);
                hold >>= op;
                bits -= op;
            }
            Tracevv((stderr, "inflate:         length %u\n", len));
            if (bits < (REGISTER_WIDTH/2)) { // helps on 32 and 64-bit CPUs
#ifdef UNALIGNED_OK
                tmpbits = *(SMALLUINT *)in;
                hold |= (BIGUINT)(tmpbits << bits);
                in += sizeof(SMALLUINT);
                bits += (REGISTER_WIDTH / 2);
#else
                hold += (unsigned long)(*in++) << bits;
                bits += 8;
                hold += (unsigned long)(*in++) << bits;
                bits += 8;
#endif
            }
//            here = dcode + (hold & dmask);
            here32 = pdcode[hold & dmask];
          dodist:
//            op = (unsigned)(here->bits);
            op = (here32 >> 8) & 0xff; // bits
            hold >>= op;
            bits -= op;
//            op = (unsigned)(here->op);
            op = here32 & 0xff; // op
            if (op & 16) {                      /* distance base */
//                dist = (unsigned)(here->val);
                dist = here32 >> 16; // val
                op &= 15;                       /* number of extra bits */
#if REGISTER_WIDTH == 32
                if (bits < op) {
                    hold += (BIGUINT)(*in++) << bits;
                    bits += 8;
                    if (bits < op) {
                        hold += (BIGUINT)(*in++) << bits;
                        bits += 8;
                    }
                }
#endif
                dist += (unsigned)hold & ((1U << op) - 1);
#ifdef INFLATE_STRICT
                if (dist > dmax) {
                    strm->msg = (char *)"invalid distance too far back";
                    state->mode = BAD;
                    break;
                }
#endif
                hold >>= op;
                bits -= op;
                Tracevv((stderr, "inflate:         distance %u\n", dist));
                op = (unsigned)(out - beg);     /* max distance in output */
                if (dist > op) {                /* see if copy from window */
                    op = dist - op;             /* distance back in window */
                    if (op > whave) {
                        if (state->sane) {
                            strm->msg =
                                (char *)"invalid distance too far back";
                            state->mode = BAD;
                            break;
                        }
#ifdef INFLATE_ALLOW_INVALID_DISTANCE_TOOFAR_ARRR
                        if (len <= op - whave) {
                            do {
                                *out++ = 0;
                            } while (--len);
                            continue;
                        }
                        len -= op - whave;
                        do {
                            *out++ = 0;
                        } while (--op > whave);
                        if (op == 0) {
                            from = out - dist;
                            do {
                                *out++ = *from++;
                            } while (--len);
                            continue;
                        }
#endif
                    }
                    from = window;
                    if (wnext == 0) {           /* very common case */
                        from += wsize - op;
                        if (op < len) {         /* some from window */
                            len -= op;
                            do {
                                *out++ = *from++;
                            } while (--op);
                            from = out - dist;  /* rest from output */
                        }
                    }
                    else if (wnext < op) {      /* wrap around window */
                        from += wsize + wnext - op;
                        op -= wnext;
                        if (op < len) {         /* some from end of window */
                            len -= op;
                            do {
                                *out++ = *from++;
                            } while (--op);
                            from = window;
                            if (wnext < len) {  /* some from start of window */
                                op = wnext;
                                len -= op;
                                do {
                                    *out++ = *from++;
                                } while (--op);
                                from = out - dist;      /* rest from output */
                            }
                        }
                    }
                    else {                      /* contiguous in window */
                        from += wnext - op;
                        if (op < len) {         /* some from window */
                            len -= op;
                            do {
                                *out++ = *from++;
                            } while (--op);
                            from = out - dist;  /* rest from output */
                        }
                    }
#ifdef UNALIGNED_OK
                    {
                        uint8_t *pEnd = out+len;
                        intptr_t overlap = (intptr_t)(from - out);
                        if (overlap >= 4) { // ok
                            while (out < pEnd) {
                                *(uint32_t *)out = *(uint32_t *)from;
                                out += 4;
                                from += 4;
                            }
                            // correct for possible overshoot of destination ptr
                            out = pEnd;
                        } else if (overlap == 1) { // pattern
                            uint32_t pattern = *from;
                            pattern = pattern | (pattern << 8);
                            pattern = pattern | (pattern << 16);
                            while (out < pEnd) {
                                *(uint32_t *)out = pattern;
                                out += 4;
                            }
                            out = pEnd;
                        } else { // slow pattern
                            while (out < pEnd) {
                                *out++ = *from++;
                            }
                        }
                    }
#else
                    while (len > 2) {
                        *out++ = *from++;
                        *out++ = *from++;
                        *out++ = *from++;
                        len -= 3;
                    }
                    if (len) {
                        *out++ = *from++;
                        if (len > 1)
                            *out++ = *from++;
                    }
#endif // UNALIGNED_OK
                }
                else {
                    from = out - dist;          /* copy direct from output */
#ifdef UNALIGNED_OK
                    {
                        uint8_t *pEnd = out+len;
                        int overlap = (int)(intptr_t)(out-from);
                        if (overlap >= 4) { // overlap of source/dest won't impede normal copy
                            while (out < pEnd) {
                                *(uint32_t *)out = *(uint32_t *)from;
                                out += 4;
                                from += 4;
                            }
                            // correct for possible overshoot of destination ptr
                            out = pEnd;
                        } else if (overlap == 1) { // copy 1-byte pattern
                            uint32_t pattern = *from;
                            pattern = pattern | (pattern << 8);
                            pattern = pattern | (pattern << 16);
                            while (out < pEnd) {
                                *(uint32_t *)out = pattern;
                                out += 4;
                            }
                            out = pEnd; // correct possible overshoot
                        } else { // overlap of 2 or 3
                            while (out < pEnd) {
                                *out++ = *from++;
                            }
                        }
                    }
#else
                    do {                        /* minimum length is three */
                        *out++ = *from++;
                        *out++ = *from++;
                        *out++ = *from++;
                        len -= 3;
                    } while (len > 2);
                    if (len) {
                        *out++ = *from++;
                        if (len > 1)
                            *out++ = *from++;
                    }
#endif // UNALIGNED_OK
                }
            }
            else if ((op & 64) == 0) {          /* 2nd level distance code */
//                here = dcode + here->val + (hold & ((1U << op) - 1));
                here32 = pdcode[(here32>>16) + (hold & ((1U << op) - 1))];
                goto dodist;
            }
            else {
                strm->msg = (char *)"invalid distance code";
                state->mode = BAD;
                break;
            }
        }
        else if ((op & 64) == 0) {              /* 2nd level length code */
//            here = lcode + here->val + (hold & ((1U << op) - 1));
            here32 = plcode[(here32 >> 16) + (hold & ((1U << op) - 1))];
            goto dolen;
        }
        else if (op & 32) {                     /* end-of-block */
            Tracevv((stderr, "inflate:         end of block\n"));
            state->mode = TYPE;
            break;
        }
        else {
            strm->msg = (char *)"invalid literal/length code";
            state->mode = BAD;
            break;
        }
    } while (in < last && out < end);

    /* return unused bytes (on entry, bits < 8, so in won't go too far back) */
    len = bits >> 3;
    in -= len;
    bits -= len << 3;
    hold &= (1U << bits) - 1;

    /* update state and return */
    strm->next_in = in;
    strm->next_out = out;
    strm->avail_in = (unsigned)(in < last ? 5 + (last - in) : 5 - (in - last));
    strm->avail_out = (unsigned)(out < end ?
                                 257 + (end - out) : 257 - (out - end));
    state->hold = hold;
    state->bits = bits;
    return;
}

/*
   inflate_fast() speedups that turned out slower (on a PowerPC G3 750CXe):
   - Using bit fields for code structure
   - Different op definition to avoid & for extra bits (do & for table bits)
   - Three separate decoding do-loops for direct, window, and wnext == 0
   - Special case for distance > 1 copies to do overlapped load and store copy
   - Explicit branch predictions (based on measured branch probabilities)
   - Deferring match copy and interspersed it with decoding subsequent codes
   - Swapping literal/length else
   - Swapping window/direct else
   - Larger unrolled copy loops (three is about right)
   - Moving len -= 3 statement into middle of loop
 */

#endif /* !ASMINF */
