/*-------------------------------------------------------------------------
 *
 * locator.c
 *		Functions that help manage table location information such as
 * partitioning and replication information.
 *
 *
 * PGXCTODO - do not use a single mappingTable for all
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 2010-2011 Nippon Telegraph and Telephone Corporation
 *
 *
 * IDENTIFICATION
 *		$$
 *
 *-------------------------------------------------------------------------
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "postgres.h"
#include "access/skey.h"
#include "access/gtm.h"
#include "access/relscan.h"
#include "catalog/indexing.h"
#include "catalog/pg_type.h"
#include "nodes/pg_list.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/tqual.h"
#include "pgxc/poolmgr.h"
#include "pgxc/locator.h"

#include "catalog/pgxc_class.h"
#include "catalog/namespace.h"
#include "access/hash.h"

/*
 * PGXCTODO For prototype, relations use the same hash mapping table.
 * Long term, make it a pointer in RelationLocInfo, and have
 * similarly handled tables point to the same mapping table,
 * to check faster for equivalency
 */
int			mappingTable[HASH_SIZE];

bool		locatorInited = false;


/* GUC parameter */
char	   *PreferredDataNodes = NULL;
int			primary_data_node = 1;

/* Local functions */
static List *get_preferred_node_list(void);
static void init_mapping_table(int nodeCount, int mapTable[]);


#ifdef XCP
static int locate_replicated_all(Locator *self, Datum value, bool isnull,
					  int *nodes, int *primarynode);
static int locate_replicated_one(Locator *self, Datum value, bool isnull,
					  int *nodes, int *primarynode);
static int locate_roundrobin_insert(Locator *self, Datum value, bool isnull,
					     int *nodes, int *primarynode);
static int locate_roundrobin_select(Locator *self, Datum value, bool isnull,
					     int *nodes, int *primarynode);
static int locate_hash_insert(Locator *self, Datum value, bool isnull,
						int *nodes, int *primarynode);
static int locate_hash_select(Locator *self, Datum value, bool isnull,
						int *nodes, int *primarynode);
static int locate_modulo_insert(Locator *self, Datum value, bool isnull,
						  int *nodes, int *primarynode);
static int locate_modulo_select(Locator *self, Datum value, bool isnull,
						  int *nodes, int *primarynode);
#endif

/*
 * init_mapping_table - initializes a mapping table
 *
 * PGXCTODO
 * For the prototype, all partitioned tables will use the same partition map.
 * We cannot assume this long term
 */
static void
init_mapping_table(int nodeCount, int mapTable[])
{
	int			i;

	for (i = 0; i < HASH_SIZE; i++)
	{
		mapTable[i] = (i % nodeCount) + 1;
	}
}

/*
 * get_preferred_node_list
 *
 * Build list of prefered Datanodes
 * from string preferred_data_nodes (GUC parameter).
 * This is used to identify nodes that should be used when
 * performing a read operation on replicated tables.
 * Result needs to be freed.
 */
static List *
get_preferred_node_list(void)
{
	List *rawlist;
	List *result = NIL;
	char *rawstring = pstrdup(PreferredDataNodes);
	ListCell *cell;

	if (!SplitIdentifierString(rawstring, ',', &rawlist))
	{
		/* Syntax error in string parameter */
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid list syntax for \"preferred_data_nodes\"")));
	}

	/* Finish list conversion */
	foreach(cell, rawlist)
	{
		int nodenum = atoi(lfirst(cell));
		result = lappend_int(result, nodenum);
	}

	pfree(rawstring);
	list_free(rawlist);
	return result;
}


#ifdef XCP
/*
 * Returns preferred data nodes as a list of integers from 1 to NumDataNodes
 * or NIL if none is defined
 */
