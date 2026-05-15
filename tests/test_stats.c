#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <rte_eal.h>

#include "flow.h"
#include "stats.h"

static void init_eal(void)
{
    char *argv[] = {
        "test_stats", "-c", "0x1",
        "--no-huge", "--no-pci",
        "--file-prefix=test_stats",
        "--iova-mode=va", "--no-telemetry",
        "--log-level", "1",
    };
    int argc = (int)(sizeof(argv) / sizeof(argv[0]));
    if (rte_eal_init(argc, argv) < 0) {
        fprintf(stderr, "rte_eal_init failed\n");
        exit(1);
    }
}

static struct flow_entry make_entry(uint32_t sip, uint32_t dip,
                                    uint16_t sp, uint16_t dp,
                                    uint8_t proto,
                                    uint64_t rxb, uint64_t txb,
                                    uint64_t rxp, uint64_t txp)
{
    struct flow_entry e = {0};
    e.key.src_ip   = sip;
    e.key.dst_ip   = dip;
    e.key.src_port = sp;
    e.key.dst_port = dp;
    e.key.proto    = proto;
    e.rx_bytes     = rxb;
    e.tx_bytes     = txb;
    e.rx_packets   = rxp;
    e.tx_packets   = txp;
    return e;
}

static void test_header_format(void)
{
    char buf[512] = {0};
    FILE *f = fmemopen(buf, sizeof(buf), "w");
    assert(f);
    stats_write_header(f);
    fflush(f);
    fclose(f);

    assert(strstr(buf, "timestamp")   != NULL);
    assert(strstr(buf, "src_ip")      != NULL);
    assert(strstr(buf, "dst_ip")      != NULL);
    assert(strstr(buf, "src_port")    != NULL);
    assert(strstr(buf, "dst_port")    != NULL);
    assert(strstr(buf, "proto")       != NULL);
    assert(strstr(buf, "rx_bytes")    != NULL);
    assert(strstr(buf, "tx_bytes")    != NULL);
    assert(strstr(buf, "rx_packets")  != NULL);
    assert(strstr(buf, "tx_packets")  != NULL);

    printf("PASS: test_header_format\n");
}

static void test_row_format(void)
{
    struct flow_entry e = make_entry(
        __builtin_bswap32(0x01020304),  /* 1.2.3.4 in network order */
        __builtin_bswap32(0x05060708),  /* 5.6.7.8 */
        __builtin_bswap16(80),
        __builtin_bswap16(443),
        6, 4096, 2048, 10, 5);

    char buf[512] = {0};
    FILE *f = fmemopen(buf, sizeof(buf), "w");
    assert(f);
    stats_write_row(f, &e, "2026-01-01T00:00:00Z");
    fflush(f);
    fclose(f);

    assert(strstr(buf, "1.2.3.4") != NULL);
    assert(strstr(buf, "5.6.7.8") != NULL);
    assert(strstr(buf, "80")      != NULL);
    assert(strstr(buf, "443")     != NULL);
    assert(strstr(buf, "6")       != NULL);
    assert(strstr(buf, "4096")    != NULL);
    assert(strstr(buf, "2048")    != NULL);
    assert(strstr(buf, "10")      != NULL);
    assert(strstr(buf, "5")       != NULL);
    assert(strchr(buf, 'T')       != NULL);
    assert(strchr(buf, 'Z')       != NULL);

    printf("PASS: test_row_format\n");
}

static void test_header_written_once(void)
{
    char buf[512] = {0};
    FILE *f = fmemopen(buf, sizeof(buf), "w+");
    assert(f);

    if (ftell(f) == 0)
        stats_write_header(f);
    fflush(f);
    size_t len1 = strlen(buf);
    assert(len1 > 0);

    /* Second call skipped because position != 0 */
    if (ftell(f) == 0)
        stats_write_header(f);
    fflush(f);
    assert(strlen(buf) == len1);

    fclose(f);
    printf("PASS: test_header_written_once\n");
}

int main(void)
{
    init_eal();
    test_header_format();
    test_row_format();
    test_header_written_once();
    printf("All stats tests passed.\n");
    rte_eal_cleanup();
    return 0;
}
