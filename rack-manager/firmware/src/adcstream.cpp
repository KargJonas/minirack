#include "adcstream.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>

#include "adc.h"
#include "wire.h"

/* ---------------------------------------------------------------------------
 * Raw ADC stream: sampler task, ring, and the TCP server that drains it.
 *
 * The split of work follows system-design/rack-manager.md section 1: the
 * sampler owns core 1 and must never block, the network stack stays on core 0.
 * Everything between them goes through one bounded ring, and the only
 * backpressure mechanism is dropping its oldest frames.
 *
 * The collector dials in (see the header for why) and the board pushes into
 * the accepted socket. Everything below the accept - packet building, the
 * ring, the drop rules - is untouched by that: the socket is the same socket.
 * ------------------------------------------------------------------------- */

static const uint32_t SEND_TIMEOUT_S = 2;

/* Retry backoff for a listener that will not come up, doubling. Not a
 * reconnect backoff: waiting for a collector costs nothing and blocks in
 * accept() rather than spinning. */
static const uint32_t BACKOFF_MIN_MS = 1000;
static const uint32_t BACKOFF_MAX_MS = 30000;

/* How often, in packets sent, the listener is checked for a collector trying
 * to take over. ~10 ms per packet, so ~0.5 s of stale stream at worst. */
static const uint32_t TAKEOVER_CHECK_PACKETS = 50;

/* TCP keepalive: a collector that dies without sending FIN would otherwise
 * absorb writes into lwIP's retransmit timer for minutes. */
static const int KEEPALIVE_IDLE_S  = 5;
static const int KEEPALIVE_INTVL_S = 2;
static const int KEEPALIVE_COUNT   = 3;

static uint16_t s_port    = 0;
static uint32_t s_session = 0;

static bool     s_listening = false;
static char     s_peer[16]  = {0};
static uint8_t  s_failStage = ADCSTREAM_FAIL_NONE;
static int      s_failErrno = 0;

/* --- ring ---------------------------------------------------------------- */

static AdcFrame s_ring[ADC_STREAM_RING_FRAMES];

/* Total frames ever written. The sampler owns it; the pusher only reads it.
 * 64-bit, so it does not wrap: at 9615 SPS a u32 would turn over every 5.2
 * days, which is well inside a normal uptime and would put a discontinuity in
 * the one field the whole timebase rests on. */
static volatile uint64_t s_writeIdx = 0;
static uint32_t          s_writePos = 0;   /* sampler only, avoids a 64-bit % */

/* xtensa has no atomic 64-bit load, so the pusher cannot read s_writeIdx
 * without risking a torn value. Held for two instructions at 9615 Hz. */
static portMUX_TYPE s_idxMux = portMUX_INITIALIZER_UNLOCKED;

static uint64_t s_framesLost = 0;

static inline uint64_t ringWriteIdx(void)
{
    portENTER_CRITICAL(&s_idxMux);
    uint64_t w = s_writeIdx;
    portEXIT_CRITICAL(&s_idxMux);
    return w;
}

static inline void ringPush(const AdcSample s[ADC_COUNT])
{
    AdcFrame *f = &s_ring[s_writePos];

    i24le_set(&f->ch[0], s[ADC1].ch[ADC_CH0]);
    i24le_set(&f->ch[1], s[ADC1].ch[ADC_CH1]);
    i24le_set(&f->ch[2], s[ADC2].ch[ADC_CH0]);
    i24le_set(&f->ch[3], s[ADC2].ch[ADC_CH1]);
    i24le_set(&f->ch[4], s[ADC3].ch[ADC_CH0]);

    if (++s_writePos == ADC_STREAM_RING_FRAMES) s_writePos = 0;

    /* Publish only after the frame is fully written, so the pusher never sees
     * an index that points at a half-built frame. */
    portENTER_CRITICAL(&s_idxMux);
    s_writeIdx++;
    portEXIT_CRITICAL(&s_idxMux);
}

/**
 * Copy exactly 'want' frames out of the ring, or none.
 *
 * Returns the number copied (0 or 'want'), sets *startIdx to the sample index
 * of the first frame copied, and sets *gap if frames were overwritten before
 * they could be read. *gap is sticky - the caller clears it once it has been
 * reported on a packet.
 */