List *
GetPreferredDataNodes(void)
{
	if (PreferredDataNodes && !globalPreferredNodes)
	{
		char	   *rawstring;
		List	   *elemlist;
		ListCell   *l;

		/* Get writeable copy */
		rawstring = pstrdup(PreferredDataNodes);

		/* Parse string into list of identifiers */
		if (!SplitIdentifierString(rawstring, ',', &elemlist))
		{
			/* syntax error in list */
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid list syntax for \"data_node_ports\"")));
		}

		/* Store entries */
		foreach(l, elemlist)
		{
			char	   *curnode = (char *) lfirst(l);
			int 		nnode = atoi(curnode);

			/* Silently skip invalid values */
			if (nnode > 0 && nnode <= NumDataNodes)
				globalPreferredNodes = lappend_int(globalPreferredNodes, nnode);
		}
		list_free_deep(elemlist);
		pfree(rawstring);
	}
	return globalPreferredNodes;
}
#endif


/*
 * GetAnyDataNode
 *
 * Pick any data node, but try a preferred node
 */
List *
GetAnyDataNode(void)
{
	List		*destList = NULL;
	List		*globalPreferredNodes = get_preferred_node_list();

	/* try and pick from the preferred list */
	if (globalPreferredNodes != NULL)
		return destList = lappend_int(NULL, linitial_int(globalPreferredNodes));

	list_free(globalPreferredNodes);

	return destList = lappend_int(NULL, 1);
}


/*
 * hash_range - hash the key to a value between 0 and HASH_SIZE
 *
 * Note, this function corresponds to GridSQL hashing
 * and is used here to allow us the wire up GridSQL
 * to the same underlying nodes
 */
static int
hash_range(char *key)
{
	int			i;
	int			length;
	int			value;

	if (key == NULL || key == '\0')
	{
		return 0;
	}

	length = strlen(key);

	value = 0x238F13AF * length;

	for (i = 0; i < length; i++)
	{
		value = value + ((key[i] << i * 5 % 24) & 0x7fffffff);
	}

	return (1103515243 * value + 12345) % 65537 & HASH_MASK;
}

/*
 * hash_range_int - hashes the integer key to a value between 0 and HASH_SIZE
 *
 * See hash_range
 */
static int
hash_range_int(int intkey)
{
	char		int_str[13];	/* plenty for 32 bit int */

	int_str[12] = '\0';
	snprintf(int_str, 12, "%d", intkey);

	return hash_range(int_str);
}


/*
 * get_node_from_hash - determine node based on hash bucket
 *
 */
static int
get_node_from_hash(int hash)
{
	if (hash > HASH_SIZE || hash < 0)
		ereport(ERROR, (errmsg("Hash value out of range\n")));

	return mappingTable[hash];
}

/*
 * compute_modulo
 */
static int
compute_modulo(int valueOfPartCol)
{
	return ((abs(valueOfPartCol)) % NumDataNodes)+1;
}

/*
 * get_node_from_modulo - determine node based on modulo
 *
 */
static int
get_node_from_modulo(int modulo)
{
	if (modulo > NumDataNodes || modulo <= 0)
		ereport(ERROR, (errmsg("Modulo value out of range\n")));

	return modulo;
}

/*
 * GetRelationDistColumn - Returns the name of the hash or modulo distribution column
 * First hash distribution is checked
 * Retuens NULL if the table is neither hash nor modulo distributed
 */
char *
GetRelationDistColumn(RelationLocInfo * rel_loc_info)
{
char *pColName;

	pColName = NULL;

	pColName = GetRelationHashColumn(rel_loc_info);
	if (pColName == NULL)
		pColName = GetRelationModuloColumn(rel_loc_info);

	return pColName;
}

/*
 * Returns whether or not the data type is hash distributable with PG-XC
 * PGXCTODO - expand support for other data types!
 */
bool
IsHashDistributable(Oid col_type)
{
	if(col_type == INT8OID
	|| col_type == INT2OID
	|| col_type == OIDOID
	|| col_type == INT4OID
	|| col_type == BOOLOID
	|| col_type == CHAROID
	|| col_type == NAMEOID
	|| col_type == INT2VECTOROID
	|| col_type == TEXTOID
	|| col_type == OIDVECTOROID
	|| col_type == FLOAT4OID
	|| col_type == FLOAT8OID
	|| col_type == ABSTIMEOID
	|| col_type == RELTIMEOID
	|| col_type == CASHOID
	|| col_type == BPCHAROID
	|| col_type == BYTEAOID
	|| col_type == VARCHAROID
	|| col_type == DATEOID
	|| col_type == TIMEOID
	|| col_type == TIMESTAMPOID
	|| col_type == TIMESTAMPTZOID
	|| col_type == INTERVALOID
	|| col_type == TIMETZOID
	|| col_type == NUMERICOID
	)
		return true;

	return false;
}

