// File: src/livespiff-ui.c
// LiveSpiff minimal GUI (GTK4) talking to livespiffd over D-Bus
// Note: "Always on top" cannot be forced by GTK4 on Wayland.
//       KDE can keep the window above via Window Rules; we store the preference.

#include <gtk/gtk.h>
#include <gio/gio.h>

#include "ui_settings.h"

#define BUS_NAME   "com.livespiff.LiveSpiff"
#define OBJ_PATH   "/com/livespiff/LiveSpiff"
#define IFACE_NAME "com.livespiff.LiveSpiff.Control"

typedef struct {
  GtkApplication *app;
  GtkWindow *win;

  GtkLabel *time_label;
  GtkLabel *state_label;
  GtkLabel *split_label;

  GtkButton *btn_settings;
  GtkButton *btn_start_split;
  GtkButton *btn_pause;
  GtkButton *btn_reset;

  GDBusProxy *proxy;
  guint tick_id;

  LiveSpiffUiSettings settings;
} Ui;

/* ---------------- Time formatting ---------------- */

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

/* ---------------- D-Bus helpers ---------------- */

static gboolean call_i64(Ui *ui, const char *method, gint64 *out_val) {
  if (!ui->proxy) return FALSE;

  GError *err = NULL;
  GVariant *ret = g_dbus_proxy_call_sync(
    ui->proxy,
    method,
    NULL,
    G_DBUS_CALL_FLAGS_NONE,
    200,
    NULL,
    &err
  );

  if (!ret) {
    if (err) g_error_free(err);
    return FALSE;
  }

  gint64 v = 0;
  g_variant_get(ret, "(x)", &v);
  g_variant_unref(ret);

  if (out_val) *out_val = v;
  return TRUE;
}

static gboolean call_i32(Ui *ui, const char *method, gint32 *out_val) {
  if (!ui->proxy) return FALSE;

  GError *err = NULL;
  GVariant *ret = g_dbus_proxy_call_sync(
    ui->proxy,
    method,
    NULL,
    G_DBUS_CALL_FLAGS_NONE,
    200,
    NULL,
    &err
  );

  if (!ret) {
    if (err) g_error_free(err);
    return FALSE;
  }

  gint32 v = 0;
  g_variant_get(ret, "(i)", &v);
  g_variant_unref(ret);

  if (out_val) *out_val = v;
  return TRUE;
}

static gboolean call_str(Ui *ui, const char *method, char **out_str) {
  if (!ui->proxy) return FALSE;

  GError *err = NULL;
  GVariant *ret = g_dbus_proxy_call_sync(
    ui->proxy,
    method,
    NULL,
    G_DBUS_CALL_FLAGS_NONE,
    200,
    NULL,
    &err
  );

  if (!ret) {
    if (err) g_error_free(err);
    return FALSE;
  }

  const char *s = NULL;
  g_variant_get(ret, "(&s)", &s);
  if (out_str) *out_str = g_strdup(s ? s : "");
  g_variant_unref(ret);
  return TRUE;
}

static void call_void(Ui *ui, const char *method) {
  if (!ui->proxy) return;

  GError *err = NULL;
  GVariant *ret = g_dbus_proxy_call_sync(
    ui->proxy,
    method,
    NULL,
    G_DBUS_CALL_FLAGS_NONE,
    200,
    NULL,
    &err
  );

  if (ret) g_variant_unref(ret);
  if (err) g_error_free(err);
}

/* ---------------- UI tick ---------------- */

