#include "ui_settings.h"

static void ensure_parent_dir(const char *file_path) {
  char *dir = g_path_get_dirname(file_path);
  g_mkdir_with_parents(dir, 0700);
  g_free(dir);
}

char* ui_settings_path(void) {
  const char *base = g_get_user_config_dir();
  char *dir = g_build_filename(base, "livespiff", NULL);
  char *path = g_build_filename(dir, "ui.ini", NULL);
  g_free(dir);
  return path;
}

void ui_settings_free_fields(LiveSpiffUiSettings *s) {
  if (!s) return;
  g_free(s->picked_window_id);
  g_free(s->picked_classname);
  g_free(s->picked_window_title);
  s->picked_window_id = NULL;
  s->picked_classname = NULL;
  s->picked_window_title = NULL;
}

LiveSpiffUiSettings ui_settings_load(void) {
  LiveSpiffUiSettings s = {0};
  s.always_on_top = FALSE;
  s.refresh_ms = 50;
  s.picked_pid = -1;

  char *path = ui_settings_path();
  GKeyFile *kf = g_key_file_new();

  if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
    if (g_key_file_has_key(kf, "ui", "always_on_top", NULL))
      s.always_on_top = g_key_file_get_boolean(kf, "ui", "always_on_top", NULL);

    if (g_key_file_has_key(kf, "ui", "refresh_ms", NULL))
      s.refresh_ms = g_key_file_get_integer(kf, "ui", "refresh_ms", NULL);

    if (g_key_file_has_key(kf, "game", "window_id", NULL))
      s.picked_window_id = g_key_file_get_string(kf, "game", "window_id", NULL);

    if (g_key_file_has_key(kf, "game", "classname", NULL))
      s.picked_classname = g_key_file_get_string(kf, "game", "classname", NULL);

    if (g_key_file_has_key(kf, "game", "title", NULL))
      s.picked_window_title = g_key_file_get_string(kf, "game", "title", NULL);

    if (g_key_file_has_key(kf, "game", "pid", NULL))
      s.picked_pid = g_key_file_get_integer(kf, "game", "pid", NULL);
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

  if (s->picked_window_id && s->picked_window_id[0])
    g_key_file_set_string(kf, "game", "window_id", s->picked_window_id);
  else
    g_key_file_remove_key(kf, "game", "window_id", NULL);

  if (s->picked_classname && s->picked_classname[0])
    g_key_file_set_string(kf, "game", "classname", s->picked_classname);
  else
    g_key_file_remove_key(kf, "game", "classname", NULL);

  if (s->picked_window_title && s->picked_window_title[0])
    g_key_file_set_string(kf, "game", "title", s->picked_window_title);
  else
    g_key_file_remove_key(kf, "game", "title", NULL);

  g_key_file_set_integer(kf, "game", "pid", s->picked_pid);

  gsize len = 0;
  gchar *data = g_key_file_to_data(kf, &len, NULL);
  g_file_set_contents(path, data, (gssize)len, NULL);

  g_free(data);
  g_key_file_free(kf);
  g_free(path);
}
