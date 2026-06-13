#ifndef HYDRA_MODBUS_REGISTERS_H
#define HYDRA_MODBUS_REGISTERS_H

/* Modbus holding-register map for the Hydra 64HD controller.
 *
 * Offsets are zero-based. Some Modbus master tools display offset 0 as
 * 40001 -- the master must agree with the slave on numbering. The
 * P2000 / CODESYS driver should be configured to address registers as
 * "4xxxx + 1" or "0-based" matching these constants.
 *
 * Full map mirrors docs/esp32-hydra64hd-controller-plan.md L826-L1071.
 * Result-code values are 1:1 with command_engine's ce_result_t so the
 * same number means the same thing across surfaces.
 *
 * All multi-register integers use LITTLE-WORD order (low word first,
 * high word second).
 */

/* ======================================================================
 * System block (offsets 0-19)
 * ====================================================================== */

#define HYDRA_MODBUS_REG_MAGIC                   0
#define HYDRA_MODBUS_REG_MAP_VERSION             1
#define HYDRA_MODBUS_REG_FW_VERSION_MAJOR        2
#define HYDRA_MODBUS_REG_FW_VERSION_MINOR        3
#define HYDRA_MODBUS_REG_FW_VERSION_PATCH        4
#define HYDRA_MODBUS_REG_CONTROLLER_STATUS       5
#define HYDRA_MODBUS_REG_CONFIG_FLAGS            6
#define HYDRA_MODBUS_REG_REGISTERED_LIGHT_COUNT  7
#define HYDRA_MODBUS_REG_REGISTERED_GROUP_COUNT  8
#define HYDRA_MODBUS_REG_ACTIVE_COMMAND_COUNT    9
#define HYDRA_MODBUS_REG_LAST_ERROR_CODE         10
#define HYDRA_MODBUS_REG_UPTIME_MS_LOW           11
#define HYDRA_MODBUS_REG_UPTIME_MS_HIGH          12
#define HYDRA_MODBUS_REG_FREE_HEAP_KB            13
#define HYDRA_MODBUS_REG_WIFI_STATUS             14
#define HYDRA_MODBUS_REG_MQTT_STATUS             15
#define HYDRA_MODBUS_REG_MODBUS_STATUS           16
#define HYDRA_MODBUS_REG_BLE_SCHEDULER_STATUS    17
#define HYDRA_MODBUS_REG_EVENT_COUNTER           18
#define HYDRA_MODBUS_REG_SYSTEM_RESERVED         19

#define HYDRA_MODBUS_MAGIC_VALUE                 0xA164
#define HYDRA_MODBUS_REG_MAP_VERSION_VALUE       1

/* controller_status bitfield (offset 5) */
#define HYDRA_CS_BIT_CONTROLLER_READY            (1u << 0)
#define HYDRA_CS_BIT_BLE_READY                   (1u << 1)
#define HYDRA_CS_BIT_MODBUS_READY                (1u << 2)
#define HYDRA_CS_BIT_WIFI_ENABLED                (1u << 3)
#define HYDRA_CS_BIT_WIFI_CONNECTED              (1u << 4)
#define HYDRA_CS_BIT_MQTT_ENABLED                (1u << 5)
#define HYDRA_CS_BIT_MQTT_CONNECTED              (1u << 6)
#define HYDRA_CS_BIT_OTA_IN_PROGRESS             (1u << 7)
#define HYDRA_CS_BIT_LIGHT_UNREACHABLE           (1u << 8)
#define HYDRA_CS_BIT_QUEUE_NONEMPTY              (1u << 9)
#define HYDRA_CS_BIT_LAST_CMD_FAILED             (1u << 10)

/* config_flags bitfield (offset 6) */
#define HYDRA_CF_BIT_WIFI_ENABLED                (1u << 0)
#define HYDRA_CF_BIT_MQTT_ENABLED                (1u << 1)
#define HYDRA_CF_BIT_HA_DISCOVERY_ENABLED        (1u << 2)
#define HYDRA_CF_BIT_WEB_UI_ENABLED              (1u << 3)
#define HYDRA_CF_BIT_MODBUS_ENABLED              (1u << 4)
#define HYDRA_CF_BIT_MODBUS_MASTER_MODE          (1u << 5)

#define HYDRA_WIFI_STATUS_DISABLED               0
#define HYDRA_WIFI_STATUS_DISCONNECTED           1
#define HYDRA_WIFI_STATUS_CONNECTED              2
#define HYDRA_WIFI_STATUS_AP_MODE                3

