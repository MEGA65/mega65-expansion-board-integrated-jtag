#include "remote_http.h"
#include "config.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_file.h"
#include "core_filter.h"
#include "jtag_gpio.h"
#include "machine_identity.h"
#include "remote_auth.h"
#include "signed_file.h"
#include "storage.h"
#include "uart_cmd.h"
#include "write_gate.h"

#include "mbedtls/sha256.h"

#ifndef M65_WIFI_CONNECT_TIMEOUT_MS
#define M65_WIFI_CONNECT_TIMEOUT_MS 15000u
#endif
#ifndef M65_WIFI_RETRY_DELAY_MS
#define M65_WIFI_RETRY_DELAY_MS 60000u
#endif
#ifndef M65_WIFI_FIRST_RETRY_DELAY_MS
#define M65_WIFI_FIRST_RETRY_DELAY_MS 5000u
#endif
#ifndef M65_WIFI_PROBE_FAST_ATTEMPTS
#define M65_WIFI_PROBE_FAST_ATTEMPTS 4u
#endif
#ifndef M65_WIFI_PROBE_FAST_RETRY_MS
#define M65_WIFI_PROBE_FAST_RETRY_MS 1000u
#endif
#ifndef M65_WIFI_NOIP_TIMEOUT_MS
#define M65_WIFI_NOIP_TIMEOUT_MS 10000u
#endif
#ifndef M65_FETCH_CONNECT_TIMEOUT_MS
#define M65_FETCH_CONNECT_TIMEOUT_MS 10000u
#endif
#ifndef M65_FETCH_IDLE_TIMEOUT_MS
#define M65_FETCH_IDLE_TIMEOUT_MS 60000u
#endif
#ifndef M65_FETCH_IDLE_DIAG_MS
#define M65_FETCH_IDLE_DIAG_MS 3000u
#endif
#ifndef M65_FETCH_ACK_NUDGE_MS
#define M65_FETCH_ACK_NUDGE_MS 1000u
#endif
#ifndef M65_AUTOFETCH_FILE_RETRIES
#define M65_AUTOFETCH_FILE_RETRIES 10u
#endif
#ifndef M65_FETCH_MAX_BYTES
#define M65_FETCH_MAX_BYTES 1073741824u
#endif
#ifndef M65_DOWNLOAD_QUEUE_DEPTH
#define M65_DOWNLOAD_QUEUE_DEPTH 4u
#endif
#ifndef M65_FILE_HASH_PROGRESS_MS
#define M65_FILE_HASH_PROGRESS_MS 1000u
#endif
#ifndef M65_FILE_HASH_PROGRESS_BYTES
#define M65_FILE_HASH_PROGRESS_BYTES 262144u
#endif
#ifndef M65_WIFI_HW_PROBE_WATCHDOG_MS
#define M65_WIFI_HW_PROBE_WATCHDOG_MS 10000u
#endif
#ifndef M65_WIFI_POLL_INTERVAL_US
#define M65_WIFI_POLL_INTERVAL_US 1000u
#endif
#ifndef M65_HTTP_HEADER_MAX
#define M65_HTTP_HEADER_MAX 1536u
#endif
#ifndef M65_HTTP_PAGE_MAX
#define M65_HTTP_PAGE_MAX 65536u
#endif
#ifndef M65_HTTP_IO_CHUNK
#define M65_HTTP_IO_CHUNK 1024u
#endif
#ifndef M65_HTTP_ROW_TEMPLATE_MAX
#define M65_HTTP_ROW_TEMPLATE_MAX 1024u
#endif
#ifndef M65_HTTP_TEMPLATE_CHUNK
#define M65_HTTP_TEMPLATE_CHUNK 128u
#endif
#ifndef M65_HTTP_INDEX_MAX_ROWS
#define M65_HTTP_INDEX_MAX_ROWS 100u
#endif
#ifndef M65_HTTP_BUSY_TEMPLATE_MAX
#define M65_HTTP_BUSY_TEMPLATE_MAX 8192u
#endif
#ifndef M65_HTTP_BUSY_PAGE_MAX
#define M65_HTTP_BUSY_PAGE_MAX 8192u
#endif
#ifndef M65_HTTP_FAVICON_MAX
#define M65_HTTP_FAVICON_MAX 4096u
#endif

#if !M65_WIFI_SUPPORTED

void remote_http_init(void) {}
void remote_http_boot_check(void) {}
void remote_http_poll(void) {}
void remote_http_defer_wifi_recovery(uint32_t quiet_ms) { (void)quiet_ms; }
void remote_http_set_verbose(uint8_t level) { (void)level; }
uint8_t remote_http_verbose(void) { return 0; }
bool remote_http_active(void) { return false; }
const char *remote_http_status(void) { return "wifi=not-supported"; }
const char *remote_http_wifi_summary(void) { return "NO HARDWARE"; }
const char *remote_http_wifi_diag(void) { return "supported=0 board=" M65_PICO_BOARD_NAME; }
bool remote_http_wifi_probe_now(void) { return false; }
bool remote_http_download_start(const char *url, int slot, char *dest, size_t dest_len, char *err, size_t err_len)
{
    (void)url;
    (void)slot;
    if (dest && dest_len) dest[0] = 0;
    if (err && err_len) snprintf(err, err_len, "wifi not supported");
    return false;
}
const char *remote_http_download_status(void) { return "downloads=unsupported"; }
void remote_http_autofetch_poll(int enabled_override, uint32_t interval_hours_override, uint8_t board_rev_override)
{
    (void)enabled_override;
    (void)interval_hours_override;
    (void)board_rev_override;
}
void remote_http_autofetch_reset_schedule(void) {}
void remote_http_autofetch_cancel(const char *reason) { (void)reason; }
bool remote_http_autofetch_start_now(int enabled_override, uint32_t interval_hours_override, uint8_t board_rev_override)
{
    (void)enabled_override;
    (void)interval_hours_override;
    (void)board_rev_override;
    return false;
}
const char *remote_http_autofetch_status(int enabled_override, uint32_t interval_hours_override, uint8_t board_rev_override)
{
    (void)enabled_override;
    (void)interval_hours_override;
    (void)board_rev_override;
    return "autofetch=unsupported";
}
uint32_t remote_http_autofetch_last_success_seconds(void) { return 0xffffffffu; }
bool remote_http_autofetch_running(void) { return false; }
const char *remote_http_firmware_status(void) { return "firmware=unsupported"; }
bool remote_http_firmware_update(char *err, size_t err_len)
{
    if (err && err_len) snprintf(err, err_len, "wifi/SD firmware update support is not built");
    return false;
}
const char *remote_http_theme_status(void) { return "theme=unsupported"; }
bool remote_http_theme_install(char *err, size_t err_len)
{
    if (err && err_len) snprintf(err, err_len, "wifi/SD theme support is not built");
    return false;
}
bool remote_http_theme_install_named(const char *theme_name, char *err, size_t err_len)
{
    (void)theme_name;
    if (err && err_len) snprintf(err, err_len, "wifi/SD theme support is not built");
    return false;
}

#else

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/tcp.h"

typedef enum {
    HTTP_RECV_HEADERS = 0,
    HTTP_RECV_BODY,
    HTTP_SEND_FILE,
    HTTP_CLOSING,
    HTTP_CLOSED,
} http_state_t;

typedef enum {
    HTTP_PUT_NONE = 0,
    HTTP_PUT_FILE,
    HTTP_PUT_JTAG,
} http_put_op_t;

typedef struct {
    struct tcp_pcb *pcb;
    http_state_t state;
    http_put_op_t put_op;
    bool in_use;
    bool aborted;
    bool jtag_active;
    bool jtag_spool;
    uint8_t jtag_board_rev;
    uint32_t remote_ip;
    char header[M65_HTTP_HEADER_MAX];
    size_t header_len;
    char method[8];
    char target[256];
    char path[256];
    char tmp_path[272];
    char content_type[64];
    bool attachment;
    uint32_t content_length;
    uint32_t body_done;
    uint32_t expected_idcode;
    uint32_t file_offset;
    uint32_t file_size;
    jtag_stream_writer_t jtag;
    signed_file_rx_t signed_rx;
} http_conn_t;

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    const char *tmpl;
} page_builder_t;

typedef struct {
    bool in_placeholder;
    char name[48];
    size_t name_len;
} template_parser_t;

typedef struct {
    page_builder_t *pb;
    const char *row_template;
    const char *dir_path;
    uint16_t rows;
    uint8_t board_rev;
    bool truncated;
} index_list_ctx_t;

typedef enum {
    FETCH_CONNECTING = 0,
    FETCH_RECV_HEADERS,
    FETCH_RECV_BODY,
    FETCH_DONE,
    FETCH_FAILED,
} fetch_state_t;

typedef enum {
    AUTOFETCH_IDLE = 0,
    AUTOFETCH_MANIFEST,
    AUTOFETCH_SCAN,
    AUTOFETCH_VERIFY,
    AUTOFETCH_CORE,
} autofetch_state_t;

typedef enum {
    MANIFEST_KIND_SKIP = 0,
    MANIFEST_KIND_CORE,
    MANIFEST_KIND_FIRMWARE,
    MANIFEST_KIND_THEME,
} manifest_kind_t;

typedef struct {
    manifest_kind_t kind;
    char payload_sha[65];
    char transfer_sha[65];
    char rel[224];
    char version[48];
    char build[64];
    char name[48];
} manifest_entry_t;

typedef enum {
    WIFI_ASSOC_OFF = 0,
    WIFI_ASSOC_CONNECTING,
    WIFI_ASSOC_FAILED,
    WIFI_ASSOC_HTTP_ACTIVE,
} wifi_assoc_state_t;

typedef enum {
    DOWNLOAD_JOB_EMPTY = 0,
    DOWNLOAD_JOB_QUEUED,
    DOWNLOAD_JOB_RUNNING,
    DOWNLOAD_JOB_DONE,
    DOWNLOAD_JOB_FAILED,
} download_job_state_t;

typedef struct {
    struct tcp_pcb *pcb;
    fetch_state_t state;
    const remote_auth_config_t *cfg;
    absolute_time_t deadline;
    absolute_time_t connect_deadline;
    bool raw_to_buffer;
    bool check_write_grant;
    char host[128];
    char path[256];
    char name[192];
    char final_path[256];
    char tmp_path[272];
    char header[M65_HTTP_HEADER_MAX];
    char *raw_buf;
    size_t raw_cap;
    size_t raw_len;
    size_t header_len;
    uint16_t port;
    uint16_t local_port;
    uint32_t content_length;
    uint32_t body_done;
    signed_file_rx_t signed_rx;
    mbedtls_sha256_context transfer_sha;
    bool transfer_sha_active;
    char transfer_sha_hex[65];
    char err[128];
    absolute_time_t body_started_at;
    absolute_time_t last_rx_at;
    absolute_time_t last_idle_diag_at;
    absolute_time_t last_ack_nudge_at;
    absolute_time_t last_progress_at;
    uint32_t last_progress_report;
    uint32_t last_progress_bytes;
} fetch_ctx_t;

typedef struct {
    download_job_state_t state;
    uint8_t slot;
    char url[320];
    char name[24];
    char path[48];
    char err[128];
    uint32_t bytes_done;
    uint32_t bytes_total;
} download_job_t;

static bool parse_query_value(const char *target, const char *key, char *out, size_t out_len);
static int find_header_end(const char *buf, size_t len, size_t *end_len);
static void autofetch_cancel(const char *reason, uint32_t retry_delay_ms);
static bool autofetch_start_manifest(uint8_t board_rev);
static void close_http_listener(void);
static bool ensure_parent_dirs(const char *path);
static err_t http_sent(void *arg, struct tcp_pcb *pcb, u16_t len);
static err_t http_poll_cb(void *arg, struct tcp_pcb *pcb);
static void html_escape(const char *in, char *out, size_t out_len);

static remote_auth_config_t http_cfg;
static http_conn_t http_conn;
static struct tcp_pcb *listen_pcb;
static bool http_is_active;
static bool cyw43_is_ready;
static bool wifi_hardware_blocked;
static bool wifi_hardware_known_present;
static bool wifi_probe_pending;
static bool wifi_probe_retry_scheduled;
static bool remote_init_after_probe_pending;
static uint32_t wifi_hardware_fault_stage;
static uint32_t wifi_probe_attempts;
static int wifi_init_rc = -9999;
static char wifi_diag_buf[320];
static wifi_assoc_state_t wifi_assoc_state = WIFI_ASSOC_OFF;
static absolute_time_t wifi_assoc_deadline;
static absolute_time_t wifi_assoc_retry_at;
static absolute_time_t wifi_noip_since;
static absolute_time_t wifi_recover_at;
static absolute_time_t wifi_probe_retry_at;
static bool wifi_noip_timer_active;
static bool wifi_recover_scheduled;
static char wifi_recover_reason[40];
static bool wifi_recover_first_retry_used;
static absolute_time_t wifi_poll_due;
static int wifi_assoc_last_status = CYW43_LINK_DOWN - 99;
static char http_status_buf[128] = "wifi=inactive hardware=not-probed";
static char last_wifi_log[128];
static uint8_t remote_verbose = 1;
static char row_template[M65_HTTP_ROW_TEMPLATE_MAX];
static absolute_time_t next_autofetch_due;
static bool autofetch_schedule_valid;
static bool autofetch_running;
static bool autofetch_last_success_valid;
static absolute_time_t autofetch_last_success;
static autofetch_state_t autofetch_state = AUTOFETCH_IDLE;
static fetch_ctx_t autofetch_fc;
static remote_auth_config_t autofetch_sig_cfg;
static uint32_t autofetch_manifest_offset;
static uint32_t autofetch_needed_count;
static uint32_t autofetch_updated_count;
static uint32_t autofetch_checked_count;
static uint32_t autofetch_manifest_total_count;
static uint32_t autofetch_fetch_retry_count;
static char autofetch_manifest_path[48];
static char autofetch_base_url[192];
static char autofetch_channel[24];
static char autofetch_pending_path[224];
static char autofetch_pending_url_path[224];
static char autofetch_pending_sha[65];
static char autofetch_pending_transfer_sha[65];
static manifest_kind_t autofetch_pending_kind;
static char autofetch_pending_version[48];
static char autofetch_pending_build[64];
static char autofetch_pending_name[48];
static char autofetch_status_buf[320] = "autofetch=idle";
static char pending_firmware_version[48];
static char pending_firmware_build[64];
static char pending_firmware_source[224];
static bool pending_firmware_seen;
static char pending_theme_name[48];
static char pending_theme_version[48];
static char pending_theme_source[224];
static bool pending_theme_seen;
static char firmware_status_buf[256] = "firmware=none";
static char theme_status_buf[256] = "theme=none";
static fetch_ctx_t download_fc;
static download_job_t download_jobs[M65_DOWNLOAD_QUEUE_DEPTH];
static int download_active_job = -1;
static uint8_t download_next_slot;
static char download_status_buf[320] = "downloads=idle";
static bool autofetch_hash_active;
static uint32_t autofetch_hash_done;
static uint32_t autofetch_hash_size;
static absolute_time_t autofetch_hash_started_at;
static char autofetch_hash_stage[24];
static char autofetch_hash_path[224];
static char busy_template[M65_HTTP_BUSY_TEMPLATE_MAX];
static bool busy_template_loaded;
static uint8_t favicon_cache[M65_HTTP_FAVICON_MAX];
static size_t favicon_cache_len;
static bool favicon_cache_loaded;

