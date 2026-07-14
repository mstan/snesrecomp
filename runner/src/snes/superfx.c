/*
 * Nintendo GSU / Super FX LLE core.
 *
 * The instruction semantics and pixel/cache behavior were independently
 * adapted from the ISC-licensed ares implementation (ares team, Near et al),
 * commit 449b93716fb162632de2fd43bf2eba2064fa43f2.  This file is a C model
 * integrated with snesrecomp's cart bus and master-clock scheduler; it does
 * not include ares framework code.
 */
#include "superfx.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum {
  SFR_Z = 1u << 1, SFR_CY = 1u << 2, SFR_S = 1u << 3,
  SFR_OV = 1u << 4, SFR_G = 1u << 5, SFR_R = 1u << 6,
  SFR_ALT1 = 1u << 8, SFR_ALT2 = 1u << 9, SFR_B = 1u << 12,
  SFR_IRQ = 1u << 15,
};

static bool bit(uint16_t v, unsigned b) { return (v >> b) & 1; }
static void flag(SuperFx *f, uint16_t m, bool v) {
  if (v) f->sfr |= m; else f->sfr &= (uint16_t)~m;
}
static uint16_t rv(SuperFx *f, unsigned n) { return f->r[n & 15].data; }
static void wr(SuperFx *f, unsigned n, uint32_t v) {
  f->r[n & 15].data = (uint16_t)v;
  f->r[n & 15].modified = true;
}
static uint16_t sr(SuperFx *f) { return rv(f, f->sreg); }
static void wd(SuperFx *f, uint32_t v) { wr(f, f->dreg, v); }
static unsigned alt(SuperFx *f) { return (f->sfr >> 8) & 3; }
static unsigned scmr_md(SuperFx *f) { return f->scmr & 3; }
static unsigned scmr_ht(SuperFx *f) {
  return ((f->scmr >> 5) & 1) * 2 + ((f->scmr >> 2) & 1);
}
static bool scmr_ron(SuperFx *f) { return f->scmr & 0x10; }
static bool scmr_ran(SuperFx *f) { return f->scmr & 0x08; }

static void reset_prefix(SuperFx *f) {
  f->sfr &= (uint16_t)~(SFR_B | SFR_ALT1 | SFR_ALT2);
  f->sreg = f->dreg = 0;
}

static void step_clocks(SuperFx *f, unsigned clocks);
static uint8_t gsu_read(SuperFx *f, uint32_t address);
static void gsu_write(SuperFx *f, uint32_t address, uint8_t data);

static void step_clocks(SuperFx *f, unsigned clocks) {
  if (f->romcl) {
    unsigned n = clocks < f->romcl ? clocks : f->romcl;
    f->romcl -= n;
    if (!f->romcl) {
      f->sfr &= (uint16_t)~SFR_R;
      f->romdr = gsu_read(f, ((uint32_t)f->rombr << 16) | rv(f, 14));
    }
  }
  if (f->ramcl) {
    unsigned n = clocks < f->ramcl ? clocks : f->ramcl;
    f->ramcl -= n;
    if (!f->ramcl)
      gsu_write(f, 0x700000u + ((uint32_t)f->rambr << 16) + f->ramar,
                f->ramdr);
  }
  f->clock_credit -= clocks;
}

static uint8_t gsu_read(SuperFx *f, uint32_t a) {
  if ((a & 0xc00000u) == 0) {
    uint32_t p = (((a & 0x3f0000u) >> 1) | (a & 0x7fffu)) & f->rom_mask;
    return f->rom[p];
  }
  if ((a & 0xe00000u) == 0x400000u)
    return f->rom[a & f->rom_mask];
  if ((a & 0xfe0000u) == 0x700000u)
    return f->ram[a & f->ram_mask];
  return 0;
}

static void gsu_write(SuperFx *f, uint32_t a, uint8_t v) {
  if ((a & 0xfe0000u) == 0x700000u)
    f->ram[a & f->ram_mask] = v;
}

