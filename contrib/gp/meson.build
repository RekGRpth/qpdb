# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

gp_sources = files(
  'copy.c',
  'ddl.c',
  'dispatch.c',
  'distribution.c',
  'gather.c',
  'gp.c',
  'insert.c',
  'interconnect.c',
  'motion.c',
  'oids.c',
  'recovery.c',
  'route.c',
  'segments.c',
  'slice.c',
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
    'env': {
      'enable_injection_points': get_option('injection_points') ? 'yes' : 'no',
    },
    'tests': [
      't/001_cluster.pl',
      't/002_gp_cluster.pl',
      't/003_register.pl',
      't/004_register_worker.pl',
      't/005_cluster_segments.pl',
      't/006_exec_on_all.pl',
      't/007_session_settings.pl',
      't/008_distribution.pl',
      't/009_same_oids.pl',
      't/010_ddl.pl',
      't/011_two_phase.pl',
      't/012_roles.pl',
      't/013_nonxact.pl',
      't/014_extension.pl',
      't/015_copy.pl',
      't/016_insert.pl',
      't/017_direct_dispatch.pl',
      't/018_gather.pl',
      't/019_default_distribution.pl',
      't/020_dtx_snapshot.pl',
      't/021_one_phase.pl',
      't/022_unique_key.pl',
      't/023_update_delete.pl',
      't/024_recovery.pl',
      't/025_agg_pushdown.pl',
      't/026_join_pushdown.pl',
      't/027_subquery_pushdown.pl',
      't/028_partial_agg.pl',
      't/029_redistributed_join.pl',
      't/030_interconnect_port.pl',
      't/031_interconnect_cancel.pl',
      't/032_gp_manifest.pl',
      't/033_multiple_slices.pl',
      't/034_union_locus.pl',
      't/035_window_locus.pl',
      't/036_cte_locus.pl',
    ],
  },
}