static void remote_log(uint8_t level, const char *fmt, ...)
{
    if (remote_verbose < level) return;
    char msg[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    uart_cmd_log_puts_best_effort(msg);
    uart_cmd_log_puts_best_effort("\r\n");
}

void remote_http_set_verbose(uint8_t level)
{
    if (level > 2u) level = 2u;
    remote_verbose = level;
}

uint8_t remote_http_verbose(void)
{
    return remote_verbose;
}

static void remote_log_wifi_change(void)
{
    if (remote_verbose == 0) return;
    if (strcmp(last_wifi_log, http_status_buf) == 0) return;
    snprintf(last_wifi_log, sizeof last_wifi_log, "%s", http_status_buf);
    remote_log(1, "+WIFI: %s", http_status_buf);
}

#define WIFI_HW_PROBE_MAGIC 0x4d365748u
#define WIFI_HW_STAGE_INIT 1u
#define WIFI_HW_STAGE_STA  2u
#define WIFI_HW_STAGE_JOIN 3u

static const char *wifi_hw_stage_name(uint32_t stage)
{
    switch (stage) {
    case WIFI_HW_STAGE_INIT: return "init";
    case WIFI_HW_STAGE_STA: return "sta";
    case WIFI_HW_STAGE_JOIN: return "join";
    default: return "unknown";
    }
}

static void wifi_hw_probe_marker_clear(void)
{
    if (watchdog_hw->scratch[0] == WIFI_HW_PROBE_MAGIC) {
        watchdog_hw->scratch[0] = 0;
        watchdog_hw->scratch[1] = 0;
        watchdog_hw->scratch[2] = 0;
    }
}

static uint32_t future_seconds(absolute_time_t when)
{
    int64_t us = absolute_time_diff_us(get_absolute_time(), when);
    if (us <= 0) return 0u;
    return (uint32_t)((us + 999999ll) / 1000000ll);
}

static void wifi_hw_status_blocked(void)
{
    if (wifi_hardware_fault_stage == WIFI_HW_STAGE_INIT) {
        snprintf(http_status_buf, sizeof http_status_buf,
                 "wifi=disabled hardware=cyw43-timeout stage=%s attempts=%lu init_rc=%d",
                 wifi_hw_stage_name(wifi_hardware_fault_stage),
                 (unsigned long)wifi_probe_attempts,
                 wifi_init_rc);
    } else {
        snprintf(http_status_buf, sizeof http_status_buf,
                 "wifi=disabled hardware=cyw43 driver-timeout stage=%s attempts=%lu init_rc=%d",
                 wifi_hw_stage_name(wifi_hardware_fault_stage),
                 (unsigned long)wifi_probe_attempts,
                 wifi_init_rc);
    }
}

static bool wifi_probe_can_retry(void)
{
    return wifi_probe_attempts < M65_WIFI_PROBE_FAST_ATTEMPTS;
}

static uint32_t wifi_probe_retry_seconds(void)
{
    if (!wifi_probe_retry_scheduled) return 0u;
    return future_seconds(wifi_probe_retry_at);
}

static void wifi_schedule_probe_retry(const char *reason, uint32_t stage)
{
    wifi_hardware_blocked = false;
    wifi_hardware_fault_stage = stage;
    wifi_probe_pending = false;
    wifi_probe_retry_scheduled = true;
    wifi_probe_retry_at = make_timeout_time_ms(M65_WIFI_PROBE_FAST_RETRY_MS);
    snprintf(http_status_buf, sizeof http_status_buf,
             "wifi=probe-retry reason=%s stage=%s attempts=%lu in=%lus",
             reason ? reason : "probe-failed",
             wifi_hw_stage_name(stage),
             (unsigned long)wifi_probe_attempts,
             (unsigned long)(M65_WIFI_PROBE_FAST_RETRY_MS / 1000u));
}

static uint32_t wifi_recover_seconds(void)
{
    if (!wifi_recover_scheduled) return 0u;
    return future_seconds(wifi_recover_at);
}

static void wifi_schedule_recover_after(const char *reason, uint32_t delay_ms)
{
    wifi_assoc_state = WIFI_ASSOC_FAILED;
    http_is_active = false;
    wifi_noip_timer_active = false;
    close_http_listener();
    wifi_recover_scheduled = true;
    snprintf(wifi_recover_reason, sizeof wifi_recover_reason, "%s", reason ? reason : "unknown");
    if (delay_ms == 0) delay_ms = M65_WIFI_FIRST_RETRY_DELAY_MS;
    wifi_recover_at = make_timeout_time_ms(delay_ms);
    snprintf(http_status_buf, sizeof http_status_buf,
             "wifi=retry reason=%s in=%lus",
             wifi_recover_reason,
             (unsigned long)(delay_ms / 1000u));
}

static void wifi_schedule_recover(const char *reason)
{
    uint32_t delay_ms = wifi_recover_first_retry_used ? M65_WIFI_RETRY_DELAY_MS : M65_WIFI_FIRST_RETRY_DELAY_MS;
    wifi_recover_first_retry_used = true;
    wifi_schedule_recover_after(reason, delay_ms);
}

static void wifi_clear_recover(void)
{
    wifi_recover_scheduled = false;
    wifi_recover_reason[0] = 0;
}

void remote_http_defer_wifi_recovery(uint32_t quiet_ms)
{
    if (!wifi_recover_scheduled || wifi_assoc_state != WIFI_ASSOC_FAILED) return;
    wifi_recover_at = make_timeout_time_ms(quiet_ms ? quiet_ms : M65_WIFI_COMMAND_QUIET_MS);
    snprintf(http_status_buf, sizeof http_status_buf,
             "wifi=retry reason=%s in=%lus",
             wifi_recover_reason[0] ? wifi_recover_reason : "operator-activity",
             (unsigned long)((quiet_ms ? quiet_ms : M65_WIFI_COMMAND_QUIET_MS) / 1000u));
}

static const char default_top[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>MEGA65 JTAG</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;margin:0;background:#0a0c10;color:#e8edf3}"
    "main{width:min(980px,calc(100vw - 32px));margin:24px auto}"
    "h1{font-size:24px;margin:0}.firmware-version{margin:5px 0 0;color:#9fb0c3;font-size:13px}"
    ".grant,.boards a,.boards span,.update-panel{background:#121821;border:1px solid #273142}"
    ".grant{display:flex;gap:12px;flex-wrap:wrap;margin:0 0 16px;padding:12px 14px;border-left:5px solid #a43838}"
    ".grant-active,.grant-not-required{border-left-color:#2fa35b}.boards{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:16px}"
    ".boards a,.boards span{padding:7px 10px}.boards a.fetch-now{margin-left:auto;border-color:#ffd34d;background:#211b10;color:#ffd34d;font-weight:700}"
    ".update-panel{display:flex;gap:12px;flex-wrap:wrap;align-items:center;margin-bottom:16px;padding:12px 14px;border-left:5px solid #ffd34d}"
    ".update-panel form{margin:0 0 0 auto}.update-panel button{padding:7px 10px;border:1px solid #ffd34d;background:#211b10;color:#ffd34d;font:inherit;font-weight:700}"
    ".update-panel button:disabled{border-color:#536171;background:#18202b;color:#8f9bab}"
    "table{border-collapse:collapse;width:100%;background:#10161f}"
    "th,td{border-bottom:1px solid #263140;padding:10px 12px;text-align:left;vertical-align:top}"
    "th{background:#1b2531;color:#fff}.start{display:inline-block;margin-right:10px;padding:5px 9px;background:#1e7a46;color:#fff;text-decoration:none}"
    "tr[data-kind=PARTIAL] td{background:#151821;color:#96a3b2}tr[data-kind=PARTIAL] .start{background:#5e6671;cursor:default}"
    ".core-name{font-weight:700}.meta{font-size:13px;color:#9fb0c3;margin-top:4px}.meta:empty{display:none}"
    "a{color:#63b3ff}.delete{color:#ff7b7b;background:none;border:0;padding:0;font:inherit;text-decoration:underline;cursor:pointer}"
    "</style></head><body>"
    "<main><h1>Expansion Board Integrated JTAG</h1><p class=\"firmware-version\">{FIRMWARE_VERSION} build {FIRMWARE_BUILD}</p>"
    "<section class=\"grant grant-{WRITE_GRANT_STATUS}\"><strong>Write grant:</strong> {WRITE_GRANT_STATUS} "
    "<span>{WRITE_GRANT_MESSAGE}</span><span>Policy: {WRITE_GRANT_REQUIRED}</span></section>"
    "{FIRMWARE_UPDATE_PANEL}{THEME_INSTALL_PANEL}"
    "<nav class=\"boards\"><span>Showing: {BOARD_LABEL}</span><span>Path: {CURRENT_PATH}</span>{PARENT_LINK}<a href=\"{R3_URL}\">R3 cores</a> <a href=\"{R6_URL}\">R6 cores</a> <a class=\"fetch-now\" href=\"{FETCH_NOW_URL}\">Check for updated cores</a></nav>"
    "<table><thead><tr><th>Core</th><th>Bytes</th><th>Actions</th></tr></thead><tbody>\n";
static const char default_row[] =
    "<tr class=\"entry-row\" data-kind=\"{TYPE}\"><td><a class=\"start\" href=\"{PRIMARY_URL}\">{PRIMARY_LABEL}</a>"
    "<a class=\"core-name\" href=\"{PRIMARY_URL}\">{FILENAME}</a><div class=\"meta\">{CORE_META}</div></td>"
    "<td>{SIZE}</td><td>{ACTIONS}</td></tr>\n";
static const char default_bottom[] =
    "</tbody></table><script>"
    "function launchCoreLink(){return true;}"
    "function deleteCore(){alert('Delete requires the WWW interface files on the SD card.');}"
    "</script></main></body></html>\n";
static const char default_busy[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<meta http-equiv=\"refresh\" content=\"{FETCH_REFRESH_SECONDS}\">"
    "<title>MEGA65 JTAG busy</title>"
    "<style>"
    "body{margin:0;background:#090c11;color:#e8edf3;font-family:Arial,sans-serif}"
    "main{width:min(760px,calc(100vw - 32px));margin:48px auto}"
    ".panel{padding:28px;background:#10161f;border:1px solid #273142;border-left:6px solid #ffd34d}"
    "h1{margin:0 0 10px;font-size:28px}.status{color:#9fb0c3;word-break:break-word}"
    ".bar{height:36px;margin:22px 0;overflow:hidden;background:#1c1114;border:2px solid #ffd34d}"
    ".fill{height:100%;width:{FETCH_PERCENT}%;background:repeating-linear-gradient(135deg,#ffd34d 0,#ffd34d 16px,#d51c1c 16px,#d51c1c 32px);animation:move .45s linear infinite}"
    ".grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}.cell{padding:10px;background:#141b25;border:1px solid #273142}"
    "button{margin-top:22px;padding:11px 16px;background:#a43838;color:#fff;border:0;font-weight:700;cursor:pointer}"
    "@keyframes move{from{background-position:0 0}to{background-position:64px 0}}"
    "@media(max-width:620px){.grid{grid-template-columns:1fr}}"
    "</style></head><body><main><section class=\"panel\">"
    "<h1>Fetching cores</h1><p>{FETCH_ACTION}</p><div class=\"status\">{FETCH_STATUS}</div>"
    "<div class=\"bar\"><div class=\"fill\"></div></div>"
    "<div class=\"grid\">"
    "<div class=\"cell\"><strong>Stage</strong><br>{FETCH_STAGE}</div>"
    "<div class=\"cell\"><strong>File</strong><br>{FETCH_FILE}</div>"
    "<div class=\"cell\"><strong>Manifest</strong><br>{FETCH_CHECKED}/{FETCH_TOTAL} checked</div>"
    "<div class=\"cell\"><strong>Updates</strong><br>{FETCH_NEEDED} needed / {FETCH_UPDATED} completed</div>"
    "<div class=\"cell\"><strong>{FETCH_PROGRESS_LABEL}</strong><br>{FETCH_BYTES}/{FETCH_TOTAL_BYTES} bytes</div>"
    "<div class=\"cell\"><strong>Average rate</strong><br>{FETCH_RATE_KIBPS} KiB/s</div>"
    "</div>"
    "<form method=\"get\" action=\"/fetch/stop\"><button type=\"submit\">Stop fetch</button></form>"
    "</section></main></body></html>\n";

static bool ext_equal_ci(const char *a, const char *b);
static bool ci_equal(const char *a, const char *b);

static bool has_core_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char e1 = (char)tolower((unsigned char)dot[1]);
    char e2 = (char)tolower((unsigned char)dot[2]);
    char e3 = (char)tolower((unsigned char)dot[3]);
    char e4 = (char)tolower((unsigned char)dot[4]);
    char e5 = (char)tolower((unsigned char)dot[5]);
    return (e1 == 'b' && e2 == 'i' && e3 == 't' && e4 == 0) ||
           (e1 == 'c' && e2 == 'o' && e3 == 'r' && e4 == 0) ||
           (e1 == 'm' && e2 == '6' && e3 == '5' && e4 == 'j' && e5 == 0);
}

static bool has_manifest_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return dot[1] &&
           tolower((unsigned char)dot[1]) == 's' &&
           tolower((unsigned char)dot[2]) == 'h' &&
           tolower((unsigned char)dot[3]) == 'a' &&
           dot[4] == '2' && dot[5] == '5' && dot[6] == '6' && dot[7] == 0;
}

static bool has_firmware_ext(const char *name)
{
    const char *dot = strrchr(name ? name : "", '.');
    if (!dot) return false;
    return ext_equal_ci(dot, ".uf2") || ext_equal_ci(dot, ".m65fw") || ext_equal_ci(dot, ".bin");
}

static bool has_theme_ext(const char *name)
{
    const char *dot = strrchr(name ? name : "", '.');
    if (!dot) return false;
    return ext_equal_ci(dot, ".m65jtheme") || ext_equal_ci(dot, ".tar");
}

static const char *manifest_kind_name(manifest_kind_t kind)
{
    switch (kind) {
    case MANIFEST_KIND_CORE: return "core";
    case MANIFEST_KIND_FIRMWARE: return "firmware";
    case MANIFEST_KIND_THEME: return "theme";
    default: return "skip";
    }
}

static bool manifest_kind_stores_signed_transfer(manifest_kind_t kind)
{
    return kind == MANIFEST_KIND_FIRMWARE || kind == MANIFEST_KIND_THEME;
}

static bool parse_manifest_kind(const char *s, manifest_kind_t *kind)
{
    if (!s || !kind) return false;
    if (ci_equal(s, "core")) {
        *kind = MANIFEST_KIND_CORE;
        return true;
    }
    if (ci_equal(s, "firmware") || ci_equal(s, "fw")) {
        *kind = MANIFEST_KIND_FIRMWARE;
        return true;
    }
    if (ci_equal(s, "theme") || ci_equal(s, "www-theme")) {
        *kind = MANIFEST_KIND_THEME;
        return true;
    }
    return false;
}

static const char *path_basename_const(const char *path)
{
    const char *base = path ? strrchr(path, '/') : NULL;
    return base ? base + 1 : path;
}

static bool safe_theme_package_name(const char *name)
{
    if (!name || !name[0]) return false;
    size_t n = strlen(name);
    if (n > 180) return false;
    if (strchr(name, '/') || strchr(name, '\\') || strchr(name, ':') ||
        strchr(name, '*') || strchr(name, '?')) {
        return false;
    }
    if (strstr(name, "..")) return false;
    return has_theme_ext(name);
}

static bool make_theme_package_path_from_name(const char *name, char *out, size_t out_len)
{
    if (!safe_theme_package_name(name)) return false;
    return snprintf(out, out_len, "%s/%s", M65_THEME_DIR_PATH, name) < (int)out_len;
}

static bool make_theme_package_path_from_rel(const char *rel, char *out, size_t out_len)
{
    const char *base = path_basename_const(rel);
    return make_theme_package_path_from_name(base, out, out_len);
}

static const char *manifest_final_path(manifest_kind_t kind, const char *rel)
{
    if (kind == MANIFEST_KIND_FIRMWARE) return M65_FIRMWARE_PACKAGE_PATH;
    if (kind == MANIFEST_KIND_THEME) {
        static char theme_path[256];
        if (make_theme_package_path_from_rel(rel, theme_path, sizeof theme_path)) return theme_path;
        return M65_THEME_PACKAGE_PATH;
    }
    return rel;
}

static bool path_starts_with_ci(const char *s, const char *prefix)
{
    if (!s || !prefix) return false;
    while (*prefix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return false;
        s++;
        prefix++;
    }
    return true;
}

static bool safe_file_path(const char *path)
{
    if (!path || !path[0]) return false;
    size_t n = strlen(path);
    if (n > 220) return false;
    if (strstr(path, "..")) return false;
    if (strchr(path, '\\') || strchr(path, ':') || strchr(path, '*') || strchr(path, '?')) return false;
    if (path_starts_with_ci(path, "WWW/") || path_starts_with_ci(path, "DOWNLOADS/")) return false;
    return has_core_ext(path) || has_manifest_ext(path);
}

static bool safe_manifest_source_path(const char *path, manifest_kind_t kind)
{
    if (!path || !path[0]) return false;
    size_t n = strlen(path);
    if (n > 220) return false;
    if (path[0] == '/') return false;
    if (strstr(path, "..")) return false;
    if (strchr(path, '\\') || strchr(path, ':') || strchr(path, '*') || strchr(path, '?')) return false;
    if (kind == MANIFEST_KIND_CORE) return has_core_ext(path);
    if (kind == MANIFEST_KIND_FIRMWARE) return has_firmware_ext(path);
    if (kind == MANIFEST_KIND_THEME) return has_theme_ext(path);
    return false;
}

static bool safe_www_path(const char *path)
{
    if (!path || !path[0]) return false;
    size_t n = strlen(path);
    if (n > 220) return false;
    if (strstr(path, "..")) return false;
    if (path[0] == '/') return false;
    if (strchr(path, '\\') || strchr(path, ':') || strchr(path, '*') || strchr(path, '?')) return false;
    return true;
}

static bool safe_download_name(const char *path)
{
    if (!path || !path[0]) return false;
    size_t n = strlen(path);
    if (n > 180) return false;
    if (path[0] == '/') return false;
    if (strstr(path, "..")) return false;
    if (strchr(path, '\\') || strchr(path, ':') || strchr(path, '*') || strchr(path, '?')) return false;
    return true;
}

static bool fixed_download_slot_name(int slot, char *out, size_t out_len)
{
    if (slot < 0 || slot > 255) return false;
    return snprintf(out, out_len, "download-%02X.dat", (unsigned)slot) < (int)out_len;
}

static bool make_download_path(const char *name, char *out, size_t out_len)
{
    if (!safe_download_name(name)) return false;
    return snprintf(out, out_len, "DOWNLOADS/%s", name) < (int)out_len;
}

static bool ext_equal_ci(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static bool path_has_ext(const char *path, const char *ext)
{
    const char *dot = strrchr(path ? path : "", '.');
    return dot && ext_equal_ci(dot, ext);
}

static bool path_has_suffix_ci(const char *path, const char *suffix)
{
    size_t n = strlen(path ? path : "");
    size_t sn = strlen(suffix ? suffix : "");
    if (n < sn) return false;
    const char *tail = path + n - sn;
    return ext_equal_ci(tail, suffix);
}

static bool strip_partial_suffix(const char *path, char *out, size_t out_len)
{
    static const char suffix[] = ".partial";
    if (!path_has_suffix_ci(path, suffix)) return false;
    size_t n = strlen(path);
    size_t base_len = n - (sizeof suffix - 1u);
    if (base_len == 0 || base_len >= out_len) return false;
    memcpy(out, path, base_len);
    out[base_len] = 0;
    return true;
}

static bool is_partial_core_path(const char *path)
{
    char base[256];
    if (!strip_partial_suffix(path, base, sizeof base)) return false;
    return has_core_ext(base);
}

static void format_core_label_html(const char *path, const char *file_name, bool is_dir, char *out, size_t out_len)
{
    if (!out_len) return;
    out[0] = 0;
    if (!is_dir && is_partial_core_path(path)) {
        char base[256];
        if (strip_partial_suffix(file_name ? file_name : "", base, sizeof base)) {
            html_escape(base, out, out_len);
            return;
        }
    }
    if (!is_dir && path_has_ext(path, ".cor")) {
        core_file_t cf;
        if (core_open(&cf, path)) {
            if (cf.title[0]) {
                html_escape(cf.title, out, out_len);
                core_close(&cf);
                return;
            }
            core_close(&cf);
        }
    }
    html_escape(file_name ? file_name : "", out, out_len);
}

static void format_core_meta_html(const char *path, const char *file_name, char *out, size_t out_len)
{
    if (!out_len) return;
    out[0] = 0;
    if (is_partial_core_path(path)) {
        (void)file_name;
        snprintf(out, out_len, "Download in progress");
        return;
    }
    if (!path_has_ext(path, ".cor")) return;

    core_file_t cf;
    if (!core_open(&cf, path)) return;

    char filename[256], version[64], model[64];
    html_escape(file_name ? file_name : "", filename, sizeof filename);
    html_escape(cf.version, version, sizeof version);
    html_escape(cf.model, model, sizeof model);

    bool wrote = false;
    size_t pos = 0;
    if (cf.title[0] && filename[0]) {
        int n = snprintf(out + pos, out_len - pos, "%s", filename);
        if (n > 0) {
            pos += (size_t)n < out_len - pos ? (size_t)n : out_len - pos - 1u;
            wrote = true;
        }
    }
    if (version[0] && pos + 1 < out_len) {
        int n = snprintf(out + pos, out_len - pos, "%sVersion %s", wrote ? " | " : "", version);
        if (n > 0) {
            pos += (size_t)n < out_len - pos ? (size_t)n : out_len - pos - 1u;
            wrote = true;
        }
    }
    if (model[0] && pos + 1 < out_len) {
        snprintf(out + pos, out_len - pos, "%sModel %s", wrote ? " | " : "", model);
    } else if ((cf.model_id == 3 || cf.model_id == 6) && pos + 1 < out_len) {
        snprintf(out + pos, out_len - pos, "%sModel R%u", wrote ? " | " : "", (unsigned)cf.model_id);
    }
    core_close(&cf);
}

static const char *content_type_for_path(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (ext_equal_ci(dot, ".html") || ext_equal_ci(dot, ".htm")) return "text/html; charset=utf-8";
    if (ext_equal_ci(dot, ".css")) return "text/css; charset=utf-8";
    if (ext_equal_ci(dot, ".js")) return "application/javascript";
    if (ext_equal_ci(dot, ".png")) return "image/png";
    if (ext_equal_ci(dot, ".sha256") || ext_equal_ci(dot, ".txt")) return "text/plain; charset=utf-8";
    if (ext_equal_ci(dot, ".jpg") || ext_equal_ci(dot, ".jpeg")) return "image/jpeg";
    if (ext_equal_ci(dot, ".gif")) return "image/gif";
    if (ext_equal_ci(dot, ".svg")) return "image/svg+xml";
    if (ext_equal_ci(dot, ".ico")) return "image/x-icon";
    return "application/octet-stream";
}

static uint32_t lwip_ip_to_host_u32(const ip_addr_t *ip)
{
    const ip4_addr_t *ip4 = ip_2_ip4(ip);
    return lwip_ntohl(ip4_addr_get_u32(ip4));
}

static void cfg_ip_to_lwip(uint32_t ip, ip4_addr_t *out)
{
    out->addr = PP_HTONL(ip);
}

static void page_append(page_builder_t *pb, const char *s)
{
    if (!pb || !s || pb->len >= pb->cap) return;
    size_t n = strlen(s);
    if (n > pb->cap - pb->len - 1u) n = pb->cap - pb->len - 1u;
    memcpy(pb->buf + pb->len, s, n);
    pb->len += n;
    pb->buf[pb->len] = 0;
}

static void page_append_n(page_builder_t *pb, const char *s, size_t n)
{
    if (!pb || !s || pb->len >= pb->cap) return;
    if (n > pb->cap - pb->len - 1u) n = pb->cap - pb->len - 1u;
    memcpy(pb->buf + pb->len, s, n);
    pb->len += n;
    pb->buf[pb->len] = 0;
}

static void html_escape(const char *in, char *out, size_t out_len)
{
    size_t pos = 0;
    if (!out_len) return;
    for (; in && *in && pos + 1 < out_len; in++) {
        const char *rep = NULL;
        switch (*in) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        default: break;
        }
        if (rep) {
            size_t n = strlen(rep);
            if (pos + n >= out_len) break;
            memcpy(out + pos, rep, n);
            pos += n;
        } else {
            out[pos++] = *in;
        }
    }
    out[pos] = 0;
}

static void url_encode(const char *in, char *out, size_t out_len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0;
    if (!out_len) return;
    for (; in && *in && pos + 1 < out_len; in++) {
        unsigned char c = (unsigned char)*in;
        bool keep = isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/';
        if (keep) {
            out[pos++] = (char)c;
        } else {
            if (pos + 3 >= out_len) break;
            out[pos++] = '%';
            out[pos++] = hex[c >> 4];
            out[pos++] = hex[c & 0x0f];
        }
    }
    out[pos] = 0;
}

static void js_string_escape(const char *in, char *out, size_t out_len)
{
    size_t pos = 0;
    if (!out_len) return;
    for (; in && *in && pos + 1 < out_len; in++) {
        unsigned char c = (unsigned char)*in;
        const char *rep = NULL;
        switch (c) {
        case '\\': rep = "\\\\"; break;
        case '\'': rep = "\\'"; break;
        case '\n': rep = "\\n"; break;
        case '\r': rep = "\\r"; break;
        case '\t': rep = "\\t"; break;
        default: break;
        }
        if (rep) {
            size_t n = strlen(rep);
            if (pos + n >= out_len) break;
            memcpy(out + pos, rep, n);
            pos += n;
        } else if (c < 0x20) {
            if (pos + 4 >= out_len) break;
            snprintf(out + pos, out_len - pos, "\\x%02x", c);
            pos += 4;
        } else {
            out[pos++] = (char)c;
        }
    }
    out[pos] = 0;
}

static bool safe_index_path(const char *path)
{
    if (!path || !path[0]) return true;
    if (strcmp(path, "/") == 0) return true;
    size_t n = strlen(path);
    if (n > 180) return false;
    if (path[0] == '/') return false;
    if (strstr(path, "..")) return false;
    if (strchr(path, '\\') || strchr(path, ':') || strchr(path, '*') || strchr(path, '?')) return false;
    return true;
}

static void normalise_index_path(const char *path, char *out, size_t out_len)
{
    if (!out_len) return;
    if (!path || !path[0] || strcmp(path, "/") == 0) {
        snprintf(out, out_len, "/");
        return;
    }
    snprintf(out, out_len, "%s", path);
    size_t n = strlen(out);
    while (n > 1 && out[n - 1] == '/') out[--n] = 0;
}

static bool index_path_from_target(const char *target, char *out, size_t out_len)
{
    char decoded[192];
    if (!parse_query_value(target, "path", decoded, sizeof decoded)) {
        normalise_index_path("/", out, out_len);
        return true;
    }
    normalise_index_path(decoded, out, out_len);
    return safe_index_path(out);
}

static bool make_child_path(const char *dir_path, const char *name, char *out, size_t out_len)
{
    if (!name || !name[0]) return false;
    if (!dir_path || !dir_path[0] || strcmp(dir_path, "/") == 0) {
        return snprintf(out, out_len, "%s", name) < (int)out_len;
    }
    return snprintf(out, out_len, "%s/%s", dir_path, name) < (int)out_len;
}

static bool make_index_url(const char *dir_path, uint8_t board_rev, char *out, size_t out_len)
{
    char encoded[256];
    const bool root = !dir_path || !dir_path[0] || strcmp(dir_path, "/") == 0;
    if (root && board_rev != 3 && board_rev != 6) {
        return snprintf(out, out_len, "/index.html") < (int)out_len;
    }
    if (!root) {
        url_encode(dir_path, encoded, sizeof encoded);
    } else {
        encoded[0] = 0;
    }
    if (board_rev == 3 || board_rev == 6) {
        if (root) return snprintf(out, out_len, "/index.html?board=%u", (unsigned)board_rev) < (int)out_len;
        return snprintf(out, out_len, "/index.html?path=%s&board=%u", encoded, (unsigned)board_rev) < (int)out_len;
    }
    return snprintf(out, out_len, "/index.html?path=%s", encoded) < (int)out_len;
}

static bool make_fetch_now_url(const char *dir_path, uint8_t board_rev, char *out, size_t out_len)
{
    char return_url[256];
    char encoded_return[320];
    if (!make_index_url(dir_path, board_rev, return_url, sizeof return_url)) {
        snprintf(return_url, sizeof return_url, "/index.html");
    }
    url_encode(return_url, encoded_return, sizeof encoded_return);
    if (board_rev == 3 || board_rev == 6) {
        return snprintf(out, out_len,
                        "/fetch/now?board=%u&return=%s",
                        (unsigned)board_rev,
                        encoded_return) < (int)out_len;
    }
    return snprintf(out, out_len, "/fetch/now?return=%s", encoded_return) < (int)out_len;
}

static bool make_parent_path(const char *dir_path, char *out, size_t out_len)
{
    if (!dir_path || !dir_path[0] || strcmp(dir_path, "/") == 0) return false;
    snprintf(out, out_len, "%s", dir_path);
    char *slash = strrchr(out, '/');
    if (!slash) {
        snprintf(out, out_len, "/");
    } else if (slash == out) {
        slash[1] = 0;
    } else {
        *slash = 0;
    }
    return true;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

static bool url_decode(const char *in, char *out, size_t out_len)
{
    size_t pos = 0;
    if (!out_len) return false;
    while (in && *in && pos + 1 < out_len) {
        if (*in == '%') {
            int h1 = hex_value(in[1]);
            int h2 = hex_value(in[2]);
            if (h1 < 0 || h2 < 0) return false;
            out[pos++] = (char)((h1 << 4) | h2);
            in += 3;
        } else if (*in == '+') {
            out[pos++] = ' ';
            in++;
        } else {
            out[pos++] = *in++;
        }
    }
    out[pos] = 0;
    return in && *in == 0;
}

static bool safe_channel_name(const char *s, char *out, size_t out_len)
{
    size_t pos = 0;
    if (!s || !s[0] || !out_len) return false;
    for (; *s && pos + 1 < out_len; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c)) {
            out[pos++] = (char)tolower(c);
        } else if (c == '-' || c == '_' || c == '.') {
            out[pos++] = '-';
        } else {
            return false;
        }
    }
    while (pos > 0 && out[pos - 1] == '-') pos--;
    out[pos] = 0;
    return pos > 0 && *s == 0;
}

static bool join_url_path(const char *base, const char *rel, char *out, size_t out_len)
{
    if (!base || !base[0] || !rel || !rel[0]) return false;
    char encoded[256];
    url_encode(rel, encoded, sizeof encoded);
    size_t n = strlen(base);
    const char *slash = (n && base[n - 1] == '/') ? "" : "/";
    return snprintf(out, out_len, "%s%s%s", base, slash, encoded) < (int)out_len;
}

static bool hex_to_bytes32(const char *hex, uint8_t out[32])
{
    if (!hex) return false;
    for (unsigned i = 0; i < 32; i++) {
        int hi = hex_value(hex[i * 2]);
        int lo = hex_value(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return hex[64] == 0;
}

static void bytes32_to_hex(const uint8_t in[32], char out[65])
{
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; i++) {
        out[i * 2] = hex[in[i] >> 4];
        out[i * 2 + 1] = hex[in[i] & 0x0f];
    }
    out[64] = 0;
}

static void autofetch_hash_status_update(const char *stage,
                                         const char *path,
                                         uint32_t done,
                                         uint32_t size)
{
    unsigned long percent = size ? (unsigned long)(((uint64_t)done * 100u) / size) : 0ul;
    snprintf(autofetch_status_buf, sizeof autofetch_status_buf,
             "autofetch=running stage=%s-hash file=%s bytes=%lu/%lu percent=%lu checked=%lu total=%lu needed=%lu updated=%lu",
             stage ? stage : "file",
             path ? path : "(unset)",
             (unsigned long)done,
             (unsigned long)size,
             percent,
             (unsigned long)autofetch_checked_count,
             (unsigned long)autofetch_manifest_total_count,
             (unsigned long)autofetch_needed_count,
             (unsigned long)autofetch_updated_count);
}

static bool file_sha256_hex(const char *path, char out[65], const char *autofetch_stage)
{
    storage_file_t f = {0};
    if (!storage_open(&f, path)) return false;

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    if (mbedtls_sha256_starts(&sha, 0) != 0) {
        mbedtls_sha256_free(&sha);
        storage_close(&f);
        return false;
    }

    uint8_t buf[M65_HTTP_IO_CHUNK];
    uint32_t done = 0;
    uint32_t size = storage_size(&f);
    absolute_time_t start = get_absolute_time();
    absolute_time_t last_report = start;
    uint32_t last_report_done = 0;
    bool report = autofetch_stage && autofetch_stage[0];
    if (report) {
        autofetch_hash_active = true;
        autofetch_hash_done = 0;
        autofetch_hash_size = size;
        autofetch_hash_started_at = start;
        snprintf(autofetch_hash_stage, sizeof autofetch_hash_stage, "%s-hash", autofetch_stage);
        snprintf(autofetch_hash_path, sizeof autofetch_hash_path, "%s", path);
        autofetch_hash_status_update(autofetch_stage, path, 0, size);
        remote_log(1, "+AUTOFETCH: verifying hash of file start stage=%s file=%s bytes=%lu",
                   autofetch_stage,
                   path,
                   (unsigned long)size);
    }
    bool ok = true;
    while (done < size) {
        size_t want = size - done;
        if (want > sizeof buf) want = sizeof buf;
        size_t got = 0;
        if (!storage_read(&f, buf, want, &got) || got == 0) {
            ok = false;
            break;
        }
        if (mbedtls_sha256_update(&sha, buf, got) != 0) {
            ok = false;
            break;
        }
        done += (uint32_t)got;
        if (report) {
            autofetch_hash_done = done;
            cyw43_arch_poll();
            absolute_time_t now = get_absolute_time();
            bool time_due = absolute_time_diff_us(last_report, now) >= (int64_t)M65_FILE_HASH_PROGRESS_MS * 1000ll;
            bool bytes_due = done - last_report_done >= M65_FILE_HASH_PROGRESS_BYTES;
            if (done == size || time_due || bytes_due) {
                int64_t us = absolute_time_diff_us(start, now);
                if (us <= 0) us = 1;
                unsigned long percent = size ? (unsigned long)(((uint64_t)done * 100u) / size) : 0ul;
                unsigned long avg_KiBps = (unsigned long)(((uint64_t)done * 1000000u) / ((uint64_t)us * 1024u));
                autofetch_hash_status_update(autofetch_stage, path, done, size);
                remote_log(remote_verbose >= 2u ? 2u : 1u,
                           "+AUTOFETCH: verifying hash of file progress stage=%s file=%s bytes=%lu/%lu percent=%lu avg_KiBps=%lu",
                           autofetch_stage,
                           path,
                           (unsigned long)done,
                           (unsigned long)size,
                           percent,
                           avg_KiBps);
                last_report = now;
                last_report_done = done;
            }
        }
    }
    storage_close(&f);

    uint8_t digest[32];
    if (ok && mbedtls_sha256_finish(&sha, digest) == 0) {
        bytes32_to_hex(digest, out);
    } else {
        ok = false;
    }
    mbedtls_sha256_free(&sha);
    if (report) {
        int64_t us = absolute_time_diff_us(start, get_absolute_time());
        if (us < 0) us = 0;
        remote_log(1, "+AUTOFETCH: verifying hash of file %s stage=%s file=%s bytes=%lu time_ms=%lu",
                   ok ? "done" : "failed",
                   autofetch_stage,
                   path,
                   (unsigned long)done,
                   (unsigned long)(us / 1000ll));
        autofetch_hash_active = false;
        autofetch_hash_done = 0;
        autofetch_hash_size = 0;
        autofetch_hash_stage[0] = 0;
        autofetch_hash_path[0] = 0;
    }
    return ok;
}

static bool ci_equal_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    }
    return true;
}

static bool ci_contains_n(const char *haystack, size_t haystack_len, const char *needle)
{
    size_t needle_len = strlen(needle);
    if (!needle_len) return true;
    if (!haystack || haystack_len < needle_len) return false;
    for (size_t i = 0; i <= haystack_len - needle_len; i++) {
        if (ci_equal_n(haystack + i, needle, needle_len)) return true;
    }
    return false;
}

