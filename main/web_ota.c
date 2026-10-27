#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "web_ota.h"
#include "uart_ota_protocol.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "elm327_emu.h"

#include <stdarg.h> // for va_list

#define LOG_BUFFER_SIZE 4096

static char log_buffer[LOG_BUFFER_SIZE];

static int log_write_pos = 0;
static size_t log_valid_bytes = 0;
static portMUX_TYPE log_buffer_mux = portMUX_INITIALIZER_UNLOCKED;

static vprintf_like_t original_vprintf = NULL;

// Глобальний прапор для детальних логів
bool g_verbose_logs = false;
// Прапори для логів компонентів
bool g_player_logs = true;
bool g_wifi_can_sniffer_logs = true;
bool g_web_ota_logs = true;
bool g_uart_ota_logs = true;
bool g_elm327_logs = true;

int log_vprintf(const char *fmt, va_list args) {

    char temp[256];
    va_list original_args;
    va_copy(original_args, args);
    int len = vsnprintf(temp, sizeof(temp), fmt, args);
    size_t count = len > 0 ? (size_t)len : 0;
    if (count >= sizeof(temp)) {
        count = sizeof(temp) - 1;
    }

    portENTER_CRITICAL(&log_buffer_mux);
    for (size_t i = 0; i < count; i++) {
        log_buffer[log_write_pos] = temp[i];
        log_write_pos = (log_write_pos + 1) % LOG_BUFFER_SIZE;
    }
    log_valid_bytes += count;
    if (log_valid_bytes > LOG_BUFFER_SIZE) {
        log_valid_bytes = LOG_BUFFER_SIZE;
    }
    portEXIT_CRITICAL(&log_buffer_mux);

    // Call original to keep serial output
    int result;
    if (original_vprintf) {
        result = original_vprintf(fmt, original_args);
    } else {
        result = vprintf(fmt, original_args);
    }
    va_end(original_args);
    return result;

}

size_t web_ota_copy_logs(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    portENTER_CRITICAL(&log_buffer_mux);
    size_t count = log_valid_bytes;
    if (count >= buffer_size) {
        count = buffer_size - 1;
    }
    int start = (log_write_pos - (int)count + LOG_BUFFER_SIZE) % LOG_BUFFER_SIZE;
    for (size_t i = 0; i < count; i++) {
        buffer[i] = log_buffer[(start + i) % LOG_BUFFER_SIZE];
    }
    buffer[count] = '\0';
    portEXIT_CRITICAL(&log_buffer_mux);
    return count;
}

// Extern declarations for player control functions
extern char* get_playlist_json(void);
extern void player_control(const char* cmd, int track);
extern void player_set_position(int position_ms);
extern char* get_player_status_json(void);
extern esp_err_t get_wifi_settings(char *sta_ssid, size_t sta_ssid_len, char *sta_password, size_t sta_pass_len,
                                  char *ap_ssid, size_t ap_ssid_len, char *ap_password, size_t ap_pass_len, int *mode);
extern esp_err_t set_wifi_settings(const char *sta_ssid, const char *sta_password,
                                  const char *ap_ssid, const char *ap_password, int mode);

// Extern declarations for sleep settings functions
extern esp_err_t get_sleep_settings(int *sleep_mode, int *idle_timeout_sec);
extern esp_err_t set_sleep_settings(int sleep_mode, int idle_timeout_sec);

static const char *TAG = "web_ota";

/**
 * Функція для збереження версії web при запуску
 */
void web_ota_save_version(void)
{
    static bool version_saved = false;
    if (version_saved) return;
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("web_ota", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        nvs_set_str(nvs_handle, "date", __DATE__);
        nvs_set_str(nvs_handle, "time", __TIME__);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "💾 Web OTA version saved to NVS");
        version_saved = true;
    } else {
        ESP_LOGE(TAG, "❌ Failed to save Web OTA version to NVS: %s", esp_err_to_name(err));
    }
}
static httpd_handle_t s_server = NULL;

// Forward declaration
extern esp_err_t uart_ota_send_firmware(const uint8_t *firmware_data, size_t firmware_size,
                                         void (*progress_callback)(size_t sent, size_t total));

// Forward declarations for OTA tasks
static void uart_ota_task(void *param);
static void master_ota_task(void *param);

// UART OTA chunk size (must match UART_OTA_BLOCK_SIZE)
#define UART_OTA_CHUNK_SIZE 240

// Simple shared progress state for UI polling
typedef struct {
    bool active;
    bool is_uart_slave;  // Renamed from is_i2c
    size_t total;
    size_t written;
    uint16_t block;
} ota_progress_t;

static ota_progress_t s_prog = {0};

// Cached slave versions
static char cached_slave_main_date[12] = "";
static char cached_slave_main_time[9] = "";
static char cached_slave_crc_date[12] = "";
static char cached_slave_crc_time[9] = "";
static char cached_slave_uart_date[12] = "";
static char cached_slave_uart_time[9] = "";

