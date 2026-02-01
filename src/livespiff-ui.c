// File: src/livespiff-ui.c
// LiveSpiff GUI (GTK4) -> talks to livespiffd via D-Bus
//
// Features:
// - Shows time/state/splits
// - Edit custom splits (names) and apply them (writes run JSON + calls LoadRun on daemon)
// - Hotkey setup helper for KDE Wayland (global hotkeys via KDE Global Shortcuts calling qdbus6)
// - Window selector removed here to keep it stable (can be re-added later)
//
// Notes:
// - On Wayland you generally cannot enforce "always on top" via GTK4.
//   Use KDE Window Rules to force "Keep Above Others" if you need overlay behavior.

#include <gtk/gtk.h>
#include <gio/gio.h>
#include <glib.h>
#include <string.h>

#include "ui_settings.h" // we reuse ui_settings_path() to store extra settings in the same ini

#define LS_BUS_NAME   "com.livespiff.LiveSpiff"
#define LS_OBJ_PATH   "/com/livespiff/LiveSpiff"
#define LS_IFACE_NAME "com.livespiff.LiveSpiff.Control"

typedef struct {
  GtkApplication *app;
  GtkWindow *win;

  GtkLabel *time_label;
  GtkLabel *state_label;
  GtkLabel *split_label;

  GtkButton *btn_settings;
  GtkButton *btn_splits;
  GtkButton *btn_hotkeys;
  GtkButton *btn_start_split;
  GtkButton *btn_pause;
  GtkButton *btn_reset;

  GDBusProxy *proxy_ls;
  guint tick_id;

  // UI preferences
  gint refresh_ms;
} Ui;

/* ------------------------- helpers: paths + INI ------------------------- */

static char* livespiff_default_run_path(void) {
  const char *data = g_get_user_data_dir(); // ~/.local/share
  return g_build_filename(data, "livespiff", "runs", "LiveSpiff_Run.json", NULL);
}

static GKeyFile* keyfile_load_or_new(void) {
  GKeyFile *kf = g_key_file_new();
  char *path = ui_settings_path();
  g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
  g_free(path);
  return kf;
}

static void keyfile_save(GKeyFile *kf) {
  char *path = ui_settings_path();
  char *dir = g_path_get_dirname(path);
  g_mkdir_with_parents(dir, 0700);
  g_free(dir);

  gsize len = 0;
  gchar *data = g_key_file_to_data(kf, &len, NULL);
  g_file_set_contents(path, data, (gssize)len, NULL);

  g_free(data);
  g_free(path);
}

/* ------------------------- helpers: splits storage ------------------------- */

static GPtrArray* splits_load(void) {
  GKeyFile *kf = keyfile_load_or_new();

  gsize n = 0;
  gchar **arr = g_key_file_get_string_list(kf, "splits", "names", &n, NULL);

  GPtrArray *splits = g_ptr_array_new_with_free_func(g_free);

  if (arr && n > 0) {
    for (gsize i = 0; i < n; i++) {
      if (arr[i] && arr[i][0]) g_ptr_array_add(splits, g_strdup(arr[i]));
    }
    g_strfreev(arr);
  }

  // default
  if (splits->len == 0) {
    g_ptr_array_add(splits, g_strdup("Split 1"));
    g_ptr_array_add(splits, g_strdup("Split 2"));
    g_ptr_array_add(splits, g_strdup("Split 3"));
  }

  g_key_file_free(kf);
  return splits;
}

static void splits_save(GPtrArray *splits) {
  GKeyFile *kf = keyfile_load_or_new();

  gsize n = splits ? splits->len : 0;
  gchar **arr = g_new0(gchar*, n);

  for (gsize i = 0; i < n; i++) {
    arr[i] = g_strdup((const char*)g_ptr_array_index(splits, i));
  }

  g_key_file_set_string_list(kf, "splits", "names", (const gchar* const*)arr, n);

  for (gsize i = 0; i < n; i++) g_free(arr[i]);
  g_free(arr);

  keyfile_save(kf);
  g_key_file_free(kf);
}

/* ------------------------- helpers: hotkeys storage (labels only) ------------------------- */

static char* hk_get_or_default(GKeyFile *kf, const char *key, const char *defv) {
  if (g_key_file_has_key(kf, "hotkeys", key, NULL)) {
    return g_key_file_get_string(kf, "hotkeys", key, NULL);
  }
  return g_strdup(defv);
}

