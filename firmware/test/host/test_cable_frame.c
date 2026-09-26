// Host tests for cable_frame.{c,h} — the exact framing the Harness daemon
// speaks. Runs the shared vector file test/vectors/cable_frame.txt (the same
// file the daemon's TypeScript half asserts against), plus the CRC check
// value and the classic resync cases by hand.
//
// Built by run_tests.sh with gcc -std=c11 -Wall -Wextra -Werror.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cable_frame.h"

static int s_failures;
static int s_checks;

#define CHECK(cond, ...)                                            \
    do {                                                            \
        s_checks++;                                                 \
        if (!(cond)) {                                              \
            s_failures++;                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);             \
            printf(__VA_ARGS__);                                    \
            printf("\n");                                           \
        }                                                           \
    } while (0)

// ── decoded-frame capture ───────────────────────────────────────────────────

typedef struct {
    uint8_t type;
    uint8_t payload[CABLE_MAX_PAYLOAD];
    size_t  len;
} captured_t;

typedef struct {
    captured_t frames[16];
    int        count;
} capture_t;

static void capture_cb(uint8_t version, uint8_t type,
                       const uint8_t *payload, size_t payload_len, void *ctx)
{
    capture_t *cap = ctx;
    CHECK(version == CABLE_FRAME_VERSION, "frame version %u != %u", version, CABLE_FRAME_VERSION);
    if (cap->count >= (int)(sizeof(cap->frames) / sizeof(cap->frames[0]))) {
        CHECK(0, "capture overflow");
        return;
    }
    captured_t *f = &cap->frames[cap->count++];
    f->type = type;
    f->len = payload_len;
    if (payload_len > 0) memcpy(f->payload, payload, payload_len);
}

// ── hex helpers for the vector file ─────────────────────────────────────────

static int hex_val(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Returns the byte count, or -1 on malformed hex.
static int unhex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (s[0] && s[1] && hex_val(s[0]) >= 0 && hex_val(s[1]) >= 0) {
        if (n >= cap) return -1;
        out[n++] = (uint8_t)((hex_val(s[0]) << 4) | hex_val(s[1]));
        s += 2;
    }
    if (*s && *s != '\t' && *s != '\n' && *s != '\r' && *s != ' ' && *s != ',') return -1;
    return (int)n;
}

static void to_hex(const uint8_t *in, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) sprintf(out + i * 2, "%02x", in[i]);
    out[n * 2] = '\0';
}

// ── fixed tests ─────────────────────────────────────────────────────────────

static void test_crc_check_value(void)
{
    const uint16_t crc = cable_crc16((const uint8_t *)"123456789", 9);
    CHECK(crc == 0x29B1, "CRC-16/CCITT-FALSE \"123456789\" = 0x%04X, want 0x29B1", crc);
}

static void test_encode_roundtrip(void)
{
    const char *payload = "{\"t\":\"hello\",\"proto\":3}";
    uint8_t frame[CABLE_MAX_FRAME];
    int len = cable_frame_encode(CABLE_TYPE_JSON, (const uint8_t *)payload, strlen(payload),
                                 frame, sizeof(frame));
    CHECK(len == (int)(CABLE_HEADER_BYTES + strlen(payload) + CABLE_CRC_BYTES),
          "encode len %d", len);
    CHECK(frame[0] == CABLE_MAGIC_0 && frame[1] == CABLE_MAGIC_1, "magic bytes");
    CHECK(frame[2] == CABLE_FRAME_VERSION, "version byte");
    CHECK(frame[3] == CABLE_TYPE_JSON, "type byte");

    cable_decoder_t d;
    cable_decoder_init(&d);
    capture_t cap = {0};
    cable_decoder_feed(&d, frame, (size_t)len, capture_cb, &cap);
    CHECK(cap.count == 1, "roundtrip decoded %d frames", cap.count);
    CHECK(cap.frames[0].type == CABLE_TYPE_JSON, "roundtrip type");
    CHECK(cap.frames[0].len == strlen(payload) &&
          memcmp(cap.frames[0].payload, payload, cap.frames[0].len) == 0,
          "roundtrip payload");
    CHECK(d.discarded_bytes == 0 && d.corrupt_frames == 0, "roundtrip counters");
}

