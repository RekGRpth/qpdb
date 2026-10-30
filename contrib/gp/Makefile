# contrib/gp/Makefile

MODULE_big = gp
OBJS = \
	$(WIN32RES) \
	ddl.o \
	dispatch.o \
	distribution.o \
	gp.o \
	oids.o \
	segments.o

EXTENSION = gp
DATA = gp--1.0.sql
PGFILEDESC = "gp - cluster of PostgreSQL servers"

PG_CPPFLAGS = -I$(libpq_srcdir)
SHLIB_LINK_INTERNAL = $(libpq)
SHLIB_PREREQS = submake-libpq

REGRESS_OPTS = --temp-config $(top_srcdir)/contrib/gp/gp.conf
REGRESS = gp

# Disabled because these tests require "shared_preload_libraries=gp",
# which typical installcheck users do not have
NO_INSTALLCHECK = 1

TAP_TESTS = 1

# Extensions that the TAP tests use
EXTRA_INSTALL = contrib/cube contrib/earthdistance contrib/hstore \
	src/test/modules/injection_points
export enable_injection_points

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/gp
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
