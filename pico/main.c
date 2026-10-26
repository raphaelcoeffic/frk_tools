/*
 * S.Port bootloader uploader and capture tool for the Raspberry Pi Pico.
 *
 * The host loads a .frk image over USB (it is kept in the Pico's flash),
 * then the Pico runs the
 * whole upload session described in PROTOCOL.md on its own, so the timing
 * on the S.Port line doesn't depend on USB latency. Every frame from the
 * device is checked against PROTOCOL.md; anything that differs is reported
 * as a warning instead of being silently tolerated. With tracing on, every
 * frame on the line is reported with a timestamp.
 *
 * It can also capture the raw S.Port line, e.g. the output of flashdump/.
 *
 * The S.Port line is single-wire half-duplex: SPORT_TX_PIN and SPORT_RX_PIN
 * are tied together. The TX pin only drives the line while a frame is being
 * sent. The signal is inverted in the GPIO block, so no external inverter is
 * needed. See README.md for the wiring and the USB command set.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/uart.h"

#define VERSION      "frkpico 1"

#ifndef SPORT_UART
#define SPORT_UART   0
#endif
#ifndef SPORT_TX_PIN
#define SPORT_TX_PIN 0
#endif
#ifndef SPORT_RX_PIN
#define SPORT_RX_PIN 1
#endif
#ifndef POWER_PIN
#define POWER_PIN    2
#endif

#define UART         UART_INSTANCE(SPORT_UART)
#define UART_IRQ     (SPORT_UART ? UART1_IRQ : UART0_IRQ)
#define BAUDRATE     57600

/*
 * The image doesn't fit in RAM next to everything else (an SR10+ image is
 * 244 KB), so it is kept in the upper half of the Pico's flash, read
 * through XIP.
 */
#ifndef IMAGE_MAX
#define IMAGE_MAX    (512 * 1024)
#endif
#define IMAGE_FLASH_OFFSET  (1024 * 1024)

_Static_assert(IMAGE_FLASH_OFFSET + IMAGE_MAX <= PICO_FLASH_SIZE_BYTES,
               "the image area doesn't fit in the flash");
#define BLOCK_SIZE   2048

/* Frame layout (PROTOCOL.md, "Frame format") */
#define START_BYTE   0x7E
#define STUFF_BYTE   0x7D
#define STUFF_XOR    0x20
#define ID_HOST      0xFF
#define ID_DEVICE    0x5E
#define FRAME_TYPE   0x50

enum {
    PRIM_REQ_POWERUP   = 0x00,
    PRIM_REQ_VERSION   = 0x01,
    PRIM_CMD_DOWNLOAD  = 0x03,
    PRIM_DATA_WORD     = 0x04,
    PRIM_DATA_EOF      = 0x05,
    PRIM_ACK_POWERUP   = 0x80,
    PRIM_ACK_VERSION   = 0x81,
    PRIM_REQ_DATA_ADDR = 0x82,
    PRIM_END_DOWNLOAD  = 0x83,
    PRIM_DATA_CRC_ERR  = 0x84,
};

/*
 * Timing. REQ_POWERUP is repeated every POWERUP_PERIOD_US, leaving the line
 * free for the ACK in between. After a data word, the next request comes
 * within WORD_TIMEOUT_US, except after the last word of a block, where the
 * bootloader erases and programs flash first.
 */
#define POWERUP_PERIOD_US  30000
#define REPLY_TIMEOUT_US   200000
#define WORD_TIMEOUT_US    50000
#define BLOCK_TIMEOUT_US   500000
#define FRAME_GRACE_US     5000
#define ECHO_TIMEOUT_US    500
#define MAX_RETRIES        10
#define VERSION_TRIES      5

/* UART data register error bits */
#define DR_FE        (1u << 8)
#define DR_PE        (1u << 9)
#define DR_BE        (1u << 10)
#define DR_OE        (1u << 11)
#define DR_ERRORS    (DR_FE | DR_PE | DR_BE | DR_OE)

static const uint8_t *const image = (const uint8_t *)(XIP_BASE + IMAGE_FLASH_OFFSET);
static uint32_t image_len;