/*
 * GetRelationHashColumn - return hash column for relation.
 *
 * Returns NULL if the relation is not hash partitioned.
 */
char *
GetRelationHashColumn(RelationLocInfo * rel_loc_info)
{
	char	   *column_str = NULL;

	if (rel_loc_info == NULL)
		column_str = NULL;
	else if (rel_loc_info->locatorType != LOCATOR_TYPE_HASH)
		column_str = NULL;
	else
	{
		int			len = strlen(rel_loc_info->partAttrName);

		column_str = (char *) palloc(len + 1);
		strncpy(column_str, rel_loc_info->partAttrName, len + 1);
	}

	return column_str;
}

/*
 * IsHashColumn - return whether or not column for relation is hashed.
 *
 */
bool
IsHashColumn(RelationLocInfo *rel_loc_info, char *part_col_name)
{
	bool		ret_value = false;

	if (!rel_loc_info || !part_col_name)
		ret_value = false;
	else if (rel_loc_info->locatorType != LOCATOR_TYPE_HASH)
		ret_value = false;
	else
		ret_value = !strcmp(part_col_name, rel_loc_info->partAttrName);

	return ret_value;
}


/*
 * IsHashColumnForRelId - return whether or not column for relation is hashed.
 *
 */
bool
IsHashColumnForRelId(Oid relid, char *part_col_name)
{
	RelationLocInfo *rel_loc_info = GetRelationLocInfo(relid);

	return IsHashColumn(rel_loc_info, part_col_name);
}

/*
 * IsDistColumnForRelId - return whether or not column for relation is used for hash or modulo distribution
 *
 */
bool
IsDistColumnForRelId(Oid relid, char *part_col_name)
{
bool bRet;
RelationLocInfo *rel_loc_info;

	rel_loc_info = GetRelationLocInfo(relid);
	bRet = false;

	bRet = IsHashColumn(rel_loc_info, part_col_name);
	if (bRet == false)
		IsModuloColumn(rel_loc_info, part_col_name);
	return bRet;
}


/*
 * Returns whether or not the data type is modulo distributable with PG-XC
 * PGXCTODO - expand support for other data types!
 */
bool
IsModuloDistributable(Oid col_type)
{
	if(col_type == INT8OID
	|| col_type == INT2OID
	|| col_type == OIDOID
	|| col_type == INT4OID
	|| col_type == BOOLOID
	|| col_type == CHAROID
	|| col_type == NAMEOID
	|| col_type == INT2VECTOROID
	|| col_type == TEXTOID
	|| col_type == OIDVECTOROID
	|| col_type == FLOAT4OID
	|| col_type == FLOAT8OID
	|| col_type == ABSTIMEOID
	|| col_type == RELTIMEOID
	|| col_type == CASHOID
	|| col_type == BPCHAROID
	|| col_type == BYTEAOID
	|| col_type == VARCHAROID
	|| col_type == DATEOID
	|| col_type == TIMEOID
	|| col_type == TIMESTAMPOID
	|| col_type == TIMESTAMPTZOID
	|| col_type == INTERVALOID
	|| col_type == TIMETZOID
	|| col_type == NUMERICOID
	)
		return true;

	return false;
}

/*
 * GetRelationModuloColumn - return modulo column for relation.
 *
 * Returns NULL if the relation is not modulo partitioned.
 */
char *
GetRelationModuloColumn(RelationLocInfo * rel_loc_info)
{
	char	   *column_str = NULL;

	if (rel_loc_info == NULL)
		column_str = NULL;
	else if (rel_loc_info->locatorType != LOCATOR_TYPE_MODULO)
		column_str = NULL;
	else
	{
		int	len = strlen(rel_loc_info->partAttrName);

		column_str = (char *) palloc(len + 1);
		strncpy(column_str, rel_loc_info->partAttrName, len + 1);
	}

	return column_str;
}