static const char *INDEX_HTML =
"<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'><title>ESP32 Web Control</title>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<style>"
"* {box-sizing: border-box;}"
"body {"
"  font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;"
"  margin: 20px;"
"  background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);"
"  min-height: 100vh;"
"  color: #333;"
"}"
".container {"
"  max-width: 800px;"
"  margin: 0 auto;"
"  background: white;"
"  border-radius: 12px;"
"  overflow: hidden;"
"  box-shadow: 0 4px 20px rgba(0,0,0,0.1);"
"}"
".header {"
"  text-align: center;"
"  padding: 2rem 1rem;"
"  background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);"
"  color: white;"
"}"
".header h2 {"
"  margin: 0;"
"  font-size: 2rem;"
"  font-weight: 600;"
"  text-shadow: 0 2px 4px rgba(0,0,0,0.3);"
"}"
".tabs {"
"  display: flex;"
"  border-radius: 12px;"
"}"
".tab {"
"  flex: 1;"
"  padding: 1rem 1.5rem;"
"  cursor: pointer;"
"  text-align: center;"
"  font-weight: 500;"
"  color: #6c757d;"
"  background: #e9ecef;"
"  border: 1px solid #dee2e6;"
"  border-radius: 12px;"
"  box-shadow: 0 2px 4px rgba(0,0,0,0.1);"
"  transition: all 0.3s ease;"
"  position: relative;"
"  margin-right: 0.5rem;"
"}"
".tab:last-child {"
"  margin-right: 0;"
"}"
".tab:hover {"
"  background: #f1f3f4;"
"  color: #495057;"
"  transform: translateY(-1px);"
"  box-shadow: 0 4px 8px rgba(0,0,0,0.15);"
"}"
".tab.active {"
"  background: white;"
"  color: #495057;"
"  font-weight: 600;"
"  border-color: #667eea;"
"  box-shadow: 0 4px 8px rgba(0,0,0,0.15);"
"}"
".tab-content {"
"  display: none;"
"  padding: 2rem;"
"  background: white;"
"  border-radius: 12px;"
"  margin-bottom: 2rem;"
"  box-shadow: 0 4px 20px rgba(0,0,0,0.1);"
"}"
".tab-content.active {"
"  display: block;"
"}"
".section {"
"  background: #f8f9fa;"
"  border-radius: 12px;"
"  padding: 1.5rem;"
"  margin-bottom: 1.5rem;"
"  border: 1px solid #e9ecef;"
"  box-shadow: 0 4px 20px rgba(0,0,0,0.1);"
"}"
".section h3 {"
"  margin: 0 0 1rem 0;"
"  color: #495057;"
"  font-size: 1.25rem;"
"  font-weight: 600;"
"  display: flex;"
"  align-items: center;"
"  gap: 0.5rem;"
"}"
".btn {"
"  background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);"
"  color: white;"
"  border: none;"
"  padding: 0.75rem 1.5rem;"
"  border-radius: 6px;"
"  font-size: 1rem;"
"  font-weight: 500;"
"  cursor: pointer;"
"  transition: all 0.3s ease;"
"  display: inline-flex;"
"  align-items: center;"
"  gap: 0.5rem;"
"  text-decoration: none;"
"  margin: 0.25rem;"
"}"
".btn:hover {"
"  transform: translateY(-1px);"
"  opacity: 0.9;"
"}"
".btn-secondary {"
"  background: #6c757d;"
"}"
".btn-secondary:hover {"
"  background: #5a6268;"
"}"
".status {"
"  padding: 0.75rem;"
"  border-radius: 6px;"
"  margin: 0.5rem 0;"
"  font-weight: 500;"
"}"
".status.success {"
"  background: #d4edda;"
"  color: #155724;"
"  border: 1px solid #c3e6cb;"
"}"
".status.error {"
"  background: #f8d7da;"
"  color: #721c24;"
"  border: 1px solid #f5c6cb;"
"}"
".status.info {"
"  background: #d1ecf1;"
"  color: #0c5460;"
"  border: 1px solid #bee5eb;"
"}"
"input[type='range'] {"
"  width: 100%;"
"  height: 6px;"
"  border-radius: 3px;"
"  background: #ddd;"
"  outline: none;"
"  margin: 1rem 0;"
"}"
"input[type='range']::-webkit-slider-thumb {"
"  appearance: none;"
"  width: 20px;"
"  height: 20px;"
"  border-radius: 50%;"
"  background: #667eea;"
"  cursor: pointer;"
"}"
"#playlist {"
"  list-style: none;"
"  padding: 0;"
"  margin: 0;"
"  max-height: 300px;"
"  overflow-y: auto;"
"}"
"#playlist li {"
"  cursor: pointer;"
"  padding: 0.75rem 1rem;"
"  transition: all 0.2s ease;"
"  display: flex;"
"  align-items: center;"
"  gap: 0.5rem;"
"  background: #fff;"
"  border-radius: 8px;"
"  box-shadow: 0 2px 4px rgba(0,0,0,0.1);"
"  margin-bottom: 0.5rem;"
"}"
"#playlist li:nth-child(even) {"
"  background: #f9f9f9;"
"}"
"#playlist li:hover {"
"  background: #e9ecef;"
"  transform: translateY(-1px);"
"  box-shadow: 0 4px 8px rgba(0,0,0,0.15);"
"}"
"#playlist li.playing {"
"  background: linear-gradient(135deg, #e3f2fd 0%, #bbdefb 100%);"
"  border-left: 4px solid #2196f3;"
"  font-weight: 600;"
"  box-shadow: 0 4px 12px rgba(33,150,243,0.3);"
"  transform: translateY(-2px);"
"}"
"#time-display {"
"  text-align: center;"
"  margin-top: 0.5rem;"
"  color: #666;"
"  font-weight: 500;"
"}"
"@media (max-width: 600px) {"
"  body { padding: 8px; }"
"  .container { margin: 0; border-radius: 0; }"
"  .header { padding: 1rem 0.75rem; }"
"  .header h2 { font-size: 1.2rem; }"
"  .tabs { flex-direction: column; gap:0.25rem; }"
"  .tab { padding: 0.75rem; font-size: 1rem; width:100%; box-sizing:border-box; }"
"  .btn { padding: 0.75rem; font-size: 1rem; margin: 0.25rem 0; min-width:0; }"
"  #playlist li { padding: 0.85rem; font-size: 1rem; }"
"  .section { padding: 0.85rem; }"
"  .tab-content { padding: 0.75rem; }"
"  .controls { display:flex; gap:0.5rem; overflow-x:auto; -webkit-overflow-scrolling:touch; padding-bottom:0.25rem; }"
"  .controls .btn { flex: 0 0 auto; min-width: 96px; padding: 0.6rem 0.8rem; }"
"  .controls .btn:nth-child(odd) { margin-right:0; }"
"  .switch { transform: scale(1.05); }"
"  .slider:before { height:20px; width:20px; left:4px; bottom:4px; }"
"}"
".controls { display:flex; gap:0.5rem; align-items:center; flex-wrap:nowrap; }"
".switch { position: relative; display: inline-block; width:50px; height:28px; }"
".switch input { opacity: 0; width: 0; height: 0; }"
".slider { position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0; background-color: #ccc; transition: .2s; border-radius: 28px; }"
".slider:before { position: absolute; content: \"\"; height: 22px; width: 22px; left: 3px; bottom: 3px; background-color: white; transition: .2s; border-radius: 50%; }"
"input:checked + .slider { background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); }"
"input:checked + .slider:before { transform: translateX(22px); }"
"</style></head><body>"
"<div class='container'>"
"  <div class='header'>"
"    <h2>🎵 ESP32 Music Player</h2>"
"    <p class='lead'>Manage music, OTA and Wi‑Fi CAN Sniffer — clean, fast control for your device.</p>"
"  </div>"
"  <div style='padding:0 1rem 1rem 1rem;'>"
"<div class='tabs'>"
"  <div class='tab active' data-tab='player'>🎵 Плеєр</div>"
"  <div class='tab' data-tab='ota'>🔧 OTA-оновлення</div>"
"  <div class='tab' data-tab='settings'>⚙️ Налаштування</div>"
"  <div class='tab' data-tab='monitoring'>📊 Моніторинг</div>"
"</div>"
"  <div class='tab-content active' id='tab-player'>"
"  <div class='section'>"
"    <h3>Керування плеєром</h3>"
"    <div class='controls' style='margin-bottom:1rem;'>" 
"      <button class='btn' onclick=\"playerCmd('prev')\">⏮️ Назад</button>" 
"      <button class='btn' onclick=\"playerCmd('play')\">▶️ Відтворити</button>" 
"      <button class='btn' onclick=\"playerCmd('pause')\">⏸️ Пауза</button>" 
"      <button class='btn' onclick=\"playerCmd('next')\">⏭️ Далі</button>" 
"      <button class='btn' onclick=\"playerCmd('switch_mode')\">🔄 Режим</button>" 
"    </div>" 
"    <div id='player-status' class='status info' style='margin-bottom:1rem;'></div>"
"    <div style='margin-bottom:1rem;'>"
"      <input type='range' id='progress-bar' min='0' max='100' value='0'>"
"      <div id='time-display'>0:00 / 0:00</div>"
"    </div>"
"    <ul id='playlist'></ul>"
"  </div>"
"</div>"
"<div class='tab-content' id='tab-ota'>"
"  <div class='section'><h3>📱 Master ESP32 (цей пристрій)</h3>"
"    <form method='POST' action='/upload' enctype='application/octet-stream' id='f1'>"
"      <input type='file' id='file1' required><br>"
"      <button class='btn' type='submit'>Завантажити та оновити Master</button>"
"    </form>"
"  </div>"
"  <div class='section'><h3>🎵 Slave ESP32 (Bluetooth модуль через UART)</h3>"
"    <form method='POST' action='/upload_slave' enctype='application/octet-stream' id='f2'>"
"      <input type='file' id='file2' required><br>"
"      <button class='btn' type='submit'>Завантажити та оновити Slave</button>"
"    </form>"
"  </div>"
"  <div class='section'><h3>ℹ️ Інформація про версії</h3>"
"    <div id='version-info' style='background:#f5f5f5; padding:1rem; border-radius:4px; margin-top:1rem;'>"
"      <div><strong>🖥️ Master ESP32:</strong></div>"
"      <div id='master-versions' style='margin-left:1rem; margin-top:0.5rem;'>Завантаження...</div>"
"      <div style='margin-top:1rem;'><strong>🎵 Slave ESP32:</strong> <span id='slave-version'>Завантаження...</span></div>"
"      <div id='slave-versions' style='margin-left:1rem; margin-top:0.5rem;'>Завантаження...</div>"
"    </div>"
"  </div>"
"  <progress id='upload-progress' value='0' max='100' style='width:100%; height:20px; display:none;'></progress><progress id='ota-progress' value='0' max='100' style='width:100%; height:20px; display:none;'></progress><div id='progress'></div><div id='log'></div>"
"</div>"
"<div class='tab-content' id='tab-settings'>"
"  <div class='section'>"
"    <div style='display:flex; align-items:center; justify-content:space-between;'>"
"      <h3>🌐 Wi-Fi налаштування</h3>"
"      <div style='display:flex; align-items:center; gap:0.5rem;'>"
"        <label style='font-weight:600;margin-right:0.5rem;'>WiFi CAN Sniffer</label>"
"        <label class='switch'>"
"          <input type='checkbox' id='sniffer-enabled'>"
"          <span class='slider'></span>"
"        </label>"
"      </div>"
"    </div>"
"    <form id='wifi-form'>"
"      <h4>STA режим (підключення до існуючої мережі)</h4>"
"      <label>STA SSID (ім'я мережі):</label><br>"
"      <input type='text' id='sta-ssid' maxlength='31'><br>"
"      <label>STA Пароль:</label><br>"
"      <div style='display:flex; align-items:center; margin-bottom:0.5rem;'>"
"        <input type='password' id='sta-password' maxlength='63' style='flex:1;'>"
"        <button type='button' onclick='togglePassword(\"sta-password\")' style='margin-left:0.5rem; padding:0.2rem 0.5rem; font-size:0.8rem;'>👁️</button>"
"      </div>"
"      <h4>AP режим (створення власної мережі)</h4>"
"      <label>AP SSID (ім'я мережі):</label><br>"
"      <input type='text' id='ap-ssid' maxlength='31'><br>"
"      <label>AP Пароль:</label><br>"
"      <div style='display:flex; align-items:center; margin-bottom:0.5rem;'>"
"        <input type='password' id='ap-password' maxlength='63' style='flex:1;'>"
"        <button type='button' onclick='togglePassword(\"ap-password\")' style='margin-left:0.5rem; padding:0.2rem 0.5rem; font-size:0.8rem;'>👁️</button>"
"      </div>"
"      <label>Режим:</label><br>"
"      <select id='wifi-mode'>"
"        <option value='0'>AP (точка доступу)</option>"
"        <option value='1'>STA (клієнт)</option>"
"      </select><br><br>"
"      <button class='btn btn-secondary' type='button' onclick='loadWifiSettings()'>Завантажити налаштування</button>"
"      <button class='btn' type='button' onclick='saveWifiSettings()'>Зберегти налаштування</button>"
"      <div id='wifi-status' style='margin-top:1rem;color:#06c;'></div>"
"    </form>"
"  </div>"
"  <div class='section'>"
"    <h3>🌙 Налаштування сну</h3>"
"    <form id='sleep-form'>"
"      <label>Режим сну:</label><br>"
"      <select id='sleep-mode'>"
"        <option value='0'>Вимкнено</option>"
"        <option value='3'>Deep Sleep</option>"
"      </select><br>"
"      <label>Час бездіяльності (секунди, min 10):</label><br>"
"      <input type='number' id='idle-timeout' min='10' max='28800' value='30'><br>"
"      <label>Рівень логів:</label><br>"
"      <select id='log-level'>"
"        <option value='0'>None</option>"
"        <option value='1'>Error</option>"
"        <option value='2'>Warn</option>"
"        <option value='3'>Info</option>"
"        <option value='4'>Debug</option>"
"        <option value='5'>Verbose</option>"
"      </select><br>"
"      <label><input type='checkbox' id='verbose-logs'> Детальні логи (емодзі та банери)</label><br><br>"
"      <h4>Логи компонентів:</h4>"
"      <label><input type='checkbox' id='player-logs' checked> Player</label><br>"
"      <label><input type='checkbox' id='wifi-can-sniffer-logs' checked> WiFi CAN Sniffer</label><br>"
"      <label><input type='checkbox' id='web-ota-logs' checked> Web OTA</label><br>"
"      <label><input type='checkbox' id='uart-ota-logs' checked> UART OTA</label><br>"
"      <label><input type='checkbox' id='elm327-logs' checked> ELM327</label><br><br>"
"      <button class='btn btn-secondary' type='button' onclick='loadSleepSettings(); loadLogLevel(); loadVerboseLogs(); loadComponentLogs()'>Завантажити налаштування</button>"
"      <button class='btn' type='button' onclick='saveSleepSettings(); saveLogLevel(); saveVerboseLogs(); saveComponentLogs()'>Зберегти налаштування</button>"
"      <button class='btn btn-danger' type='button' onclick='restartDevice()'>Перезавантажити пристрій</button>"
"      <div id='sleep-status' style='margin-top:1rem;color:#06c;'></div>"
"    </form>"
"  </div>"
"</div>"
"<div class='tab-content' id='tab-monitoring'>"
"  <div class='section'><h3>📊 Логи системи</h3>"
"    <pre id='log-output' style='background:#000;color:#0f0;padding:1rem;font-family:monospace;font-size:0.8rem;max-height:400px;overflow-y:auto;white-space:pre-wrap;'></pre>"
"  </div>"
"</div>"
"<script>"
"const tabs=document.querySelectorAll('.tab');"
"const tabContents=document.querySelectorAll('.tab-content');"
"tabs.forEach(tab=>{tab.onclick=()=>{"
"  tabs.forEach(t=>t.classList.remove('active'));"
"  tabContents.forEach(tc=>tc.classList.remove('active'));"
"  tab.classList.add('active');"
"  document.getElementById('tab-'+tab.dataset.tab).classList.add('active');"
"  if(tab.dataset.tab==='settings'){loadWifiSettings();loadSleepSettings();loadLogLevel();loadVerboseLogs();loadComponentLogs();}"
"  if(tab.dataset.tab==='ota'){loadOtaStatus();}"
"  if(tab.dataset.tab==='monitoring'){updateLogs();}"
"};});"
"function togglePassword(fieldId){"
"  const field = document.getElementById(fieldId);"
"  const button = field.nextElementSibling;"
"  if(field.type === 'password'){"
"    field.type = 'text';"
"    button.textContent = '🙈';"
"  }else{"
"    field.type = 'password';"
"    button.textContent = '👁️';"
"  }"
"}"
"function loadPlaylist(){"
"  fetch('/playlist').then(r=>r.json()).then(data=>{"
"    const ul=document.getElementById('playlist');"
"    ul.innerHTML='';"
"    data.forEach((track,i)=>{"
"      const li=document.createElement('li');"
"      li.textContent=(i+1) + '. ' + track;"
"      li.onclick=()=>{fetch('/player_cmd?cmd=play&track='+i).catch(e=>{console.log('Error playing track:',e);});};"
"      ul.appendChild(li);"
"    });"
"    updatePlayerStatus();"
"  }).catch(e=>{console.log('Error loading playlist:',e);});"
"}"
"loadPlaylist();"
"function playerCmd(cmd){"
"  fetch('/player_cmd?cmd='+cmd).then(r=>r.text()).then(t=>{"
"    document.getElementById('player-status').textContent=t;"
"  }).catch(e=>{console.log('Error sending command:',e);});"
"}"
"function setSnifferState(state){"
"  fetch('/player_cmd?cmd=set_sniffer&track='+state).then(r=>r.text()).then(t=>{"
"    document.getElementById('player-status').textContent=t;"
"  }).catch(e=>{console.log('Error setting sniffer state:',e);});"
"}"
"document.addEventListener('DOMContentLoaded', function(){" 
"  var sn = document.getElementById('sniffer-enabled');" 
"  if (sn) sn.addEventListener('change', function(e){ setSnifferState(e.target.checked?1:0); });" 
"});" 
"function updatePlayerStatus(){"
"  fetch('/player_status').then(r=>r.json()).then(s=>{"
"    const progress = document.getElementById('progress-bar');"
"    const timeDisplay = document.getElementById('time-display');"
"    const status = document.getElementById('player-status');"
"    const playlist = document.getElementById('playlist');"
"    "
"    if (playlist) {"
"      const lis = playlist.querySelectorAll('li');"
"      lis.forEach((li, i) => {"
"        li.classList.toggle('playing', i === s.current_track);"
"      });"
"    }"
"    "
"    if (progress && timeDisplay && s.duration_ms > 0) {"
"      const correctedPos = Math.min(s.position_ms / 3000, s.duration_ms / 1000);"
"      const correctedDur = s.duration_ms / 1000;"
"      progress.max = correctedDur;"
"      progress.value = correctedPos;"
"      const posMin = Math.floor(correctedPos / 60);"
"      const posSec = Math.floor(correctedPos % 60);"
"      const durMin = Math.floor(correctedDur / 60);"
"      const durSec = Math.floor(correctedDur % 60);"
"      timeDisplay.textContent = posMin + ':' + posSec.toString().padStart(2,'0') + ' / ' + durMin + ':' + durSec.toString().padStart(2,'0');"
"    }"
"    "
"    if (status) {"
"      status.textContent = 'Трек ' + (s.current_track + 1) + '/' + s.playlist_size + ' ' + (s.is_paused ? '(пауза)' : '(відтворюється)') + ' Режим: ' + s.mode;" 
"    }" 
"    try { var sn = document.getElementById('sniffer-enabled'); if (sn) sn.checked = !!s.sniffer_enabled; } catch(e){}" 
"  }).catch(e=>{console.log('Error updating status:',e);});"
"}"
"setInterval(updatePlayerStatus, 1000);"
"function updateLogs(){"
"  fetch('/logs').then(r=>r.text()).then(t=>{"
"    const logOutput = document.getElementById('log-output');"
"    logOutput.textContent=t;"
"    logOutput.scrollTop = logOutput.scrollHeight;"
"  }).catch(e=>{console.log('Error loading logs:',e);});"
"}"
"setInterval(updateLogs, 2000);"
"document.getElementById('progress-bar').oninput = function() {"
"  fetch('/player_cmd?cmd=set_pos&pos=' + (this.value * 3000)).catch(e=>{console.log('Error seeking:',e);});"
"};"
"const logEl=document.getElementById('log');const prog=document.getElementById('progress');const uploadProg=document.getElementById('upload-progress');const otaProg=document.getElementById('ota-progress');"
"const log=(msg,color)=>{logEl.style.color=color||'#0a0';logEl.textContent=msg;};"
"const poll=()=>{fetch('/ota_status').then(r=>r.json()).then(s=>{console.log('OTA status:',s);"
"if(!s.active){otaProg.style.display='none';prog.textContent='';return;}"
"const p=Math.round(s.written*100/Math.max(1,s.total));"
"const mode=s.is_uart_slave?'UART Slave':'Master Flash';"
"otaProg.value=p;otaProg.style.display='block';"
"prog.textContent=mode + ': ' + p + '% (' + s.written + '/' + s.total + ' bytes)';}).catch(e=>console.log('Poll error:',e));};setInterval(poll,500);"
"function setupForm(formId,fileId,url){"
"const f=document.getElementById(formId);const file=document.getElementById(fileId);"
"f.onsubmit=e=>{e.preventDefault();if(!file.files.length){log('Select a file','#c00');return;}"
"const fr=new FileReader();"
"fr.onload=()=>{const xhr=new XMLHttpRequest();xhr.open('POST',url);"
"xhr.upload.onprogress=ev=>{if(ev.lengthComputable){"
"const pct=Math.round(ev.loaded*100/ev.total);"
"uploadProg.value=pct;uploadProg.style.display='block';"
"prog.textContent='Uploading: ' + pct + '% (' + ev.loaded + '/' + ev.total + ' bytes)';}};"
"xhr.onload=()=>{uploadProg.style.display='none';log(xhr.responseText||'Done',xhr.status===200?'#0a0':'#c00');};"
"xhr.onerror=()=>{uploadProg.style.display='none';log('Network error','#c00');};"
"xhr.send(fr.result);uploadProg.style.display='block';log('Upload started...','#06c');};"
"fr.onerror=()=>{log('Read error','#c00');};fr.readAsArrayBuffer(file.files[0]);};}"
"setupForm('f1','file1','/upload');"
"setupForm('f2','file2','/upload_slave');"
"function loadWifiSettings(){"
"  fetch('/wifi_settings').then(r=>{"
"    if (!r.ok) {"
"      throw new Error('HTTP ' + r.status + ': ' + r.statusText);"
"    }"
"    return r.json();"
"  }).then(s=>{"
"    document.getElementById('sta-ssid').value = s.sta_ssid;"
"    document.getElementById('sta-password').value = s.sta_password;"
"    document.getElementById('ap-ssid').value = s.ap_ssid;"
"    document.getElementById('ap-password').value = s.ap_password;"
"    document.getElementById('wifi-mode').value = s.mode;"
"    document.getElementById('wifi-status').textContent = 'Налаштування завантажено';"
"  }).catch(e=>{"
"    document.getElementById('wifi-status').textContent = 'Помилка завантаження: ' + e.message;"
"  });"
"}"
"function saveWifiSettings(){"
"  const sta_ssid = document.getElementById('sta-ssid').value;"
"  const sta_password = document.getElementById('sta-password').value;"
"  const ap_ssid = document.getElementById('ap-ssid').value;"
"  const ap_password = document.getElementById('ap-password').value;"
"  const mode = document.getElementById('wifi-mode').value;"
"  const data = JSON.stringify({sta_ssid: sta_ssid, sta_password: sta_password, ap_ssid: ap_ssid, ap_password: ap_password, mode: parseInt(mode)});"
"  fetch('/wifi_settings', {method: 'POST', body: data}).then(r=>{"
"    if (r.ok) {"
"      document.getElementById('wifi-status').textContent = 'Налаштування збережено. Перезагрузіть пристрій для застосування.';"
"    } else {"
"      document.getElementById('wifi-status').textContent = 'Помилка збереження';"
"    }"
"  }).catch(e=>{"
"    document.getElementById('wifi-status').textContent = 'Помилка: ' + e.message;"
"  });"
"}"
"function loadSleepSettings(){"
"  fetch('/sleep_settings').then(r=>{"
"    if (!r.ok) {"
"      throw new Error('HTTP ' + r.status + ': ' + r.statusText);"
"    }"
"    return r.json();"
"  }).then(s=>{"
"    document.getElementById('sleep-mode').value = s.sleep_mode;"
"    document.getElementById('idle-timeout').value = s.idle_timeout_sec;"
"    document.getElementById('sleep-status').textContent = 'Налаштування сну завантажено';"
"  }).catch(e=>{"
"    document.getElementById('sleep-status').textContent = 'Помилка завантаження: ' + e.message;"
"  });"
"}"
"function loadLogLevel(){"
"  fetch('/log_level').then(r=>r.text()).then(l=>{"
"    document.getElementById('log-level').value = l;"
"  }).catch(e=>{console.log('Error loading log level:',e);});"
"}"
"function loadOtaStatus(){"
"  fetch('/ota_status').then(r=>r.json()).then(s=>{"
"    document.getElementById('progress-bar').value = s.total > 0 ? (s.written * 100 / s.total) : 0;"
"    document.getElementById('progress').textContent = s.active ? 'Завантаження: ' + s.written + '/' + s.total + ' байт' : '';"
"    document.getElementById('log').textContent = s.active ? 'Блок: ' + s.block : '';"
"    "
"    const playerVersion = s.player_build_date + ' ' + s.player_build_time;"
"    const webVersion = s.web_build_date + ' ' + s.web_build_time;"
"    const uartVersion = s.uart_build_date + ' ' + s.uart_build_time;"
"    const slaveVersion = s.slave_main_build_date && s.slave_main_build_date !== '' ? 'Підключено' : 'Не підключено';"
"    const slaveMainVersion = s.slave_main_build_date && s.slave_main_build_date !== '' ? s.slave_main_build_date + ' ' + s.slave_main_build_time : 'Не підключено';"
"    const slaveCrcVersion = s.slave_crc_build_date && s.slave_crc_build_date !== '' ? s.slave_crc_build_date + ' ' + s.slave_crc_build_time : 'Не підключено';"
"    const slaveUartVersion = s.slave_uart_build_date && s.slave_uart_build_date !== '' ? s.slave_uart_build_date + ' ' + s.slave_uart_build_time : 'Не підключено';"
"    "
"    document.getElementById('master-versions').innerHTML ="
"      '<div><strong>🎵 Player:</strong> ' + playerVersion + '</div>' +"
"      '<div><strong>🌐 Web:</strong> ' + webVersion + '</div>' +"
"      '<div><strong>📡 UART:</strong> ' + uartVersion + '</div>';"
"    document.getElementById('slave-version').textContent = slaveVersion;"
"    document.getElementById('slave-versions').innerHTML ="
"      '<div><strong>🎵 Main:</strong> ' + slaveMainVersion + '</div>' +"
"      '<div><strong>📡 CRC:</strong> ' + slaveCrcVersion + '</div>' +"
"      '<div><strong>📡 UART:</strong> ' + slaveUartVersion + '</div>';"
"  }).catch(e=>{"
"    console.log('Error loading OTA status:', e);"
"  });"
"}"
"function saveSleepSettings(){"
"  const sleepMode = document.getElementById('sleep-mode').value;"
"  const idleTimeout = document.getElementById('idle-timeout').value;"
"  const data = JSON.stringify({sleep_mode: parseInt(sleepMode), idle_timeout_sec: parseInt(idleTimeout)});"
"  fetch('/sleep_settings', {method: 'POST', body: data}).then(r=>{"
"    if (r.ok) {"
"      document.getElementById('sleep-status').textContent = 'Налаштування сну збережено';"
"    } else {"
"      document.getElementById('sleep-status').textContent = 'Помилка збереження';"
"    }"
"  }).catch(e=>{"
"    document.getElementById('sleep-status').textContent = 'Помилка: ' + e.message;"
"  });"
"}"
"function saveLogLevel(){"
"  const level = document.getElementById('log-level').value;"
"  fetch('/log_level', {method:'POST', body:level}).then(r=>r.text()).then(t=>{"
"    document.getElementById('sleep-status').textContent = 'Рівень логів збережено';"
"  }).catch(e=>{console.log('Error saving log level:',e);});"
"}"
"function loadVerboseLogs(){"
"  fetch('/verbose_logs').then(r=>r.text()).then(v=>{"
"    document.getElementById('verbose-logs').checked = (v == '1');"
"  }).catch(e=>{console.log('Error loading verbose logs:',e);});"
"}"
"function saveVerboseLogs(){"
"  const enable = document.getElementById('verbose-logs').checked ? '1' : '0';"
"  fetch('/verbose_logs', {method:'POST', body:enable}).then(r=>r.text()).then(t=>{"
"    document.getElementById('sleep-status').textContent = 'Налаштування логів збережено';"
"  }).catch(e=>{console.log('Error saving verbose logs:',e);});"
"}"
"function loadComponentLogs(){"
"  fetch('/component_logs').then(r=>r.json()).then(c=>{"
"    document.getElementById('player-logs').checked = c.player == 1;"
"    document.getElementById('wifi-can-sniffer-logs').checked = c.wifi_can_sniffer == 1;"
"    document.getElementById('web-ota-logs').checked = c.web_ota == 1;"
"    document.getElementById('uart-ota-logs').checked = c.uart_ota == 1;"
"    document.getElementById('elm327-logs').checked = c.elm327 == 1;"
"    document.getElementById('sleep-status').textContent = 'Налаштування логів компонентів завантажено';"
"  }).catch(e=>{document.getElementById('sleep-status').textContent = 'Помилка завантаження логів компонентів'; console.log('Error loading component logs:',e);});"
"}"
"function saveComponentLogs(){"
"  const data = {"
"    player: document.getElementById('player-logs').checked ? 1 : 0,"
"    wifi_can_sniffer: document.getElementById('wifi-can-sniffer-logs').checked ? 1 : 0,"
"    web_ota: document.getElementById('web-ota-logs').checked ? 1 : 0,"
"    uart_ota: document.getElementById('uart-ota-logs').checked ? 1 : 0,"
"    elm327: document.getElementById('elm327-logs').checked ? 1 : 0"
"  };"
"  const body = data.player + ' ' + data.wifi_can_sniffer + ' ' + data.web_ota + ' ' + data.uart_ota + ' ' + data.elm327;"
"  fetch('/component_logs', {method:'POST', body:body}).then(r=>r.text()).then(t=>{"
"    document.getElementById('sleep-status').textContent = 'Налаштування логів компонентів збережено';"
"  }).catch(e=>{console.log('Error saving component logs:',e);});"
"}"
"function restartDevice(){"
"  if (confirm('Ви впевнені, що хочете перезавантажити пристрій?')) {"
"    fetch('/restart', {method:'POST'}).then(r=>r.text()).then(t=>{"
"      alert('Пристрій перезавантажується...');"
"    }).catch(e=>{console.log('Error restarting:',e);});"
"  }"
"}"
"window.onload = function() { loadComponentLogs(); try{loadElm();}catch(e){} };"

