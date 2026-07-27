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
 * The board listens, the collector dials in and the board then pushes into
 * the accepted socket. TCP is bidirectional, so which end called connect() has
 * no bearing on throughput - but it decides which end has to be told where the
 * other one is, and that is the whole argument: the collector already finds
 * the board by browsing _easyota._tcp, just like flash.sh does.
 *
 * Backpressure is handled by dropping at the ring, never by blocking the
 * sampler and never by abandoning a half-written packet at the socket. A gap
 * is always self-describing: the sample index jumps and WIRE_FLAG_GAP is set.
 *
 * Wire format is in wire.h.
 */

/* Frames held in the ring. 4800 at 9615 SPS is ~500 ms, which is what the
 * pre-trigger window wants and also how long the collector may stall before
 * anything is actually lost. 15 bytes each, so 72 KB. */
static const uint32_t ADC_STREAM_RING_FRAMES = 4800;
static const uint16_t ADC_STREAM_PORT = 9000;

struct AdcStreamStats {
    bool     listening;     /* the accept socket is up                     */
    uint16_t port;          /* what it is listening on, 0 if disabled      */
    bool     connected;
    char     peer[16];      /* collector currently attached, "" if none    */
    uint32_t connects;      /* collectors accepted since boot              */
    uint32_t drops;         /* connections lost                            */
    uint64_t framesSampled; /* frames the sampler has written to the ring  */
    uint64_t framesSent;    /* frames handed to the socket                 */
    uint64_t framesLost;    /* frames overwritten before the pusher got them */
    uint32_t packets;
    uint32_t session;       /* this boot's session id, as sent in the hello */

    /* Why the listener or the last accepted collector fell over. 'listening'
     * is what makes a collector that never appears distinguishable from a
     * pusher task that never started - both otherwise look like
     * "connects: 0". */
    uint8_t  lastFailStage; /* AdcStreamFail                               */
    int      lastErrno;
};

enum AdcStreamFail : uint8_t {
    ADCSTREAM_FAIL_NONE   = 0,
    ADCSTREAM_FAIL_SOCKET = 1,   /* socket()                               */
    ADCSTREAM_FAIL_BIND   = 2,   /* bind() or listen() - port already held */
    ADCSTREAM_FAIL_ACCEPT = 3,   /* accept() failed on a healthy listener  */
    ADCSTREAM_FAIL_HELLO  = 4,   /* accepted, handshake would not send     */
};

/* Human-readable form of AdcStreamFail. */
const char *adcStreamFailName(uint8_t stage);

/**
 * Start both tasks. Call after adcInit() and after the network is up.
 *
 * 'port' is where the collector dials in; 0 disables the stream (the sampler
 * still runs, so the ring stays warm for local triggers). Returns false if
 * wireSelfTest() fails or a task will not start.
 */
bool adcStreamBegin(uint16_t port);

/* Snapshot of the counters, for the diagnostic page. */
AdcStreamStats adcStreamGetStats(void);

/* The handshake JSON, exactly as sent to the collector on connect. Exposed so
 * the board can be asked what it thinks its own configuration is without
 * standing up a collector. */
String adcStreamHelloJson(void);
