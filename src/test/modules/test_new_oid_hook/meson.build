# Copyright (c) 2026, PostgreSQL Global Development Group

test_new_oid_hook_sources = files(
  'test_new_oid_hook.c',
)

if host_system == 'windows'
  test_new_oid_hook_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_new_oid_hook',
    '--FILEDESC', 'test_new_oid_hook - test code for new_oid_hook',])
endif

test_new_oid_hook = shared_module('test_new_oid_hook',
  test_new_oid_hook_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_new_oid_hook

test_install_data += files(
  'test_new_oid_hook.control',
  'test_new_oid_hook--1.0.sql',
)

tests += {
  'name': 'test_new_oid_hook',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_new_oid_hook',
    ],
  },
}
