#include "ui_settings.h"

static void ensure_parent_dir(const char *file_path) {
  char *dir = g_path_get_dirname(file_path);
  g_mkdir_with_parents(dir, 0700);
  g_free(dir);
}

char* ui_settings_path(void) {
  const char *base = g_get_user_config_dir(); // ~/.config
  char *dir = g_build_filename(base, "livespiff", NULL);
  char *path = g_build_filename(dir, "ui.ini", NULL);
  g_free(dir);
  return path;
}

LiveSpiffUiSettings ui_settings_load(void) {
  LiveSpiffUiSettings s = {0};
  s.always_on_top = FALSE;
  s.refresh_ms = 50;

  char *path = ui_settings_path();
  GKeyFile *kf = g_key_file_new();
  GError *err = NULL;

  if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &err)) {
    if (g_key_file_has_key(kf, "ui", "always_on_top", NULL))
      s.always_on_top = g_key_file_get_boolean(kf, "ui", "always_on_top", NULL);
    if (g_key_file_has_key(kf, "ui", "refresh_ms", NULL))
      s.refresh_ms = g_key_file_get_integer(kf, "ui", "refresh_ms", NULL);
  } else {
    if (err) g_error_free(err);
  }

  g_key_file_free(kf);
  g_free(path);

  if (s.refresh_ms < 10) s.refresh_ms = 10;
  if (s.refresh_ms > 1000) s.refresh_ms = 1000;
  return s;
}

void ui_settings_save(const LiveSpiffUiSettings *s) {
  if (!s) return;

  char *path = ui_settings_path();
  ensure_parent_dir(path);

  GKeyFile *kf = g_key_file_new();
  g_key_file_set_boolean(kf, "ui", "always_on_top", s->always_on_top);
  g_key_file_set_integer(kf, "ui", "refresh_ms", s->refresh_ms);

  gsize len = 0;
  gchar *data = g_key_file_to_data(kf, &len, NULL);

  // Atomic write
  GError *err = NULL;
  if (!g_file_set_contents(path, data, (gssize)len, &err)) {
    if (err) g_error_free(err);
  }

  g_free(data);
  g_key_file_free(kf);
  g_free(path);
}