#define HYDRA_MQTT_STATUS_DISABLED               0
#define HYDRA_MQTT_STATUS_DISCONNECTED           1
#define HYDRA_MQTT_STATUS_CONNECTED              2

#define HYDRA_MODBUS_STATUS_DISABLED             0
#define HYDRA_MODBUS_STATUS_SLAVE_READY          1
#define HYDRA_MODBUS_STATUS_MASTER_READY         2
#define HYDRA_MODBUS_STATUS_ERROR                3

#define HYDRA_BLE_SCHEDULER_IDLE                 0
#define HYDRA_BLE_SCHEDULER_BUSY                 1
#define HYDRA_BLE_SCHEDULER_BACKOFF              2
#define HYDRA_BLE_SCHEDULER_ERROR                3

/* ======================================================================
 * Config action registers (offsets 30-33)
 * ====================================================================== */

#define HYDRA_MODBUS_REG_CONFIG_ACTION           30
#define HYDRA_MODBUS_REG_CONFIG_ACTION_SEQ       31
#define HYDRA_MODBUS_REG_CONFIG_ACTION_RESULT    32
#define HYDRA_MODBUS_REG_CONFIG_ACTION_RESERVED  33

#define HYDRA_CONFIG_ACTION_COMMIT_FLAGS         1
#define HYDRA_CONFIG_ACTION_REBOOT               2
#define HYDRA_CONFIG_ACTION_CLEAR_LAST_ERROR     3

/* ======================================================================
 * Light command blocks (1000 + index*100)
 * ====================================================================== */

#define HYDRA_MODBUS_LIGHT0_BASE                 1000
#define HYDRA_MODBUS_LIGHT_BLOCK_SIZE            100
#define HYDRA_MODBUS_MAX_LIGHTS                  4

#define HYDRA_MODBUS_LIGHT_BASE(idx) \
    (HYDRA_MODBUS_LIGHT0_BASE + (idx) * HYDRA_MODBUS_LIGHT_BLOCK_SIZE)

#define HYDRA_LIGHT_OFF_PRESENT                  0
#define HYDRA_LIGHT_OFF_ENABLED                  1
#define HYDRA_LIGHT_OFF_CONNECTION_STATUS        2
#define HYDRA_LIGHT_OFF_LAST_RESULT_CODE         3
#define HYDRA_LIGHT_OFF_LAST_COMMAND_SEQ         4
#define HYDRA_LIGHT_OFF_COMMAND_SEQ              5
#define HYDRA_LIGHT_OFF_COMMAND_CODE             6
#define HYDRA_LIGHT_OFF_TARGET_TYPE              7
#define HYDRA_LIGHT_OFF_TARGET_INDEX             8
#define HYDRA_LIGHT_OFF_TIMEOUT_SECONDS          9
#define HYDRA_LIGHT_OFF_PRESET_ID                10
#define HYDRA_LIGHT_OFF_REPLACE_FLAG             11
#define HYDRA_LIGHT_OFF_RAMP_FROM                12
#define HYDRA_LIGHT_OFF_RAMP_TO                  13
#define HYDRA_LIGHT_OFF_RAMP_DURATION_MS_LOW     14
#define HYDRA_LIGHT_OFF_RAMP_DURATION_MS_HIGH    15
#define HYDRA_LIGHT_OFF_RAMP_STEPS               16
#define HYDRA_LIGHT_OFF_CANCEL_RUNNING           17
#define HYDRA_LIGHT_OFF_RESERVED_18              18
#define HYDRA_LIGHT_OFF_RESERVED_19              19

#define HYDRA_LIGHT_OFF_CH_BRIGHTNESS            20
#define HYDRA_LIGHT_OFF_CH_COOLWHITE             21
#define HYDRA_LIGHT_OFF_CH_BLUE                  22
#define HYDRA_LIGHT_OFF_CH_DEEPRED               23
#define HYDRA_LIGHT_OFF_CH_VIOLET                24
#define HYDRA_LIGHT_OFF_CH_UV                    25
#define HYDRA_LIGHT_OFF_CH_GREEN                 26
#define HYDRA_LIGHT_OFF_CH_ROYALBLUE             27
#define HYDRA_LIGHT_OFF_CH_MOONLIGHT             28

