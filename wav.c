#include "wav.h"
#include <string.h>

static void le16(unsigned char *p, uint16_t n)
{
    p[0] = (unsigned char)n;
    p[1] = (unsigned char)(n >> 8);
}

static void le32(unsigned char *p, uint32_t n)
{
    le16(p, (uint16_t)n);
    le16(p + 2, (uint16_t)(n >> 16));
}

int wav_header(FILE *file, uint32_t bytes)
{
    unsigned char h[44] = {0};
    memcpy(h, "RIFF", 4);
    le32(h + 4, 36 + bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    le32(h + 16, 16);
    le16(h + 20, 1);
    le16(h + 22, 1);
    le32(h + 24, 16000);
    le32(h + 28, 32000);
    le16(h + 32, 2);
    le16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    le32(h + 40, bytes);
    if (fseek(file, 0, SEEK_SET) || fwrite(h, 1, sizeof h, file) != sizeof h)
        return -1;
    return fflush(file);
}
