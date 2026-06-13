#pragma once

/* Holding register offsets, zero-based. Some Modbus tools display 0 as 40001. */
#define HYDRA_MODBUS_MAGIC_REGISTER              0
#define HYDRA_MODBUS_REGISTER_MAP_VERSION        1
#define HYDRA_MODBUS_CONTROLLER_STATUS           5
#define HYDRA_MODBUS_CONFIG_FLAGS                6

#define HYDRA_MODBUS_LIGHT0_BASE                 1000
#define HYDRA_MODBUS_LIGHT_BLOCK_SIZE            100
#define HYDRA_MODBUS_GROUP0_BASE                 2000
#define HYDRA_MODBUS_GROUP_BLOCK_SIZE            50

#define HYDRA_MODBUS_CMD_SEQ_OFFSET              5
#define HYDRA_MODBUS_CMD_CODE_OFFSET             6
#define HYDRA_MODBUS_PRESET_ID_OFFSET            10
#define HYDRA_MODBUS_CHANNEL_BASE_OFFSET         20

#define HYDRA_MODBUS_RESULT_SUCCESS              3
#define HYDRA_MODBUS_RESULT_PARTIAL_FAILURE      4
#define HYDRA_MODBUS_RESULT_INVALID_COMMAND      10
#define HYDRA_MODBUS_RESULT_INVALID_TARGET       11
#define HYDRA_MODBUS_RESULT_INVALID_CHANNEL      12
#define HYDRA_MODBUS_RESULT_INVALID_INTENSITY    13
#define HYDRA_MODBUS_RESULT_BUSY                 14
#define HYDRA_MODBUS_RESULT_QUEUE_FULL           15