/*
 * "data" writes go to a RAM copy of one flash sector, written out when
 * another sector is addressed or the image is used.
 */
#define IMAGE_SECTORS  (IMAGE_MAX / FLASH_SECTOR_SIZE)

static uint8_t  sector_buf[FLASH_SECTOR_SIZE];
static int32_t  sector_cur = -1;                /* sector in sector_buf */
static uint32_t sector_written[(IMAGE_SECTORS + 31) / 32];

static bool sector_is_written(uint32_t sector)
{
    return sector_written[sector / 32] & (1u << (sector % 32));
}

static void sector_flush(void)
{
    uint32_t addr, ints;

    if (sector_cur < 0)
        return;

    addr = IMAGE_FLASH_OFFSET + (uint32_t)sector_cur * FLASH_SECTOR_SIZE;
    ints = save_and_disable_interrupts();
    flash_range_erase(addr, FLASH_SECTOR_SIZE);
    flash_range_program(addr, sector_buf, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);

    sector_written[sector_cur / 32] |= 1u << (sector_cur % 32);
    sector_cur = -1;
}

static void sector_select(uint32_t sector)
{
    if ((int32_t)sector == sector_cur)
        return;

    sector_flush();
    if (sector_is_written(sector))
        memcpy(sector_buf, image + sector * FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE);
    else
        memset(sector_buf, 0xFF, FLASH_SECTOR_SIZE);
    sector_cur = sector;
}

/* writes out the pending sector, and 0xFF where "data" never wrote */
static void image_commit(void)
{
    sector_flush();
    for (uint32_t s = 0; s * FLASH_SECTOR_SIZE < image_len; s++) {
        if (!sector_is_written(s)) {
            sector_select(s);
            sector_flush();
        }
    }
}

static uint32_t image_word(uint32_t off)
{
    return image[off] | image[off + 1] << 8 | image[off + 2] << 16 | (uint32_t)image[off + 3] << 24;
}

static bool invert = true;
static bool trace;

/*
 * Receive ring buffer, filled from the UART interrupt. The UART FIFO is
 * disabled so that every byte gets an accurate timestamp.
 */
#define RX_SIZE 2048

static volatile uint16_t rx_data[RX_SIZE];
static volatile uint32_t rx_time[RX_SIZE];
static volatile uint32_t rx_head, rx_tail, rx_dropped;

static void uart_irq_handler(void)
{
    while (uart_is_readable(UART)) {
        uint32_t dr   = uart_get_hw(UART)->dr;
        uint32_t next = (rx_head + 1) % RX_SIZE;

        if (next == rx_tail) {
            rx_dropped++;
            continue;
        }
        rx_data[rx_head] = dr & (0xFF | DR_ERRORS);
        rx_time[rx_head] = time_us_32();
        rx_head = next;
    }
}

static bool deadline_passed(uint32_t deadline)
{
    return (int32_t)(time_us_32() - deadline) >= 0;
}

static bool rx_get(uint16_t *v, uint32_t *t, uint32_t deadline)
{
    while (rx_head == rx_tail) {
        if (deadline_passed(deadline))
            return false;
        tight_loop_contents();
    }
    *v = rx_data[rx_tail];
    *t = rx_time[rx_tail];
    rx_tail = (rx_tail + 1) % RX_SIZE;
    return true;
}

static void rx_flush(void)
{
    rx_tail = rx_head;
}

/*
 * Line and power control
 */
static void line_set_polarity(bool inv)
{
    invert = inv;
    gpio_set_outover(SPORT_TX_PIN, inv ? GPIO_OVERRIDE_INVERT : GPIO_OVERRIDE_NORMAL);
    gpio_set_inover(SPORT_RX_PIN, inv ? GPIO_OVERRIDE_INVERT : GPIO_OVERRIDE_NORMAL);
    /* keep a floating line at the idle level */
    if (inv)
        gpio_pull_down(SPORT_RX_PIN);
    else
        gpio_pull_up(SPORT_RX_PIN);
}

