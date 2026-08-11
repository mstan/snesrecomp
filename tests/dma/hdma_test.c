#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "snes/dma.h"
#include "snes/ppu.h"
#include "snes/snes.h"

Ppu *g_ppu;
bool g_fail;

static uint8_t bus[0x1000000];
static uint8_t bbus[0x100];
static unsigned bbus_writes;

static int check(bool condition, const char *message) {
    if (!condition) fprintf(stderr, "FAIL: %s\n", message);
    return condition ? 0 : 1;
}

uint8_t snes_read(Snes *snes, uint32_t adr) {
    (void)snes;
    return bus[adr & 0xffffffu];
}

void snes_write(Snes *snes, uint32_t adr, uint8_t val) {
    (void)snes;
    bus[adr & 0xffffffu] = val;
}

uint8_t snes_readBBus(Snes *snes, uint8_t adr) {
    (void)snes;
    return bbus[adr];
}

void snes_writeBBus(Snes *snes, uint8_t adr, uint8_t val) {
    (void)snes;
    bbus[adr] = val;
    bbus_writes++;
}

static void clear_bus(void) {
    memset(bus, 0, sizeof bus);
    memset(bbus, 0, sizeof bbus);
    bbus_writes = 0;
}

static int test_direct_hdma(void) {
    Snes snes = {0};
    Dma *dma = dma_init(&snes);
    int failures = 0;
    if (!dma) return 1;
    snes.dma = dma;
    dma_reset(dma);
    clear_bus();

    bus[0x808000] = 0x82; /* two lines, transfer on each line */
    bus[0x808001] = 0x11;
    bus[0x808002] = 0x22;
    bus[0x808003] = 0x00;

    dma_write(dma, 0x4300, 0x00);
    dma_write(dma, 0x4301, 0x26);
    dma_write(dma, 0x4302, 0x00);
    dma_write(dma, 0x4303, 0x80);
    dma_write(dma, 0x4304, 0x80);
    dma_startDma(dma, 0x01, true);
    dma_initHdma(dma);

    failures += check(dma->channel[0].hdmaActive, "frame init must not clear hdmaActive");
    failures += check(!dma->channel[0].terminated, "enabled channel should start active");

    dma_doHdma(dma);
    failures += check(bbus[0x26] == 0x11, "direct line 1 wrote first table byte");
    failures += check(dma->channel[0].tableAdr == 0x8002, "direct line 1 advanced table");
    failures += check(dma->channel[0].repCount == 0x81, "direct line 1 decremented repeat count");
    failures += check(dma->channel[0].doTransfer, "direct line 1 reports transfer");

    dma_doHdma(dma);
    failures += check(bbus[0x26] == 0x22, "direct line 2 wrote second table byte");
    failures += check(dma->channel[0].tableAdr == 0x8003, "direct line 2 advanced table");
    failures += check(dma->channel[0].repCount == 0x80, "direct line 2 decremented repeat count");
    failures += check(dma->channel[0].doTransfer, "direct line 2 reports repeat transfer");

    dma_doHdma(dma);
    failures += check(dma->channel[0].terminated, "zero line count terminates channel");
    failures += check(!dma->channel[0].doTransfer, "terminator reports no transfer");
    failures += check(bbus_writes == 2, "terminator does not write B-bus");

    dma_free(dma);
    return failures;
}

static int test_indirect_hdma(void) {
    Snes snes = {0};
    Dma *dma = dma_init(&snes);
    int failures = 0;
    if (!dma) return 1;
    snes.dma = dma;
    dma_reset(dma);
    clear_bus();

    bus[0x818100] = 0x81; /* one line, mode 1 transfers two bytes */
    bus[0x818101] = 0x34;
    bus[0x818102] = 0x12;
    bus[0x7e1234] = 0xaa;
    bus[0x7e1235] = 0xbb;

    dma_write(dma, 0x4310, 0x41);
    dma_write(dma, 0x4311, 0x0d);
    dma_write(dma, 0x4312, 0x00);
    dma_write(dma, 0x4313, 0x81);
    dma_write(dma, 0x4314, 0x81);
    dma_write(dma, 0x4317, 0x7e);
    dma_startDma(dma, 0x02, true);
    dma_initHdma(dma);
    dma_doHdma(dma);

    failures += check(bbus[0x0d] == 0xaa, "indirect mode wrote first byte");
    failures += check(bbus[0x0e] == 0xbb, "indirect mode wrote offset byte");
    failures += check(dma->channel[1].doTransfer, "indirect line reports transfer");
    failures += check(dma->channel[1].size == 0x1236, "indirect pointer advanced by transfer length");
    failures += check(dma->channel[1].tableAdr == 0x8103, "indirect table advanced past pointer");

    dma_free(dma);
    return failures;
}

