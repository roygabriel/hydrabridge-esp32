#ifndef HYDRA_OTA_UPDATE_H
#define HYDRA_OTA_UPDATE_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* OTA update for the ESP32 controller firmware itself (NOT the AI
 * lights). Web-upload flow:
 *
 *   ota_update_begin()        -> allocate next OTA partition
 *   ota_update_write(chunk)   -> repeated as the HTTP server streams
 *                                the uploaded image
 *   ota_update_end()          -> finalize, flip boot partition
 *   esp_restart()             -> caller decides when to reboot
 *
 * Rollback: ota_update_init() calls esp_ota_mark_app_valid_cancel_-
 * rollback() so a successful boot of a freshly-flashed image cancels
 * any pending rollback. If the new image crashes before init runs, the
 * bootloader reverts to the previous slot (CONFIG_BOOTLOADER_APP_-
 * ROLLBACK_ENABLE=y, set in sdkconfig.defaults).
 *
 * v1 stub: no HTTPS pull, no signed-image verification. The web upload
 * endpoint lands in Phase 5.2 (web_ui).
 */

typedef enum {
    OTA_STATE_IDLE              = 0,
    OTA_STATE_WRITING           = 1,
    OTA_STATE_FINALIZING        = 2,
    OTA_STATE_SUCCESS           = 3,
    OTA_STATE_FAILED            = 4,
} ota_state_t;

esp_err_t ota_update_init(void);

esp_err_t ota_update_begin(void);
esp_err_t ota_update_write(const void *data, size_t len);
esp_err_t ota_update_end(void);
esp_err_t ota_update_abort(void);

ota_state_t ota_update_state(void);
size_t      ota_update_bytes_written(void);
const char *ota_update_last_error(void);

#endif /* HYDRA_OTA_UPDATE_H */
