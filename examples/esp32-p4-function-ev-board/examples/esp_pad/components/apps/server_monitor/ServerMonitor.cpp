/*
 * Server Monitor app - Apple-style card UI, refreshes every 5 seconds
 * from https://tz.cubegao.com/?action=api
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

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

namespace {
lv_obj_t *makeCard(lv_obj_t *parent, const char *title,
                   lv_obj_t **big, lv_obj_t **sub, lv_obj_t **bar)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, 14, 0);
    lv_obj_set_style_radius(card, 22, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_white(), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_10, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_40, 0);
    lv_obj_set_style_shadow_width(card, 24, 0);
    lv_obj_set_style_shadow_ofs_y(card, 8, 0);
    lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, 8, 0);

    lv_obj_t *tl = lv_label_create(card);
    lv_label_set_text(tl, title);
    lv_obj_set_style_text_font(tl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(tl, lv_color_make(198, 206, 230), 0);

    *big = lv_label_create(card);
    lv_label_set_text(*big, "--");
    lv_obj_set_style_text_font(*big, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(*big, lv_color_white(), 0);
    lv_obj_set_width(*big, lv_pct(100));

    if (bar) {
        *bar = lv_bar_create(card);
        lv_obj_set_width(*bar, lv_pct(100));
        lv_obj_set_height(*bar, 10);
        lv_bar_set_range(*bar, 0, 100);
        lv_bar_set_value(*bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_radius(*bar, 5, LV_PART_MAIN);
        lv_obj_set_style_bg_color(*bar, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(*bar, 18, LV_PART_MAIN);
        lv_obj_set_style_radius(*bar, 5, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(*bar, lv_color_make(48, 209, 88), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(*bar, LV_OPA_COVER, LV_PART_INDICATOR);
    }

    if (sub) {
        *sub = lv_label_create(card);
        lv_label_set_text(*sub, "");
        lv_obj_set_style_text_font(*sub, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(*sub, lv_color_make(176, 184, 208), 0);
        lv_obj_set_width(*sub, lv_pct(100));
    }

    return card;
}
} // namespace

ServerMonitor::ServerMonitor():
    ESP_Brookesia_PhoneApp("Server Monitor", &img_app_server_monitor, true),
    _root(NULL), _subtitle(NULL),
    _cpu_val(NULL), _cpu_sub(NULL), _cpu_bar(NULL),
    _mem_val(NULL), _mem_sub(NULL), _mem_bar(NULL),
    _disk_val(NULL), _disk_sub(NULL), _disk_bar(NULL),
    _net_val(NULL), _net_sub(NULL),
    _timer(NULL), _task(NULL), _task_running(false), _running(false), _mutex(NULL)
{
    memset(&_data, 0, sizeof(_data));
}

ServerMonitor::~ServerMonitor()
{
    stopRefresh();
    /* Wait (bounded) for the HTTP task to finish before freeing shared state,
       otherwise it may touch the deleted mutex / this object. */
    int guard = 0;
    while (_task_running && guard < 200) {
        vTaskDelay(pdMS_TO_TICKS(50));
        guard++;
    }
    if (_mutex) {
        vSemaphoreDelete(_mutex);
        _mutex = NULL;
    }
}

bool ServerMonitor::init(void)
{
    return true;
}

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

