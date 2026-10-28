# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

gp_sources = files(
  'gp.c',
  'segments.c',
)

if host_system == 'windows'
  gp_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'gp',
    '--FILEDESC', 'gp - cluster of PostgreSQL servers',])
endif

gp = shared_module('gp',
  gp_sources,
  kwargs: contrib_mod_args + {
    'dependencies': contrib_mod_args['dependencies'] + [libpq],
  },
)
contrib_targets += gp

install_data(
  'gp.control',
  'gp--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'gp',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'gp',
    ],
    'regress_args': ['--temp-config', files('gp.conf')],
    # Disabled because these tests require "shared_preload_libraries=gp",
    # which typical runningcheck users do not have
    'runningcheck': false,
  },
  'tap': {
    'tests': [
      't/001_cluster.pl',
      't/002_gp_cluster.pl',
      't/003_register.pl',
      't/004_register_worker.pl',
      't/005_cluster_segments.pl',
    ],
  },
}
