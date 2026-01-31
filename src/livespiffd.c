// File: src/livespiffd.c
// LiveSpiff D-Bus timer daemon (Wayland-first, KDE-friendly)
// Service:   com.livespiff.LiveSpiff
// Path:      /com/livespiff/LiveSpiff
// Interface: com.livespiff.LiveSpiff.Control

#include <gio/gio.h>
#include <stdint.h>

#include "storage.h"

#define BUS_NAME   "com.livespiff.LiveSpiff"
#define OBJ_PATH   "/com/livespiff/LiveSpiff"
#define IFACE_NAME "com.livespiff.LiveSpiff.Control"

typedef enum {
  STATE_IDLE = 0,
  STATE_RUNNING,
  STATE_PAUSED,
  STATE_FINISHED
} TimerState;

typedef struct {
  TimerState state;
  gint64 start_monotonic_us;     // g_get_monotonic_time() at start
  gint64 paused_at_us;           // time when paused
  gint64 total_paused_us;        // accumulated paused time
  gint64 paused_elapsed_us;      // snapshot when paused or finished
  int current_split;
  int split_count;
} Timer;

static Timer g_timer = {
  .state = STATE_IDLE,
  .start_monotonic_us = 0,
  .paused_at_us = 0,
  .total_paused_us = 0,
  .paused_elapsed_us = 0,
  .current_split = 0,
  .split_count = 3, // will be updated from run data
};

// Current run (segments, metadata)
static LiveSpiffRun *g_run = NULL;

static const char* state_to_string(TimerState s) {
  switch (s) {
    case STATE_IDLE: return "Idle";
    case STATE_RUNNING: return "Running";
    case STATE_PAUSED: return "Paused";
    case STATE_FINISHED: return "Finished";
    default: return "Unknown";
  }
}

static gint64 timer_elapsed_us(void) {
  if (g_timer.state == STATE_IDLE) return 0;

  if (g_timer.state == STATE_PAUSED) return g_timer.paused_elapsed_us;

  if (g_timer.state == STATE_FINISHED) {
    return g_timer.paused_elapsed_us > 0 ? g_timer.paused_elapsed_us : 0;
  }

  // Running
  gint64 now = g_get_monotonic_time();
  gint64 raw = now - g_timer.start_monotonic_us;
  gint64 adj = raw - g_timer.total_paused_us;
  if (adj < 0) adj = 0;
  return adj;
}

static void timer_start(void) {
  if (g_timer.state != STATE_IDLE) return;

  g_timer.start_monotonic_us = g_get_monotonic_time();
  g_timer.total_paused_us = 0;
  g_timer.paused_elapsed_us = 0;
  g_timer.paused_at_us = 0;
  g_timer.current_split = 0;
  g_timer.state = STATE_RUNNING;
}

static void timer_split(void) {
  if (g_timer.state != STATE_RUNNING) return;

  g_timer.current_split++;
  if (g_timer.current_split >= g_timer.split_count) {
    // Mark finished
    g_timer.paused_elapsed_us = timer_elapsed_us(); // snapshot final time
    g_timer.state = STATE_FINISHED;
  }
}

static void timer_start_or_split(void) {
  if (g_timer.state == STATE_IDLE) timer_start();
  else if (g_timer.state == STATE_RUNNING) timer_split();
}

static void timer_toggle_pause(void) {
  if (g_timer.state == STATE_RUNNING) {
    g_timer.paused_elapsed_us = timer_elapsed_us();
    g_timer.paused_at_us = g_get_monotonic_time();
    g_timer.state = STATE_PAUSED;
  } else if (g_timer.state == STATE_PAUSED) {
    gint64 now = g_get_monotonic_time();
    g_timer.total_paused_us += (now - g_timer.paused_at_us);
    g_timer.paused_at_us = 0;
    g_timer.state = STATE_RUNNING;
  }
}

static void timer_reset(void) {
  g_timer.state = STATE_IDLE;
  g_timer.start_monotonic_us = 0;
  g_timer.paused_at_us = 0;
  g_timer.total_paused_us = 0;
  g_timer.paused_elapsed_us = 0;
  g_timer.current_split = 0;
}

// Apply run data (segments length) to timer
static void apply_run_to_timer(void) {
  if (!g_run) return;
  g_timer.split_count = (int)g_run->segments->len;
  if (g_timer.split_count < 1) g_timer.split_count = 1;
  if (g_timer.current_split > g_timer.split_count) g_timer.current_split = 0;
}

static const gchar introspection_xml[] =
  "<node>"
  "  <interface name='com.livespiff.LiveSpiff.Control'>"
  "    <method name='StartOrSplit'/>"
  "    <method name='TogglePause'/>"
  "    <method name='Reset'/>"
  "    <method name='ElapsedMs'>"
  "      <arg type='x' name='ms' direction='out'/>"
  "    </method>"
  "    <method name='State'>"
  "      <arg type='s' name='state' direction='out'/>"
  "    </method>"
  "    <method name='CurrentSplit'>"
  "      <arg type='i' name='index' direction='out'/>"
  "    </method>"
  "    <method name='SplitCount'>"
  "      <arg type='i' name='count' direction='out'/>"
  "    </method>"
  "    <method name='LoadRun'>"
  "      <arg type='s' name='path' direction='in'/>"
  "      <arg type='b' name='ok' direction='out'/>"
  "      <arg type='s' name='message' direction='out'/>"
  "    </method>"
  "    <method name='SaveRun'>"
  "      <arg type='s' name='path' direction='in'/>"
  "      <arg type='b' name='ok' direction='out'/>"
  "      <arg type='s' name='message' direction='out'/>"
  "    </method>"
  "    <method name='GetRunJson'>"
  "      <arg type='s' name='json' direction='out'/>"
  "    </method>"
  "  </interface>"
  "</node>";

