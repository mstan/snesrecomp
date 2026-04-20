#pragma once
#include "types.h"

void recomp_hw_init(void);
void recomp_write_wram_port(uint16 reg, uint8 val);
uint8 recomp_read_wram_port(void);
void recomp_write_internal_reg(uint16 reg, uint8 val);
uint8 recomp_read_internal_reg(uint16 reg);