static uint16_t ringRead(uint64_t *readIdx, uint8_t *out, uint16_t want,
                         uint64_t *startIdx, bool *gap)
{
    uint64_t w = ringWriteIdx();

    /* Lapped: the pusher's cursor points at frames that no longer exist. Jump
     * to the oldest that survives. The jump in first_sample_idx is what tells
     * the collector how much went missing. */
    if (w - *readIdx > ADC_STREAM_RING_FRAMES) {
        s_framesLost += (w - *readIdx) - ADC_STREAM_RING_FRAMES;
        *readIdx = w - ADC_STREAM_RING_FRAMES;
        *gap = true;
    }

    if (w - *readIdx < want) return 0;   /* not a full packet yet */

    *startIdx = *readIdx;

    uint32_t pos   = (uint32_t)(*readIdx % ADC_STREAM_RING_FRAMES);
    uint32_t first = ADC_STREAM_RING_FRAMES - pos;
    if (first > want) first = want;

    memcpy(out, &s_ring[pos], (size_t)first * sizeof(AdcFrame));
    if (first < want)
        memcpy(out + (size_t)first * sizeof(AdcFrame), &s_ring[0],
               (size_t)(want - first) * sizeof(AdcFrame));

    /* The sampler may have lapped us mid-copy, in which case part of what we
     * just read is torn. Checking afterwards is far cheaper than locking the
     * sampler out for the length of a 1440-byte memcpy. */
    uint64_t w2 = ringWriteIdx();
    if (w2 - *readIdx > ADC_STREAM_RING_FRAMES) {
        s_framesLost += (w2 - *readIdx) - ADC_STREAM_RING_FRAMES;
        *readIdx = w2 - ADC_STREAM_RING_FRAMES;
        *gap = true;
        return 0;
    }

    *readIdx += want;
    return want;
}

/* --- sampler ------------------------------------------------------------- */

static TaskHandle_t s_samplerTask = nullptr;

static void IRAM_ATTR drdyIsr(void)
{
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_samplerTask, &woken);
    if (woken) portYIELD_FROM_ISR();
}

static void samplerTask(void *)
{
    AdcSample s[ADC_COUNT];

    for (;;) {
        /* Clear-on-take plus a drain loop rather than a counting take: DRDY is
         * only the hint, adcDataReady() is the truth, so coalesced or missed
         * notifications cost nothing. The timeout means a dead DRDY shows up
         * as a stalled sample index instead of a hung task. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));

        while (adcDataReady()) {
            if (!adcReadSampleAll(s)) break;
            ringPush(s);
        }
    }
}

/* --- handshake ----------------------------------------------------------- */

/**
 * Per-channel zero calibration, in ADC counts, subtracted by the collector and
 * not here: the stream stays a faithful record of what the chip reported, so a
 * bad calibration can be corrected after the fact instead of being baked in.
 *
 * Global chop cancels the ADC's own offset but not the front end's (adc.h), so
 * this is where a zero-cal routine lands. There is no such routine yet and
 * these are all zero.
 */
static float s_zeroCal[ADC_COUNT][ADC_CHANNELS] = {{0}};

/* snprintf returns the length it *would* have written, so accumulating it
 * directly lets n run past cap and the next call gets an underflowed size_t
 * for its bound. Clamp on every append instead. */
static void appendf(char *buf, size_t cap, size_t *n, const char *fmt, ...)
{
    if (*n >= cap) return;

    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + *n, cap - *n, fmt, ap);
    va_end(ap);

    if (w < 0) return;
    *n = ((size_t)w >= cap - *n) ? cap - 1 : *n + (size_t)w;
}

/**
 * The handshake. It reports how the hardware is configured and nothing about
 * what is connected to it: no channel names, no units, no scale factors. The
 * collector holds that map, because it is a wiring fact rather than a firmware
 * one, and adcVolts() already draws the same line ("front-end scaling is the
 * caller's business").
 *
 * The sample rate is not sent either - it follows from clkin_hz, osr, chop and
 * gc_delay as tGC_DLY + 3 x OSR x tMOD, and sending a second copy of a derived
 * value is how the two drift apart.
 */
static size_t buildHello(char *buf, size_t cap)
{
    size_t n = 0;

    appendf(buf, cap, &n,
            "{\"stream\":\"adc\",\"session\":%u,"
            "\"format\":{\"encoding\":\"int24_le\",\"frame_stride\":%u,"
            "\"channels_per_frame\":%u,\"ring_frames\":%u,"
            "\"clkin_hz\":%u,\"vref_v\":%.4f},\"adcs\":[",
            (unsigned)s_session, (unsigned)sizeof(AdcFrame),
            (unsigned)WIRE_ADC_CHANNELS, (unsigned)ADC_STREAM_RING_FRAMES,
            (unsigned)adcClkinHz(), adcFsrVolts());

    for (uint8_t a = 0; a < ADC_COUNT; a++) {
        AdcId id = (AdcId)a;

        appendf(buf, cap, &n,
                "%s{\"adc\":%u,\"online\":%s,\"osr\":%u,\"chop\":%s,"
                "\"gc_delay\":%u,\"channels\":[",
                a ? "," : "", (unsigned)(a + 1),
                adcOnline(id) ? "true" : "false",
                (unsigned)adcOsrRatio(adcGetOsr(id)),
                adcGetChop(id) ? "true" : "false",
                (unsigned)adcGetChopDelay(id));

        for (uint8_t c = 0; c < ADC_CHANNELS; c++)
            appendf(buf, cap, &n,
                    "%s{\"channel\":%u,\"gain\":%u,\"offset\":%.3f}",
                    c ? "," : "", (unsigned)c,
                    (unsigned)adcGainMultiplier(adcGetGain(id, (AdcChannel)c)),
                    s_zeroCal[a][c]);

        appendf(buf, cap, &n, "]}");
    }

    appendf(buf, cap, &n, "]}");
    return n;
}

String adcStreamHelloJson(void)
{
    char buf[1024];
    buildHello(buf, sizeof(buf));
    return String(buf);
}

/* --- socket -------------------------------------------------------------- */

static void applySocketOptions(int fd)
{
    int one = 1;

    /* Packets are 1456 bytes against a 1460 MSS, so every write is just under
     * a full segment and Nagle would hold each one waiting for an ACK. Against
     * the collector's delayed ACK that turns a 10 ms cadence into 40-200 ms of
     * stutter. */
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    /* Bounds how long a stalled collector can block the pusher. Longer than
     * this and the ring has lapped anyway, so the connection is worth dropping
     * rather than waiting on. */
    struct timeval tv = {};
    tv.tv_sec = SEND_TIMEOUT_S;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    int idle = KEEPALIVE_IDLE_S, intvl = KEEPALIVE_INTVL_S, cnt = KEEPALIVE_COUNT;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));

    /* No SO_SNDBUF here on purpose: lwIP's TCP send buffer is the compile-time
     * CONFIG_LWIP_TCP_SND_BUF_DEFAULT (5744 bytes, ~40 ms at this rate) and is
     * not settable per socket, and the Arduino core ships prebuilt. The ring is
     * therefore the real buffer, which is what it was sized for. */
}

