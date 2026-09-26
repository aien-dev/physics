/*
 * m3tool -- PHYSICS M3 host tool (no Python). Host: aarch64 little-endian.
 *
 * Built by m3/build.sh with the host gcc, linking the repo-root
 * sha256_clean.c (the same audited source the image uses). Results must
 * still be cross-checked against coreutils sha256sum so a bug shared by the
 * image and this tool cannot hide.
 *
 * Subcommands:
 *   sha256   <file>                     canonical SHA-256 hex (as sha256sum)
 *   hexfield <file> <offset> <width>    LE unsigned field, width 1/2/4/8 -> 0x...
 *   bytes    <file> <offset> <len>      raw bytes as contiguous lowercase hex
 *
 * Adding a subcommand: write `static int cmd_x(int argc, char **argv)` (argv[0]
 * is the subcommand name; return 0 on success, nonzero on failure) and add a
 * row to CMDS[]. Shared helpers: read_file(), parse_u64(), sha256_file_buf(),
 * put_hex(). Every failure prints "m3tool: ..." to stderr and exits nonzero.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* From the repo-root sha256_clean.c. data must be 4-byte aligned. */
void sha256_compute(const uint8_t *data, uint64_t len, uint8_t scratch_buf[128],
                    uint32_t out_digest[8]);

#define M3TOOL_MAX_FILE (1ull << 30) /* 1 GiB: keeps len*8 far from overflow */

#include <stdarg.h>

static int fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "m3tool: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    return 1;
}

/* Read a whole file into a malloc'd (16-byte aligned) buffer. */
static int read_file(const char *path, uint8_t **out, uint64_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return fail("cannot open %s", path);
    uint64_t cap = 65536, len = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) { fclose(f); return fail("out of memory reading %s", path); }
    for (;;) {
        if (len == cap) {
            if (cap >= M3TOOL_MAX_FILE) { free(buf); fclose(f); return fail("file too large: %s", path); }
            uint8_t *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); fclose(f); return fail("out of memory reading %s", path); }
            buf = nb; cap *= 2;
        }
        size_t n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (n == 0) break;
    }
    int err = ferror(f);
    fclose(f);
    if (err) { free(buf); return fail("read error on %s", path); }
    *out = buf; *out_len = len;
    return 0;
}

/* Strict unsigned parse: decimal or 0x hex, whole string, no sign. */
static int parse_u64(const char *s, uint64_t *v) {
    if (!s || !*s || *s == '-' || *s == '+' || *s == ' ') return fail("bad number: %s", s ? s : "(null)");
    char *end; errno = 0;
    unsigned long long x = strtoull(s, &end, 0);
    if (errno || *end) return fail("bad number: %s", s);
    *v = x;
    return 0;
}

static void put_hex(const uint8_t *p, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) printf("%02x", p[i]);
}

/* Canonical big-endian digest bytes. */
static void sha256_file_buf(const uint8_t *buf, uint64_t len, uint8_t out[32]) {
    uint8_t scratch[128] __attribute__((aligned(16)));
    uint32_t st[8];
    sha256_compute(buf, len, scratch, st);
    for (int i = 0; i < 8; i++) {
        out[4 * i + 0] = (uint8_t)(st[i] >> 24);
        out[4 * i + 1] = (uint8_t)(st[i] >> 16);
        out[4 * i + 2] = (uint8_t)(st[i] >> 8);
        out[4 * i + 3] = (uint8_t)(st[i]);
    }
}

/* Load file and bounds-check [off, off+n) without overflow. */
static int load_range(const char *path, const char *off_s, uint64_t n,
                      uint8_t **buf, uint64_t *off) {
    uint64_t len;
    if (parse_u64(off_s, off)) return 1;
    if (read_file(path, buf, &len)) return 1;
    if (*off > len || n > len - *off) { free(*buf); return fail("range outside file %s", path); }
    return 0;
}

static int cmd_sha256(int argc, char **argv) {
    if (argc != 2) return fail("usage: %s <file>", argv[0]);
    uint8_t *buf; uint64_t len; uint8_t d[32];
    if (read_file(argv[1], &buf, &len)) return 1;
    sha256_file_buf(buf, len, d);
    free(buf);
    put_hex(d, 32); putchar('\n');
    return 0;
}

static int cmd_hexfield(int argc, char **argv) {
    if (argc != 4) return fail("usage: %s <file> <offset> <width 1|2|4|8>", argv[0]);
    uint64_t w, off, v = 0; uint8_t *buf;
    if (parse_u64(argv[3], &w)) return 1;
    if (w != 1 && w != 2 && w != 4 && w != 8) return fail("width must be 1, 2, 4 or 8: %s", argv[3]);
    if (load_range(argv[1], argv[2], w, &buf, &off)) return 1;
    for (uint64_t i = 0; i < w; i++) v |= (uint64_t)buf[off + i] << (8 * i);
    free(buf);
    printf("0x%0*llx\n", (int)(2 * w), (unsigned long long)v);
    return 0;
}

static int cmd_bytes(int argc, char **argv) {
    if (argc != 4) return fail("usage: %s <file> <offset> <len>", argv[0]);
    uint64_t n, off; uint8_t *buf;
    if (parse_u64(argv[3], &n)) return 1;
    if (load_range(argv[1], argv[2], n, &buf, &off)) return 1;
    put_hex(buf + off, n); putchar('\n');
    free(buf);
    return 0;
}