"<footer style='text-align:center;padding:1rem;color:#666;font-size:0.9rem;'>ESP32 Music Player — web control • by your device</footer>"

"</body></html>";

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

// Simple JSON status for polling progress
static esp_err_t status_get_handler(httpd_req_t *req)
{
    static uint32_t last_version_check = 0;
    
    // Читаємо версії з NVS
    char player_date[12] = "Unknown";
    char player_time[9] = "Unknown";
    char uart_date[12] = "Unknown";
    char uart_time[9] = "Unknown";
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("audio_player", NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        size_t len = sizeof(player_date);
        nvs_get_str(nvs_handle, "player_date", player_date, &len);
        len = sizeof(player_time);
        nvs_get_str(nvs_handle, "player_time", player_time, &len);
        nvs_close(nvs_handle);
    }
    
    // UART version
    err = nvs_open("uart_ota", NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        size_t len = sizeof(uart_date);
        nvs_get_str(nvs_handle, "uart_date", uart_date, &len);
        len = sizeof(uart_time);
        nvs_get_str(nvs_handle, "uart_time", uart_time, &len);
        nvs_close(nvs_handle);
    }
    
    // Оновлюємо версію slave тільки раз в 30 секунд
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
    if (now - last_version_check > 30 || cached_slave_main_date[0] == '\0') {
        // Спробуємо отримати версію від slave
        extern esp_err_t get_slave_version_extended(char *main_date, size_t main_date_len, char *main_time, size_t main_time_len,
                                                   char *crc_date, size_t crc_date_len, char *crc_time, size_t crc_time_len,
                                                   char *uart_date, size_t uart_date_len, char *uart_time, size_t uart_time_len);
        esp_err_t slave_err = get_slave_version_extended(cached_slave_main_date, sizeof(cached_slave_main_date), 
                                                        cached_slave_main_time, sizeof(cached_slave_main_time),
                                                        cached_slave_crc_date, sizeof(cached_slave_crc_date),
                                                        cached_slave_crc_time, sizeof(cached_slave_crc_time),
                                                        cached_slave_uart_date, sizeof(cached_slave_uart_date),
                                                        cached_slave_uart_time, sizeof(cached_slave_uart_time));
        if (slave_err == ESP_OK) {
            last_version_check = now;
        }
    }
    
    char json[1024];
    int len = snprintf(json, sizeof(json),
                       "{\"active\":%s,\"is_uart_slave\":%s,\"total\":%u,\"written\":%u,\"block\":%u,"
                       "\"player_build_date\":\"%s\",\"player_build_time\":\"%s\","
                       "\"web_build_date\":\"%s\",\"web_build_time\":\"%s\","
                       "\"uart_build_date\":\"%s\",\"uart_build_time\":\"%s\","
                       "\"slave_main_build_date\":\"%s\",\"slave_main_build_time\":\"%s\","
                       "\"slave_crc_build_date\":\"%s\",\"slave_crc_build_time\":\"%s\","
                       "\"slave_uart_build_date\":\"%s\",\"slave_uart_build_time\":\"%s\"}",
                       s_prog.active ? "true" : "false",
                       s_prog.is_uart_slave ? "true" : "false",
                       (unsigned)s_prog.total,
                       (unsigned)s_prog.written,
                       (unsigned)s_prog.block,
                       player_date, player_time,
                       __DATE__, __TIME__,
                       uart_date, uart_time,
                       cached_slave_main_date,
                       cached_slave_main_time,
                       cached_slave_crc_date,
                       cached_slave_crc_time,
                       cached_slave_uart_date,
                       cached_slave_uart_time);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, len);
}