static void hotkeys_load(char **out_start, char **out_pause, char **out_reset) {
  GKeyFile *kf = keyfile_load_or_new();
  *out_start = hk_get_or_default(kf, "start_split", "Ctrl+Alt+S");
  *out_pause = hk_get_or_default(kf, "pause", "Ctrl+Alt+P");
  *out_reset = hk_get_or_default(kf, "reset", "Ctrl+Alt+R");
  g_key_file_free(kf);
}

static void hotkeys_save(const char *start, const char *pause, const char *reset) {
  GKeyFile *kf = keyfile_load_or_new();
  g_key_file_set_string(kf, "hotkeys", "start_split", start ? start : "");
  g_key_file_set_string(kf, "hotkeys", "pause", pause ? pause : "");
  g_key_file_set_string(kf, "hotkeys", "reset", reset ? reset : "");
  keyfile_save(kf);
  g_key_file_free(kf);
}

/* ------------------------- helpers: JSON writer (minimal, safe) ------------------------- */

static void ensure_parent_dir_for_file(const char *file_path) {
  char *dir = g_path_get_dirname(file_path);
  g_mkdir_with_parents(dir, 0700);
  g_free(dir);
}

// Basic JSON string escape
static char* json_escape(const char *s) {
  if (!s) return g_strdup("");
  GString *g = g_string_new(NULL);
  for (const unsigned char *p = (const unsigned char*)s; *p; p++) {
    unsigned char c = *p;
    switch (c) {
      case '\"': g_string_append(g, "\\\""); break;
      case '\\': g_string_append(g, "\\\\"); break;
      case '\b': g_string_append(g, "\\b"); break;
      case '\f': g_string_append(g, "\\f"); break;
      case '\n': g_string_append(g, "\\n"); break;
      case '\r': g_string_append(g, "\\r"); break;
      case '\t': g_string_append(g, "\\t"); break;
      default:
        if (c < 0x20) {
          g_string_append_printf(g, "\\u%04x", (unsigned)c);
        } else {
          g_string_append_c(g, (char)c);
        }
        break;
    }
  }
  return g_string_free(g, FALSE);
}

static gboolean write_run_json(const char *path, GPtrArray *splits, char **out_err) {
  if (out_err) *out_err = NULL;
  if (!path || !path[0]) {
    if (out_err) *out_err = g_strdup("Invalid run path");
    return FALSE;
  }

  ensure_parent_dir_for_file(path);

  GString *js = g_string_new(NULL);
  g_string_append(js, "{\n");
  g_string_append(js, "  \"game\": \"Game\",\n");
  g_string_append(js, "  \"category\": \"Any%\",\n");
  g_string_append(js, "  \"segments\": [\n");

  for (guint i = 0; i < splits->len; i++) {
    const char *name = (const char*)g_ptr_array_index(splits, i);
    char *esc = json_escape(name);
    g_string_append_printf(js, "    \"%s\"%s\n", esc, (i + 1 < splits->len) ? "," : "");
    g_free(esc);
  }

  g_string_append(js, "  ]\n");
  g_string_append(js, "}\n");

  gboolean ok = g_file_set_contents(path, js->str, (gssize)js->len, NULL);
  if (!ok && out_err) *out_err = g_strdup("Failed to write run file");

  g_string_free(js, TRUE);
  return ok;
}

/* ------------------------- D-Bus calls ------------------------- */

static gboolean ls_call_i64(Ui *ui, const char *method, gint64 *out_val) {
  if (!ui->proxy_ls) return FALSE;
  GError *err = NULL;
  GVariant *ret = g_dbus_proxy_call_sync(ui->proxy_ls, method, NULL,
                                        G_DBUS_CALL_FLAGS_NONE, 200, NULL, &err);
  if (!ret) { if (err) g_error_free(err); return FALSE; }
  gint64 v = 0; g_variant_get(ret, "(x)", &v);
  g_variant_unref(ret);
  if (out_val) *out_val = v;
  return TRUE;
}

static gboolean ls_call_i32(Ui *ui, const char *method, gint32 *out_val) {
  if (!ui->proxy_ls) return FALSE;
  GError *err = NULL;
  GVariant *ret = g_dbus_proxy_call_sync(ui->proxy_ls, method, NULL,
                                        G_DBUS_CALL_FLAGS_NONE, 200, NULL, &err);
  if (!ret) { if (err) g_error_free(err); return FALSE; }
  gint32 v = 0; g_variant_get(ret, "(i)", &v);
  g_variant_unref(ret);
  if (out_val) *out_val = v;
  return TRUE;
}