static int test_repeat_skip_hdma(void) {
    Snes snes = {0};
    Dma *dma = dma_init(&snes);
    int failures = 0;
    if (!dma) return 1;
    snes.dma = dma;
    dma_reset(dma);
    clear_bus();

    bus[0x828200] = 0x02; /* two lines, transfer only on first line */
    bus[0x828201] = 0x77;
    bus[0x828202] = 0x00;
    bus[0x828203] = 0x00;

    dma_write(dma, 0x4320, 0x00);
    dma_write(dma, 0x4321, 0x2c);
    dma_write(dma, 0x4322, 0x00);
    dma_write(dma, 0x4323, 0x82);
    dma_write(dma, 0x4324, 0x82);
    dma_startDma(dma, 0x04, true);
    dma_initHdma(dma);

    dma_doHdma(dma);
    failures += check(bbus[0x2c] == 0x77, "non-repeat descriptor transfers first line");
    failures += check(dma->channel[2].doTransfer, "non-repeat first line reports transfer");
    failures += check(dma->channel[2].repCount == 0x01, "non-repeat first line decremented count");

    dma_doHdma(dma);
    failures += check(bbus[0x2c] == 0x77, "non-repeat descriptor skips second line");
    failures += check(!dma->channel[2].doTransfer, "non-repeat skipped line reports no transfer");
    failures += check(dma->channel[2].tableAdr == 0x8202, "skipped line does not consume data byte");
    failures += check(dma->channel[2].repCount == 0x00, "non-repeat second line reaches descriptor boundary");

    dma_doHdma(dma);
    failures += check(dma->channel[2].terminated, "non-repeat terminator reached after skipped line");
    failures += check(bbus_writes == 1, "non-repeat descriptor wrote once");

    dma_free(dma);
    return failures;
}

static int test_mid_frame_enable_disable(void) {
    Snes snes = {0};
    Dma *dma = dma_init(&snes);
    int failures = 0;
    if (!dma) return 1;
    snes.dma = dma;
    dma_reset(dma);
    clear_bus();

    bus[0x838300] = 0x81;
    bus[0x838301] = 0x55;
    bus[0x838302] = 0x00;

    dma_write(dma, 0x4330, 0x00);
    dma_write(dma, 0x4331, 0x2d);
    dma_write(dma, 0x4332, 0x00);
    dma_write(dma, 0x4333, 0x83);
    dma_write(dma, 0x4334, 0x83);

    dma_startDma(dma, 0x00, true);
    dma_initHdma(dma);
    dma_startDma(dma, 0x08, true);
    dma_doHdma(dma);
    failures += check(bbus_writes == 0, "mid-frame enable waits for next frame init");
    failures += check(dma->channel[3].terminated, "mid-frame enabled channel remains terminated");

    dma_initHdma(dma);
    dma_doHdma(dma);
    failures += check(bbus[0x2d] == 0x55, "next frame init arms newly enabled channel");
    failures += check(bbus_writes == 1, "newly enabled channel writes after frame init");

    dma_startDma(dma, 0x00, true);
    dma_doHdma(dma);
    failures += check(bbus_writes == 1, "mid-frame disable suppresses further HDMA");
    failures += check(!dma->channel[3].hdmaActive, "mid-frame disable clears enable bit");

    dma_free(dma);
    return failures;
}

int main(void) {
    int failures = 0;
    failures += test_direct_hdma();
    failures += test_indirect_hdma();
    failures += test_repeat_skip_hdma();
    failures += test_mid_frame_enable_disable();
    if (failures) return 1;
    puts("hdma_test: ok");
    return 0;
}