/**
 * The listening socket. Non-blocking, because the pusher has to be able to ask
 * "has anyone else turned up?" mid-stream without giving up the collector it
 * already has - see acceptCollector().
 */
static int openListener(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        s_failStage = ADCSTREAM_FAIL_SOCKET;
        s_failErrno = errno;
        return -1;
    }

    /* A collector that vanished leaves the old connection in TIME_WAIT holding
     * the port; without this a reboot or a takeover would fail to bind for two
     * minutes and look exactly like a dead board. */
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(s_port);

    /* Backlog of 1: there is meant to be one collector, and a second one is
     * handled by taking over rather than by queueing. */
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, 1) != 0) {
        s_failStage = ADCSTREAM_FAIL_BIND;
        s_failErrno = errno;
        close(fd);
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    s_failStage = ADCSTREAM_FAIL_NONE;
    s_failErrno = 0;
    return fd;
}

/* acceptCollector() when there is no collector to take. Distinguished because
 * "nobody is waiting" is the normal state and "the listener is broken" needs a
 * rebind - and a broken one is not merely rare: select() returns immediately on
 * a bad descriptor, so treating the two alike spins this task at full speed. */
static const int ACCEPT_NOBODY = -1;
static const int ACCEPT_BROKEN = -2;

/**
 * Take the next collector off the listener.
 *
 * 'wait' blocks in select() for up to a second, which is how the pusher idles
 * when it has no collector at all. Mid-stream the same call is made with
 * wait=false, so a fresh collector is picked up without stalling the packets
 * going to the current one.
 *
 * The newcomer always wins. The collector only redials after its own read
 * failed, so it is the authority on whether the old socket is dead - the same
 * argument the server used to make in the other direction, now that the roles
 * have swapped. Keepalive would notice the corpse eventually (11 s), but a
 * collector that has already restarted should not have to wait for that.
 */
static int acceptCollector(int lfd, bool wait)
{
    if (wait) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(lfd, &rd);
        struct timeval tv = {};
        tv.tv_sec = 1;

        int rc = select(lfd + 1, &rd, nullptr, nullptr, &tv);
        if (rc == 0) return ACCEPT_NOBODY;
        if (rc < 0) {
            s_failStage = ADCSTREAM_FAIL_ACCEPT;
            s_failErrno = errno;
            return ACCEPT_BROKEN;
        }
    }

    struct sockaddr_in peer = {};
    socklen_t          len  = sizeof(peer);

    int fd = accept(lfd, (struct sockaddr *)&peer, &len);
    if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return ACCEPT_NOBODY;
        s_failStage = ADCSTREAM_FAIL_ACCEPT;
        s_failErrno = errno;
        return ACCEPT_BROKEN;
    }

    /* Inherited O_NONBLOCK would turn every send into a partial write. */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    inet_ntoa_r(peer.sin_addr, s_peer, sizeof(s_peer));
    applySocketOptions(fd);
    s_failStage = ADCSTREAM_FAIL_NONE;
    s_failErrno = 0;
    return fd;
}

