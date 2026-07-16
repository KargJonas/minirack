/*
 * minirack OTA loader
 *
 * Lives in the "factory" partition and is only ever flashed over serial.
 * Brings up the network (WiFi on the LOLIN32 dev setup, ethernet on the
 * WT32-ETH01) and serves a tiny HTTP API to manage the app in ota_0/ota_1:
 *
 *   GET  /                    info: partitions, running image, usage
 *   POST /update              raw app image body -> next OTA slot, then boot it
 *                             curl --data-binary @firmware.bin http://<ip>/update
 *   POST /boot?part=<label>   set boot partition (factory | ota_0 | ota_1) + reboot
 *   POST /reboot              just reboot
 *
 * Uploaded apps are regular Arduino (or IDF) images built against the same
 * partition table. With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE the image is
 * marked NEW; if it crashes before marking itself valid (RackOTA.begin()
 * does that), the next reset rolls back to this loader.
 */

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "esp_http_server.h"

#if defined(LOADER_USE_WIFI)
#include "esp_wifi.h"
#elif defined(LOADER_USE_ETH)
#include "esp_eth.h"
#include "driver/gpio.h"
/* WT32-ETH01: LAN8720, PHY addr 1, MDC=GPIO23, MDIO=GPIO18,
 * 50MHz oscillator feeds GPIO0, oscillator enable on GPIO16 */
#define WT32_PHY_ADDR      1
#define WT32_GPIO_MDC      23
#define WT32_GPIO_MDIO     18
#define WT32_GPIO_OSC_EN   16
#else
#error "define LOADER_USE_WIFI or LOADER_USE_ETH"
#endif

static const char *TAG = "loader";
#define HOSTNAME "minirack-loader"

/* ------------------------------------------------------------------ */
/* Network bringup                                                    */
/* ------------------------------------------------------------------ */

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "got IP: " IPSTR "  ->  curl http://" IPSTR "/",
             IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.ip));
}

#if defined(LOADER_USE_WIFI)

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "wifi disconnected, retrying...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    }
}

static void net_start(void)
{
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, HOSTNAME);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_got_ip, NULL));

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, LOADER_WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, LOADER_WIFI_PASS, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "wifi connecting to '%s'", LOADER_WIFI_SSID);
}

#elif defined(LOADER_USE_ETH)

static void net_start(void)
{
    /* enable the external 50MHz oscillator before touching the PHY */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << WT32_GPIO_OSC_EN,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(WT32_GPIO_OSC_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_config.clock_config.rmii.clock_gpio = EMAC_CLK_IN_GPIO; /* GPIO0 */
    emac_config.smi_mdc_gpio_num = WT32_GPIO_MDC;
    emac_config.smi_mdio_gpio_num = WT32_GPIO_MDIO;

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = WT32_PHY_ADDR;
    phy_config.reset_gpio_num = -1;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    esp_netif_set_hostname(netif, HOSTNAME);
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth_handle)));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &on_got_ip, NULL));
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
    ESP_LOGI(TAG, "ethernet started, waiting for link/DHCP");
}

#endif

/* ------------------------------------------------------------------ */
/* HTTP API                                                            */
/* ------------------------------------------------------------------ */

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(750)); /* let the HTTP response drain */
    esp_restart();
}

static void schedule_reboot(void)
{
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
}

static const char *ota_state_str(const esp_partition_t *part)
{
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(part, &st) != ESP_OK) return "?";
    switch (st) {
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending-verify";
    case ESP_OTA_IMG_VALID:          return "valid";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    default:                         return "undefined";
    }
}

static esp_err_t info_get_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    char buf[1024];
    int n = snprintf(buf, sizeof(buf),
        "minirack loader %s (%s %s)\n"
        "running: %-8s  boot: %-8s  next update slot: %s\n"
        "\n"
        "app partitions:\n",
        app->version, app->date, app->time,
        running ? running->label : "?", boot ? boot->label : "?",
        next ? next->label : "?");

    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP,
                                                     ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it != NULL; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        n += snprintf(buf + n, sizeof(buf) - n,
                      "  %-8s  0x%06" PRIx32 "  %4" PRIu32 "K  %s\n",
                      p->label, p->address, p->size / 1024,
                      p->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY ? "loader" : ota_state_str(p));
    }
    esp_partition_iterator_release(it);

    n += snprintf(buf + n, sizeof(buf) - n,
        "\nusage:\n"
        "  curl --data-binary @firmware.bin http://%s/update\n"
        "  curl -X POST http://%s/boot?part=factory\n"
        "  curl -X POST http://%s/reboot\n",
        HOSTNAME, HOSTNAME, HOSTNAME);

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t update_post_handler(httpd_req_t *req)
{
    const esp_partition_t *dst = esp_ota_get_next_update_partition(NULL);
    if (dst == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition found");
        return ESP_FAIL;
    }
    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "empty body; use: curl --data-binary @firmware.bin http://.../update");
        return ESP_FAIL;
    }
    if (req->content_len > dst->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image larger than OTA partition");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "receiving %d bytes -> %s", req->content_len, dst->label);

    esp_ota_handle_t ota;
    esp_err_t err = esp_ota_begin(dst, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    char *buf = malloc(8192);
    if (!buf) {
        esp_ota_abort(ota);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining < 8192 ? remaining : 8192);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            free(buf);
            esp_ota_abort(ota);
            ESP_LOGE(TAG, "recv failed with %d bytes left", remaining);
            return ESP_FAIL; /* socket is gone, no response possible */
        }
        err = esp_ota_write(ota, buf, r);
        if (err != ESP_OK) {
            free(buf);
            esp_ota_abort(ota);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
            return ESP_FAIL;
        }
        remaining -= r;
    }
    free(buf);

    err = esp_ota_end(ota); /* validates magic bytes, checksum, sha256 */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            err == ESP_ERR_OTA_VALIDATE_FAILED
                                ? "not a valid app image" : esp_err_to_name(err));
        return ESP_FAIL;
    }
    err = esp_ota_set_boot_partition(dst);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    char resp[128];
    int n = snprintf(resp, sizeof(resp), "OK: %d bytes written to %s, rebooting into it\n",
                     req->content_len, dst->label);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, resp, n);
    ESP_LOGI(TAG, "update ok, rebooting into %s", dst->label);
    schedule_reboot();
    return ESP_OK;
}

static esp_err_t boot_post_handler(httpd_req_t *req)
{
    char query[64] = "", part[32] = "";
    httpd_req_get_url_query_str(req, query, sizeof(query));
    if (httpd_query_key_value(query, "part", part, sizeof(part)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "use /boot?part=factory|ota_0|ota_1");
        return ESP_FAIL;
    }
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                        ESP_PARTITION_SUBTYPE_ANY, part);
    if (p == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no such app partition");
        return ESP_FAIL;
    }
    esp_err_t err = esp_ota_set_boot_partition(p);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK, rebooting\n");
    schedule_reboot();
    return ESP_OK;
}

static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK, rebooting\n");
    schedule_reboot();
    return ESP_OK;
}

static void http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));

    const httpd_uri_t routes[] = {
        { .uri = "/",       .method = HTTP_GET,  .handler = info_get_handler },
        { .uri = "/update", .method = HTTP_POST, .handler = update_post_handler },
        { .uri = "/boot",   .method = HTTP_POST, .handler = boot_post_handler },
        { .uri = "/reboot", .method = HTTP_POST, .handler = reboot_post_handler },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "minirack loader running from %s", running ? running->label : "?");

    net_start();
    http_start();
}