#define HYDRA_LIGHT_OFF_CURRENT_POWER            40
#define HYDRA_LIGHT_OFF_CURRENT_BRIGHTNESS       41
#define HYDRA_LIGHT_OFF_CURRENT_COOLWHITE        42
#define HYDRA_LIGHT_OFF_CURRENT_BLUE             43
#define HYDRA_LIGHT_OFF_CURRENT_DEEPRED          44
#define HYDRA_LIGHT_OFF_CURRENT_VIOLET           45
#define HYDRA_LIGHT_OFF_CURRENT_UV               46
#define HYDRA_LIGHT_OFF_CURRENT_GREEN            47
#define HYDRA_LIGHT_OFF_CURRENT_ROYALBLUE        48
#define HYDRA_LIGHT_OFF_CURRENT_MOONLIGHT        49
#define HYDRA_LIGHT_OFF_LAST_SEEN_RSSI_ABS       50
#define HYDRA_LIGHT_OFF_LAST_SEEN_AGE_SECONDS    51
#define HYDRA_LIGHT_OFF_CMD_QUEUE_DEPTH          52
#define HYDRA_LIGHT_OFF_BLE_ERROR_COUNT          53
#define HYDRA_LIGHT_OFF_SUCCESS_COUNT            54
#define HYDRA_LIGHT_OFF_FAILED_COUNT             55

#define HYDRA_LIGHT_OFF_SERIAL_ASCII_BASE        60
#define HYDRA_LIGHT_OFF_SERIAL_ASCII_LEN_REGS    10
#define HYDRA_LIGHT_OFF_NAME_ASCII_BASE          70
#define HYDRA_LIGHT_OFF_NAME_ASCII_LEN_REGS      10

#define HYDRA_LIGHT_CONN_UNKNOWN                 0
#define HYDRA_LIGHT_CONN_DISCONNECTED            1
#define HYDRA_LIGHT_CONN_CONNECTING              2
#define HYDRA_LIGHT_CONN_READY                   3
#define HYDRA_LIGHT_CONN_BUSY                    4
#define HYDRA_LIGHT_CONN_ERROR                   5

#define HYDRA_LIGHT_POWER_OFF                    0
#define HYDRA_LIGHT_POWER_ON                     1
#define HYDRA_LIGHT_POWER_UNKNOWN                2

/* ======================================================================
 * Group command blocks (2000 + index*50)
 * ====================================================================== */

#define HYDRA_MODBUS_GROUP0_BASE                 2000
#define HYDRA_MODBUS_GROUP_BLOCK_SIZE            50
#define HYDRA_MODBUS_MAX_GROUPS                  4

#define HYDRA_MODBUS_GROUP_BASE(idx) \
    (HYDRA_MODBUS_GROUP0_BASE + (idx) * HYDRA_MODBUS_GROUP_BLOCK_SIZE)

#define HYDRA_GROUP_OFF_PRESENT                  0
#define HYDRA_GROUP_OFF_ENABLED                  1
#define HYDRA_GROUP_OFF_STATUS                   2
#define HYDRA_GROUP_OFF_LAST_RESULT_CODE         3
#define HYDRA_GROUP_OFF_LAST_COMMAND_SEQ         4
#define HYDRA_GROUP_OFF_COMMAND_SEQ              5
#define HYDRA_GROUP_OFF_COMMAND_CODE             6
#define HYDRA_GROUP_OFF_TARGET_TYPE              7
#define HYDRA_GROUP_OFF_TARGET_INDEX             8
#define HYDRA_GROUP_OFF_TIMEOUT_SECONDS          9
#define HYDRA_GROUP_OFF_PRESET_ID                10
#define HYDRA_GROUP_OFF_REPLACE_FLAG             11
#define HYDRA_GROUP_OFF_RAMP_FROM                12
#define HYDRA_GROUP_OFF_RAMP_TO                  13
#define HYDRA_GROUP_OFF_RAMP_DURATION_MS_LOW     14
#define HYDRA_GROUP_OFF_RAMP_DURATION_MS_HIGH    15
#define HYDRA_GROUP_OFF_RAMP_STEPS               16

#define HYDRA_GROUP_OFF_CH_BRIGHTNESS            20
#define HYDRA_GROUP_OFF_CH_COOLWHITE             21
#define HYDRA_GROUP_OFF_CH_BLUE                  22
#define HYDRA_GROUP_OFF_CH_DEEPRED               23
#define HYDRA_GROUP_OFF_CH_VIOLET                24
#define HYDRA_GROUP_OFF_CH_UV                    25
#define HYDRA_GROUP_OFF_CH_GREEN                 26
#define HYDRA_GROUP_OFF_CH_ROYALBLUE             27
#define HYDRA_GROUP_OFF_CH_MOONLIGHT             28