static gboolean ls_call_str(Ui *ui, const char *method, char **out_str) {
  if (!ui->proxy_ls) return FALSE;
  GError *err = NULL;
  GVariant *ret = g_dbus_proxy_call_sync(ui->proxy_ls, method, NULL,
                                        G_DBUS_CALL_FLAGS_NONE, 200, NULL, &err);
  if (!ret) { if (err) g_error_free(err); return FALSE; }
  const char *s = NULL; g_variant_get(ret, "(&s)", &s);
  if (out_str) *out_str = g_strdup(s ? s : "");
  g_variant_unref(ret);
  return TRUE;
}

static void ls_call_void(Ui *ui, const char *method) {
  if (!ui->proxy_ls) return;
  GError *err = NULL;
  GVariant *ret = g_dbus_proxy_call_sync(ui->proxy_ls, method, NULL,
                                        G_DBUS_CALL_FLAGS_NONE, 200, NULL, &err);
  if (ret) g_variant_unref(ret);
  if (err) g_error_free(err);
}

// LoadRun(path) -> (b ok, s message)
static gboolean ls_call_load_run(Ui *ui, const char *path, char **out_msg) {
  if (out_msg) *out_msg = NULL;
  if (!ui->proxy_ls) {
    if (out_msg) *out_msg = g_strdup("Daemon not connected");
    return FALSE;
  }

  GVariant *params = g_variant_new("(s)", path);
  GError *err = NULL;

  GVariant *ret = g_dbus_proxy_call_sync(
    ui->proxy_ls,
    "LoadRun",
    params,
    G_DBUS_CALL_FLAGS_NONE,
    2000,
    NULL,
    &err
  );

  if (!ret) {
    if (out_msg) *out_msg = g_strdup(err ? err->message : "LoadRun failed");
    if (err) g_error_free(err);
    return FALSE;
  }

  gboolean ok = FALSE;
  const char *msg = NULL;
  g_variant_get(ret, "(bs)", &ok, &msg);
  if (out_msg) *out_msg = g_strdup(msg ? msg : "");
  g_variant_unref(ret);
  return ok;
}

/* ------------------------- time formatting ------------------------- */

static char* format_time_ms(gint64 ms) {
  if (ms < 0) ms = 0;
  gint64 total_sec = ms / 1000;
  gint64 milli = ms % 1000;
  gint64 sec = total_sec % 60;
  gint64 min = (total_sec / 60) % 60;
  gint64 hour = total_sec / 3600;

  return g_strdup_printf("%02lld:%02lld:%02lld.%03lld",
                         (long long)hour,
                         (long long)min,
                         (long long)sec,
                         (long long)milli);
}

/* ------------------------- main tick ------------------------- */

static gboolean ui_tick(gpointer user_data) {
  Ui *ui = (Ui*)user_data;

  if (!ui->proxy_ls) {
    gtk_label_set_text(ui->state_label, "Daemon not running");
    gtk_label_set_text(ui->time_label, "--:--:--.---");
    gtk_label_set_text(ui->split_label, "Split: - / -");
    return G_SOURCE_CONTINUE;
  }

  // elapsed
  gint64 ms = 0;
  if (ls_call_i64(ui, "ElapsedMs", &ms)) {
    char *t = format_time_ms(ms);
    gtk_label_set_text(ui->time_label, t);
    g_free(t);
  } else {
    gtk_label_set_text(ui->time_label, "--:--:--.---");
  }

  // state
  char *state = NULL;
  if (ls_call_str(ui, "State", &state)) {
    gtk_label_set_text(ui->state_label, state);
    g_free(state);
  } else {
    gtk_label_set_text(ui->state_label, "Unknown");
  }

  // splits
  gint32 cur = 0, count = 0;
  if (ls_call_i32(ui, "CurrentSplit", &cur) && ls_call_i32(ui, "SplitCount", &count)) {
    char *s = g_strdup_printf("Split: %d / %d", (int)(cur + 1), (int)count);
    gtk_label_set_text(ui->split_label, s);
    g_free(s);
  } else {
    gtk_label_set_text(ui->split_label, "Split: - / -");
  }

  return G_SOURCE_CONTINUE;
}

