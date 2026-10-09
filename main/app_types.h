/* main/app_types.h */
#ifndef APP_TYPES_H
#define APP_TYPES_H

typedef enum
{
    INPUT_EVT_ROTATE_CW = 0,
    INPUT_EVT_ROTATE_CCW,
    INPUT_EVT_SHORT_PRESS,
    INPUT_EVT_LONG_PRESS,
    INPUT_EVT_AUTO_HIDE_VOLUME,
} input_event_t;

typedef enum
{
    APP_MODE_BOOT_MENU = 0,
    APP_MODE_BUSY,
    APP_MODE_PLAYER,
    APP_MODE_TRANSFER,
} app_mode_t;

typedef enum
{
    APP_CMD_START_PLAYER = 0,
    APP_CMD_START_TRANSFER,
    APP_CMD_FINISH_TRANSFER,
} app_cmd_t;

#endif