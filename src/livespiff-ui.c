// File: src/livespiff-ui.c
// LiveSpiff minimal GUI (GTK4) talking to livespiffd over D-Bus
// Window selector: lists open windows using kdotool (KDE Wayland friendly)

#include <gtk/gtk.h>
#include <gio/gio.h>
#include <string.h>

#include "ui_settings.h"

#define LS_BUS_NAME   "com.livespiff.LiveSpiff"
#define LS_OBJ_PATH   "/com/livespiff/LiveSpiff"
#define LS_IFACE_NAME "com.livespiff.LiveSpiff.Control"

typedef struct {
  GtkApplication *app;
  GtkWindow *win;

  GtkLabel *time_label;
  GtkLabel *state_label;
  GtkLabel *split_label;
  GtkLabel *picked_label;

  GtkButton *btn_settings;
  GtkButton *btn_start_split;
  GtkButton *btn_pause;
  GtkButton *btn_reset;

  GDBusProxy *proxy_ls;
  guint tick_id;

  LiveSpiffUiSettings settings;
} Ui;

typedef struct {
  Ui *ui;
  GtkWindow *dlg;
  GtkListBox *list;
  GtkLabel *status;
} WindowPickerCtx;

/* ---------------- util ---------------- */

static char* str_trim(char *s) {
  if (!s) return NULL;
  while (*s && (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t')) s++;
  size_t len = strlen(s);
  while (len > 0) {
    char c = s[len - 1];
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') { s[len - 1] = 0; len--; }
    else break;
  }
  return s;
}

static gboolean run_cmd_capture(const char *cmdline, char **out_stdout) {
  if (out_stdout) *out_stdout = NULL;
  gchar *stdout_str = NULL;
  gchar *stderr_str = NULL;
  gint status = 0;

  GError *err = NULL;
  gboolean ok = g_spawn_command_line_sync(cmdline, &stdout_str, &stderr_str, &status, &err);

  if (!ok) {
    if (err) g_error_free(err);
    g_free(stdout_str);
    g_free(stderr_str);
    return FALSE;
  }

  if (status != 0) {
    g_free(stdout_str);
    g_free(stderr_str);
    return FALSE;
  }

  g_free(stderr_str);
  if (out_stdout) *out_stdout = stdout_str;
  else g_free(stdout_str);
  return TRUE;
}

/* ---------------- time formatting ---------------- */

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

/* ---------------- LiveSpiff D-Bus helpers ---------------- */

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

/* ---------------- selected window label ---------------- */

static void update_picked_label(Ui *ui) {
  if (!ui->picked_label) return;

  if (ui->settings.picked_classname && ui->settings.picked_classname[0]) {
    const char *title = (ui->settings.picked_window_title && ui->settings.picked_window_title[0])
                          ? ui->settings.picked_window_title
                          : "(no title)";
    char *txt = g_strdup_printf("Selected window: %s — %s", ui->settings.picked_classname, title);
    gtk_label_set_text(ui->picked_label, txt);
    g_free(txt);
  } else {
    gtk_label_set_text(ui->picked_label, "Selected window: (none)");
  }
}

/* ---------------- timer tick ---------------- */

static gboolean ui_tick(gpointer user_data) {
  Ui *ui = (Ui*)user_data;

  if (!ui->proxy_ls) {
    gtk_label_set_text(ui->state_label, "Daemon not running");
    gtk_label_set_text(ui->time_label, "--:--:--.---");
    gtk_label_set_text(ui->split_label, "Split: - / -");
    return G_SOURCE_CONTINUE;
  }

  gint64 ms = 0;
  if (ls_call_i64(ui, "ElapsedMs", &ms)) {
    char *t = format_time_ms(ms);
    gtk_label_set_text(ui->time_label, t);
    g_free(t);
  } else {
    gtk_label_set_text(ui->time_label, "--:--:--.---");
  }

  char *state = NULL;
  if (ls_call_str(ui, "State", &state)) {
    gtk_label_set_text(ui->state_label, state);
    g_free(state);
  } else {
    gtk_label_set_text(ui->state_label, "Unknown");
  }

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
  ui->tick_id = g_timeout_add((guint)ui->settings.refresh_ms, ui_tick, ui);
}

/* ---------------- window picker ---------------- */

static void picker_refresh(WindowPickerCtx *ctx);

static void on_picker_refresh_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  WindowPickerCtx *ctx = (WindowPickerCtx*)user_data;
  picker_refresh(ctx);
}

static void pick_apply(WindowPickerCtx *ctx, const char *wid) {
  char *out = NULL;

  char *cmd_class = g_strdup_printf("kdotool getwindowclassname %s", wid);
  char *cmd_name  = g_strdup_printf("kdotool getwindowname %s", wid);
  char *cmd_pid   = g_strdup_printf("kdotool getwindowpid %s", wid);

  char *classname = NULL, *title = NULL;
  gint pid = -1;

  if (run_cmd_capture(cmd_class, &out)) { classname = g_strdup(str_trim(out)); g_free(out); out = NULL; }
  if (run_cmd_capture(cmd_name, &out))  { title = g_strdup(str_trim(out));     g_free(out); out = NULL; }
  if (run_cmd_capture(cmd_pid, &out))   { pid = (gint)g_ascii_strtoll(str_trim(out), NULL, 10); g_free(out); out = NULL; }

  g_free(cmd_class); g_free(cmd_name); g_free(cmd_pid);

  ui_settings_free_fields(&ctx->ui->settings);
  ctx->ui->settings.picked_window_id = g_strdup(wid);
  ctx->ui->settings.picked_classname = classname ? classname : g_strdup("");
  ctx->ui->settings.picked_window_title = title ? title : g_strdup("");
  ctx->ui->settings.picked_pid = pid;

  ui_settings_save(&ctx->ui->settings);
  update_picked_label(ctx->ui);

  gtk_window_destroy(ctx->dlg);
}

static void picker_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
  (void)box;
  WindowPickerCtx *ctx = (WindowPickerCtx*)user_data;
  const char *wid = (const char*)g_object_get_data(G_OBJECT(row), "wid");
  if (wid && wid[0]) pick_apply(ctx, wid);
}