static void restart_tick(Ui *ui) {
  if (ui->tick_id) { g_source_remove(ui->tick_id); ui->tick_id = 0; }
  if (ui->refresh_ms < 10) ui->refresh_ms = 10;
  ui->tick_id = g_timeout_add((guint)ui->refresh_ms, ui_tick, ui);
}

/* ------------------------- buttons ------------------------- */

static void on_start_split_clicked(GtkButton *btn, gpointer user_data) { (void)btn; ls_call_void((Ui*)user_data, "StartOrSplit"); }
static void on_pause_clicked(GtkButton *btn, gpointer user_data) { (void)btn; ls_call_void((Ui*)user_data, "TogglePause"); }
static void on_reset_clicked(GtkButton *btn, gpointer user_data) { (void)btn; ls_call_void((Ui*)user_data, "Reset"); }

/* ------------------------- settings window ------------------------- */

typedef struct {
  Ui *ui;
  GtkWindow *dlg;
  GtkSpinButton *spin_refresh;
} SettingsCtx;

static void on_settings_destroy(GtkWidget *w, gpointer user_data) {
  (void)w;
  g_free(user_data);
}

static void on_refresh_changed(GtkSpinButton *sb, gpointer user_data) {
  SettingsCtx *ctx = (SettingsCtx*)user_data;
  gint v = gtk_spin_button_get_value_as_int(sb);
  if (v < 10) v = 10;
  if (v > 1000) v = 1000;

  ctx->ui->refresh_ms = v;

  // store
  GKeyFile *kf = keyfile_load_or_new();
  g_key_file_set_integer(kf, "ui", "refresh_ms", v);
  keyfile_save(kf);
  g_key_file_free(kf);

  restart_tick(ctx->ui);
}

/* ------------------------- splits editor ------------------------- */

typedef struct {
  Ui *ui;
  GtkWindow *dlg;
  GtkListBox *list;
  GtkLabel *status;
} SplitsCtx;

static void on_splits_destroy(GtkWidget *w, gpointer user_data) {
  (void)w;
  g_free(user_data);
}

static GtkWidget* make_split_row(const char *name) {
  GtkWidget *row = gtk_list_box_row_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(entry), name ? name : "");
  gtk_widget_set_hexpand(entry, TRUE);

  gtk_box_append(GTK_BOX(box), entry);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);

  // store pointer to entry on row for reading later
  g_object_set_data(G_OBJECT(row), "entry", entry);

  return row;
}

static void on_splits_add_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SplitsCtx *ctx = (SplitsCtx*)user_data;
  GtkWidget *row = make_split_row("New Split");
  gtk_list_box_append(ctx->list, row);
}

static void on_splits_remove_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SplitsCtx *ctx = (SplitsCtx*)user_data;

  GtkListBoxRow *selected = gtk_list_box_get_selected_row(ctx->list);
  if (!selected) {
    gtk_label_set_text(ctx->status, "Select a split row first.");
    return;
  }
  gtk_list_box_remove(ctx->list, GTK_WIDGET(selected));
}

static GPtrArray* splits_from_list(GtkListBox *list) {
  GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);

  for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(list));
       child != NULL;
       child = gtk_widget_get_next_sibling(child)) {

    if (!GTK_IS_LIST_BOX_ROW(child)) continue;

    GtkWidget *entry = (GtkWidget*)g_object_get_data(G_OBJECT(child), "entry");
    if (entry && GTK_IS_ENTRY(entry)) {
      const char *t = gtk_entry_get_text(GTK_ENTRY(entry));
      if (t && t[0]) g_ptr_array_add(arr, g_strdup(t));
    }
  }

  if (arr->len == 0) {
    g_ptr_array_add(arr, g_strdup("Split 1"));
  }

  return arr;
}

static void on_splits_apply_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SplitsCtx *ctx = (SplitsCtx*)user_data;

  GPtrArray *spl = splits_from_list(ctx->list);
  splits_save(spl);

  // write run json
  char *run_path = livespiff_default_run_path();
  char *err = NULL;

  if (!write_run_json(run_path, spl, &err)) {
    gtk_label_set_text(ctx->status, err ? err : "Failed to write run file.");
    g_free(err);
    g_free(run_path);
    g_ptr_array_free(spl, TRUE);
    return;
  }

  // tell daemon to load it (updates split_count)
  char *msg = NULL;
  gboolean ok = ls_call_load_run(ctx->ui, run_path, &msg);

  if (ok) {
    gtk_label_set_text(ctx->status, "Applied. Daemon loaded run file.");
  } else {
    gtk_label_set_text(ctx->status, msg && msg[0] ? msg : "Applied, but daemon failed to load run.");
  }

  g_free(msg);
  g_free(run_path);
  g_ptr_array_free(spl, TRUE);
}

