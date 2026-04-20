#include "framedump.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0755)
#endif

FrameDumpCallback g_framedump_callback;

static char g_framedump_dir[512];

// --- CRC32 (standard polynomial) ---
static uint32_t s_crc32_table[256];
static int s_crc32_init = 0;

static void crc32_init_table(void) {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int j = 0; j < 8; j++)
      c = (c >> 1) ^ (c & 1 ? 0xEDB88320u : 0);
    s_crc32_table[i] = c;
  }
  s_crc32_init = 1;
}

static uint32_t crc32(const uint8_t *buf, size_t len) {
  if (!s_crc32_init) crc32_init_table();
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++)
    c = s_crc32_table[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

// --- WRAM field accessors (key SMW state) ---
// All little-endian reads from WRAM buffer (65816 native order)
static uint8_t  w8 (const uint8_t *w, uint32_t a) { return w[a]; }
static uint16_t w16(const uint8_t *w, uint32_t a) { return (uint16_t)(w[a] | (w[a+1] << 8)); }

static void write_json(const char *path, uint32_t frame, const uint8_t *wram) {
  uint32_t crc = crc32(wram, 0x20000);

  uint8_t  game_mode    = w8 (wram, 0x100);
  uint8_t  frame_ctr    = w8 (wram, 0x13);
  uint8_t  ow_process   = w8 (wram, 0x13D9);
  uint16_t ow_submap    = w16(wram, 0x13C3);
  uint16_t ow_player_x  = w16(wram, 0x1F17);
  uint16_t ow_player_y  = w16(wram, 0x1F19);
  uint8_t  ow_map       = w8 (wram, 0x1F11);

  FILE *f = fopen(path, "w");
  if (!f) return;
  fprintf(f,
    "{\n"
    "  \"frame\": %u,\n"
    "  \"game_mode\": %u,\n"
    "  \"frame_ctr\": %u,\n"
    "  \"ow_process\": %u,\n"
    "  \"ow_submap\": %u,\n"
    "  \"ow_player_x\": %u,\n"
    "  \"ow_player_y\": %u,\n"
    "  \"ow_map\": %u,\n"
    "  \"crc32_wram\": \"0x%08X\"\n"
    "}\n",
    frame, game_mode, frame_ctr,
    ow_process, ow_submap,
    ow_player_x, ow_player_y, ow_map,
    crc);
  fclose(f);
}

static void write_bin(const char *path, const uint8_t *wram) {
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fwrite(wram, 1, 0x20000, f);
  fclose(f);
}

static void framedump_callback(uint32_t frame, const uint8_t *wram) {
  if (!wram) return;
  char path[768];
  snprintf(path, sizeof(path), "%s/frame_%06u.json", g_framedump_dir, frame);
  write_json(path, frame, wram);
  snprintf(path, sizeof(path), "%s/frame_%06u_wram.bin", g_framedump_dir, frame);
  write_bin(path, wram);
}

void FrameDump_Init(const char *dir) {
  strncpy(g_framedump_dir, dir, sizeof(g_framedump_dir) - 1);
  MKDIR(dir);
  g_framedump_callback = framedump_callback;
  fprintf(stderr, "framedump: writing to '%s'\n", dir);
}
