// A title's name from its control data (NACP), used by the module and the app.
//
// Since 21.0.0 a NACP can store its names compressed: when the byte at 0x3215
// (TitlesDataFormat) is 1, the 0x3000-byte title area holds a u16 size and a
// DEFLATE stream (raw, wbits -15) that inflates to 32 entries of 0x300 bytes.
// libnx's nacpGetLanguageEntry() doesn't know this yet and returns the
// compressed bytes as the "name" - Graveyard Keeper 2 showed up as "‚".
// Plain C, also included from C++.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define NACP_TITLE_AREA     0x3000
#define NACP_FORMAT_OFFSET  0x3215
#define NACP_ENTRY_SIZE     0x300   // name 0x200 + publisher 0x100
#define NACP_ENTRIES_MAX    32

// Letters and digits of the scripts a title name can be in.
static inline bool nacp_letter_like(uint32_t c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) ||  // Latin-1 / Latin Extended
           (c >= 0x370 && c <= 0x52F) ||                           // Greek, Cyrillic
           (c >= 0x590 && c <= 0x6FF) ||                           // Hebrew, Arabic
           (c >= 0xE00 && c <= 0xE7F) ||                           // Thai
           (c >= 0x1E00 && c <= 0x1FFF) ||                         // Latin / Greek extended
           (c >= 0x3040 && c <= 0x30FF) ||                         // Hiragana, Katakana
           (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) ||  // CJK
           (c >= 0xAC00 && c <= 0xD7AF) ||                         // Hangul
           (c >= 0xFF10 && c <= 0xFF19) || (c >= 0xFF21 && c <= 0xFF3A) ||
           (c >= 0xFF41 && c <= 0xFF5A) || (c >= 0xFF66 && c <= 0xFF9F);  // full / half width
}

// Letters and digits in a string of at most `max` bytes; -1 when it isn't
// valid UTF-8 (then it isn't a name, it's binary data).
static inline int nacp_count_letters(const char *s, size_t max) {
    const unsigned char *p = (const unsigned char *)s, *end = p + max;
    int letters = 0;
    while (p < end && *p) {
        uint32_t c = *p++;
        int extra = 0;
        if (c >= 0x80) {
            if (c >= 0xC2 && c <= 0xDF)      { extra = 1; c &= 0x1F; }
            else if (c >= 0xE0 && c <= 0xEF) { extra = 2; c &= 0x0F; }
            else if (c >= 0xF0 && c <= 0xF4) { extra = 3; c &= 0x07; }
            else return -1;
        }
        for (int i = 0; i < extra; i++, p++) {
            if (p >= end || (*p & 0xC0) != 0x80) return -1;
            c = (c << 6) | (*p & 0x3F);
        }
        if ((extra == 2 && c < 0x800) || (extra == 3 && (c < 0x10000 || c > 0x10FFFF)) ||
            (c >= 0xD800 && c <= 0xDFFF))
            return -1;  // overlong, out of range or a surrogate
        if (nacp_letter_like(c)) letters++;
    }
    return letters;
}

// Inflates `in` into `out` (raw DEFLATE, or a zlib-wrapped stream, which some
// tools write); returns the number of bytes produced, 0 on failure.
static inline size_t nacp_inflate(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len) {
    for (int attempt = 0; attempt < 2; attempt++) {
        z_stream z;
        memset(&z, 0, sizeof(z));
        if (inflateInit2(&z, attempt == 0 ? -15 : 15) != Z_OK) continue;
        z.next_in = (Bytef *)in;
        z.avail_in = (uInt)in_len;
        z.next_out = out;
        z.avail_out = (uInt)out_len;
        int rc = inflate(&z, Z_FINISH);
        size_t got = out_len - z.avail_out;
        inflateEnd(&z);
        // a complete stream, or one that filled all 32 entries
        if ((rc == Z_STREAM_END || (rc == Z_BUF_ERROR && z.avail_out == 0)) && got >= NACP_ENTRY_SIZE)
            return got;
    }
    return 0;
}

// NACP language slot for the console's language, -1 if unknown.
// set_language: the SetLanguage value (-1 if it couldn't be made), code: the
// raw language code (for languages newer than libnx's SetLanguage).
static inline int nacp_slot_for_language(int set_language, uint64_t code) {
    // SetLanguage order: JA ENUS FR DE IT ES ZHCN KO NL PT RU ZHTW ENGB FRCA ES419 ZHHANS ZHHANT PTBR
    static const signed char slot[] = { 2, 0, 3, 4, 7, 6, 14, 12, 8, 10, 11, 13, 1, 9, 5, 14, 13, 15 };
    if (set_language >= 0 && set_language < (int)sizeof(slot)) return slot[set_language];
    char s[9] = { 0 };
    memcpy(s, &code, 8);
    if (!strcmp(s, "pl")) return 16;  // [21.0.0+] Polish
    if (!strcmp(s, "th")) return 17;  // [21.0.0+] Thai
    return -1;
}

// Copies the title's name into `out`: the console-language entry, else
// English, else the first entry that is a real name. `nacp` points at the
// 0x4000-byte NACP, `lang_slot` comes from nacp_slot_for_language().
// Returns false when no entry holds usable text.
static inline bool nacp_title_name(const void *nacp, int lang_slot, char *out, size_t out_size) {
    const uint8_t *n = (const uint8_t *)nacp;
    const uint8_t *entries = n;
    int count = 16;
    uint8_t *inflated = NULL;
    if (n[NACP_FORMAT_OFFSET] == 1) {
        uint16_t size = (uint16_t)(n[0] | (n[1] << 8));
        if (size > NACP_TITLE_AREA - 2) size = NACP_TITLE_AREA - 2;
        inflated = (uint8_t *)calloc(NACP_ENTRIES_MAX, NACP_ENTRY_SIZE);
        size_t got = inflated ? nacp_inflate(n + 2, size, inflated, NACP_ENTRIES_MAX * NACP_ENTRY_SIZE) : 0;
        if (!got) {
            free(inflated);
            return false;  // compressed, but not a stream we can read: no name
        }
        entries = inflated;
        count = (int)(got / NACP_ENTRY_SIZE);
    }

    // console language, both Englishes, then every slot in order
    int order[2 + 3 + NACP_ENTRIES_MAX], k = 0;
    if (lang_slot >= 0) order[k++] = lang_slot;
    order[k++] = 0;
    order[k++] = 1;
    for (int i = 0; i < count; i++) order[k++] = i;

    const char *name = NULL, *plain = NULL;  // plain: valid text without letters, a last resort
    for (int i = 0; i < k && !name; i++) {
        if (order[i] >= count) continue;
        const char *e = (const char *)entries + order[i] * NACP_ENTRY_SIZE;
        if (!e[0]) continue;
        int letters = nacp_count_letters(e, 0x200);
        if (letters > 0) name = e;
        else if (letters == 0 && !plain) plain = e;
    }
    if (!name) name = plain;
    if (name) {
        size_t len = strnlen(name, 0x200);
        if (len >= out_size) len = out_size - 1;
        memcpy(out, name, len);
        out[len] = '\0';
    }
    free(inflated);
    return name != NULL;
}