static void sync_rom(SuperFx *f) { if (f->romcl) step_clocks(f, f->romcl); }
static uint8_t read_rom_buffer(SuperFx *f) { sync_rom(f); return f->romdr; }
static void update_rom_buffer(SuperFx *f) {
  f->sfr |= SFR_R;
  f->romcl = f->clsr ? 5 : 6;
}
static void sync_ram(SuperFx *f) { if (f->ramcl) step_clocks(f, f->ramcl); }
static uint8_t read_ram_buffer(SuperFx *f, uint16_t a) {
  sync_ram(f);
  return gsu_read(f, 0x700000u + ((uint32_t)f->rambr << 16) + a);
}
static void write_ram_buffer(SuperFx *f, uint16_t a, uint8_t v) {
  sync_ram(f);
  f->ramcl = f->clsr ? 5 : 6;
  f->ramar = a;
  f->ramdr = v;
}

static void flush_cache(SuperFx *f) { memset(f->cache_valid, 0, sizeof(f->cache_valid)); }
static uint8_t read_cache(SuperFx *f, uint16_t a) {
  return f->cache[(uint16_t)(a + f->cbr) & 511];
}
static void write_cache(SuperFx *f, uint16_t a, uint8_t v) {
  a = (uint16_t)(a + f->cbr) & 511;
  f->cache[a] = v;
  if ((a & 15) == 15) f->cache_valid[a >> 4] = true;
}

static uint8_t read_opcode(SuperFx *f, uint16_t a) {
  uint16_t off = (uint16_t)(a - f->cbr);
  if (off < 512) {
    if (!f->cache_valid[off >> 4]) {
      uint16_t dp = off & 0xfff0;
      uint32_t sp = ((uint32_t)f->pbr << 16) | ((f->cbr + dp) & 0xfff0);
      for (unsigned n = 0; n < 16; n++) {
        step_clocks(f, f->clsr ? 5 : 6);
        f->cache[dp++] = gsu_read(f, sp++);
      }
      f->cache_valid[off >> 4] = true;
    } else step_clocks(f, f->clsr ? 1 : 2);
    return f->cache[off];
  }
  if (f->pbr <= 0x5f) {
    sync_rom(f);
    step_clocks(f, f->clsr ? 5 : 6);
    return gsu_read(f, ((uint32_t)f->pbr << 16) | a);
  }
  sync_ram(f);
  step_clocks(f, f->clsr ? 5 : 6);
  return gsu_read(f, ((uint32_t)f->pbr << 16) | a);
}

static uint8_t pipe(SuperFx *f) {
  uint8_t out = f->pipeline;
  wr(f, 15, rv(f, 15) + 1);
  f->pipeline = read_opcode(f, rv(f, 15));
  f->r[15].modified = false;
  return out;
}

