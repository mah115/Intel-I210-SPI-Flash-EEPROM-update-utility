/*
 * i210flash.c - program the SPI flash behind a blank Intel I210 (8086:1531)
 * by bit-banging SPI through the I210's FLA (Flash Access) register in BAR0.
 *
 * Bypasses the I210's internal flash manager (which times out on blank
 * flash), so it works on a never-programmed part. Target flash: Winbond
 * W25Q80DV (8 Mbit / 1 MiB, JEDEC ID EF 40 14).
 *
 * Build:  gcc -O2 -Wall -o i210flash i210flash.c
 * Usage:  ./i210flash info  IMAGE.bin            (inspect image, no hardware)
 *         sudo ./i210flash BDF id                (read JEDEC ID - do this first)
 *         sudo ./i210flash BDF read  OUT.bin     (dump whole flash)
 *         sudo ./i210flash BDF write IMAGE.bin   (unprotect, erase, program, verify)
 *         sudo ./i210flash BDF verify IMAGE.bin
 *   BDF is the full PCI address from lspci -D, e.g. 0007:01:00.0
 *
 * Register offsets and FLA bits match the Linux igb driver, flashrom's
 * nicintel_spi I210 support, and the I210 datasheet register map. Validated
 * on a Jetson Orin NX (JetPack 7) carrier with an I210-IT + W25Q80DV.
 * The device must not be bound to a kernel driver (igb) while in use.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define REG_STATUS 0x00008
#define REG_EEC    0x12010
#define REG_FLA    0x1201C

#define FL_SCK (1u << 0)
#define FL_CE  (1u << 1)
#define FL_SI  (1u << 2)
#define FL_SO  (1u << 3)
#define FL_REQ (1u << 4)
#define FL_GNT (1u << 5)

#define FLASH_SIZE  (1u << 20)   /* W25Q80DV = 8 Mbit */
#define FPAGE       256
#define MAP_LEN     0x20000

static volatile uint32_t *bar;
static uint32_t cs_on, cs_off;   /* FLA values with chip select asserted / released */

static inline uint32_t rd(uint32_t off) { return bar[off / 4]; }
static inline void wr(uint32_t off, uint32_t v) { bar[off / 4] = v; }

/* ---------- SPI primitives (mode 0) ---------- */
static void cs_select(void)  { wr(REG_FLA, cs_on); (void)rd(REG_FLA); }
static void cs_release(void) { wr(REG_FLA, cs_off); (void)rd(REG_FLA); }

static uint8_t xfer(uint8_t out)
{
    uint8_t in = 0;
    for (int i = 7; i >= 0; i--) {
        uint32_t v = cs_on | (((out >> i) & 1) ? FL_SI : 0);
        wr(REG_FLA, v);                 /* SCK low, present SI   */
        wr(REG_FLA, v | FL_SCK);        /* rising edge           */
        in = (uint8_t)((in << 1) | ((rd(REG_FLA) & FL_SO) ? 1 : 0));
    }
    wr(REG_FLA, cs_on);                 /* falling edge, SCK idle low */
    return in;
}

static void jedec_id(uint8_t id[3])
{
    cs_select(); xfer(0x9F);
    id[0] = xfer(0); id[1] = xfer(0); id[2] = xfer(0);
    cs_release();
}

static uint8_t rdsr1(void)
{
    cs_select(); xfer(0x05); uint8_t s = xfer(0); cs_release(); return s;
}

static uint8_t rdsr2(void)
{
    cs_select(); xfer(0x35); uint8_t s = xfer(0); cs_release(); return s;
}

static void wren(void) { cs_select(); xfer(0x06); cs_release(); }

static int wait_ready(int timeout_ms)
{
    for (int i = 0; i < timeout_ms * 10; i++) {
        if (!(rdsr1() & 0x01)) return 0;
        usleep(100);
    }
    return -1;
}

/* ---------- flash ownership / CS polarity ---------- */
static int request_flash(void)
{
    wr(REG_FLA, FL_REQ);
    for (int i = 0; i < 2000; i++) {
        if (rd(REG_FLA) & FL_GNT) return 0;
        usleep(100);
    }
    return -1;
}

static void release_flash(void) { wr(REG_FLA, 0); (void)rd(REG_FLA); }

static int id_plausible(const uint8_t id[3])
{
    int all00 = !id[0] && !id[1] && !id[2];
    int allFF = id[0] == 0xFF && id[1] == 0xFF && id[2] == 0xFF;
    return !all00 && !allFF;
}

