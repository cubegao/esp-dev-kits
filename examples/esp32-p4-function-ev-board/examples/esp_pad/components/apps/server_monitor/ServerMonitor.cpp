/*
 * Server Monitor app - Blue/white 2-column card UI, refreshes every 5 seconds
 * from https://tz.cubegao.com/?action=api
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"

#include "ServerMonitor.hpp"
#include "server_cert.h"

LV_IMG_DECLARE(img_app_server_monitor);

static const char *TAG = "ServerMonitor";

/* ---- colour palette (light theme, blue primary) ---- */
#define CLR_PAGE_BG     lv_color_make(0xf6, 0xf8, 0xf7) /* #f6f8f7 */
#define CLR_SURFACE     lv_color_make(0xff, 0xff, 0xff) /* white */
#define CLR_PRIMARY     lv_color_make(0x3b, 0x82, 0xf6) /* blue-500 */
#define CLR_PRIMARY_LT  lv_color_make(0xdb, 0xea, 0xfe) /* blue-100 */
#define CLR_TEXT        lv_color_make(0x25, 0x2a, 0x31) /* slate-800 */
#define CLR_TEXT_SEC    lv_color_make(0x6b, 0x74, 0x82) /* slate-500 */
#define CLR_TEXT_MUTED  lv_color_make(0x9a, 0xa2, 0xad) /* slate-400 */
#define CLR_BORDER      lv_color_make(0xe4, 0xe8, 0xec) /* slate-200 */
#define CLR_BORDER_LT   lv_color_make(0xea, 0xed, 0xe9) /* border-light */
#define CLR_ACCENT      lv_color_make(0x3a, 0x41, 0x4c) /* slate-700 */
#define CLR_BAR_TRACK   lv_color_make(0xe4, 0xe8, 0xec)
#define CLR_BAR_FILL    CLR_PRIMARY

typedef struct {
    char *buf;
    int   len;
    int   cap;
} http_acc_t;

static const char *API_URL_BASE = "https://tz.cubegao.com/?action=api&_=";