static void picker_clear_list(WindowPickerCtx *ctx) {
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(ctx->list))) != NULL) {
    gtk_list_box_remove(ctx->list, child);
  }
}

static void picker_refresh(WindowPickerCtx *ctx) {
  picker_clear_list(ctx);
  gtk_label_set_text(ctx->status, "Loading windows...");

  char *out = NULL;
  if (!run_cmd_capture("kdotool search \"\"", &out)) {
    gtk_label_set_text(ctx->status, "Failed to run kdotool. Is it installed?");
    return;
  }

  char **lines = g_strsplit(out, "\n", -1);
  g_free(out);

  int count = 0;
  for (int i = 0; lines[i]; i++) {
    char *wid = str_trim(lines[i]);
    if (!wid || !wid[0]) continue;

    // fetch classname & title for display
    char *tmp = NULL;
    char *cmd_class = g_strdup_printf("kdotool getwindowclassname %s", wid);
    char *cmd_name  = g_strdup_printf("kdotool getwindowname %s", wid);

    char *classname = NULL, *title = NULL;

    if (run_cmd_capture(cmd_class, &tmp)) { classname = g_strdup(str_trim(tmp)); g_free(tmp); tmp = NULL; }
    if (run_cmd_capture(cmd_name, &tmp))  { title = g_strdup(str_trim(tmp));     g_free(tmp); tmp = NULL; }

    g_free(cmd_class);
    g_free(cmd_name);

    if (!classname) classname = g_strdup("(unknown)");
    if (!title) title = g_strdup("(no title)");

    char *label = g_strdup_printf("%s — %s", classname, title);

    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *lb = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(lb), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(lb), TRUE);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), lb);

    g_object_set_data_full(G_OBJECT(row), "wid", g_strdup(wid), g_free);

    gtk_list_box_append(ctx->list, row);

    g_free(label);
    g_free(classname);
    g_free(title);
    count++;
  }

  g_strfreev(lines);

  if (count == 0) gtk_label_set_text(ctx->status, "No windows found.");
  else gtk_label_set_text(ctx->status, "Double-click a window to select it.");
}

