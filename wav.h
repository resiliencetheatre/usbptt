#ifndef WAV_H
#define WAV_H
#include <stdint.h>
#include <stdio.h>
/* Mono, 16000 Hz, S16_LE. Rewrites the header without closing the stream. */
int wav_header(FILE *file, uint32_t bytes);
#endif