static void open_splits_editor(Ui *ui) {
  GtkWindow *dlg = GTK_WINDOW(gtk_window_new());
  gtk_window_set_title(dlg, "Splits");
  gtk_window_set_transient_for(dlg, ui->win);
  gtk_window_set_modal(dlg, TRUE);
  gtk_window_set_default_size(dlg, 520, 420);

  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_top(root, 12);
  gtk_widget_set_margin_bottom(root, 12);
  gtk_widget_set_margin_start(root, 12);
  gtk_widget_set_margin_end(root, 12);
  gtk_window_set_child(dlg, root);

  GtkWidget *hint = gtk_label_new("Edit your split names. Click Apply to save and load into the timer.");
  gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
  gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
  gtk_box_append(GTK_BOX(root), hint);

  GtkWidget *sc = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(sc, TRUE);
  gtk_box_append(GTK_BOX(root), sc);

  GtkListBox *list = GTK_LIST_BOX(gtk_list_box_new());
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), GTK_WIDGET(list));
  gtk_list_box_set_selection_mode(list, GTK_SELECTION_SINGLE);

  // load current splits
  GPtrArray *spl = splits_load();
  for (guint i = 0; i < spl->len; i++) {
    GtkWidget *row = make_split_row((const char*)g_ptr_array_index(spl, i));
    gtk_list_box_append(list, row);
  }
  g_ptr_array_free(spl, TRUE);

  GtkWidget *row_btn = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append(GTK_BOX(root), row_btn);

  GtkWidget *btn_add = gtk_button_new_with_label("Add");
  GtkWidget *btn_remove = gtk_button_new_with_label("Remove selected");
  GtkWidget *btn_apply = gtk_button_new_with_label("Apply");

  gtk_box_append(GTK_BOX(row_btn), btn_add);
  gtk_box_append(GTK_BOX(row_btn), btn_remove);
  gtk_box_append(GTK_BOX(row_btn), btn_apply);

  GtkWidget *status = gtk_label_new("");
  gtk_label_set_wrap(GTK_LABEL(status), TRUE);
  gtk_label_set_xalign(GTK_LABEL(status), 0.0f);
  gtk_box_append(GTK_BOX(root), status);

  SplitsCtx *ctx = g_new0(SplitsCtx, 1);
  ctx->ui = ui;
  ctx->dlg = dlg;
  ctx->list = list;
  ctx->status = GTK_LABEL(status);

  g_signal_connect(dlg, "destroy", G_CALLBACK(on_splits_destroy), ctx);
  g_signal_connect(btn_add, "clicked", G_CALLBACK(on_splits_add_clicked), ctx);
  g_signal_connect(btn_remove, "clicked", G_CALLBACK(on_splits_remove_clicked), ctx);
  g_signal_connect(btn_apply, "clicked", G_CALLBACK(on_splits_apply_clicked), ctx);

  gtk_window_present(dlg);
}

/* ------------------------- hotkeys window ------------------------- */

typedef struct {
  Ui *ui;
  GtkWindow *dlg;
  GtkEntry *e_start;
  GtkEntry *e_pause;
  GtkEntry *e_reset;
  GtkLabel *status;
} HotkeysCtx;

static void on_hotkeys_destroy(GtkWidget *w, gpointer user_data) {
  (void)w;
  g_free(user_data);
}

static const char* cmd_start(void) {
  return "qdbus6 com.livespiff.LiveSpiff /com/livespiff/LiveSpiff com.livespiff.LiveSpiff.Control.StartOrSplit";
}
static const char* cmd_pause(void) {
  return "qdbus6 com.livespiff.LiveSpiff /com/livespiff/LiveSpiff com.livespiff.LiveSpiff.Control.TogglePause";
}
static const char* cmd_reset(void) {
  return "qdbus6 com.livespiff.LiveSpiff /com/livespiff/LiveSpiff com.livespiff.LiveSpiff.Control.Reset";
}

