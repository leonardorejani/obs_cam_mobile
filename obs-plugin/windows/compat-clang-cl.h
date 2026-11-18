// Forcado com /FI no build cruzado (clang-cl). O libobs usa _udiv128 (intrinseco do MSVC) que o clang-cl nao traz.
// Mesma semantica: divide o inteiro de 128 bits high:low por div e devolve quociente e resto (instrucao divq).
#pragma once
#if defined(__clang__) && defined(_M_X64) && !defined(FMCAM_TEM_UDIV128)
#define FMCAM_TEM_UDIV128 1
static inline unsigned __int64 _udiv128(unsigned __int64 high, unsigned __int64 low, unsigned __int64 div,
                                        unsigned __int64* rem) {
  unsigned __int64 q, r;
  __asm__("divq %4" : "=a"(q), "=d"(r) : "a"(low), "d"(high), "r"(div) : "cc");
  *rem = r;
  return q;
}
#endif
