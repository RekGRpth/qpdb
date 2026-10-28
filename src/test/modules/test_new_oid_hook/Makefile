# src/test/modules/test_new_oid_hook/Makefile

MODULES = test_new_oid_hook
PGFILEDESC = "test_new_oid_hook - test code for new_oid_hook"

EXTENSION = test_new_oid_hook
DATA = test_new_oid_hook--1.0.sql

REGRESS = test_new_oid_hook

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_new_oid_hook
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
