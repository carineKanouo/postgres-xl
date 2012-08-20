/*-------------------------------------------------------------------------
 *
 * sequence.h
 *	  prototypes for sequence.c.
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/sequence.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SEQUENCE_H
#define SEQUENCE_H

#include "access/xlog.h"
#include "fmgr.h"
#include "nodes/parsenodes.h"
#include "storage/relfilenode.h"

#ifdef PGXC
#include "utils/relcache.h"
#include "gtm/gtm_c.h"
#include "access/xact.h"
#endif

typedef struct FormData_pg_sequence
{
	NameData	sequence_name;
	int64		last_value;
	int64		start_value;
	int64		increment_by;
	int64		max_value;
	int64		min_value;
	int64		cache_value;
	int64		log_cnt;
	bool		is_cycled;
	bool		is_called;
} FormData_pg_sequence;

typedef FormData_pg_sequence *Form_pg_sequence;

/*
 * Columns of a sequence relation
 */

#define SEQ_COL_NAME			1
#define SEQ_COL_LASTVAL			2
#define SEQ_COL_STARTVAL		3
#define SEQ_COL_INCBY			4
#define SEQ_COL_MAXVALUE		5
#define SEQ_COL_MINVALUE		6
#define SEQ_COL_CACHE			7
#define SEQ_COL_LOG				8
#define SEQ_COL_CYCLE			9
#define SEQ_COL_CALLED			10

#define SEQ_COL_FIRSTCOL		SEQ_COL_NAME
#define SEQ_COL_LASTCOL			SEQ_COL_CALLED

/* XLOG stuff */
#define XLOG_SEQ_LOG			0x00

typedef struct xl_seq_rec
{
	RelFileNode node;
	/* SEQUENCE TUPLE DATA FOLLOWS AT THE END */
} xl_seq_rec;

extern Datum nextval(PG_FUNCTION_ARGS);
extern Datum nextval_oid(PG_FUNCTION_ARGS);
extern Datum currval_oid(PG_FUNCTION_ARGS);
extern Datum setval_oid(PG_FUNCTION_ARGS);
extern Datum setval3_oid(PG_FUNCTION_ARGS);
extern Datum lastval(PG_FUNCTION_ARGS);

extern Datum pg_sequence_parameters(PG_FUNCTION_ARGS);

extern void DefineSequence(CreateSeqStmt *stmt);
extern void AlterSequence(AlterSeqStmt *stmt);
extern void ResetSequence(Oid seq_relid);

extern void seq_redo(XLogRecPtr lsn, XLogRecord *rptr);
extern void seq_desc(StringInfo buf, uint8 xl_info, char *rec);

#ifdef PGXC
/*
 * List of actions that registered the callback.
 * This is listed here and not in sequence.c because callback can also
 * be registered in dependency.c and tablecmds.c as sequences can be dropped
 * or renamed in cascade.
 */
typedef enum
{
	GTM_CREATE_SEQ,
	GTM_DROP_SEQ
} GTM_SequenceDropType;

/* Sequence callbacks on GTM */
extern void register_sequence_rename_cb(char *oldseqname, char *newseqname);
extern void rename_sequence_cb(GTMEvent event, void *args);
extern void register_sequence_cb(char *seqname, GTM_SequenceKeyType key, GTM_SequenceDropType type);
extern void drop_sequence_cb(GTMEvent event, void *args);

extern bool IsTempSequence(Oid relid);
extern char *GetGlobalSeqName(Relation rel, const char *new_seqname, const char *new_schemaname);
#endif

#endif   /* SEQUENCE_H */
