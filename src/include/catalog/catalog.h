/*-------------------------------------------------------------------------
 *
 * catalog.h
 *	  prototypes for functions in backend/catalog/catalog.c
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/catalog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CATALOG_H
#define CATALOG_H

#include "catalog/pg_class.h"
#include "utils/relcache.h"


extern bool IsSystemRelation(Relation relation);
extern bool IsToastRelation(Relation relation);
extern bool IsCatalogRelation(Relation relation);
extern bool IsInplaceUpdateRelation(Relation relation);

extern bool IsSystemClass(Oid relid, Form_pg_class reltuple);
extern bool IsToastClass(Form_pg_class reltuple);
extern bool IsConflictLogTableClass(Form_pg_class reltuple);

extern bool IsCatalogRelationOid(Oid relid);
extern bool IsCatalogTextUniqueIndexOid(Oid relid);
extern bool IsInplaceUpdateOid(Oid relid);

extern bool IsCatalogNamespace(Oid namespaceId);
extern bool IsToastNamespace(Oid namespaceId);
extern bool IsConflictLogTableNamespace(Oid namespaceId);

extern bool IsReservedName(const char *name);

extern bool IsSharedRelation(Oid relationId);

extern bool IsPinnedObject(Oid classId, Oid objectId);

extern Oid	GetNewOidWithIndex(Relation relation, Oid indexId,
							   AttrNumber oidcolumn);
extern RelFileNumber GetNewRelFileNumber(Oid reltablespace,
										 Relation pg_class,
										 char relpersistence);
extern bool NewOidNeedsRelFileNumber(void);

/*
 * Hook for an extension to propose the OID of a new catalog row, see
 * GetNewOidWithIndex().  Returning InvalidOid leaves the choice to the
 * server.  NEW_OID_HOOK_VERSION tells extensions that the hook exists.
 */
#define NEW_OID_HOOK_VERSION 1

typedef Oid (*new_oid_hook_type) (Relation relation, Oid indexId,
								  AttrNumber oidcolumn);
extern PGDLLIMPORT new_oid_hook_type new_oid_hook;

#endif							/* CATALOG_H */