static bool ci_equal(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static bool starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static const char *header_value(const char *headers, const char *key)
{
    size_t key_len = strlen(key);
    const char *p = headers;
    while (p && *p) {
        const char *line_end = strstr(p, "\r\n");
        if (!line_end) line_end = strchr(p, '\n');
        if (!line_end) break;
        if ((size_t)(line_end - p) == 0) break;
        if ((size_t)(line_end - p) > key_len && ci_equal_n(p, key, key_len) && p[key_len] == ':') {
            p += key_len + 1;
            while (*p == ' ' || *p == '\t') p++;
            return p;
        }
        p = (*line_end == '\r' && line_end[1] == '\n') ? line_end + 2 : line_end + 1;
    }
    return NULL;
}

static size_t header_value_len(const char *value)
{
    size_t n = 0;
    while (value && value[n] && value[n] != '\r' && value[n] != '\n') n++;
    return n;
}

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return 26 + c - 'a';
    if (c >= '0' && c <= '9') return 52 + c - '0';
    if (c == '+') return 62;
    if (c == '/') return 63;
    if (c == '=') return -2;
    return -1;
}

static bool base64_decode(const char *in, size_t in_len, char *out, size_t out_len)
{
    size_t pos = 0;
    int val = 0;
    int bits = -8;
    for (size_t i = 0; i < in_len; i++) {
        if (isspace((unsigned char)in[i])) continue;
        int v = b64_value(in[i]);
        if (v == -2) break;
        if (v < 0) return false;
        val = (val << 6) | v;
        bits += 6;
        if (bits >= 0) {
            if (pos + 1 >= out_len) return false;
            out[pos++] = (char)((val >> bits) & 0xff);
            bits -= 8;
        }
    }
    if (pos >= out_len) return false;
    out[pos] = 0;
    return true;
}

static bool basic_auth_ok(const http_conn_t *c)
{
    if (!http_cfg.http_user[0] && !http_cfg.http_password[0]) return true;
    const char *auth = header_value(c->header, "Authorization");
    if (!auth) return false;
    size_t auth_len = header_value_len(auth);
    if (auth_len < 7 || !ci_equal_n(auth, "Basic ", 6)) return false;

    char decoded[112];
    if (!base64_decode(auth + 6, auth_len - 6, decoded, sizeof decoded)) return false;

    char expected[112];
    snprintf(expected, sizeof expected, "%s:%s", http_cfg.http_user, http_cfg.http_password);
    return strcmp(decoded, expected) == 0;
}

static bool load_template(const char *path, char *buf, size_t buflen, const char *fallback)
{
    if (!buflen) return false;
    storage_file_t f = {0};
    if (storage_open(&f, path)) {
        uint32_t size = storage_size(&f);
        if (size < buflen) {
            size_t got = 0;
            if (storage_read(&f, buf, size, &got) && got == size) {
                buf[size] = 0;
                storage_close(&f);
                return true;
            }
        }
        storage_close(&f);
    }
    snprintf(buf, buflen, "%s", fallback);
    return false;
}

static bool load_binary_asset(const char *path, uint8_t *buf, size_t buflen, size_t *size_out)
{
    if (size_out) *size_out = 0;
    if (!path || !buf || buflen == 0) return false;

    storage_file_t f = {0};
    if (!storage_open(&f, path)) return false;
    uint32_t size = storage_size(&f);
    if (size == 0 || size > buflen) {
        storage_close(&f);
        return false;
    }
    size_t got = 0;
    bool ok = storage_read(&f, buf, size, &got) && got == size;
    storage_close(&f);
    if (!ok) return false;
    if (size_out) *size_out = (size_t)size;
    return true;
}

static void load_cached_web_assets(void)
{
    busy_template_loaded = load_template("WWW/fetch_busy.html",
                                         busy_template,
                                         sizeof busy_template,
                                         default_busy);
    favicon_cache_loaded = load_binary_asset("WWW/favicon-32x32.png",
                                             favicon_cache,
                                             sizeof favicon_cache,
                                             &favicon_cache_len);
}

static bool storage_file_exists(const char *path)
{
    storage_file_t f = {0};
    if (!storage_open(&f, path)) return false;
    storage_close(&f);
    return true;
}

static bool write_text_file(const char *path, const char *text)
{
    storage_file_t f = {0};
    if (!storage_open_write(&f, path, true)) return false;
    size_t put = 0;
    size_t len = strlen(text ? text : "");
    bool ok = storage_write(&f, text ? text : "", len, &put) && put == len && storage_sync(&f);
    storage_close(&f);
    return ok;
}

static void record_firmware_candidate(const manifest_entry_t *entry)
{
    if (!entry) return;
    snprintf(pending_firmware_version, sizeof pending_firmware_version, "%s", entry->version);
    snprintf(pending_firmware_build, sizeof pending_firmware_build, "%s", entry->build);
    snprintf(pending_firmware_source, sizeof pending_firmware_source, "%s", entry->rel);
    pending_firmware_seen = true;

    char info[256];
    snprintf(info, sizeof info,
             "version=%s\nbuild=%s\nsource=%s\n",
             pending_firmware_version,
             pending_firmware_build,
             pending_firmware_source);
    (void)write_text_file(M65_FIRMWARE_INFO_PATH, info);
}

static void record_theme_candidate(const manifest_entry_t *entry)
{
    if (!entry) return;
    snprintf(pending_theme_name, sizeof pending_theme_name, "%s", entry->name[0] ? entry->name : "theme");
    snprintf(pending_theme_version, sizeof pending_theme_version, "%s", entry->version);
    snprintf(pending_theme_source, sizeof pending_theme_source, "%s", entry->rel);
    pending_theme_seen = true;

    char info[256];
    snprintf(info, sizeof info,
             "name=%s\nversion=%s\nsource=%s\n",
             pending_theme_name,
             pending_theme_version,
             pending_theme_source);
    (void)write_text_file(M65_THEME_INFO_PATH, info);
}

static void parse_info_line(char *line, char *key, size_t key_len, char *value, size_t value_len)
{
    if (key_len) key[0] = 0;
    if (value_len) value[0] = 0;
    char *eq = strchr(line, '=');
    if (!eq) return;
    *eq++ = 0;
    snprintf(key, key_len, "%s", line);
    snprintf(value, value_len, "%s", eq);
}

static void load_pending_info_file(const char *path,
                                   char *a,
                                   size_t a_len,
                                   const char *a_key,
                                   char *b,
                                   size_t b_len,
                                   const char *b_key,
                                   char *source,
                                   size_t source_len)
{
    storage_file_t f = {0};
    if (!storage_open(&f, path)) return;
    uint32_t size = storage_size(&f);
    if (size > 255u) size = 255u;
    char buf[256];
    size_t got = 0;
    bool ok = storage_read(&f, buf, size, &got);
    storage_close(&f);
    if (!ok) return;
    buf[got] = 0;

    char *p = buf;
    while (*p) {
        char *line = p;
        char *nl = strchr(p, '\n');
        if (nl) {
            *nl = 0;
            p = nl + 1;
        } else {
            p += strlen(p);
        }
        char key[32], value[160];
        parse_info_line(line, key, sizeof key, value, sizeof value);
        if (a && a_key && ci_equal(key, a_key)) snprintf(a, a_len, "%s", value);
        else if (b && b_key && ci_equal(key, b_key)) snprintf(b, b_len, "%s", value);
        else if (source && ci_equal(key, "source")) snprintf(source, source_len, "%s", value);
    }
}

static bool firmware_update_available(void)
{
    if (!storage_file_exists(M65_FIRMWARE_PACKAGE_PATH)) return false;
    if (!pending_firmware_seen) {
        load_pending_info_file(M65_FIRMWARE_INFO_PATH,
                               pending_firmware_version, sizeof pending_firmware_version, "version",
                               pending_firmware_build, sizeof pending_firmware_build, "build",
                               pending_firmware_source, sizeof pending_firmware_source);
        pending_firmware_seen = pending_firmware_version[0] || pending_firmware_build[0] || pending_firmware_source[0];
    }
    if (pending_firmware_build[0]) return strcmp(pending_firmware_build, M65_BUILD_MARKER) != 0;
    if (pending_firmware_version[0]) return strcmp(pending_firmware_version, M65_VERSION_STRING) != 0;
    return true;
}

typedef struct {
    char first[192];
    uint32_t count;
} theme_scan_ctx_t;

static void theme_scan_cb(const char *name, uint32_t size, bool is_dir, void *ctx)
{
    (void)size;
    theme_scan_ctx_t *ts = (theme_scan_ctx_t *)ctx;
    if (!ts || is_dir || !safe_theme_package_name(name)) return;
    if (!ts->first[0]) snprintf(ts->first, sizeof ts->first, "%s", name);
    ts->count++;
}

static uint32_t theme_scan(theme_scan_ctx_t *out)
{
    theme_scan_ctx_t ts = {0};
    (void)storage_list_dir(M65_THEME_DIR_PATH, theme_scan_cb, &ts);
    if (out) *out = ts;
    return ts.count;
}

static bool theme_update_available(void)
{
    if (!pending_theme_seen) {
        load_pending_info_file(M65_THEME_INFO_PATH,
                               pending_theme_name, sizeof pending_theme_name, "name",
                               pending_theme_version, sizeof pending_theme_version, "version",
                               pending_theme_source, sizeof pending_theme_source);
        pending_theme_seen = pending_theme_name[0] || pending_theme_version[0] || pending_theme_source[0];
    }
    return theme_scan(NULL) > 0;
}

static bool theme_package_exists_by_name(const char *name, char *path, size_t path_len)
{
    if (!safe_theme_package_name(name)) return false;
    char candidate[256];
    if (make_theme_package_path_from_name(name, candidate, sizeof candidate) &&
        storage_file_exists(candidate)) {
        if (path && path_len) snprintf(path, path_len, "%s", candidate);
        return true;
    }
    const char *legacy = path_basename_const(M65_THEME_PACKAGE_PATH);
    if (legacy && ci_equal(name, legacy) && storage_file_exists(M65_THEME_PACKAGE_PATH)) {
        if (path && path_len) snprintf(path, path_len, "%s", M65_THEME_PACKAGE_PATH);
        return true;
    }
    return false;
}

static bool select_theme_package(const char *requested, char *path, size_t path_len, char *name, size_t name_len)
{
    if (path && path_len) path[0] = 0;
    if (name && name_len) name[0] = 0;

    if (requested && requested[0]) {
        if (!safe_theme_package_name(requested)) return false;
        if (!theme_package_exists_by_name(requested, path, path_len)) return false;
        if (name && name_len) snprintf(name, name_len, "%s", requested);
        return true;
    }

    if (!pending_theme_seen) {
        (void)theme_update_available();
    }
    if (pending_theme_source[0]) {
        const char *base = path_basename_const(pending_theme_source);
        if (base && theme_package_exists_by_name(base, path, path_len)) {
            if (name && name_len) snprintf(name, name_len, "%s", base);
            return true;
        }
    }

    theme_scan_ctx_t scan;
    if (theme_scan(&scan) && scan.first[0] &&
        theme_package_exists_by_name(scan.first, path, path_len)) {
        if (name && name_len) snprintf(name, name_len, "%s", scan.first);
        return true;
    }

    return false;
}

static const char *autofetch_stage_name(void)
{
    if (autofetch_hash_active) return "checking SD file";
    switch (autofetch_state) {
    case AUTOFETCH_MANIFEST: return "downloading manifest";
    case AUTOFETCH_SCAN: return "checking manifest";
    case AUTOFETCH_VERIFY: return "verifying download";
    case AUTOFETCH_CORE: return "downloading file";
    default: return "idle";
    }
}

static const char *autofetch_action_text(void)
{
    if (autofetch_hash_active) {
        if (strncmp(autofetch_hash_stage, "verify", 6) == 0) return "Verifying the downloaded file on the SD card";
        return "Hash checking an existing SD-card file";
    }
    switch (autofetch_state) {
    case AUTOFETCH_MANIFEST: return "Downloading and verifying the signed manifest";
    case AUTOFETCH_SCAN: return "Comparing the manifest against files already on the SD card";
    case AUTOFETCH_VERIFY: return "Verifying the downloaded file before committing it";
    case AUTOFETCH_CORE: return "Downloading a changed file from the mirror";
    default: return autofetch_running ? "Updating the SD card" : "Fetch is idle";
    }
}

static const char *autofetch_progress_label(void)
{
    if (autofetch_hash_active) return "SD hash progress";
    if (autofetch_state == AUTOFETCH_MANIFEST || autofetch_state == AUTOFETCH_CORE) return "Download progress";
    if (autofetch_state == AUTOFETCH_VERIFY) return "Verification progress";
    return "Current progress";
}

static const char *autofetch_current_file(void)
{
    if (autofetch_hash_active && autofetch_hash_path[0]) return autofetch_hash_path;
    if (autofetch_state == AUTOFETCH_MANIFEST) return autofetch_manifest_path;
    if (autofetch_state == AUTOFETCH_CORE || autofetch_state == AUTOFETCH_VERIFY) return autofetch_pending_path;
    return autofetch_manifest_path[0] ? autofetch_manifest_path : "(none)";
}

static uint32_t autofetch_current_bytes(void)
{
    if (autofetch_hash_active) return autofetch_hash_done;
    if (autofetch_state == AUTOFETCH_MANIFEST || autofetch_state == AUTOFETCH_CORE) {
        return autofetch_fc.body_done;
    }
    return 0;
}

static uint32_t autofetch_current_total_bytes(void)
{
    if (autofetch_hash_active) return autofetch_hash_size;
    if ((autofetch_state == AUTOFETCH_MANIFEST || autofetch_state == AUTOFETCH_CORE) &&
        autofetch_fc.content_length) {
        return autofetch_fc.content_length;
    }
    return 0;
}

static uint32_t autofetch_current_percent(void)
{
    uint32_t total = autofetch_current_total_bytes();
    if (!total) return (autofetch_running || autofetch_state != AUTOFETCH_IDLE) ? 0u : 100u;
    return (uint32_t)(((uint64_t)autofetch_current_bytes() * 100u) / total);
}

static uint32_t autofetch_current_rate_kibps(void)
{
    if (autofetch_hash_active && autofetch_hash_size) {
        int64_t us = absolute_time_diff_us(autofetch_hash_started_at, get_absolute_time());
        if (us <= 0) return 0;
        return (uint32_t)(((uint64_t)autofetch_hash_done * 1000000u) / ((uint64_t)us * 1024u));
    }
    if (!(autofetch_state == AUTOFETCH_MANIFEST || autofetch_state == AUTOFETCH_CORE) ||
        !autofetch_fc.content_length) {
        return 0;
    }
    int64_t us = absolute_time_diff_us(autofetch_fc.body_started_at, get_absolute_time());
    if (us <= 0) return 0;
    return (uint32_t)(((uint64_t)autofetch_fc.body_done * 1000000u) / ((uint64_t)us * 1024u));
}

static void substitution_value(const char *name,
                               const char *file_name,
                               const char *file_path,
                               const char *dir_path,
                               uint32_t size,
                               bool is_dir,
                               uint8_t board_rev,
                               char *out,
                               size_t out_len)
{
    char escaped[256];
    char encoded[256];
    char encoded_return[320];
    char js_name[256];
    char url[320];
    char tmp[300];
    bool is_partial = !is_dir && is_partial_core_path(file_path);
    bool is_theme = !is_dir && has_theme_ext(file_path);
    if (ci_equal(name, "WRITE_GRANT_STATUS")) {
        if (!http_cfg.require_write_grant) snprintf(out, out_len, "not-required");
        else snprintf(out, out_len, "%s", write_gate_active() ? "active" : "inactive");
    } else if (ci_equal(name, "WRITE_GRANT_SECONDS")) {
        uint32_t seconds = write_gate_remaining_ms() / 1000u;
        if (seconds) snprintf(out, out_len, "%lu", (unsigned long)seconds);
        else snprintf(out, out_len, "");
    } else if (ci_equal(name, "WRITE_GRANT_MS")) {
        snprintf(out, out_len, "%lu", (unsigned long)write_gate_remaining_ms());
    } else if (ci_equal(name, "WRITE_GRANT_REQUIRED")) {
        snprintf(out, out_len, "%s", http_cfg.require_write_grant ? "required" : "not required");
    } else if (ci_equal(name, "WRITE_GRANT_MESSAGE")) {
        uint32_t seconds = write_gate_remaining_ms() / 1000u;
        if (!http_cfg.require_write_grant) {
            snprintf(out, out_len, "Write grant not required");
        } else if (seconds) {
            snprintf(out, out_len, "%lu seconds remaining", (unsigned long)seconds);
        } else {
            snprintf(out, out_len, "Press button above user-port to enable writing");
        }
    } else if (ci_equal(name, "FETCH_STATUS")) {
        html_escape(autofetch_status_buf[0] ? autofetch_status_buf : "autofetch=idle", out, out_len);
    } else if (ci_equal(name, "FETCH_STAGE")) {
        snprintf(out, out_len, "%s", autofetch_stage_name());
    } else if (ci_equal(name, "FETCH_ACTION")) {
        html_escape(autofetch_action_text(), out, out_len);
    } else if (ci_equal(name, "FETCH_FILE")) {
        html_escape(autofetch_current_file(), out, out_len);
    } else if (ci_equal(name, "FETCH_CHANNEL")) {
        html_escape(autofetch_channel, out, out_len);
    } else if (ci_equal(name, "FETCH_CHECKED")) {
        snprintf(out, out_len, "%lu", (unsigned long)autofetch_checked_count);
    } else if (ci_equal(name, "FETCH_TOTAL")) {
        snprintf(out, out_len, "%lu", (unsigned long)autofetch_manifest_total_count);
    } else if (ci_equal(name, "FETCH_NEEDED") || ci_equal(name, "FETCH_FOUND")) {
        snprintf(out, out_len, "%lu", (unsigned long)autofetch_needed_count);
    } else if (ci_equal(name, "FETCH_UPDATED")) {
        snprintf(out, out_len, "%lu", (unsigned long)autofetch_updated_count);
    } else if (ci_equal(name, "FETCH_BYTES")) {
        snprintf(out, out_len, "%lu", (unsigned long)autofetch_current_bytes());
    } else if (ci_equal(name, "FETCH_TOTAL_BYTES")) {
        snprintf(out, out_len, "%lu", (unsigned long)autofetch_current_total_bytes());
    } else if (ci_equal(name, "FETCH_PERCENT")) {
        snprintf(out, out_len, "%lu", (unsigned long)autofetch_current_percent());
    } else if (ci_equal(name, "FETCH_RATE_KIBPS") || ci_equal(name, "FETCH_AVG_KIBPS")) {
        snprintf(out, out_len, "%lu", (unsigned long)autofetch_current_rate_kibps());
    } else if (ci_equal(name, "FETCH_PROGRESS_LABEL")) {
        html_escape(autofetch_progress_label(), out, out_len);
    } else if (ci_equal(name, "FETCH_RETRY")) {
        snprintf(out, out_len, "%lu", (unsigned long)autofetch_fetch_retry_count);
    } else if (ci_equal(name, "FETCH_RETRIES")) {
        snprintf(out, out_len, "%lu", (unsigned long)M65_AUTOFETCH_FILE_RETRIES);
    } else if (ci_equal(name, "FETCH_REFRESH_SECONDS")) {
        snprintf(out, out_len, "1");
    } else if (ci_equal(name, "FIRMWARE_VERSION")) {
        html_escape(M65_VERSION_STRING, out, out_len);
    } else if (ci_equal(name, "FIRMWARE_BUILD")) {
        html_escape(M65_BUILD_MARKER, out, out_len);
    } else if (ci_equal(name, "FIRMWARE_STATUS")) {
        html_escape(remote_http_firmware_status(), out, out_len);
    } else if (ci_equal(name, "FIRMWARE_UPDATE_PANEL")) {
        if (!firmware_update_available()) {
            snprintf(out, out_len, "");
        } else {
            char version[80], build[96];
            html_escape(pending_firmware_version[0] ? pending_firmware_version : "unknown", version, sizeof version);
            html_escape(pending_firmware_build[0] ? pending_firmware_build : "unknown", build, sizeof build);
#if M65_ENABLE_MCUBOOT_OTA
            snprintf(out, out_len,
                     "<section class=\"update-panel firmware-update\"><strong>New firmware is available: %s</strong>"
                     "<span>Build %s</span><form method=\"get\" action=\"/firmware/update\">"
                     "<button type=\"submit\">Update and reboot</button></form></section>",
                     version, build);
#else
            snprintf(out, out_len,
                     "<section class=\"update-panel firmware-update\"><strong>New firmware is available: %s</strong>"
                     "<span>Build %s</span><button type=\"button\" disabled>OTA bootloader not installed</button></section>",
                     version, build);
#endif
        }
    } else if (ci_equal(name, "THEME_STATUS")) {
        html_escape(remote_http_theme_status(), out, out_len);
    } else if (ci_equal(name, "THEME_INSTALL_PANEL")) {
        snprintf(out, out_len, "");
    } else if (ci_equal(name, "BOARD_REV")) {
        snprintf(out, out_len, "%lu", (unsigned long)board_rev);
    } else if (ci_equal(name, "BOARD_LABEL")) {
        snprintf(out, out_len, "%s", core_board_label(board_rev));
    } else if (ci_equal(name, "R3_URL")) {
        make_index_url(dir_path, 3, out, out_len);
    } else if (ci_equal(name, "R6_URL")) {
        make_index_url(dir_path, 6, out, out_len);
    } else if (ci_equal(name, "FETCH_NOW_URL")) {
        make_fetch_now_url(dir_path, board_rev, out, out_len);
    } else if (ci_equal(name, "CURRENT_PATH")) {
        html_escape((dir_path && dir_path[0]) ? dir_path : "/", out, out_len);
    } else if (ci_equal(name, "PARENT_LINK")) {
        char parent[192];
        if (make_parent_path(dir_path, parent, sizeof parent) &&
            make_index_url(parent, board_rev, url, sizeof url)) {
            snprintf(out, out_len, "<a href=\"%s\">Parent</a>", url);
        } else {
            snprintf(out, out_len, "");
        }
    } else if (ci_equal(name, "FILENAME")) {
        format_core_label_html(file_path, file_name, is_dir, out, out_len);
    } else if (ci_equal(name, "FILENAME_ESCAPED")) {
        html_escape(file_name ? file_name : "", out, out_len);
    } else if (ci_equal(name, "FILENAME_JS")) {
        js_string_escape(file_name ? file_name : "", out, out_len);
    } else if (ci_equal(name, "TYPE")) {
        snprintf(out, out_len, "%s", is_dir ? "DIR" : (is_partial ? "PARTIAL" : (is_theme ? "THEME" : "CORE")));
    } else if (ci_equal(name, "SIZE")) {
        if (is_dir) snprintf(out, out_len, "-");
        else snprintf(out, out_len, "%lu", (unsigned long)size);
    } else if (ci_equal(name, "PRIMARY_LABEL")) {
        snprintf(out, out_len, "%s", is_dir ? "Open" : (is_partial ? "Downloading" : (is_theme ? "Set Theme" : "Launch Core")));
    } else if (ci_equal(name, "PRIMARY_URL")) {
        if (is_dir) {
            make_index_url(file_path, board_rev, out, out_len);
        } else if (is_partial) {
            snprintf(out, out_len, "#");
        } else if (is_theme) {
            url_encode(file_name ? file_name : "", encoded, sizeof encoded);
            if (!make_index_url(dir_path, board_rev, url, sizeof url)) snprintf(url, sizeof url, "/index.html");
            url_encode(url, encoded_return, sizeof encoded_return);
            snprintf(out, out_len, "/theme/install?theme=%s&return=%s", encoded, encoded_return);
        } else {
            url_encode(file_path ? file_path : "", encoded, sizeof encoded);
            if (board_rev == 3 || board_rev == 6) {
                snprintf(out, out_len, "/jtag?file=%s&board=%lu", encoded, (unsigned long)board_rev);
            } else {
                snprintf(out, out_len, "/jtag?file=%s", encoded);
            }
        }
    } else if (ci_equal(name, "FILE_URL")) {
        if (is_dir) {
            make_index_url(file_path, board_rev, out, out_len);
        } else if (is_partial || is_theme) {
            snprintf(out, out_len, "#");
        } else {
            url_encode(file_path ? file_path : "", encoded, sizeof encoded);
            snprintf(out, out_len, "/files/%s", encoded);
        }
    } else if (ci_equal(name, "JTAG_URL")) {
        if (is_dir) {
            make_index_url(file_path, board_rev, out, out_len);
        } else if (is_partial || is_theme) {
            snprintf(out, out_len, "#");
        } else {
            url_encode(file_path ? file_path : "", encoded, sizeof encoded);
            if (board_rev == 3 || board_rev == 6) {
                snprintf(out, out_len, "/jtag?file=%s&board=%lu", encoded, (unsigned long)board_rev);
            } else {
                snprintf(out, out_len, "/jtag?file=%s", encoded);
            }
        }
    } else if (ci_equal(name, "DELETE_URL")) {
        if (is_dir || is_partial || is_theme) {
            snprintf(out, out_len, "#");
        } else {
            url_encode(file_path ? file_path : "", encoded, sizeof encoded);
            snprintf(out, out_len, "/delete?file=%s", encoded);
        }
    } else if (ci_equal(name, "ACTIONS")) {
        if (is_dir || is_partial || is_theme) {
            snprintf(out, out_len, "");
        } else {
            url_encode(file_path ? file_path : "", encoded, sizeof encoded);
            js_string_escape(file_name ? file_name : "", js_name, sizeof js_name);
            snprintf(out, out_len,
                     "<a href=\"/files/%s\">download</a> "
                     "<button class=\"delete\" type=\"button\" onclick=\"deleteCore('/delete?file=%s','%s')\">delete</button>",
                     encoded, encoded, js_name);
        }
    } else if (ci_equal(name, "CORE_META")) {
        if (is_dir) snprintf(out, out_len, "");
        else if (is_theme) snprintf(out, out_len, "Web interface theme package");
        else format_core_meta_html(file_path, file_name, out, out_len);
    } else if (ci_equal(name, "PATH")) {
        snprintf(out, out_len, "%s", file_path ? file_path : "");
    } else if (ci_equal(name, "PATH_ESCAPED")) {
        html_escape(file_path ? file_path : "", escaped, sizeof escaped);
        snprintf(out, out_len, "%s", escaped);
    } else {
        snprintf(tmp, sizeof tmp, "{%s}", name);
        snprintf(out, out_len, "%s", tmp);
    }
}

static bool template_placeholder_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static void template_flush_literal(page_builder_t *pb, template_parser_t *tp, const char *suffix, size_t suffix_len)
{
    page_append_n(pb, "{", 1);
    page_append_n(pb, tp->name, tp->name_len);
    if (suffix && suffix_len) page_append_n(pb, suffix, suffix_len);
    tp->in_placeholder = false;
    tp->name_len = 0;
}

