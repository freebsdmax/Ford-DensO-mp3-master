#include "player_tags.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "esp_log.h"

static const char *TAG = "Tags";

#define TAG_SCAN_BYTES 16384

static void trim(char *s)
{
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0) {
        char c = s[n - 1];
        if (c == ' ' || c == '\r' || c == '\n' || c == '\t' || c == '\0') {
            s[--n] = '\0';
        } else {
            break;
        }
    }
    /* прибрати пробіли на початку */
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

static void utf16_to_utf8(const uint8_t *src, size_t bytes, char *dst, size_t cap)
{
    size_t o = 0;
    size_t i = 0;

    /* пропустити BOM, якщо він є */
    int little = 1;
    if (bytes >= 2 && src[0] == 0xFF && src[1] == 0xFE) { i = 2; little = 1; }
    else if (bytes >= 2 && src[0] == 0xFE && src[1] == 0xFF) { i = 2; little = 0; }

    while (i + 1 < bytes && o + 4 < cap) {
        uint32_t cp = little ? (uint32_t)(src[i] | (src[i + 1] << 8))
                             : (uint32_t)((src[i] << 8) | src[i + 1]);
        i += 2;
        if (cp == 0) break;

        /* сурогатні пари */
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < bytes) {
            uint32_t lo = little ? (uint32_t)(src[i] | (src[i + 1] << 8))
                                 : (uint32_t)((src[i] << 8) | src[i + 1]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }

        if (cp < 0x80) {
            dst[o++] = (char)cp;
        } else if (cp < 0x800) {
            dst[o++] = (char)(0xC0 | (cp >> 6));
            dst[o++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            dst[o++] = (char)(0xE0 | (cp >> 12));
            dst[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            dst[o++] = (char)(0xF0 | (cp >> 18));
            dst[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            dst[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    dst[o] = '\0';
}

static void latin1_to_utf8(const uint8_t *src, size_t bytes, char *dst, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < bytes && o + 2 < cap; i++) {
        uint8_t c = src[i];
        if (c == 0) break;
        if (c < 0x80) {
            dst[o++] = (char)c;
        } else {
            dst[o++] = (char)(0xC0 | (c >> 6));
            dst[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    dst[o] = '\0';
}

static void copy_utf8(const uint8_t *src, size_t bytes, char *dst, size_t cap)
{
    size_t n = bytes;
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Декодує текст ID3-фрейму згідно з байтом кодування у першому байті. */
static void decode_text(const uint8_t *frame, size_t len, char *dst, size_t cap)
{
    if (!frame || len < 2 || !dst || cap == 0) {
        if (dst && cap) dst[0] = '\0';
        return;
    }
    uint8_t enc = frame[0];
    const uint8_t *txt = frame + 1;
    size_t n = len - 1;

    switch (enc) {
    case 0: latin1_to_utf8(txt, n, dst, cap); break;
    case 1: utf16_to_utf8(txt, n, dst, cap); break;
    case 2: utf16_to_utf8(txt, n, dst, cap); break;   /* BE: BOM відсутній, старший байт перший */
    case 3: copy_utf8(txt, n, dst, cap); break;
    default: latin1_to_utf8(txt, n, dst, cap); break;
    }
    trim(dst);
}

static uint32_t syncsafe(const uint8_t *p)
{
    return ((uint32_t)(p[0] & 0x7F) << 21) |
           ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7) |
           (uint32_t)(p[3] & 0x7F);
}

static bool read_id3v2(FILE *f, char *title, size_t tcap, char *artist, size_t acap)
{
    uint8_t hdr[10];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) return false;
    if (memcmp(hdr, "ID3", 3) != 0) return false;

    const uint8_t major = hdr[3];
    uint32_t tag_size = syncsafe(&hdr[6]);
    if (tag_size == 0 || tag_size > TAG_SCAN_BYTES) tag_size = TAG_SCAN_BYTES;

    uint8_t *buf = malloc(tag_size);
    if (!buf) return false;
    size_t got = fread(buf, 1, tag_size, f);
    if (got < 10) {
        free(buf);
        return false;
    }

    bool found = false;
    size_t pos = 0;
    /* пропустити розширений заголовок, якщо є */
    if (hdr[5] & 0x40) {
        if (pos + 4 <= got) {
            uint32_t ext = (major >= 4) ? syncsafe(&buf[pos]) : ((uint32_t)buf[pos] << 24 | buf[pos+1] << 16 | buf[pos+2] << 8 | buf[pos+3]);
            pos += 4 + ext;
        }
    }

    while (pos + 10 <= got) {
        const uint8_t *fh = &buf[pos];
        if (fh[0] == 0) break;   /* padding */

        char id[5] = { (char)fh[0], (char)fh[1], (char)fh[2], (char)fh[3], 0 };
        uint32_t fsize = (major >= 4)
                             ? syncsafe(&fh[4])
                             : ((uint32_t)fh[4] << 24 | (uint32_t)fh[5] << 16 |
                                (uint32_t)fh[6] << 8 | (uint32_t)fh[7]);
        pos += 10;
        if (fsize == 0 || pos + fsize > got) break;

        if (strcmp(id, "TIT2") == 0 && title) {
            decode_text(&buf[pos], fsize, title, tcap);
            if (title[0]) found = true;
        } else if (strcmp(id, "TPE1") == 0 && artist) {
            decode_text(&buf[pos], fsize, artist, acap);
            if (artist[0]) found = true;
        }
        pos += fsize;
    }

    free(buf);
    return found;
}

static bool read_id3v1(FILE *f, char *title, size_t tcap, char *artist, size_t acap)
{
    uint8_t tag[128];
    if (fseek(f, -128, SEEK_END) != 0) return false;
    if (fread(tag, 1, sizeof(tag), f) != sizeof(tag)) return false;
    if (memcmp(tag, "TAG", 3) != 0) return false;

    bool found = false;
    if (title && tcap) {
        latin1_to_utf8(&tag[3], 30, title, tcap);
        trim(title);
        if (title[0]) found = true;
    }
    if (artist && acap) {
        latin1_to_utf8(&tag[33], 30, artist, acap);
        trim(artist);
        if (artist[0]) found = true;
    }
    return found;
}

bool player_tags_read(const char *path,
                      char *title, size_t title_len,
                      char *artist, size_t artist_len)
{
    if (!path) return false;
    if (title && title_len) title[0] = '\0';
    if (artist && artist_len) artist[0] = '\0';

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    bool ok = false;
    if (!(ok = read_id3v2(f, title, title_len, artist, artist_len))) {
        ok = read_id3v1(f, title, title_len, artist, artist_len);
    }
    fclose(f);

    if (ok) {
        ESP_LOGD(TAG, "%s -> '%s' / '%s'", path, title ? title : "", artist ? artist : "");
    }
    return ok;
}