static void test_resync_garbage_prefix(void)
{
    const char *payload = "{\"t\":\"pong\"}";
    uint8_t frame[CABLE_MAX_FRAME];
    int len = cable_frame_encode(CABLE_TYPE_JSON, (const uint8_t *)payload, strlen(payload),
                                 frame, sizeof(frame));
    const char *boot = "ESP-ROM:esp32c3-api-20240307\r\nBuild:Mar 27 2021\r\n";

    cable_decoder_t d;
    cable_decoder_init(&d);
    capture_t cap = {0};
    cable_decoder_feed(&d, (const uint8_t *)boot, strlen(boot), capture_cb, &cap);
    cable_decoder_feed(&d, frame, (size_t)len, capture_cb, &cap);
    CHECK(cap.count == 1, "resync decoded %d frames", cap.count);
    CHECK(d.discarded_bytes == strlen(boot), "resync discarded %u", d.discarded_bytes);
    CHECK(d.corrupt_frames == 0, "resync corrupt %u", d.corrupt_frames);
}

static void test_truncated_frame(void)
{
    const char *payload = "{\"t\":\"hello\"}";
    uint8_t frame[CABLE_MAX_FRAME];
    int len = cable_frame_encode(CABLE_TYPE_JSON, (const uint8_t *)payload, strlen(payload),
                                 frame, sizeof(frame));

    cable_decoder_t d;
    cable_decoder_init(&d);
    capture_t cap = {0};
    // First half: nothing may be emitted, and nothing may be discarded.
    cable_decoder_feed(&d, frame, (size_t)len / 2, capture_cb, &cap);
    CHECK(cap.count == 0, "truncated emitted %d frames", cap.count);
    CHECK(d.discarded_bytes == 0, "truncated discarded %u", d.discarded_bytes);
    // The rest completes it.
    cable_decoder_feed(&d, frame + len / 2, (size_t)(len - len / 2), capture_cb, &cap);
    CHECK(cap.count == 1, "completed frame count %d", cap.count);
    CHECK(cap.frames[0].len == strlen(payload), "completed payload len");
    // Byte at a time from a fresh decoder must behave identically.
    cable_decoder_init(&d);
    memset(&cap, 0, sizeof(cap));
    for (int i = 0; i < len; i++) cable_decoder_feed(&d, frame + i, 1, capture_cb, &cap);
    CHECK(cap.count == 1, "byte-at-a-time frame count %d", cap.count);
}

static void test_oversize_payload_rejected(void)
{
    static uint8_t big[CABLE_MAX_PAYLOAD + 1];
    memset(big, 0xAB, sizeof(big));
    uint8_t frame[CABLE_MAX_FRAME + 8];
    int len = cable_frame_encode(CABLE_TYPE_FW, big, sizeof(big), frame, sizeof(frame));
    CHECK(len == -1, "oversize encode returned %d, want -1 (never truncated)", len);
    // And exactly at the cap it must fit.
    len = cable_frame_encode(CABLE_TYPE_FW, big, CABLE_MAX_PAYLOAD, frame, sizeof(frame));
    CHECK(len == CABLE_MAX_FRAME, "max-payload encode returned %d", len);
    // A too-small output buffer must also fail rather than truncate.
    len = cable_frame_encode(CABLE_TYPE_JSON, (const uint8_t *)"{}", 2, frame, 4);
    CHECK(len == -1, "small-buffer encode returned %d, want -1", len);
}

// ── shared vectors ──────────────────────────────────────────────────────────

// Decode `input` both as one blob and one byte at a time; assert the captured
// frames and the counters match the vector's expectation.
static void run_stream_vector(const char *name, const uint8_t *input, size_t input_len,
                              const char *expect_frames, long expect_discarded, long expect_corrupt)
{
    for (int mode = 0; mode < 2; mode++) {
        cable_decoder_t d;
        cable_decoder_init(&d);
        capture_t cap = {0};
        if (mode == 0) {
            cable_decoder_feed(&d, input, input_len, capture_cb, &cap);
        } else {
            for (size_t i = 0; i < input_len; i++)
                cable_decoder_feed(&d, input + i, 1, capture_cb, &cap);
        }

        // Render the captured frames in the vector format "type:hex,...".
        char got[32 + 2 * CABLE_MAX_PAYLOAD + 64];
        got[0] = '\0';
        for (int i = 0; i < cap.count; i++) {
            char part[2 * CABLE_MAX_PAYLOAD + 16];
            char hex[2 * CABLE_MAX_PAYLOAD + 1];
            to_hex(cap.frames[i].payload, cap.frames[i].len, hex);
            snprintf(part, sizeof(part), "%u:%s", cap.frames[i].type, hex);
            if (i) strncat(got, ",", sizeof(got) - strlen(got) - 1);
            strncat(got, part, sizeof(got) - strlen(got) - 1);
        }
        if (cap.count == 0) strcpy(got, "-");

        CHECK(strcmp(got, expect_frames) == 0,
              "stream %s (%s): frames differ\n  got    %.120s...\n  expect %.120s...",
              name, mode ? "bytewise" : "blob", got, expect_frames);
        CHECK((long)d.discarded_bytes == expect_discarded,
              "stream %s (%s): discarded %lu, want %ld",
              name, mode ? "bytewise" : "blob", (unsigned long)d.discarded_bytes, expect_discarded);
        CHECK((long)d.corrupt_frames == expect_corrupt,
              "stream %s (%s): corrupt %lu, want %ld",
              name, mode ? "bytewise" : "blob", (unsigned long)d.corrupt_frames, expect_corrupt);
    }
}