static void on_hotkeys_save_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  HotkeysCtx *ctx = (HotkeysCtx*)user_data;

  const char *s = gtk_entry_get_text(ctx->e_start);
  const char *p = gtk_entry_get_text(ctx->e_pause);
  const char *r = gtk_entry_get_text(ctx->e_reset);

  hotkeys_save(s, p, r);
  gtk_label_set_text(ctx->status, "Saved. Now bind the commands in KDE: System Settings → Shortcuts → Custom Shortcuts.");
}

static void open_hotkeys_window(Ui *ui) {
  GtkWindow *dlg = GTK_WINDOW(gtk_window_new());
  gtk_window_set_title(dlg, "Hotkeys");
  gtk_window_set_transient_for(dlg, ui->win);
  gtk_window_set_modal(dlg, TRUE);
  gtk_window_set_default_size(dlg, 700, 360);

  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_top(root, 12);
  gtk_widget_set_margin_bottom(root, 12);
  gtk_widget_set_margin_start(root, 12);
  gtk_widget_set_margin_end(root, 12);
  gtk_window_set_child(dlg, root);

  GtkWidget *hint = gtk_label_new(
    "Wayland note: LiveSpiff does not grab global hotkeys directly.\n"
    "Use KDE Global Shortcuts to run the commands below."
  );
  gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
  gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
  gtk_box_append(GTK_BOX(root), hint);

  char *hs = NULL, *hp = NULL, *hr = NULL;
  hotkeys_load(&hs, &hp, &hr);

  // grid-like layout using boxes
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
  gtk_box_append(GTK_BOX(root), grid);

  // Row 1: Start/Split
  gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Start / Split key (label):"), 0, 0, 1, 1);
  GtkWidget *e_start = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(e_start), hs);
  gtk_grid_attach(GTK_GRID(grid), e_start, 1, 0, 1, 1);

  GtkWidget *c_start = gtk_entry_new();
  gtk_editable_set_editable(GTK_EDITABLE(c_start), FALSE);
  gtk_entry_set_text(GTK_ENTRY(c_start), cmd_start());
  gtk_grid_attach(GTK_GRID(grid), c_start, 2, 0, 1, 1);

  // Row 2: Pause
  gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Pause / Resume key (label):"), 0, 1, 1, 1);
  GtkWidget *e_pause = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(e_pause), hp);
  gtk_grid_attach(GTK_GRID(grid), e_pause, 1, 1, 1, 1);

  GtkWidget *c_pause = gtk_entry_new();
  gtk_editable_set_editable(GTK_EDITABLE(c_pause), FALSE);
  gtk_entry_set_text(GTK_ENTRY(c_pause), cmd_pause());
  gtk_grid_attach(GTK_GRID(grid), c_pause, 2, 1, 1, 1);

  // Row 3: Reset
  gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Reset key (label):"), 0, 2, 1, 1);
  GtkWidget *e_reset = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(e_reset), hr);
  gtk_grid_attach(GTK_GRID(grid), e_reset, 1, 2, 1, 1);

  GtkWidget *c_reset = gtk_entry_new();
  gtk_editable_set_editable(GTK_EDITABLE(c_reset), FALSE);
  gtk_entry_set_text(GTK_ENTRY(c_reset), cmd_reset());
  gtk_grid_attach(GTK_GRID(grid), c_reset, 2, 2, 1, 1);

  g_free(hs); g_free(hp); g_free(hr);

  GtkWidget *btn_save = gtk_button_new_with_label("Save labels");
  gtk_box_append(GTK_BOX(root), btn_save);

  GtkWidget *status = gtk_label_new("");
  gtk_label_set_wrap(GTK_LABEL(status), TRUE);
  gtk_label_set_xalign(GTK_LABEL(status), 0.0f);
  gtk_box_append(GTK_BOX(root), status);

  HotkeysCtx *ctx = g_new0(HotkeysCtx, 1);
  ctx->ui = ui;
  ctx->dlg = dlg;
  ctx->e_start = GTK_ENTRY(e_start);
  ctx->e_pause = GTK_ENTRY(e_pause);
  ctx->e_reset = GTK_ENTRY(e_reset);
  ctx->status = GTK_LABEL(status);

  g_signal_connect(dlg, "destroy", G_CALLBACK(on_hotkeys_destroy), ctx);
  g_signal_connect(btn_save, "clicked", G_CALLBACK(on_hotkeys_save_clicked), ctx);

  gtk_window_present(dlg);
}

