#include "ota_update.h"

#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

static const char *TAG = "ota_update";

static esp_ota_handle_t       s_handle = 0;
static const esp_partition_t *s_partition = NULL;
static size_t                 s_bytes_written = 0;
static ota_state_t            s_state = OTA_STATE_IDLE;
static char                   s_last_error[64] = {0};

static void set_error(const char *msg)
{
    if (!msg) msg = "";
    strncpy(s_last_error, msg, sizeof s_last_error - 1);
    s_last_error[sizeof s_last_error - 1] = '\0';
    s_state = OTA_STATE_FAILED;
    ESP_LOGE(TAG, "%s", msg);
}

esp_err_t ota_update_init(void)
{
    /* If this boot is the new OTA image, cancel the pending rollback by
     * marking the app valid. ESP_ERR_NOT_SUPPORTED / ESP_ERR_NOT_FOUND
     * here just means there was no rollback to cancel (we're already
     * on a valid slot or rollback isn't enabled in this build). */
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "mark_app_valid err=0x%x", err);
    }
    s_handle = 0;
    s_partition = NULL;
    s_bytes_written = 0;
    s_state = OTA_STATE_IDLE;
    s_last_error[0] = '\0';

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        ESP_LOGI(TAG, "running from %s @ 0x%lx",
                 running->label, (unsigned long)running->address);
    }
    return ESP_OK;
}

esp_err_t ota_update_begin(void)
{
    if (s_state == OTA_STATE_WRITING || s_state == OTA_STATE_FINALIZING) {
        return ESP_ERR_INVALID_STATE;
    }
    s_partition = esp_ota_get_next_update_partition(NULL);
    if (!s_partition) {
        set_error("no_ota_partition");
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = esp_ota_begin(s_partition, OTA_WITH_SEQUENTIAL_WRITES, &s_handle);
    if (err != ESP_OK) {
        set_error("ota_begin_failed");
        return err;
    }
    s_bytes_written = 0;
    s_state = OTA_STATE_WRITING;
    s_last_error[0] = '\0';
    ESP_LOGI(TAG, "ota begin: partition=%s size=0x%lx",
             s_partition->label, (unsigned long)s_partition->size);
    return ESP_OK;
}

esp_err_t ota_update_write(const void *data, size_t len)
{
    if (s_state != OTA_STATE_WRITING) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_ota_write(s_handle, data, len);
    if (err != ESP_OK) {
        set_error("ota_write_failed");
        return err;
    }
    s_bytes_written += len;
    return ESP_OK;
}

esp_err_t ota_update_end(void)
{
    if (s_state != OTA_STATE_WRITING) return ESP_ERR_INVALID_STATE;
    s_state = OTA_STATE_FINALIZING;
    esp_err_t err = esp_ota_end(s_handle);
    s_handle = 0;
    if (err != ESP_OK) {
        set_error("ota_end_failed");
        return err;
    }
    err = esp_ota_set_boot_partition(s_partition);
    if (err != ESP_OK) {
        set_error("set_boot_failed");
        return err;
    }
    s_state = OTA_STATE_SUCCESS;
    ESP_LOGI(TAG, "ota success: %u bytes; restart to apply",
             (unsigned)s_bytes_written);
    return ESP_OK;
}

esp_err_t ota_update_abort(void)
{
    if (s_handle != 0) {
        esp_ota_abort(s_handle);
        s_handle = 0;
    }
    s_partition = NULL;
    s_bytes_written = 0;
    s_state = OTA_STATE_IDLE;
    s_last_error[0] = '\0';
    return ESP_OK;
}

ota_state_t ota_update_state(void)        { return s_state; }
size_t      ota_update_bytes_written(void) { return s_bytes_written; }
const char *ota_update_last_error(void)    { return s_last_error; }