static void template_feed_char(page_builder_t *pb,
                               template_parser_t *tp,
                               char ch,
                               const char *file_name,
                               const char *file_path,
                               const char *dir_path,
                               uint32_t size,
                               bool is_dir,
                               uint8_t board_rev)
{
    if (!tp->in_placeholder) {
        if (ch == '{') {
            tp->in_placeholder = true;
            tp->name_len = 0;
        } else {
            page_append_n(pb, &ch, 1);
        }
        return;
    }

    if (ch == '}') {
        if (tp->name_len == 0) {
            template_flush_literal(pb, tp, "}", 1);
            return;
        }
        tp->name[tp->name_len] = 0;
        char value[320];
        substitution_value(tp->name, file_name, file_path, dir_path, size, is_dir, board_rev, value, sizeof value);
        page_append(pb, value);
        tp->in_placeholder = false;
        tp->name_len = 0;
        return;
    }

    if (!template_placeholder_char(ch) || tp->name_len + 1u >= sizeof tp->name) {
        template_flush_literal(pb, tp, &ch, 1);
        return;
    }

    tp->name[tp->name_len++] = ch;
}

static void template_finish(page_builder_t *pb, template_parser_t *tp)
{
    if (tp->in_placeholder) template_flush_literal(pb, tp, NULL, 0);
}

static void append_substituted(page_builder_t *pb,
                               const char *tmpl,
                               const char *file_name,
                               const char *file_path,
                               const char *dir_path,
                               uint32_t size,
                               bool is_dir,
                               uint8_t board_rev)
{
    template_parser_t tp = {0};
    for (const char *p = tmpl; p && *p; p++) {
        template_feed_char(pb, &tp, *p, file_name, file_path, dir_path, size, is_dir, board_rev);
    }
    template_finish(pb, &tp);
}

static bool append_substituted_file(page_builder_t *pb,
                                    const char *path,
                                    const char *file_name,
                                    const char *file_path,
                                    const char *dir_path,
                                    uint32_t size,
                                    bool is_dir,
                                    uint8_t board_rev)
{
    storage_file_t f = {0};
    if (!storage_open(&f, path)) return false;

    template_parser_t tp = {0};
    uint8_t chunk[M65_HTTP_TEMPLATE_CHUNK];
    bool ok = true;
    for (;;) {
        size_t got = 0;
        if (!storage_read(&f, chunk, sizeof chunk, &got)) {
            ok = false;
            break;
        }
        if (got == 0) break;
        for (size_t i = 0; i < got; i++) {
            template_feed_char(pb, &tp, (char)chunk[i], file_name, file_path, dir_path, size, is_dir, board_rev);
        }
    }
    storage_close(&f);
    if (ok) template_finish(pb, &tp);
    return ok;
}

static void append_template_or_fallback(page_builder_t *pb,
                                        const char *path,
                                        const char *fallback,
                                        const char *file_name,
                                        const char *file_path,
                                        const char *dir_path,
                                        uint32_t size,
                                        bool is_dir,
                                        uint8_t board_rev)
{
    if (append_substituted_file(pb, path, file_name, file_path, dir_path, size, is_dir, board_rev)) return;
    append_substituted(pb, fallback, file_name, file_path, dir_path, size, is_dir, board_rev);
}

static void index_cb(const char *name, uint32_t size, bool is_dir, void *ctx)
{
    index_list_ctx_t *il = (index_list_ctx_t *)ctx;
    if (il->pb->len + 512u >= il->pb->cap) {
        il->truncated = true;
        return;
    }
    if (is_dir && ci_equal(name, "WWW")) return;
    if (il->rows >= M65_HTTP_INDEX_MAX_ROWS) {
        il->truncated = true;
        return;
    }

    char full_path[256];
    if (!make_child_path(il->dir_path, name, full_path, sizeof full_path)) return;

    bool is_partial = !is_dir && is_partial_core_path(full_path);
    bool is_theme = !is_dir && has_theme_ext(full_path);
    if (!is_dir && !is_partial && !is_theme && (il->board_rev == 3 || il->board_rev == 6)) {
        core_file_t cf;
        if (!core_open(&cf, full_path)) return;
        bool match = core_matches_board(&cf, full_path, il->board_rev);
        core_close(&cf);
        if (!match) return;
    }
    append_substituted(il->pb, il->row_template, name, full_path, il->dir_path, size, is_dir, il->board_rev);
    il->rows++;
}

static err_t http_close(http_conn_t *c)
{
    if (!c || !c->pcb) return ERR_OK;
    if (c->state != HTTP_CLOSING &&
        (c->put_op == HTTP_PUT_FILE || c->jtag_spool) && c->state == HTTP_RECV_BODY) {
        signed_file_receive_abort(&c->signed_rx);
    }
    if (c->state != HTTP_CLOSING && c->jtag_active) {
        jtag_program_writer_abort(&c->jtag);
        c->jtag_active = false;
    }
    struct tcp_pcb *pcb = c->pcb;
    c->state = HTTP_CLOSING;
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, http_sent);
    tcp_poll(pcb, http_poll_cb, 2);
    err_t err = tcp_close(pcb);
    if (err == ERR_OK) {
        c->pcb = NULL;
        c->in_use = false;
        c->state = HTTP_CLOSED;
        return ERR_OK;
    }
    if (err == ERR_MEM) {
        return ERR_OK;
    }

    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_poll(pcb, NULL, 0);
    c->pcb = NULL;
    c->in_use = false;
    c->state = HTTP_CLOSED;
    tcp_abort(pcb);
    c->aborted = true;
    return ERR_ABRT;
}

static bool tcp_write_copy(http_conn_t *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len) {
        u16_t snd = tcp_sndbuf(c->pcb);
        if (snd == 0) return false;
        u16_t chunk = len > snd ? snd : (u16_t)len;
        err_t err = tcp_write(c->pcb, p, chunk, TCP_WRITE_FLAG_COPY);
        if (err != ERR_OK) return false;
        p += chunk;
        len -= chunk;
    }
    tcp_output(c->pcb);
    return true;
}

static err_t send_response_bytes(http_conn_t *c,
                                 int code,
                                 const char *reason,
                                 const char *content_type,
                                 const void *body,
                                 size_t body_len)
{
    char header[256];
    snprintf(header, sizeof header,
             "HTTP/1.0 %d %s\r\n"
             "Connection: close\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %lu\r\n"
             "Cache-Control: no-store\r\n"
             "\r\n",
             code, reason, content_type ? content_type : "text/plain",
             (unsigned long)body_len);
    if (!tcp_write_copy(c, header, strlen(header))) return http_close(c);
    if (body_len && body && !tcp_write_copy(c, body, body_len)) return http_close(c);
    return http_close(c);
}

static err_t send_response(http_conn_t *c,
                           int code,
                           const char *reason,
                           const char *content_type,
                           const char *body)
{
    if (!body) body = "";
    return send_response_bytes(c, code, reason, content_type, body, strlen(body));
}

static err_t send_error(http_conn_t *c, int code, const char *reason, const char *msg)
{
    char body[256];
    snprintf(body, sizeof body, "%d %s\n%s\n", code, reason, msg ? msg : reason);
    return send_response(c, code, reason, "text/plain", body);
}

static err_t send_identity(http_conn_t *c)
{
    char identity[48];
    char body[56];
    machine_identity_format(identity, sizeof identity);
    snprintf(body, sizeof body, "%s\n", identity);
    return send_response(c, 200, "OK", "text/plain", body);
}

static err_t send_cached_favicon(http_conn_t *c)
{
    if (!favicon_cache_loaded || favicon_cache_len == 0) {
        return send_error(c, 404, "Not Found", "favicon is not cached");
    }
    return send_response_bytes(c, 200, "OK", "image/png", favicon_cache, favicon_cache_len);
}

static bool is_favicon_request(const char *path)
{
    return ci_equal(path, "/favicon.ico") || ci_equal(path, "/WWW/favicon-32x32.png");
}

static bool autofetch_web_busy(void)
{
    return autofetch_running || autofetch_state != AUTOFETCH_IDLE;
}

static err_t send_busy_page(http_conn_t *c)
{
    char *page_buf = (char *)malloc(M65_HTTP_BUSY_PAGE_MAX);
    if (!page_buf) {
        return send_error(c, 503, "Service Unavailable", "busy fetching cores");
    }
    page_builder_t pb = { .buf = page_buf, .cap = M65_HTTP_BUSY_PAGE_MAX };
    page_buf[0] = 0;
    append_substituted(&pb,
                       busy_template_loaded ? busy_template : default_busy,
                       NULL,
                       NULL,
                       "/",
                       0,
                       false,
                       0);
    err_t ret = send_response(c, 200, "OK", "text/html; charset=utf-8", page_buf);
    free(page_buf);
    return ret;
}

static bool request_wants_html(const http_conn_t *c)
{
    const char *accept = header_value(c->header, "Accept");
    return accept && ci_contains_n(accept, header_value_len(accept), "text/html");
}

static bool safe_redirect_path(const char *path)
{
    if (!path || path[0] != '/') return false;
    if (path[1] == '/') return false;
    size_t n = strlen(path);
    if (!n || n > 220) return false;
    if (strstr(path, "..")) return false;
    if (strcmp(path, "/") != 0) {
        const char index_path[] = "/index.html";
        size_t index_len = sizeof index_path - 1u;
        if (!starts_with(path, index_path)) return false;
        if (path[index_len] != 0 && path[index_len] != '?') return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)path[i];
        if (c < 0x20 || c == 0x7f) return false;
        if (c == '\\' || c == '"' || c == '\'' || c == '<' || c == '>') return false;
    }
    return true;
}

static bool copy_redirect_candidate(const char *value, size_t value_len, char *out, size_t out_len)
{
    if (!value || !out_len) return false;
    while (value_len && isspace((unsigned char)*value)) {
        value++;
        value_len--;
    }
    while (value_len && isspace((unsigned char)value[value_len - 1])) value_len--;
    if (!value_len || value_len >= 256) return false;

    char tmp[256];
    memcpy(tmp, value, value_len);
    tmp[value_len] = 0;

    const char *path = tmp;
    const char *scheme = strstr(tmp, "://");
    if (scheme) {
        const char *host = scheme + 3;
        path = strchr(host, '/');
        if (!path) path = "/index.html";
    } else if (tmp[0] != '/') {
        return false;
    }

    if (!safe_redirect_path(path)) return false;
    snprintf(out, out_len, "%s", path);
    return true;
}

static void jtag_return_target(const http_conn_t *c, char *out, size_t out_len)
{
    if (!out_len) return;
    snprintf(out, out_len, "/index.html");

    char value[256];
    if (parse_query_value(c->target, "return", value, sizeof value) &&
        copy_redirect_candidate(value, strlen(value), out, out_len)) {
        return;
    }

    const char *referer = header_value(c->header, "Referer");
    (void)copy_redirect_candidate(referer, header_value_len(referer), out, out_len);
}

static err_t send_redirect(http_conn_t *c, const char *location)
{
    if (!safe_redirect_path(location)) location = "/index.html";

    char escaped[256];
    char body[384];
    html_escape(location, escaped, sizeof escaped);
    int body_n = snprintf(body, sizeof body,
                          "<!doctype html><html><head><meta charset=\"utf-8\">"
                          "<meta http-equiv=\"refresh\" content=\"0;url=%s\">"
                          "<title>JTAG complete</title></head><body>"
                          "JTAG complete. Returning to <a href=\"%s\">core list</a>."
                          "</body></html>\n",
                          escaped, escaped);
    size_t body_len = body_n < 0 ? 0u : (body_n >= (int)sizeof body ? sizeof body - 1u : (size_t)body_n);

    char header[512];
    snprintf(header, sizeof header,
             "HTTP/1.0 303 See Other\r\n"
             "Connection: close\r\n"
             "Location: %s\r\n"
             "Content-Type: text/html; charset=utf-8\r\n"
             "Content-Length: %lu\r\n"
             "Cache-Control: no-store\r\n"
             "\r\n",
             location,
             (unsigned long)body_len);
    if (!tcp_write_copy(c, header, strlen(header))) return http_close(c);
    if (!tcp_write_copy(c, body, body_len)) return http_close(c);
    return http_close(c);
}

static bool check_perm(http_conn_t *c, remote_auth_perm_t perm)
{
    return remote_auth_allowed(&http_cfg, c->remote_ip, perm);
}

static bool check_write_grant(void)
{
    return !http_cfg.require_write_grant || write_gate_active();
}

static uint8_t board_from_target(const char *target)
{
    char value[8];
    if (!parse_query_value(target, "board", value, sizeof value)) return 0;
    if (strcmp(value, "3") == 0 || ci_equal(value, "r3")) return 3;
    if (strcmp(value, "6") == 0 || ci_equal(value, "r6")) return 6;
    return 0;
}

static err_t send_fetch_now(http_conn_t *c)
{
    if (!check_perm(c, REMOTE_AUTH_FILES) && !check_perm(c, REMOTE_AUTH_BITSTREAMS)) {
        return send_error(c, 403, "Forbidden", "remote IP is not authorised");
    }

    char location[256];
    jtag_return_target(c, location, sizeof location);

    bool was_busy = autofetch_web_busy();
    bool started = false;
    uint8_t board_rev = board_from_target(c->target);
    if (!was_busy) {
        started = remote_http_autofetch_start_now(http_cfg.autofetch_enabled,
                                                 http_cfg.fetch_interval_hours,
                                                 board_rev);
    }

    if (!started && !was_busy && !autofetch_web_busy()) {
        char msg[256];
        snprintf(msg, sizeof msg,
                 "fetch did not start: %s",
                 remote_http_autofetch_status((int)http_cfg.autofetch_enabled,
                                              http_cfg.fetch_interval_hours,
                                              board_rev));
        return send_error(c, 409, "Conflict", msg);
    }

    return send_redirect(c, location);
}

static err_t send_firmware_update(http_conn_t *c)
{
    if (autofetch_web_busy()) return send_error(c, 423, "Locked", "busy fetching cores");
    if (!check_perm(c, REMOTE_AUTH_FILES)) {
        return send_error(c, 403, "Forbidden", "firmware updates are not authorised");
    }
    char err[128];
    if (!remote_http_firmware_update(err, sizeof err)) {
        return send_error(c, 501, "Not Implemented", err[0] ? err : "firmware update failed");
    }
    return send_redirect(c, "/index.html");
}

static err_t send_theme_install(http_conn_t *c)
{
    if (autofetch_web_busy()) return send_error(c, 423, "Locked", "busy fetching cores");
    if (!check_perm(c, REMOTE_AUTH_FILES)) {
        return send_error(c, 403, "Forbidden", "theme installs are not authorised");
    }
    char theme[192];
    const char *theme_arg = parse_query_value(c->target, "theme", theme, sizeof theme) ? theme : NULL;
    char err[128];
    if (!remote_http_theme_install_named(theme_arg, err, sizeof err)) {
        return send_error(c, 400, "Bad Request", err[0] ? err : "theme install failed");
    }
    char location[256];
    jtag_return_target(c, location, sizeof location);
    return send_redirect(c, location);
}

static err_t send_index(http_conn_t *c, uint8_t board_rev)
{
    if (!check_perm(c, REMOTE_AUTH_FILES) && !check_perm(c, REMOTE_AUTH_BITSTREAMS)) {
        return send_error(c, 403, "Forbidden", "remote IP is not authorised");
    }
    char dir_path[192];
    if (!index_path_from_target(c->target, dir_path, sizeof dir_path)) {
        return send_error(c, 400, "Bad Request", "unsafe index path");
    }

    load_template("WWW/index_row.html", row_template, sizeof row_template, default_row);

    char *page_buf = (char *)malloc(M65_HTTP_PAGE_MAX);
    if (!page_buf) {
        return send_error(c, 503, "Service Unavailable", "not enough memory to build index page");
    }

    page_builder_t pb = { .buf = page_buf, .cap = M65_HTTP_PAGE_MAX };
    page_buf[0] = 0;
    append_template_or_fallback(&pb, "WWW/index_top.html", default_top, NULL, NULL, dir_path, 0, false, board_rev);

    index_list_ctx_t il = {
        .pb = &pb,
        .row_template = row_template,
        .dir_path = dir_path,
        .board_rev = board_rev,
    };
    if (!storage_list_cores(dir_path, index_cb, &il)) {
        page_append(&pb, "<tr><td colspan=\"3\">SD list failed</td></tr>\n");
    }
    if (il.truncated) {
        page_append(&pb, "<tr><td colspan=\"3\">List truncated</td></tr>\n");
    }
    append_template_or_fallback(&pb, "WWW/index_bottom.html", default_bottom, NULL, NULL, dir_path, 0, false, board_rev);
    err_t ret = send_response(c, 200, "OK", "text/html; charset=utf-8", page_buf);
    free(page_buf);
    return ret;
}

static err_t send_file_chunk(http_conn_t *c)
{
    if (!c || !c->pcb || c->state != HTTP_SEND_FILE) return ERR_OK;
    if (c->file_offset >= c->file_size) return http_close(c);

    u16_t snd = tcp_sndbuf(c->pcb);
    if (snd == 0) return ERR_OK;
    size_t want = c->file_size - c->file_offset;
    if (want > M65_HTTP_IO_CHUNK) want = M65_HTTP_IO_CHUNK;
    if (want > snd) want = snd;

    static uint8_t io[M65_HTTP_IO_CHUNK];
    storage_file_t f = {0};
    if (!storage_open(&f, c->path)) return send_error(c, 500, "Internal Server Error", storage_last_error());
    if (!storage_seek(&f, c->file_offset)) {
        storage_close(&f);
        return send_error(c, 500, "Internal Server Error", storage_last_error());
    }
    size_t got = 0;
    bool ok = storage_read(&f, io, want, &got);
    storage_close(&f);
    if (!ok || got == 0) return send_error(c, 500, "Internal Server Error", storage_last_error());

    err_t err = tcp_write(c->pcb, io, (u16_t)got, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) return ERR_OK;
    c->file_offset += (uint32_t)got;
    tcp_output(c->pcb);
    if (c->file_offset >= c->file_size) return http_close(c);
    return ERR_OK;
}

static err_t start_download(http_conn_t *c,
                            const char *path,
                            const char *content_type,
                            bool attachment)
{
    storage_file_t f = {0};
    if (!storage_open(&f, path)) return send_error(c, 404, "Not Found", storage_last_error());
    uint32_t size = storage_size(&f);
    storage_close(&f);

    snprintf(c->path, sizeof c->path, "%s", path);
    snprintf(c->content_type, sizeof c->content_type, "%s", content_type ? content_type : "application/octet-stream");
    c->attachment = attachment;
    c->file_size = size;
    c->file_offset = 0;
    c->state = HTTP_SEND_FILE;

    char header[320];
    snprintf(header, sizeof header,
             "HTTP/1.0 200 OK\r\n"
             "Connection: close\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %lu\r\n"
             "%s%s%s"
             "\r\n",
             c->content_type,
             (unsigned long)size,
             c->attachment ? "Content-Disposition: attachment; filename=\"" : "",
             c->attachment ? (strrchr(path, '/') ? strrchr(path, '/') + 1 : path) : "",
             c->attachment ? "\"\r\n" : "");
    if (!tcp_write_copy(c, header, strlen(header))) return http_close(c);
    return send_file_chunk(c);
}

static err_t start_file_download(http_conn_t *c, const char *path)
{
    if (!check_perm(c, REMOTE_AUTH_FILES)) return send_error(c, 403, "Forbidden", "file downloads are not authorised");
    if (!safe_file_path(path)) return send_error(c, 400, "Bad Request", "unsafe or unsupported file path");
    return start_download(c, path, "application/octet-stream", true);
}

static err_t start_www_download(http_conn_t *c, const char *path)
{
    if (!check_perm(c, REMOTE_AUTH_FILES) && !check_perm(c, REMOTE_AUTH_BITSTREAMS)) {
        return send_error(c, 403, "Forbidden", "remote IP is not authorised");
    }
    if (!safe_www_path(path)) return send_error(c, 400, "Bad Request", "unsafe WWW path");
    char full[256];
    if (snprintf(full, sizeof full, "WWW/%s", path) >= (int)sizeof full) {
        return send_error(c, 400, "Bad Request", "path too long");
    }
    return start_download(c, full, content_type_for_path(full), false);
}

static err_t start_downloads_download(http_conn_t *c, const char *path)
{
    if (!check_perm(c, REMOTE_AUTH_FILES)) return send_error(c, 403, "Forbidden", "download reads are not authorised");
    char full[256];
    if (!make_download_path(path, full, sizeof full)) {
        return send_error(c, 400, "Bad Request", "unsafe DOWNLOADS path");
    }
    return start_download(c, full, content_type_for_path(full), true);
}

static err_t program_file(http_conn_t *c, const char *path, uint8_t board_rev)
{
    if (!check_perm(c, REMOTE_AUTH_BITSTREAMS)) return send_error(c, 403, "Forbidden", "JTAG programming is not authorised");
    if (!safe_file_path(path)) return send_error(c, 400, "Bad Request", "unsafe or unsupported file path");

    core_file_t cf;
    if (!core_open(&cf, path)) return send_error(c, 404, "Not Found", core_last_error());
    if (!core_matches_board(&cf, path, board_rev)) {
        core_close(&cf);
        return send_error(c, 404, "Not Found", "core does not match requested board revision");
    }

    jtag_program_options_t opts = {
        .check_idcode = true,
        .use_hijack = true,
        .release_after = true,
    };
    bool ok = jtag_program_core(&cf, &opts);
    core_close(&cf);
    if (!ok) return send_error(c, 500, "Internal Server Error", jtag_last_error());
    if (request_wants_html(c)) {
        char target[256];
        jtag_return_target(c, target, sizeof target);
        return send_redirect(c, target);
    }
    return send_response(c, 200, "OK", "text/plain", "OK JTAG DONE\n");
}

static err_t delete_file(http_conn_t *c, const char *path)
{
    if (!check_perm(c, REMOTE_AUTH_FILES)) return send_error(c, 403, "Forbidden", "file deletes are not authorised");
    if (!check_write_grant()) return send_error(c, 423, "Locked", "write grant is not active");
    if (!safe_file_path(path)) return send_error(c, 400, "Bad Request", "unsafe or unsupported file path");
    if (!storage_delete(path)) return send_error(c, 500, "Internal Server Error", storage_last_error());
    return send_response(c, 200, "OK", "text/plain", "OK DELETE DONE\n");
}

static bool parse_query_value(const char *target, const char *key, char *out, size_t out_len)
{
    const char *q = strchr(target, '?');
    if (!q) return false;
    q++;
    size_t key_len = strlen(key);
    while (*q) {
        const char *next = strchr(q, '&');
        size_t part_len = next ? (size_t)(next - q) : strlen(q);
        if (part_len > key_len && strncmp(q, key, key_len) == 0 && q[key_len] == '=') {
            char encoded[256];
            size_t n = part_len - key_len - 1u;
            if (n >= sizeof encoded) n = sizeof encoded - 1u;
            memcpy(encoded, q + key_len + 1u, n);
            encoded[n] = 0;
            return url_decode(encoded, out, out_len);
        }
        if (!next) break;
        q = next + 1;
    }
    return false;
}

static err_t begin_put_file(http_conn_t *c, const char *path)
{
    if (!check_perm(c, REMOTE_AUTH_FILES)) return send_error(c, 403, "Forbidden", "file uploads are not authorised");
    if (!check_write_grant()) return send_error(c, 423, "Locked", "write grant is not active");
    if (!safe_file_path(path)) return send_error(c, 400, "Bad Request", "unsafe or unsupported file path");
    if (c->content_length == 0) return send_error(c, 411, "Length Required", "Content-Length must be non-zero");

    snprintf(c->path, sizeof c->path, "%s", path);
    if (snprintf(c->tmp_path, sizeof c->tmp_path, "%s.partial", path) >= (int)sizeof c->tmp_path) {
        return send_error(c, 400, "Bad Request", "path too long");
    }
    storage_delete(c->tmp_path);
    if (!signed_file_receive_begin(&c->signed_rx,
                                   &http_cfg,
                                   c->path,
                                   c->tmp_path,
                                   c->content_length,
                                   signed_file_type_from_path(path))) {
        storage_delete(c->tmp_path);
        return send_error(c, 400, "Bad Request", signed_file_last_error());
    }
    c->put_op = HTTP_PUT_FILE;
    c->state = HTTP_RECV_BODY;
    return ERR_OK;
}

static err_t begin_put_jtag(http_conn_t *c)
{
    if (!check_perm(c, REMOTE_AUTH_BITSTREAMS)) return send_error(c, 403, "Forbidden", "JTAG programming is not authorised");
    if (!check_write_grant()) return send_error(c, 423, "Locked", "write grant is not active");
    if (c->content_length == 0) return send_error(c, 411, "Length Required", "Content-Length must be non-zero");

    c->jtag_board_rev = board_from_target(c->target);

    char spool_name[192];
    bool have_spool_name = parse_query_value(c->target, "name", spool_name, sizeof spool_name);
    if (http_cfg.require_signatures || have_spool_name) {
        if (!have_spool_name) snprintf(spool_name, sizeof spool_name, "JTAG-PUT.bit");
        if (!make_download_path(spool_name, c->path, sizeof c->path)) {
            return send_error(c, 400, "Bad Request", "unsafe DOWNLOADS filename");
        }
        if (snprintf(c->tmp_path, sizeof c->tmp_path, "%s.partial", c->path) >= (int)sizeof c->tmp_path) {
            return send_error(c, 400, "Bad Request", "path too long");
        }
        if (!storage_mkdir("DOWNLOADS")) {
            return send_error(c, 500, "Internal Server Error", storage_last_error());
        }
        storage_delete(c->tmp_path);
        m65_signed_file_type_t type = have_spool_name ? signed_file_type_from_path(c->path) : M65_SIGNED_FILE_ANY;
        if (!signed_file_receive_begin(&c->signed_rx,
                                       &http_cfg,
                                       c->path,
                                       c->tmp_path,
                                       c->content_length,
                                       type)) {
            storage_delete(c->tmp_path);
            return send_error(c, 400, "Bad Request", signed_file_last_error());
        }
        c->jtag_spool = true;
        c->put_op = HTTP_PUT_JTAG;
        c->state = HTTP_RECV_BODY;
        return ERR_OK;
    }

    char idcode[32];
    if (parse_query_value(c->target, "idcode", idcode, sizeof idcode)) {
        c->expected_idcode = (uint32_t)strtoul(idcode, NULL, 16);
    }
    jtag_program_options_t opts = {
        .check_idcode = c->expected_idcode != 0,
        .use_hijack = true,
        .release_after = true,
    };
    if (!jtag_program_writer_begin(&c->jtag, c->content_length, c->expected_idcode, &opts)) {
        return send_error(c, 500, "Internal Server Error", jtag_last_error());
    }
    c->jtag_active = true;
    c->put_op = HTTP_PUT_JTAG;
    c->state = HTTP_RECV_BODY;
    return ERR_OK;
}