static void run_encode_vector(const char *name, int type, const uint8_t *payload, size_t payload_len,
                              const char *expect_frame_hex)
{
    uint8_t frame[CABLE_MAX_FRAME];
    int len = cable_frame_encode((uint8_t)type, payload, payload_len, frame, sizeof(frame));
    CHECK(len > 0, "encode %s failed", name);
    char got[CABLE_MAX_FRAME * 2 + 1];
    to_hex(frame, (size_t)len, got);
    CHECK(strcmp(got, expect_frame_hex) == 0, "encode %s: frame mismatch", name);

    // The frame must also decode back to the same type and payload.
    cable_decoder_t d;
    cable_decoder_init(&d);
    capture_t cap = {0};
    cable_decoder_feed(&d, frame, (size_t)len, capture_cb, &cap);
    CHECK(cap.count == 1 && cap.frames[0].type == (uint8_t)type &&
          cap.frames[0].len == payload_len &&
          (payload_len == 0 || memcmp(cap.frames[0].payload, payload, payload_len) == 0),
          "encode %s: decode mismatch", name);
}

static int run_vectors(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("FAIL: cannot open %s\n", path);
        s_failures++;
        return -1;
    }
    int encode_cases = 0, stream_cases = 0;
    // An encode line carries BOTH the payload hex and the frame hex of a
    // max-size payload: 8192*2 + 8200*2 + overhead ≈ 33 KB.
    static char line[CABLE_MAX_FRAME * 4 + 512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r' || line[0] == '\0') continue;

        // Split on tabs BY HAND: the payload column can be empty
        // (pcm_zero_length), which fscanf-style whitespace parsing would eat.
        char *cols[6] = {NULL};
        int ncols = 0;
        char *start = line;
        for (char *p = line;; p++) {
            if (*p == '\t' || *p == '\n' || *p == '\r' || *p == '\0') {
                const int eol = (*p != '\t');
                *p = '\0';
                if (ncols < 6) cols[ncols++] = start;
                start = p + 1;
                if (eol) break;
            }
        }
        if (ncols < 5) continue;

        static uint8_t bytes[CABLE_MAX_FRAME];
        if (strcmp(cols[0], "encode") == 0) {
            int plen = unhex(cols[3], bytes, sizeof(bytes));
            CHECK(plen >= 0, "vector %s: bad payload hex", cols[1]);
            run_encode_vector(cols[1], atoi(cols[2]), bytes, (size_t)plen, cols[4]);
            encode_cases++;
        } else if (strcmp(cols[0], "stream") == 0) {
            int ilen = unhex(cols[2], bytes, sizeof(bytes));
            CHECK(ilen >= 0, "vector %s: bad input hex", cols[1]);
            run_stream_vector(cols[1], bytes, (size_t)ilen, cols[3],
                              atol(cols[4]), ncols > 5 ? atol(cols[5]) : 0);
            stream_cases++;
        }
    }
    fclose(f);
    printf("  vectors: %d encode + %d stream cases from %s\n", encode_cases, stream_cases, path);
    CHECK(encode_cases > 0 && stream_cases > 0, "no vectors ran");
    return 0;
}

int main(int argc, char **argv)
{
    const char *vectors = argc > 1 ? argv[1] : "../vectors/cable_frame.txt";
    test_crc_check_value();
    test_encode_roundtrip();
    test_resync_garbage_prefix();
    test_truncated_frame();
    test_oversize_payload_rejected();
    run_vectors(vectors);
    printf("test_cable_frame: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
