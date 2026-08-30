#ifndef UI_DISPLAY_H
#define UI_DISPLAY_H

#include "esp_err.h"

void ui_display_init(void);
void ui_set_status(const char *text);
void ui_notify_track_started(const char *path, int index, int count);
void ui_notify_track_finished(const char *path);

#endif // UI_DISPLAY_H