static err_t write_file_body(http_conn_t *c, const uint8_t *data, size_t len)
{
    if (!len) return ERR_OK;
    if (c->body_done + len > c->content_length) return send_error(c, 400, "Bad Request", "body exceeds Content-Length");
    if (!check_write_grant()) return send_error(c, 423, "Locked", "write grant expired");

    if (!signed_file_receive_write(&c->signed_rx, data, len)) {
        signed_file_receive_abort(&c->signed_rx);
        return send_error(c, 400, "Bad Request", signed_file_last_error());
    }

    c->body_done += (uint32_t)len;
    return ERR_OK;
}

static err_t write_jtag_body(http_conn_t *c, const uint8_t *data, size_t len)
{
    if (!len) return ERR_OK;
    if (c->body_done + len > c->content_length) return send_error(c, 400, "Bad Request", "body exceeds Content-Length");
    if (!check_write_grant()) return send_error(c, 423, "Locked", "write grant expired");
    if (c->jtag_spool) {
        if (!signed_file_receive_write(&c->signed_rx, data, len)) {
            signed_file_receive_abort(&c->signed_rx);
            return send_error(c, 400, "Bad Request", signed_file_last_error());
        }
        c->body_done += (uint32_t)len;
        return ERR_OK;
    }
    if (!jtag_program_writer_write(&c->jtag, data, len)) {
        jtag_program_writer_abort(&c->jtag);
        c->jtag_active = false;
        return send_error(c, 500, "Internal Server Error", jtag_last_error());
    }
    c->body_done += (uint32_t)len;
    return ERR_OK;
}

static bool finish_signed_receive_logged(signed_file_rx_t *rx,
                                         const char *prefix,
                                         const char *path,
                                         bool update_autofetch_status)
{
    if (!rx) return false;
    const char *file = path && path[0] ? path : rx->final_path;
    const char *action = (rx->require_signature || rx->candidate_signature) ?
                         "verifying hash and signature of file" :
                         "finalising received file";
    if (update_autofetch_status) {
        snprintf(autofetch_status_buf, sizeof autofetch_status_buf,
                 "autofetch=running stage=signature file=%s action=\"%s\" checked=%lu total=%lu needed=%lu updated=%lu",
                 file,
                 action,
                 (unsigned long)autofetch_checked_count,
                 (unsigned long)autofetch_manifest_total_count,
                 (unsigned long)autofetch_needed_count,
                 (unsigned long)autofetch_updated_count);
    }

    absolute_time_t start = get_absolute_time();
    remote_log(1, "%s %s start file=%s", prefix ? prefix : "+FILE:", action, file);
    bool ok = signed_file_receive_finish(rx);
    int64_t us = absolute_time_diff_us(start, get_absolute_time());
    if (us < 0) us = 0;
    remote_log(1, "%s %s %s file=%s time_ms=%lu",
               prefix ? prefix : "+FILE:",
               action,
               ok ? "done" : "failed",
               file,
               (unsigned long)(us / 1000ll));
    return ok;
}

static err_t finish_put(http_conn_t *c)
{
    if (c->put_op == HTTP_PUT_FILE) {
        if (!finish_signed_receive_logged(&c->signed_rx, "+HTTP:", c->path, false)) {
            signed_file_receive_abort(&c->signed_rx);
            return send_error(c, 400, "Bad Request", signed_file_last_error());
        }
        storage_delete(c->path);
        if (!storage_rename(c->tmp_path, c->path)) {
            storage_delete(c->tmp_path);
            return send_error(c, 500, "Internal Server Error", storage_last_error());
        }
        c->put_op = HTTP_PUT_NONE;
        return send_response(c, 201, "Created", "text/plain", "OK FILE STORED\n");
    }
    if (c->put_op == HTTP_PUT_JTAG) {
        if (c->jtag_spool) {
            if (!finish_signed_receive_logged(&c->signed_rx, "+HTTP:", c->path, false)) {
                signed_file_receive_abort(&c->signed_rx);
                return send_error(c, 400, "Bad Request", signed_file_last_error());
            }
            storage_delete(c->path);
            if (!storage_rename(c->tmp_path, c->path)) {
                storage_delete(c->tmp_path);
                return send_error(c, 500, "Internal Server Error", storage_last_error());
            }

            core_file_t cf;
            if (!core_open(&cf, c->path)) {
                c->put_op = HTTP_PUT_NONE;
                c->jtag_spool = false;
                return send_error(c, 400, "Bad Request", core_last_error());
            }
            if (!core_matches_board(&cf, c->path, c->jtag_board_rev)) {
                core_close(&cf);
                c->put_op = HTTP_PUT_NONE;
                c->jtag_spool = false;
                return send_error(c, 404, "Not Found", "core does not match requested board revision");
            }

            jtag_program_options_t opts = {
                .check_idcode = true,
                .use_hijack = true,
                .release_after = true,
            };
            bool ok = jtag_program_core(&cf, &opts);
            core_close(&cf);
            c->put_op = HTTP_PUT_NONE;
            c->jtag_spool = false;
            if (!ok) return send_error(c, 500, "Internal Server Error", jtag_last_error());
            return send_response(c, 200, "OK", "text/plain", "OK JTAG DONE\n");
        }
        if (!jtag_program_writer_finish(&c->jtag)) {
            c->jtag_active = false;
            return send_error(c, 500, "Internal Server Error", jtag_last_error());
        }
        c->jtag_active = false;
        c->put_op = HTTP_PUT_NONE;
        return send_response(c, 200, "OK", "text/plain", "OK JTAG DONE\n");
    }
    return send_error(c, 400, "Bad Request", "no active PUT operation");
}

static int find_header_end(const char *buf, size_t len, size_t *end_len)
{
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            *end_len = 4;
            return (int)i;
        }
    }
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '\n' && buf[i + 1] == '\n') {
            *end_len = 2;
            return (int)i;
        }
    }
    return -1;
}

static err_t parse_headers(http_conn_t *c)
{
    char version[16];
    if (sscanf(c->header, "%7s %255s %15s", c->method, c->target, version) != 3) {
        return send_error(c, 400, "Bad Request", "cannot parse request line");
    }

    char target_path[256];
    snprintf(target_path, sizeof target_path, "%s", c->target);
    char *query = strchr(target_path, '?');
    if (query) *query = 0;

    if (ci_equal(c->method, "GET") &&
        (ci_equal(target_path, "/identity") || ci_equal(target_path, "/id") ||
         ci_equal(target_path, "/machine"))) {
        return send_identity(c);
    }

    if (!basic_auth_ok(c)) {
        const char body[] = "401 Unauthorized\n";
        const char header[] =
            "HTTP/1.0 401 Unauthorized\r\n"
            "Connection: close\r\n"
            "WWW-Authenticate: Basic realm=\"MEGA65 JTAG\"\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 17\r\n\r\n";
        tcp_write_copy(c, header, strlen(header));
        tcp_write_copy(c, body, strlen(body));
        return http_close(c);
    }

    c->content_length = 0;
    const char *cl = header_value(c->header, "Content-Length");
    if (cl) c->content_length = (uint32_t)strtoul(cl, NULL, 10);

    if (ci_equal(c->method, "GET") && is_favicon_request(target_path)) {
        if (favicon_cache_loaded) return send_cached_favicon(c);
        if (ci_equal(target_path, "/WWW/favicon-32x32.png") && !autofetch_web_busy()) {
            return start_www_download(c, "favicon-32x32.png");
        }
        return send_error(c, 404, "Not Found", "favicon is not cached");
    }

    if (ci_equal(c->method, "GET") && ci_equal(target_path, "/fetch/now")) {
        return send_fetch_now(c);
    }

    if ((ci_equal(c->method, "GET") || ci_equal(c->method, "PUT")) &&
        ci_equal(target_path, "/fetch/stop")) {
        autofetch_cancel("web stop", 900000u);
        return send_redirect(c, "/index.html");
    }

    if ((ci_equal(c->method, "GET") || ci_equal(c->method, "PUT")) &&
        ci_equal(target_path, "/firmware/update")) {
        return send_firmware_update(c);
    }

    if ((ci_equal(c->method, "GET") || ci_equal(c->method, "PUT")) &&
        ci_equal(target_path, "/theme/install")) {
        return send_theme_install(c);
    }

    if (autofetch_web_busy() && ci_equal(c->method, "GET") &&
        (request_wants_html(c) || ci_equal(target_path, "/") || ci_equal(target_path, "/index.html"))) {
        return send_busy_page(c);
    }
    if (autofetch_web_busy()) {
        return send_error(c, 423, "Locked", "busy fetching cores");
    }

    if (ci_equal(c->method, "GET")) {
        if (ci_equal(target_path, "/") || ci_equal(target_path, "/index.html")) {
            return send_index(c, board_from_target(c->target));
        }
        if (starts_with(target_path, "/files/")) {
            char decoded[256];
            if (!url_decode(target_path + 7, decoded, sizeof decoded)) {
                return send_error(c, 400, "Bad Request", "bad URL encoding");
            }
            return start_file_download(c, decoded);
        }
        if (starts_with(target_path, "/WWW/")) {
            char decoded[256];
            if (!url_decode(target_path + 5, decoded, sizeof decoded)) {
                return send_error(c, 400, "Bad Request", "bad URL encoding");
            }
            return start_www_download(c, decoded);
        }
        if (starts_with(target_path, "/downloads/")) {
            char decoded[256];
            if (!url_decode(target_path + 11, decoded, sizeof decoded)) {
                return send_error(c, 400, "Bad Request", "bad URL encoding");
            }
            return start_downloads_download(c, decoded);
        }
        if (ci_equal(target_path, "/jtag")) {
            char file[256];
            if (!parse_query_value(c->target, "file", file, sizeof file)) {
                return send_error(c, 400, "Bad Request", "missing file query parameter");
            }
            return program_file(c, file, board_from_target(c->target));
        }
        return send_error(c, 404, "Not Found", "unknown GET endpoint");
    }

    if (ci_equal(c->method, "PUT")) {
        if (starts_with(target_path, "/files/")) {
            char decoded[256];
            if (!url_decode(target_path + 7, decoded, sizeof decoded)) {
                return send_error(c, 400, "Bad Request", "bad URL encoding");
            }
            return begin_put_file(c, decoded);
        }
        if (ci_equal(target_path, "/jtag")) {
            return begin_put_jtag(c);
        }
        if (ci_equal(target_path, "/delete")) {
            char file[256];
            if (!parse_query_value(c->target, "file", file, sizeof file)) {
                return send_error(c, 400, "Bad Request", "missing file query parameter");
            }
            return delete_file(c, file);
        }
        return send_error(c, 404, "Not Found", "unknown PUT endpoint");
    }

    return send_error(c, 405, "Method Not Allowed", "only GET and PUT are supported");
}

static err_t process_body(http_conn_t *c, const uint8_t *data, size_t len)
{
    while (len && c->state == HTTP_RECV_BODY) {
        size_t want = c->content_length - c->body_done;
        if (want > len) want = len;
        err_t err = ERR_OK;
        if (c->put_op == HTTP_PUT_FILE) err = write_file_body(c, data, want);
        else if (c->put_op == HTTP_PUT_JTAG) err = write_jtag_body(c, data, want);
        else err = send_error(c, 400, "Bad Request", "unexpected body");
        if (err != ERR_OK || c->state == HTTP_CLOSED) return err;
        data += want;
        len -= want;
        if (c->body_done == c->content_length) return finish_put(c);
    }
    return ERR_OK;
}

static err_t process_bytes(http_conn_t *c, const uint8_t *data, size_t len)
{
    if (c->state == HTTP_RECV_BODY) return process_body(c, data, len);
    if (c->state != HTTP_RECV_HEADERS) return ERR_OK;

    size_t copy = len;
    if (copy > sizeof(c->header) - c->header_len - 1u) {
        return send_error(c, 431, "Request Header Fields Too Large", "HTTP headers too large");
    }
    memcpy(c->header + c->header_len, data, copy);
    c->header_len += copy;
    c->header[c->header_len] = 0;

    size_t end_len = 0;
    int hdr_end = find_header_end(c->header, c->header_len, &end_len);
    if (hdr_end < 0) return ERR_OK;

    size_t body_offset = (size_t)hdr_end + end_len;
    size_t body_len = c->header_len - body_offset;
    uint8_t initial_body[M65_HTTP_HEADER_MAX];
    if (body_len) memcpy(initial_body, c->header + body_offset, body_len);
    c->header[hdr_end] = 0;

    err_t err = parse_headers(c);
    if (err != ERR_OK || c->state == HTTP_CLOSED) return err;
    if (body_len && c->state == HTTP_RECV_BODY) return process_body(c, initial_body, body_len);
    return ERR_OK;
}

static err_t http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)pcb;
    http_conn_t *c = (http_conn_t *)arg;
    if (!c) return ERR_OK;
    if (!p) return http_close(c);
    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }

    tcp_recved(pcb, p->tot_len);
    for (struct pbuf *q = p; q; q = q->next) {
        err_t perr = process_bytes(c, (const uint8_t *)q->payload, q->len);
        if (perr == ERR_ABRT || c->aborted) {
            pbuf_free(p);
            return ERR_ABRT;
        }
        if (c->state == HTTP_CLOSED) break;
    }
    pbuf_free(p);
    return c->aborted ? ERR_ABRT : ERR_OK;
}

static err_t http_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)pcb;
    (void)len;
    http_conn_t *c = (http_conn_t *)arg;
    if (c && c->state == HTTP_SEND_FILE) return send_file_chunk(c);
    if (c && c->state == HTTP_CLOSING) return http_close(c);
    return ERR_OK;
}

static err_t http_poll_cb(void *arg, struct tcp_pcb *pcb)
{
    (void)pcb;
    http_conn_t *c = (http_conn_t *)arg;
    if (c && c->state == HTTP_SEND_FILE) return send_file_chunk(c);
    if (c && c->state == HTTP_CLOSING) return http_close(c);
    return ERR_OK;
}

static void http_err(void *arg, err_t err)
{
    (void)err;
    http_conn_t *c = (http_conn_t *)arg;
    if (!c) return;
    if (c->jtag_active) jtag_program_writer_abort(&c->jtag);
    if (c->put_op == HTTP_PUT_FILE || c->jtag_spool) signed_file_receive_abort(&c->signed_rx);
    memset(c, 0, sizeof *c);
}

static void fetch_set_error(fetch_ctx_t *fc, const char *msg)
{
    if (!fc) return;
    bool already_failed = fc->state == FETCH_FAILED;
    snprintf(fc->err, sizeof fc->err, "%s", msg ? msg : "fetch failed");
    fc->state = FETCH_FAILED;
    if (!already_failed) {
        remote_log(1, "+FETCH: fail dest=%s reason=%s",
                   fc->final_path[0] ? fc->final_path : "(unset)",
                   fc->err);
    }
}

static void fetch_report_progress(fetch_ctx_t *fc)
{
    if (!fc || fc->content_length == 0) return;

    const uint32_t done = fc->body_done;
    const uint32_t total = fc->content_length;
    const absolute_time_t now = get_absolute_time();
    int64_t inst_us = absolute_time_diff_us(fc->last_progress_at, now);
    int64_t avg_us = absolute_time_diff_us(fc->body_started_at, now);
    if (inst_us <= 0) inst_us = 1;
    if (avg_us <= 0) avg_us = 1;
    const uint32_t delta = done - fc->last_progress_bytes;
    const unsigned long percent = (unsigned long)(((uint64_t)done * 100u) / total);
    const unsigned long rate_KiBps = (unsigned long)(((uint64_t)delta * 1000000u) / ((uint64_t)inst_us * 1024u));
    const unsigned long avg_KiBps = (unsigned long)(((uint64_t)done * 1000000u) / ((uint64_t)avg_us * 1024u));

    if (fc == &autofetch_fc &&
        (autofetch_state == AUTOFETCH_MANIFEST || autofetch_state == AUTOFETCH_CORE)) {
        const char *stage = autofetch_state == AUTOFETCH_MANIFEST ? "manifest" : manifest_kind_name(autofetch_pending_kind);
        const char *file = autofetch_state == AUTOFETCH_MANIFEST ? autofetch_manifest_path :
                           (autofetch_pending_url_path[0] ? autofetch_pending_url_path : autofetch_pending_path);
        snprintf(autofetch_status_buf, sizeof autofetch_status_buf,
                 "autofetch=running stage=%s file=%s dest=%s bytes=%lu/%lu percent=%lu rate_KiBps=%lu avg_KiBps=%lu checked=%lu total=%lu needed=%lu updated=%lu",
                 stage,
                 file[0] ? file : fc->final_path,
                 fc->final_path,
                 (unsigned long)done,
                 (unsigned long)total,
                 percent,
                 rate_KiBps,
                 avg_KiBps,
                 (unsigned long)autofetch_checked_count,
                 (unsigned long)autofetch_manifest_total_count,
                 (unsigned long)autofetch_needed_count,
                 (unsigned long)autofetch_updated_count);
    }

    if (remote_verbose == 0) return;
    uint32_t threshold = remote_verbose >= 2u ? 65536u : 262144u;
    uint8_t level = remote_verbose >= 2u ? 2u : 1u;
    if (done != total && done - fc->last_progress_report < threshold) return;

    fc->last_progress_report = done;
    fc->last_progress_at = now;
    fc->last_progress_bytes = done;
    remote_log(level, "+FETCH: progress dest=%s bytes=%lu/%lu percent=%lu rate_KiBps=%lu avg_KiBps=%lu",
               fc->final_path,
               (unsigned long)done,
               (unsigned long)total,
               percent,
               rate_KiBps,
               avg_KiBps);
}

static void fetch_idle_diagnostics(fetch_ctx_t *fc, absolute_time_t now)
{
    if (!fc || !fc->pcb || fc->state != FETCH_RECV_BODY) return;
    int64_t idle_us = absolute_time_diff_us(fc->last_rx_at, now);
    if (idle_us < 0) idle_us = 0;

    if (idle_us >= (int64_t)M65_FETCH_ACK_NUDGE_MS * 1000ll &&
        absolute_time_diff_us(fc->last_ack_nudge_at, now) >= (int64_t)M65_FETCH_ACK_NUDGE_MS * 1000ll) {
        err_t ack_err = tcp_send_empty_ack(fc->pcb);
        fc->last_ack_nudge_at = now;
        if (remote_verbose >= 2u) {
            remote_log(2, "+FETCHACK: dest=%s idle_ms=%lu rc=%d ack=%lu wnd=%lu flags=%04x",
                       fc->final_path,
                       (unsigned long)(idle_us / 1000ll),
                       (int)ack_err,
                       (unsigned long)fc->pcb->rcv_nxt,
                       (unsigned long)fc->pcb->rcv_wnd,
                       (unsigned)fc->pcb->flags);
        }
    }

    if (remote_verbose < 2u) return;
    if (idle_us < (int64_t)M65_FETCH_IDLE_DIAG_MS * 1000ll ||
        absolute_time_diff_us(fc->last_idle_diag_at, now) < (int64_t)M65_FETCH_IDLE_DIAG_MS * 1000ll) {
        return;
    }
    fc->last_idle_diag_at = now;
#if TCP_QUEUE_OOSEQ
    unsigned long ooseq = fc->pcb->ooseq ? 1ul : 0ul;
#else
    unsigned long ooseq = 0;
#endif
    remote_log(2,
               "+FETCHIDLE: dest=%s idle_ms=%lu bytes=%lu/%lu rcv_nxt=%lu rcv_wnd=%lu rcv_ann_wnd=%lu rcv_ann_edge=%lu state=%u flags=%04x rtime=%d rto=%d nrtx=%u dupacks=%u lastack=%lu snd_nxt=%lu snd_wnd=%lu unsent=%lu unacked=%lu ooseq=%lu",
               fc->final_path,
               (unsigned long)(idle_us / 1000ll),
               (unsigned long)fc->body_done,
               (unsigned long)fc->content_length,
               (unsigned long)fc->pcb->rcv_nxt,
               (unsigned long)fc->pcb->rcv_wnd,
               (unsigned long)fc->pcb->rcv_ann_wnd,
               (unsigned long)fc->pcb->rcv_ann_right_edge,
               (unsigned)fc->pcb->state,
               (unsigned)fc->pcb->flags,
               (int)fc->pcb->rtime,
               (int)fc->pcb->rto,
               (unsigned)fc->pcb->nrtx,
               (unsigned)fc->pcb->dupacks,
               (unsigned long)fc->pcb->lastack,
               (unsigned long)fc->pcb->snd_nxt,
               (unsigned long)fc->pcb->snd_wnd,
               (unsigned long)(fc->pcb->unsent ? 1u : 0u),
               (unsigned long)(fc->pcb->unacked ? 1u : 0u),
               ooseq);
}

static void fetch_close(fetch_ctx_t *fc, bool aborting)
{
    if (!fc) return;
    if (fc->transfer_sha_active) {
        mbedtls_sha256_free(&fc->transfer_sha);
        fc->transfer_sha_active = false;
    }
    if (!fc->pcb) return;
    struct tcp_pcb *pcb = fc->pcb;
    fc->pcb = NULL;
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_poll(pcb, NULL, 0);
    if (aborting) {
        tcp_abort(pcb);
    } else if (tcp_close(pcb) != ERR_OK) {
        tcp_abort(pcb);
    }
}

static bool fetch_transfer_sha_start(fetch_ctx_t *fc)
{
    if (!fc) return false;
    mbedtls_sha256_init(&fc->transfer_sha);
    if (mbedtls_sha256_starts(&fc->transfer_sha, 0) != 0) {
        mbedtls_sha256_free(&fc->transfer_sha);
        fc->transfer_sha_active = false;
        return false;
    }
    fc->transfer_sha_active = true;
    fc->transfer_sha_hex[0] = 0;
    return true;
}

static bool fetch_transfer_sha_update(fetch_ctx_t *fc, const uint8_t *data, size_t len)
{
    if (!fc || !fc->transfer_sha_active) return true;
    if (len && mbedtls_sha256_update(&fc->transfer_sha, data, len) != 0) {
        mbedtls_sha256_free(&fc->transfer_sha);
        fc->transfer_sha_active = false;
        fetch_set_error(fc, "transfer SHA-256 update failed");
        return false;
    }
    return true;
}

static bool fetch_transfer_sha_finish(fetch_ctx_t *fc)
{
    if (!fc || !fc->transfer_sha_active) return true;
    uint8_t digest[32];
    if (mbedtls_sha256_finish(&fc->transfer_sha, digest) != 0) {
        mbedtls_sha256_free(&fc->transfer_sha);
        fc->transfer_sha_active = false;
        fetch_set_error(fc, "transfer SHA-256 finish failed");
        return false;
    }
    mbedtls_sha256_free(&fc->transfer_sha);
    fc->transfer_sha_active = false;
    bytes32_to_hex(digest, fc->transfer_sha_hex);
    return true;
}

static uint16_t fetch_random_local_port(void)
{
    static uint32_t state;
    if (state == 0) {
        state = time_us_32() ^ 0xa65e7101u ^ ((uint32_t)wifi_probe_attempts << 16);
        if (state == 0) state = 0x13579bdfu;
    }
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    state += 0x9e3779b9u + time_us_32();
    return (uint16_t)(49152u + (state % (65535u - 49152u)));
}

static bool fetch_bind_random_local_port(fetch_ctx_t *fc)
{
    if (!fc || !fc->pcb) return false;
    for (unsigned i = 0; i < 8u; ++i) {
        uint16_t port = fetch_random_local_port();
        err_t err = tcp_bind(fc->pcb, IP_ANY_TYPE, port);
        if (err == ERR_OK) {
            fc->local_port = port;
            return true;
        }
        if (err != ERR_USE) break;
    }
    return false;
}

static bool parse_http_url(const char *url, char *host, size_t host_len, uint16_t *port, char *path, size_t path_len)
{
    if (!url || strncmp(url, "http://", 7) != 0) return false;
    const char *p = url + 7;
    const char *slash = strchr(p, '/');
    const char *host_end = slash ? slash : p + strlen(p);
    if (host_end == p) return false;

    const char *colon = NULL;
    for (const char *q = p; q < host_end; ++q) {
        if (*q == ':') colon = q;
    }
    size_t hn = (size_t)((colon ? colon : host_end) - p);
    if (hn == 0 || hn >= host_len) return false;
    memcpy(host, p, hn);
    host[hn] = 0;

    *port = 80;
    if (colon) {
        char *end = NULL;
        unsigned long v = strtoul(colon + 1, &end, 10);
        if (end != host_end || v == 0 || v > 65535u) return false;
        *port = (uint16_t)v;
    }

    if (!slash) {
        snprintf(path, path_len, "/");
    } else {
        if (strlen(slash) >= path_len) return false;
        snprintf(path, path_len, "%s", slash);
    }
    return true;
}

static err_t fetch_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    fetch_ctx_t *fc = (fetch_ctx_t *)arg;
    if (!fc) return ERR_OK;
    if (err != ERR_OK) {
        fetch_set_error(fc, "TCP connect failed");
        fetch_close(fc, false);
        return ERR_OK;
    }

    remote_log(1, "+FETCH: connected host=%s port=%u local_port=%u path=%s",
               fc->host, (unsigned)fc->port, (unsigned)fc->local_port, fc->path);

    char req[512];
    int n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.0\r\n"
                     "Host: %s\r\n"
                     "User-Agent: MEGA65-JTAG/1.0\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     fc->path, fc->host);
    if (n <= 0 || n >= (int)sizeof req ||
        tcp_write(pcb, req, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) {
        fetch_set_error(fc, "cannot send HTTP request");
        fetch_close(fc, true);
        return ERR_ABRT;
    }
    tcp_output(pcb);
    fc->state = FETCH_RECV_HEADERS;
    return ERR_OK;
}

