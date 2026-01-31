#pragma once
#include <glib.h>

typedef struct {
  gboolean always_on_top;
  gint refresh_ms; // UI update interval (ms)
} LiveSpiffUiSettings;

LiveSpiffUiSettings ui_settings_load(void);
void ui_settings_save(const LiveSpiffUiSettings *s);

// Returns ~/.config/livespiff/ui.ini (caller frees)
char* ui_settings_path(void);