// Get versions as JSON
static esp_err_t version_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handler: version_get_handler called");
    
    // Читаємо версії з NVS
    char player_date[12] = "Unknown";
    char player_time[9] = "Unknown";
    char uart_date[12] = "Unknown";
    char uart_time[9] = "Unknown";
    char web_date[12] = "Unknown";
    char web_time[9] = "Unknown";
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("audio_player", NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        size_t len = sizeof(player_date);
        nvs_get_str(nvs_handle, "player_date", player_date, &len);
        len = sizeof(player_time);
        nvs_get_str(nvs_handle, "player_time", player_time, &len);
        nvs_close(nvs_handle);
    }
    ESP_LOGI(TAG, "Player version: %s %s", player_date, player_time);
    
    // UART version
    ESP_LOGI(TAG, "Handler: Opening uart_ota for UART");
    err = nvs_open("uart_ota", NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Handler: Reading UART date");
        size_t len = sizeof(uart_date);
        nvs_get_str(nvs_handle, "uart_date", uart_date, &len);
        ESP_LOGI(TAG, "Handler: Reading UART time");
        len = sizeof(uart_time);
        nvs_get_str(nvs_handle, "uart_time", uart_time, &len);
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "Handler: UART read done");
    } else {
        ESP_LOGE(TAG, "Handler: Failed to open uart_ota: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "UART version: %s %s", uart_date, uart_time);
    
    // Web version
    err = nvs_open("web_ota", NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        size_t len = sizeof(web_date);
        nvs_get_str(nvs_handle, "web_date", web_date, &len);
        len = sizeof(web_time);
        nvs_get_str(nvs_handle, "time", web_time, &len);
        nvs_close(nvs_handle);
    }
    ESP_LOGI(TAG, "Web version: %s %s", web_date, web_time);
    
    char json[512];
    int len = snprintf(json, sizeof(json),
                       "{\"player_build_date\":\"%s\",\"player_build_time\":\"%s\","
                       "\"web_build_date\":\"%s\",\"web_build_time\":\"%s\","
                       "\"uart_build_date\":\"%s\",\"uart_build_time\":\"%s\","
                       "\"slave_main_build_date\":\"%s\",\"slave_main_build_time\":\"%s\","
                       "\"slave_crc_build_date\":\"%s\",\"slave_crc_build_time\":\"%s\","
                       "\"slave_uart_build_date\":\"%s\",\"slave_uart_build_time\":\"%s\"}",
                       player_date, player_time,
                       web_date, web_time,
                       uart_date, uart_time,
                       cached_slave_main_date, cached_slave_main_time,
                       cached_slave_crc_date, cached_slave_crc_time,
                       cached_slave_uart_date, cached_slave_uart_time);
    
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, len);
}