/**
 * A packet is never abandoned half-written: a partial send is finished before
 * anything new is built, because dropping at the socket would desynchronise
 * the stream. All dropping happens at the ring instead, where it is
 * self-describing.
 */
static bool sendAll(int fd, const uint8_t *p, size_t n)
{
    while (n > 0) {
        int w = send(fd, p, n, 0);
        if (w <= 0) return false;   /* incl. SO_SNDTIMEO expiry: drop and redial */
        p += (size_t)w;
        n -= (size_t)w;
    }
    return true;
}

/* --- pusher -------------------------------------------------------------- */

/* Header and payload in one buffer, so a packet is one send() and cannot be
 * split across segments by us. */
static uint8_t s_tx[sizeof(AdcPacketHeader) + WIRE_MAX_FRAMES * sizeof(AdcFrame)];

static bool     s_connected = false;
static uint32_t s_connects  = 0;
static uint32_t s_drops     = 0;
static uint32_t s_packets   = 0;
static uint64_t s_framesSent = 0;

static bool sendHello(int fd)
{
    char   json[1024];
    size_t len = buildHello(json, sizeof(json));

    WireHelloHeader h;
    put_u32le(h.magic, WIRE_MAGIC_HELLO);
    put_u32le(h.json_len, (uint32_t)len);

    return sendAll(fd, (const uint8_t *)&h, sizeof(h)) &&
           sendAll(fd, (const uint8_t *)json, len);
}

static uint32_t crcErrorTotal(void)
{
    return adcCrcErrors(ADC1) + adcCrcErrors(ADC2) + adcCrcErrors(ADC3);
}

/* Which of the five frame slots carry data from a chip that is actually
 * usable. A chip that failed its configuration still occupies its bytes - the
 * stride is fixed - but its samples are not to be trusted (adc.h). */
static uint8_t channelMask(void)
{
    uint8_t m = 0;
    if (adcOnline(ADC1)) m |= 0x03;
    if (adcOnline(ADC2)) m |= 0x0C;
    if (adcOnline(ADC3)) m |= 0x10;
    return m;
}

