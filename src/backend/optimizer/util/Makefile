#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for optimizer/util
#
# IDENTIFICATION
#    src/backend/optimizer/util/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/optimizer/util
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	appendinfo.o \
	clauses.o \
	inherit.o \
	joininfo.o \
	orclauses.o \
	paramassign.o \
	pathnode.o \
	placeholder.o \
	plancat.o \
	predtest.o \
	relnode.o \
	restrictinfo.o \
	tlist.o \
	var.o

OBJS += \
	walkers.o

include $(top_srcdir)/src/backend/common.mk