// Get playlist as JSON array
static esp_err_t playlist_get_handler(httpd_req_t *req)
{
    char *json = get_playlist_json();
    if (!json) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "[]", 2);
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    return ret;
}

// Handle player commands
static esp_err_t player_cmd_handler(httpd_req_t *req)
{
    char buf[100];
    int ret = httpd_req_get_url_query_str(req, buf, sizeof(buf));
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No query");
        return ESP_FAIL;
    }

    char cmd[32] = {0};
    char track_str[32] = {0};
    char pos_str[32] = {0};
    httpd_query_key_value(buf, "cmd", cmd, sizeof(cmd));
    httpd_query_key_value(buf, "track", track_str, sizeof(track_str));
    httpd_query_key_value(buf, "pos", pos_str, sizeof(pos_str));

    int track = -1;
    if (track_str[0]) {
        track = atoi(track_str);
    }

    if (strcmp(cmd, "set_pos") == 0 && pos_str[0]) {
        int pos = atoi(pos_str);
        player_set_position(pos);
    } else {
        player_control(cmd, track);
    }

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK", 2);
}

// Get player status as JSON
static esp_err_t player_status_handler(httpd_req_t *req)
{
    char *json = get_player_status_json();
    if (!json) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{}", 2);
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    return ret;
}

// Get Wi-Fi settings as JSON
static esp_err_t wifi_settings_get_handler(httpd_req_t *req)
{
    char sta_ssid[32] = "";
    char sta_password[64] = "";
    char ap_ssid[32] = "";
    char ap_password[64] = "";
    int mode = 0;
    
    esp_err_t err = get_wifi_settings(sta_ssid, sizeof(sta_ssid), sta_password, sizeof(sta_password),
                                     ap_ssid, sizeof(ap_ssid), ap_password, sizeof(ap_password), &mode);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to load settings");
        return ESP_FAIL;
    }
    
    char json[512];
    int len = snprintf(json, sizeof(json),
                       "{\"sta_ssid\":\"%s\",\"sta_password\":\"%s\",\"ap_ssid\":\"%s\",\"ap_password\":\"%s\",\"mode\":%d}",
                       sta_ssid, sta_password, ap_ssid, ap_password, mode);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, len);
}

