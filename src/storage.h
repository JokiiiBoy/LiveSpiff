#pragma once
#include <glib.h>

typedef struct {
  char *game;
  char *category;
  GPtrArray *segments; // array of char*
} LiveSpiffRun;

// Paths (XDG)
char* livespiff_config_dir(void);   // ~/.config/livespiff
char* livespiff_data_dir(void);     // ~/.local/share/livespiff
char* livespiff_runs_dir(void);     // ~/.local/share/livespiff/runs

// Run lifecycle
LiveSpiffRun* run_new_default(void);
void run_free(LiveSpiffRun *run);

// Save / load
gboolean run_load_json(const char *path, LiveSpiffRun **out_run, char **out_error);
gboolean run_save_json(const char *path, const LiveSpiffRun *run, char **out_error);

// Helpers
char* run_to_json_string(const LiveSpiffRun *run); // caller frees
