#pragma once
#include <glib.h>

typedef struct {
  gboolean always_on_top; // preference only (Wayland: KDE rules handle actual "above")
  gint refresh_ms;

  // Selected window (from kdotool)
  char *picked_window_id;     // KWin internal id (UUID-like)
  char *picked_classname;     // kdotool getwindowclassname
  char *picked_window_title;  // kdotool getwindowname
  gint picked_pid;            // kdotool getwindowpid (optional)
} LiveSpiffUiSettings;

LiveSpiffUiSettings ui_settings_load(void);
void ui_settings_save(const LiveSpiffUiSettings *s);
void ui_settings_free_fields(LiveSpiffUiSettings *s);

// returns ~/.config/livespiff/ui.ini (caller frees)
char* ui_settings_path(void);