static bool fetch_process_body(fetch_ctx_t *fc, const uint8_t *data, size_t len)
{
    if (fc->body_done + len > fc->content_length) {
        fetch_set_error(fc, "HTTP body exceeds Content-Length");
        return false;
    }
    const remote_auth_config_t *cfg = fc->cfg ? fc->cfg : &http_cfg;
    if (fc->check_write_grant && cfg->require_write_grant && !write_gate_active()) {
        fetch_set_error(fc, "write grant expired");
        return false;
    }
    if (!fetch_transfer_sha_update(fc, data, len)) return false;
    if (!signed_file_receive_write(&fc->signed_rx, data, len)) {
        fetch_set_error(fc, signed_file_last_error());
        return false;
    }
    fc->body_done += (uint32_t)len;
    fc->last_rx_at = get_absolute_time();
    fetch_report_progress(fc);
    if (fc->body_done == fc->content_length) {
        if (!fetch_transfer_sha_finish(fc)) return false;
        if (!finish_signed_receive_logged(&fc->signed_rx, "+FETCH:", fc->final_path, fc == &autofetch_fc)) {
            fetch_set_error(fc, signed_file_last_error());
            return false;
        }
        storage_delete(fc->final_path);
        if (!storage_rename(fc->tmp_path, fc->final_path)) {
            snprintf(fc->err, sizeof fc->err, "rename failed: %s", storage_last_error());
            fc->state = FETCH_FAILED;
            return false;
        }
        fc->state = FETCH_DONE;
        remote_log(1, "+FETCH: done dest=%s bytes=%lu",
                   fc->final_path,
                   (unsigned long)fc->content_length);
    }
    return true;
}

static bool fetch_parse_headers(fetch_ctx_t *fc, const uint8_t *body, size_t body_len)
{
    int status = 0;
    if (sscanf(fc->header, "HTTP/%*s %d", &status) != 1 || status < 200 || status >= 300) {
        fetch_set_error(fc, "HTTP status was not 2xx");
        return false;
    }
    const char *cl = header_value(fc->header, "Content-Length");
    if (!cl) {
        fetch_set_error(fc, "HTTP response missing Content-Length");
        return false;
    }
    char *cl_end = NULL;
    unsigned long cl_value = strtoul(cl, &cl_end, 10);
    if (cl_end == cl || cl_value == 0) {
        fetch_set_error(fc, "HTTP response has zero Content-Length");
        return false;
    }
    if (cl_value > M65_FETCH_MAX_BYTES) {
        fetch_set_error(fc, "HTTP response exceeds maximum download size");
        return false;
    }
    fc->content_length = (uint32_t)cl_value;
    remote_log(1, "+FETCH: headers status=%d bytes=%lu dest=%s",
               status,
               (unsigned long)fc->content_length,
               fc->final_path);
    fc->body_started_at = get_absolute_time();
    fc->last_rx_at = fc->body_started_at;
    fc->last_idle_diag_at = fc->body_started_at;
    fc->last_ack_nudge_at = fc->body_started_at;
    fc->last_progress_at = fc->body_started_at;
    fc->last_progress_report = 0;
    fc->last_progress_bytes = 0;

    const remote_auth_config_t *cfg = fc->cfg ? fc->cfg : &http_cfg;
    storage_delete(fc->tmp_path);
    if (!ensure_parent_dirs(fc->tmp_path)) {
        fetch_set_error(fc, "cannot create destination directory");
        return false;
    }
    if (!fetch_transfer_sha_start(fc)) {
        fetch_set_error(fc, "cannot initialise transfer SHA-256");
        return false;
    }
    if (!signed_file_receive_begin(&fc->signed_rx,
                                   cfg,
                                   fc->final_path,
                                   fc->tmp_path,
                                   fc->content_length,
                                   signed_file_type_from_path(fc->final_path))) {
        fetch_set_error(fc, signed_file_last_error());
        return false;
    }

    fc->state = FETCH_RECV_BODY;
    if (body_len) return fetch_process_body(fc, body, body_len);
    return true;
}

static bool fetch_process_bytes(fetch_ctx_t *fc, const uint8_t *data, size_t len)
{
    if (fc->state == FETCH_RECV_BODY) return fetch_process_body(fc, data, len);
    if (fc->state != FETCH_RECV_HEADERS) return true;

    if (len > sizeof(fc->header) - fc->header_len - 1u) {
        fetch_set_error(fc, "HTTP response headers too large");
        return false;
    }
    memcpy(fc->header + fc->header_len, data, len);
    fc->header_len += len;
    fc->header[fc->header_len] = 0;

    size_t end_len = 0;
    int hdr_end = find_header_end(fc->header, fc->header_len, &end_len);
    if (hdr_end < 0) return true;

    size_t body_offset = (size_t)hdr_end + end_len;
    size_t body_len = fc->header_len - body_offset;
    uint8_t initial_body[M65_HTTP_HEADER_MAX];
    if (body_len) memcpy(initial_body, fc->header + body_offset, body_len);
    fc->header[hdr_end] = 0;
    return fetch_parse_headers(fc, initial_body, body_len);
}

static err_t fetch_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)pcb;
    fetch_ctx_t *fc = (fetch_ctx_t *)arg;
    if (!fc) return ERR_OK;
    if (!p) {
        if (fc->state != FETCH_DONE) fetch_set_error(fc, "connection closed before download completed");
        fetch_close(fc, false);
        return ERR_OK;
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        fetch_set_error(fc, "TCP receive failed");
        return err;
    }

    for (struct pbuf *q = p; q; q = q->next) {
        if (!fetch_process_bytes(fc, (const uint8_t *)q->payload, q->len)) {
            pbuf_free(p);
            signed_file_receive_abort(&fc->signed_rx);
            fetch_close(fc, true);
            return ERR_ABRT;
        }
        if (fc->state == FETCH_DONE) break;
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    if (fc->state == FETCH_DONE) fetch_close(fc, false);
    return ERR_OK;
}

static void fetch_err(void *arg, err_t err)
{
    (void)err;
    fetch_ctx_t *fc = (fetch_ctx_t *)arg;
    if (!fc) return;
    fc->pcb = NULL;
    if (fc->state != FETCH_DONE) {
        signed_file_receive_abort(&fc->signed_rx);
        if (fc->transfer_sha_active) {
            mbedtls_sha256_free(&fc->transfer_sha);
            fc->transfer_sha_active = false;
        }
        fetch_set_error(fc, "TCP connection aborted");
    }
}

static void fetch_dns_cb(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    (void)name;
    fetch_ctx_t *fc = (fetch_ctx_t *)arg;
    if (!fc) return;
    if (!ipaddr) {
        fetch_set_error(fc, "DNS lookup failed");
        return;
    }
    fc->pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!fc->pcb) {
        fetch_set_error(fc, "cannot allocate TCP PCB");
        return;
    }
    char ip_text[24] = "?";
    if (IP_IS_V4(ipaddr)) {
        uint32_t ip = lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(ipaddr)));
        remote_auth_format_ipv4(ip, ip_text, sizeof ip_text);
    }
    remote_log(1, "+FETCH: resolved host=%s ip=%s port=%u",
               fc->host,
               ip_text,
               (unsigned)fc->port);
    if (!fetch_bind_random_local_port(fc)) {
        fetch_set_error(fc, "cannot bind TCP source port");
        fetch_close(fc, true);
        return;
    }
    tcp_arg(fc->pcb, fc);
    tcp_recv(fc->pcb, fetch_recv);
    tcp_err(fc->pcb, fetch_err);
    err_t err = tcp_connect(fc->pcb, ipaddr, fc->port, fetch_connected);
    if (err != ERR_OK) {
        fetch_set_error(fc, "TCP connect start failed");
        fetch_close(fc, true);
    } else {
        fc->state = FETCH_CONNECTING;
        remote_log(1, "+FETCH: connecting ip=%s port=%u local_port=%u",
                   ip_text,
                   (unsigned)fc->port,
                   (unsigned)fc->local_port);
    }
}

static bool fetch_start(fetch_ctx_t *fc,
                        const char *url,
                        const char *final_path,
                        const remote_auth_config_t *cfg,
                        bool check_write_grant,
                        uint32_t timeout_ms,
                        char *err,
                        size_t err_len)
{
    if (err && err_len) err[0] = 0;
    if (!http_is_active) {
        if (err && err_len) snprintf(err, err_len, "wifi HTTP is not active");
        return false;
    }
    if (check_write_grant && cfg && cfg->require_write_grant && !write_gate_active()) {
        if (err && err_len) snprintf(err, err_len, "write grant is not active");
        return false;
    }
    if (!url || !final_path || !final_path[0]) {
        if (err && err_len) snprintf(err, err_len, "invalid fetch parameters");
        return false;
    }

    memset(fc, 0, sizeof *fc);
    fc->state = FETCH_CONNECTING;
    fc->cfg = cfg ? cfg : &http_cfg;
    fc->check_write_grant = check_write_grant;
    fc->deadline = make_timeout_time_ms(timeout_ms ? timeout_ms : 60000u);
    fc->connect_deadline = make_timeout_time_ms(M65_FETCH_CONNECT_TIMEOUT_MS);
    snprintf(fc->final_path, sizeof fc->final_path, "%s", final_path);
    if (snprintf(fc->tmp_path, sizeof fc->tmp_path, "%s.partial", fc->final_path) >= (int)sizeof fc->tmp_path) {
        if (err && err_len) snprintf(err, err_len, "destination filename too long");
        return false;
    }
    if (!parse_http_url(url, fc->host, sizeof fc->host, &fc->port, fc->path, sizeof fc->path)) {
        if (err && err_len) snprintf(err, err_len, "expected http://host[:port]/path URL");
        return false;
    }
    remote_log(1, "+FETCH: start url=%s dest=%s", url, final_path);

    ip_addr_t addr;
    err_t dns_err = dns_gethostbyname(fc->host, &addr, fetch_dns_cb, fc);
    if (dns_err == ERR_OK) {
        fetch_dns_cb(fc->host, &addr, fc);
    } else if (dns_err != ERR_INPROGRESS) {
        if (err && err_len) snprintf(err, err_len, "DNS lookup failed");
        return false;
    }
    if (fc->state == FETCH_FAILED) {
        if (err && err_len) snprintf(err, err_len, "%s", fc->err[0] ? fc->err : "fetch start failed");
        return false;
    }
    return true;
}

static bool fetch_poll(fetch_ctx_t *fc)
{
    if (!fc) return false;
    if (fc->state == FETCH_DONE || fc->state == FETCH_FAILED) return true;
    absolute_time_t now = get_absolute_time();
    if (fc->state == FETCH_CONNECTING &&
        absolute_time_diff_us(now, fc->connect_deadline) <= 0) {
        fetch_set_error(fc, "TCP connect timed out");
        fetch_close(fc, true);
        storage_delete(fc->tmp_path);
        return true;
    }
    fetch_idle_diagnostics(fc, now);
    if (fc->state == FETCH_RECV_BODY && fc->body_done < fc->content_length &&
        absolute_time_diff_us(fc->last_rx_at, now) >= (int64_t)M65_FETCH_IDLE_TIMEOUT_MS * 1000ll) {
        fetch_set_error(fc, "TCP receive idle timed out");
        signed_file_receive_abort(&fc->signed_rx);
        fetch_close(fc, true);
        storage_delete(fc->tmp_path);
        return true;
    }
    if (absolute_time_diff_us(now, fc->deadline) <= 0) {
        fetch_set_error(fc, "fetch timed out");
        signed_file_receive_abort(&fc->signed_rx);
        fetch_close(fc, true);
        storage_delete(fc->tmp_path);
        return true;
    }
    return false;
}

static uint32_t download_queued_count(void)
{
    uint32_t count = 0;
    for (unsigned i = 0; i < M65_DOWNLOAD_QUEUE_DEPTH; ++i) {
        if (download_jobs[i].state == DOWNLOAD_JOB_QUEUED) count++;
    }
    return count;
}

static bool download_slot_in_use(uint8_t slot)
{
    for (unsigned i = 0; i < M65_DOWNLOAD_QUEUE_DEPTH; ++i) {
        if ((download_jobs[i].state == DOWNLOAD_JOB_QUEUED ||
             download_jobs[i].state == DOWNLOAD_JOB_RUNNING) &&
            download_jobs[i].slot == slot) {
            return true;
        }
    }
    return false;
}

static int download_allocate_slot(int requested)
{
    if (requested >= 0) {
        if (requested > 255 || download_slot_in_use((uint8_t)requested)) return -1;
        return requested;
    }
    for (unsigned i = 0; i < 256u; ++i) {
        uint8_t slot = download_next_slot++;
        if (!download_slot_in_use(slot)) return (int)slot;
    }
    return -1;
}

static int download_find_free_job(void)
{
    for (unsigned i = 0; i < M65_DOWNLOAD_QUEUE_DEPTH; ++i) {
        if (download_jobs[i].state == DOWNLOAD_JOB_EMPTY ||
            download_jobs[i].state == DOWNLOAD_JOB_DONE ||
            download_jobs[i].state == DOWNLOAD_JOB_FAILED) {
            return (int)i;
        }
    }
    return -1;
}

static void download_refresh_status(void)
{
    if (download_active_job >= 0 && download_active_job < (int)M65_DOWNLOAD_QUEUE_DEPTH) {
        download_job_t *job = &download_jobs[download_active_job];
        job->bytes_done = download_fc.body_done;
        job->bytes_total = download_fc.content_length;
        snprintf(download_status_buf, sizeof download_status_buf,
                 "downloads=running slot=%02X path=%s bytes=%lu/%lu queued=%lu",
                 (unsigned)job->slot,
                 job->path,
                 (unsigned long)job->bytes_done,
                 (unsigned long)job->bytes_total,
                 (unsigned long)download_queued_count());
        return;
    }

    uint32_t queued = download_queued_count();
    if (queued) {
        snprintf(download_status_buf, sizeof download_status_buf,
                 "downloads=queued queued=%lu",
                 (unsigned long)queued);
        return;
    }

    if (!download_status_buf[0]) snprintf(download_status_buf, sizeof download_status_buf, "downloads=idle");
}

static bool download_start_job(unsigned idx)
{
    if (idx >= M65_DOWNLOAD_QUEUE_DEPTH) return false;
    download_job_t *job = &download_jobs[idx];
    if (job->state != DOWNLOAD_JOB_QUEUED) return false;

    char fetch_err[96];
    if (!fetch_start(&download_fc, job->url, job->path, &http_cfg, true, 14400000u, fetch_err, sizeof fetch_err)) {
        job->state = DOWNLOAD_JOB_FAILED;
        snprintf(job->err, sizeof job->err, "%s", fetch_err[0] ? fetch_err : "fetch start failed");
        snprintf(download_status_buf, sizeof download_status_buf,
                 "downloads=failed slot=%02X path=%s reason=\"%s\" queued=%lu",
                 (unsigned)job->slot,
                 job->path,
                 job->err,
                 (unsigned long)download_queued_count());
        remote_log(1, "+DOWNLOAD: failed slot=%02X path=%s reason=%s",
                   (unsigned)job->slot, job->path, job->err);
        return false;
    }

    job->state = DOWNLOAD_JOB_RUNNING;
    job->bytes_done = 0;
    job->bytes_total = 0;
    job->err[0] = 0;
    download_active_job = (int)idx;
    snprintf(download_status_buf, sizeof download_status_buf,
             "downloads=running slot=%02X path=%s bytes=0/0 queued=%lu",
             (unsigned)job->slot,
             job->path,
             (unsigned long)download_queued_count());
    remote_log(1, "+DOWNLOAD: start slot=%02X url=%s path=%s",
               (unsigned)job->slot, job->url, job->path);
    return true;
}

static void download_start_next_job(void)
{
    if (download_active_job >= 0) return;
    for (unsigned i = 0; i < M65_DOWNLOAD_QUEUE_DEPTH; ++i) {
        if (download_jobs[i].state == DOWNLOAD_JOB_QUEUED) {
            (void)download_start_job(i);
            return;
        }
    }
    download_refresh_status();
}

static void remote_http_download_poll(void)
{
    if (download_active_job >= 0 && download_active_job < (int)M65_DOWNLOAD_QUEUE_DEPTH) {
        download_job_t *job = &download_jobs[download_active_job];
        fetch_poll(&download_fc);
        job->bytes_done = download_fc.body_done;
        job->bytes_total = download_fc.content_length;
        if (download_fc.state == FETCH_DONE) {
            job->state = DOWNLOAD_JOB_DONE;
            snprintf(download_status_buf, sizeof download_status_buf,
                     "downloads=done slot=%02X path=%s bytes=%lu queued=%lu",
                     (unsigned)job->slot,
                     job->path,
                     (unsigned long)job->bytes_done,
                     (unsigned long)download_queued_count());
            remote_log(1, "+DOWNLOAD: done slot=%02X path=%s bytes=%lu",
                       (unsigned)job->slot, job->path, (unsigned long)job->bytes_done);
            download_active_job = -1;
            download_start_next_job();
            return;
        }
        if (download_fc.state == FETCH_FAILED) {
            signed_file_receive_abort(&download_fc.signed_rx);
            storage_delete(download_fc.tmp_path);
            job->state = DOWNLOAD_JOB_FAILED;
            snprintf(job->err, sizeof job->err, "%s", download_fc.err[0] ? download_fc.err : "fetch failed");
            snprintf(download_status_buf, sizeof download_status_buf,
                     "downloads=failed slot=%02X path=%s reason=\"%s\" queued=%lu",
                     (unsigned)job->slot,
                     job->path,
                     job->err,
                     (unsigned long)download_queued_count());
            remote_log(1, "+DOWNLOAD: failed slot=%02X path=%s reason=%s",
                       (unsigned)job->slot, job->path, job->err);
            download_active_job = -1;
            download_start_next_job();
            return;
        }
        download_refresh_status();
        return;
    }

    download_active_job = -1;
    download_start_next_job();
}

bool remote_http_download_start(const char *url, int slot, char *dest, size_t dest_len, char *err, size_t err_len)
{
    if (dest && dest_len) dest[0] = 0;
    if (err && err_len) err[0] = 0;
    if (!http_is_active) {
        if (err && err_len) snprintf(err, err_len, "wifi HTTP is not active");
        return false;
    }
    if (http_cfg.require_write_grant && !write_gate_active()) {
        if (err && err_len) snprintf(err, err_len, "write grant is not active");
        return false;
    }

    char host[128];
    char path[256];
    uint16_t port = 0;
    if (!parse_http_url(url, host, sizeof host, &port, path, sizeof path)) {
        if (err && err_len) snprintf(err, err_len, "expected http://host[:port]/path URL");
        return false;
    }

    uint32_t pending = download_queued_count() + (download_active_job >= 0 ? 1u : 0u);
    if (pending >= M65_DOWNLOAD_QUEUE_DEPTH) {
        if (err && err_len) snprintf(err, err_len, "download queue full");
        return false;
    }

    int use_slot = download_allocate_slot(slot);
    if (use_slot < 0) {
        if (err && err_len) snprintf(err, err_len, "download slot unavailable");
        return false;
    }

    int idx = download_find_free_job();
    if (idx < 0) {
        if (err && err_len) snprintf(err, err_len, "download queue full");
        return false;
    }

    if (!storage_mkdir("DOWNLOADS")) {
        if (err && err_len) snprintf(err, err_len, "cannot create DOWNLOADS: %s", storage_last_error());
        return false;
    }

    download_job_t *job = &download_jobs[idx];
    memset(job, 0, sizeof *job);
    job->state = DOWNLOAD_JOB_QUEUED;
    job->slot = (uint8_t)use_slot;
    snprintf(job->url, sizeof job->url, "%s", url);
    if (!fixed_download_slot_name(use_slot, job->name, sizeof job->name) ||
        !make_download_path(job->name, job->path, sizeof job->path)) {
        job->state = DOWNLOAD_JOB_EMPTY;
        if (err && err_len) snprintf(err, err_len, "download slot path too long");
        return false;
    }
    if (dest && dest_len) snprintf(dest, dest_len, "%s", job->path);
    snprintf(download_status_buf, sizeof download_status_buf,
             "downloads=queued slot=%02X path=%s queued=%lu",
             (unsigned)job->slot,
             job->path,
             (unsigned long)download_queued_count());
    remote_log(1, "+DOWNLOAD: queued slot=%02X url=%s path=%s",
               (unsigned)job->slot, job->url, job->path);

    if (download_active_job < 0) {
        if (!download_start_job((unsigned)idx) && job->state == DOWNLOAD_JOB_FAILED) {
            if (err && err_len) snprintf(err, err_len, "%s", job->err[0] ? job->err : "download start failed");
            return false;
        }
    }
    return true;
}

const char *remote_http_download_status(void)
{
    download_refresh_status();
    return download_status_buf;
}

static uint32_t clamp_interval_hours(uint32_t hours)
{
    if (hours < 3u) return 3u;
    return hours;
}

static uint32_t interval_ms_for_hours(uint32_t hours)
{
    uint64_t ms = (uint64_t)clamp_interval_hours(hours) * 3600000ull;
    if (ms > 0x7ffffffful) ms = 0x7ffffffful;
    return (uint32_t)ms;
}

static bool effective_autofetch_enabled(int enabled_override)
{
    if (enabled_override == 0) return false;
    if (enabled_override == 1) return true;
    return http_cfg.autofetch_enabled;
}

static uint32_t effective_fetch_interval(uint32_t interval_hours_override)
{
    return clamp_interval_hours(interval_hours_override ? interval_hours_override : http_cfg.fetch_interval_hours);
}

static uint8_t effective_fetch_board(uint8_t board_rev_override)
{
    if (board_rev_override == 3 || board_rev_override == 6) return board_rev_override;
    if (http_cfg.fetch_board_rev == 3 || http_cfg.fetch_board_rev == 6) return http_cfg.fetch_board_rev;
    return 0;
}

static void autofetch_schedule_after(uint32_t delay_ms)
{
    next_autofetch_due = make_timeout_time_ms(delay_ms);
    autofetch_schedule_valid = true;
}

void remote_http_autofetch_reset_schedule(void)
{
    autofetch_schedule_after(30000u);
}

typedef struct {
    const char *dir;
    uint32_t deleted;
} partial_cleanup_ctx_t;

static void delete_partial_tree(const char *dir, uint32_t *deleted);

static void delete_partial_cb(const char *name, uint32_t size, bool is_dir, void *ctx)
{
    (void)size;
    partial_cleanup_ctx_t *pc = (partial_cleanup_ctx_t *)ctx;
    if (!pc || !name || !name[0]) return;

    char path[256];
    if (!make_child_path(pc->dir, name, path, sizeof path)) return;
    if (is_dir) {
        if (!ci_equal(name, "WWW")) delete_partial_tree(path, &pc->deleted);
        return;
    }
    if (is_partial_core_path(path) && storage_delete(path)) pc->deleted++;
}

static void delete_partial_tree(const char *dir, uint32_t *deleted)
{
    partial_cleanup_ctx_t ctx = {
        .dir = (dir && dir[0]) ? dir : "/",
        .deleted = deleted ? *deleted : 0,
    };
    storage_list_cores(ctx.dir, delete_partial_cb, &ctx);
    if (deleted) *deleted = ctx.deleted;
}

static void delete_stale_partials_for_fetch(void)
{
    uint32_t deleted = 0;
    absolute_time_t start = get_absolute_time();
    remote_log(2, "+AUTOFETCH: partial-cleanup start");
    delete_partial_tree("/", &deleted);
    int64_t us = absolute_time_diff_us(start, get_absolute_time());
    if (us < 0) us = 0;
    remote_log(2, "+AUTOFETCH: partial-cleanup done deleted=%lu time_ms=%lu",
               (unsigned long)deleted,
               (unsigned long)(us / 1000ll));
    if (deleted) remote_log(1, "+AUTOFETCH: deleted stale partials=%lu", (unsigned long)deleted);
}

static void autofetch_cancel(const char *reason, uint32_t retry_delay_ms)
{
    bool had_status = autofetch_status_buf[0] != 0 &&
                      (strstr(autofetch_status_buf, "autofetch=failed") == autofetch_status_buf);
    if (autofetch_state == AUTOFETCH_MANIFEST || autofetch_state == AUTOFETCH_CORE) {
        if (autofetch_fc.state != FETCH_DONE) signed_file_receive_abort(&autofetch_fc.signed_rx);
        fetch_close(&autofetch_fc, true);
        storage_delete(autofetch_fc.tmp_path);
    }
    if ((autofetch_running || autofetch_state != AUTOFETCH_IDLE) && !had_status) {
        snprintf(autofetch_status_buf, sizeof autofetch_status_buf,
                 "autofetch=cancelled reason=\"%s\" checked=%lu needed=%lu updated=%lu",
                 reason ? reason : "activity",
                 (unsigned long)autofetch_checked_count,
                 (unsigned long)autofetch_needed_count,
                 (unsigned long)autofetch_updated_count);
    }
    autofetch_running = false;
    autofetch_state = AUTOFETCH_IDLE;
    autofetch_schedule_after(retry_delay_ms ? retry_delay_ms : 900000u);
}

void remote_http_autofetch_cancel(const char *reason)
{
    autofetch_cancel(reason ? reason : "operator activity", 900000u);
}

bool remote_http_autofetch_start_now(int enabled_override, uint32_t interval_hours_override, uint8_t board_rev_override)
{
    (void)enabled_override;
    if (!http_is_active) {
        snprintf(autofetch_status_buf, sizeof autofetch_status_buf, "autofetch=blocked reason=wifi-http-inactive");
        remote_log(1, "+AUTOFETCH: blocked reason=wifi-http-inactive");
        return false;
    }
    if (autofetch_running || autofetch_state != AUTOFETCH_IDLE) {
        return false;
    }

    uint32_t interval_hours = effective_fetch_interval(interval_hours_override);
    uint8_t board_rev = effective_fetch_board(board_rev_override);
    http_cfg.fetch_interval_hours = interval_hours;
    autofetch_schedule_valid = false;
    delete_stale_partials_for_fetch();
    if (!autofetch_start_manifest(board_rev)) {
        remote_log(1, "+AUTOFETCH: not-started %s", autofetch_status_buf);
        autofetch_schedule_after(900000u);
        return false;
    }
    return true;
}

