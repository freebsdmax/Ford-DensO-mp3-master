#pragma once
#include <stdbool.h>
#include <stddef.h>

/**
 * ID3-теги MP3: назва треку та виконавець.
 * Підтримуються ID3v2.3/2.4 (ISO-8859-1, UTF-16 з BOM, UTF-16BE, UTF-8)
 * і запасний варіант ID3v1 (останні 128 байт файлу).
 *
 * @return true, якщо вдалося прочитати хоча б назву або виконавця
 */
bool player_tags_read(const char *path,
                      char *title, size_t title_len,
                      char *artist, size_t artist_len);