/*
 * IsModuloColumn - return whether or not column for relation is used for modulo distribution.
 *
 */
bool
IsModuloColumn(RelationLocInfo *rel_loc_info, char *part_col_name)
{
	bool		ret_value = false;

	if (!rel_loc_info || !part_col_name)
		ret_value = false;
	else if (rel_loc_info->locatorType != LOCATOR_TYPE_MODULO)
		ret_value = false;
	else
		ret_value = !strcmp(part_col_name, rel_loc_info->partAttrName);

	return ret_value;
}


/*
 * IsModuloColumnForRelId - return whether or not column for relation is used for modulo distribution.
 *
 */
bool
IsModuloColumnForRelId(Oid relid, char *part_col_name)
{
	RelationLocInfo *rel_loc_info = GetRelationLocInfo(relid);

	return IsModuloColumn(rel_loc_info, part_col_name);
}

/*
 * Update the round robin node for the relation
 *
 * PGXCTODO - may not want to bother with locking here, we could track
 * these in the session memory context instead...
 */
int
GetRoundRobinNode(Oid relid)
{
	int			ret_node;

	Relation	rel = relation_open(relid, AccessShareLock);

#ifdef XCP
    Assert (IsReplicated(rel->rd_locator_info->locatorType) ||
			rel->rd_locator_info->locatorType == LOCATOR_TYPE_RROBIN);
#else
    Assert (rel->rd_locator_info->locatorType == LOCATOR_TYPE_REPLICATED ||
			rel->rd_locator_info->locatorType == LOCATOR_TYPE_RROBIN);
#endif

	ret_node = lfirst_int(rel->rd_locator_info->roundRobinNode);

	/* Move round robin indicator to next node */
	if (rel->rd_locator_info->roundRobinNode->next != NULL)
		rel->rd_locator_info->roundRobinNode = rel->rd_locator_info->roundRobinNode->next;
	else
		/* reset to first one */
		rel->rd_locator_info->roundRobinNode = rel->rd_locator_info->nodeList->head;

	relation_close(rel, AccessShareLock);

	return ret_node;
}


/*
 * GetRelationNodes
 *
 * Get list of relation nodes
 * If the table is replicated and we are reading, we can just pick one.
 * If the table is partitioned, we apply partitioning column value, if possible.
 *
 * If the relation is partitioned, partValue will be applied if present
 * (indicating a value appears for partitioning column), otherwise it
 * is ignored.
 *
 * preferredNodes is only used when for replicated tables. If set, it will
 * use one of the nodes specified if the table is replicated on it.
 * This helps optimize for avoiding introducing additional nodes into the
 * transaction.
 *
 * The returned List is a copy, so it should be freed when finished.
 */
