#pragma once
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Wire format for the raw ADC stream (system-design/rack-manager.md section 2).
 *
 * The ESP is a TCP client: it dials the collector, sends a JSON handshake
 * describing how the ADCs are configured, then pushes fixed-layout binary
 * frames until the connection breaks.
 *
 * Everything here is byte arrays and explicit shifts rather than packed
 * bitfields or multi-byte struct members. That is deliberate. Bitfield
 * allocation order, whether a field may straddle a storage unit, and the
 * signedness of a plain int field are all implementation-defined (C11
 * 6.7.2.1), and none of them are visible to static_assert - several wrong
 * layouts are still the right size. With every member a uint8_t there is no
 * bit order, no endianness and no alignment to get wrong, and the one thing
 * left, trailing padding, is caught at compile time below.
 *
 * The cost is put_u16le(h.n_frames, n) instead of h.n_frames = n. That is the
 * whole downside, and it buys a format that is identical under any compiler on
 * any host the collector ever runs on.
 * ------------------------------------------------------------------------- */

/* Message tags. Readable in the source and in a hexdump, which is the only
 * reason they are ASCII.
 *
 * The trailing digit on a binary tag is its layout version: change the frame
 * and change the tag, so an out-of-date collector rejects the connection
 * instead of quietly misparsing. HELLO carries no version because it is JSON
 * and tolerates new fields on its own. */
#define WIRE_FOURCC(a, b, c, d)                                    \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | \
     ((uint32_t)(d) << 24))

static const uint32_t WIRE_MAGIC_HELLO = WIRE_FOURCC('H', 'E', 'L', 'O');
static const uint32_t WIRE_MAGIC_ADC   = WIRE_FOURCC('A', 'D', 'C', '0');

/* Channels carried per frame. ADC3 ch1 is not sent - bus current is derived
 * (adc.h). Fixed, not implied by ch_mask: a constant stride is what lets the
 * collector seek by index arithmetic and decode with one numpy dtype, and a
 * chip that drops offline is a fault rather than a normal mode. */
static const uint8_t WIRE_ADC_CHANNELS = 5;

/* Little-endian 24-bit two's complement, exactly as it sits on the wire. */
typedef struct {
    uint8_t b[3];
} i24le;

static inline void i24le_set(i24le *f, int32_t v)
{
    f->b[0] = (uint8_t)(v);
    f->b[1] = (uint8_t)(v >> 8);
    f->b[2] = (uint8_t)(v >> 16);
}

static inline int32_t i24le_get(const i24le *f)
{
    uint32_t u = (uint32_t)f->b[0] | ((uint32_t)f->b[1] << 8) |
                 ((uint32_t)f->b[2] << 16);
    /* Sign-extend without relying on the sign of a right shift, which is also
     * implementation-defined. The collector does the same thing:
     *   codes = (u ^ 0x800000) - 0x800000                                  */
    return (int32_t)(u ^ 0x800000u) - 0x800000;
}

/**
 * One conversion frame, ordered by ADC position and never by signal meaning:
 * what is wired to each input belongs to the board (system-design/design.md)
 * and to the collector, not to this struct.
 *
 *   ch[0] ADC1 ch0    ch[1] ADC1 ch1
 *   ch[2] ADC2 ch0    ch[3] ADC2 ch1
 *   ch[4] ADC3 ch0
 */
typedef struct {
    i24le ch[WIRE_ADC_CHANNELS];
} AdcFrame;
static_assert(sizeof(AdcFrame) == 15, "AdcFrame must pack to 15 bytes");

/**
 * Packet header, followed immediately by n_frames x AdcFrame.
 *
 * The payload length is derivable (n_frames x sizeof(AdcFrame)) so no explicit
 * length is sent. magic stays first so a reader can dispatch on four bytes
 * before it knows how long the rest of the header is.
 */
typedef struct {
    uint8_t magic[4];             /* WIRE_MAGIC_ADC                          */
    uint8_t first_sample_idx[8];  /* u64 LE; index of frame[0] in this session*/
    uint8_t n_frames[2];          /* u16 LE                                  */
    uint8_t ch_mask;              /* bit n: ch[n] carries valid data         */
    uint8_t flags;                /* WIRE_FLAG_*                             */
} AdcPacketHeader;
static_assert(sizeof(AdcPacketHeader) == 16, "AdcPacketHeader must be 16 bytes");

enum {
    /* Samples were lost immediately before this packet. The jump in
     * first_sample_idx says how many, so a gap is always self-describing and
     * the collector can tell lost data from a quiet line. */
    WIRE_FLAG_GAP     = 1u << 0,
    /* At least one frame here failed the ADC's own CRC. The frame is still
     * sent: a corrupt sample stays visible rather than leaving a hole in the
     * timebase (adc.h). */
    WIRE_FLAG_CRC_ERR = 1u << 1,
    /* First packet after adcSync(), so the frames before and after are not
     * contiguous in conversion phase. */
    WIRE_FLAG_RESYNC  = 1u << 2,
};

/**
 * JSON handshake header, followed by json_len bytes of JSON. Sent once,
 * immediately on connect, before any AdcPacketHeader.
 */
typedef struct {
    uint8_t magic[4];    /* WIRE_MAGIC_HELLO */
    uint8_t json_len[4]; /* u32 LE           */
} WireHelloHeader;
static_assert(sizeof(WireHelloHeader) == 8, "WireHelloHeader must be 8 bytes");

/* Largest packet that still fits one 1460-byte TCP segment. 1460 - 16 = 1444,
 * so 96 frames = 1440 bytes of payload and 1456 on the wire. */
static const uint16_t WIRE_MAX_FRAMES = 96;
static_assert(sizeof(AdcPacketHeader) + WIRE_MAX_FRAMES * sizeof(AdcFrame) <= 1460,
              "packet must fit one segment");

static inline void put_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

static inline void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline void put_u64le(uint8_t *p, uint64_t v)
{
    put_u32le(p, (uint32_t)v);
    put_u32le(p + 4, (uint32_t)(v >> 32));
}

/**
 * Prove the layout is what the collector is told it is. sizeof() alone does
 * not: it cannot see byte order, and it cannot see sign extension. Cheap
 * enough to run unconditionally at boot, where it catches a toolchain change
 * rather than letting it reach the wire.
 */
static inline bool wireSelfTest(void)
{
    AdcFrame f;
    memset(&f, 0, sizeof(f));

    i24le_set(&f.ch[0], 0x123456);
    i24le_set(&f.ch[1], -1);
    i24le_set(&f.ch[2], -8388608);   /* most negative 24-bit code */
    i24le_set(&f.ch[3], 8388607);    /* most positive             */

    const uint8_t *b = (const uint8_t *)&f;
    bool ok = b[0] == 0x56 && b[1] == 0x34 && b[2] == 0x12    /* LSB first  */
           && b[3] == 0xFF && b[4] == 0xFF && b[5] == 0xFF;   /* sign extends */

    ok = ok && i24le_get(&f.ch[0]) == 0x123456
            && i24le_get(&f.ch[1]) == -1
            && i24le_get(&f.ch[2]) == -8388608
            && i24le_get(&f.ch[3]) == 8388607
            && i24le_get(&f.ch[4]) == 0;

    uint8_t idx[8];
    put_u64le(idx, 0x0123456789ABCDEFull);
    ok = ok && idx[0] == 0xEF && idx[7] == 0x01;

    return ok;
}