static void line_init(void)
{
    uart_init(UART, BAUDRATE);
    uart_set_format(UART, 8, 1, UART_PARITY_NONE);
    uart_set_hw_flow(UART, false, false);
    uart_set_fifo_enabled(UART, false);

    gpio_set_function(SPORT_TX_PIN, UART_FUNCSEL_NUM(UART, SPORT_TX_PIN));
    gpio_set_function(SPORT_RX_PIN, UART_FUNCSEL_NUM(UART, SPORT_RX_PIN));
    /* gpio_set_function() clears the overrides: set them afterwards */
    gpio_set_oeover(SPORT_TX_PIN, GPIO_OVERRIDE_LOW);
    line_set_polarity(invert);

    irq_set_exclusive_handler(UART_IRQ, uart_irq_handler);
    irq_set_priority(UART_IRQ, PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_enabled(UART_IRQ, true);
    uart_set_irqs_enabled(UART, true, false);
}

static void power_set(bool on)
{
    gpio_put(POWER_PIN, on);
}

static void led_set(bool on)
{
#ifdef PICO_DEFAULT_LED_PIN
    gpio_put(PICO_DEFAULT_LED_PIN, on);
#else
    (void)on;
#endif
}

/*
 * Checksums
 */
static uint16_t crc16_xmodem(const uint8_t *p, int n)
{
    uint16_t crc = 0;

    while (n--) {
        crc ^= (uint16_t)*p++ << 8;
        for (int i = 0; i < 8; i++)
            crc = crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
}

static uint32_t crc32_ieee(const uint8_t *p, uint32_t n)
{
    uint32_t crc = 0xFFFFFFFFu;

    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    return ~crc;
}

/*
 * Session statistics and warnings
 */
static struct {
    uint32_t warnings;
    uint32_t retries;
    uint32_t repeats;
    uint32_t rx_errors;
    uint32_t max_word_us;
    uint32_t min_block_us;
    uint32_t max_block_us;
} stats;

static void warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void warn(const char *fmt, ...)
{
    va_list ap;

    stats.warnings++;
    printf("W ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/*
 * Trace queue. Frames are queued while the line is busy and printed once
 * our reply is out, so that tracing doesn't delay the replies.
 */
#define RAW_MAX 20

struct trace_entry {
    char     dir;           /* 'S' sent, 'R' received, 'A' aborted frame,
                               'J' bytes outside a frame, 'E' UART errors */
    uint32_t t;             /* end of the first byte (R) or of the last byte (S) */
    uint8_t  n;
    uint8_t  raw[RAW_MAX];
};

static struct trace_entry trace_queue[8];
static int trace_count;

static void trace_add(char dir, uint32_t t, const uint8_t *raw, int n)
{
    struct trace_entry *e;

    if (!trace || trace_count == count_of(trace_queue))
        return;
    e = &trace_queue[trace_count++];
    e->dir = dir;
    e->t   = t;
    e->n   = n;
    memcpy(e->raw, raw, n);
}

/* Bytes outside frames: consecutive ones are grouped */
static void trace_stray(char dir, uint32_t t, uint8_t b)
{
    struct trace_entry *e = &trace_queue[trace_count - 1];

    if (trace && trace_count && e->dir == dir && e->n < RAW_MAX)
        e->raw[e->n++] = b;
    else
        trace_add(dir, t, &b, 1);
}

static void trace_flush(void)
{
    for (int i = 0; i < trace_count; i++) {
        const struct trace_entry *e = &trace_queue[i];

        printf("T %lu %c ", (unsigned long)e->t, e->dir);
        for (int j = 0; j < e->n; j++)
            printf("%02X", e->raw[j]);
        printf("\n");
    }
    trace_count = 0;
}

/*
 * Frames
 */
struct frame {
    uint8_t  id;
    uint8_t  p[8];
    uint32_t t;             /* time the start byte was received */
};

static uint32_t frame_arg(const struct frame *f)
{
    return f->p[2] | f->p[3] << 8 | f->p[4] << 16 | (uint32_t)f->p[5] << 24;
}

static uint32_t tx_end;     /* time our last frame finished */

/* Discards unread bytes (traced as stray). Returns how many there were. */
static uint32_t rx_drain(void)
{
    uint32_t n = 0;
    uint16_t v;
    uint32_t t;

    while (rx_get(&v, &t, time_us_32())) {
        trace_stray(v & DR_ERRORS ? 'E' : 'J', t, v & 0xFF);
        n++;
    }
    return n;
}

static void frame_send(uint8_t cmd, uint32_t arg, uint8_t extra)
{
    uint8_t p[8] = {
        FRAME_TYPE, cmd,
        arg & 0xFF, (arg >> 8) & 0xFF, (arg >> 16) & 0xFF, arg >> 24,
        extra, 0
    };
    uint8_t  raw[RAW_MAX];
    int      n = 0;
    uint32_t pending;
    uint16_t v;
    uint32_t t;

    /* host -> device: low byte of the CRC */
    p[7] = crc16_xmodem(p, 7) & 0xFF;

    raw[n++] = START_BYTE;
    raw[n++] = ID_HOST;
    for (int i = 0; i < 8; i++) {
        if (p[i] == START_BYTE || p[i] == STUFF_BYTE) {
            raw[n++] = STUFF_BYTE;
            raw[n++] = p[i] ^ STUFF_XOR;
        } else {
            raw[n++] = p[i];
        }
    }

    pending = rx_drain();

    gpio_set_oeover(SPORT_TX_PIN, GPIO_OVERRIDE_NORMAL);
    uart_write_blocking(UART, raw, n);
    uart_tx_wait_blocking(UART);
    gpio_set_oeover(SPORT_TX_PIN, GPIO_OVERRIDE_LOW);
    tx_end = time_us_32();

    trace_add('S', tx_end, raw, n);

    /* The RX pin hears everything we send: check it came back unchanged */
    for (int i = 0; i < n; i++) {
        if (!rx_get(&v, &t, tx_end + ECHO_TIMEOUT_US)) {
            warn("echo: got %d of %d bytes back (RX not tied to TX?)", i, n);
            break;
        }
        if (v != raw[i]) {
            warn("echo: byte %d sent %02X, read %03X (collision?)", i, raw[i], v);
            rx_flush();
            break;
        }
    }

    trace_flush();
    if (pending)
        warn("discarded %lu unread byte(s) before sending", (unsigned long)pending);
}

/*
 * Check a device frame against PROTOCOL.md. Returns false if it can't be
 * used at all.
 */
static bool frame_check(const struct frame *f)
{
    uint16_t crc = crc16_xmodem(f->p, 7);

    if (f->id == ID_HOST) {
        warn("rx: frame with the host ID 0x%02X", f->id);
        return false;
    }
    if (f->p[0] != FRAME_TYPE) {
        warn("rx: frame type 0x%02X instead of 0x%02X", f->p[0], FRAME_TYPE);
        return false;
    }
    if (f->p[7] != crc >> 8) {
        if (f->p[7] != (crc & 0xFF)) {
            warn("rx: bad CRC %02X (expected high byte %02X), cmd 0x%02X",
                 f->p[7], crc >> 8, f->p[1]);
            return false;
        }
        warn("rx: CRC is the low byte %02X, not the high byte %02X, cmd 0x%02X",
             f->p[7], crc >> 8, f->p[1]);
    }
    if (f->id != ID_DEVICE)
        warn("rx: physical ID 0x%02X instead of 0x%02X", f->id, ID_DEVICE);
    if (f->p[6] != 0)
        warn("rx: byte 6 is 0x%02X, cmd 0x%02X", f->p[6], f->p[1]);
    if (f->p[1] != PRIM_ACK_VERSION && f->p[1] != PRIM_REQ_DATA_ADDR && frame_arg(f) != 0)
        warn("rx: non-zero argument 0x%08lX, cmd 0x%02X",
             (unsigned long)frame_arg(f), f->p[1]);
    return true;
}

/*
 * Wait for a valid device frame until the deadline. A frame that has
 * started when the deadline is reached gets a little more time.
 */
static bool frame_recv(struct frame *f, uint32_t deadline)
{
    uint8_t raw[RAW_MAX];
    int     n = 0, pos = 0;
    bool    esc = false, graced = false;

    for (;;) {
        uint16_t v;
        uint32_t t;
        uint8_t  b;

        if (!rx_get(&v, &t, deadline)) {
            if (n && !graced) {
                deadline = time_us_32() + FRAME_GRACE_US;
                graced = true;
                continue;
            }
            if (n)
                trace_add('A', f->t, raw, n);
            return false;
        }

        if (v & DR_ERRORS) {
            stats.rx_errors++;
            if (n)
                trace_add('A', f->t, raw, n);
            trace_stray('E', t, v & 0xFF);
            n = 0;
            if (v & DR_OE)
                warn("rx: UART overrun");
            continue;
        }

        b = v;
        if (b == START_BYTE) {
            /* a start byte in the middle of a frame aborts it */
            if (n)
                trace_add('A', f->t, raw, n);
            raw[0] = b;
            n      = 1;
            pos    = -1;
            esc    = false;
            f->t   = t;
            continue;
        }
        if (n == 0) {
            trace_stray('J', t, b);
            continue;
        }

        raw[n++] = b;
        if (pos < 0) {
            /* the physical ID byte is never stuffed */
            f->id = b;
            pos   = 0;
            continue;
        }
        if (esc) {
            b  ^= STUFF_XOR;
            esc = false;
        } else if (b == STUFF_BYTE) {
            esc = true;
            continue;
        }
        f->p[pos++] = b;

        if (pos == 8) {
            trace_add('R', f->t, raw, n);
            n = 0;
            if (frame_check(f))
                return true;
        }
    }
}

static bool cancel_requested(void)
{
    return getchar_timeout_us(0) != PICO_ERROR_TIMEOUT;
}

static void stats_reset(void)
{
    memset(&stats, 0, sizeof(stats));
    stats.min_block_us = UINT32_MAX;
}

static void stats_print(void)
{
    printf("I stats warnings=%lu retries=%lu repeats=%lu rx_errors=%lu"
           " max_word_us=%lu block_us=%lu..%lu\n",
           (unsigned long)stats.warnings, (unsigned long)stats.retries,
           (unsigned long)stats.repeats, (unsigned long)stats.rx_errors,
           (unsigned long)stats.max_word_us,
           (unsigned long)(stats.max_block_us ? stats.min_block_us : 0),
           (unsigned long)stats.max_block_us);
}

/*
 * Sends a request the bootloader has already answered: it should be ignored
 * (PROTOCOL.md, "Handshake").
 */
static void check_ignored(uint8_t cmd, const char *name)
{
    struct frame f;

    frame_send(cmd, 0, 0);
    if (frame_recv(&f, time_us_32() + REPLY_TIMEOUT_US)) {
        trace_flush();
        warn("%s sent again: got cmd 0x%02X back", name, f.p[1]);
    } else {
        trace_flush();
        printf("I %s sent again: no reply\n", name);
    }
}

/*
 * Entering the bootloader and the handshake (PROTOCOL.md, "Entering the
 * bootloader" and "Handshake"). With probe set, each request is sent once
 * more after its ACK, to check that it is ignored. Returns NULL on success,
 * or an error message.
 */
static const char *handshake(uint32_t boot_timeout_ms, uint32_t off_ms, bool probe,
                             bool *got_version)
{
    struct frame f;
    uint32_t start, deadline, attempts = 0;

    *got_version = false;

    /* left over from before, e.g. the receiver losing power */
    rx_drain();

    if (off_ms) {
        printf("I power off for %lu ms\n", (unsigned long)off_ms);
        power_set(false);
        sleep_ms(off_ms);
        power_set(true);
    }

    /* REQ_POWERUP until ACK_POWERUP */
    start    = time_us_32();
    deadline = start + boot_timeout_ms * 1000;
    printf("I waiting for the bootloader\n");
    for (;;) {
        if (deadline_passed(deadline))
            return "no ACK_POWERUP";
        if (cancel_requested())
            return "cancelled";

        frame_send(PRIM_REQ_POWERUP, 0, 0);
        attempts++;
        if (frame_recv(&f, time_us_32() + POWERUP_PERIOD_US)) {
            trace_flush();
            if (f.p[1] == PRIM_ACK_POWERUP)
                break;
            warn("got cmd 0x%02X instead of ACK_POWERUP", f.p[1]);
        }
        trace_flush();
    }
    printf("I ACK_POWERUP after %lu ms, %lu REQ_POWERUP sent\n",
           (unsigned long)((f.t - start) / 1000), (unsigned long)attempts);

    if (probe)
        check_ignored(PRIM_REQ_POWERUP, "REQ_POWERUP");

    /* REQ_VERSION. Repeating it is harmless (ignored once acked). */
    for (int i = 0; i < VERSION_TRIES && !*got_version; i++) {
        frame_send(PRIM_REQ_VERSION, 0, 0);
        deadline = time_us_32() + REPLY_TIMEOUT_US;
        while (frame_recv(&f, deadline)) {
            trace_flush();
            if (f.p[1] == PRIM_ACK_VERSION) {
                printf("I ACK_VERSION after %lu us: 0x%04X 0x%04X\n",
                       (unsigned long)(f.t - tx_end),
                       f.p[2] | f.p[3] << 8, f.p[4] | f.p[5] << 8);
                *got_version = true;
                break;
            }
            warn("got cmd 0x%02X instead of ACK_VERSION", f.p[1]);
        }
        trace_flush();
    }

    if (probe && *got_version)
        check_ignored(PRIM_REQ_VERSION, "REQ_VERSION");
    return NULL;
}

/*
 * Handshake only: nothing is written to the device, which is left waiting
 * for CMD_DOWNLOAD.
 */
static const char *probe(uint32_t boot_timeout_ms, uint32_t off_ms)
{
    const char *err;
    bool        got_version;

    stats_reset();
    err = handshake(boot_timeout_ms, off_ms, true, &got_version);
    if (err)
        return err;
    if (!got_version)
        return "no ACK_VERSION";
    printf("I the bootloader now waits for CMD_DOWNLOAD: power-cycle the receiver to leave it\n");
    stats_print();
    return NULL;
}

/*
 * Upload session (PROTOCOL.md, "Session"). Returns NULL on success, or an
 * error message.
 */
static const char *session(uint32_t boot_timeout_ms, uint32_t off_ms)
{
    struct frame f;
    const char  *err;
    uint32_t     start, last_off = 0, timeout;
    bool         got_version, started = false, eof_sent = false;
    int          retries = 0;

    stats_reset();

    if (!image_len)
        return "no image loaded";
    image_commit();
    if (image_len % BLOCK_SIZE)
        warn("image length %lu isn't a multiple of %d: the last %lu bytes won't be flashed",
             (unsigned long)image_len, BLOCK_SIZE, (unsigned long)(image_len % BLOCK_SIZE));

    err = handshake(boot_timeout_ms, off_ms, false, &got_version);
    if (err)
        return err;
    if (!got_version)
        warn("no ACK_VERSION, trying CMD_DOWNLOAD anyway");

    /*
     * Data transfer. On a timeout, the last frame is sent again. Until the
     * first request arrives, it alternates between DATA_WORD(0) and
     * CMD_DOWNLOAD: a second CMD_DOWNLOAD would abort the download if the
     * first one was received and only its REQ_DATA_ADDR(0) got lost, while
     * DATA_WORD is ignored by a bootloader still waiting for CMD_DOWNLOAD.
     */
    start   = time_us_32();
    timeout = REPLY_TIMEOUT_US;
    frame_send(PRIM_CMD_DOWNLOAD, 0, 0);

    for (;;) {
        uint32_t off, latency;

        if (!frame_recv(&f, time_us_32() + timeout)) {
            trace_flush();
            if (++retries > MAX_RETRIES)
                return started ? "device stopped requesting data" : "no REQ_DATA_ADDR after CMD_DOWNLOAD";
            stats.retries++;
            if (!started && (retries & 1)) {
                warn("timeout, sending DATA_WORD(0)");
                frame_send(PRIM_DATA_WORD, image_word(0), 0);
            } else if (!started) {
                warn("timeout, sending CMD_DOWNLOAD again");
                frame_send(PRIM_CMD_DOWNLOAD, 0, 0);
            } else if (eof_sent) {
                warn("timeout, sending DATA_EOF again");
                frame_send(PRIM_DATA_EOF, 0, 0);
            } else {
                warn("timeout, sending the word at %lu again", (unsigned long)last_off);
                frame_send(PRIM_DATA_WORD, image_word(last_off), last_off & 0xFF);
            }
            continue;
        }

        switch (f.p[1]) {
        case PRIM_REQ_DATA_ADDR:
            off     = frame_arg(&f);
            latency = f.t - tx_end;

            if (!started) {
                if (off != 0)
                    warn("first request is for offset %lu", (unsigned long)off);
                started = true;
            } else if (eof_sent) {
                warn("offset %lu requested again after DATA_EOF", (unsigned long)off);
            } else if (off == last_off) {
                stats.repeats++;
                warn("offset %lu requested again", (unsigned long)off);
            } else if (off != last_off + 4) {
                warn("offset %lu requested after %lu", (unsigned long)off, (unsigned long)last_off);
            } else if (off % BLOCK_SIZE == 0) {
                if (latency < stats.min_block_us)
                    stats.min_block_us = latency;
                if (latency > stats.max_block_us)
                    stats.max_block_us = latency;
            } else if (latency > stats.max_word_us) {
                stats.max_word_us = latency;
            }

            if ((off & 3) || off > image_len) {
                trace_flush();
                return "device requested an offset outside the image";
            }

            retries = 0;
            if (off == image_len) {
                frame_send(PRIM_DATA_EOF, 0, 0);
                eof_sent = true;
                timeout  = BLOCK_TIMEOUT_US;
            } else {
                frame_send(PRIM_DATA_WORD, image_word(off), off & 0xFF);
                last_off = off;
                timeout  = (off + 4) % BLOCK_SIZE ? WORD_TIMEOUT_US : BLOCK_TIMEOUT_US;
            }
            if (off % BLOCK_SIZE == 0)
                printf("P %lu %lu %lu\n", (unsigned long)off,
                       (unsigned long)image_len, (unsigned long)latency);
            break;

        case PRIM_END_DOWNLOAD:
            trace_flush();
            if (!eof_sent)
                return "END_DOWNLOAD before DATA_EOF";
            printf("I END_DOWNLOAD after %lu us, transfer took %lu ms\n",
                   (unsigned long)(f.t - tx_end), (unsigned long)((f.t - start) / 1000));
            stats_print();
            return NULL;

        case PRIM_DATA_CRC_ERR:
            trace_flush();
            return "device sent DATA_CRC_ERR";

        default:
            trace_flush();
            warn("unexpected cmd 0x%02X during the transfer", f.p[1]);
            break;
        }
    }
}

/*
 * Raw capture: everything received is sent to the host as hex lines until
 * the host sends anything.
 */
static void capture(void)
{
    uint8_t  buf[32];
    int      n = 0;
    uint32_t last = time_us_32(), bytes = 0, fe = 0, be = 0, oe = 0;
    uint32_t dropped = rx_dropped;

    rx_flush();
    for (;;) {
        uint16_t v;
        uint32_t t;
        bool     got = rx_get(&v, &t, time_us_32() + 1000);

        if (got) {
            if (v & DR_FE)
                fe++;
            if (v & DR_BE)
                be++;
            if (v & DR_OE)
                oe++;
            if (!(v & (DR_FE | DR_BE | DR_PE))) {
                buf[n++] = v;
                bytes++;
            }
            last = t;
        }
        if (n == sizeof(buf) || (n && !got && time_us_32() - last > 5000)) {
            printf("D ");
            for (int i = 0; i < n; i++)
                printf("%02X", buf[i]);
            printf("\n");
            n = 0;
        }
        if (cancel_requested())
            break;
    }
    if (n) {
        printf("D ");
        for (int i = 0; i < n; i++)
            printf("%02X", buf[i]);
        printf("\n");
    }
    printf("OK bytes=%lu fe=%lu be=%lu oe=%lu dropped=%lu\n",
           (unsigned long)bytes, (unsigned long)fe, (unsigned long)be,
           (unsigned long)oe, (unsigned long)(rx_dropped - dropped));
}

/*
 * USB command interface
 */
static int read_line(char *buf, int size)
{
    int n = 0;

    for (;;) {
        int c = getchar_timeout_us(100000);

        if (c == PICO_ERROR_TIMEOUT)
            continue;
        if (c == '\r' || c == '\n') {
            if (n == 0)
                continue;
            if (n >= size)
                return -1;
            buf[n] = 0;
            return n;
        }
        if (n < size - 1)
            buf[n] = c;
        if (n < size)
            n++;
    }
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* data <offset> <hex> */
static const char *cmd_data(char *args)
{
    char    *hex;
    uint32_t off = strtoul(args, &hex, 0);

    while (*hex == ' ')
        hex++;
    for (; hex[0] && hex[1]; hex += 2, off++) {
        int hi = hexval(hex[0]), lo = hexval(hex[1]);

        if (hi < 0 || lo < 0)
            return "bad hex";
        if (off >= image_len)
            return "data beyond the image length";
        sector_select(off / FLASH_SECTOR_SIZE);
        sector_buf[off % FLASH_SECTOR_SIZE] = hi << 4 | lo;
    }
    return *hex ? "odd number of hex digits" : NULL;
}

int main(void)
{
    static char line[640];

    stdio_init_all();

    gpio_init(POWER_PIN);
    gpio_put(POWER_PIN, 0);
    gpio_set_dir(POWER_PIN, GPIO_OUT);
#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#endif

    line_init();

    for (;;) {
        char       *args;
        const char *err = NULL;

        if (read_line(line, sizeof(line)) < 0) {
            printf("ERR line too long\n");
            continue;
        }
        args = strchr(line, ' ');
        if (args)
            *args++ = 0;
        else
            args = line + strlen(line);

        if (!strcmp(line, "ping")) {
            printf("OK " VERSION "\n");
            continue;
        } else if (!strcmp(line, "load")) {
            uint32_t len = strtoul(args, NULL, 0);

            if (len == 0 || len > IMAGE_MAX)
                err = "bad length";
            else if (len % 4)
                err = "length must be a multiple of 4";
            else {
                image_len  = len;
                sector_cur = -1;
                memset(sector_written, 0, sizeof(sector_written));
            }
        } else if (!strcmp(line, "data")) {
            err = cmd_data(args);
        } else if (!strcmp(line, "crc")) {
            image_commit();
            printf("OK %08lX %lu\n", (unsigned long)crc32_ieee(image, image_len),
                   (unsigned long)image_len);
            continue;
        } else if (!strcmp(line, "invert")) {
            line_set_polarity(atoi(args) != 0);
        } else if (!strcmp(line, "trace")) {
            trace = atoi(args) != 0;
        } else if (!strcmp(line, "power")) {
            power_set(atoi(args) != 0);
        } else if (!strcmp(line, "flash")) {
            char    *end;
            uint32_t boot_timeout_ms = strtoul(args, &end, 0);
            uint32_t off_ms          = strtoul(end, NULL, 0);

            led_set(true);
            err = session(boot_timeout_ms ? boot_timeout_ms : 60000, off_ms);
            led_set(false);
        } else if (!strcmp(line, "probe")) {
            char    *end;
            uint32_t boot_timeout_ms = strtoul(args, &end, 0);
            uint32_t off_ms          = strtoul(end, NULL, 0);

            led_set(true);
            err = probe(boot_timeout_ms ? boot_timeout_ms : 60000, off_ms);
            led_set(false);
        } else if (!strcmp(line, "capture")) {
            led_set(true);
            capture();
            led_set(false);
            continue;
        } else {
            err = "unknown command";
        }

        if (err)
            printf("ERR %s\n", err);
        else
            printf("OK\n");
    }
}