static void pusherTask(void *)
{
    uint32_t backoffMs = BACKOFF_MIN_MS;
    int      lfd       = -1;
    int      fd        = -1;   /* carried over from a takeover, if any */

    for (;;) {
        /* The listener is the one thing worth retrying with backoff: if the
         * port cannot be bound, nothing else can happen. */
        if (lfd < 0) {
            lfd = openListener();
            if (lfd < 0) {
                s_listening = false;
                Serial.printf("[adcstream] listen on %u failed: %s errno %d\n",
                              (unsigned)s_port, adcStreamFailName(s_failStage),
                              s_failErrno);
                vTaskDelay(pdMS_TO_TICKS(backoffMs));
                backoffMs = backoffMs * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS
                                                           : backoffMs * 2;
                continue;
            }
            s_listening = true;
            backoffMs   = BACKOFF_MIN_MS;
            Serial.printf("[adcstream] listening on :%u\n", (unsigned)s_port);
        }

        /* Waiting for a collector is free - accept() blocks in select() rather
         * than spinning - so there is no backoff on this path at all. A broken
         * listener is the exception: it can only be recovered by rebinding,
         * and that does go through the backoff above. */
        if (fd < 0) {
            fd = acceptCollector(lfd, true);
            if (fd == ACCEPT_BROKEN) {
                close(lfd);
                lfd = -1;
                s_listening = false;
            }
            if (fd < 0) continue;
        }

        /* A collector that connects and then drops the handshake would
         * otherwise be re-accepted as fast as the LAN allows. */
        if (!sendHello(fd)) {
            s_failStage = ADCSTREAM_FAIL_HELLO;
            s_failErrno = errno;
            close(fd);
            fd = -1;
            s_peer[0] = '\0';
            vTaskDelay(pdMS_TO_TICKS(BACKOFF_MIN_MS));
            continue;
        }

        s_connects++;
        s_connected = true;

        /* Start from what is in the ring now. Anything older was either
         * already delivered or overwritten long ago, and replaying it would
         * only put stale samples at the head of a fresh session. */
        uint64_t readIdx  = ringWriteIdx();
        bool     gap      = false;
        uint32_t crcSeen  = crcErrorTotal();
        uint32_t sinceChk = 0;
        int      next     = -1;   /* a collector taking over */

        for (;;) {
            uint64_t startIdx = 0;
            uint16_t n = ringRead(&readIdx, s_tx + sizeof(AdcPacketHeader),
                                  WIRE_MAX_FRAMES, &startIdx, &gap);
            if (n == 0) {
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }

            uint8_t  flags = gap ? WIRE_FLAG_GAP : 0;
            uint32_t crcNow = crcErrorTotal();
            if (crcNow != crcSeen) {
                flags  |= WIRE_FLAG_CRC_ERR;
                crcSeen = crcNow;
            }

            AdcPacketHeader *h = (AdcPacketHeader *)s_tx;
            put_u32le(h->magic, WIRE_MAGIC_ADC);
            put_u64le(h->first_sample_idx, startIdx);
            put_u16le(h->n_frames, n);
            h->ch_mask = channelMask();
            h->flags   = flags;

            if (!sendAll(fd, s_tx, sizeof(AdcPacketHeader) + (size_t)n * sizeof(AdcFrame)))
                break;

            gap = false;
            s_packets++;
            s_framesSent += n;

            /* Checked between packets, never mid-packet: a half-written packet
             * handed off to a new collector would desynchronise it. */
            if (++sinceChk >= TAKEOVER_CHECK_PACKETS) {
                sinceChk = 0;
                next = acceptCollector(lfd, false);
                if (next >= 0) break;
            }
        }

        s_connected = false;
        s_drops++;
        close(fd);

        /* The newcomer starts the next iteration with its own hello and its
         * own ring cursor, exactly as a first connection would. Anything else
         * (nobody, or a listener that has gone bad) goes back to accepting,
         * which is where the rebind lives. */
        fd = next >= 0 ? next : -1;
        if (fd < 0) s_peer[0] = '\0';
    }
}

/* --- public -------------------------------------------------------------- */

bool adcStreamBegin(uint16_t port)
{
    if (!wireSelfTest()) {
        Serial.println("[adcstream] wire self-test FAILED - not starting");
        return false;
    }

    s_session = esp_random();

    /* The sampler runs whether or not there is a collector: the ring is also
     * the pre-trigger window for local event capture. */
    if (xTaskCreatePinnedToCore(samplerTask, "adcsampler", 3072, nullptr,
                                configMAX_PRIORITIES - 2, &s_samplerTask, 1) != pdPASS) {
        Serial.println("[adcstream] sampler task would not start");
        return false;
    }

    attachInterrupt(digitalPinToInterrupt(adcDrdyPin()), drdyIsr, FALLING);

    if (port == 0) {
        Serial.println("[adcstream] stream disabled; sampling only");
        return true;
    }

    s_port = port;

    /* Core 0 alongside the network stack, and below it in priority: a late
     * packet is a dropped packet, which the ring already accounts for, whereas
     * a starved lwIP is a dropped connection. */
    if (xTaskCreatePinnedToCore(pusherTask, "adcpusher", 5120, nullptr, 5,
                                nullptr, 0) != pdPASS) {
        Serial.println("[adcstream] pusher task would not start");
        return false;
    }

    Serial.printf("[adcstream] session %08X, collector dials in on :%u\n",
                  (unsigned)s_session, (unsigned)s_port);
    return true;
}

const char *adcStreamFailName(uint8_t stage)
{
    switch (stage) {
    case ADCSTREAM_FAIL_NONE:   return "none";
    case ADCSTREAM_FAIL_SOCKET: return "socket";
    case ADCSTREAM_FAIL_BIND:   return "bind";
    case ADCSTREAM_FAIL_ACCEPT: return "accept";
    case ADCSTREAM_FAIL_HELLO:  return "hello";
    default:                    return "?";
    }
}

AdcStreamStats adcStreamGetStats(void)
{
    AdcStreamStats st = {};
    st.listening     = s_listening;
    st.port          = s_port;
    st.connected     = s_connected;
    st.connects      = s_connects;
    st.drops         = s_drops;
    st.framesSampled = ringWriteIdx();
    st.framesSent    = s_framesSent;
    st.framesLost    = s_framesLost;
    st.packets       = s_packets;
    st.session       = s_session;
    st.lastFailStage = s_failStage;
    st.lastErrno     = s_failErrno;
    strncpy(st.peer, s_connected ? s_peer : "", sizeof(st.peer) - 1);
    return st;
}