/* ------------------------- settings window UI ------------------------- */

static void open_settings_window(Ui *ui) {
  GtkWindow *dlg = GTK_WINDOW(gtk_window_new());
  gtk_window_set_title(dlg, "Settings");
  gtk_window_set_transient_for(dlg, ui->win);
  gtk_window_set_modal(dlg, TRUE);
  gtk_window_set_default_size(dlg, 520, 240);

  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_top(root, 12);
  gtk_widget_set_margin_bottom(root, 12);
  gtk_widget_set_margin_start(root, 12);
  gtk_widget_set_margin_end(root, 12);
  gtk_window_set_child(dlg, root);

  GtkWidget *note = gtk_label_new(
    "Overlay note: On KDE Wayland, use Window Rules to force \"Keep Above Others\" for LiveSpiff.\n"
    "Many games must be set to Borderless Fullscreen for overlays to remain visible."
  );
  gtk_label_set_wrap(GTK_LABEL(note), TRUE);
  gtk_label_set_xalign(GTK_LABEL(note), 0.0f);
  gtk_box_append(GTK_BOX(root), note);

  GtkWidget *lbl = gtk_label_new("Refresh interval (ms)");
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
  gtk_box_append(GTK_BOX(root), lbl);

  GtkWidget *spin = gtk_spin_button_new_with_range(10, 1000, 10);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), ui->refresh_ms);
  gtk_box_append(GTK_BOX(root), spin);

  SettingsCtx *ctx = g_new0(SettingsCtx, 1);
  ctx->ui = ui;
  ctx->dlg = dlg;
  ctx->spin_refresh = GTK_SPIN_BUTTON(spin);

  g_signal_connect(dlg, "destroy", G_CALLBACK(on_settings_destroy), ctx);
  g_signal_connect(spin, "value-changed", G_CALLBACK(on_refresh_changed), ctx);

  gtk_window_present(dlg);
}

/* ------------------------- main window build ------------------------- */

static void on_settings_clicked(GtkButton *btn, gpointer user_data) { (void)btn; open_settings_window((Ui*)user_data); }
static void on_splits_clicked(GtkButton *btn, gpointer user_data) { (void)btn; open_splits_editor((Ui*)user_data); }
static void on_hotkeys_clicked(GtkButton *btn, gpointer user_data) { (void)btn; open_hotkeys_window((Ui*)user_data); }