ExecNodes *
GetRelationNodes(RelationLocInfo *rel_loc_info, Datum valueForDistCol, Oid typeOfValueForDistCol, RelationAccessType accessType)
{
	ListCell   *prefItem;
	ListCell   *stepItem;
	ExecNodes *exec_nodes;
	long	hashValue;
	int	nError;

	if (rel_loc_info == NULL)
		return NULL;

	exec_nodes = makeNode(ExecNodes);
	exec_nodes->baselocatortype = rel_loc_info->locatorType;

	switch (rel_loc_info->locatorType)
	{
		case LOCATOR_TYPE_REPLICATED:

			if (accessType == RELATION_ACCESS_UPDATE ||
					accessType == RELATION_ACCESS_INSERT)
			{
				/* we need to write to all synchronously */
				exec_nodes->nodelist = list_copy(rel_loc_info->nodeList);

				/*
				 * Write to primary node first, to reduce chance of a deadlock
				 * on replicated tables. If 0, do not use primary copy.
				 */
				if (primary_data_node && exec_nodes->nodelist
						&& list_length(exec_nodes->nodelist) > 1) /* make sure more than 1 */
				{
					exec_nodes->primarynodelist = lappend_int(NULL, primary_data_node);
					list_delete_int(exec_nodes->nodelist, primary_data_node);
				}
			}
			else
			{
				List *globalPreferredNodes = get_preferred_node_list();

				if (accessType == RELATION_ACCESS_READ_FOR_UPDATE
						&& primary_data_node)
				{
					/*
					 * We should ensure row is locked on the primary node to
					 * avoid distributed deadlock if updating the same row
					 * concurrently
					 */
					exec_nodes->nodelist = lappend_int(NULL, primary_data_node);
				}
				else if (globalPreferredNodes != NULL)
				{
					/* try and pick from the preferred list */
					foreach(prefItem, globalPreferredNodes)
					{
						/* make sure it is valid for this relation */
						foreach(stepItem, rel_loc_info->nodeList)
						{
							if (lfirst_int(stepItem) == lfirst_int(prefItem))
							{
								exec_nodes->nodelist = lappend_int(NULL, lfirst_int(prefItem));
								break;
							}
						}
					}
				}
				list_free(globalPreferredNodes);

				if (exec_nodes->nodelist == NULL)
					/* read from just one of them. Use round robin mechanism */
					exec_nodes->nodelist = lappend_int(NULL, GetRoundRobinNode(rel_loc_info->relid));
			}
			break;

		case LOCATOR_TYPE_HASH:
			hashValue = compute_hash(typeOfValueForDistCol, valueForDistCol, &nError);
			if (nError == 0)
				/* in prototype, all partitioned tables use same map */
				exec_nodes->nodelist = lappend_int(NULL, get_node_from_hash(hash_range_int(hashValue)));
			else
				if (accessType == RELATION_ACCESS_INSERT)
					/* Insert NULL to node 1 */
					exec_nodes->nodelist = lappend_int(NULL, 1);
				else
					/* Use all nodes for other types of access */
					exec_nodes->nodelist = list_copy(rel_loc_info->nodeList);
			break;

		case LOCATOR_TYPE_MODULO:
			hashValue = compute_hash(typeOfValueForDistCol, valueForDistCol, &nError);
			if (nError == 0)
				/* in prototype, all partitioned tables use same map */
				exec_nodes->nodelist = lappend_int(NULL, get_node_from_modulo(compute_modulo(hashValue)));
			else
				if (accessType == RELATION_ACCESS_INSERT)
					/* Insert NULL to node 1 */
					exec_nodes->nodelist = lappend_int(NULL, 1);
				else
					/* Use all nodes for other types of access */
					exec_nodes->nodelist = list_copy(rel_loc_info->nodeList);
			break;

		case LOCATOR_TYPE_SINGLE:

			/* just return first (there should only be one) */
			exec_nodes->nodelist = list_copy(rel_loc_info->nodeList);
			break;

		case LOCATOR_TYPE_RROBIN:

			/* round robin, get next one */
			if (accessType == RELATION_ACCESS_INSERT)
			{
				/* write to just one of them */
				exec_nodes->nodelist = lappend_int(NULL, GetRoundRobinNode(rel_loc_info->relid));
			}
			else
			{
				/* we need to read from all */
				exec_nodes->nodelist = list_copy(rel_loc_info->nodeList);
			}

			break;

			/* PGXCTODO case LOCATOR_TYPE_RANGE: */
			/* PGXCTODO case LOCATOR_TYPE_CUSTOM: */
		default:
			ereport(ERROR, (errmsg("Error: no such supported locator type: %c\n",
								   rel_loc_info->locatorType)));
			break;
	}

	return exec_nodes;
}


/*
 * ConvertToLocatorType
 *		get locator distribution type
 * We really should just have pgxc_class use disttype instead...
 */
char
ConvertToLocatorType(int disttype)
{
	char		loctype;

	switch (disttype)
	{
		case DISTTYPE_HASH:
			loctype = LOCATOR_TYPE_HASH;
			break;
		case DISTTYPE_ROUNDROBIN:
			loctype = LOCATOR_TYPE_RROBIN;
			break;
		case DISTTYPE_REPLICATION:
			loctype = LOCATOR_TYPE_REPLICATED;
			break;
		case DISTTYPE_MODULO:
			loctype = LOCATOR_TYPE_MODULO;
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("Invalid distribution type")));
			break;
	}

	return loctype;
}