static void on_select_window_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  Ui *ui = (Ui*)user_data;

  GtkWindow *dlg = GTK_WINDOW(gtk_window_new());
  gtk_window_set_title(dlg, "Select window");
  gtk_window_set_transient_for(dlg, ui->win);
  gtk_window_set_modal(dlg, TRUE);
  gtk_window_set_default_size(dlg, 620, 420);

  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_top(root, 12);
  gtk_widget_set_margin_bottom(root, 12);
  gtk_widget_set_margin_start(root, 12);
  gtk_widget_set_margin_end(root, 12);
  gtk_window_set_child(dlg, root);

  GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append(GTK_BOX(root), top);

  GtkWidget *refresh = gtk_button_new_with_label("Refresh");
  gtk_box_append(GTK_BOX(top), refresh);

  GtkWidget *close = gtk_button_new_with_label("Close");
  gtk_box_append(GTK_BOX(top), close);

  GtkWidget *status = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(status), 0.0f);
  gtk_box_append(GTK_BOX(root), status);

  GtkWidget *sc = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(sc, TRUE);
  gtk_box_append(GTK_BOX(root), sc);

  GtkListBox *list = GTK_LIST_BOX(gtk_list_box_new());
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), GTK_WIDGET(list));
  gtk_list_box_set_selection_mode(list, GTK_SELECTION_NONE);

  WindowPickerCtx *ctx = g_new0(WindowPickerCtx, 1);
  ctx->ui = ui;
  ctx->dlg = dlg;
  ctx->list = list;
  ctx->status = GTK_LABEL(status);

  g_signal_connect(list, "row-activated", G_CALLBACK(picker_row_activated), ctx);
  g_signal_connect(refresh, "clicked", G_CALLBACK(on_picker_refresh_clicked), ctx);
  g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_window_destroy), dlg);

  // Free ctx when dialog is destroyed
  g_signal_connect_data(dlg, "destroy", G_CALLBACK(g_free), ctx, NULL, 0);

  picker_refresh(ctx);

  gtk_window_present(dlg);
}

/* ---------------- settings window ---------------- */

static void on_setting_refresh_changed(GtkSpinButton *sb, gpointer user_data) {
  Ui *ui = (Ui*)user_data;
  gint v = gtk_spin_button_get_value_as_int(sb);
  if (v < 10) v = 10;
  if (v > 1000) v = 1000;
  ui->settings.refresh_ms = v;
  ui_settings_save(&ui->settings);
  restart_tick(ui);
}

static void on_settings_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  Ui *ui = (Ui*)user_data;

  GtkWindow *sw = GTK_WINDOW(gtk_window_new());
  gtk_window_set_title(sw, "Settings");
  gtk_window_set_transient_for(sw, ui->win);
  gtk_window_set_modal(sw, TRUE);
  gtk_window_set_default_size(sw, 520, 260);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 12);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_window_set_child(sw, box);

  GtkWidget *pick_btn = gtk_button_new_with_label("Select window from list");
  g_signal_connect(pick_btn, "clicked", G_CALLBACK(on_select_window_clicked), ui);
  gtk_box_append(GTK_BOX(box), pick_btn);

  GtkWidget *picked = gtk_label_new("");
  gtk_label_set_wrap(GTK_LABEL(picked), TRUE);
  gtk_label_set_xalign(GTK_LABEL(picked), 0.0f);
  gtk_box_append(GTK_BOX(box), picked);

  // show current selection
  GtkLabel *old = ui->picked_label;
  ui->picked_label = GTK_LABEL(picked);
  update_picked_label(ui);
  ui->picked_label = old;

  GtkWidget *lbl = gtk_label_new("Refresh interval (ms)");
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
  gtk_box_append(GTK_BOX(box), lbl);

  GtkWidget *spin = gtk_spin_button_new_with_range(10, 1000, 10);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), ui->settings.refresh_ms);
  g_signal_connect(spin, "value-changed", G_CALLBACK(on_setting_refresh_changed), ui);
  gtk_box_append(GTK_BOX(box), spin);

  GtkWidget *note = gtk_label_new(
    "Overlay note: On KDE Wayland, use Window Rules to force \"Keep Above Others\" for LiveSpiff.\n"
    "Many games must be set to Borderless Fullscreen for overlays to remain visible."
  );
  gtk_label_set_wrap(GTK_LABEL(note), TRUE);
  gtk_label_set_xalign(GTK_LABEL(note), 0.0f);
  gtk_box_append(GTK_BOX(box), note);

  gtk_window_present(sw);
}

