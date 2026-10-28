#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#define MAX_PLAYLIST_SIZE 1000

extern char **g_playlist;
extern int g_playlist_size;
extern bool g_playlist_ready;
extern int g_current_track;

void add_to_playlist(const char *file_path);
void cp866_to_utf8(const char *src, char *dst, size_t dst_len);
void playlist_get_display_name(int idx, char *dst, size_t dst_len);
void find_mp3_files_recursive(const char *path);
void clear_playlist(void);
void create_playlist_from_usb(void);
