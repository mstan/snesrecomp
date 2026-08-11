#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "snes/dma.h"
#include "snes/ppu.h"
#include "snes/snes.h"

Ppu *g_ppu;
uint8_t g_snesrecomp_last_hdmaen;
bool g_fail;
int g_interp_apu_driving;

static uint8_t ram[0x20000];
static uint8_t ppu_regs[0x40];

uint8_t ppu_read(Ppu *ppu, uint8_t adr) {
    (void)ppu;
    return ppu_regs[adr & 0x3f];
}

void ppu_write(Ppu *ppu, uint8_t adr, uint8_t val) {
    (void)ppu;
    ppu_regs[adr & 0x3f] = val;
}

void RtlApuLock(void) {}
void RtlApuUnlock(void) {}
void rtl_sync_apu_to_cpu_locked(void) {}
void RtlApuWrite(uint16_t adr, uint8_t val) { (void)adr; (void)val; }
void audio_trace_on_cpu_port_read(uint8_t port, uint8_t value) {
    (void)port;
    (void)value;
}

Cpu *cpu_init(void) { return NULL; }
void cpu_free(Cpu *cpu) { (void)cpu; }
void cpu_reset(Cpu *cpu) { (void)cpu; }
void cpu_saveload(Cpu *cpu, SaveLoadInfo *sli) { (void)cpu; (void)sli; }

Apu *apu_init(void) { return NULL; }
void apu_free(Apu *apu) { (void)apu; }
void apu_reset(Apu *apu) { (void)apu; }
void apu_cycle(Apu *apu) { (void)apu; }
void apu_saveload(Apu *apu, SaveLoadInfo *sli) { (void)apu; (void)sli; }

Ppu *ppu_init(void) { return NULL; }
void ppu_free(Ppu *ppu) { (void)ppu; }
void ppu_reset(Ppu *ppu) { (void)ppu; }
void ppu_saveload(Ppu *ppu, SaveLoadInfo *sli) { (void)ppu; (void)sli; }

Cart *cart_init(Snes *snes) { (void)snes; return NULL; }
void cart_free(Cart *cart) { (void)cart; }
void cart_reset(Cart *cart) { (void)cart; }
void cart_saveload(Cart *cart, SaveLoadInfo *sli) { (void)cart; (void)sli; }

void audio_trace_set_producer(int producer) { (void)producer; }
void joypad_write_strobe(Snes *snes, uint8_t value) { (void)snes; (void)value; }
uint8_t joypad_read_serial(Snes *snes, unsigned port) { (void)snes; (void)port; return 0; }
void ppudma_record_dma(int channel, int fromB, uint8_t aBank, uint16_t aAdr,
                       uint8_t bAdr, uint16_t size) {
    (void)channel;
    (void)fromB;
    (void)aBank;
    (void)aAdr;
    (void)bAdr;
    (void)size;
}

uint8_t cart_read(Cart *cart, uint8_t bank, uint16_t adr) {
    (void)cart;
    (void)bank;
    (void)adr;
    return 0;
}

void cart_write(Cart *cart, uint8_t bank, uint16_t adr, uint8_t val) {
    (void)cart;
    (void)bank;
    (void)adr;
    (void)val;
}

static int check(bool condition, const char *message) {
    if (!condition) fprintf(stderr, "FAIL: %s\n", message);
    return condition ? 0 : 1;
}

int main(void) {
    Snes snes = {0};
    Dma *dma = dma_init(&snes);
    int failures = 0;
    if (!dma) return 2;
    snes.dma = dma;
    snes.ram = ram;
    dma_reset(dma);
    memset(ram, 0, sizeof ram);
    memset(ppu_regs, 0, sizeof ppu_regs);

    ram[0x0100] = 0x81;
    ram[0x0101] = 0x44;
    ram[0x0102] = 0x00;
    dma_write(dma, 0x4300, 0x00);
    dma_write(dma, 0x4301, 0x26);
    dma_write(dma, 0x4302, 0x00);
    dma_write(dma, 0x4303, 0x01);
    dma_write(dma, 0x4304, 0x7e);
    dma_startDma(dma, 0x01, true);
    dma_initHdma(dma);

    snes_advance_master_cycles(&snes, 1023);
    failures += check(ppu_regs[0x26] == 0x00, "HDMA does not fire before HBlank");
    snes_advance_master_cycles(&snes, 1);
    failures += check(ppu_regs[0x26] == 0x44, "HDMA fires at HBlank");
    failures += check(dma->channel[0].tableAdr == 0x0102, "HDMA advanced table at timing edge");

    snes_advance_master_cycles(&snes, 1364u * 262u - 1024u);
    failures += check(snes.vPos == 0 && snes.hPos == 0, "beam reached next frame");
    failures += check(dma->channel[0].tableAdr == 0x0100, "frame rollover reinitialized table pointer");
    failures += check(dma->channel[0].hdmaActive, "frame rollover preserved HDMAEN state");

    dma_free(dma);
    if (failures) return 1;
    puts("hdma_timing_test: ok");
    return 0;
}