/*
 * GetLocatorType - Returns the locator type of the table
 *
 */
char
GetLocatorType(Oid relid)
{
	char		ret = '\0';

	RelationLocInfo *ret_loc_info = GetRelationLocInfo(relid);

	if (ret_loc_info != NULL)
		ret = ret_loc_info->locatorType;

	return ret;
}


/*
 * Return a list of all Datanodes.
 * We assume all tables use all nodes in the prototype, so just return a list
 * from first one.
 */
List *
GetAllDataNodes(void)
{
	int			i;

	/*
	 * PGXCTODO - add support for having nodes on a subset of nodes
	 * For now, assume on all nodes
	 */
	List	   *nodeList = NIL;

	for (i = 1; i < NumDataNodes + 1; i++)
	{
		nodeList = lappend_int(nodeList, i);
	}

	return nodeList;
}

/*
 * Return a list of all Coordinators
 * This is used to send DDL to all nodes and to clean up pooler connections.
 * Do not put in the list the local Coordinator where this function is launched.
 */
List *
GetAllCoordNodes(void)
{
	int			i;

	/*
	 * PGXCTODO - add support for having nodes on a subset of nodes
	 * For now, assume on all nodes
	 */
	List	   *nodeList = NIL;

	for (i = 1; i < NumCoords + 1; i++)
	{
		/*
		 * Do not put in list the Coordinator we are on,
		 * it doesn't make sense to connect to the local coordinator.
		 */
		if (i != PGXCNodeId)
			nodeList = lappend_int(nodeList, i);
	}

	return nodeList;
}


/*
 * Build locator information associated with the specified relation.
 */
void
RelationBuildLocator(Relation rel)
{
	Relation	pcrel;
	ScanKeyData skey;
	SysScanDesc pcscan;
	HeapTuple	htup;
	MemoryContext oldContext;
	RelationLocInfo *relationLocInfo;
	int			i;
	int			offset;
	Form_pgxc_class pgxc_class;


	/** PGXCTODO temporarily use the same mapping table for all
	 * Use all nodes.
	 */
	if (!locatorInited)
	{
		init_mapping_table(NumDataNodes, mappingTable);
		locatorInited = true;
	}

	ScanKeyInit(&skey,
				Anum_pgxc_class_pcrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));

	pcrel = heap_open(PgxcClassRelationId, AccessShareLock);
	pcscan = systable_beginscan(pcrel, PgxcClassPgxcRelIdIndexId, true,
								SnapshotNow, 1, &skey);
	htup = systable_getnext(pcscan);

	if (!HeapTupleIsValid(htup))
	{
		/* Assume local relation only */
		rel->rd_locator_info = NULL;
		systable_endscan(pcscan);
		heap_close(pcrel, AccessShareLock);
		return;
	}

	pgxc_class = (Form_pgxc_class) GETSTRUCT(htup);

	oldContext = MemoryContextSwitchTo(CacheMemoryContext);

	relationLocInfo = (RelationLocInfo *) palloc(sizeof(RelationLocInfo));
	rel->rd_locator_info = relationLocInfo;

	relationLocInfo->relid = RelationGetRelid(rel);
	relationLocInfo->locatorType = pgxc_class->pclocatortype;

	relationLocInfo->partAttrNum = pgxc_class->pcattnum;

	relationLocInfo->partAttrName = get_attname(relationLocInfo->relid,
												pgxc_class->pcattnum);

	/** PGXCTODO - add support for having nodes on a subset of nodes
	 * For now, assume on all nodes
	 */
	relationLocInfo->nodeList = GetAllDataNodes();
	relationLocInfo->nodeCount = relationLocInfo->nodeList->length;

	/*
	 * If the locator type is round robin, we set a node to
	 * use next time. In addition, if it is replicated,
	 * we choose a node to use for balancing reads.
	 */
	if (relationLocInfo->locatorType == LOCATOR_TYPE_RROBIN
#ifdef XCP
		|| IsReplicated(relationLocInfo->locatorType))
#else
		|| relationLocInfo->locatorType == LOCATOR_TYPE_REPLICATED)
