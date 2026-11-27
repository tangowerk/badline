
#pragma once

#define _unreachable    __builtin_unreachable();
#define likely(x)	    __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)
#define _swapWord  __builtin_bswap16
#define _swapLong  __builtin_bswap32

#define LoByte(x)       ((x) & 0xff)
#define HiByte(x)       (((x) >> 8) & 0xff)
#define Word(x)         ((x) & 0xffff)
#define HiWord(x)       (((x) >> 16) & 0xffff)
#define LoByteHiWord(x) LoByte(HiWord(x))
#define HiByteHiWord(x) HiByte(HiWord(x))

#define LoNibble(x)     ((x) & 0xf)
#define HiNibble(x)     (((x) >> 4) & 0xf)
