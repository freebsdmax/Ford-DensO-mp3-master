#include "player_playlist.h"
#include "player_pipeline.h"
#include "player_nvs.h"
#include "player_usb.h"
#include "esp_log.h"
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include "esp_err.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "Player";

char **g_playlist = NULL;
int g_playlist_size = 0;
bool g_playlist_ready = false;
int g_current_track = 0;

void add_to_playlist(const char *file_path) {
    if (g_playlist_size >= MAX_PLAYLIST_SIZE) {
        ESP_LOGW(TAG, "Playlist full, skipping: %s", file_path);
        return;
    }

    if (g_playlist_size == 0) {
        g_playlist = malloc(sizeof(char*));
    } else {
        g_playlist = realloc(g_playlist, (g_playlist_size + 1) * sizeof(char*));
    }

    if (!g_playlist) {
        ESP_LOGE(TAG, "Failed to allocate playlist");
        return;
    }

    g_playlist[g_playlist_size] = strdup(file_path);
    g_playlist_size++;
}

void clear_playlist(void) {
    if (g_playlist) {
        for (int i = 0; i < g_playlist_size; i++) {
            free(g_playlist[i]);
        }
        free(g_playlist);
        g_playlist = NULL;
    }
    g_playlist_size = 0;
    g_current_track = 0;
    g_playlist_ready = false;
}


void find_mp3_files_recursive(const char *path)
{
    struct dirent *d;
    DIR *dh = opendir(path);
    if (!dh) {
        ESP_LOGE(TAG, "❌ Не вдалося відкрити директорію: %s", path);
        return;
    }

    while ((d = readdir(dh)) != NULL) {
        if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0) {
            continue;
        }

        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, d->d_name);

        struct stat st;
        if (stat(full_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                find_mp3_files_recursive(full_path);
            } else if (S_ISREG(st.st_mode)) {
                const char *ext = strrchr(d->d_name, '.');
                if (ext && (strcasecmp(ext, ".mp3") == 0)) {
                    //ESP_LOGI(TAG, "🔍 Знайдено MP3 файл: %s", full_path);
                    add_to_playlist(full_path);
                }
            }
        }
    }
    closedir(dh);
}

void create_playlist_from_usb(void)
{
    clear_playlist();
    
    for (int i = 0; i < MAX_MSC_DEVICES; i++) {
        if (msc_devices[i]) {
            char mount_path[16];
            snprintf(mount_path, sizeof(mount_path), MNT_PATH "%d", i);
            ESP_LOGI(TAG, "🔍 Пошук MP3 файлів в %s", mount_path);
            find_mp3_files_recursive(mount_path);
        }
    }
    
    if (g_playlist_size > 0) {
        char dname[256];
        playlist_get_display_name(0, dname, sizeof(dname));
        ESP_LOGI(TAG, "Playlist sample: raw='%s' display='%s'", g_playlist[0], dname);
        g_playlist_ready = true;
        
        // 🔍 Перевірити, чи є збережена позиція для цього плейлиста
        if (g_saved_track_index >= 0 && g_saved_track_index < g_playlist_size) {
            g_current_track = g_saved_track_index;
            g_restore_playback = true;
            ESP_LOGI(TAG, "✅ Плейлист створено! Знайдено %d треків. Відновлюємо з треку %" PRId32, 
                     g_playlist_size, g_saved_track_index + 1);
        } else {
            g_current_track = 0;
            ESP_LOGI(TAG, "✅ Плейлист створено! Знайдено %d треків", g_playlist_size);
        }
        
        // 📨 Відправити подію про підключення USB
        //send_can_event(EVENT_USB_CONNECTED, g_playlist_size, 0x00);
    } else {
        ESP_LOGW(TAG, "⚠️ MP3 файлів не знайдено");
    }
}

