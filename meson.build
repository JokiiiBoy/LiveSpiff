project(
  'livespiff',
  'c',
  version : '0.1.1',
  default_options : ['c_std=c11']
)

# Dependencies
glib_dep = dependency('glib-2.0', version: '>=2.64')
gio_dep  = dependency('gio-2.0')
json_dep = dependency('json-glib-1.0')
gtk_dep  = dependency('gtk4')

# LiveSpiff daemon (D-Bus backend)
executable(
  'livespiffd',
  sources : [
    'src/livespiffd.c',
    'src/storage.c'
  ],
  dependencies : [
    glib_dep,
    gio_dep,
    json_dep
  ],
  install : true
)

# LiveSpiff GUI (GTK4 frontend)
executable(
  'livespiff',
  sources : [
    'src/livespiff-ui.c',
    'src/ui_settings.c'
  ],
  dependencies : [
    gtk_dep,
    gio_dep,
    glib_dep
  ],
  install : true
)