static int cmd_pin_inc(int argc, char **argv) {
    if (argc != 3) return fail("usage: %s <binary> <out-inc-file>", argv[0]);
    uint8_t *buf; uint64_t len; uint8_t d[32];
    if (read_file(argv[1], &buf, &len)) return 1;
    sha256_file_buf(buf, len, d);
    free(buf);

    FILE *f = fopen(argv[2], "w");
    if (!f) return fail("cannot open %s for writing", argv[2]);
    fprintf(f, "/* Generated by m3tool pin-inc */\n");
    fprintf(f, "    .word ");
    for (int i = 0; i < 4; i++) {
        uint32_t w = ((uint32_t)d[4*i] << 24) | ((uint32_t)d[4*i+1] << 16) |
                     ((uint32_t)d[4*i+2] << 8)  | ((uint32_t)d[4*i+3]);
        fprintf(f, "0x%08x%s", w, i < 3 ? ", " : "\n");
    }
    fprintf(f, "    .word ");
    for (int i = 4; i < 8; i++) {
        uint32_t w = ((uint32_t)d[4*i] << 24) | ((uint32_t)d[4*i+1] << 16) |
                     ((uint32_t)d[4*i+2] << 8)  | ((uint32_t)d[4*i+3]);
        fprintf(f, "0x%08x%s", w, i < 7 ? ", " : "\n");
    }
    fclose(f);
    return 0;
}

static int cmd_verify_receipt(int argc, char **argv) {
    if (argc != 3) return fail("usage: %s <dump-file> <offset>", argv[0]);
    uint64_t off; uint8_t *buf;
    if (load_range(argv[1], argv[2], 192, &buf, &off)) return 1;
    uint8_t *rcpt = buf + off;

    uint8_t computed[32];
    sha256_file_buf(rcpt, 128, computed);

    if (memcmp(computed, rcpt + 0x80, 32) != 0) {
        free(buf);
        return fail("receipt digest mismatch at offset %s", argv[2]);
    }
    uint32_t decision = *(uint32_t *)(rcpt + 4);
    uint64_t effect = *(uint64_t *)(rcpt + 0x38);
    printf("PASS receipt at %s decision=0x%08x effect=0x%016llx digest=", argv[2], decision, (unsigned long long)effect);
    put_hex(computed, 32);
    putchar('\n');
    free(buf);
    return 0;
}

static int cmd_verify_chain(int argc, char **argv) {
    if (argc != 2) return fail("usage: %s <receipt-ledger-dump>", argv[0]);
    uint8_t *buf; uint64_t len;
    if (read_file(argv[1], &buf, &len)) return 1;
    if (len < 128) { free(buf); return fail("dump too small for ledger header: %s", argv[1]); }

    uint64_t magic = *(uint64_t *)(buf + 0);
    if (magic != 0x524350544C444752ULL) { free(buf); return fail("bad ledger magic 0x%llx", (unsigned long long)magic); }

    uint32_t cap = *(uint32_t *)(buf + 8);
    uint32_t count = *(uint32_t *)(buf + 12);
    if (cap != 32 || count > 32) { free(buf); return fail("invalid cap=%u or count=%u", (unsigned)cap, (unsigned)count); }

    uint8_t prev_digest[32];
    memset(prev_digest, 0, 32);

    for (uint32_t i = 0; i < count; i++) {
        uint64_t roff = 128 + i * 192;
        if (roff + 192 > len) { free(buf); return fail("truncated receipt %u at offset %llu", i, (unsigned long long)roff); }
        uint8_t *rcpt = buf + roff;

        /* Verify previous_receipt_digest */
        if (memcmp(rcpt + 0x60, prev_digest, 32) != 0) {
            free(buf);
            return fail("broken chain at receipt %u: previous digest mismatch", (unsigned)i);
        }

        /* Verify receipt_digest */
        uint8_t comp[32];
        sha256_file_buf(rcpt, 128, comp);
        if (memcmp(comp, rcpt + 0x80, 32) != 0) {
            free(buf);
            return fail("tampered receipt %u: computed digest does not match seal", (unsigned)i);
        }
        memcpy(prev_digest, comp, 32);
    }

    if (count > 0) {
        if (memcmp(buf + 0x20, prev_digest, 32) != 0) {
            free(buf);
            return fail("%s", "header.latest_receipt_digest does not match last receipt seal");
        }
    }

    printf("PASS receipt chain: %u receipts verified (chain intact)\n", count);
    free(buf);
    return 0;
}

static const struct {
    const char *name;
    int (*fn)(int, char **);
    const char *help;
} CMDS[] = {
    { "sha256",         cmd_sha256,         "sha256 <file>" },
    { "hexfield",       cmd_hexfield,       "hexfield <file> <offset> <width 1|2|4|8>" },
    { "bytes",          cmd_bytes,          "bytes <file> <offset> <len>" },
    { "pin-inc",        cmd_pin_inc,        "pin-inc <binary> <out-inc-file>" },
    { "verify-receipt", cmd_verify_receipt, "verify-receipt <dump-file> <offset>" },
    { "verify-chain",   cmd_verify_chain,   "verify-chain <receipt-ledger-dump>" },
};

int main(int argc, char **argv) {
    if (argc >= 2) {
        for (size_t i = 0; i < sizeof CMDS / sizeof CMDS[0]; i++) {
            if (strcmp(argv[1], CMDS[i].name) == 0) {
                int rc = CMDS[i].fn(argc - 1, argv + 1);
                if (fflush(stdout) != 0 || ferror(stdout)) return fail("%s", "write error on stdout");
                return rc ? 1 : 0;
            }
        }
    }
    fprintf(stderr, "usage: m3tool <subcommand> ...\n");
    for (size_t i = 0; i < sizeof CMDS / sizeof CMDS[0]; i++) fprintf(stderr, "  m3tool %s\n", CMDS[i].help);
    return 2;
}