static void ui_build(Ui *ui) {
  ui->win = GTK_WINDOW(gtk_application_window_new(ui->app));
  gtk_window_set_title(ui->win, "LiveSpiff");
  gtk_window_set_default_size(ui->win, 560, 300);

  // CSS
  GdkDisplay *disp = gdk_display_get_default();
  if (disp) {
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
      "label.time { font-size: 52px; font-weight: 700; }"
      "label.meta { font-size: 16px; opacity: 0.85; }"
    );
    gtk_style_context_add_provider_for_display(
      disp, GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );
    g_object_unref(css);
  }

  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_top(root, 16);
  gtk_widget_set_margin_bottom(root, 16);
  gtk_widget_set_margin_start(root, 16);
  gtk_widget_set_margin_end(root, 16);
  gtk_window_set_child(ui->win, root);

  ui->time_label = GTK_LABEL(gtk_label_new("--:--:--.---"));
  gtk_widget_add_css_class(GTK_WIDGET(ui->time_label), "time");
  gtk_widget_set_halign(GTK_WIDGET(ui->time_label), GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(root), GTK_WIDGET(ui->time_label));

  GtkWidget *meta = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_halign(meta, GTK_ALIGN_CENTER);

  ui->state_label = GTK_LABEL(gtk_label_new("Connecting..."));
  gtk_widget_add_css_class(GTK_WIDGET(ui->state_label), "meta");

  ui->split_label = GTK_LABEL(gtk_label_new("Split: - / -"));
  gtk_widget_add_css_class(GTK_WIDGET(ui->split_label), "meta");

  gtk_box_append(GTK_BOX(meta), GTK_WIDGET(ui->state_label));
  gtk_box_append(GTK_BOX(meta), GTK_WIDGET(ui->split_label));
  gtk_box_append(GTK_BOX(root), meta);

  // top utility buttons
  GtkWidget *tools = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(tools, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(root), tools);

  ui->btn_settings = GTK_BUTTON(gtk_button_new_with_label("Settings"));
  ui->btn_splits   = GTK_BUTTON(gtk_button_new_with_label("Splits"));
  ui->btn_hotkeys  = GTK_BUTTON(gtk_button_new_with_label("Hotkeys"));

  gtk_box_append(GTK_BOX(tools), GTK_WIDGET(ui->btn_settings));
  gtk_box_append(GTK_BOX(tools), GTK_WIDGET(ui->btn_splits));
  gtk_box_append(GTK_BOX(tools), GTK_WIDGET(ui->btn_hotkeys));

  g_signal_connect(ui->btn_settings, "clicked", G_CALLBACK(on_settings_clicked), ui);
  g_signal_connect(ui->btn_splits,   "clicked", G_CALLBACK(on_splits_clicked), ui);
  g_signal_connect(ui->btn_hotkeys,  "clicked", G_CALLBACK(on_hotkeys_clicked), ui);

  // timer control buttons
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(root), row);

  ui->btn_start_split = GTK_BUTTON(gtk_button_new_with_label("Start / Split"));
  ui->btn_pause       = GTK_BUTTON(gtk_button_new_with_label("Pause / Resume"));
  ui->btn_reset       = GTK_BUTTON(gtk_button_new_with_label("Reset"));

  gtk_box_append(GTK_BOX(row), GTK_WIDGET(ui->btn_start_split));
  gtk_box_append(GTK_BOX(row), GTK_WIDGET(ui->btn_pause));
  gtk_box_append(GTK_BOX(row), GTK_WIDGET(ui->btn_reset));

  g_signal_connect(ui->btn_start_split, "clicked", G_CALLBACK(on_start_split_clicked), ui);
  g_signal_connect(ui->btn_pause,       "clicked", G_CALLBACK(on_pause_clicked), ui);
  g_signal_connect(ui->btn_reset,       "clicked", G_CALLBACK(on_reset_clicked), ui);
}

/* ------------------------- activate ------------------------- */

static void on_activate(GtkApplication *app, gpointer user_data) {
  Ui *ui = (Ui*)user_data;
  ui->app = app;

  // load refresh interval from ini
  ui->refresh_ms = 50;
  {
    GKeyFile *kf = keyfile_load_or_new();
    if (g_key_file_has_key(kf, "ui", "refresh_ms", NULL)) {
      ui->refresh_ms = g_key_file_get_integer(kf, "ui", "refresh_ms", NULL);
      if (ui->refresh_ms < 10) ui->refresh_ms = 10;
      if (ui->refresh_ms > 1000) ui->refresh_ms = 1000;
    }
    g_key_file_free(kf);
  }

  ui_build(ui);

  // Connect to daemon
  GError *err = NULL;
  ui->proxy_ls = g_dbus_proxy_new_for_bus_sync(
    G_BUS_TYPE_SESSION,
    G_DBUS_PROXY_FLAGS_NONE,
    NULL,
    LS_BUS_NAME,
    LS_OBJ_PATH,
    LS_IFACE_NAME,
    NULL,
    &err
  );

  if (!ui->proxy_ls) {
    gtk_label_set_text(ui->state_label, "Daemon not running");
    if (err) g_error_free(err);
  }

  // On startup, ensure daemon has our run file (if any splits were set)
  {
    GPtrArray *spl = splits_load();
    char *run_path = livespiff_default_run_path();
    char *werr = NULL;
    if (write_run_json(run_path, spl, &werr)) {
      char *msg = NULL;
      ls_call_load_run(ui, run_path, &msg);
      g_free(msg);
    }
    g_free(werr);
    g_free(run_path);
    g_ptr_array_free(spl, TRUE);
  }

  restart_tick(ui);
  gtk_window_present(ui->win);
}

int main(int argc, char **argv) {
  Ui ui = {0};

  GtkApplication *app =
    gtk_application_new("com.livespiff.LiveSpiff.UI", G_APPLICATION_DEFAULT_FLAGS);

  g_signal_connect(app, "activate", G_CALLBACK(on_activate), &ui);

  int status = g_application_run(G_APPLICATION(app), argc, argv);

  if (ui.tick_id) g_source_remove(ui.tick_id);
  if (ui.proxy_ls) g_object_unref(ui.proxy_ls);
  g_object_unref(app);

  return status;
}
