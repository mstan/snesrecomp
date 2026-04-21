#ifndef SM_TYPES_H_
#define SM_TYPES_H_

#pragma warning(disable: 4244)

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

#if defined(_WIN32)
#include <crtdbg.h>
#ifndef assert
#define assert _ASSERTE
#endif
#else
#include <assert.h>
#endif

typedef uint8_t uint8;
typedef int8_t int8;
typedef uint16_t uint16;
typedef int16_t int16;
typedef uint32_t uint32;
typedef int32_t int32;
typedef uint64_t uint64;
typedef int64_t int64;
typedef unsigned int uint;

typedef uint16 VoidP;

#define arraysize(x) sizeof(x)/sizeof(x[0])

#ifdef _MSC_VER
#define countof _countof
#define NORETURN __declspec(noreturn)
#define FORCEINLINE __forceinline
#define NOINLINE __declspec(noinline)
#else
#define countof(a) (sizeof(a)/sizeof(*(a)))
#define NORETURN
#define FORCEINLINE inline
#define NOINLINE
#endif

#ifdef _DEBUG
#define kDebugFlag 1
#else
#define kDebugFlag 0
#endif

static FORCEINLINE int IntMin(int a, int b) { return a < b ? a : b; }
static FORCEINLINE int IntMax(int a, int b) { return a > b ? a : b; }

static inline uint16 swap16(uint16 v) { return (v << 8) | (v >> 8); }

void NORETURN Die(const char *error);
void Warning(const char *error);

#pragma pack(push, 1)
typedef struct LongPtr {
  VoidP addr;
  uint8 bank;
} LongPtr;
#pragma pack (pop)

typedef struct PairU16 {
  uint16 first, second;
} PairU16;

typedef struct RetAY {
  uint8 a, y;
} RetAY;

typedef struct RetY {
  uint8 y;
} RetY;

typedef struct PointU16 {
  uint16 x, y;
} PointU16;

typedef struct PointU8 {
  uint8 x, y;
} PointU8;

typedef struct OamEnt {
  uint8 xpos;
  uint8 ypos;
  uint8 charnum;
  uint8 flags;
} OamEnt;

// Dispatch table function-pointer typedefs.
// Each is the shape of a handler cast from a JSL/JSR dispatch site.
// The recompiler picks one based on the UNION of live-in registers across
// all handlers in a given dispatch table: if any handler reads Y at
// entry, all handlers in that table must be declared to accept `j`, and
// so on. Mixing shapes within one table would produce cast-mismatch UB,
// so the emitter upgrades the whole table to the widest shape needed.
typedef void FuncV(void);              // no live-in registers
typedef void FuncU8(uint8 kk);         // k only (X live-in)
typedef void FuncU8J(uint8 kk, uint8 jj);        // k + j (X + Y live-in)
typedef void FuncU8A(uint8 kk, uint8 aa);        // k + a (X + A live-in)
typedef void FuncU8JA(uint8 kk, uint8 jj, uint8 aa);  // k + j + a

// windows.h defines HIBYTE; undef and redefine as the byte-extract helper.
#ifdef HIBYTE
#undef HIBYTE
#endif
#define BYTEn(x, n)   (*((uint8*)&(x)+n))
#define HIBYTE(x)     BYTEn(x, 1)

// 16-bit pair composition: (hi << 8) | lo.
#define PAIR16(high, low) ((uint16)((high) << 8) | (uint8)(low))

static inline PairU16 MakePairU16(uint16 k, uint16 j) {
  PairU16 r = { k, j };
  return r;
}

typedef struct MemBlk {
  const uint8 *ptr;
  size_t size;
} MemBlk;
MemBlk FindIndexInMemblk(MemBlk data, size_t i);
const uint8 *FindAddrInMemblk(MemBlk data, uint32 addr);

#endif  // SM_TYPES_H_