static gboolean ui_tick(gpointer user_data) {
  Ui *ui = (Ui*)user_data;

  if (!ui->proxy) {
    gtk_label_set_text(ui->state_label, "Daemon not running");
    gtk_label_set_text(ui->time_label, "--:--:--.---");
    gtk_label_set_text(ui->split_label, "Split: - / -");
    return G_SOURCE_CONTINUE;
  }

  // Elapsed
  gint64 ms = 0;
  if (call_i64(ui, "ElapsedMs", &ms)) {
    char *t = format_time_ms(ms);
    gtk_label_set_text(ui->time_label, t);
    g_free(t);
  } else {
    gtk_label_set_text(ui->time_label, "--:--:--.---");
  }

  // State
  char *state = NULL;
  if (call_str(ui, "State", &state)) {
    gtk_label_set_text(ui->state_label, state);
    g_free(state);
  } else {
    gtk_label_set_text(ui->state_label, "Unknown");
  }

  // Splits
  gint32 cur = 0, count = 0;
  if (call_i32(ui, "CurrentSplit", &cur) && call_i32(ui, "SplitCount", &count)) {
    char *s = g_strdup_printf("Split: %d / %d", (int)(cur + 1), (int)count);
    gtk_label_set_text(ui->split_label, s);
    g_free(s);
  } else {
    gtk_label_set_text(ui->split_label, "Split: - / -");
  }

  return G_SOURCE_CONTINUE;
}

/* ---------------- Button callbacks ---------------- */

static void on_start_split_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  Ui *ui = (Ui*)user_data;
  call_void(ui, "StartOrSplit");
}

static void on_pause_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  Ui *ui = (Ui*)user_data;
  call_void(ui, "TogglePause");
}

static void on_reset_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  Ui *ui = (Ui*)user_data;
  call_void(ui, "Reset");
}

/* ---------------- Settings callbacks ---------------- */

static void restart_tick(Ui *ui) {
  if (ui->tick_id) {
    g_source_remove(ui->tick_id);
    ui->tick_id = 0;
  }
  ui->tick_id = g_timeout_add((guint)ui->settings.refresh_ms, ui_tick, ui);
}

static void on_setting_always_on_top_toggled(GtkCheckButton *b, gpointer user_data) {
  Ui *ui = (Ui*)user_data;
  ui->settings.always_on_top = gtk_check_button_get_active(b);
  ui_settings_save(&ui->settings);
}

static void on_setting_refresh_changed(GtkSpinButton *sb, gpointer user_data) {
  Ui *ui = (Ui*)user_data;

  gint v = gtk_spin_button_get_value_as_int(sb);
  if (v < 10) v = 10;
  if (v > 1000) v = 1000;

  ui->settings.refresh_ms = v;
  ui_settings_save(&ui->settings);
  restart_tick(ui);
}

/* ---------------- Settings window (GTK4 way) ---------------- */

static void on_settings_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  Ui *ui = (Ui*)user_data;

  GtkWindow *sw = GTK_WINDOW(gtk_window_new());
  gtk_window_set_title(sw, "Settings");
  gtk_window_set_transient_for(sw, ui->win);
  gtk_window_set_modal(sw, TRUE);
  gtk_window_set_default_size(sw, 360, 180);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 12);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_window_set_child(sw, box);

  // Always on top note
  GtkWidget *chk = gtk_check_button_new_with_label("Always on top (managed by KDE)");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(chk), ui->settings.always_on_top);
  g_signal_connect(chk, "toggled", G_CALLBACK(on_setting_always_on_top_toggled), ui);
  gtk_box_append(GTK_BOX(box), chk);

  GtkWidget *hint = gtk_label_new(
    "Tip: On KDE Wayland, use Window Rules to force \"Keep Above Others\" for LiveSpiff."
  );
  gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
  gtk_widget_set_halign(hint, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(box), hint);

  // Refresh interval
  GtkWidget *lbl = gtk_label_new("Refresh interval (ms)");
  gtk_widget_set_halign(lbl, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(box), lbl);

  GtkWidget *spin = gtk_spin_button_new_with_range(10, 1000, 10);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), ui->settings.refresh_ms);
  g_signal_connect(spin, "value-changed", G_CALLBACK(on_setting_refresh_changed), ui);
  gtk_box_append(GTK_BOX(box), spin);

  gtk_window_present(sw);
}

/* ---------------- UI build ---------------- */

