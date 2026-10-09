#include "wav.h"
#include <assert.h>
#include <string.h>

static void check(unsigned samples)
{
    FILE *f = tmpfile();
    assert(f);
    /* Explicit little-endian samples: zero, positive maximum, negative minimum. */
    const unsigned char pcm[] = {0, 0, 255, 127, 0, 128};
    unsigned char expected[] = {
        'R','I','F','F',36,0,0,0,'W','A','V','E','f','m','t',' ',
        16,0,0,0,1,0,1,0,128,62,0,0,0,125,0,0,2,0,16,0,
        'd','a','t','a',0,0,0,0
    };
    unsigned bytes = samples * 2;
    expected[4] += (unsigned char)bytes;
    expected[40] = (unsigned char)bytes;
    assert(wav_header(f, 0) == 0);
    assert(fwrite(pcm, 1, bytes, f) == bytes);
    assert(wav_header(f, bytes) == 0);
    assert(wav_header(f, bytes) == 0); /* Finalization is idempotent. */
    unsigned char actual[44 + sizeof pcm];
    rewind(f);
    assert(fread(actual, 1, sizeof actual, f) == 44 + bytes);
    assert(!memcmp(actual, expected, 44));
    assert(!memcmp(actual + 44, pcm, bytes));
    assert(feof(f) || fgetc(f) == EOF);
    assert(fclose(f) == 0);
}

int main(void)
{
    check(0);
    check(1);
    check(3);
    puts("WAV checks passed: empty, one sample, three samples; header and payload preserved.");
    return 0;
}