/* CP866 (OEM Russian) -> UTF-8. FATFS returns names in the configured OEM codepage. */
static const uint16_t s_cp866[128] = {
    /* 0x80 */ 0x0410,0x0411,0x0412,0x0413,0x0414,0x0415,0x0416,0x0417,
                0x0418,0x0419,0x041A,0x041B,0x041C,0x041D,0x041E,0x041F,
    /* 0x90 */ 0x0420,0x0421,0x0422,0x0423,0x0424,0x0425,0x0426,0x0427,
                0x0428,0x0429,0x042A,0x042B,0x042C,0x042D,0x042E,0x042F,
    /* 0xA0 */ 0x0430,0x0431,0x0432,0x0433,0x0434,0x0435,0x0436,0x0437,
                0x0438,0x0439,0x043A,0x043B,0x043C,0x043D,0x043E,0x043F,
    /* 0xB0 */ 0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,
                0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510,
    /* 0xC0 */ 0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,
                0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,
    /* 0xD0 */ 0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B,
                0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580,
    /* 0xE0 */ 0x0440,0x0441,0x0442,0x0443,0x0444,0x0445,0x0446,0x0447,
                0x0448,0x0449,0x044A,0x044B,0x044C,0x044D,0x044E,0x044F,
    /* 0xF0 */ 0x0401,0x0451,0x0404,0x0454,0x0407,0x0457,0x040E,0x045E,
                0x00B0,0x2219,0x00B7,0x221A,0x2116,0x00A4,0x25A0,0x00A0
};

static bool utf8_is_valid(const unsigned char *s)
{
    int multibyte = 0;
    while (*s) {
        if (*s < 0x80) { s++; continue; }
        if ((*s & 0xE0) == 0xC0) {
            if ((s[1] & 0xC0) != 0x80) return false;
            multibyte = 1; s += 2;
        } else if ((*s & 0xF0) == 0xE0) {
            if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return false;
            multibyte = 1; s += 3;
        } else if ((*s & 0xF8) == 0xF0) {
            if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return false;
            multibyte = 1; s += 4;
        } else {
            return false;
        }
    }
    return multibyte != 0;
}

static const uint16_t s_cp1251[64] = {
    /* 0xC0 */ 0x0410,0x0411,0x0412,0x0413,0x0414,0x0415,0x0416,0x0417,
                0x0418,0x0419,0x041A,0x041B,0x041C,0x041D,0x041E,0x041F,
    /* 0xD0 */ 0x0420,0x0421,0x0422,0x0423,0x0424,0x0425,0x0426,0x0427,
                0x0428,0x0429,0x042A,0x042B,0x042C,0x042D,0x042E,0x042F,
    /* 0xE0 */ 0x0430,0x0431,0x0432,0x0433,0x0434,0x0435,0x0436,0x0437,
                0x0438,0x0439,0x043A,0x043B,0x043C,0x043D,0x043E,0x043F,
    /* 0xF0 */ 0x0440,0x0441,0x0442,0x0443,0x0444,0x0445,0x0446,0x0447,
                0x0448,0x0449,0x044A,0x044B,0x044C,0x044D,0x044E,0x044F
};

static uint16_t cp1251_special(unsigned char b)
{
    switch (b) {
        case 0x80: return 0x0402; case 0x81: return 0x0403; case 0x82: return 0x201A;
        case 0x83: return 0x0453; case 0x84: return 0x201E; case 0x85: return 0x2026;
        case 0x86: return 0x2020; case 0x87: return 0x2021; case 0x88: return 0x20AC;
        case 0x89: return 0x2030; case 0x8A: return 0x0409; case 0x8B: return 0x2039;
        case 0x8C: return 0x040A; case 0x8D: return 0x040C; case 0x8E: return 0x040B;
        case 0x8F: return 0x040F; case 0x90: return 0x0452; case 0x91: return 0x2018;
        case 0x92: return 0x2019; case 0x93: return 0x201C; case 0x94: return 0x201D;
        case 0x95: return 0x2022; case 0x96: return 0x2013; case 0x97: return 0x2014;
        case 0x99: return 0x2122; case 0x9A: return 0x0459; case 0x9B: return 0x203A;
        case 0x9C: return 0x045A; case 0x9D: return 0x045C; case 0x9E: return 0x045B;
        case 0x9F: return 0x045F; case 0xA0: return 0x00A0; case 0xA1: return 0x040E;
        case 0xA2: return 0x045E; case 0xA3: return 0x0408; case 0xA4: return 0x00A4;
        case 0xA5: return 0x0490; case 0xA6: return 0x00A6; case 0xA7: return 0x00A7;
        case 0xA8: return 0x0401; case 0xA9: return 0x00A9; case 0xAA: return 0x0404;
        case 0xAB: return 0x00AB; case 0xAC: return 0x00AC; case 0xAD: return 0x00AD;
        case 0xAE: return 0x00AE; case 0xAF: return 0x0407; case 0xB0: return 0x00B0;
        case 0xB1: return 0x00B1; case 0xB2: return 0x0406; case 0xB3: return 0x0456;
        case 0xB4: return 0x0491; case 0xB5: return 0x00B5; case 0xB6: return 0x00B6;
        case 0xB7: return 0x00B7; case 0xB8: return 0x0451; case 0xB9: return 0x2116;
        case 0xBA: return 0x0454; case 0xBB: return 0x00BB; case 0xBC: return 0x0458;
        case 0xBD: return 0x0405; case 0xBE: return 0x0455; case 0xBF: return 0x0457;
        default: return '?';
    }
}