static GDBusNodeInfo *introspection_data = NULL;

static void on_method_call(GDBusConnection *connection,
                           const gchar *sender,
                           const gchar *object_path,
                           const gchar *interface_name,
                           const gchar *method_name,
                           GVariant *parameters,
                           GDBusMethodInvocation *invocation,
                           gpointer user_data) {
  (void)connection; (void)sender; (void)object_path; (void)interface_name; (void)user_data;

  // Timer controls
  if (g_strcmp0(method_name, "StartOrSplit") == 0) {
    timer_start_or_split();
    g_dbus_method_invocation_return_value(invocation, NULL);
    return;
  }
  if (g_strcmp0(method_name, "TogglePause") == 0) {
    timer_toggle_pause();
    g_dbus_method_invocation_return_value(invocation, NULL);
    return;
  }
  if (g_strcmp0(method_name, "Reset") == 0) {
    timer_reset();
    g_dbus_method_invocation_return_value(invocation, NULL);
    return;
  }

  // Queries
  if (g_strcmp0(method_name, "ElapsedMs") == 0) {
    gint64 ms = timer_elapsed_us() / 1000;
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(x)", ms));
    return;
  }
  if (g_strcmp0(method_name, "State") == 0) {
    const char *s = state_to_string(g_timer.state);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", s));
    return;
  }
  if (g_strcmp0(method_name, "CurrentSplit") == 0) {
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(i)", (gint32)g_timer.current_split));
    return;
  }
  if (g_strcmp0(method_name, "SplitCount") == 0) {
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(i)", (gint32)g_timer.split_count));
    return;
  }

  // Run save/load
  if (g_strcmp0(method_name, "LoadRun") == 0) {
    const char *path = NULL;
    g_variant_get(parameters, "(&s)", &path);

    LiveSpiffRun *loaded = NULL;
    char *err_str = NULL;

    gboolean ok = run_load_json(path, &loaded, &err_str);
    if (ok) {
      run_free(g_run);
      g_run = loaded;
      apply_run_to_timer();
      timer_reset();
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(bs)", TRUE, "Run loaded"));
    } else {
      const char *msg = err_str ? err_str : "Failed to load run";
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(bs)", FALSE, msg));
      g_free(err_str);
    }
    return;
  }

  if (g_strcmp0(method_name, "SaveRun") == 0) {
    const char *path = NULL;
    g_variant_get(parameters, "(&s)", &path);

    if (!g_run) g_run = run_new_default();

    char *err_str = NULL;
    gboolean ok = run_save_json(path, g_run, &err_str);
    if (ok) {
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(bs)", TRUE, "Run saved"));
    } else {
      const char *msg = err_str ? err_str : "Failed to save run";
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(bs)", FALSE, msg));
      g_free(err_str);
    }
    return;
  }

  if (g_strcmp0(method_name, "GetRunJson") == 0) {
    if (!g_run) g_run = run_new_default();
    char *json = run_to_json_string(g_run);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", json ? json : "{}"));
    g_free(json);
    return;
  }

  // Unknown method
  g_dbus_method_invocation_return_dbus_error(
    invocation,
    "com.livespiff.LiveSpiff.Error.UnknownMethod",
    "Unknown method"
  );
}

static const GDBusInterfaceVTable interface_vtable = {
  .method_call = on_method_call,
  .get_property = NULL,
  .set_property = NULL
};

static void on_bus_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data) {
  (void)name; (void)user_data;

  guint reg_id = g_dbus_connection_register_object(
    connection,
    OBJ_PATH,
    introspection_data->interfaces[0],
    &interface_vtable,
    NULL, NULL, NULL
  );

  if (reg_id == 0) {
    g_printerr("Failed to register object on D-Bus.\n");
    exit(1);
  }

  g_print("LiveSpiff D-Bus service online: %s %s %s\n", BUS_NAME, OBJ_PATH, IFACE_NAME);
}

static void on_name_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data) {
  (void)connection; (void)name; (void)user_data;
}

static void on_name_lost(GDBusConnection *connection, const gchar *name, gpointer user_data) {
  (void)connection; (void)name; (void)user_data;
  g_printerr("Lost D-Bus name '%s'. Is another LiveSpiff running?\n", BUS_NAME);
  exit(1);
}

int main(void) {
  GMainLoop *loop = NULL;

  introspection_data = g_dbus_node_info_new_for_xml(introspection_xml, NULL);
  if (!introspection_data) {
    g_printerr("Failed to parse introspection XML.\n");
    return 1;
  }

  // Initialize default run and apply its segment count
  g_run = run_new_default();
  apply_run_to_timer();

  guint owner_id = g_bus_own_name(
    G_BUS_TYPE_SESSION,
    BUS_NAME,
    G_BUS_NAME_OWNER_FLAGS_NONE,
    on_bus_acquired,
    on_name_acquired,
    on_name_lost,
    NULL, NULL
  );

  loop = g_main_loop_new(NULL, FALSE);
  g_main_loop_run(loop);

  // Cleanup (not reached unless loop quits)
  g_bus_unown_name(owner_id);
  g_main_loop_unref(loop);
  run_free(g_run);
  g_dbus_node_info_unref(introspection_data);
  return 0;
}