#endif
	{
		/*
		 * pick a random one to start with,
		 * since each process will do this independently
		 */
		srand(time(NULL));
		offset = rand() % relationLocInfo->nodeCount + 1;
		relationLocInfo->roundRobinNode = relationLocInfo->nodeList->head;		/* initialize */

		for (i = 0; i < offset && relationLocInfo->roundRobinNode->next != NULL; i++)
		{
			relationLocInfo->roundRobinNode = relationLocInfo->roundRobinNode->next;
		}
	}

	systable_endscan(pcscan);
	heap_close(pcrel, AccessShareLock);

	MemoryContextSwitchTo(oldContext);
}

/*
 * GetLocatorRelationInfo - Returns the locator information for relation,
 * in a copy of the RelationLocatorInfo struct in relcache
 *
 */
RelationLocInfo *
GetRelationLocInfo(Oid relid)
{
	RelationLocInfo *ret_loc_info = NULL;

	Relation	rel = relation_open(relid, AccessShareLock);

	if (rel && rel->rd_locator_info)
		ret_loc_info = CopyRelationLocInfo(rel->rd_locator_info);

	relation_close(rel, AccessShareLock);

	return ret_loc_info;
}

/*
 * Copy the RelationLocInfo struct
 */
RelationLocInfo *
CopyRelationLocInfo(RelationLocInfo * src_info)
{
	RelationLocInfo *dest_info;


	Assert(src_info);

	dest_info = (RelationLocInfo *) palloc0(sizeof(RelationLocInfo));

	dest_info->relid = src_info->relid;
	dest_info->locatorType = src_info->locatorType;
	dest_info->partAttrNum = src_info->partAttrNum;
	if (src_info->partAttrName)
		dest_info->partAttrName = pstrdup(src_info->partAttrName);
	dest_info->nodeCount = src_info->nodeCount;
	if (src_info->nodeList)
		dest_info->nodeList = list_copy(src_info->nodeList);

	/* Note, for round robin, we use the relcache entry */

	return dest_info;
}


/*
 * Free RelationLocInfo struct
 */
void
FreeRelationLocInfo(RelationLocInfo *relationLocInfo)
{
	if (relationLocInfo)
	{
		if (relationLocInfo->partAttrName)
			pfree(relationLocInfo->partAttrName);
		pfree(relationLocInfo);
	}
}


#ifdef XCP
Locator *
createLocator(char locatorType, RelationAccessType accessType,
			  Oid dataType, List *nodeList)
{
	Locator    *locator;
	ListCell   *lc;
	int			i;

	Assert(list_length(nodeList) > 0);
	locator = (Locator *) palloc(sizeof(Locator) +
								 (list_length(nodeList) - 1) * sizeof(int));
	locator->dataType = dataType;
	i = 0;
	foreach(lc, nodeList)
		locator->nodeMap[i++] = lfirst_int(lc);
	locator->nodeCount = list_length(nodeList);
	locator->roundRobinNode = -1;
	switch (locatorType)
	{
		case LOCATOR_TYPE_REPLICATED:
			if (accessType == RELATION_ACCESS_INSERT ||
					accessType == RELATION_ACCESS_UPDATE)
				locator->locateNodes = locate_replicated_all;
			else
				locator->locateNodes = locate_replicated_one;
			break;
		case LOCATOR_TYPE_RROBIN:
			if (accessType == RELATION_ACCESS_INSERT)
				locator->locateNodes = locate_roundrobin_insert;
			else
				locator->locateNodes = locate_roundrobin_select;
			break;
		case LOCATOR_TYPE_HASH:
			if (accessType == RELATION_ACCESS_INSERT)
				locator->locateNodes = locate_hash_insert;
			else
				locator->locateNodes = locate_hash_select;
			break;
		case LOCATOR_TYPE_MODULO:
			if (accessType == RELATION_ACCESS_INSERT)
				locator->locateNodes = locate_modulo_insert;
			else
				locator->locateNodes = locate_modulo_select;
			break;
		default:
			ereport(ERROR, (errmsg("Error: no such supported locator type: %c\n",
								   locatorType)));
	}
	return locator;
}