static void fmt_bytes(uint64_t b, char *out, size_t n)
{
    static const char *u[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double v = (double)b;
    int i = 0;
    while (v >= 1024.0 && i < 5) {
        v /= 1024.0;
        i++;
    }
    if (i == 0) {
        snprintf(out, n, "%llu %s", (unsigned long long)b, u[i]);
    } else {
        snprintf(out, n, "%.1f %s", v, u[i]);
    }
}

/* Helper: format a float to string (avoids LVGL %f which may be unsupported) */
static void fmt_pct(float val, char *out, size_t n)
{
    int whole = (int)val;
    int frac  = (int)((val - whole) * 10 + 0.5f);
    if (frac < 0) frac = 0;
    if (frac > 9) { frac = 0; whole++; }
    snprintf(out, n, "%d.%d%%", whole, frac);
}

namespace {
lv_obj_t *makeCard(lv_obj_t *parent, const char *title,
                   lv_obj_t **big, lv_obj_t **sub, lv_obj_t **bar)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_style_bg_color(card, CLR_SURFACE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, CLR_BORDER, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_20, 0);
    lv_obj_set_style_shadow_width(card, 16, 0);
    lv_obj_set_style_shadow_ofs_y(card, 4, 0);
    lv_obj_set_style_shadow_color(card, lv_color_make(0, 0, 0), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, 14, 0);

    lv_obj_t *tl = lv_label_create(card);
    lv_label_set_text(tl, title);
    lv_obj_set_style_text_font(tl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(tl, CLR_TEXT_MUTED, 0);

    *big = lv_label_create(card);
    lv_label_set_text(*big, "--");
    lv_obj_set_style_text_font(*big, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(*big, CLR_TEXT, 0);
    lv_obj_set_width(*big, lv_pct(100));
    lv_label_set_long_mode(*big, LV_LABEL_LONG_SCROLL_CIRCULAR);

    if (bar) {
        *bar = lv_bar_create(card);
        lv_obj_set_width(*bar, lv_pct(100));
        lv_obj_set_height(*bar, 10);
        lv_bar_set_range(*bar, 0, 100);
        lv_bar_set_value(*bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_radius(*bar, 5, LV_PART_MAIN);
        lv_obj_set_style_bg_color(*bar, CLR_BAR_TRACK, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(*bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(*bar, 5, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(*bar, CLR_BAR_FILL, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(*bar, LV_OPA_COVER, LV_PART_INDICATOR);
    }

    if (sub) {
        *sub = lv_label_create(card);
        lv_label_set_text(*sub, "");
        lv_obj_set_style_text_font(*sub, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(*sub, CLR_TEXT_SEC, 0);
        lv_obj_set_width(*sub, lv_pct(100));
    }

    return card;
}
} // namespace

/* ------------------------------------------------------------------ */

ServerMonitor::ServerMonitor():
    ESP_Brookesia_PhoneApp("Server Monitor", &img_app_server_monitor, true),
    _root(NULL), _subtitle(NULL),
    _cpu_val(NULL), _cpu_sub(NULL), _cpu_bar(NULL),
    _mem_val(NULL), _mem_sub(NULL), _mem_bar(NULL),
    _disk_val(NULL), _disk_sub(NULL), _disk_bar(NULL),
    _net_val(NULL), _net_sub(NULL),
    _dots{NULL}, _ts_label(NULL),
    _timer(NULL), _http_task_handle(NULL),
    _dot_idx(0), _running(false), _paused(false), _mutex(NULL)
{
    memset(&_data, 0, sizeof(_data));
    _last_url_ts = 0;
}

ServerMonitor::~ServerMonitor()
{
    stopRefresh();
    if (_mutex) {
        vSemaphoreDelete(_mutex);
        _mutex = NULL;
    }
}

bool ServerMonitor::init(void) { return true; }

bool ServerMonitor::run(void)
{
    createUi();
    startRefresh();
    return true;
}

bool ServerMonitor::back(void)
{
    stopRefresh();
    notifyCoreClosed();
    return true;
}

bool ServerMonitor::close(void)
{
    stopRefresh();
    return true;
}

bool ServerMonitor::pause(void)
{
    ESP_LOGI(TAG, "paused");
    _paused = true;
    stopRefresh();
    return true;
}

bool ServerMonitor::resume(void)
{
    ESP_LOGI(TAG, "resumed");
    _paused = false;
    startRefresh();
    return true;
}

/* ------------------------------------------------------------------ */

void ServerMonitor::createUi(void)
{
    lv_area_t area = getVisualArea();
    int W = area.x2 - area.x1;
    int H = area.y2 - area.y1;

    /* ---- root: vertical scroll ---- */
    _root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_root, W, H);
    lv_obj_align(_root, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(_root, CLR_PAGE_BG, 0);
    lv_obj_set_style_bg_opa(_root, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(_root, 0, 0);
    lv_obj_set_style_border_width(_root, 0, 0);
    lv_obj_set_style_pad_all(_root, 0, 0);
    lv_obj_set_flex_flow(_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(_root, 10, 0);
    lv_obj_set_style_pad_top(_root, 4, 0);
    lv_obj_set_style_pad_bottom(_root, 24, 0);
    lv_obj_set_style_pad_hor(_root, 16, 0);
    lv_obj_add_flag(_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(_root, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(_root, LV_SCROLLBAR_MODE_AUTO);

    /* ---- title (centered) ---- */
    lv_obj_t *title = lv_label_create(_root);
    lv_label_set_text(title, "Server Monitor");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(title, CLR_TEXT, 0);
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    /* ---- info row: subtitle (centered) ---- */
    _subtitle = lv_label_create(_root);
    lv_obj_set_style_text_font(_subtitle, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_subtitle, CLR_TEXT_SEC, 0);
    lv_label_set_text(_subtitle, "Loading\u2026");
    lv_obj_set_width(_subtitle, lv_pct(100));
    lv_obj_set_style_text_align(_subtitle, LV_TEXT_ALIGN_CENTER, 0);

    /* ---- card rows ---- */
    auto makeRow = [&](const char *t1, lv_obj_t **v1, lv_obj_t **s1, lv_obj_t **b1,
                       const char *t2, lv_obj_t **v2, lv_obj_t **s2, lv_obj_t **b2) {
        lv_obj_t *row = lv_obj_create(_root);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, 200);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

        lv_obj_t *c1 = makeCard(row, t1, v1, s1, b1);
        lv_obj_set_width(c1, LV_PCT(48));
        lv_obj_set_height(c1, lv_pct(100));
        lv_obj_t *c2 = makeCard(row, t2, v2, s2, b2);
        lv_obj_set_width(c2, LV_PCT(48));
        lv_obj_set_height(c2, lv_pct(100));

        return row;
    };

    /* Row 1: CPU + Memory */
    makeRow("CPU",      &_cpu_val, &_cpu_sub, &_cpu_bar,
            "Memory",   &_mem_val, &_mem_sub, &_mem_bar);
    /* Row 2: Disk + Network */
    makeRow("Disk",     &_disk_val, &_disk_sub, &_disk_bar,
            "Network",  &_net_val, &_net_sub,  NULL);

    /* ---- bottom bar: raw timestamp + 5 dots (right-aligned) ---- */
    lv_obj_t *bottom_bar = lv_obj_create(_root);
    lv_obj_set_width(bottom_bar, lv_pct(100));
    lv_obj_set_height(bottom_bar, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(bottom_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bottom_bar, 0, 0);
    lv_obj_set_style_pad_all(bottom_bar, 0, 0);
    lv_obj_set_style_pad_top(bottom_bar, 4, 0);
    lv_obj_set_flex_flow(bottom_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bottom_bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* spacer to push content far right */
    lv_obj_t *spacer = lv_label_create(bottom_bar);
    lv_obj_set_flex_grow(spacer, 2);
    lv_label_set_text(spacer, "");
    lv_obj_set_style_text_font(spacer, &lv_font_montserrat_14, 0);

    _ts_label = lv_label_create(bottom_bar);
    lv_obj_set_style_text_font(_ts_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_ts_label, CLR_TEXT_MUTED, 0);
    lv_label_set_text(_ts_label, "");

    /* 5 dots after the timestamp */
    lv_obj_t *dot_row = lv_obj_create(bottom_bar);
    lv_obj_set_height(dot_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(dot_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dot_row, 0, 0);
    lv_obj_set_style_pad_all(dot_row, 0, 0);
    lv_obj_set_flex_flow(dot_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dot_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(dot_row, 4, 0);
    lv_obj_set_style_pad_left(dot_row, 4, 0);

    for (int i = 0; i < MAX_DOTS; i++) {
        _dots[i] = lv_obj_create(dot_row);
        lv_obj_set_size(_dots[i], 10, 10);
        lv_obj_set_style_radius(_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(_dots[i], 0, 0);
        lv_obj_set_style_bg_color(_dots[i], lv_color_make(220, 220, 220), 0);
        lv_obj_set_style_bg_opa(_dots[i], LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(_dots[i], 0, 0);
    }
}

/* ------------------------------------------------------------------ */

/* ── dot colours ── */
#define CLR_DOT_IDLE   lv_color_make(220, 220, 220) /* grey */
#define CLR_DOT_BUSY   lv_color_make(76, 175, 80)    /* green (pending) */
#define CLR_DOT_OK     lv_color_make(33, 150, 243)    /* blue (succeeded) */
#define CLR_DOT_FAIL   lv_color_make(244, 67, 54)     /* red */

void ServerMonitor::updateUi(void)
{
    /* ── status dots: cumulative 1→2→3→4→5 then reset ── */
    for (int i = 0; i < MAX_DOTS; i++) {
        lv_obj_set_style_bg_color(_dots[i], (i <= _dot_idx) ? CLR_DOT_BUSY : CLR_DOT_IDLE, 0);
    }
    if (_last_url_ts) {
        lv_label_set_text_fmt(_ts_label, "t:%llu", (unsigned long long)_last_url_ts);
    }

    /* ── subtitle: local time ticks every second ── */
    {
        time_t now = time(NULL);
        struct tm t;
        localtime_r(&now, &t);
        lv_label_set_text_fmt(_subtitle, "%04d-%02d-%02d  %02d:%02d:%02d CST",
                              t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                              t.tm_hour, t.tm_min, t.tm_sec);
    }

    monitor_data_t d;
    if (_mutex) {
        xSemaphoreTake(_mutex, portMAX_DELAY);
    }
    d = _data;
    if (_mutex) {
        xSemaphoreGive(_mutex);
    }
    if (!d.valid) {
        return;
    }

    char a[48], b[48], pct[48];

    /* ---- CPU ---- */
    fmt_pct(d.cpu_usage, pct, sizeof(pct));
    lv_label_set_text(_cpu_val, pct);
    {
        int cp = (int)(d.cpu_usage + 0.5f);
        if (cp < 0) cp = 0;
        if (cp > 100) cp = 100;
        lv_bar_set_value(_cpu_bar, cp, LV_ANIM_ON);
    }
    snprintf(a, sizeof(a), "%s", d.cpu_model[0] ? d.cpu_model : "");
    {
        int ld_int = (int)d.load1;
        int ld_dec = (int)((d.load1 - ld_int) * 10 + 0.5f);
        if (ld_dec < 0) ld_dec = 0;
        if (ld_dec > 9) { ld_dec = 0; ld_int++; }
        char load_buf[8];
        snprintf(load_buf, sizeof(load_buf), "%d.%d", ld_int, ld_dec);
        lv_label_set_text_fmt(_cpu_sub, "%s%s%d core%s    load %s",
                              a, a[0] ? "  " : "",
                              d.cpu_cores, d.cpu_cores == 1 ? "" : "s", load_buf);
    }

    /* ---- Memory ---- */
    fmt_pct(d.mem_usage, pct, sizeof(pct));
    lv_label_set_text(_mem_val, pct);
    lv_bar_set_value(_mem_bar, (int)(d.mem_usage + 0.5f), LV_ANIM_ON);
    fmt_bytes(d.mem_used, a, sizeof(a));
    fmt_bytes(d.mem_total, b, sizeof(b));
    lv_label_set_text_fmt(_mem_sub, "Used %s / %s", a, b);

    /* ---- Disk ---- */
    fmt_pct(d.disk_usage, pct, sizeof(pct));
    lv_label_set_text(_disk_val, pct);
    lv_bar_set_value(_disk_bar, (int)(d.disk_usage + 0.5f), LV_ANIM_ON);
    fmt_bytes(d.disk_used, a, sizeof(a));
    fmt_bytes(d.disk_total, b, sizeof(b));
    lv_label_set_text_fmt(_disk_sub, "Used %s / %s", a, b);

    /* ---- Network ---- */
    fmt_bytes(d.net_rx_speed, a, sizeof(a));
    fmt_bytes(d.net_tx_speed, b, sizeof(b));
    snprintf(pct, sizeof(pct), "RX %s/s  TX %s/s", a, b);
    lv_obj_set_style_text_font(_net_val, &lv_font_montserrat_18, 0);
    lv_label_set_text(_net_val, pct);
    fmt_bytes(d.net_rx_bytes, a, sizeof(a));
    fmt_bytes(d.net_tx_bytes, b, sizeof(b));
    lv_label_set_text_fmt(_net_sub, "Total  RX %s  TX %s\n%s",
                          a, b, d.iface[0] ? d.iface : "-");
}

/* ------------------------------------------------------------------ */
/*  refresh / HTTP / JSON  -  unchanged logic                          */
/* ------------------------------------------------------------------ */

void ServerMonitor::startRefresh(void)
{
    _running = true;
    _dot_idx = 0;
    if (_mutex == NULL) {
        _mutex = xSemaphoreCreateMutex();
    }
    /* UI update timer (1 Hz) – advances dot, updates UI */
    if (_timer == NULL) {
        _timer = lv_timer_create(refresh_timer_cb, 1000, this);
    }
    /* Create HTTP task only if old one has fully exited */
    if (_http_task_handle) {
        /* Brief wait for graceful self-exit (1s max) */
        int timeout = 20;
        while (_http_task_handle != NULL && timeout-- > 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (_http_task_handle != NULL) {
            ESP_LOGW(TAG, "Old HTTP task still alive, recreating");
            vTaskDelete(_http_task_handle);
            _http_task_handle = NULL;
        }
    }
    if (_http_task_handle == NULL) {
        xTaskCreate(http_task, "srvmon", 8192, this, 5, &_http_task_handle);
    }
}

void ServerMonitor::stopRefresh(void)
{
    _running = false;
    if (_timer) {
        lv_timer_del(_timer);
        _timer = NULL;
    }
    /* Do NOT block here — pause() runs in LVGL task context.
     * Just signal _running=false and return immediately.
     * The HTTP task will clean up on its next iteration. */
}

void ServerMonitor::refresh_timer_cb(lv_timer_t *t)
{
    ServerMonitor *app = (ServerMonitor *)t->user_data;
    if (!app) return;
    /* advance dot (0→1→2→3→4→0…) */
    app->_dot_idx = (app->_dot_idx + 1) % MAX_DOTS;
    app->updateUi();
}

/* ── single HTTP task: fetch → wait 3s on success → repeat ── */
void ServerMonitor::http_task(void *arg)
{
    ServerMonitor *app = (ServerMonitor *)arg;
    while (app->_running) {
        bool ok = app->fetchAndParse();
        int delay = ok ? 3000 : 500;  /* 3s on success, fast retry on fail */
        int waited = 0;
        while (waited < delay && app->_running) {
            vTaskDelay(pdMS_TO_TICKS(100));
            waited += 100;
        }
    }
    app->_http_task_handle = NULL;
    vTaskDelete(NULL);
}

namespace {
esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_acc_t *acc = (http_acc_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int need = acc->len + evt->data_len + 1;
        if (need > acc->cap) {
            int ncap = acc->cap ? acc->cap * 2 : 1024;
            while (ncap < need) ncap *= 2;
            char *nb = (char *)realloc(acc->buf, ncap);
            if (!nb) return ESP_FAIL;
            acc->buf = nb;
            acc->cap = ncap;
        }
        memcpy(acc->buf + acc->len, evt->data, evt->data_len);
        acc->len += evt->data_len;
        acc->buf[acc->len] = 0;
    }
    return ESP_OK;
}
} // namespace

bool ServerMonitor::fetchAndParse(void)
{
    char url[192];
    struct timeval tv;
    gettimeofday(&tv, NULL);
    _last_url_ts = (uint64_t)tv.tv_sec * 1000 + (uint64_t)(tv.tv_usec / 1000);
    snprintf(url, sizeof(url), "%s%llu", API_URL_BASE,
             (unsigned long long)_last_url_ts);

    http_acc_t acc = {0};
    esp_http_client_config_t config = {
        .url = url,
        .cert_pem = SERVER_CERT_PEM,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .event_handler = http_event_handler,
        .user_data = &acc,
        .skip_cert_common_name_check = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "HTTP init fail");
        return false;
    }

    esp_err_t err = esp_http_client_perform(client);
    bool ok = (err == ESP_OK && acc.buf);
    if (ok) {
        parseJson(acc.buf);
        ESP_LOGI(TAG, "OK");
    } else {
        ESP_LOGE(TAG, "%s", esp_err_to_name(err));
    }

    if (acc.buf) free(acc.buf);
    esp_http_client_cleanup(client);
    return ok;
}

void ServerMonitor::parseJson(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed");
        return;
    }

    monitor_data_t d;
    memset(&d, 0, sizeof(d));
    d.valid = true;

    auto getNum = [](cJSON *obj, const char *key, double *out) -> bool {
        cJSON *it = cJSON_GetObjectItem(obj, key);
        if (it && cJSON_IsNumber(it)) { *out = it->valuedouble; return true; }
        return false;
    };
    auto getStr = [](cJSON *obj, const char *key, char *dst, int n) -> bool {
        cJSON *it = cJSON_GetObjectItem(obj, key);
        if (it && it->valuestring) { strncpy(dst, it->valuestring, n - 1); dst[n - 1] = 0; return true; }
        return false;
    };

    cJSON *cpu = cJSON_GetObjectItem(root, "cpu");
    if (cpu) {
        double v;
        getStr(cpu, "model", d.cpu_model, sizeof(d.cpu_model));
        if (getNum(cpu, "cores", &v)) d.cpu_cores = (int)v;
        if (getNum(cpu, "usage_percent", &v)) d.cpu_usage = (float)v;
        if (getNum(cpu, "load_1", &v)) d.load1 = (float)v;
        if (getNum(cpu, "load_5", &v)) d.load5 = (float)v;
        if (getNum(cpu, "load_15", &v)) d.load15 = (float)v;
    }

    cJSON *mem = cJSON_GetObjectItem(root, "memory");
    if (mem) {
        double v;
        if (getNum(mem, "usage_pct", &v)) d.mem_usage = (float)v;
        if (getNum(mem, "total", &v)) d.mem_total = (uint64_t)v;
        if (getNum(mem, "used", &v)) d.mem_used = (uint64_t)v;
        if (getNum(mem, "available", &v)) d.mem_available = (uint64_t)v;
        if (getNum(mem, "swap_total", &v)) d.swap_total = (uint64_t)v;
        if (getNum(mem, "swap_used", &v)) d.swap_used = (uint64_t)v;
    }

    cJSON *disk = cJSON_GetObjectItem(root, "disk");
    if (disk) {
        double v;
        if (getNum(disk, "usage_pct", &v)) d.disk_usage = (float)v;
        if (getNum(disk, "total", &v)) d.disk_total = (uint64_t)v;
        if (getNum(disk, "used", &v)) d.disk_used = (uint64_t)v;
        if (getNum(disk, "free", &v)) d.disk_free = (uint64_t)v;
    }

    cJSON *net = cJSON_GetObjectItem(root, "network");
    if (net) {
        double v;
        if (getNum(net, "rx_bytes", &v)) d.net_rx_bytes = (uint64_t)v;
        if (getNum(net, "tx_bytes", &v)) d.net_tx_bytes = (uint64_t)v;
        if (getNum(net, "rx_speed", &v)) d.net_rx_speed = (uint32_t)v;
        if (getNum(net, "tx_speed", &v)) d.net_tx_speed = (uint32_t)v;
        cJSON *ifs = cJSON_GetObjectItem(net, "interfaces");
        if (cJSON_IsArray(ifs) && cJSON_GetArraySize(ifs) > 0) {
            cJSON *first = cJSON_GetArrayItem(ifs, 0);
            getStr(first, "name", d.iface, sizeof(d.iface));
        }
    }

    cJSON *t = cJSON_GetObjectItem(root, "time");
    if (t && cJSON_IsNumber(t)) {
        d.time = (uint32_t)t->valuedouble;
    }

    cJSON_Delete(root);

    if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
    _data = d;
    if (_mutex) xSemaphoreGive(_mutex);
}
