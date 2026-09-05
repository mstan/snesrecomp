#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "common_cpu_infra.h"
#include "cpu_state.h"
#include "snes/cart.h"
#include "snes/snes.h"

typedef struct recomp_snap_entry recomp_snap_entry;
typedef struct Ppu Ppu;
typedef struct Dma Dma;

#ifdef EXPECT_FUNC_SNAPSHOT_ON
#if SNESRECOMP_FUNC_SNAPSHOT != 1
#error "SNESRECOMP_FUNC_SNAPSHOT must be numeric 1 when opt-in diagnostics are enabled"
#endif
#endif

#ifdef EXPECT_STACK_BALANCE_DIAGNOSTICS_ON
#if SNESRECOMP_STACK_BALANCE_DIAGNOSTICS != 1
#error "SNESRECOMP_STACK_BALANCE_DIAGNOSTICS must be numeric 1 when opt-in diagnostics are enabled"
#endif
#endif

CpuState g_cpu;
int snes_frame_counter;
int g_wlog_configured;
uint8 g_ram[0x20000];
uint8 *g_sram;
int g_sram_size;
const uint8 *g_rom;
Ppu *g_ppu;
Dma *g_dma;

extern const char *g_recomp_snap_on_func;
extern int g_recomp_snap_count;
extern int g_recomp_stack_top;
extern uint16_t g_cpu_entry_s[];
extern const recomp_snap_entry *recomp_snap_lookup(int call_idx);

int wlog_scope_available(void) { return 0; }
void wlog_scope_enter(const char *tag) { (void)tag; }
void wlog_scope_exit(void) {}
uint8 *RomPtr(uint32_t addr) { (void)addr; return NULL; }
void Tier2CoverageWriteDefaultManifest(const char *rom_title) {
  (void)rom_title;
}
void tier2_capture_set_default_enabled(int enabled) { (void)enabled; }
int tier2_capture_enabled(void) { return 0; }
void msu1_init(void) {}
Snes *snes_init(uint8_t *ram) { (void)ram; return NULL; }
bool snes_loadRom(Snes *snes, const uint8_t *data, int length) {
  (void)snes; (void)data; (void)length; return false;
}
void snes_reset(Snes *snes, bool hard) { (void)snes; (void)hard; }
void cart_set_master_clock_source(Cart *cart, const uint64_t *master_clock) {
  (void)cart; (void)master_clock;
}
void ppu_reset(Ppu *ppu) { (void)ppu; }
void dma_reset(Dma *dma) { (void)dma; }

static int check(int condition, const char *message) {
  if (!condition)
    fprintf(stderr, "FAIL: %s\n", message);
  return condition ? 0 : 1;
}

static int json_matches_stack_balance_mode(void) {
  FILE *f = tmpfile();
  char buf[256];
  size_t n;
  int ok;

  if (!f)
    return 0;
  RecompStackBalDumpJson(f);
  rewind(f);
  n = fread(buf, 1, sizeof(buf) - 1, f);
  buf[n] = 0;
  fclose(f);

#if SNESRECOMP_STACK_BALANCE_DIAGNOSTICS
  ok = strstr(buf, "\"stack_balance\": [") != NULL &&
       strstr(buf, "diagnostic_gates_test") != NULL &&
       strstr(buf, "\"stack_balance_disabled\"") == NULL;
#else
  ok = strstr(buf, "\"stack_balance\": []") != NULL &&
       strstr(buf, "\"stack_balance_disabled\": true") != NULL;
#endif
  return ok;
}

int main(void) {
  int failures = 0;

  memset(&g_cpu, 0, sizeof(g_cpu));
  g_recomp_stack_top = 0;
  g_cpu.host_return_valid = 2;
  g_cpu.S = 0x01fd;
  RecompStackPush("diagnostic_gates_test");
  g_cpu_entry_s[g_recomp_stack_top - 1] = 0x01fd;
  g_cpu.S = 0x01fe;
  RecompStackPop();
  failures += check(g_recomp_stack_top == 0,
                    "functional recomp stack still pushes and pops");
  failures += check(json_matches_stack_balance_mode(),
                    "stack-balance dump matches diagnostic mode");

  g_recomp_snap_on_func = "diagnostic_gates_test";
  g_recomp_snap_count = 7;
  RecompStackPush("diagnostic_gates_test");
  RecompStackPop();
#if SNESRECOMP_FUNC_SNAPSHOT
  failures += check(g_recomp_snap_count == 8 && recomp_snap_lookup(8) != NULL,
                    "function snapshots capture when enabled");
#else
  failures += check(g_recomp_snap_count == 7 && recomp_snap_lookup(1) == NULL,
                    "function snapshots stay inert by default");
#endif

  if (failures)
    return 1;
  puts("diagnostic_gates_test: PASS");
  return 0;
}