// Set Wi-Fi settings from POST data
static esp_err_t wifi_settings_post_handler(httpd_req_t *req)
{
    char buf[512];  // Збільшено буфер
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[ret] = '\0';  // Null-terminate
    
    // Parse JSON (покращений парсинг)
    char sta_ssid[32] = "";
    char sta_password[64] = "";
    char ap_ssid[32] = "";
    char ap_password[64] = "";
    int mode = 0;
    
    // Шукаємо поля в JSON
    const char *sta_ssid_key = "\"sta_ssid\":\"";
    const char *sta_pass_key = "\"sta_password\":\"";
    const char *ap_ssid_key = "\"ap_ssid\":\"";
    const char *ap_pass_key = "\"ap_password\":\"";
    const char *mode_key = "\"mode\":";
    
    // Парсинг STA SSID
    char *ssid_start = strstr(buf, sta_ssid_key);
    if (ssid_start) {
        ssid_start += strlen(sta_ssid_key);
        char *ssid_end = strchr(ssid_start, '"');
        if (ssid_end && (ssid_end - ssid_start) < sizeof(sta_ssid)) {
            size_t len = ssid_end - ssid_start;
            memcpy(sta_ssid, ssid_start, len);
            sta_ssid[len] = '\0';
            // Unescape quotes if needed
            char *quote = strchr(sta_ssid, '\\');
            if (quote && quote[1] == '"') {
                memmove(quote, quote + 1, strlen(quote));
            }
        }
    }
    
    // Парсинг STA password
    char *pass_start = strstr(buf, sta_pass_key);
    if (pass_start) {
        pass_start += strlen(sta_pass_key);
        char *pass_end = strchr(pass_start, '"');
        if (pass_end && (pass_end - pass_start) < sizeof(sta_password)) {
            size_t len = pass_end - pass_start;
            memcpy(sta_password, pass_start, len);
            sta_password[len] = '\0';
            // Unescape quotes if needed
            char *quote = strchr(sta_password, '\\');
            if (quote && quote[1] == '"') {
                memmove(quote, quote + 1, strlen(quote));
            }
        }
    }
    
    // Парсинг AP SSID
    ssid_start = strstr(buf, ap_ssid_key);
    if (ssid_start) {
        ssid_start += strlen(ap_ssid_key);
        char *ssid_end = strchr(ssid_start, '"');
        if (ssid_end && (ssid_end - ssid_start) < sizeof(ap_ssid)) {
            size_t len = ssid_end - ssid_start;
            memcpy(ap_ssid, ssid_start, len);
            ap_ssid[len] = '\0';
            // Unescape quotes if needed
            char *quote = strchr(ap_ssid, '\\');
            if (quote && quote[1] == '"') {
                memmove(quote, quote + 1, strlen(quote));
            }
        }
    }
    
    // Парсинг AP password
    pass_start = strstr(buf, ap_pass_key);
    if (pass_start) {
        pass_start += strlen(ap_pass_key);
        char *pass_end = strchr(pass_start, '"');
        if (pass_end && (pass_end - pass_start) < sizeof(ap_password)) {
            size_t len = pass_end - pass_start;
            memcpy(ap_password, pass_start, len);
            ap_password[len] = '\0';
            // Unescape quotes if needed
            char *quote = strchr(ap_password, '\\');
            if (quote && quote[1] == '"') {
                memmove(quote, quote + 1, strlen(quote));
            }
        }
    }
    
    // Парсинг mode
    char *mode_start = strstr(buf, mode_key);
    if (mode_start) {
        mode_start += strlen(mode_key);
        mode = atoi(mode_start);
    }
    
    // Валідація
    if (strlen(sta_ssid) == 0 && mode == 1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "STA SSID required for STA mode");
        return ESP_FAIL;
    }
    if (strlen(ap_ssid) == 0 && mode == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "AP SSID required for AP mode");
        return ESP_FAIL;
    }
    
    esp_err_t err = set_wifi_settings(sta_ssid, sta_password, ap_ssid, ap_password, mode);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save settings");
        return ESP_FAIL;
    }
    
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK", 2);
}

// 🌙 GET handler для налаштувань сну
static esp_err_t sleep_settings_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Sleep settings GET handler called for URI: %s", req->uri);
    extern esp_err_t get_sleep_settings(int *sleep_mode, int *idle_timeout_sec);
    
    int sleep_mode = 0;
    int idle_timeout_sec = 30;
    
    esp_err_t err = get_sleep_settings(&sleep_mode, &idle_timeout_sec);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get sleep settings: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to load sleep settings");
        return ESP_FAIL;
    }
    
    char json[128];
    int len = snprintf(json, sizeof(json), 
                      "{\"sleep_mode\":%d,\"idle_timeout_sec\":%d}", 
                      sleep_mode, idle_timeout_sec);
    
    ESP_LOGI(TAG, "Returning sleep settings JSON: %s", json);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, len);
}

// 🌙 POST handler для налаштувань сну
static esp_err_t sleep_settings_post_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    // Парсинг JSON
    int sleep_mode = 0;
    int idle_timeout_sec = 30;
    
    // Шукаємо поля в JSON
    const char *mode_key = "\"sleep_mode\":";
    const char *timeout_key = "\"idle_timeout_sec\":";
    
    // Парсинг sleep_mode
    char *mode_start = strstr(buf, mode_key);
    if (mode_start) {
        mode_start += strlen(mode_key);
        sleep_mode = atoi(mode_start);
    }
    
    // Парсинг idle_timeout_sec
    char *timeout_start = strstr(buf, timeout_key);
    if (timeout_start) {
        timeout_start += strlen(timeout_key);
        idle_timeout_sec = atoi(timeout_start);
    }
    
    // Валідація
    if (sleep_mode < 0 || sleep_mode > 3) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sleep mode (0-3)");
        return ESP_FAIL;
    }
    if (idle_timeout_sec < 10 || idle_timeout_sec > 28800) { // 10 сек - 8 годин
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid timeout (10-28800 sec)");
        return ESP_FAIL;
    }
    
    extern esp_err_t set_sleep_settings(int sleep_mode, int idle_timeout_sec);
    esp_err_t err = set_sleep_settings(sleep_mode, idle_timeout_sec);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save sleep settings");
        return ESP_FAIL;
    }
    
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK", 2);
}

// Forward declarations for stopping other tasks
// extern void stop_playback_and_services(void);

static esp_err_t upload_post_handler(httpd_req_t *req)
{
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    if (req->content_len <= 0 || req->content_len > update_partition->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid firmware size");
        return ESP_FAIL;
    }

    // Allocate buffer for entire firmware
    uint8_t *firmware_buf = malloc(req->content_len);
    if (!firmware_buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Reading firmware data: %d bytes", (int)req->content_len);

    // Read all data into buffer
    size_t remaining = req->content_len;
    size_t offset = 0;
    char *buf = malloc(1024);
    if (!buf) {
        free(firmware_buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf, remaining > 1024 ? 1024 : remaining);
        if (recv_len <= 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            free(buf);
            free(firmware_buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }
        
        memcpy(firmware_buf + offset, buf, recv_len);
        offset += recv_len;
        remaining -= recv_len;
    }
    free(buf);

    // Sanity check: ensure we actually received data
    if (offset == 0) {
        ESP_LOGE(TAG, "OTA aborted: received 0 bytes");
        free(firmware_buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data received");
        return ESP_FAIL;
    }

    // init progress state
    s_prog.active = true;
    s_prog.is_uart_slave = false;
    s_prog.total = req->content_len;
    s_prog.written = 0;
    s_prog.block = 0;

    ESP_LOGI(TAG, "Starting async Master OTA task");

    // Start async OTA task
    if (xTaskCreate(master_ota_task, "master_ota", 4096, firmware_buf, 5, NULL) != pdPASS) {
        free(firmware_buf);
        s_prog.active = false;
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start OTA task");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK: firmware received, OTA started asynchronously");
    ESP_LOGI(TAG, "Async Master OTA started, task will handle the update");

    return ESP_OK;
}

// Progress callback для UART OTA
static void uart_ota_progress_cb(size_t sent, size_t total)
{
    s_prog.written = sent;
    s_prog.total = total;
}

// Task для асинхронного UART OTA
static void uart_ota_task(void *param)
{
    uint8_t *firmware_buf = (uint8_t *)param;
    size_t size = s_prog.total;

    ESP_LOGI(TAG, "🎵 Starting UART OTA task: %zu bytes", size);

    esp_err_t ret = uart_ota_send_firmware(firmware_buf, size, uart_ota_progress_cb);

    free(firmware_buf);
    s_prog.active = false;

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ UART OTA task failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "🎉 UART OTA task completed successfully");
    }

    vTaskDelete(NULL);
}

// Task для асинхронного Master OTA
static void master_ota_task(void *param)
{
    uint8_t *firmware_buf = (uint8_t *)param;
    size_t size = s_prog.total;

    ESP_LOGI(TAG, "🖥️ Starting Master OTA task: %zu bytes", size);

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "No OTA partition in task");
        free(firmware_buf);
        s_prog.active = false;
        vTaskDelete(NULL);
        return;
    }

    // Збільшуємо watchdog timeout для OTA
    esp_task_wdt_config_t ota_wdt_config = {
        .timeout_ms = 30000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = true
    };
    esp_task_wdt_reconfigure(&ota_wdt_config);

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, size, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed in task: %s", esp_err_to_name(err));
        free(firmware_buf);
        s_prog.active = false;
        vTaskDelete(NULL);
        return;
    }

    // Write in chunks
    size_t offset = 0;
    while (offset < size) {
        size_t chunk = (size - offset) > 1024 ? 1024 : (size - offset);
        err = esp_ota_write(ota_handle, firmware_buf + offset, chunk);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed in task: %s", esp_err_to_name(err));
            esp_ota_end(ota_handle);
            free(firmware_buf);
            s_prog.active = false;
            vTaskDelete(NULL);
            return;
        }
        offset += chunk;
        s_prog.written = offset;
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA end failed in task: %s", esp_err_to_name(err));
        free(firmware_buf);
        s_prog.active = false;
        vTaskDelete(NULL);
        return;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Set boot partition failed in task: %s", esp_err_to_name(err));
        free(firmware_buf);
        s_prog.active = false;
        vTaskDelete(NULL);
        return;
    }

    free(firmware_buf);
    s_prog.active = false;

    ESP_LOGI(TAG, "🎉 Master OTA task completed successfully, rebooting");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