static unsigned bpp(SuperFx *f) {
  static const uint8_t k[4] = {2, 4, 4, 8};
  return k[scmr_md(f)];
}
static uint32_t pixel_character(SuperFx *f, uint8_t x, uint8_t y) {
  switch ((f->por & 0x10) ? 3 : scmr_ht(f)) {
    case 0: return ((x & 0xf8) << 1) + ((y & 0xf8) >> 3);
    case 1: return ((x & 0xf8) << 1) + ((x & 0xf8) >> 1) + ((y & 0xf8) >> 3);
    case 2: return ((x & 0xf8) << 1) + (x & 0xf8) + ((y & 0xf8) >> 3);
    default:return ((y & 0x80) << 2) + ((x & 0x80) << 1) +
                  ((y & 0x78) << 1) + ((x & 0x78) >> 3);
  }
}
static uint32_t pixel_address(SuperFx *f, uint8_t x, uint8_t y) {
  return 0x700000u + pixel_character(f, x, y) * (bpp(f) << 3) +
         ((uint32_t)f->scbr << 10) + ((y & 7) * 2);
}
static void flush_pixel(SuperFx *f, SuperFxPixelCache *p) {
  if (!p->bitpend) return;
  uint8_t x0 = (uint8_t)(p->offset << 3), y = (uint8_t)(p->offset >> 5);
  uint32_t a = pixel_address(f, x0, y);
  for (unsigned n = 0; n < bpp(f); n++) {
    uint32_t by = ((n >> 1) << 4) + (n & 1);
    uint8_t d = 0;
    for (unsigned x = 0; x < 8; x++) d |= ((p->data[x] >> n) & 1) << x;
    if (p->bitpend != 0xff) {
      step_clocks(f, f->clsr ? 5 : 6);
      d = (d & p->bitpend) | (gsu_read(f, a + by) & (uint8_t)~p->bitpend);
    }
    step_clocks(f, f->clsr ? 5 : 6);
    gsu_write(f, a + by, d);
  }
  p->bitpend = 0;
}
static uint8_t color(SuperFx *f, uint8_t s) {
  if (f->por & 4) return (f->colr & 0xf0) | (s >> 4);
  if (f->por & 8) return (f->colr & 0xf0) | (s & 15);
  return s;
}
static void plot(SuperFx *f, uint8_t x, uint8_t y) {
  if (!(f->por & 1)) {
    if (scmr_md(f) == 3) {
      if ((f->por & 8) ? !(f->colr & 15) : !f->colr) return;
    } else if (!(f->colr & 15)) return;
  }
  uint8_t c = f->colr;
  if ((f->por & 2) && scmr_md(f) != 3) {
    if ((x ^ y) & 1) c >>= 4;
    c &= 15;
  }
  uint16_t off = (uint16_t)((y << 5) + (x >> 3));
  if (off != f->pixel[0].offset) {
    flush_pixel(f, &f->pixel[1]);
    f->pixel[1] = f->pixel[0];
    f->pixel[0].bitpend = 0;
    f->pixel[0].offset = off;
  }
  x = (x & 7) ^ 7;
  f->pixel[0].data[x] = c;
  f->pixel[0].bitpend |= 1u << x;
  if (f->pixel[0].bitpend == 0xff) {
    flush_pixel(f, &f->pixel[1]);
    f->pixel[1] = f->pixel[0];
    f->pixel[0].bitpend = 0;
  }
}
static uint8_t rpix(SuperFx *f, uint8_t x, uint8_t y) {
  flush_pixel(f, &f->pixel[1]); flush_pixel(f, &f->pixel[0]);
  uint32_t a = pixel_address(f, x, y);
  uint8_t d = 0; x = (x & 7) ^ 7;
  for (unsigned n = 0; n < bpp(f); n++) {
    uint32_t by = ((n >> 1) << 4) + (n & 1);
    step_clocks(f, f->clsr ? 5 : 6);
    d |= ((gsu_read(f, a + by) >> x) & 1) << n;
  }
  return d;
}

static void set_sz(SuperFx *f, uint16_t v) {
  flag(f, SFR_S, v & 0x8000); flag(f, SFR_Z, v == 0);
}

