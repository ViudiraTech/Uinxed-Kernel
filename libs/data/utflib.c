/*
 *
 *      utflib.c
 *      UTF encoding library
 *
 *      2025/10/7 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <utflib.h>

/* lookup table for the number of bytes expected in a sequence */
const uint8_t utftab[64] = {
    0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, // 1100xxxx
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, // 1101xxxx
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, // 1110xxxx
    4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 0, 0, // 1111xxxx
};

/* Check if the rune is a valid Unicode character */
int isvalidrune(rune_t r)
{
    if (r < 0) return 0;                      // negative value
    if (r >= 0xd800 && r <= 0xdfff) return 0; // surrogate pair range
    if (r >= 0xfdd0 && r <= 0xfdef) return 0; // non-character range
    if ((r & 0xfffe) == 0xfffe) return 0;     // non-character at end of plane
    if (r > 0x10ffff) return 0;               // too large
    return 1;
}

/* Convert a UTF-8 sequence to a Unicode rune */
int charntorune(rune_t *p, const char *s, size_t len)
{
    rune_t  r;
    uint8_t c, i, m, n, x;

    if (!len) return 0; // can't even look at s[0]

    c = *s++;

    if (!(c & 0200)) return (*p = c, 1);         // basic byte
    if (!(c & 0100)) return (*p = Runeerror, 1); // continuation byte

    n = utftab[c & 077];

    if (!n) return (*p = Runeerror, 1);                  // illegal byte
    if (len == 1) return 0;                              // reached len limit
    if ((*s & 0300) != 0200) return (*p = Runeerror, 1); // not a continuation byte

    x = 0377 >> n;
    r = c & x;
    r = (r << 6) | (*s++ & 077);

    if (r <= x) return (*p = Runeerror, 2); // overlong sequence

    m = (len < n) ? len : n;

    for (i = 2; i < m; i++) {
        if ((*s & 0300) != 0200) return (*p = Runeerror, i); // not a continuation byte
        r = (r << 6) | (*s++ & 077);
    }

    if (i < n) return 0; // must have reached len limit
    if (!isvalidrune(r)) return (*p = Runeerror, i);

    return (*p = r, i);
}

/* Convert a single UTF-8 character to a Unicode rune */
int chartorune(rune_t *p, const char *s)
{
    return charntorune(p, s, UTFmax);
}

/* Get the number of Unicode characters in a UTF-8 string */
size_t utfnlen(const char *s, size_t len)
{
    uint8_t     c, i, m, n, x;
    const char *p;
    size_t      k;
    rune_t      r;

    for (k = 0; *(p = s) != '\0'; len -= s - p, k++) {
        if (!len) return k; // can't even look at s[0]

        c = *s++;

        if ((c & 0300) != 0300) continue; // not a leading byte

        n = utftab[c & 077];

        if (!n) continue;                  // illegal byte
        if (len == 1) return k;            // reached len limit
        if ((*s & 0300) != 0200) continue; // not a continuation byte

        x = 0377 >> n;
        r = c & x;
        r = (r << 6) | (*s++ & 077);

        if (r <= x) continue; // overlong sequence

        m = (len < n) ? len : n;

        for (i = 2; i < m; i++) {
            if ((*s & 0300) != 0200) break; // not a continuation byte
            s++;
        }

        if (i < m) continue; // broke out of loop early
        if (i < n) return k; // must have reached len limit
    }
    return k;
}

/* Get the number of Unicode characters in a UTF-8 string (without length limit) */
size_t utflen(const char *s)
{
    return utfnlen(s, (size_t)-1);
}
