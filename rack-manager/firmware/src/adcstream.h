#pragma once
#include <Arduino.h>

/**
 * Raw ADC waveform stream (system-design/rack-manager.md section 2).
 *
 * Two tasks and a ring between them:
 *
 *   sampler (core 1)  DRDY edge -> adcReadSampleAll() -> ring
 *   pusher  (core 0)  ring -> TCP -> collector
 *
 * The ESP dials out; the collector never connects in. Backpressure is handled
 * by dropping at the ring, never by blocking the sampler and never by
 * abandoning a half-written packet at the socket. A gap is always
 * self-describing: the sample index jumps and WIRE_FLAG_GAP is set.
 *
 * Wire format is in wire.h.
 */

/* Frames held in the ring. 4800 at 9615 SPS is ~500 ms, which is what the
 * pre-trigger window wants and also how long the collector may stall before
 * anything is actually lost. 15 bytes each, so 72 KB. */
static const uint32_t ADC_STREAM_RING_FRAMES = 4800;

struct AdcStreamStats {
    bool     connected;
    uint32_t connects;      /* successful connections since boot           */
    uint32_t drops;         /* connections lost                            */
    uint64_t framesSampled; /* frames the sampler has written to the ring  */
    uint64_t framesSent;    /* frames handed to the socket                 */
    uint64_t framesLost;    /* frames overwritten before the pusher got them */
    uint32_t packets;
    uint32_t session;       /* this boot's session id, as sent in the hello */

    /* Why the last connect attempt failed. Without these a collector that
     * never appears is indistinguishable from a pusher task that never
     * started, and both look like "connects: 0". */
    uint32_t attempts;      /* connect attempts made                        */
    uint8_t  lastFailStage; /* AdcStreamFail                                */
    int      lastErrno;
};

enum AdcStreamFail : uint8_t {
    ADCSTREAM_FAIL_NONE     = 0,
    ADCSTREAM_FAIL_RESOLVE  = 1,   /* getaddrinfo                           */
    ADCSTREAM_FAIL_SOCKET   = 2,   /* socket()                              */
    ADCSTREAM_FAIL_CONNECT  = 3,   /* connect() failed outright             */
    ADCSTREAM_FAIL_TIMEOUT  = 4,   /* select() expired - SYNs went nowhere  */
    ADCSTREAM_FAIL_REFUSED  = 5,   /* SO_ERROR set - RST, or no listener    */
    ADCSTREAM_FAIL_HELLO    = 6,   /* connected, handshake would not send   */
};

/* Human-readable form of AdcStreamFail. */
const char *adcStreamFailName(uint8_t stage);

/**
 * Start both tasks. Call after adcInit() and after the network is up.
 *
 * 'host' and 'port' are the collector; an empty host disables the stream (the
 * sampler still runs, so the ring stays warm for local triggers). Returns
 * false if wireSelfTest() fails or a task will not start.
 */
bool adcStreamBegin(const char *host, uint16_t port);

/* Snapshot of the counters, for the diagnostic page. */
AdcStreamStats adcStreamGetStats(void);

/* The handshake JSON, exactly as sent to the collector on connect. Exposed so
 * the board can be asked what it thinks its own configuration is without
 * standing up a collector. */
String adcStreamHelloJson(void);