static bool read_manifest_line(uint32_t *offset, char *line, size_t line_len, bool *eof)
{
    if (eof) *eof = false;
    if (!offset || !line || line_len == 0) return false;
    line[0] = 0;

    storage_file_t f = {0};
    if (!storage_open(&f, autofetch_manifest_path)) return false;
    uint32_t size = storage_size(&f);
    if (*offset >= size) {
        storage_close(&f);
        if (eof) *eof = true;
        return true;
    }
    if (!storage_seek(&f, *offset)) {
        storage_close(&f);
        return false;
    }

    size_t pos = 0;
    while (*offset < size) {
        char ch = 0;
        size_t got = 0;
        if (!storage_read(&f, &ch, 1, &got) || got != 1) {
            storage_close(&f);
            return false;
        }
        (*offset)++;
        if (ch == '\n') break;
        if (ch != '\r' && pos + 1 < line_len) line[pos++] = ch;
    }
    line[pos] = 0;
    storage_close(&f);
    return true;
}

static char *manifest_next_token(char **cursor)
{
    if (!cursor || !*cursor) return NULL;
    char *s = *cursor;
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s || *s == '#') {
        *cursor = s;
        return NULL;
    }
    char *tok = s;
    while (*s && !isspace((unsigned char)*s)) s++;
    if (*s) *s++ = 0;
    *cursor = s;
    return tok;
}

static bool is_hex64_token(const char *s)
{
    if (!s || strlen(s) != 64) return false;
    for (unsigned i = 0; i < 64u; ++i) {
        if (!isxdigit((unsigned char)s[i])) return false;
    }
    return true;
}

static void parse_manifest_attrs(char *s, manifest_entry_t *entry)
{
    char *cursor = s;
    for (;;) {
        char *tok = manifest_next_token(&cursor);
        if (!tok) break;
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq++ = 0;
        if (ci_equal(tok, "version")) snprintf(entry->version, sizeof entry->version, "%s", eq);
        else if (ci_equal(tok, "build")) snprintf(entry->build, sizeof entry->build, "%s", eq);
        else if (ci_equal(tok, "name")) snprintf(entry->name, sizeof entry->name, "%s", eq);
    }
}

static bool parse_manifest_entry(char *line,
                                 manifest_entry_t *entry,
                                 bool *skip,
                                 char *err,
                                 size_t err_len)
{
    if (skip) *skip = true;
    if (!entry) return false;
    memset(entry, 0, sizeof *entry);
    entry->kind = MANIFEST_KIND_SKIP;

    char *s = line;
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s || *s == '#') return true;

    char *cursor = s;
    char *first = manifest_next_token(&cursor);
    if (!first) return true;

    char *payload_hash = NULL;
    char *transfer_hash = NULL;
    char *rel = NULL;
    manifest_kind_t kind = MANIFEST_KIND_CORE;

    if (is_hex64_token(first)) {
        payload_hash = first;
        transfer_hash = manifest_next_token(&cursor);
        rel = manifest_next_token(&cursor);
    } else {
        if (!parse_manifest_kind(first, &kind)) {
            if (err && err_len) snprintf(err, err_len, "bad v3 manifest type");
            return false;
        }
        payload_hash = manifest_next_token(&cursor);
        transfer_hash = manifest_next_token(&cursor);
        rel = manifest_next_token(&cursor);
    }

    if (!payload_hash || !transfer_hash || !rel ||
        !is_hex64_token(payload_hash) || !is_hex64_token(transfer_hash)) {
        if (err && err_len) snprintf(err, err_len, "bad manifest line");
        return false;
    }
    uint8_t digest[32];
    if (!hex_to_bytes32(payload_hash, digest)) {
        if (err && err_len) snprintf(err, err_len, "bad manifest payload SHA-256");
        return false;
    }
    if (!hex_to_bytes32(transfer_hash, digest)) {
        if (err && err_len) snprintf(err, err_len, "bad manifest transfer SHA-256");
        return false;
    }
    if (!safe_manifest_source_path(rel, kind)) {
        if (err && err_len) snprintf(err, err_len, "unsafe manifest %s filename", manifest_kind_name(kind));
        return false;
    }

    entry->kind = kind;
    snprintf(entry->payload_sha, sizeof entry->payload_sha, "%s", payload_hash);
    snprintf(entry->transfer_sha, sizeof entry->transfer_sha, "%s", transfer_hash);
    snprintf(entry->rel, sizeof entry->rel, "%s", rel);
    parse_manifest_attrs(cursor, entry);
    if (skip) *skip = false;
    return true;
}

static uint32_t count_manifest_entries(void)
{
    uint32_t offset = 0;
    uint32_t total = 0;
    char line[512];
    for (;;) {
        bool eof = false;
        if (!read_manifest_line(&offset, line, sizeof line, &eof)) return total;
        if (eof) return total;

        manifest_entry_t entry;
        bool skip = true;
        char err[80];
        if (!parse_manifest_entry(line, &entry, &skip, err, sizeof err)) return total;
        if (!skip) total++;
    }
}

static bool autofetch_start_manifest(uint8_t board_rev)
{
    char channel[sizeof autofetch_channel];
    if (!safe_channel_name(http_cfg.fetch_channel[0] ? http_cfg.fetch_channel : "stable", channel, sizeof channel)) {
        snprintf(autofetch_status_buf, sizeof autofetch_status_buf, "autofetch=blocked reason=bad-channel");
        return false;
    }
    if (!http_cfg.fetch_base_url[0]) {
        snprintf(autofetch_status_buf, sizeof autofetch_status_buf, "autofetch=blocked reason=no-fetch-base-url");
        return false;
    }
    if (http_cfg.trusted_key_count == 0) {
        snprintf(autofetch_status_buf, sizeof autofetch_status_buf, "autofetch=blocked reason=no-trusted-keys");
        return false;
    }
    if (board_rev != 3 && board_rev != 6) {
        snprintf(autofetch_status_buf, sizeof autofetch_status_buf, "autofetch=blocked reason=no-fetch-board");
        return false;
    }

    snprintf(autofetch_base_url, sizeof autofetch_base_url, "%s", http_cfg.fetch_base_url);
    snprintf(autofetch_channel, sizeof autofetch_channel, "%s", channel);
    snprintf(autofetch_manifest_path, sizeof autofetch_manifest_path, "%s-r%u.sha256",
             autofetch_channel, (unsigned)board_rev);

    char url[320];
    if (!join_url_path(autofetch_base_url, autofetch_manifest_path, url, sizeof url)) {
        snprintf(autofetch_status_buf, sizeof autofetch_status_buf, "autofetch=blocked reason=manifest-url-too-long");
        return false;
    }

    autofetch_sig_cfg = http_cfg;
    autofetch_sig_cfg.require_signatures = true;
    char err[96];
    if (!fetch_start(&autofetch_fc, url, autofetch_manifest_path, &autofetch_sig_cfg, false, 120000u, err, sizeof err)) {
        snprintf(autofetch_status_buf, sizeof autofetch_status_buf, "autofetch=failed stage=manifest reason=\"%s\"", err);
        return false;
    }
    autofetch_manifest_offset = 0;
    autofetch_checked_count = 0;
    autofetch_needed_count = 0;
    autofetch_updated_count = 0;
    autofetch_manifest_total_count = 0;
    autofetch_fetch_retry_count = 0;
    autofetch_running = true;
    autofetch_state = AUTOFETCH_MANIFEST;
    snprintf(autofetch_status_buf, sizeof autofetch_status_buf,
             "autofetch=running stage=manifest channel=%s board=R%u",
             autofetch_channel, (unsigned)board_rev);
    return true;
}

static bool autofetch_start_entry(const manifest_entry_t *entry)
{
    if (!entry) return false;
    const char *final_path = manifest_final_path(entry->kind, entry->rel);
    char url[384];
    if (!join_url_path(autofetch_base_url, entry->rel, url, sizeof url)) {
        snprintf(autofetch_status_buf, sizeof autofetch_status_buf,
                 "autofetch=failed reason=url-too-long type=%s file=%s",
                 manifest_kind_name(entry->kind),
                 entry->rel);
        return false;
    }
    char err[96];
    if (!fetch_start(&autofetch_fc, url, final_path, &autofetch_sig_cfg, false, 600000u, err, sizeof err)) {
        snprintf(autofetch_status_buf, sizeof autofetch_status_buf,
                 "autofetch=failed type=%s file=%s reason=\"%s\"",
                 manifest_kind_name(entry->kind),
                 entry->rel,
                 err);
        return false;
    }
    snprintf(autofetch_pending_path, sizeof autofetch_pending_path, "%s", final_path);
    snprintf(autofetch_pending_url_path, sizeof autofetch_pending_url_path, "%s", entry->rel);
    snprintf(autofetch_pending_sha, sizeof autofetch_pending_sha, "%s", entry->payload_sha);
    snprintf(autofetch_pending_transfer_sha, sizeof autofetch_pending_transfer_sha, "%s", entry->transfer_sha);
    snprintf(autofetch_pending_version, sizeof autofetch_pending_version, "%s", entry->version);
    snprintf(autofetch_pending_build, sizeof autofetch_pending_build, "%s", entry->build);
    snprintf(autofetch_pending_name, sizeof autofetch_pending_name, "%s", entry->name);
    autofetch_pending_kind = entry->kind;
    autofetch_state = AUTOFETCH_CORE;
    snprintf(autofetch_status_buf, sizeof autofetch_status_buf,
             "autofetch=running stage=%s file=%s dest=%s checked=%lu total=%lu needed=%lu updated=%lu",
             manifest_kind_name(entry->kind),
             entry->rel,
             final_path,
             (unsigned long)autofetch_checked_count,
             (unsigned long)autofetch_manifest_total_count,
             (unsigned long)autofetch_needed_count,
             (unsigned long)autofetch_updated_count);
    return true;
}

static bool autofetch_retry_current_fetch(const char *reason)
{
    autofetch_fetch_retry_count++;
    if (autofetch_fetch_retry_count >= M65_AUTOFETCH_FILE_RETRIES) return false;
    const char *stage = autofetch_state == AUTOFETCH_MANIFEST ? "manifest" :
                        manifest_kind_name(autofetch_pending_kind);
    remote_log(1, "+AUTOFETCH: retry stage=%s failure=%lu/%lu reason=%s",
               stage,
               (unsigned long)autofetch_fetch_retry_count,
               (unsigned long)M65_AUTOFETCH_FILE_RETRIES,
               reason ? reason : "fetch failed");

    if (autofetch_state == AUTOFETCH_MANIFEST) {
        char url[320];
        if (!join_url_path(autofetch_base_url, autofetch_manifest_path, url, sizeof url)) {
            snprintf(autofetch_status_buf, sizeof autofetch_status_buf, "autofetch=failed stage=manifest reason=manifest-url-too-long");
            return false;
        }
        char err[96];
        if (!fetch_start(&autofetch_fc, url, autofetch_manifest_path, &autofetch_sig_cfg, false, 120000u, err, sizeof err)) {
            snprintf(autofetch_status_buf, sizeof autofetch_status_buf,
                     "autofetch=failed stage=manifest reason=\"%s\"", err);
            return false;
        }
        snprintf(autofetch_status_buf, sizeof autofetch_status_buf,
                 "autofetch=running stage=manifest-retry failure=%lu channel=%s",
                 (unsigned long)autofetch_fetch_retry_count,
                 autofetch_channel);
        return true;
    }

    if (autofetch_state == AUTOFETCH_CORE) {
        manifest_entry_t entry = {0};
        entry.kind = autofetch_pending_kind ? autofetch_pending_kind : MANIFEST_KIND_CORE;
        snprintf(entry.rel, sizeof entry.rel, "%s", autofetch_pending_url_path[0] ? autofetch_pending_url_path : autofetch_pending_path);
        snprintf(entry.payload_sha, sizeof entry.payload_sha, "%s", autofetch_pending_sha);
        snprintf(entry.transfer_sha, sizeof entry.transfer_sha, "%s", autofetch_pending_transfer_sha);
        snprintf(entry.version, sizeof entry.version, "%s", autofetch_pending_version);
        snprintf(entry.build, sizeof entry.build, "%s", autofetch_pending_build);
        snprintf(entry.name, sizeof entry.name, "%s", autofetch_pending_name);
        if (!autofetch_start_entry(&entry)) return false;
        snprintf(autofetch_status_buf, sizeof autofetch_status_buf,
                 "autofetch=running stage=%s-retry file=%s failure=%lu checked=%lu total=%lu needed=%lu updated=%lu",
                 manifest_kind_name(entry.kind),
                 entry.rel,
                 (unsigned long)autofetch_fetch_retry_count,
                 (unsigned long)autofetch_checked_count,
                 (unsigned long)autofetch_manifest_total_count,
                 (unsigned long)autofetch_needed_count,
                 (unsigned long)autofetch_updated_count);
        return true;
    }

    return false;
}

static void autofetch_skip_current_core(const char *reason)
{
    remote_log(1, "+AUTOFETCH: skip file=%s failures=%lu reason=%s",
               autofetch_pending_path[0] ? autofetch_pending_path : "(unset)",
               (unsigned long)autofetch_fetch_retry_count,
               reason ? reason : "fetch failed");
    snprintf(autofetch_status_buf, sizeof autofetch_status_buf,
             "autofetch=running stage=scan skipped=%s failures=%lu reason=\"%s\" checked=%lu total=%lu needed=%lu updated=%lu",
             autofetch_pending_path[0] ? autofetch_pending_path : "(unset)",
             (unsigned long)autofetch_fetch_retry_count,
             reason ? reason : "fetch failed",
             (unsigned long)autofetch_checked_count,
             (unsigned long)autofetch_manifest_total_count,
             (unsigned long)autofetch_needed_count,
             (unsigned long)autofetch_updated_count);
    autofetch_fetch_retry_count = 0;
    autofetch_pending_path[0] = 0;
    autofetch_pending_url_path[0] = 0;
    autofetch_pending_sha[0] = 0;
    autofetch_pending_transfer_sha[0] = 0;
    autofetch_pending_version[0] = 0;
    autofetch_pending_build[0] = 0;
    autofetch_pending_name[0] = 0;
    autofetch_pending_kind = MANIFEST_KIND_SKIP;
    autofetch_state = AUTOFETCH_SCAN;
}

static bool autofetch_scan_manifest(void)
{
    char line[512];
    for (;;) {
        bool eof = false;
        if (!read_manifest_line(&autofetch_manifest_offset, line, sizeof line, &eof)) {
            snprintf(autofetch_status_buf, sizeof autofetch_status_buf,
                     "autofetch=failed stage=manifest-scan reason=\"%s\"", storage_last_error());
            return false;
        }
        if (eof) {
            autofetch_last_success = get_absolute_time();
            autofetch_last_success_valid = true;
            autofetch_running = false;
            autofetch_state = AUTOFETCH_IDLE;
            autofetch_schedule_after(interval_ms_for_hours(http_cfg.fetch_interval_hours));
            snprintf(autofetch_status_buf, sizeof autofetch_status_buf,
                     "autofetch=idle last=success channel=%s checked=%lu total=%lu needed=%lu updated=%lu",
                     autofetch_channel,
                     (unsigned long)autofetch_checked_count,
                     (unsigned long)autofetch_manifest_total_count,
                     (unsigned long)autofetch_needed_count,
                     (unsigned long)autofetch_updated_count);
            remote_log(1, "+AUTOFETCH: done channel=%s checked=%lu total=%lu needed=%lu updated=%lu",
                       autofetch_channel,
                       (unsigned long)autofetch_checked_count,
                       (unsigned long)autofetch_manifest_total_count,
                       (unsigned long)autofetch_needed_count,
                       (unsigned long)autofetch_updated_count);
            return true;
        }

        manifest_entry_t entry;
        bool skip = true;
        char err[80];
        if (!parse_manifest_entry(line, &entry, &skip, err, sizeof err)) {
            snprintf(autofetch_status_buf, sizeof autofetch_status_buf,
                     "autofetch=failed stage=manifest-scan reason=\"%s\"", err);
            return false;
        }
        if (skip) continue;

        autofetch_checked_count++;
        const char *final_path = manifest_final_path(entry.kind, entry.rel);
        snprintf(autofetch_status_buf, sizeof autofetch_status_buf,
                 "autofetch=running stage=scan type=%s file=%s dest=%s checked=%lu total=%lu needed=%lu updated=%lu",
                 manifest_kind_name(entry.kind),
                 entry.rel,
                 final_path,
                 (unsigned long)autofetch_checked_count,
                 (unsigned long)autofetch_manifest_total_count,
                 (unsigned long)autofetch_needed_count,
                 (unsigned long)autofetch_updated_count);
        remote_log(1, "+AUTOFETCH: check type=%s file=%s checked=%lu/%lu",
                   manifest_kind_name(entry.kind),
                   entry.rel,
                   (unsigned long)autofetch_checked_count,
                   (unsigned long)autofetch_manifest_total_count);
        char have[65];
        const char *wanted_hash = manifest_kind_stores_signed_transfer(entry.kind) ?
                                  entry.transfer_sha : entry.payload_sha;
        if (file_sha256_hex(final_path, have, "scan") && ci_equal(have, wanted_hash)) {
            if (entry.kind == MANIFEST_KIND_FIRMWARE) record_firmware_candidate(&entry);
            else if (entry.kind == MANIFEST_KIND_THEME) record_theme_candidate(&entry);
            remote_log(1, "+AUTOFETCH: unchanged type=%s file=%s", manifest_kind_name(entry.kind), final_path);
            return true;
        }
        remote_log(1, "+AUTOFETCH: update type=%s file=%s dest=%s",
                   manifest_kind_name(entry.kind),
                   entry.rel,
                   final_path);
        autofetch_fetch_retry_count = 0;
        autofetch_needed_count++;
        if (!autofetch_start_entry(&entry)) {
            if (autofetch_needed_count) autofetch_needed_count--;
            return false;
        }
        return true;
    }
}

void remote_http_autofetch_poll(int enabled_override, uint32_t interval_hours_override, uint8_t board_rev_override)
{
    if (!http_is_active) return;

    uint32_t interval_hours = effective_fetch_interval(interval_hours_override);
    uint8_t board_rev = effective_fetch_board(board_rev_override);

    if (autofetch_state == AUTOFETCH_MANIFEST || autofetch_state == AUTOFETCH_CORE) {
        fetch_poll(&autofetch_fc);
        if (autofetch_fc.state == FETCH_FAILED) {
            const char *stage = autofetch_state == AUTOFETCH_MANIFEST ? "manifest" :
                                manifest_kind_name(autofetch_pending_kind);
            const char *reason = autofetch_fc.err[0] ? autofetch_fc.err : "fetch failed";
            storage_delete(autofetch_fc.tmp_path);
            if (autofetch_retry_current_fetch(reason)) return;
            if (autofetch_state == AUTOFETCH_CORE) {
                autofetch_skip_current_core(reason);
                return;
            }
            snprintf(autofetch_status_buf, sizeof autofetch_status_buf,
                     "autofetch=failed stage=%s reason=\"%s\"",
                     stage,
                     reason);
            remote_log(1, "+AUTOFETCH: failed stage=%s reason=%s",
                       stage,
                       reason);
            autofetch_cancel("fetch failed", 900000u);
            return;
        }
        if (autofetch_fc.state != FETCH_DONE) return;

        if (autofetch_state == AUTOFETCH_MANIFEST) {
            autofetch_manifest_total_count = count_manifest_entries();
            autofetch_state = AUTOFETCH_SCAN;
            snprintf(autofetch_status_buf, sizeof autofetch_status_buf,
                     "autofetch=running stage=scan channel=%s total=%lu",
                     autofetch_channel,
                     (unsigned long)autofetch_manifest_total_count);
            remote_log(1, "+AUTOFETCH: manifest channel=%s total=%lu",
                       autofetch_channel,
                       (unsigned long)autofetch_manifest_total_count);
        } else {
            char have[65];
            autofetch_state = AUTOFETCH_VERIFY;
            snprintf(autofetch_status_buf, sizeof autofetch_status_buf,
                     "autofetch=running stage=verify file=%s checked=%lu total=%lu needed=%lu updated=%lu",
                     autofetch_pending_path,
                     (unsigned long)autofetch_checked_count,
                     (unsigned long)autofetch_manifest_total_count,
                     (unsigned long)autofetch_needed_count,
                     (unsigned long)autofetch_updated_count);
            remote_log(1, "+AUTOFETCH: verify file=%s", autofetch_pending_path);
            if (!autofetch_fc.transfer_sha_hex[0] ||
                !ci_equal(autofetch_fc.transfer_sha_hex, autofetch_pending_transfer_sha)) {
                storage_delete(autofetch_pending_path);
                snprintf(autofetch_status_buf, sizeof autofetch_status_buf,
                         "autofetch=failed stage=verify-transfer file=%s", autofetch_pending_path);
                remote_log(1, "+AUTOFETCH: transfer hash mismatch file=%s have=%s want=%s",
                           autofetch_pending_path,
                           autofetch_fc.transfer_sha_hex[0] ? autofetch_fc.transfer_sha_hex : "(none)",
                           autofetch_pending_transfer_sha);
                autofetch_cancel("transfer hash mismatch", 900000u);
                return;
            }
            remote_log(2, "+AUTOFETCH: transfer hash verified file=%s", autofetch_pending_path);
            const char *wanted_hash = manifest_kind_stores_signed_transfer(autofetch_pending_kind) ?
                                      autofetch_pending_transfer_sha : autofetch_pending_sha;
            if (!file_sha256_hex(autofetch_pending_path, have, "verify") ||
                !ci_equal(have, wanted_hash)) {
                storage_delete(autofetch_pending_path);
                snprintf(autofetch_status_buf, sizeof autofetch_status_buf,
                         "autofetch=failed stage=verify-stored file=%s", autofetch_pending_path);
                autofetch_cancel("stored hash mismatch", 900000u);
                return;
            }
            autofetch_updated_count++;
            if (autofetch_pending_kind == MANIFEST_KIND_FIRMWARE) {
                manifest_entry_t entry = {0};
                entry.kind = MANIFEST_KIND_FIRMWARE;
                snprintf(entry.rel, sizeof entry.rel, "%s", autofetch_pending_url_path);
                snprintf(entry.version, sizeof entry.version, "%s", autofetch_pending_version);
                snprintf(entry.build, sizeof entry.build, "%s", autofetch_pending_build);
                record_firmware_candidate(&entry);
            } else if (autofetch_pending_kind == MANIFEST_KIND_THEME) {
                manifest_entry_t entry = {0};
                entry.kind = MANIFEST_KIND_THEME;
                snprintf(entry.rel, sizeof entry.rel, "%s", autofetch_pending_url_path);
                snprintf(entry.version, sizeof entry.version, "%s", autofetch_pending_version);
                snprintf(entry.name, sizeof entry.name, "%s", autofetch_pending_name);
                record_theme_candidate(&entry);
            }
            remote_log(1, "+AUTOFETCH: verified type=%s file=%s updated=%lu",
                       manifest_kind_name(autofetch_pending_kind),
                       autofetch_pending_path,
                       (unsigned long)autofetch_updated_count);
            autofetch_pending_path[0] = 0;
            autofetch_pending_url_path[0] = 0;
            autofetch_pending_sha[0] = 0;
            autofetch_pending_transfer_sha[0] = 0;
            autofetch_pending_version[0] = 0;
            autofetch_pending_build[0] = 0;
            autofetch_pending_name[0] = 0;
            autofetch_pending_kind = MANIFEST_KIND_SKIP;
            autofetch_state = AUTOFETCH_SCAN;
        }
    }

    if (autofetch_state == AUTOFETCH_SCAN) {
        if (!autofetch_scan_manifest()) {
            autofetch_cancel("manifest scan failed", 900000u);
        } else if (autofetch_state == AUTOFETCH_IDLE && !autofetch_running) {
            remote_log(2, "+AUTOFETCH: returned-to-main-loop state=idle checked=%lu total=%lu needed=%lu updated=%lu",
                       (unsigned long)autofetch_checked_count,
                       (unsigned long)autofetch_manifest_total_count,
                       (unsigned long)autofetch_needed_count,
                       (unsigned long)autofetch_updated_count);
        }
        return;
    }

    if (!effective_autofetch_enabled(enabled_override)) {
        if (!autofetch_running && autofetch_state == AUTOFETCH_IDLE) {
            snprintf(autofetch_status_buf, sizeof autofetch_status_buf,
                     "autofetch=disabled interval_hours=%lu board=R%u",
                     (unsigned long)interval_hours, (unsigned)board_rev);
        }
        return;
    }

    if (!autofetch_schedule_valid) autofetch_schedule_after(30000u);

    if (autofetch_state == AUTOFETCH_IDLE &&
        absolute_time_diff_us(get_absolute_time(), next_autofetch_due) <= 0) {
        http_cfg.fetch_interval_hours = interval_hours;
        delete_stale_partials_for_fetch();
        if (!autofetch_start_manifest(board_rev)) {
            remote_log(1, "+AUTOFETCH: not-started %s", autofetch_status_buf);
            autofetch_schedule_after(900000u);
        }
    }
}

const char *remote_http_autofetch_status(int enabled_override, uint32_t interval_hours_override, uint8_t board_rev_override)
{
    uint32_t interval_hours = effective_fetch_interval(interval_hours_override);
    uint8_t board_rev = effective_fetch_board(board_rev_override);
    const char *state = autofetch_status_buf[0] ? autofetch_status_buf : "autofetch=idle";
    uint32_t last_seconds = remote_http_autofetch_last_success_seconds();
    static char buf[320];
    snprintf(buf, sizeof buf,
             "%s enabled=%lu interval_hours=%lu board=R%u last_success_seconds=%lu running=%lu",
             state,
             (unsigned long)(effective_autofetch_enabled(enabled_override) ? 1u : 0u),
             (unsigned long)interval_hours,
             (unsigned)board_rev,
             (unsigned long)last_seconds,
             (unsigned long)(autofetch_running ? 1u : 0u));
    return buf;
}