/* Try both FL_CE polarities; keep the one that returns a real JEDEC ID. */
static int detect(uint8_t id[3])
{
    const uint32_t pol[2][2] = {
        { FL_REQ | FL_CE, FL_REQ },          /* FL_CE=1 asserts chip select */
        { FL_REQ,         FL_REQ | FL_CE },  /* FL_CE=1 releases chip select */
    };
    for (int p = 0; p < 2; p++) {
        cs_on = pol[p][0]; cs_off = pol[p][1];
        cs_release(); usleep(10);
        jedec_id(id);
        if (id_plausible(id)) {
            printf("JEDEC ID: %02X %02X %02X  (CS polarity %s)\n", id[0], id[1], id[2],
                   p ? "FL_CE=1 releases" : "FL_CE=1 asserts");
            if (!(id[0] == 0xEF && id[1] == 0x40 && id[2] == 0x14))
                printf("WARNING: not the expected W25Q80DV ID (EF 40 14)\n");
            return 0;
        }
    }
    fprintf(stderr, "No flash response with either CS polarity (last ID %02X %02X %02X).\n"
            "Check FLA offset/bits against the datasheet, and flash power/wiring.\n",
            id[0], id[1], id[2]);
    return -1;
}

/* ---------- flash operations ---------- */
static int unprotect(void)
{
    uint8_t s1 = rdsr1(), s2 = rdsr2();
    printf("Status regs before: SR1=%02X SR2=%02X\n", s1, s2);
    wren();
    cs_select(); xfer(0x01); xfer(0x00); xfer(0x00); cs_release();  /* SR1=0, SR2=0 */
    if (wait_ready(100)) { fprintf(stderr, "WRSR timeout\n"); return -1; }
    s1 = rdsr1(); s2 = rdsr2();
    printf("Status regs after:  SR1=%02X SR2=%02X\n", s1, s2);
    if ((s1 & 0xFC) || (s2 & 0x41)) {
        fprintf(stderr, "Protection bits did not clear (check /WP is high)\n");
        return -1;
    }
    return 0;
}

static int chip_erase(void)
{
    printf("Erasing chip...\n");
    wren();
    cs_select(); xfer(0xC7); cs_release();
    if (wait_ready(30000)) { fprintf(stderr, "Chip erase timeout\n"); return -1; }
    return 0;
}

static int page_program(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    wren();
    cs_select();
    xfer(0x02); xfer(addr >> 16); xfer(addr >> 8); xfer(addr);
    for (uint32_t i = 0; i < len; i++) xfer(buf[i]);
    cs_release();
    return wait_ready(50);
}

static void flash_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    cs_select();
    xfer(0x03); xfer(addr >> 16); xfer(addr >> 8); xfer(addr);
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = xfer(0);
        if ((i & 0xFFFF) == 0xFFFF) { printf("  read %u KiB\r", (i + 1) >> 10); fflush(stdout); }
    }
    cs_release();
    printf("\n");
}

/* ---------- image handling ---------- */
static uint8_t *load_file(const char *path, uint32_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (long)FLASH_SIZE) {
        fprintf(stderr, "%s: size %ld bytes, must be 1..%u\n", path, n, FLASH_SIZE);
        fclose(f); return NULL;
    }
    uint8_t *b = malloc(n);
    if (fread(b, 1, n, f) != (size_t)n) { perror("read"); fclose(f); free(b); return NULL; }
    fclose(f);
    *len = (uint32_t)n;
    return b;
}

static uint16_t w16(const uint8_t *b, int word) { return b[word * 2] | (b[word * 2 + 1] << 8); }

static void image_info(const uint8_t *b, uint32_t len)
{
    uint16_t sum = 0;
    for (int i = 0; i <= 0x3F; i++) sum += w16(b, i);
    printf("Image size:   %u bytes (%u Kbit)\n", len, len * 8 / 1024);
    printf("MAC (w0-2):   %02X:%02X:%02X:%02X:%02X:%02X\n", b[0], b[1], b[2], b[3], b[4], b[5]);
    printf("Subsys ID:    0x%04X  Subsys vendor: 0x%04X\n", w16(b, 0x0B), w16(b, 0x0C));
    printf("Device ID:    0x%04X  Vendor: 0x%04X\n", w16(b, 0x0D), w16(b, 0x0E));
    printf("Checksum:     words 0x00-0x3F sum to 0x%04X (%s)\n", sum,
           sum == 0xBABA ? "valid" : "INVALID - expected 0xBABA");
}

