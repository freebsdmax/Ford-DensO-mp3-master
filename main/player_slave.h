#pragma once
#include <stddef.h>

void slave_version_start(void);
void slave_version_refresh_now(void);

void slave_version_get(char *main_date, size_t main_date_len,
                       char *main_time, size_t main_time_len,
                       char *crc_date, size_t crc_date_len,
                       char *crc_time, size_t crc_time_len,
                       char *uart_date, size_t uart_date_len,
                       char *uart_time, size_t uart_time_len);