void ServerMonitor::createUi(void)
{
    lv_area_t area = getVisualArea();
    int W = area.x2 - area.x1;
    int H = area.y2 - area.y1;

    _root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_root, W, H);
    lv_obj_align(_root, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(_root, lv_color_make(18, 22, 36), 0);
    lv_obj_set_style_bg_grad_color(_root, lv_color_make(26, 30, 52), 0);
    lv_obj_set_style_bg_grad_dir(_root, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(_root, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(_root, 0, 0);
    lv_obj_set_style_border_width(_root, 0, 0);
    lv_obj_set_style_pad_all(_root, 0, 0);
    lv_obj_set_flex_flow(_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(_root, 16, 0);
    lv_obj_set_style_pad_ver(_root, 18, 0);
    lv_obj_set_style_pad_hor(_root, 18, 0);
    lv_obj_add_flag(_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(_root, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(_root, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *title = lv_label_create(_root);
    lv_label_set_text(title, "Server Monitor");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_34, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_width(title, lv_pct(100));

    _subtitle = lv_label_create(_root);
    lv_label_set_text(_subtitle, "Loading…");
    lv_obj_set_style_text_font(_subtitle, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_subtitle, lv_color_make(170, 178, 200), 0);
    lv_obj_set_width(_subtitle, lv_pct(100));

    makeCard(_root, "CPU", &_cpu_val, &_cpu_sub, &_cpu_bar);
    makeCard(_root, "Memory", &_mem_val, &_mem_sub, &_mem_bar);
    makeCard(_root, "Disk", &_disk_val, &_disk_sub, &_disk_bar);
    makeCard(_root, "Network", &_net_val, &_net_sub, NULL);
}

void ServerMonitor::updateUi(void)
{
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

    char a[24], b[24];

    lv_label_set_text_fmt(_cpu_val, "%.1f%%", d.cpu_usage);
    int cp = (int)(d.cpu_usage + 0.5f);
    if (cp < 0) cp = 0;
    if (cp > 100) cp = 100;
    lv_bar_set_value(_cpu_bar, cp, LV_ANIM_ON);
    snprintf(a, sizeof(a), "%s", d.cpu_model[0] ? d.cpu_model : "Unknown CPU");
    lv_label_set_text_fmt(_cpu_sub, "%s  ·  %d cores", a, d.cpu_cores);

    lv_label_set_text_fmt(_mem_val, "%.1f%%", d.mem_usage);
    lv_bar_set_value(_mem_bar, (int)(d.mem_usage + 0.5f), LV_ANIM_ON);
    fmt_bytes(d.mem_used, a, sizeof(a));
    fmt_bytes(d.mem_total, b, sizeof(b));
    lv_label_set_text_fmt(_mem_sub, "Used %s / %s", a, b);

    lv_label_set_text_fmt(_disk_val, "%.1f%%", d.disk_usage);
    lv_bar_set_value(_disk_bar, (int)(d.disk_usage + 0.5f), LV_ANIM_ON);
    fmt_bytes(d.disk_used, a, sizeof(a));
    fmt_bytes(d.disk_total, b, sizeof(b));
    lv_label_set_text_fmt(_disk_sub, "Used %s / %s", a, b);

    fmt_bytes(d.net_rx_speed, a, sizeof(a));
    fmt_bytes(d.net_tx_speed, b, sizeof(b));
    lv_label_set_text_fmt(_net_val, "RX %s/s    TX %s/s", a, b);
    fmt_bytes(d.net_rx_bytes, a, sizeof(a));
    fmt_bytes(d.net_tx_bytes, b, sizeof(b));
    lv_label_set_text_fmt(_net_sub, "Total  RX %s    TX %s\n%s", a, b,
                           d.iface[0] ? d.iface : "-");

    time_t ts = (time_t)d.time;
    struct tm tminfo;
    gmtime_r(&ts, &tminfo);
    lv_label_set_text_fmt(_subtitle, "Server %02d:%02d:%02d UTC  ·  auto 5s",
                           tminfo.tm_hour, tminfo.tm_min, tminfo.tm_sec);
}

void ServerMonitor::startRefresh(void)
{
    _running = true;
    if (_mutex == NULL) {
        _mutex = xSemaphoreCreateMutex();
    }
    /* Always re-create the timer – stopRefresh deletes it on nav away,
       but the HTTP task may still be alive from a previous session. */
    if (_timer == NULL) {
        _timer = lv_timer_create(refresh_timer_cb, 1000, this);
    }
    if (!_task_running) {
        _task_running = true;
        xTaskCreate(http_task, "srvmon", 10240, this, 5, &_task);
    }
}

void ServerMonitor::stopRefresh(void)
{
    _running = false;
    if (_timer) {
        lv_timer_del(_timer);
        _timer = NULL;
    }
}

void ServerMonitor::refresh_timer_cb(lv_timer_t *t)
{
    ServerMonitor *app = (ServerMonitor *)t->user_data;
    if (app) {
        app->updateUi();
    }
}

void ServerMonitor::http_task(void *arg)
{
    ServerMonitor *app = (ServerMonitor *)arg;
    app->fetchAndParse();
    while (app->_running) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (!app->_running) {
            break;
        }
        app->fetchAndParse();
    }
    app->_task_running = false;
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
            while (ncap < need) {
                ncap *= 2;
            }
            char *nb = (char *)realloc(acc->buf, ncap);
            if (!nb) {
                return ESP_FAIL;
            }
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

void ServerMonitor::fetchAndParse(void)
{
    char url[160];
    snprintf(url, sizeof(url), "%s%lu", API_URL_BASE, (unsigned long)time(NULL));

    http_acc_t acc = {0};
    esp_http_client_config_t config = {
        .url = url,
        .cert_pem = SERVER_CERT_PEM,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .event_handler = http_event_handler,
        .user_data = &acc,
        .skip_cert_common_name_check = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client init failed");
        return;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK && acc.buf) {
        parseJson(acc.buf);
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }

    if (acc.buf) {
        free(acc.buf);
    }
    esp_http_client_cleanup(client);
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
        if (it && cJSON_IsNumber(it)) {
            *out = it->valuedouble;
            return true;
        }
        return false;
    };
    auto getStr = [](cJSON *obj, const char *key, char *dst, int n) -> bool {
        cJSON *it = cJSON_GetObjectItem(obj, key);
        if (it && it->valuestring) {
            strncpy(dst, it->valuestring, n - 1);
            dst[n - 1] = 0;
            return true;
        }
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

    if (_mutex) {
        xSemaphoreTake(_mutex, portMAX_DELAY);
    }
    _data = d;
    if (_mutex) {
        xSemaphoreGive(_mutex);
    }
}