static void instruction(SuperFx *f, uint8_t op) {
  unsigned n = op & 15, a = alt(f);
  if (op == 0x00) {
    if (!(f->cfgr & 0x80)) { f->sfr |= SFR_IRQ; f->irq_pending = true; }
    f->sfr &= (uint16_t)~SFR_G; f->pipeline = 1; reset_prefix(f); return;
  }
  if (op == 0x01) { reset_prefix(f); return; }
  if (op == 0x02) {
    uint16_t c = rv(f,15) & 0xfff0; if (f->cbr != c) { f->cbr=c; flush_cache(f); }
    reset_prefix(f); return;
  }
  if (op == 0x03) { flag(f,SFR_CY,sr(f)&1); wd(f,sr(f)>>1); set_sz(f,rv(f,f->dreg)); reset_prefix(f); return; }
  if (op == 0x04) { bool c=sr(f)&0x8000; wd(f,(sr(f)<<1)|bit(f->sfr,2)); set_sz(f,rv(f,f->dreg)); flag(f,SFR_CY,c); reset_prefix(f); return; }
  if (op >= 0x05 && op <= 0x0f) {
    bool take = op==5 || (op==6 && bit(f->sfr,3)==bit(f->sfr,4)) ||
      (op==7 && bit(f->sfr,3)!=bit(f->sfr,4)) || (op==8 && !bit(f->sfr,1)) ||
      (op==9 && bit(f->sfr,1)) || (op==0x0a && !bit(f->sfr,3)) ||
      (op==0x0b && bit(f->sfr,3)) || (op==0x0c && !bit(f->sfr,2)) ||
      (op==0x0d && bit(f->sfr,2)) || (op==0x0e && !bit(f->sfr,4)) ||
      (op==0x0f && bit(f->sfr,4));
    int8_t d=(int8_t)pipe(f); if(take) wr(f,15,rv(f,15)+d); return;
  }
  if (op >= 0x10 && op <= 0x1f) { if (!(f->sfr&SFR_B)) f->dreg=n; else { wr(f,n,sr(f)); reset_prefix(f); } return; }
  if (op >= 0x20 && op <= 0x2f) { f->sreg=f->dreg=n; f->sfr|=SFR_B; return; }
  if (op >= 0x30 && op <= 0x3b) { f->ramaddr=rv(f,n); write_ram_buffer(f,f->ramaddr,(uint8_t)sr(f)); if(!(f->sfr&SFR_ALT1)) write_ram_buffer(f,f->ramaddr^1,(uint8_t)(sr(f)>>8)); reset_prefix(f); return; }
  if (op == 0x3c) { wr(f,12,rv(f,12)-1); set_sz(f,rv(f,12)); if(!bit(f->sfr,1)) wr(f,15,rv(f,13)); reset_prefix(f); return; }
  if (op == 0x3d) { f->sfr=(f->sfr&~SFR_B)|SFR_ALT1; return; }
  if (op == 0x3e) { f->sfr=(f->sfr&~SFR_B)|SFR_ALT2; return; }
  if (op == 0x3f) { f->sfr=(f->sfr&~SFR_B)|SFR_ALT1|SFR_ALT2; return; }
  if (op >= 0x40 && op <= 0x4b) { f->ramaddr=rv(f,n); uint16_t v=read_ram_buffer(f,f->ramaddr); if(!(f->sfr&SFR_ALT1)) v|=(uint16_t)read_ram_buffer(f,f->ramaddr^1)<<8; wd(f,v); reset_prefix(f); return; }
  if (op == 0x4c) { if(!(f->sfr&SFR_ALT1)){ plot(f,(uint8_t)rv(f,1),(uint8_t)rv(f,2)); wr(f,1,rv(f,1)+1); } else { wd(f,rpix(f,(uint8_t)rv(f,1),(uint8_t)rv(f,2))); set_sz(f,rv(f,f->dreg)); } reset_prefix(f); return; }
  if (op == 0x4d) { wd(f,(sr(f)>>8)|(sr(f)<<8)); set_sz(f,rv(f,f->dreg)); reset_prefix(f); return; }
  if (op == 0x4e) { if(!(f->sfr&SFR_ALT1)) f->colr=color(f,(uint8_t)sr(f)); else f->por=(uint8_t)sr(f)&0x1f; reset_prefix(f); return; }
  if (op == 0x4f) { wd(f,~sr(f)); set_sz(f,rv(f,f->dreg)); reset_prefix(f); return; }
  if (op >= 0x50 && op <= 0x5f) { uint16_t q=(f->sfr&SFR_ALT2)?n:rv(f,n); uint32_t z=(uint32_t)sr(f)+q+((f->sfr&SFR_ALT1)&&bit(f->sfr,2)); flag(f,SFR_OV,(~(sr(f)^q)&(q^z)&0x8000)!=0); flag(f,SFR_CY,z>=0x10000); wd(f,z); set_sz(f,rv(f,f->dreg)); reset_prefix(f); return; }
  if (op >= 0x60 && op <= 0x6f) { uint16_t q=(!(f->sfr&SFR_ALT2)||(f->sfr&SFR_ALT1))?rv(f,n):n; int32_t z=(int32_t)sr(f)-q-((!(f->sfr&SFR_ALT2)&&(f->sfr&SFR_ALT1)&&!bit(f->sfr,2))?1:0); flag(f,SFR_OV,((sr(f)^q)&(sr(f)^z)&0x8000)!=0); flag(f,SFR_CY,z>=0); flag(f,SFR_S,z&0x8000); flag(f,SFR_Z,(uint16_t)z==0); if(!(f->sfr&SFR_ALT2)||!(f->sfr&SFR_ALT1)) wd(f,z); reset_prefix(f); return; }
  if (op == 0x70) { wd(f,(rv(f,7)&0xff00)|(rv(f,8)>>8)); uint16_t v=rv(f,f->dreg); flag(f,SFR_OV,v&0xc0c0); flag(f,SFR_S,v&0x8080); flag(f,SFR_CY,v&0xe0e0); flag(f,SFR_Z,v&0xf0f0); reset_prefix(f); return; }
  if (op >= 0x71 && op <= 0x7f) { uint16_t q=(f->sfr&SFR_ALT2)?n:rv(f,n); wd(f,sr(f)&((f->sfr&SFR_ALT1)?~q:q)); set_sz(f,rv(f,f->dreg)); reset_prefix(f); return; }
  if (op >= 0x80 && op <= 0x8f) { uint16_t q=(f->sfr&SFR_ALT2)?n:rv(f,n); uint16_t z=(f->sfr&SFR_ALT1)?(uint8_t)sr(f)*(uint8_t)q:(int8_t)sr(f)*(int8_t)q; wd(f,z); set_sz(f,z); reset_prefix(f); if(!(f->cfgr&0x20)) step_clocks(f,f->clsr?1:2); return; }
  if (op == 0x90) { write_ram_buffer(f,f->ramaddr,(uint8_t)sr(f)); write_ram_buffer(f,f->ramaddr^1,(uint8_t)(sr(f)>>8)); reset_prefix(f); return; }
  if (op >= 0x91 && op <= 0x94) { wr(f,11,rv(f,15)+n); reset_prefix(f); return; }
  if (op == 0x95) { wd(f,(int8_t)sr(f)); set_sz(f,rv(f,f->dreg)); reset_prefix(f); return; }
  if (op == 0x96) { flag(f,SFR_CY,sr(f)&1); wd(f,(uint16_t)((int16_t)sr(f)>>1)+((f->sfr&SFR_ALT1)?((sr(f)+1)>>16):0)); set_sz(f,rv(f,f->dreg)); reset_prefix(f); return; }
  if (op == 0x97) { bool c=sr(f)&1; wd(f,(bit(f->sfr,2)<<15)|(sr(f)>>1)); set_sz(f,rv(f,f->dreg)); flag(f,SFR_CY,c); reset_prefix(f); return; }
  if (op >= 0x98 && op <= 0x9d) { if(!(f->sfr&SFR_ALT1)) wr(f,15,rv(f,n)); else { f->pbr=rv(f,n)&0x7f; wr(f,15,sr(f)); f->cbr=rv(f,15)&0xfff0; flush_cache(f); } reset_prefix(f); return; }
  if (op == 0x9e) { wd(f,sr(f)&0xff); flag(f,SFR_S,rv(f,f->dreg)&0x80); flag(f,SFR_Z,rv(f,f->dreg)==0); reset_prefix(f); return; }
  if (op == 0x9f) { int32_t z=(int16_t)sr(f)*(int16_t)rv(f,6); if(f->sfr&SFR_ALT1) wr(f,4,z); wd(f,z>>16); set_sz(f,rv(f,f->dreg)); flag(f,SFR_CY,z&0x8000); reset_prefix(f); step_clocks(f,((f->cfgr&0x20)?3:7)*(f->clsr?1:2)); return; }
  if (op >= 0xa0 && op <= 0xaf) { if(f->sfr&SFR_ALT1){ f->ramaddr=(uint16_t)pipe(f)<<1; wr(f,n,read_ram_buffer(f,f->ramaddr)|((uint16_t)read_ram_buffer(f,f->ramaddr^1)<<8)); } else if(f->sfr&SFR_ALT2){ f->ramaddr=(uint16_t)pipe(f)<<1; write_ram_buffer(f,f->ramaddr,(uint8_t)rv(f,n)); write_ram_buffer(f,f->ramaddr^1,(uint8_t)(rv(f,n)>>8)); } else wr(f,n,(int8_t)pipe(f)); reset_prefix(f); return; }
  if (op >= 0xb0 && op <= 0xbf) { if(!(f->sfr&SFR_B)) f->sreg=n; else { wd(f,rv(f,n)); flag(f,SFR_OV,rv(f,f->dreg)&0x80); set_sz(f,rv(f,f->dreg)); reset_prefix(f); } return; }
  if (op == 0xc0) { wd(f,sr(f)>>8); flag(f,SFR_S,rv(f,f->dreg)&0x80); flag(f,SFR_Z,rv(f,f->dreg)==0); reset_prefix(f); return; }
  if (op >= 0xc1 && op <= 0xcf) { uint16_t q=(f->sfr&SFR_ALT2)?n:rv(f,n); wd(f,(f->sfr&SFR_ALT1)?(sr(f)^q):(sr(f)|q)); set_sz(f,rv(f,f->dreg)); reset_prefix(f); return; }
  if (op >= 0xd0 && op <= 0xde) { wr(f,n,rv(f,n)+1); set_sz(f,rv(f,n)); reset_prefix(f); return; }
  if (op == 0xdf) { if(!(f->sfr&SFR_ALT2)) f->colr=color(f,read_rom_buffer(f)); else if(!(f->sfr&SFR_ALT1)){ sync_ram(f); f->rambr=sr(f)&1; } else { sync_rom(f); f->rombr=sr(f)&0x7f; } reset_prefix(f); return; }
  if (op >= 0xe0 && op <= 0xee) { wr(f,n,rv(f,n)-1); set_sz(f,rv(f,n)); reset_prefix(f); return; }
  if (op == 0xef) { uint8_t q=read_rom_buffer(f); switch(a){case 0:wd(f,q);break;case 1:wd(f,(q<<8)|(uint8_t)sr(f));break;case 2:wd(f,(sr(f)&0xff00)|q);break;default:wd(f,(int8_t)q);break;} reset_prefix(f); return; }
  if (op >= 0xf0) { if(f->sfr&SFR_ALT1){ f->ramaddr=pipe(f); f->ramaddr|=(uint16_t)pipe(f)<<8; wr(f,n,read_ram_buffer(f,f->ramaddr)|((uint16_t)read_ram_buffer(f,f->ramaddr^1)<<8)); } else if(f->sfr&SFR_ALT2){ f->ramaddr=pipe(f); f->ramaddr|=(uint16_t)pipe(f)<<8; write_ram_buffer(f,f->ramaddr,(uint8_t)rv(f,n)); write_ram_buffer(f,f->ramaddr^1,(uint8_t)(rv(f,n)>>8)); } else { uint8_t lo=pipe(f); wr(f,n,lo|((uint16_t)pipe(f)<<8)); } reset_prefix(f); return; }
}

