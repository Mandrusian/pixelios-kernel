#pragma once
/* freestanding: the little of stdlib that qrcodegen.c uses */
static inline int abs(int x) { return x < 0 ? -x : x; }
static inline long labs(long x) { return x < 0 ? -x : x; }