#define HYDRA_GROUP_OFF_MEMBER_SUCCESS_BITMASK   40
#define HYDRA_GROUP_OFF_MEMBER_FAILURE_BITMASK   41
#define HYDRA_GROUP_OFF_ACTIVE_MEMBER_COUNT      42

#define HYDRA_GROUP_STATUS_UNKNOWN               0
#define HYDRA_GROUP_STATUS_READY                 1
#define HYDRA_GROUP_STATUS_RUNNING               2
#define HYDRA_GROUP_STATUS_PARTIAL_FAILURE       3
#define HYDRA_GROUP_STATUS_ERROR                 4

/* ======================================================================
 * Cross-cutting enums
 * ====================================================================== */

#define HYDRA_CMD_CODE_NOOP                      0
#define HYDRA_CMD_CODE_OFF                       1
#define HYDRA_CMD_CODE_ON                        2
#define HYDRA_CMD_CODE_APPLY_CHANNELS            3
#define HYDRA_CMD_CODE_PRESET                    4
#define HYDRA_CMD_CODE_RAMP                      5
#define HYDRA_CMD_CODE_IDENTIFY                  6

#define HYDRA_TARGET_TYPE_NONE                   0
#define HYDRA_TARGET_TYPE_LIGHT                  1
#define HYDRA_TARGET_TYPE_GROUP                  2

#define HYDRA_PRESET_ID_NONE                     0
#define HYDRA_PRESET_ID_OFF                      1
#define HYDRA_PRESET_ID_ON                       2
#define HYDRA_PRESET_ID_MOONLIGHT                3
#define HYDRA_PRESET_ID_BLUE_MOONLIGHT           4
#define HYDRA_PRESET_ID_TEST_25                  5

#define HYDRA_MODBUS_RESULT_IDLE                 0
#define HYDRA_MODBUS_RESULT_ACCEPTED             1
#define HYDRA_MODBUS_RESULT_RUNNING              2
#define HYDRA_MODBUS_RESULT_SUCCESS              3
#define HYDRA_MODBUS_RESULT_PARTIAL_FAILURE      4
#define HYDRA_MODBUS_RESULT_INVALID_COMMAND      10
#define HYDRA_MODBUS_RESULT_INVALID_TARGET       11
#define HYDRA_MODBUS_RESULT_INVALID_CHANNEL      12
#define HYDRA_MODBUS_RESULT_INVALID_INTENSITY    13
#define HYDRA_MODBUS_RESULT_BUSY                 14
#define HYDRA_MODBUS_RESULT_QUEUE_FULL           15
#define HYDRA_MODBUS_RESULT_BLE_CONNECT_FAILED   20
#define HYDRA_MODBUS_RESULT_BLE_CONFIRM_TIMEOUT  21
#define HYDRA_MODBUS_RESULT_UNSUPPORTED_LIGHT    22
#define HYDRA_MODBUS_RESULT_INTERNAL_ERROR       30

#define HYDRA_MODBUS_CH_BASE_OFFSET              20

/* Legacy short-name aliases for code that imported the original
 * scaffolded version. */
#define HYDRA_MODBUS_MAGIC_REGISTER              HYDRA_MODBUS_REG_MAGIC
#define HYDRA_MODBUS_REGISTER_MAP_VERSION        HYDRA_MODBUS_REG_MAP_VERSION
#define HYDRA_MODBUS_CONTROLLER_STATUS           HYDRA_MODBUS_REG_CONTROLLER_STATUS
#define HYDRA_MODBUS_CONFIG_FLAGS                HYDRA_MODBUS_REG_CONFIG_FLAGS
#define HYDRA_MODBUS_CMD_SEQ_OFFSET              HYDRA_LIGHT_OFF_COMMAND_SEQ
#define HYDRA_MODBUS_CMD_CODE_OFFSET             HYDRA_LIGHT_OFF_COMMAND_CODE
#define HYDRA_MODBUS_PRESET_ID_OFFSET            HYDRA_LIGHT_OFF_PRESET_ID
#define HYDRA_MODBUS_CHANNEL_BASE_OFFSET         HYDRA_MODBUS_CH_BASE_OFFSET

#endif /* HYDRA_MODBUS_REGISTERS_H */
