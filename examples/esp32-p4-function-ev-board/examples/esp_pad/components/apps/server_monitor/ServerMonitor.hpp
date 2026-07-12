#pragma once

#include "lvgl.h"
#include "esp_brookesia.hpp"
#include <cstdint>

#define MAX_DOTS  5

typedef struct {
    bool      valid;
    char      cpu_model[64];
    int       cpu_cores;
    float     cpu_usage;
    float     load1;
    float     load5;
    float     load15;
    float     mem_usage;
    uint64_t  mem_total;
    uint64_t  mem_used;
    uint64_t  mem_available;
    uint64_t  swap_total;
    uint64_t  swap_used;
    float     disk_usage;
    uint64_t  disk_total;
    uint64_t  disk_used;
    uint64_t  disk_free;
    uint64_t  net_rx_bytes;
    uint64_t  net_tx_bytes;
    uint32_t  net_rx_speed;
    uint32_t  net_tx_speed;
    char      iface[16];
    uint32_t  time;
} monitor_data_t;

class ServerMonitor : public ESP_Brookesia_PhoneApp {
public:
    ServerMonitor();
    ~ServerMonitor();

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;
    bool pause(void) override;
    bool resume(void) override;

private:
    void createUi(void);
    void updateUi(void);
    void startRefresh(void);
    void stopRefresh(void);
    bool fetchAndParse(void);
    void parseJson(const char *json);

    static void refresh_timer_cb(lv_timer_t *t);
    static void http_task(void *arg);

    /* UI */
    lv_obj_t *_root;
    lv_obj_t *_subtitle;
    lv_obj_t *_cpu_val, *_cpu_sub, *_cpu_bar;
    lv_obj_t *_mem_val, *_mem_sub, *_mem_bar;
    lv_obj_t *_disk_val, *_disk_sub, *_disk_bar;
    lv_obj_t *_net_val, *_net_sub;
    lv_obj_t *_dots[MAX_DOTS];
    lv_obj_t *_ts_label;

    lv_timer_t     *_timer;
    TaskHandle_t    _http_task_handle;
    int             _dot_idx;
    volatile bool   _running;
    volatile bool   _paused;
    SemaphoreHandle_t _mutex;
    monitor_data_t   _data;
    uint64_t         _last_url_ts;
};
