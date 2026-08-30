#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int snes_text_xlate_init_c(const char* table_path, const char* language);
void snes_text_xlate_set_language_c(const char* language);
void snes_text_xlate_shutdown_c(void);
void snes_text_xlate_on_frame_c(void);
void snes_text_xlate_on_vram_write_c(uint16_t word_addr);
const char* snes_text_xlate_last_error_c(void);
int snes_text_xlate_debug_json_c(const char* subcmd, char* out, int cap);

#ifdef __cplusplus
}
#endif
