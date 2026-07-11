#pragma once

#include "lvgl.h"
#include "esp_brookesia.hpp"
#include <cstdint>

typedef struct {
    bool      valid;
    /* cpu */
    char      cpu_model[64];
    int       cpu_cores;
    float     cpu_usage;
    float     load1;
    float     load5;
    float     load15;
    /* memory */
    float     mem_usage;
    uint64_t  mem_total;
    uint64_t  mem_used;
    uint64_t  mem_available;
    uint64_t  swap_total;
    uint64_t  swap_used;
    /* disk */
    float     disk_usage;
    uint64_t  disk_total;
    uint64_t  disk_used;
    uint64_t  disk_free;
    /* network */
    uint64_t  net_rx_bytes;
    uint64_t  net_tx_bytes;
    uint32_t  net_rx_speed;
    uint32_t  net_tx_speed;
    char      iface[16];
    /* time */
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

private:
    void createUi(void);
    void updateUi(void);
    void startRefresh(void);
    void stopRefresh(void);
    void fetchAndParse(void);
    void parseJson(const char *json);

    static void refresh_timer_cb(lv_timer_t *t);
    static void http_task(void *arg);

    /* UI references */
    lv_obj_t *_root;
    lv_obj_t *_subtitle;
    lv_obj_t *_cpu_val;
    lv_obj_t *_cpu_sub;
    lv_obj_t *_cpu_bar;
    lv_obj_t *_mem_val;
    lv_obj_t *_mem_sub;
    lv_obj_t *_mem_bar;
    lv_obj_t *_disk_val;
    lv_obj_t *_disk_sub;
    lv_obj_t *_disk_bar;
    lv_obj_t *_net_val;
    lv_obj_t *_net_sub;

    lv_timer_t     *_timer;
    TaskHandle_t    _task;
    volatile bool   _task_running;
    volatile bool   _running;
    SemaphoreHandle_t _mutex;
    monitor_data_t   _data;
};