/**
 * Upload firmware to UART slave (Bluetooth ESP32) through UART
 */
static esp_err_t upload_slave_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > UART_OTA_MAX_SIZE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid firmware size (max 2MB)");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "🎵 Starting UART OTA for slave: %d bytes", (int)req->content_len);

    // Виділити пам'ять для firmware
    uint8_t *firmware_buf = malloc(req->content_len);
    if (!firmware_buf) {
        ESP_LOGE(TAG, "Failed to allocate %d bytes for firmware", (int)req->content_len);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    // Ініціалізувати прогрес
    s_prog.active = true;
    s_prog.is_uart_slave = true;
    s_prog.total = req->content_len;
    s_prog.written = 0;
    s_prog.block = 0;

    // Отримати весь firmware в пам'ять
    size_t remaining = req->content_len;
    size_t offset = 0;
    
    while (remaining > 0) {
        size_t to_recv = remaining > 4096 ? 4096 : remaining;
        int recv_len = httpd_req_recv(req, (char *)(firmware_buf + offset), to_recv);
        
        if (recv_len <= 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "Failed to receive firmware data");
            free(firmware_buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            s_prog.active = false;
            return ESP_FAIL;
        }
        
        offset += recv_len;
        remaining -= recv_len;
        
        // Оновити прогрес отримання
        s_prog.written = offset;
    }

    ESP_LOGI(TAG, "✅ Firmware received: %zu bytes, starting UART OTA task...", offset);

    // Запустити асинхронний OTA task
    xTaskCreate(uart_ota_task, "uart_ota", 4096, firmware_buf, 5, NULL);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "✅ UART OTA started asynchronously!");
    ESP_LOGI(TAG, "🚀 UART OTA task started");
    
    return ESP_OK;
}

/**
 * Upload firmware to storage partition for later I2C OTA
 */
static esp_err_t upload_to_storage_handler(httpd_req_t *req)
{
    const esp_partition_t *storage_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 
                                                                     ESP_PARTITION_SUBTYPE_DATA_FAT, NULL);
    if (!storage_part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Storage partition not found");
        return ESP_FAIL;
    }

    if (req->content_len <= 0 || req->content_len > storage_part->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid firmware size for storage");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Uploading firmware to storage: %d bytes", (int)req->content_len);

    s_prog.active = true;
    s_prog.is_uart_slave = false;
    s_prog.total = req->content_len;
    s_prog.written = 0;

    // Erase storage partition first
    esp_err_t err = esp_partition_erase_range(storage_part, 0, storage_part->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Storage erase failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erase failed");
        s_prog.active = false;
        return err;
    }

    size_t remaining = req->content_len;
    size_t offset = 0;
    char *buf = malloc(1024);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory error");
        s_prog.active = false;
        return ESP_FAIL;
    }

    while (remaining > 0) {
        size_t to_recv = remaining > 1024 ? 1024 : remaining;
        int recv_len = httpd_req_recv(req, buf, to_recv);
        if (recv_len <= 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "Storage upload receive error");
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            s_prog.active = false;
            return ESP_FAIL;
        }

        err = esp_partition_write(storage_part, offset, buf, recv_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Storage write failed: %s", esp_err_to_name(err));
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
            s_prog.active = false;
            return err;
        }

        offset += recv_len;
        remaining -= recv_len;
        s_prog.written = offset;
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    free(buf);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Firmware saved to storage");
    s_prog.active = false;
    ESP_LOGI(TAG, "Firmware uploaded to storage: %zu bytes", offset);
    return ESP_OK;
}



static esp_err_t logs_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    // Send the log buffer
    char *response = malloc(LOG_BUFFER_SIZE + 1);
    if (!response) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory error");
        return ESP_FAIL;
    }
    int pos = log_write_pos;
    for(int i = 0; i < LOG_BUFFER_SIZE; i++) {
        response[i] = log_buffer[pos];
        pos = (pos + 1) % LOG_BUFFER_SIZE;
    }
    response[LOG_BUFFER_SIZE] = '\0';
    httpd_resp_send(req, response, strlen(response));
    free(response);
    return ESP_OK;
}



static esp_err_t log_level_get_handler(httpd_req_t *req)
{
    esp_log_level_t level = esp_log_level_get("*");
    char buf[10];
    snprintf(buf, sizeof(buf), "%d", level);
    httpd_resp_send(req, buf, strlen(buf));
    return ESP_OK;
}