uint32_t remote_http_autofetch_last_success_seconds(void)
{
    if (!autofetch_last_success_valid) return 0xffffffffu;
    int64_t us = absolute_time_diff_us(autofetch_last_success, get_absolute_time());
    return us > 0 ? (uint32_t)(us / 1000000ll) : 0u;
}

bool remote_http_autofetch_running(void)
{
    return autofetch_running;
}

const char *remote_http_firmware_status(void)
{
    bool have = firmware_update_available();
    snprintf(firmware_status_buf, sizeof firmware_status_buf,
             "firmware=%s current_version=\"%s\" current_build=%s pending_version=\"%s\" pending_build=%s path=%s source=%s update_supported=%lu",
             have ? "pending" : "none",
             M65_VERSION_STRING,
             M65_BUILD_MARKER,
             pending_firmware_version[0] ? pending_firmware_version : "",
             pending_firmware_build[0] ? pending_firmware_build : "(unknown)",
             have ? M65_FIRMWARE_PACKAGE_PATH : "(none)",
             pending_firmware_source[0] ? pending_firmware_source : "(unknown)",
             (unsigned long)M65_ENABLE_MCUBOOT_OTA);
    return firmware_status_buf;
}

bool remote_http_firmware_update(char *err, size_t err_len)
{
    if (!firmware_update_available()) {
        if (err && err_len) snprintf(err, err_len, "no pending firmware package");
        return false;
    }
    if (!signed_file_verify_stored(&http_cfg,
                                   M65_FIRMWARE_PACKAGE_PATH,
                                   M65_SIGNED_FILE_FIRMWARE,
                                   true)) {
        if (err && err_len) snprintf(err, err_len, "firmware signature check failed: %s", signed_file_last_error());
        return false;
    }
#if M65_ENABLE_MCUBOOT_OTA
    if (err && err_len) snprintf(err, err_len, "MCUboot OTA apply hook is not linked yet");
#else
    if (err && err_len) snprintf(err, err_len, "MCUboot OTA bootloader is not installed in this build");
#endif
    return false;
}

typedef struct {
    const char *dir;
    bool ok;
} delete_tree_ctx_t;

static void delete_tree(const char *dir, bool delete_self, bool *ok);

static void delete_tree_cb(const char *name, uint32_t size, bool is_dir, void *ctx)
{
    (void)size;
    delete_tree_ctx_t *dt = (delete_tree_ctx_t *)ctx;
    if (!dt || !dt->ok || !name || !name[0]) return;

    char path[256];
    if (!make_child_path(dt->dir, name, path, sizeof path)) {
        dt->ok = false;
        return;
    }
    if (is_dir) {
        delete_tree(path, true, &dt->ok);
    } else if (!storage_delete(path)) {
        dt->ok = false;
    }
}

static void delete_tree(const char *dir, bool delete_self, bool *ok)
{
    if (ok && !*ok) return;
    delete_tree_ctx_t ctx = {
        .dir = (dir && dir[0]) ? dir : "/",
        .ok = true,
    };
    if (!storage_list_dir(ctx.dir, delete_tree_cb, &ctx)) {
        ctx.ok = false;
    }
    if (ctx.ok && delete_self && !storage_delete(ctx.dir)) {
        ctx.ok = false;
    }
    if (ok) *ok = ctx.ok;
}

static bool tar_zero_block(const uint8_t block[512])
{
    for (unsigned i = 0; i < 512u; ++i) {
        if (block[i]) return false;
    }
    return true;
}

static bool tar_octal(const uint8_t *p, size_t len, uint32_t *out)
{
    uint32_t v = 0;
    bool any = false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = p[i];
        if (c == 0 || c == ' ') {
            if (any) break;
            continue;
        }
        if (c < '0' || c > '7') return false;
        any = true;
        if (v > (UINT32_MAX >> 3)) return false;
        v = (v << 3) | (uint32_t)(c - '0');
    }
    *out = v;
    return true;
}

static bool tar_checksum_ok(const uint8_t block[512])
{
    uint32_t stored = 0;
    if (!tar_octal(block + 148, 8, &stored)) return false;
    uint32_t sum = 0;
    for (unsigned i = 0; i < 512u; ++i) {
        sum += (i >= 148u && i < 156u) ? (uint8_t)' ' : block[i];
    }
    return sum == stored;
}

static bool tar_name(const uint8_t block[512], char *out, size_t out_len)
{
    char name[101];
    char prefix[156];
    memcpy(name, block, 100);
    name[100] = 0;
    memcpy(prefix, block + 345, 155);
    prefix[155] = 0;
    if (prefix[0]) {
        return snprintf(out, out_len, "%s/%s", prefix, name) < (int)out_len;
    }
    return snprintf(out, out_len, "%s", name) < (int)out_len;
}

static bool make_www_output_path(const char *rel, char *out, size_t out_len)
{
    char tmp[224];
    snprintf(tmp, sizeof tmp, "%s", rel ? rel : "");
    size_t n = strlen(tmp);
    while (n > 0 && tmp[n - 1] == '/') tmp[--n] = 0;
    if (!safe_www_path(tmp)) return false;
    if (!tmp[0]) return false;
    return snprintf(out, out_len, "WWW/%s", tmp) < (int)out_len;
}

static bool ensure_parent_dirs(const char *path)
{
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", path ? path : "");
    for (char *p = tmp; *p; ++p) {
        if (*p != '/') continue;
        *p = 0;
        if (tmp[0] && !storage_mkdir(tmp)) return false;
        *p = '/';
    }
    return true;
}

static bool theme_archive_process(const char *package_path, bool apply, char *err, size_t err_len)
{
    storage_file_t in = {0};
    if (!storage_open(&in, package_path)) {
        if (err && err_len) snprintf(err, err_len, "theme package not found");
        return false;
    }
    uint32_t package_size = storage_size(&in);
    if (package_size == 0 || package_size > M65_THEME_MAX_PACKAGE_BYTES) {
        storage_close(&in);
        if (err && err_len) snprintf(err, err_len, "theme package size is invalid");
        return false;
    }

    uint8_t header[512];
    uint8_t buf[M65_HTTP_IO_CHUNK];
    bool ok = true;
    uint32_t files = 0;
    while (ok && storage_tell(&in) < package_size) {
        size_t got = 0;
        if (!storage_read(&in, header, sizeof header, &got) || got != sizeof header) {
            ok = false;
            if (err && err_len) snprintf(err, err_len, "short tar header");
            break;
        }
        if (tar_zero_block(header)) break;
        if (!tar_checksum_ok(header)) {
            ok = false;
            if (err && err_len) snprintf(err, err_len, "bad tar checksum");
            break;
        }

        uint32_t size = 0;
        char rel[224];
        char out_path[256];
        char type = (char)header[156];
        if (!tar_octal(header + 124, 12, &size) ||
            !tar_name(header, rel, sizeof rel) ||
            !make_www_output_path(rel, out_path, sizeof out_path)) {
            ok = false;
            if (err && err_len) snprintf(err, err_len, "unsafe tar member");
            break;
        }
        if (type == 0) type = '0';
        if (type != '0' && type != '5') {
            ok = false;
            if (err && err_len) snprintf(err, err_len, "unsupported tar member type");
            break;
        }

        if (apply) {
            if (type == '5') {
                if (!storage_mkdir(out_path)) {
                    ok = false;
                    if (err && err_len) snprintf(err, err_len, "cannot create theme directory: %s", storage_last_error());
                    break;
                }
            } else {
                if (!ensure_parent_dirs(out_path)) {
                    ok = false;
                    if (err && err_len) snprintf(err, err_len, "cannot create theme parent directory");
                    break;
                }
                storage_file_t out = {0};
                if (!storage_open_write(&out, out_path, true)) {
                    ok = false;
                    if (err && err_len) snprintf(err, err_len, "cannot create theme file: %s", storage_last_error());
                    break;
                }
                uint32_t remain = size;
                while (remain) {
                    size_t want = remain > sizeof buf ? sizeof buf : remain;
                    size_t rd = 0;
                    size_t wr = 0;
                    if (!storage_read(&in, buf, want, &rd) || rd != want ||
                        !storage_write(&out, buf, rd, &wr) || wr != rd) {
                        ok = false;
                        if (err && err_len) snprintf(err, err_len, "theme file copy failed: %s", storage_last_error());
                        break;
                    }
                    remain -= (uint32_t)rd;
                }
                if (!storage_sync(&out)) {
                    ok = false;
                    if (err && err_len) snprintf(err, err_len, "theme file sync failed: %s", storage_last_error());
                }
                storage_close(&out);
                if (!ok) break;
            }
        } else if (type == '0') {
            if (!storage_seek(&in, storage_tell(&in) + size)) {
                ok = false;
                if (err && err_len) snprintf(err, err_len, "theme seek failed");
                break;
            }
        }

        if (type == '0' && apply) {
            files++;
        } else if (type == '0') {
            files++;
        }
        uint32_t pad = (512u - (size & 511u)) & 511u;
        if (pad && !storage_seek(&in, storage_tell(&in) + pad)) {
            ok = false;
            if (err && err_len) snprintf(err, err_len, "theme padding seek failed");
            break;
        }
    }
    storage_close(&in);
    if (ok && files == 0) {
        if (err && err_len) snprintf(err, err_len, "theme archive contains no files");
        return false;
    }
    return ok;
}

const char *remote_http_theme_status(void)
{
    (void)theme_update_available();
    theme_scan_ctx_t scan;
    uint32_t count = theme_scan(&scan);
    snprintf(theme_status_buf, sizeof theme_status_buf,
             "theme=%s count=%lu selected=\"%s\" name=\"%s\" version=\"%s\" dir=%s source=%s",
             count ? "available" : "none",
             (unsigned long)count,
             scan.first,
             pending_theme_name[0] ? pending_theme_name : "",
             pending_theme_version[0] ? pending_theme_version : "",
             M65_THEME_DIR_PATH,
             pending_theme_source[0] ? pending_theme_source : "(unknown)");
    return theme_status_buf;
}

bool remote_http_theme_install(char *err, size_t err_len)
{
    return remote_http_theme_install_named(NULL, err, err_len);
}

bool remote_http_theme_install_named(const char *theme_name, char *err, size_t err_len)
{
    if (!theme_update_available()) {
        if (err && err_len) snprintf(err, err_len, "no pending theme package");
        return false;
    }
    if (!check_write_grant()) {
        if (err && err_len) snprintf(err, err_len, "write grant is not active");
        return false;
    }
    char package_path[256];
    char selected_name[192];
    if (!select_theme_package(theme_name, package_path, sizeof package_path, selected_name, sizeof selected_name)) {
        if (err && err_len) snprintf(err, err_len, "theme package not found");
        return false;
    }
    if (!signed_file_verify_stored(&http_cfg,
                                   package_path,
                                   M65_SIGNED_FILE_THEME,
                                   true)) {
        if (err && err_len) snprintf(err, err_len, "theme signature check failed: %s", signed_file_last_error());
        return false;
    }
    if (!theme_archive_process(package_path, false, err, err_len)) return false;
    (void)storage_mkdir("WWW");

    bool ok = true;
    delete_tree("WWW", false, &ok);
    if (!ok) {
        if (err && err_len) snprintf(err, err_len, "cannot clear WWW directory");
        return false;
    }
    if (!storage_mkdir("WWW")) {
        if (err && err_len) snprintf(err, err_len, "cannot create WWW directory: %s", storage_last_error());
        return false;
    }
    if (!theme_archive_process(package_path, true, err, err_len)) return false;
    load_cached_web_assets();
    if (err && err_len) snprintf(err, err_len, "theme installed: %s", selected_name);
    return true;
}

static err_t http_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || !newpcb) return ERR_VAL;
    if (http_conn.in_use) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    memset(&http_conn, 0, sizeof http_conn);
    http_conn.pcb = newpcb;
    http_conn.in_use = true;
    http_conn.state = HTTP_RECV_HEADERS;
    http_conn.remote_ip = lwip_ip_to_host_u32(&newpcb->remote_ip);

    tcp_arg(newpcb, &http_conn);
    tcp_recv(newpcb, http_recv);
    tcp_sent(newpcb, http_sent);
    tcp_err(newpcb, http_err);
    tcp_poll(newpcb, http_poll_cb, 2);
    return ERR_OK;
}

static void close_http_listener(void)
{
    if (!listen_pcb) return;
    tcp_accept(listen_pcb, NULL);
    if (tcp_close(listen_pcb) == ERR_OK) listen_pcb = NULL;
}

static bool listen_http(uint16_t port)
{
    close_http_listener();
    listen_pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!listen_pcb) return false;
    err_t err = tcp_bind(listen_pcb, IP_ANY_TYPE, port);
    if (err != ERR_OK) {
        tcp_close(listen_pcb);
        listen_pcb = NULL;
        return false;
    }
    listen_pcb = tcp_listen_with_backlog(listen_pcb, 1);
    if (!listen_pcb) return false;
    tcp_accept(listen_pcb, http_accept);
    return true;
}

static bool configure_static_ip(void)
{
    struct netif *n = &cyw43_state.netif[CYW43_ITF_STA];
    ip4_addr_t ip, mask, gw;
    cfg_ip_to_lwip(http_cfg.static_ip, &ip);
    cfg_ip_to_lwip(http_cfg.netmask, &mask);
    cfg_ip_to_lwip(http_cfg.gateway, &gw);
    dhcp_stop(n);
    netif_set_addr(n, &ip, &mask, &gw);
    return true;
}

static bool remote_http_probe_wifi_hardware(void)
{
    if (wifi_hardware_blocked) {
        wifi_hw_status_blocked();
        return false;
    }
    if (cyw43_is_ready) return true;
    if (wifi_probe_retry_scheduled) {
        snprintf(http_status_buf, sizeof http_status_buf,
                 "wifi=probe-retry stage=%s attempts=%lu in=%lus",
                 wifi_hw_stage_name(wifi_hardware_fault_stage),
                 (unsigned long)wifi_probe_attempts,
                 (unsigned long)wifi_probe_retry_seconds());
        return false;
    }

    ++wifi_probe_attempts;
    wifi_init_rc = -9999;
    snprintf(http_status_buf, sizeof http_status_buf, "wifi=probing hardware=cyw43");
    watchdog_hw->scratch[0] = WIFI_HW_PROBE_MAGIC;
    watchdog_hw->scratch[1] = WIFI_HW_STAGE_INIT;
    watchdog_hw->scratch[2] = wifi_probe_attempts;
    watchdog_enable(M65_WIFI_HW_PROBE_WATCHDOG_MS, false);

    wifi_init_rc = cyw43_arch_init();
    if (wifi_init_rc != 0) {
        watchdog_disable();
        wifi_hw_probe_marker_clear();
        wifi_hardware_fault_stage = WIFI_HW_STAGE_INIT;
        if (wifi_probe_can_retry()) {
            wifi_schedule_probe_retry("init-failed", WIFI_HW_STAGE_INIT);
        } else {
            wifi_hardware_blocked = true;
            snprintf(http_status_buf, sizeof http_status_buf,
                     "wifi=disabled hardware=cyw43-init-failed init_rc=%d attempts=%lu",
                     wifi_init_rc,
                     (unsigned long)wifi_probe_attempts);
        }
        return false;
    }

    watchdog_disable();
    wifi_hw_probe_marker_clear();
    cyw43_is_ready = true;
    wifi_hardware_known_present = true;
    wifi_probe_retry_scheduled = false;
    wifi_poll_due = get_absolute_time();
    snprintf(http_status_buf, sizeof http_status_buf, "wifi=inactive hardware=cyw43");
    return true;
}

void remote_http_boot_check(void)
{
    if (watchdog_caused_reboot() && watchdog_hw->scratch[0] == WIFI_HW_PROBE_MAGIC) {
        wifi_hardware_fault_stage = watchdog_hw->scratch[1];
        wifi_probe_attempts = watchdog_hw->scratch[2] ? watchdog_hw->scratch[2] : 1u;
        if (wifi_hardware_fault_stage != WIFI_HW_STAGE_INIT) {
            wifi_hardware_known_present = true;
        }
        if (wifi_probe_can_retry()) {
            wifi_schedule_probe_retry("driver-timeout", wifi_hardware_fault_stage);
        } else {
            wifi_hardware_blocked = true;
            wifi_hw_status_blocked();
        }
    }
    wifi_hw_probe_marker_clear();
}

static const char *wifi_link_name(int status)
{
    switch (status) {
    case CYW43_LINK_DOWN: return "down";
    case CYW43_LINK_JOIN: return "join";
    case CYW43_LINK_NOIP: return "noip";
    case CYW43_LINK_UP: return "up";
    case CYW43_LINK_FAIL: return "fail";
    case CYW43_LINK_NONET: return "nonet";
    case CYW43_LINK_BADAUTH: return "badauth";
    default: return "unknown";
    }
}

static void remote_http_finish_wifi_join(void)
{
    if (!listen_http(http_cfg.http_port)) {
        wifi_schedule_recover("http-listen-failed");
        return;
    }

    struct netif *n = &cyw43_state.netif[CYW43_ITF_STA];
    uint32_t ip = lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(n)));
    char ip_text[24];
    remote_auth_format_ipv4(ip, ip_text, sizeof ip_text);
    snprintf(http_status_buf, sizeof http_status_buf, "wifi=up ip=%s port=%u", ip_text, (unsigned)http_cfg.http_port);
    http_is_active = true;
    wifi_assoc_state = WIFI_ASSOC_HTTP_ACTIVE;
    wifi_noip_timer_active = false;
    wifi_clear_recover();
    wifi_recover_first_retry_used = false;
}

static void remote_http_poll_wifi_join(void)
{
    absolute_time_t now = get_absolute_time();
    int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    if (status == CYW43_LINK_UP) {
        remote_http_finish_wifi_join();
        return;
    }

    if (status == CYW43_LINK_NOIP) {
        if (!wifi_noip_timer_active) {
            wifi_noip_timer_active = true;
            wifi_noip_since = now;
            wifi_assoc_deadline = make_timeout_time_ms(M65_WIFI_NOIP_TIMEOUT_MS + 1000u);
        } else if (absolute_time_diff_us(wifi_noip_since, now) >= (int64_t)M65_WIFI_NOIP_TIMEOUT_MS * 1000ll) {
            wifi_schedule_recover("dhcp-timeout");
            return;
        }
    } else {
        wifi_noip_timer_active = false;
    }

    if (status == CYW43_LINK_NONET) {
        if (absolute_time_diff_us(now, wifi_assoc_retry_at) <= 0) {
            int rc = cyw43_arch_wifi_connect_async(http_cfg.wifi_ssid,
                                                   http_cfg.wifi_psk[0] ? http_cfg.wifi_psk : NULL,
                                                   CYW43_AUTH_WPA2_AES_PSK);
            wifi_assoc_retry_at = make_timeout_time_ms(1000u);
            if (rc != 0) {
                wifi_schedule_recover("connect-retry-failed");
                return;
            }
        }
        status = CYW43_LINK_JOIN;
    } else if (status < 0) {
        wifi_schedule_recover(wifi_link_name(status));
        return;
    }

    if (absolute_time_diff_us(now, wifi_assoc_deadline) <= 0) {
        wifi_schedule_recover("connect-timeout");
        return;
    }

    if (status != wifi_assoc_last_status) {
        wifi_assoc_last_status = status;
        snprintf(http_status_buf, sizeof http_status_buf, "wifi=connecting status=%s ssid=%s",
                 wifi_link_name(status), http_cfg.wifi_ssid);
    }
}

void remote_http_init(void)
{
    http_is_active = false;
    wifi_assoc_state = WIFI_ASSOC_OFF;
    wifi_assoc_last_status = CYW43_LINK_DOWN - 99;
    wifi_clear_recover();

    if (!cyw43_is_ready) {
        if (wifi_hardware_blocked) {
            wifi_hw_status_blocked();
            return;
        }
        if (wifi_probe_retry_scheduled) {
            remote_init_after_probe_pending = true;
            snprintf(http_status_buf, sizeof http_status_buf,
                     "wifi=probe-retry stage=%s attempts=%lu in=%lus remote=init",
                     wifi_hw_stage_name(wifi_hardware_fault_stage),
                     (unsigned long)wifi_probe_attempts,
                     (unsigned long)wifi_probe_retry_seconds());
            return;
        }
        wifi_probe_pending = true;
        remote_init_after_probe_pending = true;
        snprintf(http_status_buf, sizeof http_status_buf, "wifi=probe-pending hardware=cyw43 remote=init");
        return;
    }

    if (!storage_mount()) {
        wifi_schedule_recover("config-unavailable");
        return;
    }

    char err[96];
    if (!remote_auth_load(&http_cfg, err, sizeof err)) {
        snprintf(http_status_buf, sizeof http_status_buf, "wifi=disabled hardware=cyw43 remote=%s", err);
        return;
    }
    if (machine_identity_board_rev() == 0u &&
        (http_cfg.fetch_board_rev == 3u || http_cfg.fetch_board_rev == 6u)) {
        machine_identity_set_board_rev(http_cfg.fetch_board_rev);
    }
    if (!http_cfg.http_enabled) {
        snprintf(http_status_buf, sizeof http_status_buf, "wifi=disabled hardware=cyw43 http=0");
        return;
    }
    load_cached_web_assets();
    if (!http_cfg.wifi_ssid[0]) {
        snprintf(http_status_buf, sizeof http_status_buf, "wifi=disabled hardware=cyw43 ssid=missing");
        return;
    }

    watchdog_hw->scratch[0] = WIFI_HW_PROBE_MAGIC;
    watchdog_hw->scratch[1] = WIFI_HW_STAGE_STA;
    watchdog_hw->scratch[2] = wifi_probe_attempts;
    watchdog_enable(M65_WIFI_HW_PROBE_WATCHDOG_MS, false);

    cyw43_arch_enable_sta_mode();
    if (!http_cfg.dhcp) configure_static_ip();

    watchdog_hw->scratch[1] = WIFI_HW_STAGE_JOIN;
    int rc = cyw43_arch_wifi_connect_async(http_cfg.wifi_ssid,
                                           http_cfg.wifi_psk[0] ? http_cfg.wifi_psk : NULL,
                                           CYW43_AUTH_WPA2_AES_PSK);
    watchdog_disable();
    wifi_hw_probe_marker_clear();
    if (rc != 0) {
        wifi_schedule_recover("connect-start-failed");
        return;
    }

    wifi_assoc_deadline = make_timeout_time_ms(M65_WIFI_CONNECT_TIMEOUT_MS);
    wifi_assoc_retry_at = make_timeout_time_ms(1000u);
    wifi_noip_timer_active = false;
    wifi_assoc_state = WIFI_ASSOC_CONNECTING;
    wifi_assoc_last_status = CYW43_LINK_DOWN - 99;
    snprintf(http_status_buf, sizeof http_status_buf, "wifi=connecting status=start ssid=%s", http_cfg.wifi_ssid);
}

void remote_http_poll(void)
{
    remote_log_wifi_change();
    absolute_time_t now = get_absolute_time();
    if (wifi_probe_retry_scheduled && absolute_time_diff_us(now, wifi_probe_retry_at) <= 0) {
        wifi_probe_retry_scheduled = false;
        wifi_probe_pending = true;
    }
    if (wifi_probe_pending) {
        wifi_probe_pending = false;
        bool ok = remote_http_probe_wifi_hardware();
        if (ok && remote_init_after_probe_pending) {
            remote_init_after_probe_pending = false;
            remote_http_init();
        }
    }
    if (!cyw43_is_ready) return;
    if (absolute_time_diff_us(now, wifi_poll_due) > 0) return;
    wifi_poll_due = delayed_by_us(now, M65_WIFI_POLL_INTERVAL_US);
    cyw43_arch_poll();
    if (wifi_assoc_state == WIFI_ASSOC_FAILED && wifi_recover_scheduled &&
        absolute_time_diff_us(now, wifi_recover_at) <= 0) {
        wifi_recover_scheduled = false;
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        remote_http_init();
        return;
    }
    if (wifi_assoc_state == WIFI_ASSOC_CONNECTING) remote_http_poll_wifi_join();
    if (http_is_active) remote_http_download_poll();
    remote_log_wifi_change();
}

bool remote_http_active(void)
{
    return http_is_active;
}

const char *remote_http_status(void)
{
    return http_status_buf;
}

const char *remote_http_wifi_summary(void)
{
    if (cyw43_is_ready || wifi_hardware_known_present) return "BUILT-IN";
    if (wifi_hardware_blocked) return "PROBE FAILED";
    return "PROBING";
}

const char *remote_http_wifi_diag(void)
{
    snprintf(wifi_diag_buf, sizeof wifi_diag_buf,
             "supported=1 board=%s ready=%lu blocked=%lu known_present=%lu pending=%lu probe_retry=%lu probe_retry_seconds=%lu remote_after_probe=%lu attempts=%lu init_rc=%d fault_stage=%s assoc=%u noip=%lu retry=%lu retry_seconds=%lu status=\"%s\"",
             M65_PICO_BOARD_NAME,
             (unsigned long)(cyw43_is_ready ? 1u : 0u),
             (unsigned long)(wifi_hardware_blocked ? 1u : 0u),
             (unsigned long)(wifi_hardware_known_present ? 1u : 0u),
             (unsigned long)(wifi_probe_pending ? 1u : 0u),
             (unsigned long)(wifi_probe_retry_scheduled ? 1u : 0u),
             (unsigned long)wifi_probe_retry_seconds(),
             (unsigned long)(remote_init_after_probe_pending ? 1u : 0u),
             (unsigned long)wifi_probe_attempts,
             wifi_init_rc,
             wifi_hw_stage_name(wifi_hardware_fault_stage),
             (unsigned)wifi_assoc_state,
             (unsigned long)(wifi_noip_timer_active ? 1u : 0u),
             (unsigned long)(wifi_recover_scheduled ? 1u : 0u),
             (unsigned long)wifi_recover_seconds(),
             http_status_buf);
    return wifi_diag_buf;
}

bool remote_http_wifi_probe_now(void)
{
    if (cyw43_is_ready) {
        return true;
    }
    wifi_hardware_blocked = false;
    wifi_hardware_fault_stage = 0;
    wifi_probe_attempts = 0;
    wifi_init_rc = -9999;
    wifi_probe_retry_scheduled = false;
    wifi_probe_pending = true;
    snprintf(http_status_buf, sizeof http_status_buf, "wifi=probe-pending hardware=cyw43 manual=1");
    return true;
}

#endif
