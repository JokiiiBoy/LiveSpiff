#include "storage.h"
#include <json-glib/json-glib.h>

static gboolean ensure_dir(const char *path, char **out_error) {
  GError *err = NULL;
  if (g_mkdir_with_parents(path, 0700) != 0) {
    if (out_error) *out_error = g_strdup_printf("Failed to create directory: %s", path);
    return FALSE;
  }
  return TRUE;
}

char* livespiff_config_dir(void) {
  const char *base = g_get_user_config_dir(); // ~/.config
  return g_build_filename(base, "livespiff", NULL);
}

char* livespiff_data_dir(void) {
  const char *base = g_get_user_data_dir(); // ~/.local/share
  return g_build_filename(base, "livespiff", NULL);
}

char* livespiff_runs_dir(void) {
  char *data = livespiff_data_dir();
  char *runs = g_build_filename(data, "runs", NULL);
  g_free(data);
  return runs;
}

LiveSpiffRun* run_new_default(void) {
  LiveSpiffRun *r = g_new0(LiveSpiffRun, 1);
  r->game = g_strdup("Game");
  r->category = g_strdup("Any%");
  r->segments = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(r->segments, g_strdup("Split 1"));
  g_ptr_array_add(r->segments, g_strdup("Split 2"));
  g_ptr_array_add(r->segments, g_strdup("Split 3"));
  return r;
}

void run_free(LiveSpiffRun *run) {
  if (!run) return;
  g_free(run->game);
  g_free(run->category);
  if (run->segments) g_ptr_array_free(run->segments, TRUE);
  g_free(run);
}

static JsonNode* run_to_json_node(const LiveSpiffRun *run) {
  JsonBuilder *b = json_builder_new();

  json_builder_begin_object(b);

  json_builder_set_member_name(b, "game");
  json_builder_add_string_value(b, run->game ? run->game : "");

  json_builder_set_member_name(b, "category");
  json_builder_add_string_value(b, run->category ? run->category : "");

  json_builder_set_member_name(b, "segments");
  json_builder_begin_array(b);
  for (guint i = 0; i < run->segments->len; i++) {
    const char *name = (const char*)g_ptr_array_index(run->segments, i);
    json_builder_add_string_value(b, name ? name : "");
  }
  json_builder_end_array(b);

  json_builder_end_object(b);

  JsonNode *root = json_builder_get_root(b);
  g_object_unref(b);
  return root;
}

char* run_to_json_string(const LiveSpiffRun *run) {
  JsonNode *root = run_to_json_node(run);
  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, root);
  json_generator_set_pretty(gen, TRUE);
  char *out = json_generator_to_data(gen, NULL);
  g_object_unref(gen);
  json_node_free(root);
  return out; // g_free by caller
}

gboolean run_save_json(const char *path, const LiveSpiffRun *run, char **out_error) {
  // Ensure parent dirs exist
  char *dir = g_path_get_dirname(path);
  if (!ensure_dir(dir, out_error)) {
    g_free(dir);
    return FALSE;
  }
  g_free(dir);

  JsonNode *root = run_to_json_node(run);
  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, root);
  json_generator_set_pretty(gen, TRUE);

  GError *err = NULL;
  gboolean ok = json_generator_to_file(gen, path, &err);

  if (!ok) {
    if (out_error) *out_error = g_strdup(err ? err->message : "Unknown error");
    if (err) g_error_free(err);
  }

  g_object_unref(gen);
  json_node_free(root);
  return ok;
}

gboolean run_load_json(const char *path, LiveSpiffRun **out_run, char **out_error) {
  if (!out_run) return FALSE;

  JsonParser *parser = json_parser_new();
  GError *err = NULL;

  if (!json_parser_load_from_file(parser, path, &err)) {
    if (out_error) *out_error = g_strdup(err ? err->message : "Failed to load JSON");
    if (err) g_error_free(err);
    g_object_unref(parser);
    return FALSE;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root)) {
    if (out_error) *out_error = g_strdup("Invalid JSON: root is not an object");
    g_object_unref(parser);
    return FALSE;
  }

  JsonObject *obj = json_node_get_object(root);

  LiveSpiffRun *r = g_new0(LiveSpiffRun, 1);
  r->game = g_strdup(json_object_get_string_member_with_default(obj, "game", "Game"));
  r->category = g_strdup(json_object_get_string_member_with_default(obj, "category", "Any%"));
  r->segments = g_ptr_array_new_with_free_func(g_free);

  if (json_object_has_member(obj, "segments")) {
    JsonArray *arr = json_object_get_array_member(obj, "segments");
    guint n = json_array_get_length(arr);
    for (guint i = 0; i < n; i++) {
      const char *s = json_array_get_string_element(arr, i);
      g_ptr_array_add(r->segments, g_strdup(s ? s : ""));
    }
  }

  if (r->segments->len == 0) {
    // fallback to at least 1 segment
    g_ptr_array_add(r->segments, g_strdup("Split 1"));
  }

  *out_run = r;
  g_object_unref(parser);
  return TRUE;
}