static void run_one(SuperFx *f) {
  if (!(f->sfr & SFR_G)) { step_clocks(f, 6); return; }
  uint8_t op=f->pipeline;
  uint64_t sequence=++f->instruction_count;
  SuperFxTraceEntry *trace=&f->trace[sequence&255];
  trace->sequence=sequence;
  trace->r15=rv(f,15);
  trace->sfr=f->sfr;
  trace->pbr=f->pbr;
  trace->opcode=op;
  f->pipeline=read_opcode(f,rv(f,15));
  f->r[15].modified=false;
  instruction(f,op);
  if(f->r[14].modified){ f->r[14].modified=false; update_rom_buffer(f); }
  if(f->r[15].modified) f->r[15].modified=false; else wr(f,15,rv(f,15)+1), f->r[15].modified=false;
}

SuperFx *superfx_create(uint8_t *rom, uint32_t rom_size, uint8_t *ram, uint32_t ram_size) {
  SuperFx *f=(SuperFx*)calloc(1,sizeof(*f)); if(!f) return NULL;
  f->rom=rom; f->rom_size=rom_size; f->rom_mask=rom_size-1;
  f->ram=ram; f->ram_size=ram_size; f->ram_mask=ram_size-1;
  superfx_reset(f); return f;
}
void superfx_destroy(SuperFx *f) { free(f); }
void superfx_reset(SuperFx *f) {
  uint8_t *rom=f->rom,*ram=f->ram; uint32_t rs=f->rom_size,rm=f->ram_size;
  memset(f,0,sizeof(*f)); f->rom=rom;f->rom_size=rs;f->rom_mask=rs-1;f->ram=ram;f->ram_size=rm;f->ram_mask=rm-1;
  f->vcr=4; f->pipeline=1; f->pixel[0].offset=f->pixel[1].offset=UINT16_MAX;
}
void superfx_sync(SuperFx *f, uint64_t master) {
  if(!f) return;
  if(master < f->master_clock){ f->master_clock=master; f->clock_credit=0; return; }
  f->clock_credit += (int64_t)(master-f->master_clock); f->master_clock=master;
  /* Six clocks is the longest idle quantum and the normal uncached access. */
  unsigned guard=0; while(f->clock_credit>=6 && guard++<2000000) run_one(f);
}