static void emit_utf8(uint32_t cp, char *dst, size_t dst_len, size_t *o)
{
    if (*o + 4 >= dst_len) return;
    if (cp < 0x80) {
        dst[(*o)++] = (char)cp;
    } else if (cp < 0x800) {
        dst[(*o)++] = (char)(0xC0 | (cp >> 6));
        dst[(*o)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        dst[(*o)++] = (char)(0xE0 | (cp >> 12));
        dst[(*o)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[(*o)++] = (char)(0x80 | (cp & 0x3F));
    }
}

void cp866_to_utf8(const char *src, char *dst, size_t dst_len)
{
    if (!src || !dst || dst_len == 0) return;
    const unsigned char *p = (const unsigned char *)src;

    /* FATFS may already give us UTF-8 (CONFIG_FATFS_API_ENCODING_UTF_8). */
    if (utf8_is_valid(p)) {
        size_t n = strlen(src);
        if (n >= dst_len) n = dst_len - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
        return;
    }

    /* Detect Windows-1251 vs OEM-866 for Cyrillic text.
       CP866 Russian letters live in 0x80-0xAF / 0xE0-0xEF,
       CP1251 letters in 0xC0-0xFF (plus a few 0xA5..0xBF). */
    int score866 = 0, score1251 = 0;
    for (const unsigned char *q = p; *q; q++) {
        unsigned char b = *q;
        if (b < 0x80) continue;
        if (b >= 0x80 && b <= 0x9F) score866 += 2;
        else if (b >= 0xE0 && b <= 0xEF) score866 += 1;
        else if (b >= 0xC0 && b <= 0xFF) score1251 += 1;
        else if (b == 0xA8 || b == 0xB8) score1251 += 1;
    }
    int use1251 = (score1251 > score866);

    /* If OEM-866 decoding would produce box-drawing characters, the source
       is almost certainly Windows-1251 (filenames rarely contain box glyphs). */
    if (!use1251) {
        for (const unsigned char *q = p; *q; q++) {
            if (*q >= 0xB0 && *q <= 0xDF) {
                use1251 = 1;
                break;
            }
        }
    }

    size_t o = 0;
    while (*p && o + 4 < dst_len) {
        unsigned char b = *p++;
        uint32_t cp;
        if (b < 0x80) {
            cp = b;
        } else if (use1251) {
            cp = (b >= 0xC0) ? s_cp1251[b - 0xC0] : cp1251_special(b);
        } else {
            cp = s_cp866[b - 0x80];
        }
        emit_utf8(cp, dst, dst_len, &o);
    }
    dst[o] = '\0';
}

void playlist_get_display_name(int idx, char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    if (idx < 0 || idx >= g_playlist_size || !g_playlist[idx]) return;
    const char *base = strrchr(g_playlist[idx], '/');
    base = base ? base + 1 : g_playlist[idx];
    cp866_to_utf8(base, dst, dst_len);
}