/* ---------------- main window build ---------------- */

static void on_start_split_clicked(GtkButton *btn, gpointer user_data) { (void)btn; ls_call_void((Ui*)user_data, "StartOrSplit"); }
static void on_pause_clicked(GtkButton *btn, gpointer user_data) { (void)btn; ls_call_void((Ui*)user_data, "TogglePause"); }
static void on_reset_clicked(GtkButton *btn, gpointer user_data) { (void)btn; ls_call_void((Ui*)user_data, "Reset"); }

static void ui_build(Ui *ui) {
  ui->win = GTK_WINDOW(gtk_application_window_new(ui->app));
  gtk_window_set_title(ui->win, "LiveSpiff Timer");
  gtk_window_set_default_size(ui->win, 560, 280);

  GtkCssProvider *css = gtk_css_provider_new();
  gtk_css_provider_load_from_string(css,
    "label.time { font-size: 48px; font-weight: 700; }"
    "label.meta { font-size: 16px; opacity: 0.85; }"
    "label.small { font-size: 13px; opacity: 0.80; }"
  );
  gtk_style_context_add_provider_for_display(
    gdk_display_get_default(),
    GTK_STYLE_PROVIDER(css),
    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
  );
  g_object_unref(css);

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

  GtkWidget *picked = gtk_label_new("Selected window: (none)");
  gtk_widget_add_css_class(picked, "small");
  gtk_widget_set_halign(picked, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(root), picked);
  ui->picked_label = GTK_LABEL(picked);
  update_picked_label(ui);

  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_halign(row, GTK_ALIGN_CENTER);

  ui->btn_settings    = GTK_BUTTON(gtk_button_new_with_label("Settings"));
  ui->btn_start_split = GTK_BUTTON(gtk_button_new_with_label("Start / Split"));
  ui->btn_pause       = GTK_BUTTON(gtk_button_new_with_label("Pause / Resume"));
  ui->btn_reset       = GTK_BUTTON(gtk_button_new_with_label("Reset"));

  g_signal_connect(ui->btn_settings, "clicked", G_CALLBACK(on_settings_clicked), ui);
  g_signal_connect(ui->btn_start_split, "clicked", G_CALLBACK(on_start_split_clicked), ui);
  g_signal_connect(ui->btn_pause, "clicked", G_CALLBACK(on_pause_clicked), ui);
  g_signal_connect(ui->btn_reset, "clicked", G_CALLBACK(on_reset_clicked), ui);

  gtk_box_append(GTK_BOX(row), GTK_WIDGET(ui->btn_settings));
  gtk_box_append(GTK_BOX(row), GTK_WIDGET(ui->btn_start_split));
  gtk_box_append(GTK_BOX(row), GTK_WIDGET(ui->btn_pause));
  gtk_box_append(GTK_BOX(row), GTK_WIDGET(ui->btn_reset));
  gtk_box_append(GTK_BOX(root), row);
}

/* ---------------- activate ---------------- */

static void on_activate(GtkApplication *app, gpointer user_data) {
  Ui *ui = (Ui*)user_data;
  ui->app = app;

  ui->settings = ui_settings_load();

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

  ui_settings_free_fields(&ui.settings);
  g_object_unref(app);

  return status;
}