uint8_t superfx_cpu_read_io(SuperFx *f, uint16_t a) {
  a=0x3000|(a&0x3ff);
  if(a>=0x3100) return read_cache(f,a-0x3100);
  if(a<=0x301f) return (uint8_t)(rv(f,(a>>1)&15)>>((a&1)*8));
  switch(a){
    case 0x3030:return (uint8_t)f->sfr;
    case 0x3031:{uint8_t v=(uint8_t)(f->sfr>>8);f->sfr&=~SFR_IRQ;f->irq_pending=false;return v;}
    case 0x3034:return f->pbr; case 0x3036:return f->rombr; case 0x303b:return f->vcr;
    case 0x303c:return f->rambr; case 0x303e:return (uint8_t)f->cbr; case 0x303f:return (uint8_t)(f->cbr>>8);
    default:return 0;
  }
}
void superfx_cpu_write_io(SuperFx *f, uint16_t a, uint8_t v) {
  a=0x3000|(a&0x3ff);
  if(a>=0x3100){write_cache(f,a-0x3100,v);return;}
  if(a<=0x301f){unsigned n=(a>>1)&15;uint16_t q=rv(f,n);wr(f,n,(a&1)?((q&255)|(v<<8)):((q&0xff00)|v));if(n==14)update_rom_buffer(f);if(a==0x301f)f->sfr|=SFR_G;return;}
  switch(a){
    case 0x3030:{bool g=f->sfr&SFR_G;f->sfr=(f->sfr&0xff00)|v;if(g&&!(f->sfr&SFR_G)){f->cbr=0;flush_cache(f);}}break;
    case 0x3031:f->sfr=(f->sfr&0x00ff)|((uint16_t)v<<8);break;
    case 0x3033:f->bramr=v&1;break; case 0x3034:f->pbr=v&0x7f;flush_cache(f);break;
    case 0x3037:f->cfgr=v&0xa0;break; case 0x3038:f->scbr=v;break;
    case 0x3039:f->clsr=v&1;break; case 0x303a:f->scmr=v&0x3f;break;
  }
}
uint8_t superfx_cpu_read_rom(SuperFx *f,uint32_t a,uint8_t open){
  (void)open;
  if((f->sfr&SFR_G)&&scmr_ron(f)){static const uint8_t v[16]={0,1,0,1,4,1,0,1,0,1,8,1,0,1,12,1};return v[a&15];}
  return f->rom[a&f->rom_mask];
}
uint8_t superfx_cpu_read_ram(SuperFx *f,uint32_t a,uint8_t open){return ((f->sfr&SFR_G)&&scmr_ran(f))?open:f->ram[a&f->ram_mask];}
void superfx_cpu_write_ram(SuperFx *f,uint32_t a,uint8_t v){f->ram[a&f->ram_mask]=v;}