static int
locate_replicated_all(Locator *self, Datum value, bool isnull,
					  int *nodes, int *primarynode)
{
	if (primarynode)
	{
		*primarynode = primary_data_node;
		if (primary_data_node > 0)
		{
			if (self->roundRobinNode < 0)
			{
				int i, j;
				for (i = 0, j = 0; i < self->nodeCount; i++)
					if (self->nodeMap[i] == primary_data_node)
						self->roundRobinNode = i;
					else
						nodes[j++] = self->nodeMap[i];
				return j;
			}
			else
			{
				if (self->roundRobinNode > 0)
					memcpy(nodes, self->nodeMap,
						   self->roundRobinNode * sizeof(int));
				if (self->roundRobinNode < self->nodeCount - 1)
					memcpy(nodes + self->roundRobinNode,
						   self->nodeMap + self->roundRobinNode + 1,
						   (self->nodeCount - self->roundRobinNode - 1) * sizeof(int));
				return self->nodeCount - 1;
			}
		}
		/* if primary node is not configured fallthru and return all as
		 * ordinary nodes */
	}
	memcpy(nodes, self->nodeMap, self->nodeCount * sizeof(int));
	return self->nodeCount;
}


static int
locate_replicated_one(Locator *self, Datum value, bool isnull,
					  int *nodes, int *primarynode)
{
	if (primarynode)
	{
		*primarynode = primary_data_node;
		if (primary_data_node > 0)
			return 0;
		/* if primary node is not configured fallthru and return one as an
		 * ordinary node */
	}
	if (++self->roundRobinNode >= self->nodeCount)
		self->roundRobinNode = 0;
	*nodes = self->nodeMap[self->roundRobinNode];
	return 1;
}


static int
locate_roundrobin_insert(Locator *self, Datum value, bool isnull,
						 int *nodes, int *primarynode)
{
	if (primarynode)
		*primarynode = 0;
	if (++self->roundRobinNode >= self->nodeCount)
		self->roundRobinNode = 0;
	*nodes = self->nodeMap[self->roundRobinNode];
	return 1;
}


static int
locate_roundrobin_select(Locator *self, Datum value, bool isnull,
						 int *nodes, int *primarynode)
{
	if (primarynode)
		*primarynode = 0;
	memcpy(nodes, self->nodeMap, self->nodeCount * sizeof(int));
	return self->nodeCount;
}


static int
locate_hash_insert(Locator *self, Datum value, bool isnull,
						int *nodes, int *primarynode)
{
	int nErr;
	if (primarynode)
		*primarynode = 0;
	if (isnull)
		*nodes = self->nodeMap[0];
	else
		*nodes = self->nodeMap[hash_range_int(compute_hash(self->dataType, value, &nErr)) % self->nodeCount];
	return 1;
}


static int
locate_hash_select(Locator *self, Datum value, bool isnull,
						int *nodes, int *primarynode)
{
	int nErr;
	if (primarynode)
		*primarynode = 0;
	if (isnull)
	{
		memcpy(nodes, self->nodeMap, self->nodeCount * sizeof(int));
		return self->nodeCount;
	}
	else
	{
		*nodes = self->nodeMap[hash_range_int(compute_hash(self->dataType, value, &nErr)) % self->nodeCount];
		return 1;
	}
}


static int
locate_modulo_insert(Locator *self, Datum value, bool isnull,
						  int *nodes, int *primarynode)
{
	if (primarynode)
		*primarynode = 0;
	if (isnull)
		*nodes = self->nodeMap[0];
	else
		*nodes = self->nodeMap[abs(DatumGetInt32(value)) % self->nodeCount];
	return 1;
}

static int
locate_modulo_select(Locator *self, Datum value, bool isnull,
						  int *nodes, int *primarynode)
{
	if (primarynode)
		*primarynode = 0;
	if (isnull)
	{
		memcpy(nodes, self->nodeMap, self->nodeCount * sizeof(int));
		return self->nodeCount;
	}
	else
	{
		*nodes = self->nodeMap[abs(DatumGetInt32(value)) % self->nodeCount];
		return 1;
	}
}
#endif