/* ---------- PCI setup ---------- */
static int pci_open(const char *bdf)
{
    char path[256], link[256];
    snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/driver", bdf);
    ssize_t n = readlink(path, link, sizeof link - 1);
    if (n > 0) {
        link[n] = 0;
        const char *drv = strrchr(link, '/');
        drv = drv ? drv + 1 : link;
        fprintf(stderr, "Device %s is bound to driver '%s'. Unbind it first:\n"
                "  echo %s | sudo tee /sys/bus/pci/drivers/%s/unbind\n", bdf, drv, bdf, drv);
        return -1;
    }
    snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/config", bdf);
    int fd = open(path, O_RDWR);
    if (fd < 0) { perror(path); return -1; }
    uint16_t ven, dev, cmd;
    if (pread(fd, &ven, 2, 0) != 2 || pread(fd, &dev, 2, 2) != 2 || pread(fd, &cmd, 2, 4) != 2) {
        perror("read PCI config"); close(fd); return -1;
    }
    printf("PCI %s: %04x:%04x, COMMAND=0x%04x\n", bdf, ven, dev, cmd);
    if (ven != 0x8086 || (dev != 0x1531 && dev != 0x1533)) {
        fprintf(stderr, "Not an I210 (expected 8086:1531 or 8086:1533)\n");
        close(fd); return -1;
    }
    if (!(cmd & 0x2)) {
        cmd |= 0x2;
        if (pwrite(fd, &cmd, 2, 4) != 2) { perror("write PCI COMMAND"); close(fd); return -1; }
        printf("Enabled memory decode\n");
    }
    close(fd);

    snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/resource0", bdf);
    fd = open(path, O_RDWR | O_SYNC);
    if (fd < 0) { perror(path); return -1; }
    void *m = mmap(NULL, MAP_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { perror("mmap resource0"); return -1; }
    bar = m;

    uint32_t st = rd(REG_STATUS), eec = rd(REG_EEC), fla = rd(REG_FLA);
    printf("STATUS=0x%08x EEC=0x%08x FLA=0x%08x\n", st, eec, fla);
    if (st == 0xFFFFFFFF) {
        fprintf(stderr, "BAR0 reads all 1s - device registers not reachable\n");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "info")) {
        uint32_t len; uint8_t *img = load_file(argv[2], &len);
        if (!img) return 1;
        image_info(img, len); return 0;
    }
    if (argc < 3) {
        fprintf(stderr, "usage: %s info IMG | %s BDF id|read OUT|write IMG|verify IMG\n",
                argv[0], argv[0]);
        return 1;
    }
    const char *bdf = argv[1], *cmd = argv[2], *file = argc > 3 ? argv[3] : NULL;

    if (pci_open(bdf)) return 1;
    if (request_flash())
        printf("WARNING: FL_GNT not asserted; continuing (JEDEC test will tell)\n");

    uint8_t id[3];
    int rc = 1;
    if (detect(id)) goto out;

    if (!strcmp(cmd, "id")) {
        printf("SR1=%02X SR2=%02X\n", rdsr1(), rdsr2());
        rc = 0;
    } else if (!strcmp(cmd, "read") && file) {
        uint8_t *buf = malloc(FLASH_SIZE);
        flash_read(0, buf, FLASH_SIZE);
        FILE *f = fopen(file, "wb");
        if (f && fwrite(buf, 1, FLASH_SIZE, f) == FLASH_SIZE) { printf("Wrote %s\n", file); rc = 0; }
        if (f) fclose(f);
        free(buf);
    } else if ((!strcmp(cmd, "write") || !strcmp(cmd, "verify")) && file) {
        uint32_t len; uint8_t *img = load_file(file, &len);
        if (!img) goto out;
        image_info(img, len);
        if (!strcmp(cmd, "write")) {
            if (unprotect() || chip_erase()) goto out;
            printf("Programming %u bytes...\n", len);
            for (uint32_t a = 0; a < len; a += FPAGE) {
                uint32_t n = len - a < FPAGE ? len - a : FPAGE, blank = 1;
                for (uint32_t i = 0; i < n; i++) if (img[a + i] != 0xFF) { blank = 0; break; }
                if (!blank && page_program(a, img + a, n)) {
                    fprintf(stderr, "Program timeout at 0x%06x\n", a); goto out;
                }
                if ((a & 0xFFFF) == 0) { printf("  0x%06x\r", a); fflush(stdout); }
            }
            printf("\n");
        }
        printf("Verifying...\n");
        uint8_t *rb = malloc(len);
        flash_read(0, rb, len);
        for (uint32_t i = 0; i < len; i++)
            if (rb[i] != img[i]) {
                fprintf(stderr, "MISMATCH at 0x%06x: flash %02X, image %02X\n", i, rb[i], img[i]);
                free(rb); goto out;
            }
        free(rb);
        printf("Verify OK. Fully power-cycle the board; the I210 should enumerate as 8086:1533.\n");
        rc = 0;
    } else {
        fprintf(stderr, "unknown command or missing file\n");
    }
out:
    release_flash();
    return rc;
}
