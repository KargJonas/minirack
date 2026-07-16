#include "RackOTA.h"
#include <WebServer.h>
#include <Update.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"

RackOTAClass RackOTA;

void RackOTAClass::begin(const char *appInfo, uint16_t port)
{
    /* We survived until here: keep this image across reboots. */
    esp_ota_mark_app_valid_cancel_rollback();

    _info = appInfo;
    _server = new WebServer(port);
    _server->on("/", HTTP_GET, [this]() { handleInfo(); });
    _server->on("/update", HTTP_POST,
                [this]() { handleUpdateDone(); },
                [this]() { handleUpdateData(); });
    _server->on("/loader", HTTP_POST, [this]() { handleLoader(); });
    _server->on("/reboot", HTTP_POST, [this]() { handleReboot(); });
    _server->begin();
}

void RackOTAClass::handle()
{
    if (_server) _server->handleClient();
    if (_rebootPending && (int32_t)(millis() - _rebootAt) >= 0) {
        if (_onReboot) _onReboot();
        ESP.restart();
    }
}

void RackOTAClass::scheduleReboot()
{
    _rebootPending = true;
    _rebootAt = millis() + 750; /* let the response drain first */
}

void RackOTAClass::handleInfo()
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    String s = "minirack app: " + _info + "\n";
    s += "running from: " + String(running ? running->label : "?") + "\n";
    s += "uptime: " + String(millis() / 1000) + "s\n";
    s += "\nusage:\n";
    s += "  curl --data-binary @firmware.bin http://<ip>/update\n";
    s += "  curl -X POST http://<ip>/loader   (reboot into factory loader)\n";
    s += "  curl -X POST http://<ip>/reboot\n";
    _server->send(200, "text/plain", s);
}

/* Streaming callback: receives the raw POST body chunk by chunk. */
void RackOTAClass::handleUpdateData()
{
    HTTPRaw &raw = _server->raw();
    switch (raw.status) {
    case RAW_START:
        Serial.println("[RackOTA] update started");
        Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH);
        break;
    case RAW_WRITE:
        Update.write(raw.buf, raw.currentSize);
        break;
    case RAW_END:
        Update.end(true);
        Serial.printf("[RackOTA] update finished, %u bytes\n", (unsigned)raw.totalSize);
        break;
    case RAW_ABORTED:
        Update.abort();
        Serial.println("[RackOTA] update aborted");
        break;
    }
}

void RackOTAClass::handleUpdateDone()
{
    if (Update.hasError()) {
        _server->send(400, "text/plain",
                      String("update failed: ") + Update.errorString() + "\n");
        Update.clearError();
        return;
    }
    _server->send(200, "text/plain", "OK: update written, rebooting into it\n");
    scheduleReboot();
}

void RackOTAClass::handleLoader()
{
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (!factory) {
        _server->send(500, "text/plain", "no factory partition found\n");
        return;
    }
    esp_err_t err = esp_ota_set_boot_partition(factory);
    if (err != ESP_OK) {
        _server->send(500, "text/plain", String(esp_err_to_name(err)) + "\n");
        return;
    }
    _server->send(200, "text/plain", "OK, rebooting into loader\n");
    scheduleReboot();
}

void RackOTAClass::handleReboot()
{
    _server->send(200, "text/plain", "OK, rebooting\n");
    scheduleReboot();
}