static void ui_build(Ui *ui) {
  ui->win = GTK_WINDOW(gtk_application_window_new(ui->app));
  gtk_window_set_title(ui->win, "LiveSpiff Timer");
  gtk_window_set_default_size(ui->win, 520, 240);

  // CSS (no deprecated call)
  GtkCssProvider *css = gtk_css_provider_new();
  gtk_css_provider_load_from_string(css,
    "label.time { font-size: 48px; font-weight: 700; }"
    "label.meta { font-size: 16px; opacity: 0.85; }"
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

  // Time (big)
  ui->time_label = GTK_LABEL(gtk_label_new("--:--:--.---"));
  gtk_widget_add_css_class(GTK_WIDGET(ui->time_label), "time");
  gtk_widget_set_halign(GTK_WIDGET(ui->time_label), GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(root), GTK_WIDGET(ui->time_label));

  // Meta row
  GtkWidget *meta = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_halign(meta, GTK_ALIGN_CENTER);

  ui->state_label = GTK_LABEL(gtk_label_new("Connecting..."));
  gtk_widget_add_css_class(GTK_WIDGET(ui->state_label), "meta");

  ui->split_label = GTK_LABEL(gtk_label_new("Split: - / -"));
  gtk_widget_add_css_class(GTK_WIDGET(ui->split_label), "meta");

  gtk_box_append(GTK_BOX(meta), GTK_WIDGET(ui->state_label));
  gtk_box_append(GTK_BOX(meta), GTK_WIDGET(ui->split_label));
  gtk_box_append(GTK_BOX(root), meta);

  // Buttons row
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_halign(row, GTK_ALIGN_CENTER);

  ui->btn_settings    = GTK_BUTTON(gtk_button_new_with_label("Settings"));
  ui->btn_start_split = GTK_BUTTON(gtk_button_new_with_label("Start / Split"));
  ui->btn_pause       = GTK_BUTTON(gtk_button_new_with_label("Pause / Resume"));
  ui->btn_reset       = GTK_BUTTON(gtk_button_new_with_label("Reset"));

  g_signal_connect(ui->btn_settings,    "clicked", G_CALLBACK(on_settings_clicked), ui);
  g_signal_connect(ui->btn_start_split, "clicked", G_CALLBACK(on_start_split_clicked), ui);
  g_signal_connect(ui->btn_pause,       "clicked", G_CALLBACK(on_pause_clicked), ui);
  g_signal_connect(ui->btn_reset,       "clicked", G_CALLBACK(on_reset_clicked), ui);

  gtk_box_append(GTK_BOX(row), GTK_WIDGET(ui->btn_settings));
  gtk_box_append(GTK_BOX(row), GTK_WIDGET(ui->btn_start_split));
  gtk_box_append(GTK_BOX(row), GTK_WIDGET(ui->btn_pause));
  gtk_box_append(GTK_BOX(row), GTK_WIDGET(ui->btn_reset));

  gtk_box_append(GTK_BOX(root), row);
}

/* ---------------- App activate ---------------- */

static void on_activate(GtkApplication *app, gpointer user_data) {
  Ui *ui = (Ui*)user_data;
  ui->app = app;

  // Load UI settings
  ui->settings = ui_settings_load();

  // Build window
  ui_build(ui);

  // Connect to daemon via D-Bus
  GError *err = NULL;
  ui->proxy = g_dbus_proxy_new_for_bus_sync(
    G_BUS_TYPE_SESSION,
    G_DBUS_PROXY_FLAGS_NONE,
    NULL,
    BUS_NAME,
    OBJ_PATH,
    IFACE_NAME,
    NULL,
    &err
  );

  if (!ui->proxy) {
    gtk_label_set_text(ui->state_label, "Daemon not running");
    if (err) g_error_free(err);
  }

  // Tick UI
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
  if (ui.proxy) g_object_unref(ui.proxy);
  g_object_unref(app);

  return status;
}