static esp_err_t log_level_post_handler(httpd_req_t *req)
{
    char buf[10];
    int ret = httpd_req_recv(req, buf, sizeof(buf)-1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';
    int level = atoi(buf);
    if (level >= ESP_LOG_NONE && level <= ESP_LOG_VERBOSE) {
        esp_log_level_set("*", level);
        // save to NVS
        nvs_handle_t nvs_handle;
        if (nvs_open("audio_player", NVS_READWRITE, &nvs_handle) == ESP_OK) {
            nvs_set_u8(nvs_handle, "log_level", level);
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
        }
    }
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t verbose_logs_get_handler(httpd_req_t *req)
{
    char buf[10];
    snprintf(buf, sizeof(buf), "%d", g_verbose_logs ? 1 : 0);
    httpd_resp_send(req, buf, strlen(buf));
    return ESP_OK;
}

static esp_err_t verbose_logs_post_handler(httpd_req_t *req)
{
    char buf[10];
    int ret = httpd_req_recv(req, buf, sizeof(buf)-1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';
    int enable = atoi(buf);
    g_verbose_logs = (enable != 0);
    // save to NVS
    nvs_handle_t nvs_handle;
    if (nvs_open("audio_player", NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, "verbose_logs", g_verbose_logs ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t restart_handler(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "Restarting device...");
    vTaskDelay(pdMS_TO_TICKS(100)); // Give time to send response
    esp_restart();
    return ESP_OK; // Never reached
}

static esp_err_t component_logs_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Component logs GET handler called");
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"player\":%d,\"wifi_can_sniffer\":%d,\"web_ota\":%d,\"uart_ota\":%d,\"elm327\":%d}",
             g_player_logs ? 1 : 0,
             g_wifi_can_sniffer_logs ? 1 : 0,
             g_web_ota_logs ? 1 : 0,
             g_uart_ota_logs ? 1 : 0,
             g_elm327_logs ? 1 : 0);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, strlen(buf));
    return ESP_OK;
}

static esp_err_t component_logs_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Component logs POST handler called");
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf)-1);
    if (ret <= 0) {
        ESP_LOGE(TAG, "httpd_req_recv failed: %d", ret);
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    ESP_LOGI(TAG, "Received component logs buf: %s", buf);
    // Parse plain text
    int player, wifi, web, uart, elm327;
    if (sscanf(buf, "%d %d %d %d %d", &player, &wifi, &web, &uart, &elm327) != 5) {
        ESP_LOGE(TAG, "Failed to parse component logs");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Parsed component logs: player=%d, wifi=%d, web=%d, uart=%d, elm=%d", player, wifi, web, uart, elm327);
    g_player_logs = (player != 0);
    g_wifi_can_sniffer_logs = (wifi != 0);
    g_web_ota_logs = (web != 0);
    g_uart_ota_logs = (uart != 0);
    g_elm327_logs = (elm327 != 0);
    // save to NVS
    ESP_LOGI(TAG, "Saving component logs to NVS: player=%d, wifi=%d, web=%d, uart=%d, elm=%d", g_player_logs, g_wifi_can_sniffer_logs, g_web_ota_logs, g_uart_ota_logs, g_elm327_logs);
    nvs_handle_t nvs_handle;
    if (nvs_open("audio_player", NVS_READWRITE, &nvs_handle) == ESP_OK) {
        ESP_LOGI(TAG, "NVS opened for component logs save");
        esp_err_t err;
        err = nvs_set_u8(nvs_handle, "player_logs", g_player_logs ? 1 : 0);
        if (err != ESP_OK) ESP_LOGE(TAG, "nvs_set_u8 player_logs failed: %s", esp_err_to_name(err));
        err = nvs_set_u8(nvs_handle, "wifi_logs", g_wifi_can_sniffer_logs ? 1 : 0);
        if (err != ESP_OK) ESP_LOGE(TAG, "nvs_set_u8 wifi_logs failed: %s", esp_err_to_name(err));
        err = nvs_set_u8(nvs_handle, "web_ota_logs", g_web_ota_logs ? 1 : 0);
        if (err != ESP_OK) ESP_LOGE(TAG, "nvs_set_u8 web_ota_logs failed: %s", esp_err_to_name(err));
        err = nvs_set_u8(nvs_handle, "uart_ota_logs", g_uart_ota_logs ? 1 : 0);
        if (err != ESP_OK) ESP_LOGE(TAG, "nvs_set_u8 uart_ota_logs failed: %s", esp_err_to_name(err));
        err = nvs_set_u8(nvs_handle, "elm327_logs", g_elm327_logs ? 1 : 0);
        if (err != ESP_OK) ESP_LOGE(TAG, "nvs_set_u8 elm327_logs failed: %s", esp_err_to_name(err));
        if (nvs_commit(nvs_handle) == ESP_OK) {
            ESP_LOGI(TAG, "NVS committed for component logs");
        } else {
            ESP_LOGE(TAG, "NVS commit failed for component logs");
        }
        nvs_close(nvs_handle);
    } else {
        ESP_LOGE(TAG, "Failed to open NVS for component logs save");
    }
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}


static esp_err_t elm327_config_get_handler(httpd_req_t *req)
{
    elm327_config_t cfg;
    if (elm327_emu_get_config(&cfg) != ESP_OK) {
        elm327_emu_load_config(&cfg);
    }
    char resp[128];
    sprintf(resp, "{\"enabled\":%d,\"wifi_enabled\":%d,\"bt_enabled\":%d}", cfg.enabled?1:0, cfg.wifi_enabled?1:0, cfg.bt_enabled?1:0);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static esp_err_t elm327_config_post_handler(httpd_req_t *req)
{
    char buf[256];
    int ret, remaining = req->content_len;
    if (remaining > 255) remaining = 255;
    if (remaining < 0) remaining = 0;
    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty");
        return ESP_FAIL;
    }
    buf[ret] = 0;
    elm327_config_t cfg = {0};
    if (strstr(buf, "\"enabled\":1") || strstr(buf, "\"enabled\":true")) cfg.enabled = true;
    if (strstr(buf, "\"wifi_enabled\":1") || strstr(buf, "\"wifi_enabled\":true")) cfg.wifi_enabled = true;
    if (strstr(buf, "\"bt_enabled\":1") || strstr(buf, "\"bt_enabled\":true")) cfg.bt_enabled = true;
    if (elm327_emu_set_config(&cfg) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "err");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", 15);
    return ESP_OK;
}

static esp_err_t elm327_status_handler(httpd_req_t *req)
{
    elm327_config_t cfg;
    elm327_emu_get_config(&cfg);
    char resp[160];
    sprintf(resp, "{\"enabled\":%d,\"wifi_enabled\":%d,\"bt_enabled\":%d,\"running\":%d}", cfg.enabled?1:0, cfg.wifi_enabled?1:0, cfg.bt_enabled?1:0, (cfg.enabled && (cfg.wifi_enabled || cfg.bt_enabled)) ? 1 : 0);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}



esp_err_t web_ota_start(void)
{
    if (s_server) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 30;  // Unlimited
    config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server: %s", esp_err_to_name(err));
        return err;
    }

    // Load log level from NVS
    esp_log_level_t level = ESP_LOG_INFO;
    nvs_handle_t nvs_handle;
    if (nvs_open("audio_player", NVS_READONLY, &nvs_handle) == ESP_OK) {
        uint8_t val;
        if (nvs_get_u8(nvs_handle, "log_level", &val) == ESP_OK) {
            level = val;
        }
        nvs_close(nvs_handle);
    }
    esp_log_level_set("*", level);

    // Set up log capture
    original_vprintf = esp_log_set_vprintf(log_vprintf);
    ESP_LOGI(TAG, "Log capture enabled");

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t upload = {
        .uri = "/upload",
        .method = HTTP_POST,
        .handler = upload_post_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t upload_storage = {
        .uri = "/upload_to_storage",
        .method = HTTP_POST,
        .handler = upload_to_storage_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t upload_slave = {
        .uri = "/upload_slave",
        .method = HTTP_POST,
        .handler = upload_slave_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t status = {
        .uri = "/ota_status",
        .method = HTTP_GET,
        .handler = status_get_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t version = {
        .uri = "/version",
        .method = HTTP_GET,
        .handler = version_get_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t playlist = {
        .uri = "/playlist",
        .method = HTTP_GET,
        .handler = playlist_get_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t player_cmd = {
        .uri = "/player_cmd",
        .method = HTTP_GET,
        .handler = player_cmd_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t player_status = {
        .uri = "/player_status",
        .method = HTTP_GET,
        .handler = player_status_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t wifi_settings_get = {
        .uri = "/wifi_settings",
        .method = HTTP_GET,
        .handler = wifi_settings_get_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t wifi_settings_post = {
        .uri = "/wifi_settings",
        .method = HTTP_POST,
        .handler = wifi_settings_post_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t sleep_settings_get = {
        .uri = "/sleep_settings",
        .method = HTTP_GET,
        .handler = sleep_settings_get_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t sleep_settings_post = {
        .uri = "/sleep_settings",
        .method = HTTP_POST,
        .handler = sleep_settings_post_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t logs = {
        .uri = "/logs",
        .method = HTTP_GET,
        .handler = logs_get_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t log_level_get = {
        .uri = "/log_level",
        .method = HTTP_GET,
        .handler = log_level_get_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t log_level_post = {
        .uri = "/log_level",
        .method = HTTP_POST,
        .handler = log_level_post_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t verbose_logs_get = {
        .uri = "/verbose_logs",
        .method = HTTP_GET,
        .handler = verbose_logs_get_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t verbose_logs_post = {
        .uri = "/verbose_logs",
        .method = HTTP_POST,
        .handler = verbose_logs_post_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t restart = {
        .uri = "/restart",
        .method = HTTP_POST,
        .handler = restart_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t component_logs_get = {
        .uri = "/component_logs",
        .method = HTTP_GET,
        .handler = component_logs_get_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t component_logs_post = {
        .uri = "/component_logs",
        .method = HTTP_POST,
        .handler = component_logs_post_handler,
        .user_ctx = NULL,
    };

    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &upload);
    httpd_register_uri_handler(s_server, &upload_storage);
    httpd_register_uri_handler(s_server, &upload_slave);
    httpd_register_uri_handler(s_server, &status);
    httpd_register_uri_handler(s_server, &version);
    httpd_register_uri_handler(s_server, &playlist);
    httpd_register_uri_handler(s_server, &player_cmd);
    httpd_register_uri_handler(s_server, &player_status);
    httpd_register_uri_handler(s_server, &sleep_settings_get);
    httpd_register_uri_handler(s_server, &sleep_settings_post);
    httpd_register_uri_handler(s_server, &wifi_settings_get);
    httpd_register_uri_handler(s_server, &wifi_settings_post);
    httpd_register_uri_handler(s_server, &logs);
    httpd_register_uri_handler(s_server, &log_level_get);
    httpd_register_uri_handler(s_server, &log_level_post);
    httpd_register_uri_handler(s_server, &verbose_logs_get);
    httpd_register_uri_handler(s_server, &verbose_logs_post);
    httpd_register_uri_handler(s_server, &restart);
    httpd_register_uri_handler(s_server, &component_logs_post);
    httpd_register_uri_handler(s_server, &component_logs_get);
    {
        httpd_uri_t u1 = { .uri = "/elm327_config", .method = HTTP_GET, .handler = elm327_config_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(s_server, &u1);
    }
    {
        httpd_uri_t u2 = { .uri = "/elm327_config", .method = HTTP_POST, .handler = elm327_config_post_handler, .user_ctx = NULL };
        httpd_register_uri_handler(s_server, &u2);
    }
    {
        httpd_uri_t u3 = { .uri = "/elm327_status", .method = HTTP_GET, .handler = elm327_status_handler, .user_ctx = NULL };
        httpd_register_uri_handler(s_server, &u3);
    }


    ESP_LOGI(TAG, "Web OTA server started on port %d", config.server_port);
    ESP_LOGI(TAG, "Registered URI handlers: root, upload, upload_storage, upload_slave, status, version, playlist, player_cmd, player_status, sleep_settings_get, sleep_settings_post, wifi_settings_get, wifi_settings_post, logs, log_level_get, log_level_post, verbose_logs_get, verbose_logs_post, restart, component_logs_get, component_logs_post");
    return ESP_OK;
}

void web_ota_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}