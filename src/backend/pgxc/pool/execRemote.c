/*-------------------------------------------------------------------------
 *
 * execRemote.c
 *
 *	  Functions to execute commands on remote data nodes
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 2010-2012 Nippon Telegraph and Telephone Corporation
 *
 * IDENTIFICATION
 *	  $$
 *
 *
 *-------------------------------------------------------------------------
 */

#include <time.h>
#include "postgres.h"
#include "access/twophase.h"
#include "access/gtm.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "catalog/pgxc_node.h"
#include "commands/prepare.h"
#include "executor/executor.h"
#include "gtm/gtm_c.h"
#include "libpq/libpq.h"
#include "miscadmin.h"
#include "pgxc/execRemote.h"
#ifdef XCP
#include "executor/nodeSubplan.h"
#include "nodes/nodeFuncs.h"
#endif
#include "nodes/nodes.h"
#include "nodes/nodeFuncs.h"
#include "pgxc/nodemgr.h"
#include "pgxc/poolmgr.h"
#include "storage/ipc.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/tuplesort.h"
#include "utils/snapmgr.h"
#include "utils/builtins.h"
#include "pgxc/locator.h"
#include "pgxc/pgxc.h"
#include "parser/parse_type.h"
#include "pgxc/xc_maintenance_mode.h"

/* Enforce the use of two-phase commit when temporary objects are used */
bool EnforceTwoPhaseCommit = true;

#define END_QUERY_TIMEOUT	20
#define DATA_NODE_FETCH_SIZE 1

typedef enum RemoteXactNodeStatus
{
	RXACT_NODE_NONE,					/* Initial state */
	RXACT_NODE_PREPARE_SENT,			/* PREPARE request sent */
	RXACT_NODE_PREPARE_FAILED,		/* PREPARE failed on the node */
	RXACT_NODE_PREPARED,				/* PREARED successfully on the node */
	RXACT_NODE_COMMIT_SENT,			/* COMMIT sent successfully */
	RXACT_NODE_COMMIT_FAILED,		/* failed to COMMIT on the node */
	RXACT_NODE_COMMITTED,			/* COMMITTed successfully on the node */
	RXACT_NODE_ABORT_SENT,			/* ABORT sent successfully */
	RXACT_NODE_ABORT_FAILED,			/* failed to ABORT on the node */
	RXACT_NODE_ABORTED				/* ABORTed successfully on the node */
} RemoteXactNodeStatus;

typedef enum RemoteXactStatus
{
	RXACT_NONE,				/* Initial state */
	RXACT_PREPARE_FAILED,	/* PREPARE failed */
	RXACT_PREPARED,			/* PREPARED succeeded on all nodes */
	RXACT_COMMIT_FAILED,		/* COMMIT failed on all the nodes */
	RXACT_PART_COMMITTED,	/* COMMIT failed on some and succeeded on other nodes */
	RXACT_COMMITTED,			/* COMMIT succeeded on all the nodes */
	RXACT_ABORT_FAILED,		/* ABORT failed on all the nodes */
	RXACT_PART_ABORTED,		/* ABORT failed on some and succeeded on other nodes */
	RXACT_ABORTED			/* ABORT succeeded on all the nodes */
} RemoteXactStatus;

typedef struct RemoteXactState
{
	/* Current status of the remote 2PC */
	RemoteXactStatus		status;

	/*
	 * Information about all the nodes involved in the transaction. We track
	 * the number of writers and readers. The first numWriteRemoteNodes entries
	 * in the remoteNodeHandles and remoteNodeStatus correspond to the writer
	 * connections and rest correspond to the reader connections.
	 */
	int 					numWriteRemoteNodes;
	int 					numReadRemoteNodes;
	int						maxRemoteNodes;
	PGXCNodeHandle			**remoteNodeHandles;
	RemoteXactNodeStatus	*remoteNodeStatus;

	GlobalTransactionId		commitXid;

	bool					preparedLocalNode;

	char					prepareGID[256]; /* GID used for internal 2PC */
} RemoteXactState;

static RemoteXactState remoteXactState;

/*
 * Buffer size does not affect performance significantly, just do not allow
 * connection buffer grows infinitely
 */
#define COPY_BUFFER_SIZE 8192
#define PRIMARY_NODE_WRITEAHEAD 1024 * 1024

/*
 * List of PGXCNodeHandle to track readers and writers involved in the
 * current transaction
 */
static List *XactWriteNodes;
static List *XactReadNodes;
static char *preparedNodes;

#ifndef XCP
/*
 * Flag to track if a temporary object is accessed by the current transaction
 */
static bool temp_object_included = false;
#endif

static int	pgxc_node_begin(int conn_count, PGXCNodeHandle ** connections,
				GlobalTransactionId gxid, bool need_tran_block,
				bool readOnly, char node_type);
static PGXCNodeAllHandles * get_exec_connections(RemoteQueryState *planstate,
					 ExecNodes *exec_nodes,
					 RemoteQueryExecType exec_type);

#ifndef XCP
static void close_node_cursors(PGXCNodeHandle **connections, int conn_count, char *cursor);
#endif

static int pgxc_get_transaction_nodes(PGXCNodeHandle *connections[], int size, bool writeOnly);
static int pgxc_get_connections(PGXCNodeHandle *connections[], int size, List *connlist);

static bool pgxc_start_command_on_connection(PGXCNodeHandle *connection,
					RemoteQueryState *remotestate, Snapshot snapshot);
#ifndef XCP
static TupleTableSlot * RemoteQueryNext(RemoteQueryState *node);
#endif

static char *generate_begin_command(void);
static bool pgxc_node_remote_prepare(char *prepareGID);
static void pgxc_node_remote_commit(void);
static void pgxc_node_remote_abort(void);
static char *pgxc_node_get_nodelist(bool localNode);
#ifdef XCP
static void pgxc_connections_cleanup(ResponseCombiner *combiner);
#endif

#ifndef XCP
static void ExecClearTempObjectIncluded(void);
#endif
static void init_RemoteXactState(bool preparedLocalNode);
static void clear_RemoteXactState(void);

#ifdef XCP
#define REMOVE_CURR_CONN(combiner) \
	if ((combiner)->current_conn < --((combiner)->conn_count)) \
	{ \
		(combiner)->connections[(combiner)->current_conn] = \
				(combiner)->connections[(combiner)->conn_count]; \
	} \
	else \
		(combiner)->current_conn = 0
#endif

#define MAX_STATEMENTS_PER_TRAN 10

/* Variables to collect statistics */
static int	total_transactions = 0;
static int	total_statements = 0;
static int	total_autocommit = 0;
static int	nonautocommit_2pc = 0;
static int	autocommit_2pc = 0;
static int	current_tran_statements = 0;
static int *statements_per_transaction = NULL;
static int *nodes_per_transaction = NULL;

/*
 * statistics collection: count a statement
 */
static void
stat_statement()
{
	total_statements++;
	current_tran_statements++;
}

/*
 * To collect statistics: count a transaction
 */
static void
stat_transaction(int node_count)
{
	total_transactions++;

	if (!statements_per_transaction)
	{
		statements_per_transaction = (int *) malloc((MAX_STATEMENTS_PER_TRAN + 1) * sizeof(int));
		memset(statements_per_transaction, 0, (MAX_STATEMENTS_PER_TRAN + 1) * sizeof(int));
	}
	if (current_tran_statements > MAX_STATEMENTS_PER_TRAN)
		statements_per_transaction[MAX_STATEMENTS_PER_TRAN]++;
	else
		statements_per_transaction[current_tran_statements]++;
	current_tran_statements = 0;
	if (node_count > 0 && node_count <= NumDataNodes)
	{
		if (!nodes_per_transaction)
		{
			nodes_per_transaction = (int *) malloc(NumDataNodes * sizeof(int));
			memset(nodes_per_transaction, 0, NumDataNodes * sizeof(int));
		}
		nodes_per_transaction[node_count - 1]++;
	}
}


#ifdef NOT_USED
/*
 * To collect statistics: count a two-phase commit on nodes
 */
static void
stat_2pc()
{
	if (autocommit)
		autocommit_2pc++;
	else
		nonautocommit_2pc++;
}
#endif


/*
 * Output collected statistics to the log
 */
static void
stat_log()
{
	elog(DEBUG1, "Total Transactions: %d Total Statements: %d", total_transactions, total_statements);
	elog(DEBUG1, "Autocommit: %d 2PC for Autocommit: %d 2PC for non-Autocommit: %d",
		 total_autocommit, autocommit_2pc, nonautocommit_2pc);
	if (total_transactions)
	{
		if (statements_per_transaction)
		{
			int			i;

			for (i = 0; i < MAX_STATEMENTS_PER_TRAN; i++)
				elog(DEBUG1, "%d Statements per Transaction: %d (%d%%)",
					 i, statements_per_transaction[i], statements_per_transaction[i] * 100 / total_transactions);
		}
		elog(DEBUG1, "%d+ Statements per Transaction: %d (%d%%)",
			 MAX_STATEMENTS_PER_TRAN, statements_per_transaction[MAX_STATEMENTS_PER_TRAN], statements_per_transaction[MAX_STATEMENTS_PER_TRAN] * 100 / total_transactions);
		if (nodes_per_transaction)
		{
			int			i;

			for (i = 0; i < NumDataNodes; i++)
				elog(DEBUG1, "%d Nodes per Transaction: %d (%d%%)",
					 i + 1, nodes_per_transaction[i], nodes_per_transaction[i] * 100 / total_transactions);
		}
	}
}


/*
 * Create a structure to store parameters needed to combine responses from
 * multiple connections as well as state information
 */
#ifdef XCP
void
InitResponseCombiner(ResponseCombiner *combiner, int node_count,
					   CombineType combine_type)
#else
static RemoteQueryState *
CreateResponseCombiner(int node_count, CombineType combine_type)
#endif
{
#ifndef XCP
	RemoteQueryState *combiner;

	/* ResponseComber is a typedef for pointer to ResponseCombinerData */
	combiner = makeNode(RemoteQueryState);
	if (combiner == NULL)
	{
		/* Out of memory */
		return combiner;
	}
#endif
	combiner->node_count = node_count;
	combiner->connections = NULL;
	combiner->conn_count = 0;
	combiner->combine_type = combine_type;
	combiner->command_complete_count = 0;
	combiner->request_type = REQUEST_TYPE_NOT_DEFINED;
	combiner->description_count = 0;
	combiner->copy_in_count = 0;
	combiner->copy_out_count = 0;
	combiner->copy_file = NULL;
	combiner->errorMessage = NULL;
	combiner->errorDetail = NULL;
	combiner->tuple_desc = NULL;
#ifdef XCP
	combiner->currentRow = NULL;
#else
	combiner->currentRow.msg = NULL;
	combiner->currentRow.msglen = 0;
	combiner->currentRow.msgnode = 0;
#endif
	combiner->rowBuffer = NIL;
	combiner->tapenodes = NULL;
#ifdef XCP
	combiner->merge_sort = false;
	combiner->extended_query = false;
	combiner->tapemarks = NULL;
	combiner->tuplesortstate = NULL;
	combiner->cursor = NULL;
	combiner->update_cursor = NULL;
	combiner->cursor_count = 0;
	combiner->cursor_connections = NULL;
#else
	combiner->initAggregates = true;
	combiner->query_Done = false;

	return combiner;
#endif
}


/*
 * Parse out row count from the command status response and convert it to integer
 */
static int
parse_row_count(const char *message, size_t len, uint64 *rowcount)
{
	int			digits = 0;
	int			pos;

	*rowcount = 0;
	/* skip \0 string terminator */
	for (pos = 0; pos < len - 1; pos++)
	{
		if (message[pos] >= '0' && message[pos] <= '9')
		{
			*rowcount = *rowcount * 10 + message[pos] - '0';
			digits++;
		}
		else
		{
			*rowcount = 0;
			digits = 0;
		}
	}
	return digits;
}

/*
 * Convert RowDescription message to a TupleDesc
 */
static TupleDesc
create_tuple_desc(char *msg_body, size_t len)
{
	TupleDesc 	result;
	int 		i, nattr;
	uint16		n16;

	/* get number of attributes */
	memcpy(&n16, msg_body, 2);
	nattr = ntohs(n16);
	msg_body += 2;

	result = CreateTemplateTupleDesc(nattr, false);

	/* decode attributes */
	for (i = 1; i <= nattr; i++)
	{
		AttrNumber	attnum;
		char		*attname;
		char		*typname;
		Oid 		oidtypeid;
		int32 		typemode, typmod;

		attnum = (AttrNumber) i;

		/* attribute name */
		attname = msg_body;
		msg_body += strlen(attname) + 1;

		/* type name */
		typname = msg_body;
		msg_body += strlen(typname) + 1;

		/* table OID, ignored */
		msg_body += 4;

		/* column no, ignored */
		msg_body += 2;

		/* data type OID, ignored */
		msg_body += 4;

		/* type len, ignored */
		msg_body += 2;

		/* type mod */
		memcpy(&typemode, msg_body, 4);
		typmod = ntohl(typemode);
		msg_body += 4;

		/* PGXCTODO text/binary flag? */
		msg_body += 2;

		/* Get the OID type and mode type from typename */
		parseTypeString(typname, &oidtypeid, NULL);

		TupleDescInitEntry(result, attnum, attname, oidtypeid, typmod, 0);
	}
	return result;
}

/*
 * Handle CopyOutCommandComplete ('c') message from a data node connection
 */
static void
#ifdef XCP
HandleCopyOutComplete(ResponseCombiner *combiner)
#else
HandleCopyOutComplete(RemoteQueryState *combiner)
#endif
{
#ifdef XCP
	if (combiner->request_type == REQUEST_TYPE_ERROR)
		return;
#endif
	if (combiner->request_type == REQUEST_TYPE_NOT_DEFINED)
		combiner->request_type = REQUEST_TYPE_COPY_OUT;
	if (combiner->request_type != REQUEST_TYPE_COPY_OUT)
		/* Inconsistent responses */
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("Unexpected response from the data nodes for 'c' message, current request type %d", combiner->request_type)));
	/* Just do nothing, close message is managed by the coordinator */
	combiner->copy_out_count++;
}

/*
 * Handle CommandComplete ('C') message from a data node connection
 */
static void
#ifdef XCP
HandleCommandComplete(ResponseCombiner *combiner, char *msg_body, size_t len)
#else
HandleCommandComplete(RemoteQueryState *combiner, char *msg_body, size_t len)
#endif
{
	int 			digits = 0;
	EState		   *estate = combiner->ss.ps.state;

	/*
	 * If we did not receive description we are having rowcount or OK response
	 */
	if (combiner->request_type == REQUEST_TYPE_NOT_DEFINED)
		combiner->request_type = REQUEST_TYPE_COMMAND;
	/* Extract rowcount */
	if (combiner->combine_type != COMBINE_TYPE_NONE && estate)
	{
		uint64	rowcount;
		digits = parse_row_count(msg_body, len, &rowcount);
		if (digits > 0)
		{
			/* Replicated write, make sure they are the same */
			if (combiner->combine_type == COMBINE_TYPE_SAME)
			{
				if (combiner->command_complete_count)
				{
					if (rowcount != estate->es_processed)
						/* There is a consistency issue in the database with the replicated table */
						ereport(ERROR,
								(errcode(ERRCODE_DATA_CORRUPTED),
								 errmsg("Write to replicated table returned different results from the data nodes")));
				}
				else
					/* first result */
					estate->es_processed = rowcount;
			}
			else
				estate->es_processed += rowcount;
		}
		else
			combiner->combine_type = COMBINE_TYPE_NONE;
	}

	combiner->command_complete_count++;
}

/*
 * Handle RowDescription ('T') message from a data node connection
 */
static bool
#ifdef XCP
HandleRowDescription(ResponseCombiner *combiner, char *msg_body, size_t len)
#else
HandleRowDescription(RemoteQueryState *combiner, char *msg_body, size_t len)
#endif
{
#ifdef XCP
	if (combiner->request_type == REQUEST_TYPE_ERROR)
		return false;
#endif
	if (combiner->request_type == REQUEST_TYPE_NOT_DEFINED)
		combiner->request_type = REQUEST_TYPE_QUERY;
	if (combiner->request_type != REQUEST_TYPE_QUERY)
	{
		/* Inconsistent responses */
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("Unexpected response from the data nodes for 'T' message, current request type %d", combiner->request_type)));
	}
	/* Increment counter and check if it was first */
	if (combiner->description_count++ == 0)
	{
		combiner->tuple_desc = create_tuple_desc(msg_body, len);
		return true;
	}
	return false;
}


#ifdef NOT_USED
/*
 * Handle ParameterStatus ('S') message from a data node connection (SET command)
 */
static void
HandleParameterStatus(RemoteQueryState *combiner, char *msg_body, size_t len)
{
	if (combiner->request_type == REQUEST_TYPE_NOT_DEFINED)
		combiner->request_type = REQUEST_TYPE_QUERY;
	if (combiner->request_type != REQUEST_TYPE_QUERY)
	{
		/* Inconsistent responses */
		ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("Unexpected response from the data nodes for 'S' message, current request type %d", combiner->request_type)));
	}
	/* Proxy last */
	if (++combiner->description_count == combiner->node_count)
	{
		pq_putmessage('S', msg_body, len);
	}
}
#endif

/*
 * Handle CopyInResponse ('G') message from a data node connection
 */
static void
#ifdef XCP
HandleCopyIn(ResponseCombiner *combiner)
#else
HandleCopyIn(RemoteQueryState *combiner)
#endif
{
#ifdef XCP
	if (combiner->request_type == REQUEST_TYPE_ERROR)
		return;
#endif
	if (combiner->request_type == REQUEST_TYPE_NOT_DEFINED)
		combiner->request_type = REQUEST_TYPE_COPY_IN;
	if (combiner->request_type != REQUEST_TYPE_COPY_IN)
	{
		/* Inconsistent responses */
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("Unexpected response from the data nodes for 'G' message, current request type %d", combiner->request_type)));
	}
	/*
	 * The normal PG code will output an G message when it runs in the
	 * coordinator, so do not proxy message here, just count it.
	 */
	combiner->copy_in_count++;
}

/*
 * Handle CopyOutResponse ('H') message from a data node connection
 */
static void
#ifdef XCP
HandleCopyOut(ResponseCombiner *combiner)
#else
HandleCopyOut(RemoteQueryState *combiner)
#endif
{
#ifdef XCP
	if (combiner->request_type == REQUEST_TYPE_ERROR)
		return;
#endif
	if (combiner->request_type == REQUEST_TYPE_NOT_DEFINED)
		combiner->request_type = REQUEST_TYPE_COPY_OUT;
	if (combiner->request_type != REQUEST_TYPE_COPY_OUT)
	{
		/* Inconsistent responses */
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("Unexpected response from the data nodes for 'H' message, current request type %d", combiner->request_type)));
	}
	/*
	 * The normal PG code will output an H message when it runs in the
	 * coordinator, so do not proxy message here, just count it.
	 */
	combiner->copy_out_count++;
}

/*
 * Handle CopyOutDataRow ('d') message from a data node connection
 */
static void
#ifdef XCP
HandleCopyDataRow(ResponseCombiner *combiner, char *msg_body, size_t len)
#else
HandleCopyDataRow(RemoteQueryState *combiner, char *msg_body, size_t len)
#endif
{
#ifdef XCP
	if (combiner->request_type == REQUEST_TYPE_ERROR)
		return;
#endif
	if (combiner->request_type == REQUEST_TYPE_NOT_DEFINED)
		combiner->request_type = REQUEST_TYPE_COPY_OUT;

	/* Inconsistent responses */
	if (combiner->request_type != REQUEST_TYPE_COPY_OUT)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("Unexpected response from the data nodes for 'd' message, current request type %d", combiner->request_type)));

	/* count the row */
	combiner->processed++;

	/* If there is a copy file, data has to be sent to the local file */
	if (combiner->copy_file)
		/* write data to the copy file */
		fwrite(msg_body, 1, len, combiner->copy_file);
	else
		pq_putmessage('d', msg_body, len);
}

/*
 * Handle DataRow ('D') message from a data node connection
 * The function returns true if buffer contains row or false if it has been
 * skipped
 */
#ifdef XCP
static bool
HandleDataRow(ResponseCombiner *combiner, char *msg_body, size_t len, Oid node)
{
	/* We expect previous message is consumed */
	Assert(combiner->currentRow == NULL);

	if (combiner->request_type == REQUEST_TYPE_ERROR)
		return false;

	if (combiner->request_type != REQUEST_TYPE_QUERY)
	{
		/* Inconsistent responses */
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("Unexpected response from the data nodes for 'D' message, current request type %d", combiner->request_type)));
	}

	/*
	 * If we got an error already ignore incoming data rows from other nodes
	 * Still we want to continue reading until get CommandComplete
	 */
	if (combiner->errorMessage)
		return false;

	/*
	 * We are copying message because it points into connection buffer, and
	 * will be overwritten on next socket read
	 */
	combiner->currentRow = (RemoteDataRow) palloc(sizeof(RemoteDataRowData) + len);
	memcpy(combiner->currentRow->msg, msg_body, len);
	combiner->currentRow->msglen = len;
	combiner->currentRow->msgnode = node;

	return true;
}
#else
static void
HandleDataRow(RemoteQueryState *combiner, char *msg_body, size_t len, int nid)
{
	/* We expect previous message is consumed */
	Assert(combiner->currentRow.msg == NULL);

	if (nid < 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("invalid node id %d",
						nid)));

	if (combiner->request_type != REQUEST_TYPE_QUERY)
	{
		/* Inconsistent responses */
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("Unexpected response from the data nodes for 'D' message, current request type %d", combiner->request_type)));
	}

	/*
	 * If we got an error already ignore incoming data rows from other nodes
	 * Still we want to continue reading until get CommandComplete
	 */
	if (combiner->errorMessage)
		return;

	/*
	 * We are copying message because it points into connection buffer, and
	 * will be overwritten on next socket read
	 */
	combiner->currentRow.msg = (char *) palloc(len);
	memcpy(combiner->currentRow.msg, msg_body, len);
	combiner->currentRow.msglen = len;
	combiner->currentRow.msgnode = nid;
}
#endif

/*
 * Handle ErrorResponse ('E') message from a data node connection
 */
static void
#ifdef XCP
HandleError(ResponseCombiner *combiner, char *msg_body, size_t len)
#else
HandleError(RemoteQueryState *combiner, char *msg_body, size_t len)
#endif
{
	/* parse error message */
	char *code = NULL;
	char *message = NULL;
	char *detail = NULL;
	int   offset = 0;

	/*
	 * Scan until point to terminating \0
	 */
	while (offset + 1 < len)
	{
		/* pointer to the field message */
		char *str = msg_body + offset + 1;

		switch (msg_body[offset])
		{
			case 'C':	/* code */
				code = str;
				break;
			case 'M':	/* message */
				message = str;
				break;
			case 'D':	/* details */
				detail = str;
				break;

			/* Fields not yet in use */
			case 'S':	/* severity */
			case 'R':	/* routine */
			case 'H':	/* hint */
			case 'P':	/* position string */
			case 'p':	/* position int */
			case 'q':	/* int query */
			case 'W':	/* where */
			case 'F':	/* file */
			case 'L':	/* line */
			default:
				break;
		}

		/* code, message and \0 */
		offset += strlen(str) + 2;
	}

	/*
	 * We may have special handling for some errors, default handling is to
	 * throw out error with the same message. We can not ereport immediately
	 * because we should read from this and other connections until
	 * ReadyForQuery is received, so we just store the error message.
	 * If multiple connections return errors only first one is reported.
	 */
#ifdef XCP
	/*
	 * The producer error may me hiding primary error, so if previously received
	 * error is a producer error allow it to be overwritten.
	 */
	if (combiner->errorMessage == NULL ||
			MAKE_SQLSTATE(combiner->errorCode[0], combiner->errorCode[1],
						  combiner->errorCode[2], combiner->errorCode[3],
						  combiner->errorCode[4]) == ERRCODE_PRODUCER_ERROR)
	{
		combiner->errorMessage = pstrdup(message);
		/* Error Code is exactly 5 significant bytes */
		if (code)
			memcpy(combiner->errorCode, code, 5);
		if (detail)
			combiner->errorDetail = pstrdup(detail);
	}
#else
	if (!combiner->errorMessage)
	{
		combiner->errorMessage = pstrdup(message);
		/* Error Code is exactly 5 significant bytes */
		if (code)
			memcpy(combiner->errorCode, code, 5);
	}

	if (!combiner->errorDetail && detail != NULL)
	{
		combiner->errorDetail = pstrdup(detail);
	}
#endif

	/*
	 * If data node have sent ErrorResponse it will never send CommandComplete.
	 * Increment the counter to prevent endless waiting for it.
	 */
	combiner->command_complete_count++;
}

/*
 * HandleCmdComplete -
 *	combine deparsed sql statements execution results
 *
 * Input parameters:
 *	commandType is dml command type
 *	combineTag is used to combine the completion result
 *	msg_body is execution result needed to combine
 *	len is msg_body size
 */
void
HandleCmdComplete(CmdType commandType, CombineTag *combine,
						const char *msg_body, size_t len)
{
	int	digits = 0;
	uint64	originrowcount = 0;
	uint64	rowcount = 0;
	uint64	total = 0;

	if (msg_body == NULL)
		return;

	/* if there's nothing in combine, just copy the msg_body */
	if (strlen(combine->data) == 0)
	{
		strcpy(combine->data, msg_body);
		combine->cmdType = commandType;
		return;
	}
	else
	{
		/* commandType is conflict */
		if (combine->cmdType != commandType)
			return;

		/* get the processed row number from msg_body */
		digits = parse_row_count(msg_body, len + 1, &rowcount);
		elog(DEBUG1, "digits is %d\n", digits);
		Assert(digits >= 0);

		/* no need to combine */
		if (digits == 0)
			return;

		/* combine the processed row number */
		parse_row_count(combine->data, strlen(combine->data) + 1, &originrowcount);
		elog(DEBUG1, "originrowcount is %lu, rowcount is %lu\n", originrowcount, rowcount);
		total = originrowcount + rowcount;

	}

	/* output command completion tag */
	switch (commandType)
	{
		case CMD_SELECT:
			strcpy(combine->data, "SELECT");
			break;
		case CMD_INSERT:
			snprintf(combine->data, COMPLETION_TAG_BUFSIZE,
			   "INSERT %u %lu", 0, total);
			break;
		case CMD_UPDATE:
			snprintf(combine->data, COMPLETION_TAG_BUFSIZE,
					 "UPDATE %lu", total);
			break;
		case CMD_DELETE:
			snprintf(combine->data, COMPLETION_TAG_BUFSIZE,
					 "DELETE %lu", total);
			break;
		default:
			strcpy(combine->data, "");
			break;
	}

}

/*
 * Examine the specified combiner state and determine if command was completed
 * successfully
 */
static bool
#ifdef XCP
validate_combiner(ResponseCombiner *combiner)
#else
validate_combiner(RemoteQueryState *combiner)
#endif
{
	/* There was error message while combining */
	if (combiner->errorMessage)
		return false;
	/* Check if state is defined */
	if (combiner->request_type == REQUEST_TYPE_NOT_DEFINED)
		return false;

	/* Check all nodes completed */
	if ((combiner->request_type == REQUEST_TYPE_COMMAND
	        || combiner->request_type == REQUEST_TYPE_QUERY)
	        && combiner->command_complete_count != combiner->node_count)
		return false;

	/* Check count of description responses */
	if (combiner->request_type == REQUEST_TYPE_QUERY
	        && combiner->description_count != combiner->node_count)
		return false;

	/* Check count of copy-in responses */
	if (combiner->request_type == REQUEST_TYPE_COPY_IN
	        && combiner->copy_in_count != combiner->node_count)
		return false;

	/* Check count of copy-out responses */
	if (combiner->request_type == REQUEST_TYPE_COPY_OUT
	        && combiner->copy_out_count != combiner->node_count)
		return false;

	/* Add other checks here as needed */

	/* All is good if we are here */
	return true;
}

/*
 * Close combiner and free allocated memory, if it is not needed
 */
#ifdef XCP
void
CloseCombiner(ResponseCombiner *combiner)
{
	if (combiner->connections)
		pfree(combiner->connections);
	if (combiner->tuple_desc)
		FreeTupleDesc(combiner->tuple_desc);
	if (combiner->errorMessage)
		pfree(combiner->errorMessage);
	if (combiner->cursor_connections)
		pfree(combiner->cursor_connections);
	if (combiner->tapenodes)
		pfree(combiner->tapenodes);
	if (combiner->tapemarks)
		pfree(combiner->tapemarks);
}
#else
static void
CloseCombiner(RemoteQueryState *combiner)
{
	if (combiner)
	{
		if (combiner->connections)
			pfree(combiner->connections);
		if (combiner->tuple_desc)
			FreeTupleDesc(combiner->tuple_desc);
		if (combiner->errorMessage)
			pfree(combiner->errorMessage);
		if (combiner->errorDetail)
			pfree(combiner->errorDetail);
		if (combiner->cursor_connections)
			pfree(combiner->cursor_connections);
		if (combiner->tapenodes)
			pfree(combiner->tapenodes);
		pfree(combiner);
	}
}
#endif

/*
 * Validate combiner and release storage freeing allocated memory
 */
static bool
#ifdef XCP
ValidateAndCloseCombiner(ResponseCombiner *combiner)
#else
ValidateAndCloseCombiner(RemoteQueryState *combiner)
#endif
{
	bool		valid = validate_combiner(combiner);

	CloseCombiner(combiner);

	return valid;
}

/*
 * It is possible if multiple steps share the same data node connection, when
 * executor is running multi-step query or client is running multiple queries
 * using Extended Query Protocol. After returning next tuple ExecRemoteQuery
 * function passes execution control to the executor and then it can be given
 * to the same RemoteQuery or to different one. It is possible that before
 * returning a tuple the function do not read all data node responses. In this
 * case pending responses should be read in context of original RemoteQueryState
 * till ReadyForQuery message and data rows should be stored (buffered) to be
 * available when fetch from that RemoteQueryState is requested again.
 * BufferConnection function does the job.
 * If a RemoteQuery is going to use connection it should check connection state.
 * DN_CONNECTION_STATE_QUERY indicates query has data to read and combiner
 * points to the original RemoteQueryState. If combiner differs from "this" the
 * connection should be buffered.
 */
#ifdef XCP
void
BufferConnection(PGXCNodeHandle *conn)
{
	ResponseCombiner *combiner = conn->combiner;
	MemoryContext oldcontext;

	if (combiner == NULL || conn->state != DN_CONNECTION_STATE_QUERY)
		return;

	elog(LOG, "Buffer connection %u to step %s", conn->nodeoid, combiner->cursor);

	/*
	 * When BufferConnection is invoked CurrentContext is related to other
	 * portal, which is trying to control the connection.
	 * TODO See if we can find better context to switch to
	 */
	oldcontext = MemoryContextSwitchTo(combiner->ss.ps.ps_ResultTupleSlot->tts_mcxt);

	/* Verify the connection is in use by the combiner */
	combiner->current_conn = 0;
	while (combiner->current_conn < combiner->conn_count)
	{
		if (combiner->connections[combiner->current_conn] == conn)
			break;
		combiner->current_conn++;
	}
	Assert(combiner->current_conn < combiner->conn_count);

	if (combiner->tapemarks == NULL)
		combiner->tapemarks = (ListCell**) palloc0(combiner->conn_count * sizeof(ListCell*));

	/*
	 * If current bookmark for the current tape is not set it means either
	 * first row in the buffer is from the current tape or no rows from
	 * the tape in the buffer, so if first row is not from current
	 * connection bookmark the last cell in the list.
	 */
	if (combiner->tapemarks[combiner->current_conn] == NULL &&
			list_length(combiner->rowBuffer) > 0)
	{
		RemoteDataRow dataRow = (RemoteDataRow) linitial(combiner->rowBuffer);
		if (dataRow->msgnode != conn->nodeoid)
			combiner->tapemarks[combiner->current_conn] = list_tail(combiner->rowBuffer);
	}

	/*
	 * Buffer data rows until data node return number of rows specified by the
	 * fetch_size parameter of last Execute message (PortalSuspended message)
	 * or end of result set is reached (CommandComplete message)
	 */
	while (true)
	{
		int res;

		/* Move to buffer currentRow (received from the data node) */
		if (combiner->currentRow)
		{
			combiner->rowBuffer = lappend(combiner->rowBuffer,
										  combiner->currentRow);
			combiner->currentRow = NULL;
		}

		res = handle_response(conn, combiner);
		/*
		 * If response message is a DataRow it will be handled on the next
		 * iteration.
		 * PortalSuspended will cause connection state change and break the loop
		 * The same is for CommandComplete, but we need additional handling -
		 * remove connection from the list of active connections.
		 * We may need to add handling error response
		 */

		/* Most often result check first */
		if (res == RESPONSE_DATAROW)
		{
			/*
			 * The row is in the combiner->currentRow, on next iteration it will
			 * be moved to the buffer
			 */
			continue;
		}

		/* incomplete message, read more */
		if (res == RESPONSE_EOF)
		{
			if (pgxc_node_receive(1, &conn, NULL))
			{
				conn->state = DN_CONNECTION_STATE_ERROR_FATAL;
				add_error_message(conn, "Failed to fetch from data node");
			}
		}

		/*
		 * End of result set is reached, so either set the pointer to the
		 * connection to NULL (combiner with sort) or remove it from the list
		 * (combiner without sort)
		 */
		else if (res == RESPONSE_COMPLETE)
		{
			/*
			 * If combiner is doing merge sort we should set reference to the
			 * current connection to NULL in the array, indicating the end
			 * of the tape is reached. FetchTuple will try to access the buffer
			 * first anyway.
			 * Since we remove that reference we can not determine what node
			 * number was this connection, but we need this info to find proper
			 * tuple in the buffer if we are doing merge sort. So store node
			 * number in special array.
			 * NB: We can not test if combiner->tuplesortstate is set here:
			 * connection may require buffering inside tuplesort_begin_merge
			 * - while pre-read rows from the tapes, one of the tapes may be
			 * the local connection with RemoteSubplan in the tree. The
			 * combiner->tuplesortstate is set only after tuplesort_begin_merge
			 * returns.
			 */
			if (combiner->merge_sort)
			{
				combiner->connections[combiner->current_conn] = NULL;
				if (combiner->tapenodes == NULL)
					combiner->tapenodes = (Oid *)
							palloc0(combiner->conn_count * sizeof(Oid));
				combiner->tapenodes[combiner->current_conn] = conn->nodeoid;
			}
			else
			{
				/* Remove current connection, move last in-place, adjust current_conn */
				if (combiner->current_conn < --combiner->conn_count)
					combiner->connections[combiner->current_conn] = combiner->connections[combiner->conn_count];
				else
					combiner->current_conn = 0;
			}
			/*
			 * If combiner runs Simple Query Protocol we need to read in
			 * ReadyForQuery. In case of Extended Query Protocol it is not
			 * sent and we should quit.
			 */
			if (combiner->extended_query)
				break;
		}
		else if (res == RESPONSE_ERROR)
		{
			if (combiner->extended_query)
			{
				/*
				 * Need to sync connection to enable receiving commands
				 * by the datanode
				 */
				if (pgxc_node_send_sync(conn) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("Failed to fetch from data node")));
			}
		}
		else if (res == RESPONSE_SUSPENDED || res == RESPONSE_READY)
		{
			/* Now it is OK to quit */
			break;
		}
	}
	Assert(conn->state != DN_CONNECTION_STATE_QUERY);
	MemoryContextSwitchTo(oldcontext);
	conn->combiner = NULL;
}
#else
void
BufferConnection(PGXCNodeHandle *conn)
{
	RemoteQueryState *combiner = conn->combiner;
	MemoryContext oldcontext;

	if (combiner == NULL || conn->state != DN_CONNECTION_STATE_QUERY)
		return;

	/*
	 * When BufferConnection is invoked CurrentContext is related to other
	 * portal, which is trying to control the connection.
	 * TODO See if we can find better context to switch to
	 */
	oldcontext = MemoryContextSwitchTo(combiner->ss.ss_ScanTupleSlot->tts_mcxt);

	/* Verify the connection is in use by the combiner */
	combiner->current_conn = 0;
	while (combiner->current_conn < combiner->conn_count)
	{
		if (combiner->connections[combiner->current_conn] == conn)
			break;
		combiner->current_conn++;
	}
	Assert(combiner->current_conn < combiner->conn_count);

	/*
	 * Buffer data rows until data node return number of rows specified by the
	 * fetch_size parameter of last Execute message (PortalSuspended message)
	 * or end of result set is reached (CommandComplete message)
	 */
	while (conn->state == DN_CONNECTION_STATE_QUERY)
	{
		int res;

		/* Move to buffer currentRow (received from the data node) */
		if (combiner->currentRow.msg)
		{
			RemoteDataRow dataRow = (RemoteDataRow) palloc(sizeof(RemoteDataRowData));
			*dataRow = combiner->currentRow;
			combiner->currentRow.msg = NULL;
			combiner->currentRow.msglen = 0;
			combiner->currentRow.msgnode = 0;
			combiner->rowBuffer = lappend(combiner->rowBuffer, dataRow);
		}

		res = handle_response(conn, combiner);
		/*
		 * If response message is a DataRow it will be handled on the next
		 * iteration.
		 * PortalSuspended will cause connection state change and break the loop
		 * The same is for CommandComplete, but we need additional handling -
		 * remove connection from the list of active connections.
		 * We may need to add handling error response
		 */
		if (res == RESPONSE_EOF)
		{
			/* incomplete message, read more */
			if (pgxc_node_receive(1, &conn, NULL))
			{
				conn->state = DN_CONNECTION_STATE_ERROR_FATAL;
				add_error_message(conn, "Failed to fetch from data node");
			}
		}
		else if (res == RESPONSE_COMPLETE)
		{
			/*
			 * End of result set is reached, so either set the pointer to the
			 * connection to NULL (step with sort) or remove it from the list
			 * (step without sort)
			 */
			if (combiner->tuplesortstate)
			{
				combiner->connections[combiner->current_conn] = NULL;
				if (combiner->tapenodes == NULL)
					combiner->tapenodes = (int*) palloc0(NumDataNodes * sizeof(int));
				combiner->tapenodes[combiner->current_conn] =
						PGXCNodeGetNodeId(conn->nodeoid,
										  PGXC_NODE_DATANODE);
			}
			else
				/* Remove current connection, move last in-place, adjust current_conn */
				if (combiner->current_conn < --combiner->conn_count)
					combiner->connections[combiner->current_conn] = combiner->connections[combiner->conn_count];
				else
					combiner->current_conn = 0;
		}
		/*
		 * Before output RESPONSE_COMPLETE or PORTAL_SUSPENDED handle_response()
		 * changes connection state to DN_CONNECTION_STATE_IDLE, breaking the
		 * loop. We do not need to do anything specific in case of
		 * PORTAL_SUSPENDED so skiping "else if" block for that case
		 */
	}
	MemoryContextSwitchTo(oldcontext);
	conn->combiner = NULL;
}
#endif

/*
 * copy the datarow from combiner to the given slot, in the slot's memory
 * context
 */
#ifdef XCP
static void
CopyDataRowTupleToSlot(ResponseCombiner *combiner, TupleTableSlot *slot)
{
	RemoteDataRow 	datarow;
	MemoryContext	oldcontext;
	oldcontext = MemoryContextSwitchTo(slot->tts_mcxt);
	datarow = (RemoteDataRow) palloc(sizeof(RemoteDataRowData) + combiner->currentRow->msglen);
	datarow->msgnode = combiner->currentRow->msgnode;
	datarow->msglen = combiner->currentRow->msglen;
	memcpy(datarow->msg, combiner->currentRow->msg, datarow->msglen);
	ExecStoreDataRowTuple(datarow, slot, true);
	pfree(combiner->currentRow);
	combiner->currentRow = NULL;
	MemoryContextSwitchTo(oldcontext);
}
#else
static void
CopyDataRowTupleToSlot(RemoteQueryState *combiner, TupleTableSlot *slot)
{
	char 		*msg;
	MemoryContext	oldcontext;
	oldcontext = MemoryContextSwitchTo(slot->tts_mcxt);
	msg = (char *)palloc(combiner->currentRow.msglen);
	memcpy(msg, combiner->currentRow.msg, combiner->currentRow.msglen);
	ExecStoreDataRowTuple(msg, combiner->currentRow.msglen, slot, true);
	pfree(combiner->currentRow.msg);
	combiner->currentRow.msg = NULL;
	combiner->currentRow.msglen = 0;
	combiner->currentRow.msgnode = 0;
	MemoryContextSwitchTo(oldcontext);
}
#endif


#ifdef XCP
/*
 * FetchTuple
 *
		Get next tuple from one of the datanode connections.
 * The connections should be in combiner->connections, if "local" dummy
 * connection presents it should be the last active connection in the array.
 *      If combiner is set up to perform merge sort function returns tuple from
 * connection defined by combiner->current_conn, or NULL slot if no more tuple
 * are available from the connection. Otherwise it returns tuple from any
 * connection or NULL slot if no more available connections.
 * 		Function looks into combiner->rowBuffer before accessing connection
 * and return a tuple from there if found.
 * 		Function may wait while more data arrive from the data nodes. If there
 * is a locally executed subplan function advance it and buffer resulting rows
 * instead of waiting.
 */
TupleTableSlot *
FetchTuple(ResponseCombiner *combiner)
{
	PGXCNodeHandle *conn;
	TupleTableSlot *slot;
	Oid 			nodeOid;

	/*
	 * Case if we run local subplan.
	 * We do not have remote connections, so just get local tuple and return it
	 */
	if (outerPlanState(combiner))
	{
		RemoteSubplanState *planstate = (RemoteSubplanState *) combiner;
		RemoteSubplan *plan = (RemoteSubplan *) combiner->ss.ps.plan;
		/* Advance subplan in a loop until we have something to return */
		for (;;)
		{
			Datum 	value;
			bool 	isnull;
			int 	numnodes;
			int		i;

			slot = ExecProcNode(outerPlanState(combiner));
			/* If locator is not defined deliver all the results */
			if (planstate->locator == NULL)
				return slot;

			/*
			 * If NULL tuple is returned we done with the subplan, finish it up and
			 * return NULL
			 */
			if (TupIsNull(slot))
				return NULL;

			/* Get partitioning value if defined */
			if (plan->distributionKey != InvalidAttrNumber)
				value = slot_getattr(slot, plan->distributionKey, &isnull);

			/* Determine target nodes */
			numnodes = GET_NODES(planstate->locator, value, isnull, NULL);
			for (i = 0; i < numnodes; i++)
			{
				/* Deliver the node */
				if (planstate->dest_nodes[i] == PGXCNodeId-1)
					return slot;
			}
		}
	}

	/*
	 * Get current connection
	 */
	if (combiner->conn_count > combiner->current_conn)
		conn = combiner->connections[combiner->current_conn];
	else
		conn = NULL;

	/*
	 * If doing merge sort determine the node number.
	 * It may be needed to get buffered row.
	 */
	if (combiner->merge_sort)
	{
		Assert(conn || combiner->tapenodes);
		nodeOid = conn ? conn->nodeoid :
						 combiner->tapenodes[combiner->current_conn];
		Assert(OidIsValid(nodeOid));
	}

	/*
	 * First look into the row buffer.
	 * When we are performing merge sort we need to get from the buffer record
	 * from the connection marked as "current". Otherwise get first.
	 */
	if (list_length(combiner->rowBuffer) > 0)
	{
		RemoteDataRow dataRow;

		Assert(combiner->currentRow == NULL);

		if (combiner->merge_sort)
		{
			ListCell *lc;
			ListCell *prev;

			elog(LOG, "Getting buffered tuple from node %x", nodeOid);

			prev = combiner->tapemarks[combiner->current_conn];
			if (prev)
			{
				/*
				 * Start looking through the list from the bookmark.
				 * Probably the first cell we check contains row from the needed
				 * node. Otherwise continue scanning until we encounter one,
				 * advancing prev pointer as well.
				 */
				while((lc = lnext(prev)) != NULL)
				{
					dataRow = (RemoteDataRow) lfirst(lc);
					if (dataRow->msgnode == nodeOid)
					{
						combiner->currentRow = dataRow;
						break;
					}
					prev = lc;
				}
			}
			else
			{
				/*
				 * Either needed row is the first in the buffer or no such row
				 */
				lc = list_head(combiner->rowBuffer);
				dataRow = (RemoteDataRow) lfirst(lc);
				if (dataRow->msgnode == nodeOid)
					combiner->currentRow = dataRow;
				else
					lc = NULL;
			}
			if (lc)
			{
				/*
				 * Delete cell from the buffer. Before we delete we must check
				 * the bookmarks, if the cell is a bookmark for any tape.
				 * If it is the case we are deleting last row of the current
				 * block from the current tape. That tape should have bookmark
				 * like current, and current bookmark will be advanced when we
				 * read the tape once again.
				 */
				int i;
				for (i = 0; i < combiner->conn_count; i++)
				{
					if (combiner->tapemarks[i] == lc)
						combiner->tapemarks[i] = prev;
				}
				elog(LOG, "Found buffered tuple from node %x", nodeOid);
				combiner->rowBuffer = list_delete_cell(combiner->rowBuffer,
													   lc, prev);
			}
			elog(LOG, "Update tapemark");
			combiner->tapemarks[combiner->current_conn] = prev;
		}
		else
		{
			dataRow = (RemoteDataRow) linitial(combiner->rowBuffer);
			combiner->currentRow = dataRow;
			combiner->rowBuffer = list_delete_first(combiner->rowBuffer);
		}
	}

	/* If we have node message in the currentRow slot, and it is from a proper
	 * node, consume it.  */
	if (combiner->currentRow)
	{
		Assert(!combiner->merge_sort ||
			   combiner->currentRow->msgnode == nodeOid);
		slot = combiner->ss.ps.ps_ResultTupleSlot;
		CopyDataRowTupleToSlot(combiner, slot);
		return slot;
	}

	while (conn)
	{
		int res;

		/* Going to use a connection, buffer it if needed */
		CHECK_OWNERSHIP(conn, combiner);

		/*
		 * If current connection is idle it means portal on the data node is
		 * suspended. Request more and try to get it
		 */
		if (combiner->extended_query &&
				conn->state == DN_CONNECTION_STATE_IDLE)
		{
			if (pgxc_node_send_execute(conn, combiner->cursor, 1000) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("Failed to fetch from data node")));
			if (pgxc_node_send_flush(conn) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("Failed to fetch from data node")));
			if (pgxc_node_receive(1, &conn, NULL))
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("Failed to fetch from data node")));
		}

		/* read messages */
		res = handle_response(conn, combiner);
		if (res == RESPONSE_DATAROW)
		{
			slot = combiner->ss.ps.ps_ResultTupleSlot;
			CopyDataRowTupleToSlot(combiner, slot);
			return slot;
		}
		else if (res == RESPONSE_EOF)
		{
			/* incomplete message, read more */
			if (pgxc_node_receive(1, &conn, NULL))
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("Failed to fetch from data node")));
			continue;
		}
		else if (res == RESPONSE_SUSPENDED)
		{
			/*
			 * If we are doing merge sort continue with current connection and
			 * send request for more rows from it, otherwise make next
			 * connection current
			 */
			if (combiner->merge_sort)
				continue;
			if (++combiner->current_conn >= combiner->conn_count)
				combiner->current_conn = 0;
			conn = combiner->connections[combiner->current_conn];
		}
		else if (res == RESPONSE_COMPLETE)
		{
			/*
			 * In case of Simple Query Protocol we should receive ReadyForQuery
			 * before removing connection from the list. In case of Extended
			 * Query Protocol we may remove connection right away.
			 */
			if (combiner->extended_query)
			{
				/* If we are doing merge sort clean current connection and return
				 * NULL, otherwise remove current connection, move last in-place,
				 * adjust current_conn and continue if it is not last connection */
				if (combiner->merge_sort)
				{
					combiner->connections[combiner->current_conn] = NULL;
					return NULL;
				}
				REMOVE_CURR_CONN(combiner);
				if (combiner->conn_count > 0)
					conn = combiner->connections[combiner->current_conn];
				else
					return NULL;
			}
		}
		else if (res == RESPONSE_ERROR)
		{
			/*
			 * If doing Extended Query Protocol we need to sync connection,
			 * otherwise subsequent commands will be ignored.
			 */
			if (combiner->extended_query)
			{
				if (pgxc_node_send_sync(conn) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("Failed to fetch from data node")));
			}
		}
		else if (res == RESPONSE_READY)
		{
			/* If we are doing merge sort clean current connection and return
			 * NULL, otherwise remove current connection, move last in-place,
			 * adjust current_conn and continue if it is not last connection */
			if (combiner->merge_sort)
			{
				combiner->connections[combiner->current_conn] = NULL;
				return NULL;
			}
			REMOVE_CURR_CONN(combiner);
			if (combiner->conn_count > 0)
				conn = combiner->connections[combiner->current_conn];
			else
				return NULL;
		}
		else if (res == RESPONSE_TUPDESC)
		{
			ExecSetSlotDescriptor(combiner->ss.ps.ps_ResultTupleSlot,
								  combiner->tuple_desc);
			/* Now slot is responsible for freeng the descriptor */
			combiner->tuple_desc = NULL;
		}
		else
		{
			// Can not get here?
			Assert(false);
		}
	}

	return NULL;
}
#else
/*
 * Get next data row from the combiner's buffer into provided slot
 * Just clear slot and return false if buffer is empty, that means end of result
 * set is reached
 */
bool
FetchTuple(RemoteQueryState *combiner, TupleTableSlot *slot)
{
	bool have_tuple = false;

	/* If we have message in the buffer, consume it */
	if (combiner->currentRow.msg)
	{
		CopyDataRowTupleToSlot(combiner, slot);
		have_tuple = true;
	}

	/*
	 * If this is ordered fetch we can not know what is the node
	 * to handle next, so sorter will choose next itself and set it as
	 * currentRow to have it consumed on the next call to FetchTuple.
	 * Otherwise allow to prefetch next tuple
	 */
	if (((RemoteQuery *)combiner->ss.ps.plan)->sort)
		return have_tuple;

	/*
	 * Note: If we are fetching not sorted results we can not have both
	 * currentRow and buffered rows. When connection is buffered currentRow
	 * is moved to buffer, and then it is cleaned after buffering is
	 * completed. Afterwards rows will be taken from the buffer bypassing
	 * currentRow until buffer is empty, and only after that data are read
	 * from a connection.
	 * PGXCTODO: the message should be allocated in the same memory context as
	 * that of the slot. Are we sure of that in the call to
	 * ExecStoreDataRowTuple below? If one fixes this memory issue, please
	 * consider using CopyDataRowTupleToSlot() for the same.
	 */
	if (list_length(combiner->rowBuffer) > 0)
	{
		RemoteDataRow dataRow = (RemoteDataRow) linitial(combiner->rowBuffer);
		combiner->rowBuffer = list_delete_first(combiner->rowBuffer);
		ExecStoreDataRowTuple(dataRow->msg, dataRow->msglen, slot, true);
		pfree(dataRow);
		return true;
	}

	while (combiner->conn_count > 0)
	{
		int res;
		PGXCNodeHandle *conn = combiner->connections[combiner->current_conn];

		/* Going to use a connection, buffer it if needed */
		if (conn->state == DN_CONNECTION_STATE_QUERY && conn->combiner != NULL
				&& conn->combiner != combiner)
			BufferConnection(conn);

		/*
		 * If current connection is idle it means portal on the data node is
		 * suspended. If we have a tuple do not hurry to request more rows,
		 * leave connection clean for other RemoteQueries.
		 * If we do not have, request more and try to get it
		 */
		if (conn->state == DN_CONNECTION_STATE_IDLE)
		{
			/*
			 * If we have tuple to return do not hurry to request more, keep
			 * connection clean
			 */
			if (have_tuple)
				return true;
			else
			{
				if (pgxc_node_send_execute(conn, combiner->cursor, 1) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("Failed to fetch from data node")));
				if (pgxc_node_send_sync(conn) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("Failed to fetch from data node")));
				if (pgxc_node_receive(1, &conn, NULL))
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("Failed to fetch from data node")));
				conn->combiner = combiner;
			}
		}

		/* read messages */
		res = handle_response(conn, combiner);
		if (res == RESPONSE_EOF)
		{
			/* incomplete message, read more */
			if (pgxc_node_receive(1, &conn, NULL))
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("Failed to fetch from data node")));
			continue;
		}
		else if (res == RESPONSE_SUSPENDED)
		{
			/* Make next connection current */
			if (++combiner->current_conn >= combiner->conn_count)
				combiner->current_conn = 0;
		}
		else if (res == RESPONSE_COMPLETE)
		{
			/* Remove current connection, move last in-place, adjust current_conn */
			if (combiner->current_conn < --combiner->conn_count)
				combiner->connections[combiner->current_conn] = combiner->connections[combiner->conn_count];
			else
				combiner->current_conn = 0;
		}
		else if (res == RESPONSE_DATAROW && have_tuple)
		{
			/*
			 * We already have a tuple and received another one, leave it till
			 * next fetch
			 */
			return true;
		}

		/* If we have message in the buffer, consume it */
		if (combiner->currentRow.msg)
		{
			CopyDataRowTupleToSlot(combiner, slot);
			have_tuple = true;
		}

		/*
		 * If this is ordered fetch we can not know what is the node
		 * to handle next, so sorter will choose next itself and set it as
		 * currentRow to have it consumed on the next call to FetchTuple.
		 * Otherwise allow to prefetch next tuple
		 */
		if (((RemoteQuery *)combiner->ss.ps.plan)->sort)
			return have_tuple;
	}

	/* report end of data to the caller */
	if (!have_tuple)
		ExecClearTuple(slot);

	return have_tuple;
}
#endif


/*
 * Handle responses from the Data node connections
 */
static int
#ifdef XCP
pgxc_node_receive_responses(const int conn_count, PGXCNodeHandle ** connections,
						 struct timeval * timeout, ResponseCombiner *combiner)
#else
pgxc_node_receive_responses(const int conn_count, PGXCNodeHandle ** connections,
						 struct timeval * timeout, RemoteQueryState *combiner)
#endif
{
	int			count = conn_count;
	PGXCNodeHandle *to_receive[conn_count];

	/* make a copy of the pointers to the connections */
	memcpy(to_receive, connections, conn_count * sizeof(PGXCNodeHandle *));

	/*
	 * Read results.
	 * Note we try and read from data node connections even if there is an error on one,
	 * so as to avoid reading incorrect results on the next statement.
	 * Other safegaurds exist to avoid this, however.
	 */
	while (count > 0)
	{
		int i = 0;

		if (pgxc_node_receive(count, to_receive, timeout))
			return EOF;
		while (i < count)
		{
			int result =  handle_response(to_receive[i], combiner);
			switch (result)
			{
				case RESPONSE_EOF: /* have something to read, keep receiving */
					i++;
					break;
				case RESPONSE_COMPLETE:
#ifdef XCP
					if (to_receive[i]->state != DN_CONNECTION_STATE_ERROR_FATAL)
						/* Continue read until ReadyForQuery */
						break;
					/* fallthru */
				case RESPONSE_READY:
					/* fallthru */
#endif
				case RESPONSE_COPY:
					/* Handling is done, do not track this connection */
					count--;
					/* Move last connection in place */
					if (i < count)
						to_receive[i] = to_receive[count];
					break;
#ifdef XCP
				case RESPONSE_ERROR:
					/* no handling needed, just wait for ReadyForQuery */
					break;
#endif
				default:
					/* Inconsistent responses */
					add_error_message(to_receive[i], "Unexpected response from the data nodes");
					elog(ERROR, "Unexpected response from the data nodes, result = %d, request type %d", result, combiner->request_type);
					/* Stop tracking and move last connection in place */
					count--;
					if (i < count)
						to_receive[i] = to_receive[count];
			}
		}
	}

	return 0;
}

#ifdef XCP
/*
 * Read next message from the connection and update the combiner
 * and connection state accordingly
 * If we are in an error state we just consume the messages, and do not proxy
 * Long term, we should look into cancelling executing statements
 * and closing the connections.
 * It returns if states need to be handled
 * Return values:
 * RESPONSE_EOF - need to receive more data for the connection
 * RESPONSE_READY - got ReadyForQuery
 * RESPONSE_COMPLETE - done with the connection, but not yet ready for query.
 * Also this result is output in case of error
 * RESPONSE_SUSPENDED - got PortalSuspended
 * RESPONSE_TUPLEDESC - got tuple description
 * RESPONSE_DATAROW - got data row
 * RESPONSE_COPY - got copy response
 * RESPONSE_BARRIER_OK - barrier command completed successfully
 */
int
handle_response(PGXCNodeHandle *conn, ResponseCombiner *combiner)
{
	char	   *msg;
	int			msg_len;
	char		msg_type;

	for (;;)
	{
		/*
		 * If we are in the process of shutting down, we
		 * may be rolling back, and the buffer may contain other messages.
		 * We want to avoid a procarray exception
		 * as well as an error stack overflow.
		 */
		if (proc_exit_inprogress)
			conn->state = DN_CONNECTION_STATE_ERROR_FATAL;

		/*
		 * Don't read from from the connection if there is a fatal error.
		 * We still return RESPONSE_COMPLETE, not RESPONSE_ERROR, since
		 * Handling of RESPONSE_ERROR assumes sending SYNC message, but
		 * State DN_CONNECTION_STATE_ERROR_FATAL indicates connection is
		 * not usable.
		 */
		if (conn->state == DN_CONNECTION_STATE_ERROR_FATAL)
			return RESPONSE_COMPLETE;

		/* No data available, exit */
		if (!HAS_MESSAGE_BUFFERED(conn))
			return RESPONSE_EOF;

		Assert(conn->combiner == combiner || conn->combiner == NULL);

		/* TODO handle other possible responses */
		msg_type = get_message(conn, &msg_len, &msg);
		switch (msg_type)
		{
			case '\0':			/* Not enough data in the buffer */
				return RESPONSE_EOF;
			case 'c':			/* CopyToCommandComplete */
				HandleCopyOutComplete(combiner);
				break;
			case 'C':			/* CommandComplete */
				HandleCommandComplete(combiner, msg, msg_len);
				conn->combiner = NULL;
				conn->state = DN_CONNECTION_STATE_IDLE;
				return RESPONSE_COMPLETE;
			case 'T':			/* RowDescription */
#ifdef DN_CONNECTION_DEBUG
				Assert(!conn->have_row_desc);
				conn->have_row_desc = true;
#endif
				if (HandleRowDescription(combiner, msg, msg_len))
					return RESPONSE_TUPDESC;
				break;
			case 'D':			/* DataRow */
#ifdef DN_CONNECTION_DEBUG
				Assert(conn->have_row_desc);
#endif
				/* Do not return if data row has not been actually handled */
				if (HandleDataRow(combiner, msg, msg_len, conn->nodeoid))
					return RESPONSE_DATAROW;
				break;
			case 's':			/* PortalSuspended */
				/* No activity is expected on the connection until next query */
				conn->state = DN_CONNECTION_STATE_IDLE;
				conn->combiner = NULL;
				return RESPONSE_SUSPENDED;
			case '1': /* ParseComplete */
			case '2': /* BindComplete */
			case '3': /* CloseComplete */
			case 'n': /* NoData */
				/* simple notifications, continue reading */
				break;
			case 'G': /* CopyInResponse */
				conn->state = DN_CONNECTION_STATE_COPY_IN;
				HandleCopyIn(combiner);
				/* Done, return to caller to let it know the data can be passed in */
				return RESPONSE_COPY;
			case 'H': /* CopyOutResponse */
				conn->state = DN_CONNECTION_STATE_COPY_OUT;
				HandleCopyOut(combiner);
				return RESPONSE_COPY;
			case 'd': /* CopyOutDataRow */
				conn->state = DN_CONNECTION_STATE_COPY_OUT;
				HandleCopyDataRow(combiner, msg, msg_len);
				break;
			case 'E':			/* ErrorResponse */
				HandleError(combiner, msg, msg_len);
				add_error_message(conn, combiner->errorMessage);
				return RESPONSE_ERROR;
			case 'A':			/* NotificationResponse */
			case 'N':			/* NoticeResponse */
			case 'S':			/* SetCommandComplete */
				/*
				 * Ignore these to prevent multiple messages, one from each
				 * node. Coordinator will send one for DDL anyway
				 */
				break;
			case 'Z':			/* ReadyForQuery */
				conn->transaction_status = msg[0];
				conn->state = DN_CONNECTION_STATE_IDLE;
				conn->combiner = NULL;
#ifdef DN_CONNECTION_DEBUG
				conn->have_row_desc = false;
#endif
				return RESPONSE_READY;

			case 'b':
				conn->state = DN_CONNECTION_STATE_IDLE;
				return RESPONSE_BARRIER_OK;
			case 'I':			/* EmptyQuery */
			default:
				/* sync lost? */
				elog(WARNING, "Received unsupported message type: %c", msg_type);
				conn->state = DN_CONNECTION_STATE_ERROR_FATAL;
				/* stop reading */
				return RESPONSE_COMPLETE;
		}
	}
	/* never happen, but keep compiler quiet */
	return RESPONSE_EOF;
}
#else
/*
 * Read next message from the connection and update the combiner accordingly
 * If we are in an error state we just consume the messages, and do not proxy
 * Long term, we should look into cancelling executing statements
 * and closing the connections.
 * Return values:
 * RESPONSE_EOF - need to receive more data for the connection
 * RESPONSE_COMPLETE - done with the connection
 * RESPONSE_TUPLEDESC - got tuple description
 * RESPONSE_DATAROW - got data row
 * RESPONSE_COPY - got copy response
 * RESPONSE_BARRIER_OK - barrier command completed successfully
 */
int
handle_response(PGXCNodeHandle * conn, RemoteQueryState *combiner)
{
	char		*msg;
	int		msg_len;
	char		msg_type;
	bool		suspended = false;

	for (;;)
	{
		Assert(conn->state != DN_CONNECTION_STATE_IDLE);

		/*
		 * If we are in the process of shutting down, we
		 * may be rolling back, and the buffer may contain other messages.
		 * We want to avoid a procarray exception
		 * as well as an error stack overflow.
		 */
		if (proc_exit_inprogress)
			conn->state = DN_CONNECTION_STATE_ERROR_FATAL;

		/* don't read from from the connection if there is a fatal error */
		if (conn->state == DN_CONNECTION_STATE_ERROR_FATAL)
			return RESPONSE_COMPLETE;

		/* No data available, exit */
		if (!HAS_MESSAGE_BUFFERED(conn))
			return RESPONSE_EOF;

		Assert(conn->combiner == combiner || conn->combiner == NULL);

		/* TODO handle other possible responses */
		msg_type = get_message(conn, &msg_len, &msg);
		switch (msg_type)
		{
			case '\0':			/* Not enough data in the buffer */
				return RESPONSE_EOF;
			case 'c':			/* CopyToCommandComplete */
				HandleCopyOutComplete(combiner);
				break;
			case 'C':			/* CommandComplete */
				HandleCommandComplete(combiner, msg, msg_len);
				break;
			case 'T':			/* RowDescription */
#ifdef DN_CONNECTION_DEBUG
				Assert(!conn->have_row_desc);
				conn->have_row_desc = true;
#endif
				if (HandleRowDescription(combiner, msg, msg_len))
					return RESPONSE_TUPDESC;
				break;
			case 'D':			/* DataRow */
#ifdef DN_CONNECTION_DEBUG
				Assert(conn->have_row_desc);
#endif
				HandleDataRow(combiner, msg, msg_len, PGXCNodeGetNodeId(conn->nodeoid,
																		PGXC_NODE_DATANODE));
				return RESPONSE_DATAROW;
			case 's':			/* PortalSuspended */
				suspended = true;
				break;
			case '1': /* ParseComplete */
			case '2': /* BindComplete */
			case '3': /* CloseComplete */
			case 'n': /* NoData */
				/* simple notifications, continue reading */
				break;
			case 'G': /* CopyInResponse */
				conn->state = DN_CONNECTION_STATE_COPY_IN;
				HandleCopyIn(combiner);
				/* Done, return to caller to let it know the data can be passed in */
				return RESPONSE_COPY;
			case 'H': /* CopyOutResponse */
				conn->state = DN_CONNECTION_STATE_COPY_OUT;
				HandleCopyOut(combiner);
				return RESPONSE_COPY;
			case 'd': /* CopyOutDataRow */
				conn->state = DN_CONNECTION_STATE_COPY_OUT;
				HandleCopyDataRow(combiner, msg, msg_len);
				break;
			case 'E':			/* ErrorResponse */
				HandleError(combiner, msg, msg_len);
				add_error_message(conn, combiner->errorMessage);
				/*
				 * Do not return with an error, we still need to consume Z,
				 * ready-for-query
				 */
				break;
			case 'A':			/* NotificationResponse */
			case 'N':			/* NoticeResponse */
			case 'S':			/* SetCommandComplete */
				/*
				 * Ignore these to prevent multiple messages, one from each
				 * node. Coordinator will send one for DDL anyway
				 */
				break;
			case 'Z':			/* ReadyForQuery */
			{
				/*
				 * Return result depends on previous connection state.
				 * If it was PORTAL_SUSPENDED coordinator want to send down
				 * another EXECUTE to fetch more rows, otherwise it is done
				 * with the connection
				 */
				int result = suspended ? RESPONSE_SUSPENDED : RESPONSE_COMPLETE;
				conn->transaction_status = msg[0];
				conn->state = DN_CONNECTION_STATE_IDLE;
				conn->combiner = NULL;
#ifdef DN_CONNECTION_DEBUG
				conn->have_row_desc = false;
#endif
				return result;
			}

#ifdef PGXC
			case 'b':
				{
					conn->state = DN_CONNECTION_STATE_IDLE;
					return RESPONSE_BARRIER_OK;
				}
#endif

			case 'I':			/* EmptyQuery */
			default:
				/* sync lost? */
				elog(WARNING, "Received unsupported message type: %c", msg_type);
				conn->state = DN_CONNECTION_STATE_ERROR_FATAL;
				/* stop reading */
				return RESPONSE_COMPLETE;
		}
	}
	/* never happen, but keep compiler quiet */
	return RESPONSE_EOF;
}
#endif


/*
 * Has the data node sent Ready For Query
 */

bool
is_data_node_ready(PGXCNodeHandle * conn)
{
	char		*msg;
	int		msg_len;
	char		msg_type;
	bool		suspended = false;

	for (;;)
	{
		/*
		 * If we are in the process of shutting down, we
		 * may be rolling back, and the buffer may contain other messages.
		 * We want to avoid a procarray exception
		 * as well as an error stack overflow.
		 */
		if (proc_exit_inprogress)
			conn->state = DN_CONNECTION_STATE_ERROR_FATAL;

		/* don't read from from the connection if there is a fatal error */
		if (conn->state == DN_CONNECTION_STATE_ERROR_FATAL)
			return true;

		/* No data available, exit */
		if (!HAS_MESSAGE_BUFFERED(conn))
			return false;

		msg_type = get_message(conn, &msg_len, &msg);
		switch (msg_type)
		{
			case 's':			/* PortalSuspended */
				suspended = true;
				break;

			case 'Z':			/* ReadyForQuery */
				/*
				 * Return result depends on previous connection state.
				 * If it was PORTAL_SUSPENDED coordinator want to send down
				 * another EXECUTE to fetch more rows, otherwise it is done
				 * with the connection
				 */
				conn->transaction_status = msg[0];
				conn->state = DN_CONNECTION_STATE_IDLE;
				conn->combiner = NULL;
				return true;
		}
	}
	/* never happen, but keep compiler quiet */
	return false;
}

/*
 * Construct a BEGIN TRANSACTION command after taking into account the
 * current options. The returned string is not palloced and is valid only until
 * the next call to the function.
 */
static char *
generate_begin_command(void)
{
	static char begin_cmd[1024];
	const char *read_only;
	const char *isolation_level;

	/*
	 * First get the READ ONLY status because the next call to GetConfigOption
	 * will overwrite the return buffer
	 */
	if (strcmp(GetConfigOption("transaction_read_only", false), "on") == 0)
		read_only = "READ ONLY";
	else
		read_only = "READ WRITE";

	/* Now get the isolation_level for the transaction */
	isolation_level = GetConfigOption("transaction_isolation", false);
	if (strcmp(isolation_level, "default") == 0)
		isolation_level = GetConfigOption("default_transaction_isolation", false);

	/* Finally build a START TRANSACTION command */
	sprintf(begin_cmd, "START TRANSACTION ISOLATION LEVEL %s %s", isolation_level, read_only);

	return begin_cmd;
}

/*
 * Send BEGIN command to the Datanodes or Coordinators and receive responses.
 * Also send the GXID for the transaction.
 */
static int
pgxc_node_begin(int conn_count, PGXCNodeHandle **connections,
				GlobalTransactionId gxid, bool need_tran_block,
				bool readOnly, char node_type)
{
	int			i;
	struct timeval *timeout = NULL;
#ifdef XCP
	ResponseCombiner combiner;
#else
	RemoteQueryState *combiner;
#endif
	TimestampTz timestamp = GetCurrentGTMStartTimestamp();
	PGXCNodeHandle *new_connections[conn_count];
	int new_count = 0;
	int 			con[conn_count];
	int				j = 0;

	/*
	 * If no remote connections, we don't have anything to do
	 */
	if (conn_count == 0)
		return 0;

	for (i = 0; i < conn_count; i++)
	{
		/*
		 * If the node is already a participant in the transaction, skip it
		 */
		if (list_member(XactReadNodes, connections[i]) ||
				list_member(XactWriteNodes, connections[i]))
		{
			/*
			 * If we are doing a write operation, we may need to shift the node
			 * to the write-list. RegisterTransactionNodes does that for us
			 */
			if (!readOnly)
				RegisterTransactionNodes(1, (void **)&connections[i], true);
			continue;
		}

		/*
		 * PGXC TODO - A connection should not be in DN_CONNECTION_STATE_QUERY
		 * state when we are about to send a BEGIN TRANSACTION command to the
		 * node. We should consider changing the following to an assert and fix
		 * any bugs reported
		 */
		if (connections[i]->state == DN_CONNECTION_STATE_QUERY)
			BufferConnection(connections[i]);

		/* Send GXID and check for errors */
		if (GlobalTransactionIdIsValid(gxid) && pgxc_node_send_gxid(connections[i], gxid))
			return EOF;

		/* Send timestamp and check for errors */
		if (GlobalTimestampIsValid(timestamp) && pgxc_node_send_timestamp(connections[i], timestamp))
			return EOF;

		/* Send BEGIN */
		if (need_tran_block)
		{
			/* Send the BEGIN TRANSACTION command and check for errors */
			if (pgxc_node_send_query(connections[i], generate_begin_command()))
				return EOF;

#ifdef XCP
			con[j++] = PGXCNodeGetNodeId(connections[i]->nodeoid, &node_type);
#else
			con[j++] = PGXCNodeGetNodeId(connections[i]->nodeoid, node_type);
#endif
			/*
			 * Register the node as a participant in the transaction. The
			 * caller should tell us if the node may do any write activitiy
			 *
			 * XXX This is a bit tricky since it would be difficult to know if
			 * statement has any side effect on the data node. So a SELECT
			 * statement may invoke a function on the data node which may end up
			 * modifying the data at the data node. We can possibly rely on the
			 * function qualification to decide if a statement is a read-only or a
			 * read-write statement.
			 */
			RegisterTransactionNodes(1, (void **)&connections[i], !readOnly);
			new_connections[new_count++] = connections[i];
		}
	}

	/*
	 * If we did not send a BEGIN command to any node, we are done. Otherwise,
	 * we need to check for any errors and report them
	 */
	if (new_count == 0)
		return 0;

#ifdef XCP
	InitResponseCombiner(&combiner, new_count, COMBINE_TYPE_NONE);
	/*
	 * Make sure there are zeroes in unused fields
	 */
	memset(&combiner, 0, sizeof(ScanState));

	/* Receive responses */
	if (pgxc_node_receive_responses(new_count, new_connections, timeout, &combiner))
		return EOF;

	/* Verify status */
	if (!ValidateAndCloseCombiner(&combiner))
		return EOF;
#else
	combiner = CreateResponseCombiner(new_count, COMBINE_TYPE_NONE);

	/* Receive responses */
	if (pgxc_node_receive_responses(new_count, new_connections, timeout, combiner))
		return EOF;

	/* Verify status */
	if (!ValidateAndCloseCombiner(combiner))
		return EOF;

	/*
	 * Ask pooler to send commands (if any) to nodes involved in transaction to alter the
	 * behavior of current transaction. This fires all transaction level commands before
	 * issuing any DDL, DML or SELECT within the current transaction block.
	 */
	if (GetCurrentLocalParamStatus())
	{
		int res;
		if (node_type == PGXC_NODE_DATANODE)
			res = PoolManagerSendLocalCommand(j, con, 0, NULL);
		else
			res = PoolManagerSendLocalCommand(0, NULL, j, con);

		if (res != 0)
			return EOF;
	}
#endif

	/* No problem, let's get going */
	return 0;
}

/*
 * Prepare all remote nodes involved in this transaction. The local node is
 * handled separately and prepared first in xact.c. If there is any error
 * during this phase, it will be reported via ereport() and the transaction
 * will be aborted on the local as well as remote nodes
 *
 * prepareGID is created and passed from xact.c
 */
static bool
pgxc_node_remote_prepare(char *prepareGID)
{
	int         	result = 0;
	int				write_conn_count = remoteXactState.numWriteRemoteNodes;
	char			prepare_cmd[256];
	int				i;
	PGXCNodeHandle	**connections = remoteXactState.remoteNodeHandles;
#ifdef XCP
	ResponseCombiner combiner;
#else
	RemoteQueryState *combiner = NULL;
#endif

	/*
	 * If there is NO write activity or the caller does not want us to run a
	 * 2PC protocol, we don't need to do anything special
	 */
	if ((write_conn_count == 0) || (prepareGID == NULL))
		return false;

	/* Save the prepareGID in the global state information */
	sprintf(remoteXactState.prepareGID, "%s", prepareGID);

	/* Generate the PREPARE TRANSACTION command */
	sprintf(prepare_cmd, "PREPARE TRANSACTION '%s'", remoteXactState.prepareGID);

	for (i = 0; i < write_conn_count; i++)
	{
		/*
		 * PGXCTODO - We should actually make sure that the connection state is
		 * IDLE when we reach here. The executor should have guaranteed that
		 * before the transaction gets to the commit point. For now, consume
		 * the pending data on the connection
		 */
		if (connections[i]->state != DN_CONNECTION_STATE_IDLE)
			BufferConnection(connections[i]);

		/* Clean the previous errors, if any */
		connections[i]->error = NULL;

		/*
		 * Now we are ready to PREPARE the transaction. Any error at this point
		 * can be safely ereport-ed and the transaction will be aborted.
		 */
		if (pgxc_node_send_query(connections[i], prepare_cmd))
		{
			remoteXactState.remoteNodeStatus[i] = RXACT_NODE_PREPARE_FAILED;
			remoteXactState.status = RXACT_PREPARE_FAILED;
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("failed to send PREPARE TRANSACTION command to "
						"the node %u", connections[i]->nodeoid)));
		}
		else
			remoteXactState.remoteNodeStatus[i] = RXACT_NODE_PREPARE_SENT;
	}

	/*
	 * Receive and check for any errors. In case of errors, we don't bail out
	 * just yet. We first go through the list of connections and look for
	 * errors on each connection. This is important to ensure that we run
	 * an appropriate ROLLBACK command later on (prepared transactions must be
	 * rolled back with ROLLBACK PREPARED commands).
	 *
	 * PGXCTODO - There doesn't seem to be a solid mechanism to track errors on
	 * individual connections. The transaction_status field doesn't get set
	 * every time there is an error on the connection. The combiner mechanism is
	 * good for parallel proessing, but I think we should have a leak-proof
	 * mechanism to track connection status
	 */
	if (write_conn_count)
	{
#ifdef XCP
		InitResponseCombiner(&combiner, write_conn_count, COMBINE_TYPE_NONE);
		/* Receive responses */
		result = pgxc_node_receive_responses(write_conn_count, connections, NULL, &combiner);
		if (result || !validate_combiner(&combiner))
			result = EOF;
		else
			CloseCombiner(&combiner);
#else
		combiner = CreateResponseCombiner(write_conn_count, COMBINE_TYPE_NONE);
		/* Receive responses */
		result = pgxc_node_receive_responses(write_conn_count, connections, NULL, combiner);
		if (result || !validate_combiner(combiner))
			result = EOF;
		else
		{
			CloseCombiner(combiner);
			combiner = NULL;
		}
#endif

		for (i = 0; i < write_conn_count; i++)
		{
			if (remoteXactState.remoteNodeStatus[i] == RXACT_NODE_PREPARE_SENT)
			{
				if (connections[i]->error)
				{
					remoteXactState.remoteNodeStatus[i] = RXACT_NODE_PREPARE_FAILED;
					remoteXactState.status = RXACT_PREPARE_FAILED;
				}
				else
					remoteXactState.remoteNodeStatus[i] = RXACT_NODE_PREPARED;
			}
		}
	}

	/*
	 * If we failed to PREPARE on one or more nodes, report an error and let
	 * the normal abort processing take charge of aborting the transaction
	 */
	if (result)
	{
		remoteXactState.status = RXACT_PREPARE_FAILED;
#ifdef XCP
		if (combiner.errorMessage)
		{
			char *code = combiner.errorCode;
			if (combiner.errorDetail != NULL)
				ereport(ERROR,
						(errcode(MAKE_SQLSTATE(code[0], code[1], code[2], code[3], code[4])),
						errmsg("%s", combiner.errorMessage), errdetail("%s", combiner.errorDetail) ));
			else
				ereport(ERROR,
						(errcode(MAKE_SQLSTATE(code[0], code[1], code[2], code[3], code[4])),
						errmsg("%s", combiner.errorMessage)));
		}
#else
		if (combiner && combiner->errorMessage)
		{
			char *code = combiner->errorCode;
			if (combiner->errorDetail != NULL)
				ereport(ERROR,
						(errcode(MAKE_SQLSTATE(code[0], code[1], code[2], code[3], code[4])),
						errmsg("%s", combiner->errorMessage), errdetail("%s", combiner->errorDetail) ));
			else
				ereport(ERROR,
						(errcode(MAKE_SQLSTATE(code[0], code[1], code[2], code[3], code[4])),
						errmsg("%s", combiner->errorMessage)));
		}
#endif
		else
			elog(ERROR, "failed to PREPARE transaction on one or more nodes");
	}

	if (remoteXactState.status == RXACT_PREPARE_FAILED)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("Failed to PREPARE the transaction on one or more nodes")));

	/* Everything went OK. */
	remoteXactState.status = RXACT_PREPARED;
	return result;
}

/*
 * Commit a running or a previously PREPARED transaction on the remote nodes.
 * The local transaction is handled separately in xact.c
 *
 * Once a COMMIT command is sent to any node, the transaction must be finally
 * be committed. But we still report errors via ereport and let
 * AbortTransaction take care of handling partly committed transactions.
 *
 * For 2PC transactions: If local node is involved in the transaction, its
 * already prepared locally and we are in a context of a different transaction
 * (we call it auxulliary transaction) already. So AbortTransaction will
 * actually abort the auxilliary transaction, which is OK. OTOH if the local
 * node is not involved in the main transaction, then we don't care much if its
 * rolled back on the local node as part of abort processing.
 *
 * When 2PC is not used for reasons such as because transaction has accessed
 * some temporary objects, we are already exposed to the risk of committing it
 * one node and aborting on some other node. So such cases need not get more
 * attentions.
 */
static void
pgxc_node_remote_commit(void)
{
	int				result = 0;
	char			commitPrepCmd[256];
	char			commitCmd[256];
	int				write_conn_count = remoteXactState.numWriteRemoteNodes;
	int				read_conn_count = remoteXactState.numReadRemoteNodes;
	PGXCNodeHandle	**connections = remoteXactState.remoteNodeHandles;
	PGXCNodeHandle  *new_connections[write_conn_count + read_conn_count];
	int				new_conn_count = 0;
	int				i;
#ifdef XCP
	ResponseCombiner combiner;
#else
	RemoteQueryState *combiner = NULL;
#endif

	/*
	 * We must handle reader and writer connections both since the transaction
	 * must be closed even on a read-only node
	 */
	if (read_conn_count + write_conn_count == 0)
		return;

	/*
	 * Barrier:
	 *
	 * We should acquire the BarrierLock in SHARE mode here to ensure that
	 * there are no in-progress barrier at this point. This mechanism would
	 * work as long as LWLock mechanism does not starve a EXCLUSIVE lock
	 * requester
	 */
	LWLockAcquire(BarrierLock, LW_SHARED);

	/*
	 * The readers can be committed with a simple COMMIT command. We still need
	 * this to close the transaction block
	 */
	sprintf(commitCmd, "COMMIT TRANSACTION");

	/*
	 * If we are running 2PC, construct a COMMIT command to commit the prepared
	 * transactions
	 */
	if (remoteXactState.status == RXACT_PREPARED)
	{
		sprintf(commitPrepCmd, "COMMIT PREPARED '%s'", remoteXactState.prepareGID);
		/*
		 * If the local node is involved in the transaction, we would have
		 * already prepared it and started a new transaction. We can use the
		 * GXID of the new transaction to run the COMMIT PREPARED commands.
		 * So get an auxilliary GXID only if the local node is not involved
		 */

		if (!GlobalTransactionIdIsValid(remoteXactState.commitXid))
			remoteXactState.commitXid = GetAuxilliaryTransactionId();
	}

	/*
	 * First send GXID if necessary. If there is an error at this stage, the
	 * transaction can be aborted safely because we haven't yet sent COMMIT
	 * command to any participant
	 */
	for (i = 0; i < write_conn_count + read_conn_count; i++)
	{
		if (remoteXactState.remoteNodeStatus[i] == RXACT_NODE_PREPARED)
		{
			Assert(GlobalTransactionIdIsValid(remoteXactState.commitXid));
			if (pgxc_node_send_gxid(connections[i], remoteXactState.commitXid))
			{
				remoteXactState.status = RXACT_COMMIT_FAILED;
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("failed to send GXID for COMMIT PREPARED "
							 "command")));
			}
		}
	}

	/*
	 * Now send the COMMIT command to all the participants
	 */
	for (i = 0; i < write_conn_count + read_conn_count; i++)
	{
		const char *command;

		Assert(remoteXactState.remoteNodeStatus[i] == RXACT_NODE_PREPARED ||
			   remoteXactState.remoteNodeStatus[i] == RXACT_NODE_NONE);

		if (remoteXactState.remoteNodeStatus[i] == RXACT_NODE_PREPARED)
			command = commitPrepCmd;
		else
			command = commitCmd;

		/* Clean the previous errors, if any */
		connections[i]->error = NULL;

		if (pgxc_node_send_query(connections[i], command))
		{
			remoteXactState.remoteNodeStatus[i] = RXACT_NODE_COMMIT_FAILED;
			remoteXactState.status = RXACT_COMMIT_FAILED;

			/*
			 * If the error occurred on the first connection, we still have
			 * chance to abort the whole transaction. We prefer that because
			 * that reduces the need for any manual intervention at least until
			 * we have a automatic mechanism to resolve in-doubt transactions
			 *
			 * XXX We can ideally check for first writer connection, but keep
			 * it simple for now
			 */
			if (i == 0)
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("failed to send COMMIT command to node")));
			else
				add_error_message(connections[i], "failed to send COMMIT "
						"command to node");
		}
		else
		{
			remoteXactState.remoteNodeStatus[i] = RXACT_NODE_COMMIT_SENT;
			new_connections[new_conn_count++] = connections[i];
		}
	}

	/*
	 * Release the BarrierLock.
	 */
	LWLockRelease(BarrierLock);

	if (new_conn_count)
	{
#ifdef XCP
		InitResponseCombiner(&combiner, new_conn_count, COMBINE_TYPE_NONE);
		/* Receive responses */
		result = pgxc_node_receive_responses(new_conn_count, new_connections, NULL, &combiner);
		if (result || !validate_combiner(&combiner))
			result = EOF;
		else
			CloseCombiner(&combiner);
#else
		combiner = CreateResponseCombiner(new_conn_count, COMBINE_TYPE_NONE);
		/* Receive responses */
		result = pgxc_node_receive_responses(new_conn_count, new_connections, NULL, combiner);
		if (result || !validate_combiner(combiner))
			result = EOF;
		else
		{
			CloseCombiner(combiner);
			combiner = NULL;
		}
#endif
		/*
		 * Even if the command failed on some node, don't throw an error just
		 * yet. That gives a chance to look for individual connection status
		 * and record appropriate information for later recovery
		 *
		 * XXX A node once prepared must be able to either COMMIT or ABORT. So a
		 * COMMIT can fail only because of either communication error or because
		 * the node went down. Even if one node commits, the transaction must be
		 * eventually committed on all the nodes.
		 */

		/* At this point, we must be in one the following state */
		Assert(remoteXactState.status == RXACT_COMMIT_FAILED ||
				remoteXactState.status == RXACT_PREPARED ||
				remoteXactState.status == RXACT_NONE);

		/*
		 * Go through every connection and check if COMMIT succeeded or failed on
		 * that connection. If the COMMIT has failed on one node, but succeeded on
		 * some other, such transactions need special attention (by the
		 * administrator for now)
		 */
		for (i = 0; i < write_conn_count + read_conn_count; i++)
		{
			if (remoteXactState.remoteNodeStatus[i] == RXACT_NODE_COMMIT_SENT)
			{
				if (connections[i]->error)
				{
					remoteXactState.remoteNodeStatus[i] = RXACT_NODE_COMMIT_FAILED;
					if (remoteXactState.status != RXACT_PART_COMMITTED)
						remoteXactState.status = RXACT_COMMIT_FAILED;
				}
				else
				{
					remoteXactState.remoteNodeStatus[i] = RXACT_NODE_COMMITTED;
					if (remoteXactState.status == RXACT_COMMIT_FAILED)
						remoteXactState.status = RXACT_PART_COMMITTED;
				}
			}
		}
	}

	stat_transaction(write_conn_count + read_conn_count);

	if (result)
	{
#ifdef XCP
		if (combiner.errorMessage)
		{
			char *code = combiner.errorCode;
			if (combiner.errorDetail != NULL)
				ereport(ERROR,
						(errcode(MAKE_SQLSTATE(code[0], code[1], code[2], code[3], code[4])),
						errmsg("%s", combiner.errorMessage), errdetail("%s", combiner.errorDetail) ));
			else
				ereport(ERROR,
						(errcode(MAKE_SQLSTATE(code[0], code[1], code[2], code[3], code[4])),
						errmsg("%s", combiner.errorMessage)));
		}
#else
		if (combiner && combiner->errorMessage)
		{
			char *code = combiner->errorCode;
			if (combiner->errorDetail != NULL)
				ereport(ERROR,
						(errcode(MAKE_SQLSTATE(code[0], code[1], code[2], code[3], code[4])),
						errmsg("%s", combiner->errorMessage), errdetail("%s", combiner->errorDetail) ));
			else
				ereport(ERROR,
						(errcode(MAKE_SQLSTATE(code[0], code[1], code[2], code[3], code[4])),
						errmsg("%s", combiner->errorMessage)));
		}
#endif
		else
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("Failed to COMMIT the transaction on one or more nodes")));
	}

	if (remoteXactState.status == RXACT_COMMIT_FAILED ||
		remoteXactState.status == RXACT_PART_COMMITTED)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("Failed to COMMIT the transaction on one or more nodes")));

	remoteXactState.status = RXACT_COMMITTED;
}

/*
 * Abort the current transaction on the local and remote nodes. If the
 * transaction is prepared on the remote node, we send a ROLLBACK PREPARED
 * command, otherwise a ROLLBACK command is sent.
 *
 * Note that if the local node was involved and prepared successfully, we are
 * running in a separate transaction context right now
 */
static void
pgxc_node_remote_abort(void)
{
	int				result = 0;
	char			*rollbackCmd = "ROLLBACK TRANSACTION";
	char			rollbackPrepCmd[256];
	int				write_conn_count = remoteXactState.numWriteRemoteNodes;
	int				read_conn_count = remoteXactState.numReadRemoteNodes;
	int				i;
	PGXCNodeHandle	**connections = remoteXactState.remoteNodeHandles;
	PGXCNodeHandle  *new_connections[remoteXactState.numWriteRemoteNodes + remoteXactState.numReadRemoteNodes];
	int				new_conn_count = 0;
#ifdef XCP
	ResponseCombiner combiner;
#else
	RemoteQueryState *combiner = NULL;
#endif


	/* Send COMMIT/ROLLBACK PREPARED TRANSACTION to the remote nodes */
	for (i = 0; i < write_conn_count + read_conn_count; i++)
	{
		RemoteXactNodeStatus status = remoteXactState.remoteNodeStatus[i];

		/* Clean the previous errors, if any */
		connections[i]->error = NULL;

		if ((status == RXACT_NODE_PREPARED) ||
			(status == RXACT_NODE_PREPARE_SENT))
		{
			sprintf(rollbackPrepCmd, "ROLLBACK PREPARED '%s'", remoteXactState.prepareGID);

			if (!GlobalTransactionIdIsValid(remoteXactState.commitXid))
				remoteXactState.commitXid = GetAuxilliaryTransactionId();

			if (pgxc_node_send_gxid(connections[i], remoteXactState.commitXid))
			{
#ifdef XCP
				connections[i]->state = DN_CONNECTION_STATE_ERROR_FATAL;
#endif
				add_error_message(connections[i], "failed to send GXID for "
						"ROLLBACK PREPARED command");
				remoteXactState.remoteNodeStatus[i] = RXACT_NODE_ABORT_FAILED;
				remoteXactState.status = RXACT_ABORT_FAILED;

			}
			else if (pgxc_node_send_query(connections[i], rollbackPrepCmd))
			{
#ifdef XCP
				connections[i]->state = DN_CONNECTION_STATE_ERROR_FATAL;
#endif
				add_error_message(connections[i], "failed to send ROLLBACK PREPARED "
						"TRANSACTION command to node");
				remoteXactState.remoteNodeStatus[i] = RXACT_NODE_ABORT_FAILED;
				remoteXactState.status = RXACT_ABORT_FAILED;
			}
			else
			{
				remoteXactState.remoteNodeStatus[i] = RXACT_NODE_ABORT_SENT;
				new_connections[new_conn_count++] = connections[i];
			}
		}
		else
		{
			if (pgxc_node_send_query(connections[i], rollbackCmd))
			{
#ifdef XCP
				connections[i]->state = DN_CONNECTION_STATE_ERROR_FATAL;
#endif
				add_error_message(connections[i], "failed to send ROLLBACK "
						"TRANSACTION command to node");
				remoteXactState.remoteNodeStatus[i] = RXACT_NODE_ABORT_FAILED;
				remoteXactState.status = RXACT_ABORT_FAILED;
			}
			else
			{
				remoteXactState.remoteNodeStatus[i] = RXACT_NODE_ABORT_SENT;
				new_connections[new_conn_count++] = connections[i];
			}
		}
	}

	if (new_conn_count)
	{
#ifdef XCP
		/* Receive responses */
		InitResponseCombiner(&combiner, new_conn_count, COMBINE_TYPE_NONE);
		result = pgxc_node_receive_responses(new_conn_count, new_connections, NULL, &combiner);
		if (result || !validate_combiner(&combiner))
			result = EOF;
		else
			CloseCombiner(&combiner);
#else
		combiner = CreateResponseCombiner(new_conn_count, COMBINE_TYPE_NONE);
		/* Receive responses */
		result = pgxc_node_receive_responses(new_conn_count, new_connections, NULL, combiner);
		if (result || !validate_combiner(combiner))
			result = EOF;
		else
		{
			CloseCombiner(combiner);
			combiner = NULL;
		}
#endif

		for (i = 0; i < write_conn_count + read_conn_count; i++)
		{
			if (remoteXactState.remoteNodeStatus[i] == RXACT_NODE_ABORT_SENT)
			{
				if (connections[i]->error)
				{
					remoteXactState.remoteNodeStatus[i] = RXACT_NODE_ABORT_FAILED;
					if (remoteXactState.status != RXACT_PART_ABORTED)
						remoteXactState.status = RXACT_ABORT_FAILED;
					elog(LOG, "Failed to ABORT at node %d\nDetail: %s",
							connections[i]->nodeoid, connections[i]->error);
				}
				else
				{
					remoteXactState.remoteNodeStatus[i] = RXACT_NODE_ABORTED;
					if (remoteXactState.status == RXACT_ABORT_FAILED)
						remoteXactState.status = RXACT_PART_ABORTED;
				}
			}
		}
	}

	if (result)
	{
#ifdef XCP
		if (combiner.errorMessage)
		{
			if (combiner.errorDetail != NULL)
				elog(LOG, "%s %s", combiner.errorMessage, combiner.errorDetail);
			else
				elog(LOG, "%s", combiner.errorMessage);
		}
#else
		if (combiner && combiner->errorMessage)
		{
			if (combiner->errorDetail != NULL)
				elog(LOG, "%s %s", combiner->errorMessage, combiner->errorDetail);
			else
				elog(LOG, "%s", combiner->errorMessage);
		}
#endif
		else
			elog(LOG, "Failed to ABORT an implicitly PREPARED "
					"transaction - result %d", result);
	}

	/*
	 * Don't ereport because we might already been abort processing and any
	  * error at this point can lead to infinite recursion
	 *
	 * XXX How do we handle errors reported by internal functions used to
	 * communicate with remote nodes ?
	 */
	if (remoteXactState.status == RXACT_ABORT_FAILED ||
		remoteXactState.status == RXACT_PART_ABORTED)
		elog(LOG, "Failed to ABORT an implicitly PREPARED transaction "
				"status - %d", remoteXactState.status);
	else
		remoteXactState.status = RXACT_ABORTED;

	return;
}

/*
 * Begin COPY command
 * The copy_connections array must have room for NumDataNodes items
 */
#ifdef XCP
Locator*
DataNodeCopyBegin(const char *query, RelationLocInfo *rel_loc,
								  Oid partType, bool is_from)
{
	int i;
	List *nodelist = rel_loc->nodeList;
	PGXCNodeHandle **connections;
	bool need_tran_block;
	GlobalTransactionId gxid;
	ResponseCombiner combiner;
	Snapshot snapshot = GetActiveSnapshot();
	int conn_count = list_length(nodelist);
	Locator *result;

	/* Get needed datanode connections */
	if (!is_from && IsReplicated(rel_loc->locatorType))
	{
		/* Connections is a single handle to read from */
		connections = (PGXCNodeHandle **) palloc(sizeof(PGXCNodeHandle *));
		connections[0] = get_any_handle(nodelist);
		conn_count = 1;
	}
	else
	{
		PGXCNodeAllHandles *pgxc_handles;
		pgxc_handles = get_handles(nodelist, NULL, false);
		connections = pgxc_handles->datanode_handles;
		Assert(pgxc_handles->dn_conn_count == conn_count);
		pfree(pgxc_handles);
	}

	/*
	 * If more than one nodes are involved or if we are already in a
	 * transaction block, we must the remote statements in a transaction block
	 */
	need_tran_block = (conn_count > 1) || (TransactionBlockStatusCode() == 'T');

	elog(DEBUG1, "conn_count = %d, need_tran_block = %s", conn_count,
			need_tran_block ? "true" : "false");

	/* Gather statistics */
	stat_statement();
	stat_transaction(conn_count);

	gxid = GetCurrentTransactionId();
	if (!GlobalTransactionIdIsValid(gxid))
	{
		pfree(connections);
		return NULL;
	}

	/* Start transaction on connections where it is not started */
	if (pgxc_node_begin(conn_count, connections, gxid, need_tran_block, false, PGXC_NODE_DATANODE))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("Could not begin transaction on data nodes.")));
	}

	/*
	 * COPY TO do not use locator, it just takes connections from it, and
	 * we do not look up distribution data type in this case.
	 * So always use LOCATOR_TYPE_RROBIN to avoid errors because of not
	 * defined partType if real locator type is HASH or MODULO.
	 * Create locator before sending down query, because createLocator may
	 * fail and we leave with dirty connections.
	 * If we get an error now datanode connection will be clean and error
	 * handler will issue transaction abort.
	 */
	result = createLocator(is_from ? rel_loc->locatorType : LOCATOR_TYPE_RROBIN,
					 is_from ? RELATION_ACCESS_INSERT : RELATION_ACCESS_READ,
						 partType,
						 LOCATOR_LIST_POINTER,
						 conn_count,
						 (void *) connections,
						 NULL,
						 false);

	/* Send query to nodes */
	for (i = 0; i < conn_count; i++)
	{
		CHECK_OWNERSHIP(connections[i], NULL);

		if (snapshot && pgxc_node_send_snapshot(connections[i], snapshot))
		{
			add_error_message(connections[i], "Can not send request");
			pfree(connections);
			freeLocator(result);
			return NULL;
		}
		if (pgxc_node_send_query(connections[i], query) != 0)
		{
			add_error_message(connections[i], "Can not send request");
			pfree(connections);
			freeLocator(result);
			return NULL;
		}
	}

	/*
	 * We are expecting CopyIn response, but do not want to send it to client,
	 * caller should take care about this, because here we do not know if
	 * client runs console or file copy
	 */
	InitResponseCombiner(&combiner, conn_count, COMBINE_TYPE_NONE);
	/*
	 * Make sure there are zeroes in unused fields
	 */
	memset(&combiner, 0, sizeof(ScanState));

	/* Receive responses */
	if (pgxc_node_receive_responses(conn_count, connections, NULL, &combiner)
			|| !ValidateAndCloseCombiner(&combiner))
	{
		DataNodeCopyFinish(conn_count, connections);
		release_handles();
		freeLocator(result);
		return NULL;
	}
	pfree(connections);
	return result;
}
#else
PGXCNodeHandle**
DataNodeCopyBegin(const char *query, List *nodelist, Snapshot snapshot, bool is_from)
{
	int i;
	int conn_count = list_length(nodelist) == 0 ? NumDataNodes : list_length(nodelist);
	struct timeval *timeout = NULL;
	PGXCNodeAllHandles *pgxc_handles;
	PGXCNodeHandle **connections;
	PGXCNodeHandle **copy_connections;
	ListCell *nodeitem;
	bool need_tran_block;
	GlobalTransactionId gxid;
	RemoteQueryState *combiner;

	if (conn_count == 0)
		return NULL;

	/* Get needed datanode connections */
	pgxc_handles = get_handles(nodelist, NULL, false);
	connections = pgxc_handles->datanode_handles;

	if (!connections)
		return NULL;

	/*
	 * If more than one nodes are involved or if we are already in a
	 * transaction block, we must the remote statements in a transaction block
	 */
	need_tran_block = (conn_count > 1) || (TransactionBlockStatusCode() == 'T');

	elog(DEBUG1, "conn_count = %d, need_tran_block = %s", conn_count,
			need_tran_block ? "true" : "false");

	/*
	 * We need to be able quickly find a connection handle for specified node number,
	 * So store connections in an array where index is node-1.
	 * Unused items in the array should be NULL
	 */
	copy_connections = (PGXCNodeHandle **) palloc0(NumDataNodes * sizeof(PGXCNodeHandle *));
	i = 0;
	foreach(nodeitem, nodelist)
		copy_connections[lfirst_int(nodeitem)] = connections[i++];

	/* Gather statistics */
	stat_statement();
	stat_transaction(conn_count);

	gxid = GetCurrentTransactionId();

	if (!GlobalTransactionIdIsValid(gxid))
	{
		pfree_pgxc_all_handles(pgxc_handles);
		pfree(copy_connections);
		return NULL;
	}

	/* Start transaction on connections where it is not started */
	if (pgxc_node_begin(conn_count, connections, gxid, need_tran_block, false, PGXC_NODE_DATANODE))
	{
		pfree_pgxc_all_handles(pgxc_handles);
		pfree(copy_connections);
		return NULL;
	}

	/* Send query to nodes */
	for (i = 0; i < conn_count; i++)
	{
		if (connections[i]->state == DN_CONNECTION_STATE_QUERY)
			BufferConnection(connections[i]);

		if (snapshot && pgxc_node_send_snapshot(connections[i], snapshot))
		{
			add_error_message(connections[i], "Can not send request");
			pfree_pgxc_all_handles(pgxc_handles);
			pfree(copy_connections);
			return NULL;
		}
		if (pgxc_node_send_query(connections[i], query) != 0)
		{
			add_error_message(connections[i], "Can not send request");
			pfree_pgxc_all_handles(pgxc_handles);
			pfree(copy_connections);
			return NULL;
		}
	}

	/*
	 * We are expecting CopyIn response, but do not want to send it to client,
	 * caller should take care about this, because here we do not know if
	 * client runs console or file copy
	 */
	combiner = CreateResponseCombiner(conn_count, COMBINE_TYPE_NONE);

	/* Receive responses */
	if (pgxc_node_receive_responses(conn_count, connections, timeout, combiner)
			|| !ValidateAndCloseCombiner(combiner))
	{
		DataNodeCopyFinish(connections, -1, COMBINE_TYPE_NONE);
		pfree(connections);
		pfree(copy_connections);
		return NULL;
	}
	pfree(connections);
	return copy_connections;
}
#endif


/*
 * Send a data row to the specified nodes
 */
#ifdef XCP
int
DataNodeCopyIn(char *data_row, int len, int conn_count, PGXCNodeHandle** copy_connections)
{
	/* size + data row + \n */
	int msgLen = 4 + len + 1;
	int nLen = htonl(msgLen);
	int i;

	for(i = 0; i < conn_count; i++)
	{
		PGXCNodeHandle *handle = copy_connections[i];
		if (handle->state == DN_CONNECTION_STATE_COPY_IN)
		{
			/* precalculate to speed up access */
			int bytes_needed = handle->outEnd + 1 + msgLen;

			/* flush buffer if it is almost full */
			if (bytes_needed > COPY_BUFFER_SIZE)
			{
				int to_send = handle->outEnd;

				/* First look if data node has sent a error message */
				int read_status = pgxc_node_read_data(handle, true);
				if (read_status == EOF || read_status < 0)
				{
					add_error_message(handle, "failed to read data from data node");
					return EOF;
				}

				if (handle->inStart < handle->inEnd)
				{
					ResponseCombiner combiner;
					InitResponseCombiner(&combiner, 1, COMBINE_TYPE_NONE);
					/*
					 * Make sure there are zeroes in unused fields
					 */
					memset(&combiner, 0, sizeof(ScanState));
					handle_response(handle, &combiner);
					if (!ValidateAndCloseCombiner(&combiner))
						return EOF;
				}

				if (DN_CONNECTION_STATE_ERROR(handle))
					return EOF;

				/*
				 * Try to send down buffered data if we have
				 */
				if (to_send && send_some(handle, to_send) < 0)
				{
					add_error_message(handle, "failed to send data to data node");
					return EOF;
				}
			}

			if (ensure_out_buffer_capacity(bytes_needed, handle) != 0)
			{
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory")));
			}

			handle->outBuffer[handle->outEnd++] = 'd';
			memcpy(handle->outBuffer + handle->outEnd, &nLen, 4);
			handle->outEnd += 4;
			memcpy(handle->outBuffer + handle->outEnd, data_row, len);
			handle->outEnd += len;
			handle->outBuffer[handle->outEnd++] = '\n';
		}
		else
		{
			add_error_message(handle, "Invalid data node connection");
			return EOF;
		}
	}
	return 0;
}
#else
int
DataNodeCopyIn(char *data_row, int len, ExecNodes *exec_nodes, PGXCNodeHandle** copy_connections)
{
	PGXCNodeHandle *primary_handle = NULL;
	ListCell *nodeitem;
	/* size + data row + \n */
	int msgLen = 4 + len + 1;
	int nLen = htonl(msgLen);

	if (exec_nodes->primarynodelist)
	{
		primary_handle = copy_connections[lfirst_int(list_head(exec_nodes->primarynodelist))];
	}

	if (primary_handle)
	{
		if (primary_handle->state == DN_CONNECTION_STATE_COPY_IN)
		{
			/* precalculate to speed up access */
			int bytes_needed = primary_handle->outEnd + 1 + msgLen;

			/* flush buffer if it is almost full */
			if (bytes_needed > COPY_BUFFER_SIZE)
			{
				/* First look if data node has sent a error message */
				int read_status = pgxc_node_read_data(primary_handle, true);
				if (read_status == EOF || read_status < 0)
				{
					add_error_message(primary_handle, "failed to read data from data node");
					return EOF;
				}

				if (primary_handle->inStart < primary_handle->inEnd)
				{
					RemoteQueryState *combiner = CreateResponseCombiner(1, COMBINE_TYPE_NONE);
					handle_response(primary_handle, combiner);
					if (!ValidateAndCloseCombiner(combiner))
						return EOF;
				}

				if (DN_CONNECTION_STATE_ERROR(primary_handle))
					return EOF;

				if (send_some(primary_handle, primary_handle->outEnd) < 0)
				{
					add_error_message(primary_handle, "failed to send data to data node");
					return EOF;
				}
			}

			if (ensure_out_buffer_capacity(bytes_needed, primary_handle) != 0)
			{
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory")));
			}

			primary_handle->outBuffer[primary_handle->outEnd++] = 'd';
			memcpy(primary_handle->outBuffer + primary_handle->outEnd, &nLen, 4);
			primary_handle->outEnd += 4;
			memcpy(primary_handle->outBuffer + primary_handle->outEnd, data_row, len);
			primary_handle->outEnd += len;
			primary_handle->outBuffer[primary_handle->outEnd++] = '\n';
		}
		else
		{
			add_error_message(primary_handle, "Invalid data node connection");
			return EOF;
		}
	}

	foreach(nodeitem, exec_nodes->nodeList)
	{
		PGXCNodeHandle *handle = copy_connections[lfirst_int(nodeitem)];
		if (handle && handle->state == DN_CONNECTION_STATE_COPY_IN)
		{
			/* precalculate to speed up access */
			int bytes_needed = handle->outEnd + 1 + msgLen;

			/* flush buffer if it is almost full */
			if ((primary_handle && bytes_needed > PRIMARY_NODE_WRITEAHEAD)
					|| (!primary_handle && bytes_needed > COPY_BUFFER_SIZE))
			{
				int to_send = handle->outEnd;

				/* First look if data node has sent a error message */
				int read_status = pgxc_node_read_data(handle, true);
				if (read_status == EOF || read_status < 0)
				{
					add_error_message(handle, "failed to read data from data node");
					return EOF;
				}

				if (handle->inStart < handle->inEnd)
				{
					RemoteQueryState *combiner = CreateResponseCombiner(1, COMBINE_TYPE_NONE);
					handle_response(handle, combiner);
					if (!ValidateAndCloseCombiner(combiner))
						return EOF;
				}

				if (DN_CONNECTION_STATE_ERROR(handle))
					return EOF;

				/*
				 * Allow primary node to write out data before others.
				 * If primary node was blocked it would not accept copy data.
				 * So buffer at least PRIMARY_NODE_WRITEAHEAD at the other nodes.
				 * If primary node is blocked and is buffering, other buffers will
				 * grow accordingly.
				 */
				if (primary_handle)
				{
					if (primary_handle->outEnd + PRIMARY_NODE_WRITEAHEAD < handle->outEnd)
						to_send = handle->outEnd - primary_handle->outEnd - PRIMARY_NODE_WRITEAHEAD;
					else
						to_send = 0;
				}

				/*
				 * Try to send down buffered data if we have
				 */
				if (to_send && send_some(handle, to_send) < 0)
				{
					add_error_message(handle, "failed to send data to data node");
					return EOF;
				}
			}

			if (ensure_out_buffer_capacity(bytes_needed, handle) != 0)
			{
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory")));
			}

			handle->outBuffer[handle->outEnd++] = 'd';
			memcpy(handle->outBuffer + handle->outEnd, &nLen, 4);
			handle->outEnd += 4;
			memcpy(handle->outBuffer + handle->outEnd, data_row, len);
			handle->outEnd += len;
			handle->outBuffer[handle->outEnd++] = '\n';
		}
		else
		{
			add_error_message(handle, "Invalid data node connection");
			return EOF;
		}
	}
	return 0;
}
#endif


#ifdef XCP
uint64
DataNodeCopyOut(PGXCNodeHandle** copy_connections,
							  int conn_count, FILE* copy_file)
{
	ResponseCombiner combiner;
	uint64		processed;
	bool 		error;

	InitResponseCombiner(&combiner, conn_count, COMBINE_TYPE_SUM);
	/*
	 * Make sure there are zeroes in unused fields
	 */
	memset(&combiner, 0, sizeof(ScanState));
	combiner.processed = 0;
	/* If there is an existing file where to copy data, pass it to combiner */
	if (copy_file)
		combiner.copy_file = copy_file;

	error = (pgxc_node_receive_responses(conn_count, copy_connections, NULL, &combiner) != 0);

	processed = combiner.processed;

	if (!ValidateAndCloseCombiner(&combiner) || error)
	{
		if (!PersistentConnections)
			release_handles();
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("Unexpected response from the data nodes when combining, request type %d", combiner.request_type)));
	}

	return processed;
}
#else
uint64
DataNodeCopyOut(ExecNodes *exec_nodes, PGXCNodeHandle** copy_connections, FILE* copy_file)
{
	RemoteQueryState *combiner;
	int 		conn_count = list_length(exec_nodes->nodeList) == 0 ? NumDataNodes : list_length(exec_nodes->nodeList);
	int 		count = 0;
	List		*nodelist;
	ListCell	*nodeitem;
	uint64		processed;

	nodelist = exec_nodes->nodeList;

	combiner = CreateResponseCombiner(conn_count, COMBINE_TYPE_SUM);
	combiner->processed = 0;
	/* If there is an existing file where to copy data, pass it to combiner */
	if (copy_file)
		combiner->copy_file = copy_file;

	foreach(nodeitem, exec_nodes->nodeList)
	{
		PGXCNodeHandle *handle = copy_connections[count];
		count++;

		if (handle && handle->state == DN_CONNECTION_STATE_COPY_OUT)
		{
			int read_status = 0;
			/* H message has been consumed, continue to manage data row messages */
			while (read_status >= 0 && handle->state == DN_CONNECTION_STATE_COPY_OUT) /* continue to read as long as there is data */
			{
				if (handle_response(handle,combiner) == RESPONSE_EOF)
				{
					/* read some extra-data */
					read_status = pgxc_node_read_data(handle, true);
					if (read_status < 0)
						ereport(ERROR,
								(errcode(ERRCODE_CONNECTION_FAILURE),
								 errmsg("unexpected EOF on datanode connection")));
					else
						/*
						 * Set proper connection status - handle_response
						 * has changed it to DN_CONNECTION_STATE_QUERY
						 */
						handle->state = DN_CONNECTION_STATE_COPY_OUT;
				}
				/* There is no more data that can be read from connection */
			}
		}
	}

	processed = combiner->processed;

	if (!ValidateAndCloseCombiner(combiner))
	{
		if (!PersistentConnections)
			release_handles();
		pfree(copy_connections);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("Unexpected response from the data nodes when combining, request type %d", combiner->request_type)));
	}

	return processed;
}
#endif


/*
 * Finish copy process on all connections
 */
#ifdef XCP
void
DataNodeCopyFinish(int conn_count, PGXCNodeHandle** connections)
#else
void
DataNodeCopyFinish(PGXCNodeHandle** copy_connections, int primary_dn_index, CombineType combine_type)
#endif
{
	int		i;
#ifdef XCP
	ResponseCombiner combiner;
#else
	RemoteQueryState *combiner = NULL;
#endif
	bool 		error = false;
#ifndef XCP
	struct timeval *timeout = NULL; /* wait forever */
	PGXCNodeHandle *connections[NumDataNodes];
	PGXCNodeHandle *primary_handle = NULL;
	int 		conn_count = 0;

	for (i = 0; i < NumDataNodes; i++)
	{
		PGXCNodeHandle *handle = copy_connections[i];

		if (!handle)
			continue;

		if (i == primary_dn_index)
			primary_handle = handle;
		else
			connections[conn_count++] = handle;
	}

	if (primary_handle)
	{
		error = true;
		if (primary_handle->state == DN_CONNECTION_STATE_COPY_IN || primary_handle->state == DN_CONNECTION_STATE_COPY_OUT)
			error = DataNodeCopyEnd(primary_handle, false);

		combiner = CreateResponseCombiner(conn_count + 1, combine_type);
		error = (pgxc_node_receive_responses(1, &primary_handle, timeout, combiner) != 0) || error;
	}
#endif

	for (i = 0; i < conn_count; i++)
	{
		PGXCNodeHandle *handle = connections[i];

		error = true;
		if (handle->state == DN_CONNECTION_STATE_COPY_IN || handle->state == DN_CONNECTION_STATE_COPY_OUT)
			error = DataNodeCopyEnd(handle, false);
	}

#ifdef XCP
	InitResponseCombiner(&combiner, conn_count, COMBINE_TYPE_NONE);
	/*
	 * Make sure there are zeroes in unused fields
	 */
	memset(&combiner, 0, sizeof(ScanState));
	error = (pgxc_node_receive_responses(conn_count, connections, NULL, &combiner) != 0) || error;

	if (!ValidateAndCloseCombiner(&combiner) || error)
#else
	if (!combiner)
		combiner = CreateResponseCombiner(conn_count, combine_type);
	error = (pgxc_node_receive_responses(conn_count, connections, timeout, combiner) != 0) || error;

	if (!ValidateAndCloseCombiner(combiner) || error)
#endif
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("Error while running COPY")));
}

/*
 * End copy process on a connection
 */
bool
DataNodeCopyEnd(PGXCNodeHandle *handle, bool is_error)
{
	int 		nLen = htonl(4);

	if (handle == NULL)
		return true;

	/* msgType + msgLen */
	if (ensure_out_buffer_capacity(handle->outEnd + 1 + 4, handle) != 0)
		return true;

	if (is_error)
		handle->outBuffer[handle->outEnd++] = 'f';
	else
		handle->outBuffer[handle->outEnd++] = 'c';

	memcpy(handle->outBuffer + handle->outEnd, &nLen, 4);
	handle->outEnd += 4;

	/* We need response right away, so send immediately */
	if (pgxc_node_flush(handle) < 0)
		return true;

	return false;
}


#ifndef XCP
RemoteQueryState *
ExecInitRemoteQuery(RemoteQuery *node, EState *estate, int eflags)
{
	RemoteQueryState   *remotestate;

	/* RemoteQuery node is the leaf node in the plan tree, just like seqscan */
	Assert(innerPlan(node) == NULL);
	Assert(outerPlan(node) == NULL);

	remotestate = CreateResponseCombiner(0, node->combine_type);
	remotestate->ss.ps.plan = (Plan *) node;
	remotestate->ss.ps.state = estate;

	remotestate->ss.ps.qual = (List *)
		ExecInitExpr((Expr *) node->scan.plan.qual,
					 (PlanState *) remotestate);

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_MARK)));

	/* Extract the eflags bits that are relevant for tuplestorestate */
	remotestate->eflags = (eflags & (EXEC_FLAG_REWIND | EXEC_FLAG_BACKWARD));

	/* We anyways have to support REWIND for ReScan */
	remotestate->eflags |= EXEC_FLAG_REWIND;

	remotestate->eof_underlying = false;
	remotestate->tuplestorestate = NULL;

	ExecInitResultTupleSlot(estate, &remotestate->ss.ps);
	if (node->scan.plan.targetlist)
	{
		TupleDesc typeInfo = ExecCleanTypeFromTL(node->scan.plan.targetlist, false);
		ExecSetSlotDescriptor(remotestate->ss.ps.ps_ResultTupleSlot, typeInfo);
	}
	else
	{
		/* In case there is no target list, force its creation */
		ExecAssignResultTypeFromTL(&remotestate->ss.ps);
	}

	ExecInitScanTupleSlot(estate, &remotestate->ss);

	remotestate->ss.ps.ps_TupFromTlist = false;

	/*
	 * If there are parameters supplied, get them into a form to be sent to the
	 * datanodes with bind message. We should not have had done this before.
	 */
	if (estate->es_param_list_info)
	{
		Assert(!remotestate->paramval_data);
		remotestate->paramval_len = ParamListToDataRow(estate->es_param_list_info,
												&remotestate->paramval_data);
	}

	/* We need expression context to evaluate */
	if (node->exec_nodes && node->exec_nodes->en_expr)
		ExecAssignExprContext(estate, &remotestate->ss.ps);
	else if (remotestate->ss.ps.qual)
		ExecAssignExprContext(estate, &remotestate->ss.ps);

	return remotestate;
}


static void
copy_slot(RemoteQueryState *node, TupleTableSlot *src, TupleTableSlot *dst)
{
	if (src->tts_dataRow
			&& dst->tts_tupleDescriptor->natts == src->tts_tupleDescriptor->natts)
	{
		if (src->tts_mcxt == dst->tts_mcxt)
		{
			/* now dst slot controls the backing message */
			ExecStoreDataRowTuple(src->tts_dataRow, src->tts_dataLen, dst,
								  src->tts_shouldFreeRow);
			src->tts_shouldFreeRow = false;
		}
		else
		{
			/* have to make a copy */
			MemoryContext	oldcontext = MemoryContextSwitchTo(dst->tts_mcxt);
			int 		len = src->tts_dataLen;
			char		*msg = (char *) palloc(len);

			memcpy(msg, src->tts_dataRow, len);
			ExecStoreDataRowTuple(msg, len, dst, true);
			MemoryContextSwitchTo(oldcontext);
		}
	}
	else
	{
		int i;

		/*
		 * Data node may be sending junk columns which are always at the end,
		 * but it must not be shorter then result slot.
		 */
		Assert(dst->tts_tupleDescriptor->natts <= src->tts_tupleDescriptor->natts);
		ExecClearTuple(dst);
		slot_getallattrs(src);
		/*
		 * PGXCTODO revisit: if it is correct to copy Datums using assignment?
		 */
		for (i = 0; i < dst->tts_tupleDescriptor->natts; i++)
		{
			dst->tts_values[i] = src->tts_values[i];
			dst->tts_isnull[i] = src->tts_isnull[i];
		}
		ExecStoreVirtualTuple(dst);
	}
}
#endif


/*
 * Get Node connections depending on the connection type:
 * Datanodes Only, Coordinators only or both types
 */
static PGXCNodeAllHandles *
get_exec_connections(RemoteQueryState *planstate,
					 ExecNodes *exec_nodes,
					 RemoteQueryExecType exec_type)
{
	List 	   *nodelist = NIL;
	List 	   *primarynode = NIL;
	List	   *coordlist = NIL;
	PGXCNodeHandle *primaryconnection;
	int			co_conn_count, dn_conn_count;
	bool		is_query_coord_only = false;
	PGXCNodeAllHandles *pgxc_handles = NULL;

	/*
	 * If query is launched only on Coordinators, we have to inform get_handles
	 * not to ask for Datanode connections even if list of Datanodes is NIL.
	 */
	if (exec_type == EXEC_ON_COORDS)
		is_query_coord_only = true;

#ifdef XCP
	if (exec_type == EXEC_ON_CURRENT)
		return get_current_handles();
#endif

	if (exec_nodes)
	{
#ifndef XCP
		if (exec_nodes->en_expr)
		{
			/* execution time determining of target data nodes */
			bool isnull;
			ExprState *estate = ExecInitExpr(exec_nodes->en_expr,
											 (PlanState *) planstate);
			Datum partvalue = ExecEvalExpr(estate,
										   planstate->ss.ps.ps_ExprContext,
										   &isnull,
										   NULL);
			RelationLocInfo *rel_loc_info = GetRelationLocInfo(exec_nodes->en_relid);
			/* PGXCTODO what is the type of partvalue here */
			ExecNodes *nodes = GetRelationNodes(rel_loc_info,
												partvalue,
												isnull,
												exprType((Node *) exec_nodes->en_expr),
												exec_nodes->accesstype);
			if (nodes)
			{
				nodelist = nodes->nodeList;
				primarynode = nodes->primarynodelist;
				pfree(nodes);
			}
			FreeRelationLocInfo(rel_loc_info);
		}
		else if (OidIsValid(exec_nodes->en_relid))
		{
			RelationLocInfo *rel_loc_info = GetRelationLocInfo(exec_nodes->en_relid);
			ExecNodes *nodes = GetRelationNodes(rel_loc_info, 0, true, InvalidOid, exec_nodes->accesstype);

			/* Use the obtained list for given table */
			if (nodes)
				nodelist = nodes->nodeList;

			/*
			 * Special handling for ROUND ROBIN distributed tables. The target
			 * node must be determined at the execution time
			 */
			if (rel_loc_info->locatorType == LOCATOR_TYPE_RROBIN && nodes)
			{
				nodelist = nodes->nodeList;
				primarynode = nodes->primarynodelist;
			}
			else if (nodes)
			{
				if (exec_type == EXEC_ON_DATANODES || exec_type == EXEC_ON_ALL_NODES)
					nodelist = exec_nodes->nodeList;
			}

			if (nodes)
				pfree(nodes);
			FreeRelationLocInfo(rel_loc_info);
		}
		else
#endif
		{
			if (exec_type == EXEC_ON_DATANODES || exec_type == EXEC_ON_ALL_NODES)
				nodelist = exec_nodes->nodeList;
			else if (exec_type == EXEC_ON_COORDS)
				coordlist = exec_nodes->nodeList;

			primarynode = exec_nodes->primarynodelist;
		}
	}

	/* Set node list and DN number */
	if (list_length(nodelist) == 0 &&
		(exec_type == EXEC_ON_ALL_NODES ||
		 exec_type == EXEC_ON_DATANODES))
	{
		/* Primary connection is included in this number of connections if it exists */
		dn_conn_count = NumDataNodes;
	}
	else
	{
		if (exec_type == EXEC_ON_DATANODES || exec_type == EXEC_ON_ALL_NODES)
		{
			if (primarynode)
				dn_conn_count = list_length(nodelist) + 1;
			else
				dn_conn_count = list_length(nodelist);
		}
		else
			dn_conn_count = 0;
	}

	/* Set Coordinator list and coordinator number */
	if ((list_length(nodelist) == 0 && exec_type == EXEC_ON_ALL_NODES) ||
		(list_length(coordlist) == 0 && exec_type == EXEC_ON_COORDS))
	{
		coordlist = GetAllCoordNodes();
		co_conn_count = list_length(coordlist);
	}
	else
	{
		if (exec_type == EXEC_ON_COORDS)
			co_conn_count = list_length(coordlist);
		else
			co_conn_count = 0;
	}

	/* Get other connections (non-primary) */
	pgxc_handles = get_handles(nodelist, coordlist, is_query_coord_only);
	if (!pgxc_handles)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("Could not obtain connection from pool")));

	/* Get connection for primary node, if used */
	if (primarynode)
	{
		/* Let's assume primary connection is always a datanode connection for the moment */
		PGXCNodeAllHandles *pgxc_conn_res;
		pgxc_conn_res = get_handles(primarynode, NULL, false);

		/* primary connection is unique */
		primaryconnection = pgxc_conn_res->datanode_handles[0];

		pfree(pgxc_conn_res);

		if (!primaryconnection)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("Could not obtain connection from pool")));
		pgxc_handles->primary_handle = primaryconnection;
	}

	/* Depending on the execution type, we still need to save the initial node counts */
	pgxc_handles->dn_conn_count = dn_conn_count;
	pgxc_handles->co_conn_count = co_conn_count;

	return pgxc_handles;
}


static bool
pgxc_start_command_on_connection(PGXCNodeHandle *connection,
									RemoteQueryState *remotestate,
									Snapshot snapshot)
{
#ifdef XCP
	ResponseCombiner *combiner = (ResponseCombiner *) remotestate;
	RemoteQuery	*step = (RemoteQuery *) combiner->ss.ps.plan;
	CHECK_OWNERSHIP(connection, combiner);
#else
	RemoteQuery	*step = (RemoteQuery *) remotestate->ss.ps.plan;
	if (connection->state == DN_CONNECTION_STATE_QUERY)
		BufferConnection(connection);
#endif

	if (snapshot && pgxc_node_send_snapshot(connection, snapshot))
		return false;
	if (step->statement || step->cursor || step->param_types)
	{
		/* need to use Extended Query Protocol */
		int	fetch = 0;
		bool	prepared = false;

#ifndef XCP
		/* if prepared statement is referenced see if it is already exist */
		if (step->statement)
			prepared = ActivateDatanodeStatementOnNode(step->statement,
													   PGXCNodeGetNodeId(connection->nodeoid,
																		 PGXC_NODE_DATANODE));
#endif
		/*
		 * execute and fetch rows only if they will be consumed
		 * immediately by the sorter
		 */
		if (step->cursor)
			fetch = 1;

#ifdef XCP
		combiner->extended_query = true;
#endif

		if (pgxc_node_send_query_extended(connection,
							prepared ? NULL : step->sql_statement,
							step->statement,
							step->cursor,
							step->num_params,
							step->param_types,
							remotestate->paramval_len,
							remotestate->paramval_data,
							step->read_only,
							fetch) != 0)
			return false;
	}
	else
	{
		if (pgxc_node_send_query(connection, step->sql_statement) != 0)
			return false;
	}
	return true;
}


#ifndef XCP
static void
do_query(RemoteQueryState *node)
{
	RemoteQuery		*step = (RemoteQuery *) node->ss.ps.plan;
	TupleTableSlot		*scanslot = node->ss.ss_ScanTupleSlot;
	bool			force_autocommit = step->force_autocommit;
	bool			is_read_only = step->read_only;
	GlobalTransactionId	gxid = InvalidGlobalTransactionId;
	Snapshot		snapshot = GetActiveSnapshot();
	PGXCNodeHandle		**connections = NULL;
	PGXCNodeHandle		*primaryconnection = NULL;
	int			i;
	int			regular_conn_count;
	int			total_conn_count;
	bool			need_tran_block;
	PGXCNodeAllHandles	*pgxc_connections;

	/*
	 * Remember if the remote query is accessing a temp object
	 *
	 * !! PGXC TODO Check if the is_temp flag is propogated correctly when a
	 * remote join is reduced
	 */
	if (step->is_temp)
		ExecSetTempObjectIncluded();

	/*
	 * Get connections for Datanodes only, utilities and DDLs
	 * are launched in ExecRemoteUtility
	 */
	pgxc_connections = get_exec_connections(node, step->exec_nodes, step->exec_type);

	if (step->exec_type == EXEC_ON_DATANODES)
	{
		connections = pgxc_connections->datanode_handles;
		total_conn_count = regular_conn_count = pgxc_connections->dn_conn_count;
	}
	else if (step->exec_type == EXEC_ON_COORDS)
	{
		connections = pgxc_connections->coord_handles;
		total_conn_count = regular_conn_count = pgxc_connections->co_conn_count;
	}

	primaryconnection = pgxc_connections->primary_handle;

	/*
	 * Primary connection is counted separately but is included in total_conn_count if used.
	 */
	if (primaryconnection)
		regular_conn_count--;

	pfree(pgxc_connections);

	/*
	 * We save only regular connections, at the time we exit the function
	 * we finish with the primary connection and deal only with regular
	 * connections on subsequent invocations
	 */
	node->node_count = regular_conn_count;

	if (force_autocommit)
		need_tran_block = false;
	else
		need_tran_block = true;
	/*
	 * XXX We are forcing a transaction block for every remote query. We can
	 * get smarter here and avoid a transaction block if all of the following
	 * conditions are true:
	 *
	 * 	- there is only one writer node involved in the transaction (including
	 * 	the local node)
	 * 	- the statement being executed on the remote writer node is a single
	 * 	step statement. IOW, coordinator must not send multiple queries to the
	 * 	remote node.
	 *
	 * 	Once we have leak-proof mechanism to enforce these constraints, we
	 * 	should relax the transaction block requirement.
	 *
	   need_tran_block = (!is_read_only && total_conn_count > 1) ||
	   					 (TransactionBlockStatusCode() == 'T');
	 */

	elog(DEBUG1, "has primary = %s, regular_conn_count = %d, "
				 "need_tran_block = %s", primaryconnection ? "true" : "false",
				 regular_conn_count, need_tran_block ? "true" : "false");

	stat_statement();
	stat_transaction(total_conn_count);

	gxid = GetCurrentTransactionId();

	if (!GlobalTransactionIdIsValid(gxid))
	{
		if (primaryconnection)
			pfree(primaryconnection);
		pfree(connections);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("Failed to get next transaction ID")));
	}

	/* See if we have a primary node, execute on it first before the others */
	if (primaryconnection)
	{
		if (pgxc_node_begin(1, &primaryconnection, gxid, need_tran_block,
					is_read_only, PGXC_NODE_DATANODE))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("Could not begin transaction on primary data node.")));

		if (!pgxc_start_command_on_connection(primaryconnection, node, snapshot))
		{
			pfree(connections);
			pfree(primaryconnection);
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("Failed to send command to data nodes")));
		}
		Assert(node->combine_type == COMBINE_TYPE_SAME);

		/* Make sure the command is completed on the primary node */
		while (true)
		{
			int res;
			if (pgxc_node_receive(1, &primaryconnection, NULL))
				break;

			res = handle_response(primaryconnection, node);
			if (res == RESPONSE_COMPLETE)
				break;
			else if (res == RESPONSE_EOF)
				continue;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("Unexpected response from data node")));
		}
		if (node->errorMessage)
		{
			char *code = node->errorCode;
			if (node->errorDetail != NULL)
				ereport(ERROR,
						(errcode(MAKE_SQLSTATE(code[0], code[1], code[2], code[3], code[4])),
						errmsg("%s", node->errorMessage), errdetail("%s", node->errorDetail) ));
			else
				ereport(ERROR,
						(errcode(MAKE_SQLSTATE(code[0], code[1], code[2], code[3], code[4])),
						errmsg("%s", node->errorMessage)));
		}
	}

	for (i = 0; i < regular_conn_count; i++)
	{
		if (pgxc_node_begin(1, &connections[i], gxid, need_tran_block,
					is_read_only, PGXC_NODE_DATANODE))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("Could not begin transaction on data nodes.")));

		if (!pgxc_start_command_on_connection(connections[i], node, snapshot))
		{
			pfree(connections);
			if (primaryconnection)
				pfree(primaryconnection);
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("Failed to send command to data nodes")));
		}
		connections[i]->combiner = node;
	}

	if (step->cursor)
	{
		node->cursor = step->cursor;
		node->cursor_count = regular_conn_count;
		node->cursor_connections = (PGXCNodeHandle **) palloc(regular_conn_count * sizeof(PGXCNodeHandle *));
		memcpy(node->cursor_connections, connections, regular_conn_count * sizeof(PGXCNodeHandle *));
	}

	/*
	 * Stop if all commands are completed or we got a data row and
	 * initialized state node for subsequent invocations
	 */
	while (regular_conn_count > 0 && node->connections == NULL)
	{
		int i = 0;

		if (pgxc_node_receive(regular_conn_count, connections, NULL))
		{
			pfree(connections);
			if (primaryconnection)
				pfree(primaryconnection);
			if (node->cursor_connections)
				pfree(node->cursor_connections);

			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("Failed to read response from data nodes")));
		}
		/*
		 * Handle input from the data nodes.
		 * If we got a RESPONSE_DATAROW we can break handling to wrap
		 * it into a tuple and return. Handling will be continued upon
		 * subsequent invocations.
		 * If we got 0, we exclude connection from the list. We do not
		 * expect more input from it. In case of non-SELECT query we quit
		 * the loop when all nodes finish their work and send ReadyForQuery
		 * with empty connections array.
		 * If we got EOF, move to the next connection, will receive more
		 * data on the next iteration.
		 */
		while (i < regular_conn_count)
		{
			int res = handle_response(connections[i], node);
			if (res == RESPONSE_EOF)
			{
				i++;
			}
			else if (res == RESPONSE_COMPLETE)
			{
				if (i < --regular_conn_count)
					connections[i] = connections[regular_conn_count];
			}
			else if (res == RESPONSE_TUPDESC)
			{
				ExecSetSlotDescriptor(scanslot, node->tuple_desc);
				/*
				 * Now tuple table slot is responsible for freeing the
				 * descriptor
				 */
				node->tuple_desc = NULL;
				if (step->sort)
				{
					SimpleSort *sort = step->sort;

					node->connections = connections;
					node->conn_count = regular_conn_count;
					/*
					 * First message is already in the buffer
					 * Further fetch will be under tuplesort control
					 * If query does not produce rows tuplesort will not
					 * be initialized
					 */
					node->tuplesortstate = tuplesort_begin_merge(
										   scanslot->tts_tupleDescriptor,
										   sort->numCols,
										   sort->sortColIdx,
										   sort->sortOperators,
										   sort->sortCollations,
										   sort->nullsFirst,
										   node,
										   work_mem);
					/*
					 * Break the loop, do not wait for first row.
					 * Tuplesort module want to control node it is
					 * fetching rows from, while in this loop first
					 * row would be got from random node
					 */
					break;
				}
			}
			else if (res == RESPONSE_DATAROW)
			{
				/*
				 * Got first data row, quit the loop
				 */
				node->connections = connections;
				node->conn_count = regular_conn_count;
				node->current_conn = i;
				break;
			}
			else
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("Unexpected response from data node")));
		}

		if (node->errorMessage)
		{
			char *code = node->errorCode;
			if (node->errorDetail != NULL)
				ereport(ERROR,
						(errcode(MAKE_SQLSTATE(code[0], code[1], code[2], code[3], code[4])),
						errmsg("%s", node->errorMessage), errdetail("%s", node->errorDetail) ));
			else
				ereport(ERROR,
						(errcode(MAKE_SQLSTATE(code[0], code[1], code[2], code[3], code[4])),
						errmsg("%s", node->errorMessage)));
		}
	}

	if (node->cursor_count)
	{
		node->conn_count = node->cursor_count;
		memcpy(connections, node->cursor_connections, node->cursor_count * sizeof(PGXCNodeHandle *));
		node->connections = connections;
	}
}

/*
 * apply_qual
 * Applies the qual of the query, and returns the same slot if
 * the qual returns true, else returns NULL
 */
static TupleTableSlot *
apply_qual(TupleTableSlot *slot, RemoteQueryState *node)
{
   List *qual = node->ss.ps.qual;
   if (qual)
   {
	   ExprContext *econtext = node->ss.ps.ps_ExprContext;
	   econtext->ecxt_scantuple = slot;
	   if (!ExecQual(qual, econtext, false))
		   return NULL;
   }

   return slot;
}

/*
 * ExecRemoteQuery
 * Wrapper around the main RemoteQueryNext() function. This
 * wrapper provides materialization of the result returned by
 * RemoteQueryNext
 */

TupleTableSlot *
ExecRemoteQuery(RemoteQueryState *node)
{
	TupleTableSlot *resultslot = node->ss.ps.ps_ResultTupleSlot;
	Tuplestorestate *tuplestorestate;
	bool		eof_tuplestore;
	bool forward = ScanDirectionIsForward(node->ss.ps.state->es_direction);
	TupleTableSlot *slot;

	/*
	 * If sorting is needed, then tuplesortstate takes care of
	 * materialization
	 */
	if (((RemoteQuery *) node->ss.ps.plan)->sort)
		return RemoteQueryNext(node);


	tuplestorestate = node->tuplestorestate;

	if (tuplestorestate == NULL)
	{
		tuplestorestate = tuplestore_begin_heap(true, false, work_mem);
		tuplestore_set_eflags(tuplestorestate, node->eflags);
		node->tuplestorestate = tuplestorestate;
	}

	/*
	 * If we are not at the end of the tuplestore, or are going backwards, try
	 * to fetch a tuple from tuplestore.
	 */
	eof_tuplestore = (tuplestorestate == NULL) ||
		tuplestore_ateof(tuplestorestate);

	if (!forward && eof_tuplestore)
	{
		if (!node->eof_underlying)
		{
			/*
			 * When reversing direction at tuplestore EOF, the first
			 * gettupleslot call will fetch the last-added tuple; but we want
			 * to return the one before that, if possible. So do an extra
			 * fetch.
			 */
			if (!tuplestore_advance(tuplestorestate, forward))
				return NULL;	/* the tuplestore must be empty */
		}
		eof_tuplestore = false;
	}

	/*
	 * If we can fetch another tuple from the tuplestore, return it.
	 */
	slot = node->ss.ps.ps_ResultTupleSlot;
	if (!eof_tuplestore)
	{
		/* Look for the first tuple that matches qual */
		while (tuplestore_gettupleslot(tuplestorestate, forward, false, slot))
		{
			if (apply_qual(slot, node))
				return slot;
		}
		/* Not found */
		if (forward)
			eof_tuplestore = true;
	}


	/*
	 * If tuplestore has reached its end but the underlying RemoteQueryNext() hasn't
	 * finished yet, try to fetch another row.
	 */
	if (eof_tuplestore && !node->eof_underlying)
	{
		TupleTableSlot *outerslot;

		while (1)
		{
			/*
			 * We can only get here with forward==true, so no need to worry about
			 * which direction the subplan will go.
			 */
			outerslot = RemoteQueryNext(node);
			if (TupIsNull(outerslot))
			{
				node->eof_underlying = true;
				return NULL;
			}

			/*
			 * Append a copy of the returned tuple to tuplestore.  NOTE: because
			 * the tuplestore is certainly in EOF state, its read position will
			 * move forward over the added tuple.  This is what we want.
			 */
			if (tuplestorestate)
				tuplestore_puttupleslot(tuplestorestate, outerslot);

			/*
			 * If qual returns true, return the fetched tuple, else continue for
			 * next tuple
			 */
			if (apply_qual(outerslot, node))
				return outerslot;
		}

	}

	return ExecClearTuple(resultslot);
}

/*
 * Execute step of PGXC plan.
 * The step specifies a command to be executed on specified nodes.
 * On first invocation connections to the data nodes are initialized and
 * command is executed. Further, as well as within subsequent invocations,
 * responses are received until step is completed or there is a tuple to emit.
 * If there is a tuple it is returned, otherwise returned NULL. The NULL result
 * from the function indicates completed step.
 * The function returns at most one tuple per invocation.
 */
static TupleTableSlot *
RemoteQueryNext(RemoteQueryState *node)
{
	TupleTableSlot *resultslot = node->ss.ps.ps_ResultTupleSlot;
	TupleTableSlot *scanslot = node->ss.ss_ScanTupleSlot;
	bool have_tuple = false;
	List			*qual = node->ss.ps.qual;
	ExprContext		*econtext = node->ss.ps.ps_ExprContext;

	if (!node->query_Done)
	{
		do_query(node);
		node->query_Done = true;
	}

	if (node->update_cursor)
	{
		PGXCNodeAllHandles *all_dn_handles = get_exec_connections(node, NULL, EXEC_ON_DATANODES);
		close_node_cursors(all_dn_handles->datanode_handles,
						  all_dn_handles->dn_conn_count,
						  node->update_cursor);
		pfree(node->update_cursor);
		node->update_cursor = NULL;
		pfree_pgxc_all_handles(all_dn_handles);
	}

	if (node->tuplesortstate)
	{
		while (tuplesort_gettupleslot((Tuplesortstate *) node->tuplesortstate,
									  true, scanslot))
		{
			if (qual)
				econtext->ecxt_scantuple = scanslot;
			if (!qual || ExecQual(qual, econtext, false))
				have_tuple = true;
			else
			{
				have_tuple = false;
				continue;
			}
			copy_slot(node, scanslot, resultslot);
			break;
		}
		if (!have_tuple)
			ExecClearTuple(resultslot);
	}
	else
	{
		if (FetchTuple(node, scanslot) && !TupIsNull(scanslot))
		{
			if (qual)
				econtext->ecxt_scantuple = scanslot;
			copy_slot(node, scanslot, resultslot);
		}
		else
			ExecClearTuple(resultslot);
	}

	if (node->errorMessage)
	{
		char *code = node->errorCode;
		if (node->errorDetail != NULL)
			ereport(ERROR,
					(errcode(MAKE_SQLSTATE(code[0], code[1], code[2], code[3], code[4])),
						errmsg("%s", node->errorMessage), errdetail("%s", node->errorDetail) ));
		else
			ereport(ERROR,
					(errcode(MAKE_SQLSTATE(code[0], code[1], code[2], code[3], code[4])),
						errmsg("%s", node->errorMessage)));
	}

	/*
	 * While we are emitting rows we ignore outer plan
	 */
	if (!TupIsNull(resultslot))
		return resultslot;

	/*
	 * OK, we have nothing to return, so return NULL
	 */
	return NULL;
}

/*
 * End the remote query
 */
void
ExecEndRemoteQuery(RemoteQueryState *node)
{
	ListCell *lc;

	/* clean up the buffer */
	foreach(lc, node->rowBuffer)
	{
		RemoteDataRow dataRow = (RemoteDataRow) lfirst(lc);
		pfree(dataRow->msg);
	}
	list_free_deep(node->rowBuffer);

	node->current_conn = 0;
	while (node->conn_count > 0)
	{
		int res;
		PGXCNodeHandle *conn = node->connections[node->current_conn];

		/* throw away message */
		if (node->currentRow.msg)
		{
			pfree(node->currentRow.msg);
			node->currentRow.msg = NULL;
		}

		if (conn == NULL)
		{
			node->conn_count--;
			continue;
		}

		/* no data is expected */
		if (conn->state == DN_CONNECTION_STATE_IDLE ||
				conn->state == DN_CONNECTION_STATE_ERROR_FATAL)
		{
			if (node->current_conn < --node->conn_count)
				node->connections[node->current_conn] = node->connections[node->conn_count];
			continue;
		}
		res = handle_response(conn, node);
		if (res == RESPONSE_EOF)
		{
			struct timeval timeout;
			timeout.tv_sec = END_QUERY_TIMEOUT;
			timeout.tv_usec = 0;

			if (pgxc_node_receive(1, &conn, &timeout))
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("Failed to read response from data nodes when ending query")));
		}
	}

	if (node->tuplesortstate != NULL || node->tuplestorestate != NULL)
		ExecClearTuple(node->ss.ss_ScanTupleSlot);
	/*
	 * Release tuplesort resources
	 */
	if (node->tuplesortstate != NULL)
		tuplesort_end((Tuplesortstate *) node->tuplesortstate);
	node->tuplesortstate = NULL;
	/*
	 * Release tuplestore resources
	 */
	if (node->tuplestorestate != NULL)
		tuplestore_end(node->tuplestorestate);
	node->tuplestorestate = NULL;

	/*
	 * If there are active cursors close them
	 */
	if (node->cursor || node->update_cursor)
	{
		PGXCNodeAllHandles *all_handles = NULL;
		PGXCNodeHandle    **cur_handles;
		bool bFree = false;
		int nCount;
		int i;

		cur_handles = node->cursor_connections;
		nCount = node->cursor_count;

		for(i=0;i<node->cursor_count;i++)
		{
			if (node->cursor_connections == NULL || node->cursor_connections[i]->sock == -1)
			{
				bFree = true;
				all_handles = get_exec_connections(node, NULL, EXEC_ON_DATANODES);
				cur_handles = all_handles->datanode_handles;
				nCount = all_handles->dn_conn_count;
				break;
			}
		}

		if (node->cursor)
		{
			close_node_cursors(cur_handles, nCount, node->cursor);
			pfree(node->cursor);
			node->cursor = NULL;
		}

		if (node->update_cursor)
		{
			close_node_cursors(cur_handles, nCount, node->update_cursor);
			pfree(node->update_cursor);
			node->update_cursor = NULL;
		}

		if (bFree)
			pfree_pgxc_all_handles(all_handles);
	}

	/*
	 * Clean up parameters if they were set
	 */
	if (node->paramval_data)
	{
		pfree(node->paramval_data);
		node->paramval_data = NULL;
		node->paramval_len = 0;
	}

	if (node->ss.ss_currentRelation)
		ExecCloseScanRelation(node->ss.ss_currentRelation);

	if (node->tmp_ctx)
		MemoryContextDelete(node->tmp_ctx);

	CloseCombiner(node);
}

static void
close_node_cursors(PGXCNodeHandle **connections, int conn_count, char *cursor)
{
	int i;
	RemoteQueryState *combiner;

	for (i = 0; i < conn_count; i++)
	{
		if (connections[i]->state == DN_CONNECTION_STATE_QUERY)
			BufferConnection(connections[i]);
		if (pgxc_node_send_close(connections[i], false, cursor) != 0)
			ereport(WARNING,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("Failed to close data node cursor")));
		if (pgxc_node_send_sync(connections[i]) != 0)
			ereport(WARNING,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("Failed to close data node cursor")));
	}

	combiner = CreateResponseCombiner(conn_count, COMBINE_TYPE_NONE);

	while (conn_count > 0)
	{
		if (pgxc_node_receive(conn_count, connections, NULL))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("Failed to close data node cursor")));
		i = 0;
		while (i < conn_count)
		{
			int res = handle_response(connections[i], combiner);
			if (res == RESPONSE_EOF)
			{
				i++;
			}
			else if (res == RESPONSE_COMPLETE)
			{
				if (--conn_count > i)
					connections[i] = connections[conn_count];
			}
			else
			{
				// Unexpected response, ignore?
			}
		}
	}

	ValidateAndCloseCombiner(combiner);
}
#endif


/*
 * Encode parameter values to format of DataRow message (the same format is
 * used in Bind) to prepare for sending down to data nodes.
 * The buffer to store encoded value is palloc'ed and returned as the result
 * parameter. Function returns size of the result
 */
int
ParamListToDataRow(ParamListInfo params, char** result)
{
	StringInfoData buf;
	uint16 n16;
	int i;
	int real_num_params = params->numParams;

	/*
	 * It is necessary to fetch parameters
	 * before looking at the output value.
	 */
	for (i = 0; i < params->numParams; i++)
	{
		ParamExternData *param;

		param = &params->params[i];

		if (!OidIsValid(param->ptype) && params->paramFetch != NULL)
			(*params->paramFetch) (params, i + 1);

		/*
		 * In case parameter type is not defined, it is not necessary to include
		 * it in message sent to backend nodes.
		 */
		if (!OidIsValid(param->ptype))
			real_num_params--;
	}

	initStringInfo(&buf);

	/* Number of parameter values */
	n16 = htons(real_num_params);
	appendBinaryStringInfo(&buf, (char *) &n16, 2);

	/* Parameter values */
	for (i = 0; i < params->numParams; i++)
	{
		ParamExternData *param = &params->params[i];
		uint32 n32;

		/* If parameter has no type defined it is not necessary to include it in message */
		if (!OidIsValid(param->ptype))
			continue;

		if (param->isnull)
		{
			n32 = htonl(-1);
			appendBinaryStringInfo(&buf, (char *) &n32, 4);
		}
		else
		{
			Oid		typOutput;
			bool	typIsVarlena;
			Datum	pval;
			char   *pstring;
			int		len;

			/* Get info needed to output the value */
			getTypeOutputInfo(param->ptype, &typOutput, &typIsVarlena);

			/*
			 * If we have a toasted datum, forcibly detoast it here to avoid
			 * memory leakage inside the type's output routine.
			 */
			if (typIsVarlena)
				pval = PointerGetDatum(PG_DETOAST_DATUM(param->value));
			else
				pval = param->value;

			/* Convert Datum to string */
			pstring = OidOutputFunctionCall(typOutput, pval);

			/* copy data to the buffer */
			len = strlen(pstring);
			n32 = htonl(len);
			appendBinaryStringInfo(&buf, (char *) &n32, 4);
			appendBinaryStringInfo(&buf, pstring, len);
		}
	}

	/* Take data from the buffer */
	*result = palloc(buf.len);
	memcpy(*result, buf.data, buf.len);
	pfree(buf.data);
	return buf.len;
}


#ifndef XCP
/* ----------------------------------------------------------------
 *		ExecRemoteQueryReScan
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecRemoteQueryReScan(RemoteQueryState *node, ExprContext *exprCtxt)
{
	/*
	 * If the materialized store is not empty, just rewind the stored output.
	 */
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	if (((RemoteQuery *) node->ss.ps.plan)->sort)
	{
		if (!node->tuplesortstate)
			return;

		tuplesort_rescan(node->tuplesortstate);
	}
	else
	{
		if (!node->tuplestorestate)
			return;

		tuplestore_rescan(node->tuplestorestate);
	}

}
#endif


/*
 * Execute utility statement on multiple data nodes
 * It does approximately the same as
 *
 * RemoteQueryState *state = ExecInitRemoteQuery(plan, estate, flags);
 * Assert(TupIsNull(ExecRemoteQuery(state));
 * ExecEndRemoteQuery(state)
 *
 * But does not need an Estate instance and does not do some unnecessary work,
 * like allocating tuple slots.
 */
void
ExecRemoteUtility(RemoteQuery *node)
{
	RemoteQueryState *remotestate;
#ifdef XCP
	ResponseCombiner *combiner;
#endif
	bool		force_autocommit = node->force_autocommit;
	RemoteQueryExecType exec_type = node->exec_type;
	GlobalTransactionId gxid = InvalidGlobalTransactionId;
#ifdef XCP
	Snapshot snapshot;
#else
	Snapshot snapshot = GetActiveSnapshot();
#endif
	PGXCNodeAllHandles *pgxc_connections;
	int			co_conn_count;
	int			dn_conn_count;
	bool		need_tran_block;
	ExecDirectType		exec_direct_type = node->exec_direct_type;
	int			i;

	if (!force_autocommit)
		RegisterTransactionLocalNode(true);

#ifdef XCP
	remotestate = makeNode(RemoteQueryState);
	combiner = (ResponseCombiner *)remotestate;
	InitResponseCombiner(combiner, 0, node->combine_type);
#else
	/*
	 * It is possible to invoke create table with inheritance on
	 * temporary objects. Remember that we might have accessed a temp object
	 */
	if (node->is_temp)
		ExecSetTempObjectIncluded();

	remotestate = CreateResponseCombiner(0, node->combine_type);
#endif

	pgxc_connections = get_exec_connections(NULL, node->exec_nodes, exec_type);

	dn_conn_count = pgxc_connections->dn_conn_count;
	co_conn_count = pgxc_connections->co_conn_count;
#ifdef XCP
	/* exit right away if no nodes to run command on */
	if (dn_conn_count == 0 && co_conn_count == 0)
		return;
#endif

	if (force_autocommit)
		need_tran_block = false;
	else
		need_tran_block = true;
	/*
	else if (exec_type == EXEC_ON_ALL_NODES ||
			 exec_type == EXEC_ON_COORDS)
		need_tran_block = true;
	else
		need_tran_block = (total_conn_count > 1) ||
						  (TransactionBlockStatusCode() == 'T');
						  */

	/* Commands launched through EXECUTE DIRECT do not need start a transaction */
	if (exec_direct_type == EXEC_DIRECT_UTILITY)
	{
		need_tran_block = false;

		/* This check is not done when analyzing to limit dependencies */
		if (IsTransactionBlock())
			ereport(ERROR,
					(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
					 errmsg("cannot run EXECUTE DIRECT with utility inside a transaction block")));
	}

	gxid = GetCurrentTransactionId();
#ifdef XCP
	snapshot = GetActiveSnapshot();
#endif
	if (!GlobalTransactionIdIsValid(gxid))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("Failed to get next transaction ID")));

#ifndef XCP
	if (exec_type == EXEC_ON_ALL_NODES || exec_type == EXEC_ON_DATANODES)
#endif
	{
		if (pgxc_node_begin(dn_conn_count, pgxc_connections->datanode_handles,
					gxid, need_tran_block, false, PGXC_NODE_DATANODE))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("Could not begin transaction on data nodes")));
		for (i = 0; i < dn_conn_count; i++)
		{
			PGXCNodeHandle *conn = pgxc_connections->datanode_handles[i];

			if (conn->state == DN_CONNECTION_STATE_QUERY)
				BufferConnection(conn);
			if (snapshot && pgxc_node_send_snapshot(conn, snapshot))
			{
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("Failed to send command to data nodes")));
			}
			if (pgxc_node_send_query(conn, node->sql_statement) != 0)
			{
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("Failed to send command to data nodes")));
			}
		}
	}

#ifndef XCP
	if (exec_type == EXEC_ON_ALL_NODES || exec_type == EXEC_ON_COORDS)
#endif
	{
		if (pgxc_node_begin(co_conn_count, pgxc_connections->coord_handles,
					gxid, need_tran_block, false, PGXC_NODE_COORDINATOR))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("Could not begin transaction on coordinators")));
		/* Now send it to Coordinators if necessary */
		for (i = 0; i < co_conn_count; i++)
		{
			if (snapshot && pgxc_node_send_snapshot(pgxc_connections->coord_handles[i], snapshot))
			{
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("Failed to send command to coordinators")));
			}
			if (pgxc_node_send_query(pgxc_connections->coord_handles[i], node->sql_statement) != 0)
			{
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("Failed to send command to coordinators")));
			}
		}
	}

	/*
	 * Stop if all commands are completed or we got a data row and
	 * initialized state node for subsequent invocations
	 */
#ifndef XCP
	if (exec_type == EXEC_ON_ALL_NODES ||
		exec_type == EXEC_ON_DATANODES)
#endif
	{
		while (dn_conn_count > 0)
		{
			int i = 0;

			if (pgxc_node_receive(dn_conn_count, pgxc_connections->datanode_handles, NULL))
				break;
			/*
			 * Handle input from the data nodes.
			 * We do not expect data nodes returning tuples when running utility
			 * command.
			 * If we got EOF, move to the next connection, will receive more
			 * data on the next iteration.
			 */
			while (i < dn_conn_count)
			{
				PGXCNodeHandle *conn = pgxc_connections->datanode_handles[i];
#ifdef XCP
				int res = handle_response(conn, combiner);
#else
				int res = handle_response(conn, remotestate);
#endif
				if (res == RESPONSE_EOF)
				{
					i++;
				}
				else if (res == RESPONSE_COMPLETE)
#ifdef XCP
				{
					/* Ignore, wait for ReadyForQuery */
				}
				else if (res == RESPONSE_ERROR)
				{
					/* Ignore, wait for ReadyForQuery */
				}
				else if (res == RESPONSE_READY)
#endif
				{
					if (i < --dn_conn_count)
						pgxc_connections->datanode_handles[i] =
							pgxc_connections->datanode_handles[dn_conn_count];
				}
				else if (res == RESPONSE_TUPDESC)
				{
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("Unexpected response from data node")));
				}
				else if (res == RESPONSE_DATAROW)
				{
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("Unexpected response from data node")));
				}
			}
		}
	}

	/* Make the same for Coordinators */
#ifndef XCP
	if (exec_type == EXEC_ON_ALL_NODES ||
		exec_type == EXEC_ON_COORDS)
#endif
	{
		while (co_conn_count > 0)
		{
			int i = 0;

			if (pgxc_node_receive(co_conn_count, pgxc_connections->coord_handles, NULL))
				break;

			while (i < co_conn_count)
			{
#ifdef XCP
				int res = handle_response(pgxc_connections->coord_handles[i], combiner);
#else
				int res = handle_response(pgxc_connections->coord_handles[i], remotestate);
#endif
				if (res == RESPONSE_EOF)
				{
					i++;
				}
				else if (res == RESPONSE_COMPLETE)
#ifdef XCP
				{
					/* Ignore, wait for ReadyForQuery */
				}
				else if (res == RESPONSE_ERROR)
				{
					/* Ignore, wait for ReadyForQuery */
				}
				else if (res == RESPONSE_READY)
#endif
				{
					if (i < --co_conn_count)
						pgxc_connections->coord_handles[i] =
							 pgxc_connections->coord_handles[co_conn_count];
				}
				else if (res == RESPONSE_TUPDESC)
				{
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("Unexpected response from coordinator")));
				}
				else if (res == RESPONSE_DATAROW)
				{
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("Unexpected response from coordinator")));
				}
			}
		}
	}
	/*
	 * We have processed all responses from nodes and if we have
	 * error message pending we can report it. All connections should be in
	 * consistent state now and so they can be released to the pool after ROLLBACK.
	 */
#ifdef XCP
	if (combiner->errorMessage)
	{
		char *code = combiner->errorCode;
		if (combiner->errorDetail)
			ereport(ERROR,
					(errcode(MAKE_SQLSTATE(code[0], code[1], code[2], code[3], code[4])),
						errmsg("%s", combiner->errorMessage), errdetail("%s", combiner->errorDetail) ));
		else
			ereport(ERROR,
					(errcode(MAKE_SQLSTATE(code[0], code[1], code[2], code[3], code[4])),
						errmsg("%s", combiner->errorMessage)));
	}
#else
	if (remotestate->errorMessage)
 	{
		char *code = remotestate->errorCode;
		if (remotestate->errorDetail != NULL)
 			ereport(ERROR,
 					(errcode(MAKE_SQLSTATE(code[0], code[1], code[2], code[3], code[4])),
						errmsg("%s", remotestate->errorMessage), errdetail("%s", remotestate->errorDetail) ));
 		else
 			ereport(ERROR,
 					(errcode(MAKE_SQLSTATE(code[0], code[1], code[2], code[3], code[4])),
						errmsg("%s", remotestate->errorMessage)));
	}
#endif
}


/*
 * Called when the backend is ending.
 */
void
PGXCNodeCleanAndRelease(int code, Datum arg)
{
#ifndef XCP
	/* Clean up prepared transactions before releasing connections */
	DropAllPreparedStatements();
#endif

	/* Release data node connections */
	release_handles();

	/* Disconnect from Pooler */
	PoolManagerDisconnect();

	/* Close connection with GTM */
	CloseGTM();

	/* Dump collected statistics to the log */
	stat_log();
}

static int
pgxc_get_connections(PGXCNodeHandle *connections[], int size, List *connlist)
{
	ListCell *lc;
	int count = 0;

	foreach(lc, connlist)
	{
		PGXCNodeHandle *conn = (PGXCNodeHandle *) lfirst(lc);
		Assert (count < size);
		connections[count++] = conn;
	}
	return count;
}
/*
 * Get all connections for which we have an open transaction,
 * for both data nodes and coordinators
 */
static int
pgxc_get_transaction_nodes(PGXCNodeHandle *connections[], int size, bool write)
{
	return pgxc_get_connections(connections, size, write ? XactWriteNodes : XactReadNodes);
}

void
ExecCloseRemoteStatement(const char *stmt_name, List *nodelist)
{
	PGXCNodeAllHandles *all_handles;
	PGXCNodeHandle	  **connections;
#ifdef XCP
	ResponseCombiner	combiner;
#else
	RemoteQueryState   *combiner;
#endif
	int					conn_count;
	int 				i;

	/* Exit if nodelist is empty */
	if (list_length(nodelist) == 0)
		return;

	/* get needed data node connections */
	all_handles = get_handles(nodelist, NIL, false);
	conn_count = all_handles->dn_conn_count;
	connections = all_handles->datanode_handles;

	for (i = 0; i < conn_count; i++)
	{
		if (connections[i]->state == DN_CONNECTION_STATE_QUERY)
			BufferConnection(connections[i]);
		if (pgxc_node_send_close(connections[i], true, stmt_name) != 0)
		{
			/*
			 * statements are not affected by statement end, so consider
			 * unclosed statement on the datanode as a fatal issue and
			 * force connection is discarded
			 */
			connections[i]->state = DN_CONNECTION_STATE_ERROR_FATAL;
			ereport(WARNING,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("Failed to close data node statemrnt")));
		}
		if (pgxc_node_send_sync(connections[i]) != 0)
		{
			connections[i]->state = DN_CONNECTION_STATE_ERROR_FATAL;
			ereport(WARNING,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("Failed to close data node statement")));
		}
	}

#ifdef XCP
	InitResponseCombiner(&combiner, conn_count, COMBINE_TYPE_NONE);
	/*
	 * Make sure there are zeroes in unused fields
	 */
	memset(&combiner, 0, sizeof(ScanState));
#else
	combiner = CreateResponseCombiner(conn_count, COMBINE_TYPE_NONE);
#endif

	while (conn_count > 0)
	{
		if (pgxc_node_receive(conn_count, connections, NULL))
		{
			for (i = 0; i <= conn_count; i++)
				connections[i]->state = DN_CONNECTION_STATE_ERROR_FATAL;

			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("Failed to close data node statement")));
		}
		i = 0;
		while (i < conn_count)
		{
#ifdef XCP
			int res = handle_response(connections[i], &combiner);
#else
			int res = handle_response(connections[i], combiner);
#endif
			if (res == RESPONSE_EOF)
			{
				i++;
			}
#ifdef XCP
			else if (res == RESPONSE_READY ||
					connections[i]->state == DN_CONNECTION_STATE_ERROR_FATAL)
#else
			else if (res == RESPONSE_COMPLETE)
#endif
			{
				if (--conn_count > i)
					connections[i] = connections[conn_count];
			}
#ifndef XCP
			else
			{
				connections[i]->state = DN_CONNECTION_STATE_ERROR_FATAL;
			}
#endif
		}
	}

#ifdef XCP
	ValidateAndCloseCombiner(&combiner);
#else
	ValidateAndCloseCombiner(combiner);
#endif
	pfree_pgxc_all_handles(all_handles);
}

/*
 * DataNodeCopyInBinaryForAll
 *
 * In a COPY TO, send to all datanodes PG_HEADER for a COPY TO in binary mode.
 */
#ifdef XCP
int
DataNodeCopyInBinaryForAll(char *msg_buf, int len, int conn_count,
									  PGXCNodeHandle** connections)
#else
int DataNodeCopyInBinaryForAll(char *msg_buf, int len, PGXCNodeHandle** copy_connections)
#endif
{
	int 		i;
#ifndef XCP
	int 		conn_count = 0;
	PGXCNodeHandle *connections[NumDataNodes];
#endif
	int msgLen = 4 + len + 1;
	int nLen = htonl(msgLen);

#ifndef XCP
	for (i = 0; i < NumDataNodes; i++)
	{
		PGXCNodeHandle *handle = copy_connections[i];

		if (!handle)
			continue;

		connections[conn_count++] = handle;
	}
#endif

	for (i = 0; i < conn_count; i++)
	{
		PGXCNodeHandle *handle = connections[i];
		if (handle->state == DN_CONNECTION_STATE_COPY_IN)
		{
			/* msgType + msgLen */
			if (ensure_out_buffer_capacity(handle->outEnd + 1 + msgLen, handle) != 0)
			{
				ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					errmsg("out of memory")));
			}

			handle->outBuffer[handle->outEnd++] = 'd';
			memcpy(handle->outBuffer + handle->outEnd, &nLen, 4);
			handle->outEnd += 4;
			memcpy(handle->outBuffer + handle->outEnd, msg_buf, len);
			handle->outEnd += len;
			handle->outBuffer[handle->outEnd++] = '\n';
		}
		else
		{
			add_error_message(handle, "Invalid data node connection");
			return EOF;
		}
	}

	return 0;
}

#ifndef XCP
/*
 * ExecSetTempObjectIncluded
 *
 * Remember that we have accessed a temporary object.
 */
void
ExecSetTempObjectIncluded(void)
{
	temp_object_included = true;
}

/*
 * ExecClearTempObjectIncluded
 *
 * Forget about temporary objects
 */
static void
ExecClearTempObjectIncluded(void)
{
	temp_object_included = false;
}

/* ExecIsTempObjectIncluded
 *
 * Check if a temporary object has been accessed
 */
bool
ExecIsTempObjectIncluded(void)
{
	return temp_object_included;
}

/*
 * Execute given tuple in the remote relation. We use extended query protocol
 * to avoid repeated planning of the query. So we must pass the column values
 * as parameters while executing the query.
 * This is used by queries using a remote query planning of standard planner.
 */
void
ExecRemoteQueryStandard(Relation resultRelationDesc,
						RemoteQueryState *resultRemoteRel,
						TupleTableSlot *slot)
{
	ExprContext		*econtext = resultRemoteRel->ss.ps.ps_ExprContext;

	/*
	 * Use data row returned by the previous step as a parameters for
	 * the main query.
	 */
	if (!TupIsNull(slot))
	{
		resultRemoteRel->paramval_len = ExecCopySlotDatarow(slot,
												 &resultRemoteRel->paramval_data);

		/*
		 * The econtext is set only when en_expr is set for execution time
		 * evalulation of the target node.
		 */
		if (econtext)
			econtext->ecxt_scantuple = slot;
		do_query(resultRemoteRel);
	}
}
#endif


void
RegisterTransactionNodes(int count, void **connections, bool write)
{
	int i;
	MemoryContext oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	for (i = 0; i < count; i++)
	{
		/*
		 * Add the node to either read or write participants. If a node is
		 * already in the write participant's list, don't add it to the read
		 * participant's list. OTOH if a node is currently in the read
		 * participant's list, but we are now initiating a write operation on
		 * the node, move it to the write participant's list
		 */
		if (write)
		{
			XactWriteNodes = list_append_unique(XactWriteNodes, connections[i]);
			XactReadNodes = list_delete(XactReadNodes, connections[i]);
		}
		else
		{
			if (!list_member(XactWriteNodes, connections[i]))
				XactReadNodes = list_append_unique(XactReadNodes, connections[i]);
		}
	}

	MemoryContextSwitchTo(oldcontext);
}

void
ForgetTransactionNodes(void)
{
	list_free(XactReadNodes);
	XactReadNodes = NIL;

	list_free(XactWriteNodes);
	XactWriteNodes = NIL;
}

/*
 * Clear per transaction remote information
 */
void
AtEOXact_Remote(void)
{
#ifndef XCP
	ExecClearTempObjectIncluded();
#endif
	ForgetTransactionNodes();
	clear_RemoteXactState();
}

/*
 * Do pre-commit processing for remote nodes which includes data nodes and
 * coordinators. If more than one nodes are involved in the transaction write
 * activity, then we must run 2PC. For 2PC, we do the following steps:
 *
 *  1. PREPARE the transaction locally if the local node is involved in the
 *     transaction. If local node is not involved, skip this step and go to the
 *     next step
 *  2. PREPARE the transaction on all the remote nodes. If any node fails to
 *     PREPARE, directly go to step 6
 *  3. Now that all the involved nodes are PREPAREd, we can commit the
 *     transaction. We first inform the GTM that the transaction is fully
 *     PREPARED and also supply the list of the nodes involved in the
 *     transaction
 *  4. COMMIT PREPARED the transaction on all the remotes nodes and then
 *     finally COMMIT PREPARED on the local node if its involved in the
 *     transaction and start a new transaction so that normal commit processing
 *     works unchanged. Go to step 5.
 *  5. Return and let the normal commit processing resume
 *  6. Abort by ereporting the error and let normal abort-processing take
 *     charge.
 */
void
PreCommit_Remote(char *prepareGID, bool preparedLocalNode)
{
	if (!preparedLocalNode)
		PrePrepare_Remote(prepareGID, preparedLocalNode, false);

	/*
	 * OK, everything went fine. At least one remote node is in PREPARED state
	 * and the transaction is successfully prepared on all the involved nodes.
	 * Now we are ready to commit the transaction. We need a new GXID to send
	 * down the remote nodes to execute the forthcoming COMMIT PREPARED
	 * command. So grab one from the GTM and track it. It will be closed along
	 * with the main transaction at the end.
	 */
	pgxc_node_remote_commit();

	/*
	 * If the transaction is not committed successfully on all the involved
	 * nodes, it will remain in PREPARED state on those nodes. Such transaction
	 * should be be reported as live in the snapshots. So we must not close the
	 * transaction on the GTM. We just record the state of the transaction in
	 * the GTM and flag a warning for applications to take care of such
	 * in-doubt transactions
	 */
	if (remoteXactState.status == RXACT_PART_COMMITTED)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("Failed to commit the transaction on one or more nodes")));


	Assert(remoteXactState.status == RXACT_COMMITTED ||
		   remoteXactState.status == RXACT_NONE);

	clear_RemoteXactState();

	/*
	 * The transaction is now successfully committed on all the remote nodes.
	 * (XXX How about the local node ?). It can now be cleaned up from the GTM
	 * as well
	 */
	if (!PersistentConnections)
		release_handles();
}

/*
 * Do abort processing for the transaction. We must abort the transaction on
 * all the involved nodes. If a node has already prepared a transaction, we run
 * ROLLBACK PREPARED command on the node. Otherwise, a simple ROLLBACK command
 * is sufficient.
 *
 * We must guard against the case when a transaction is prepared succefully on
 * all the nodes and some error occurs after we send a COMMIT PREPARED message
 * to at lease one node. Such a transaction must not be aborted to preserve
 * global consistency. We handle this case by recording the nodes involved in
 * the transaction at the GTM and keep the transaction open at the GTM so that
 * its reported as "in-progress" on all the nodes until resolved
 */
bool
PreAbort_Remote(void)
{
#ifdef XCP
	/*
	 * We are about to abort current transaction, and there could be an
	 * unexpected error leaving the node connection in some state requiring
	 * clean up, like COPY or pending query results.
	 * If we are running copy we should send down CopyFail message and read
	 * all possible incoming messages, there could be copy rows (if running
	 * COPY TO) ErrorResponse, ReadyForQuery.
	 * If there are pending results (connection state is DN_CONNECTION_STATE_QUERY)
	 * we just need to read them in and discard, all necessary commands are
	 * already sent. The end of input could be CommandComplete or
	 * PortalSuspended, in either case subsequent ROLLBACK closes the portal.
	 */
	PGXCNodeAllHandles *all_handles;
	PGXCNodeHandle	   *clean_nodes[NumCoords + NumDataNodes];
	int					node_count = 0;
	int 				i;

	all_handles = get_current_handles();
	/*
	 * Find "dirty" coordinator connections.
	 * COPY is never running on a coordinator connections, we just check for
	 * pending data.
	 */
	for (i = 0; i < all_handles->co_conn_count; i++)
	{
		PGXCNodeHandle *handle = all_handles->coord_handles[i];

		if (handle->state == DN_CONNECTION_STATE_QUERY)
		{
			/*
			 * Forget previous combiner if any since input will be handled by
			 * different one.
			 */
			handle->combiner = NULL;
			clean_nodes[node_count++] = handle;
		}
	}

	/*
	 * The same for data nodes, but cancel COPY if it is running.
	 */
	for (i = 0; i < all_handles->dn_conn_count; i++)
	{
		PGXCNodeHandle *handle = all_handles->datanode_handles[i];

		if (handle->state == DN_CONNECTION_STATE_QUERY)
		{
			/*
			 * Forget previous combiner if any since input will be handled by
			 * different one.
			 */
			handle->combiner = NULL;
			clean_nodes[node_count++] = handle;
		}
		else if (handle->state == DN_CONNECTION_STATE_COPY_IN ||
				handle->state == DN_CONNECTION_STATE_COPY_OUT)
		{
			DataNodeCopyEnd(handle, true);
			clean_nodes[node_count++] = handle;
		}
	}

	pfree_pgxc_all_handles(all_handles);

	/*
	 * Now read and discard any data from the connections found "dirty"
	 */
	if (node_count > 0)
	{
		ResponseCombiner combiner;

		InitResponseCombiner(&combiner, node_count, COMBINE_TYPE_NONE);
		/*
		 * Make sure there are zeroes in unused fields
		 */
		memset(&combiner, 0, sizeof(ScanState));
		combiner.connections = clean_nodes;
		combiner.conn_count = node_count;
		combiner.request_type = REQUEST_TYPE_ERROR;

		pgxc_connections_cleanup(&combiner);

		/* prevent pfree'ing local variable */
		combiner.connections = NULL;

		CloseCombiner(&combiner);
	}
#else
	if (IS_PGXC_COORDINATOR && !IsConnFromCoord())
	{
		cancel_query();
		clear_all_data();
	}
#endif

	if (remoteXactState.status == RXACT_COMMITTED)
		return false;

	if (remoteXactState.status == RXACT_PART_COMMITTED)
	{
		/*
		 * In this case transaction is partially committed, pick up the list of nodes
		 * prepared and not committed and register them on GTM as if it is an explicit 2PC.
		 * This permits to keep the transaction alive in snapshot and other transaction
		 * don't have any side effects with partially committed transactions
		 */
		char	*nodestring = NULL;

		/*
		 * Get the list of nodes in prepared state; such nodes have not
		 * committed successfully
		 */
		nodestring = pgxc_node_get_nodelist(remoteXactState.preparedLocalNode);
		Assert(nodestring);

		/* Save the node list and gid on GTM. */
		StartPreparedTranGTM(GetTopGlobalTransactionId(),
				remoteXactState.prepareGID,
				nodestring);

		/* Finish to prepare the transaction. */
		PrepareTranGTM(GetTopGlobalTransactionId());
		clear_RemoteXactState();
		return false;
	}
	else
	{
		/*
		 * The transaction is neither part or fully committed. We can safely
		 * abort such transaction
		 */
		if (remoteXactState.status == RXACT_NONE)
			init_RemoteXactState(false);

		pgxc_node_remote_abort();
	}

	clear_RemoteXactState();

	if (!PersistentConnections)
		release_handles();

	return true;
}


char *
PrePrepare_Remote(char *prepareGID, bool localNode, bool implicit)
{
	init_RemoteXactState(false);
	/*
	 * PREPARE the transaction on all nodes including remote nodes as well as
	 * local node. Any errors will be reported via ereport and the transaction
	 * will be aborted accordingly.
	 */
	pgxc_node_remote_prepare(prepareGID);

	if (preparedNodes)
		pfree(preparedNodes);
	preparedNodes = NULL;

	if (!implicit)
		preparedNodes = pgxc_node_get_nodelist(true);

	return preparedNodes;
}

void
PostPrepare_Remote(char *prepareGID, char *nodestring, bool implicit)
{
#ifdef XCP
	/*
	 * If we are running PREPARE on a datanode we should commit active
	 * transactions on remote nodes already.
	 */
	if (IS_PGXC_DATANODE)
	{
		pgxc_node_remote_commit();
		clear_RemoteXactState();
		ForgetTransactionNodes();
		return;
	}
#endif

	remoteXactState.preparedLocalNode = true;

	/*
	 * If this is an explicit PREPARE request by the client, we must also save
	 * the list of nodes involved in this transaction on the GTM for later use
	 */
	if (!implicit)
	{
		/* Save the node list and gid on GTM. */
		StartPreparedTranGTM(GetTopGlobalTransactionId(),
				prepareGID,
				nodestring);

		/* Finish to prepare the transaction. */
		PrepareTranGTM(GetTopGlobalTransactionId());
		clear_RemoteXactState();
	}

	/* Now forget the transaction nodes */
	ForgetTransactionNodes();
}

/*
 * Return the list of nodes where the prepared transaction is not yet committed
 */
static char *
pgxc_node_get_nodelist(bool localNode)
{
	int i;
	char *nodestring = NULL, *nodename;

	for (i = 0; i < remoteXactState.numWriteRemoteNodes; i++)
	{
		RemoteXactNodeStatus status = remoteXactState.remoteNodeStatus[i];
		PGXCNodeHandle *conn = remoteXactState.remoteNodeHandles[i];

		if (status != RXACT_NODE_COMMITTED)
		{
			nodename = get_pgxc_nodename(conn->nodeoid);
			if (!nodestring)
			{
				nodestring = (char *) MemoryContextAlloc(TopMemoryContext, strlen(nodename) + 1);
				sprintf(nodestring, "%s", nodename);
			}
			else
			{
				nodestring = (char *) repalloc(nodestring,
											   strlen(nodename) + strlen(nodestring) + 2);
				sprintf(nodestring, "%s,%s", nodestring, nodename);
			}
		}
	}

	/* Case of a single Coordinator */
	if (localNode && PGXCNodeId >= 0)
	{
		if (!nodestring)
		{
			nodestring = (char *) MemoryContextAlloc(TopMemoryContext, strlen(PGXCNodeName) + 1);
			sprintf(nodestring, "%s", PGXCNodeName);
		}
		else
		{
			nodestring = (char *) repalloc(nodestring,
					strlen(PGXCNodeName) + strlen(nodestring) + 2);
			sprintf(nodestring, "%s,%s", nodestring, PGXCNodeName);
		}
	}

	return nodestring;
}

bool
IsTwoPhaseCommitRequired(bool localWrite)
{
	if ((list_length(XactWriteNodes) > 1) ||
		((list_length(XactWriteNodes) == 1) && localWrite))
	{
#ifdef XCP
		if (MyXactAccessedTempRel)
#else
		if (ExecIsTempObjectIncluded())
#endif
		{
			elog(DEBUG1, "Transaction accessed temporary objects - "
					"2PC will not be used and that can lead to data inconsistencies "
					"in case of failures");
			return false;
		}
		return true;
	}
	else
		return false;
}

static void
clear_RemoteXactState(void)
{
	/* Clear the previous state */
	remoteXactState.numWriteRemoteNodes = 0;
	remoteXactState.numReadRemoteNodes = 0;
	remoteXactState.status = RXACT_NONE;
	remoteXactState.commitXid = InvalidGlobalTransactionId;
	remoteXactState.prepareGID[0] = '\0';

	if ((remoteXactState.remoteNodeHandles == NULL) ||
		(remoteXactState.maxRemoteNodes < (NumDataNodes + NumCoords)))
	{
		remoteXactState.remoteNodeHandles = (PGXCNodeHandle **)
			realloc (remoteXactState.remoteNodeHandles,
					sizeof (PGXCNodeHandle *) * (NumDataNodes + NumCoords));
		remoteXactState.remoteNodeStatus = (RemoteXactNodeStatus *)
			realloc (remoteXactState.remoteNodeStatus,
					sizeof (RemoteXactNodeStatus) * (NumDataNodes + NumCoords));
		remoteXactState.maxRemoteNodes = NumDataNodes + NumCoords;
	}

	if (remoteXactState.remoteNodeHandles)
		memset(remoteXactState.remoteNodeHandles, 0,
				sizeof (PGXCNodeHandle *) * (NumDataNodes + NumCoords));
	if (remoteXactState.remoteNodeStatus)
		memset(remoteXactState.remoteNodeStatus, 0,
				sizeof (RemoteXactNodeStatus) * (NumDataNodes + NumCoords));
}

static void
init_RemoteXactState(bool preparedLocalNode)
{
	int write_conn_count, read_conn_count;
	PGXCNodeHandle	  **connections;

	clear_RemoteXactState();

	remoteXactState.preparedLocalNode = preparedLocalNode;
	connections = remoteXactState.remoteNodeHandles;

	Assert(connections);

	/*
	 * First get information about all the nodes involved in this transaction
	 */
	write_conn_count = pgxc_get_transaction_nodes(connections,
			NumDataNodes + NumCoords, true);
	remoteXactState.numWriteRemoteNodes = write_conn_count;

	read_conn_count = pgxc_get_transaction_nodes(connections + write_conn_count,
			NumDataNodes + NumCoords - write_conn_count, false);
	remoteXactState.numReadRemoteNodes = read_conn_count;

}

bool
FinishRemotePreparedTransaction(char *prepareGID, bool commit)
{
	char					*nodename, *nodestring;
	List					*nodelist = NIL, *coordlist = NIL;
	GlobalTransactionId		gxid, prepare_gxid;
	PGXCNodeAllHandles 		*pgxc_handles;
	bool					prepared_local = false;
	int						i;

	/*
	 * Please note that with xc_maintenance_mode = on, COMMIT/ROLLBACK PREPARED will not
	 * propagate to remote nodes.  Only GTM status is cleaned up.  Prepared transaction
	 * on remote nodes will be cleaned up by pgxc_clean using EXECUTE DIRECT.
	 */
	if (xc_maintenance_mode)
	{
		if (commit)
		{
			pgxc_node_remote_commit();
			CommitPreparedTranGTM(prepare_gxid, gxid);
		}
		else
		{
			pgxc_node_remote_abort();
			RollbackTranGTM(prepare_gxid);
			RollbackTranGTM(gxid);
		}
		return false;
	}
	/*
	 * Get the list of nodes involved in this transaction.
	 *
	 * This function returns the GXID of the prepared transaction. It also
	 * returns a fresh GXID which can be used for running COMMIT PREPARED
	 * commands on the remote nodes. Both these GXIDs can then be either
	 * committed or aborted together.
	 *
	 * XXX While I understand that we get the prepared and a new GXID with a
	 * single call, it doesn't look nicer and create confusion. We should
	 * probably split them into two parts. This is used only for explicit 2PC
	 * which should not be very common in XC
	 */
	if (GetGIDDataGTM(prepareGID, &gxid, &prepare_gxid, &nodestring) < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("prepared transaction with identifier \"%s\" does not exist",
						prepareGID)));

	/*
	 * Now based on the nodestring, run COMMIT/ROLLBACK PREPARED command on the
	 * remote nodes and also finish the transaction locally is required
	 */
	nodename = strtok(nodestring, ",");
	while (nodename != NULL)
	{
#ifdef XCP
		int		nodeIndex;
		char	nodetype;

		/* Get node type and index */
		nodetype = PGXC_NODE_NONE;
		nodeIndex = PGXCNodeGetNodeIdFromName(nodename, &nodetype);
		if (nodetype == PGXC_NODE_NONE)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("PGXC Node %s: object not defined",
							nodename)));
#else
		Oid		nodeoid;
		int		nodeIndex;
		char	nodetype;

		nodeoid = get_pgxc_nodeoid(nodename);

		if (!OidIsValid(nodeoid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("PGXC Node %s: object not defined",
							nodename)));

		/* Get node type and index */
		nodetype = get_pgxc_nodetype(nodeoid);
		nodeIndex = PGXCNodeGetNodeId(nodeoid, get_pgxc_nodetype(nodeoid));
#endif

		/* Check if node is requested is the self-node or not */
		if (nodetype == PGXC_NODE_COORDINATOR)
		{
			if (nodeIndex == PGXCNodeId - 1)
				prepared_local = true;
			else
				coordlist = lappend_int(coordlist, nodeIndex);
		}
		else
			nodelist = lappend_int(nodelist, nodeIndex);

		nodename = strtok(NULL, ",");
	}

	/*
	 * Now get handles for all the involved data nodes and the coordinators
	 */
	pgxc_handles = get_handles(nodelist, coordlist, false);

	/*
	 * Send GXID (as received above) to the remote nodes.
	 if (pgxc_node_begin(pgxc_handles->dn_conn_count,
	 pgxc_handles->datanode_handles,
	 gxid, false, false))
	 ereport(ERROR,
	 (errcode(ERRCODE_INTERNAL_ERROR),
	 errmsg("Could not begin transaction on data nodes")));
	*/
	RegisterTransactionNodes(pgxc_handles->dn_conn_count,
							 (void **) pgxc_handles->datanode_handles, true);

	/*
	  if (pgxc_node_begin(pgxc_handles->co_conn_count,
	  pgxc_handles->coord_handles,
	  gxid, false, false))
	  ereport(ERROR,
	  (errcode(ERRCODE_INTERNAL_ERROR),
	  errmsg("Could not begin transaction on coordinators")));
	*/
	RegisterTransactionNodes(pgxc_handles->co_conn_count,
							 (void **) pgxc_handles->coord_handles, true);

	/*
	 * Initialize the remoteXactState so that we can use the APIs to take care
	 * of commit/abort.
	 */
	init_RemoteXactState(prepared_local);
	remoteXactState.commitXid = gxid;

	/*
	 * At this point, most of the things are set up in remoteXactState except
	 * the state information for all the involved nodes. Force that now and we
	 * are ready to call the commit/abort API
	 */
	strcpy(remoteXactState.prepareGID, prepareGID);
	for (i = 0; i < remoteXactState.numWriteRemoteNodes; i++)
		remoteXactState.remoteNodeStatus[i] = RXACT_NODE_PREPARED;
	remoteXactState.status = RXACT_PREPARED;

	if (commit)
	{
		pgxc_node_remote_commit();
		CommitPreparedTranGTM(prepare_gxid, gxid);
	}
	else
	{
		pgxc_node_remote_abort();
		RollbackTranGTM(prepare_gxid);
		RollbackTranGTM(gxid);
	}

	/*
	 * The following is also only for usual operation.  With xc_maintenance_mode = on,
	 * no remote operation will be done here and no post-operation work is needed.
	 */
	clear_RemoteXactState();
	ForgetTransactionNodes();

	return prepared_local;
}


#ifdef XCP
/*****************************************************************************
 *
 * Simplified versions of PGXC's ExecInitRemoteQuery, ExecRemoteQuery and
 * ExecEndRemoteQuery: in XCP they are only used to execute simple queries.
 *
 *****************************************************************************/
RemoteQueryState *
ExecInitRemoteQuery(RemoteQuery *node, EState *estate, int eflags)
{
	RemoteQueryState   *remotestate;
	ResponseCombiner   *combiner;

	remotestate = makeNode(RemoteQueryState);
	combiner = (ResponseCombiner *) remotestate;
	InitResponseCombiner(combiner, 0, COMBINE_TYPE_NONE);
	combiner->ss.ps.plan = (Plan *) node;
	combiner->ss.ps.state = estate;

	combiner->ss.ps.qual = NIL;

	combiner->request_type = REQUEST_TYPE_QUERY;

	ExecInitResultTupleSlot(estate, &combiner->ss.ps);
	if (node->scan.plan.targetlist)
		ExecAssignResultTypeFromTL((PlanState *) remotestate);

	/*
	 * If there are parameters supplied, get them into a form to be sent to the
	 * datanodes with bind message. We should not have had done this before.
	 */
	if (estate->es_param_list_info)
	{
		Assert(!remotestate->paramval_data);
		remotestate->paramval_len = ParamListToDataRow(estate->es_param_list_info,
												&remotestate->paramval_data);
	}

	/* We need expression context to evaluate */
	if (node->exec_nodes && node->exec_nodes->en_expr)
	{
		Expr *expr = node->exec_nodes->en_expr;

		if (IsA(expr, Var) && ((Var *) expr)->vartype == TIDOID)
		{
			/* Special case if expression does not need to be evaluated */
		}
		else
		{
			/* prepare expression evaluation */
			ExecAssignExprContext(estate, &combiner->ss.ps);
		}
	}

	return remotestate;
}


/*
 * Execute step of PGXC plan.
 * The step specifies a command to be executed on specified nodes.
 * On first invocation connections to the data nodes are initialized and
 * command is executed. Further, as well as within subsequent invocations,
 * responses are received until step is completed or there is a tuple to emit.
 * If there is a tuple it is returned, otherwise returned NULL. The NULL result
 * from the function indicates completed step.
 * The function returns at most one tuple per invocation.
 */
TupleTableSlot *
ExecRemoteQuery(RemoteQueryState *node)
{
	ResponseCombiner *combiner = (ResponseCombiner *) node;
	RemoteQuery    *step = (RemoteQuery *) combiner->ss.ps.plan;
	TupleTableSlot *resultslot = combiner->ss.ps.ps_ResultTupleSlot;
	if (!node->query_Done)
	{
		GlobalTransactionId gxid = InvalidGlobalTransactionId;
		Snapshot		snapshot = GetActiveSnapshot();
		PGXCNodeHandle **connections = NULL;
		PGXCNodeHandle *primaryconnection = NULL;
		int				i;
		int				regular_conn_count;
		int				total_conn_count;
		bool			need_tran_block;
		PGXCNodeAllHandles *pgxc_connections;

		/*
		 * Get connections for Datanodes only, utilities and DDLs
		 * are launched in ExecRemoteUtility
		 */
		pgxc_connections = get_exec_connections(node, step->exec_nodes,
												step->exec_type);

		if (step->exec_type == EXEC_ON_DATANODES)
		{
			connections = pgxc_connections->datanode_handles;
			total_conn_count = regular_conn_count = pgxc_connections->dn_conn_count;
		}
		else if (step->exec_type == EXEC_ON_COORDS)
		{
			connections = pgxc_connections->coord_handles;
			total_conn_count = regular_conn_count = pgxc_connections->co_conn_count;
		}

		primaryconnection = pgxc_connections->primary_handle;

		/*
		 * Primary connection is counted separately but is included in total_conn_count if used.
		 */
		if (primaryconnection)
			regular_conn_count--;

		pfree(pgxc_connections);

		/*
		 * We save only regular connections, at the time we exit the function
		 * we finish with the primary connection and deal only with regular
		 * connections on subsequent invocations
		 */
		combiner->node_count = regular_conn_count;

		/*
		 * Start transaction on data nodes if we are in explicit transaction
		 * or going to use extended query protocol or write to multiple nodes
		 */
		if (step->force_autocommit)
			need_tran_block = false;
		else
			need_tran_block = step->cursor ||
					(!step->read_only && total_conn_count > 1) ||
					(TransactionBlockStatusCode() == 'T');

		stat_statement();
		stat_transaction(total_conn_count);

		gxid = GetCurrentTransactionId();

		if (!GlobalTransactionIdIsValid(gxid))
		{
			if (primaryconnection)
				pfree(primaryconnection);
			pfree(connections);
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("Failed to get next transaction ID")));
		}

		/* See if we have a primary node, execute on it first before the others */
		if (primaryconnection)
		{
			if (pgxc_node_begin(1, &primaryconnection, gxid, need_tran_block,
								step->read_only, PGXC_NODE_DATANODE))
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("Could not begin transaction on data node.")));

			/* If explicit transaction is needed gxid is already sent */
			if (!pgxc_start_command_on_connection(primaryconnection, node, snapshot))
			{
				pfree(connections);
				pfree(primaryconnection);
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("Failed to send command to data nodes")));
			}
			Assert(combiner->combine_type == COMBINE_TYPE_SAME);

			pgxc_node_receive(1, &primaryconnection, NULL);
			/* Make sure the command is completed on the primary node */
			while (true)
			{
				int res = handle_response(primaryconnection, combiner);
				if (res == RESPONSE_READY)
					break;
				else if (res == RESPONSE_EOF)
					pgxc_node_receive(1, &primaryconnection, NULL);
				else if (res == RESPONSE_COMPLETE || res == RESPONSE_ERROR)
				    /* Get ReadyForQuery */
					continue;
				else
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("Unexpected response from data node")));
			}
			if (combiner->errorMessage)
			{
				char *code = combiner->errorCode;
				if (combiner->errorDetail != NULL)
					ereport(ERROR,
							(errcode(MAKE_SQLSTATE(code[0], code[1], code[2], code[3], code[4])),
							 errmsg("%s", combiner->errorMessage), errdetail("%s", combiner->errorDetail) ));
				else
					ereport(ERROR,
							(errcode(MAKE_SQLSTATE(code[0], code[1], code[2], code[3], code[4])),
							 errmsg("%s", combiner->errorMessage)));
			}
		}

		for (i = 0; i < regular_conn_count; i++)
		{
			if (pgxc_node_begin(1, &connections[i], gxid, need_tran_block,
								step->read_only, PGXC_NODE_DATANODE))
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("Could not begin transaction on data node.")));

			/* If explicit transaction is needed gxid is already sent */
			if (!pgxc_start_command_on_connection(connections[i], node, snapshot))
			{
				pfree(connections);
				if (primaryconnection)
					pfree(primaryconnection);
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("Failed to send command to data nodes")));
			}
			connections[i]->combiner = combiner;
		}

		if (step->cursor)
		{
			combiner->cursor = step->cursor;
			combiner->cursor_count = regular_conn_count;
			combiner->cursor_connections = (PGXCNodeHandle **) palloc(regular_conn_count * sizeof(PGXCNodeHandle *));
			memcpy(combiner->cursor_connections, connections, regular_conn_count * sizeof(PGXCNodeHandle *));
		}

		combiner->connections = connections;
		combiner->conn_count = regular_conn_count;
		combiner->current_conn = 0;

		if (combiner->cursor_count)
		{
			combiner->conn_count = combiner->cursor_count;
			memcpy(connections, combiner->cursor_connections,
				   combiner->cursor_count * sizeof(PGXCNodeHandle *));
			combiner->connections = connections;
		}

		node->query_Done = true;

		if (step->sort)
		{
			SimpleSort *sort = step->sort;

			/*
			 * First message is already in the buffer
			 * Further fetch will be under tuplesort control
			 * If query does not produce rows tuplesort will not
			 * be initialized
			 */
			combiner->tuplesortstate = tuplesort_begin_merge(
								   resultslot->tts_tupleDescriptor,
								   sort->numCols,
								   sort->sortColIdx,
								   sort->sortOperators,
								   sort->sortCollations,
								   sort->nullsFirst,
								   combiner,
								   work_mem);
		}
	}

	if (combiner->tuplesortstate)
 	{
		if (tuplesort_gettupleslot((Tuplesortstate *) combiner->tuplesortstate,
									  true, resultslot))
			return resultslot;
		else
			ExecClearTuple(resultslot);
	}
	else
	{
		TupleTableSlot *slot = FetchTuple(combiner);
		if (!TupIsNull(slot))
			return slot;
	}

	if (combiner->errorMessage)
 	{
		char *code = combiner->errorCode;
		if (combiner->errorDetail != NULL)
 			ereport(ERROR,
 					(errcode(MAKE_SQLSTATE(code[0], code[1], code[2], code[3], code[4])),
						errmsg("%s", combiner->errorMessage), errdetail("%s", combiner->errorDetail) ));
 		else
 			ereport(ERROR,
 					(errcode(MAKE_SQLSTATE(code[0], code[1], code[2], code[3], code[4])),
						errmsg("%s", combiner->errorMessage)));
	}

	return NULL;
}


/*
 * Clean up and discard any data on the data node connections that might not
 * handled yet, including pending on the remote connection.
 */
static void
pgxc_connections_cleanup(ResponseCombiner *combiner)
{
	/* clean up the buffer */
	list_free_deep(combiner->rowBuffer);

	/*
	 * Read in and discard remaining data from the connections, if any
	 */
	combiner->current_conn = 0;
	while (combiner->conn_count > 0)
	{
		int res;
		PGXCNodeHandle *conn = combiner->connections[combiner->current_conn];

		/*
		 * Possible if we are doing merge sort.
		 * We can do usual procedure and move connections around since we are
		 * cleaning up and do not care what connection at what position
		 */
		if (conn == NULL)
		{
			REMOVE_CURR_CONN(combiner);
			continue;
		}

		/* throw away current message that may be in the buffer */
		if (combiner->currentRow)
		{
			pfree(combiner->currentRow);
			combiner->currentRow = NULL;
		}

		/* no data is expected */
		if (conn->state == DN_CONNECTION_STATE_IDLE ||
				conn->state == DN_CONNECTION_STATE_ERROR_FATAL)
		{
			REMOVE_CURR_CONN(combiner);
			continue;
		}

		/*
		 * Connection owner is different, so no our data pending at
		 * the connection, nothing to read in.
		 */
		if (conn->combiner && conn->combiner != combiner)
		{
			REMOVE_CURR_CONN(combiner);
			continue;
		}

		res = handle_response(conn, combiner);
		if (res == RESPONSE_EOF)
		{
			struct timeval timeout;
			timeout.tv_sec = END_QUERY_TIMEOUT;
			timeout.tv_usec = 0;

			if (pgxc_node_receive(1, &conn, &timeout))
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("Failed to read response from data nodes when ending query")));
		}
	}

	/*
	 * Release tuplesort resources
	 */
	if (combiner->tuplesortstate)
	{
		/*
		 * Free these before tuplesort_end, because these arrays may appear
		 * in the tuplesort's memory context, tuplesort_end deletes this
		 * context and may invalidate the memory.
		 * We still want to free them here, because these may be in different
		 * context.
		 */
		if (combiner->tapenodes)
		{
			pfree(combiner->tapenodes);
			combiner->tapenodes = NULL;
		}
		if (combiner->tapemarks)
		{
			pfree(combiner->tapemarks);
			combiner->tapemarks = NULL;
		}
		/*
		 * tuplesort_end invalidates minimal tuple if it is in the slot because
		 * deletes the TupleSort memory context, causing seg fault later when
		 * releasing tuple table
		 */
		ExecClearTuple(combiner->ss.ps.ps_ResultTupleSlot);
		tuplesort_end((Tuplesortstate *) combiner->tuplesortstate);
		combiner->tuplesortstate = NULL;
	}
}


/*
 * End the remote query
 */
void
ExecEndRemoteQuery(RemoteQueryState *node)
{
	ResponseCombiner *combiner = (ResponseCombiner *) node;

	/*
	 * Clean up remote connections
	 */
	pgxc_connections_cleanup(combiner);

	/*
	 * Clean up parameters if they were set, since plan may be reused
	 */
	if (node->paramval_data)
	{
		pfree(node->paramval_data);
		node->paramval_data = NULL;
		node->paramval_len = 0;
	}

	CloseCombiner(combiner);
	pfree(node);
}


/**********************************************
 *
 * Routines to support RemoteSubplan plan node
 *
 **********************************************/
struct find_params_context
{
	RemoteParam *rparams;
	Bitmapset *defineParams;
};

static bool
determine_param_types_walker(Node *node, struct find_params_context *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, Param))
	{
		Param *param = (Param *) node;
		int paramno = param->paramid;

		if (param->paramkind == PARAM_EXEC &&
				bms_is_member(paramno, context->defineParams))
		{
			RemoteParam *cur = context->rparams;
			while (cur->paramkind != PARAM_EXEC || cur->paramid != paramno)
				cur++;
			cur->paramtype = param->paramtype;
			context->defineParams = bms_del_member(context->defineParams,
												   paramno);
			return bms_is_empty(context->defineParams);
		}
	}
	return expression_tree_walker(node, determine_param_types_walker,
								  (void *) context);

}

/*
 * Scan expressions in the plan tree to find Param nodes and get data types
 * from them
 */
static bool
determine_param_types(Plan *plan,  struct find_params_context *context)
{
	Bitmapset *intersect;

	if (plan == NULL)
		return false;

	intersect = bms_intersect(plan->allParam, context->defineParams);
	if (bms_is_empty(intersect))
	{
		/* the subplan does not depend on params we are interested in */
		bms_free(intersect);
		return false;
	}
	bms_free(intersect);

	/* scan target list */
	if (expression_tree_walker((Node *) plan->targetlist,
							   determine_param_types_walker,
							   (void *) context))
		return true;
	/* scan qual */
	if (expression_tree_walker((Node *) plan->qual,
							   determine_param_types_walker,
							   (void *) context))
		return true;

	/* Check additional node-type-specific fields */
	switch (nodeTag(plan))
	{
		case T_Result:
			if (expression_tree_walker((Node *) ((Result *) plan)->resconstantqual,
									   determine_param_types_walker,
									   (void *) context))
				return true;
			break;

		case T_SeqScan:
			break;

		case T_IndexScan:
			if (expression_tree_walker((Node *) ((IndexScan *) plan)->indexqual,
									   determine_param_types_walker,
									   (void *) context))
				return true;
			break;

		case T_BitmapIndexScan:
			if (expression_tree_walker((Node *) ((BitmapIndexScan *) plan)->indexqual,
									   determine_param_types_walker,
									   (void *) context))
				return true;
			break;

		case T_BitmapHeapScan:
			if (expression_tree_walker((Node *) ((BitmapHeapScan *) plan)->bitmapqualorig,
									   determine_param_types_walker,
									   (void *) context))
				return true;
			break;

		case T_TidScan:
			if (expression_tree_walker((Node *) ((TidScan *) plan)->tidquals,
									   determine_param_types_walker,
									   (void *) context))
				return true;
			break;

		case T_SubqueryScan:
			if (determine_param_types(((SubqueryScan *) plan)->subplan, context))
				return true;
			break;

		case T_FunctionScan:
			if (expression_tree_walker((Node *) ((FunctionScan *) plan)->funcexpr,
									   determine_param_types_walker,
									   (void *) context))
				return true;
			break;

		case T_ValuesScan:
			if (expression_tree_walker((Node *) ((ValuesScan *) plan)->values_lists,
									   determine_param_types_walker,
									   (void *) context))
				return true;
			break;

		case T_ModifyTable:
			{
				ListCell   *l;

				foreach(l, ((ModifyTable *) plan)->plans)
				{
					if (determine_param_types((Plan *) lfirst(l), context))
						return true;
				}
			}
			break;

		case T_RemoteSubplan:
			break;

		case T_Append:
			{
				ListCell   *l;

				foreach(l, ((Append *) plan)->appendplans)
				{
					if (determine_param_types((Plan *) lfirst(l), context))
						return true;
				}
			}
			break;

		case T_BitmapAnd:
			{
				ListCell   *l;

				foreach(l, ((BitmapAnd *) plan)->bitmapplans)
				{
					if (determine_param_types((Plan *) lfirst(l), context))
						return true;
				}
			}
			break;

		case T_BitmapOr:
			{
				ListCell   *l;

				foreach(l, ((BitmapOr *) plan)->bitmapplans)
				{
					if (determine_param_types((Plan *) lfirst(l), context))
						return true;
				}
			}
			break;

		case T_NestLoop:
			if (expression_tree_walker((Node *) ((Join *) plan)->joinqual,
									   determine_param_types_walker,
									   (void *) context))
				return true;
			break;

		case T_MergeJoin:
			if (expression_tree_walker((Node *) ((Join *) plan)->joinqual,
									   determine_param_types_walker,
									   (void *) context))
				return true;
			if (expression_tree_walker((Node *) ((MergeJoin *) plan)->mergeclauses,
									   determine_param_types_walker,
									   (void *) context))
				return true;
			break;

		case T_HashJoin:
			if (expression_tree_walker((Node *) ((Join *) plan)->joinqual,
									   determine_param_types_walker,
									   (void *) context))
				return true;
			if (expression_tree_walker((Node *) ((HashJoin *) plan)->hashclauses,
									   determine_param_types_walker,
									   (void *) context))
				return true;
			break;

		case T_Limit:
			if (expression_tree_walker((Node *) ((Limit *) plan)->limitOffset,
									   determine_param_types_walker,
									   (void *) context))
				return true;
			if (expression_tree_walker((Node *) ((Limit *) plan)->limitCount,
									   determine_param_types_walker,
									   (void *) context))
				return true;
			break;

		case T_RecursiveUnion:
			break;

		case T_LockRows:
			break;

		case T_WindowAgg:
			if (expression_tree_walker((Node *) ((WindowAgg *) plan)->startOffset,
									   determine_param_types_walker,
									   (void *) context))
			if (expression_tree_walker((Node *) ((WindowAgg *) plan)->endOffset,
									   determine_param_types_walker,
									   (void *) context))
			break;

		case T_Hash:
		case T_Agg:
		case T_Material:
		case T_Sort:
		case T_Unique:
		case T_SetOp:
		case T_Group:
			break;

		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(plan));
	}


	/* recurse into subplans */
	return determine_param_types(plan->lefttree, context) ||
			determine_param_types(plan->righttree, context);
}


RemoteSubplanState *
ExecInitRemoteSubplan(RemoteSubplan *node, EState *estate, int eflags)
{
	RemoteStmt			rstmt;
	RemoteSubplanState *remotestate;
	ResponseCombiner   *combiner;
	CombineType			combineType;

	remotestate = makeNode(RemoteSubplanState);
	combiner = (ResponseCombiner *) remotestate;
	/*
	 * We do not need to combine row counts if we will receive intermediate
	 * results or if we won't return row count.
	 */
	if (IS_PGXC_DATANODE || estate->es_plannedstmt->commandType == CMD_SELECT)
	{
		combineType = COMBINE_TYPE_NONE;
		remotestate->execOnAll = node->execOnAll;
	}
	else
	{
		if (node->execOnAll)
			combineType = COMBINE_TYPE_SUM;
		else
			combineType = COMBINE_TYPE_SAME;
		/*
		 * If we are updating replicated table we should run plan on all nodes.
		 * We are choosing single node only to read
		 */
		remotestate->execOnAll = true;
	}
	remotestate->execNodes = list_copy(node->nodeList);
	InitResponseCombiner(combiner, 0, combineType);
	combiner->ss.ps.plan = (Plan *) node;
	combiner->ss.ps.state = estate;

	combiner->ss.ps.qual = NIL;

	combiner->request_type = REQUEST_TYPE_QUERY;

	ExecInitResultTupleSlot(estate, &combiner->ss.ps);
	ExecAssignResultTypeFromTL((PlanState *) remotestate);

	/*
	 * We optimize execution if we going to send down query to next level
	 */
	remotestate->local_exec = false;
	if (IS_PGXC_DATANODE)
	{
		if (remotestate->execNodes == NIL)
		{
			/*
			 * Special case, if subplan is not distributed, like Result, or
			 * query against catalog tables only.
			 * We are only interested in filtering out the subplan results and
			 * get only those we are interested in.
			 * XXX we may want to prevent multiple executions in this case
			 * either, to achieve this we will set single execNode on planning
			 * time and this case would never happen, this code branch could
			 * be removed.
			 */
			remotestate->local_exec = true;
		}
		else if (!remotestate->execOnAll)
		{
			/*
			 * XXX We should change planner and remove this flag.
			 * We want only one node is producing the replicated result set,
			 * and planner should choose that node - it is too hard to determine
			 * right node at execution time, because it should be guaranteed
			 * that all consumers make the same decision.
			 * For now always execute replicated plan on local node to save
			 * resources.
			 */

			/*
			 * Make sure local node is in execution list
			 */
			if (list_member_int(remotestate->execNodes, PGXCNodeId-1))
			{
				list_free(remotestate->execNodes);
				remotestate->execNodes = NIL;
				remotestate->local_exec = true;
			}
			else
			{
				/*
				 * To support, we need to connect to some producer, so
				 * each producer should be prepared to serve rows for random
				 * number of consumers. It is hard, because new consumer may
				 * connect after producing is started, on the other hand,
				 * absence of expected consumer is a problem too.
				 */
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("Getting replicated results from remote node is not supported")));
			}
		}
	}

	/*
	 * If we are going to execute subplan locally or doing explain in itialize
	 * the subplan. Otherwise have remote node doing that.
	 */
	if (remotestate->local_exec || (eflags & EXEC_FLAG_EXPLAIN_ONLY))
	{
		outerPlanState(remotestate) = ExecInitNode(outerPlan(node), estate,
												   eflags);
		if (node->distributionNodes)
		{
			Oid 		distributionType = InvalidOid;
			TupleDesc 	typeInfo;

			typeInfo = combiner->ss.ps.ps_ResultTupleSlot->tts_tupleDescriptor;
			if (node->distributionKey != InvalidAttrNumber)
			{
				Form_pg_attribute attr;
				attr = typeInfo->attrs[node->distributionKey - 1];
				distributionType = attr->atttypid;
			}
			/* Set up locator */
			remotestate->locator = createLocator(node->distributionType,
												 RELATION_ACCESS_INSERT,
												 distributionType,
												 LOCATOR_LIST_LIST,
												 0,
												 (void *) node->distributionNodes,
												 (void **) &remotestate->dest_nodes,
												 false);
		}
		else
			remotestate->locator = NULL;
	}

	/*
	 * Encode subplan if it will be sent to remote nodes
	 */
	if (remotestate->execNodes && !(eflags & EXEC_FLAG_EXPLAIN_ONLY))
	{
		ParamListInfo ext_params;
		/* Encode plan if we are going to execute it on other nodes */
		rstmt.type = T_RemoteStmt;
		rstmt.planTree = outerPlan(node);
		/*
		 * If datanode launch further execution of a command it should tell
		 * it is a SELECT, otherwise secondary data nodes won't return tuples
		 * expecting there will be nothing to return.
		 */
		if (IsA(outerPlan(node), ModifyTable))
		{
			rstmt.commandType = estate->es_plannedstmt->commandType;
			rstmt.hasReturning = estate->es_plannedstmt->hasReturning;
			rstmt.resultRelations = estate->es_plannedstmt->resultRelations;
		}
		else
		{
			rstmt.commandType = CMD_SELECT;
			rstmt.hasReturning = false;
			rstmt.resultRelations = NIL;
		}
		rstmt.rtable = estate->es_range_table;
		rstmt.subplans = estate->es_plannedstmt->subplans;
		rstmt.nParamExec = estate->es_plannedstmt->nParamExec;
		ext_params = estate->es_param_list_info;
		rstmt.nParamRemote = (ext_params ? ext_params->numParams : 0) +
				bms_num_members(node->scan.plan.allParam);
		if (rstmt.nParamRemote > 0)
		{
			Bitmapset *tmpset;
			int i;
			int paramno;

			/* Allocate enough space */
			rstmt.remoteparams = (RemoteParam *) palloc(rstmt.nParamRemote *
														sizeof(RemoteParam));
			paramno = 0;
			if (ext_params)
			{
				for (i = 0; i < ext_params->numParams; i++)
				{
					ParamExternData *param = &ext_params->params[i];
					/*
					 * If parameter type is not yet defined but can be defined
					 * do that
					 */
					if (!OidIsValid(param->ptype) && ext_params->paramFetch)
						(*ext_params->paramFetch) (ext_params, i + 1);
					/*
					 * If parameter type is still not defined assume it is
					 * unused
					 */
					if (!OidIsValid(param->ptype))
						continue;

					rstmt.remoteparams[paramno].paramkind = PARAM_EXTERN;
					rstmt.remoteparams[paramno].paramid = i + 1;
					rstmt.remoteparams[paramno].paramtype = param->ptype;
					paramno++;
				}
				/* store actual number of parameters */
				rstmt.nParamRemote = paramno;
			}

			if (!bms_is_empty(node->scan.plan.allParam))
			{
				Bitmapset *defineParams = NULL;
				tmpset = bms_copy(node->scan.plan.allParam);
				while ((i = bms_first_member(tmpset)) >= 0)
				{
					ParamExecData *prmdata;

					prmdata = &(estate->es_param_exec_vals[i]);
					rstmt.remoteparams[paramno].paramkind = PARAM_EXEC;
					rstmt.remoteparams[paramno].paramid = i;
					rstmt.remoteparams[paramno].paramtype = prmdata->ptype;
					/* Will scan plan tree to find out data type of the param */
					if (prmdata->ptype == InvalidOid)
						defineParams = bms_add_member(defineParams, i);
					paramno++;
				}
				/* store actual number of parameters */
				rstmt.nParamRemote = paramno;
				bms_free(tmpset);
				if (!bms_is_empty(defineParams))
				{
					struct find_params_context context;
					bool all_found;

					context.rparams = rstmt.remoteparams;
					context.defineParams = defineParams;

					all_found = determine_param_types(node->scan.plan.lefttree,
													  &context);
					/*
					 * Remove not defined params from the list of remote params.
					 * If they are not referenced no need to send them down
					 */
					if (!all_found)
					{
						for (i = 0; i < rstmt.nParamRemote; i++)
						{
							if (rstmt.remoteparams[i].paramkind == PARAM_EXEC &&
									bms_is_member(rstmt.remoteparams[i].paramid,
												  context.defineParams))
							{
								/* Copy last parameter inplace */
								rstmt.nParamRemote--;
								if (i < rstmt.nParamRemote)
									rstmt.remoteparams[i] =
										rstmt.remoteparams[rstmt.nParamRemote];
								/* keep current in the same position */
								i--;
							}
						}
					}
					bms_free(context.defineParams);
				}
			}
			remotestate->nParamRemote = rstmt.nParamRemote;
			remotestate->remoteparams = rstmt.remoteparams;
		}
		else
			rstmt.remoteparams = NULL;
		rstmt.distributionKey = node->distributionKey;
		rstmt.distributionType = node->distributionType;
		rstmt.distributionNodes = node->distributionNodes;
		rstmt.distributionRestrict = node->distributionRestrict;

		set_portable_output(true);
		remotestate->subplanstr = nodeToString(&rstmt);
		set_portable_output(false);

		/*
		 * Connect to remote nodes and send down subplan
		 */
		if (!(eflags & EXEC_FLAG_SUBPLAN))
			ExecFinishInitRemoteSubplan(remotestate);
	}
	remotestate->bound = false;
	/*
	 * It does not makes sense to merge sort if there is only one tuple source.
	 * By the contract it is already sorted
	 */
	if (node->sort && remotestate->execOnAll &&
			list_length(remotestate->execNodes) > 1)
		combiner->merge_sort = true;

	return remotestate;
}


void
ExecFinishInitRemoteSubplan(RemoteSubplanState *node)
{
	ResponseCombiner   *combiner = (ResponseCombiner *) node;
	RemoteSubplan  	   *plan = (RemoteSubplan *) combiner->ss.ps.plan;
	Oid        		   *paramtypes = NULL;
	GlobalTransactionId gxid = InvalidGlobalTransactionId;
	Snapshot			snapshot;
	TimestampTz			timestamp;
	int 				i;
	bool				is_read_only;

	/*
	 * Name is required to store plan as a statement
	 */
	Assert(plan->cursor);

	/* If it is alreaty fully initialized nothing to do */
	if (combiner->connections)
		return;

	/* local only or explain only execution */
	if (node->subplanstr == NULL)
		return;

	/*
	 * Acquire connections and send down subplan where it will be stored
	 * as a prepared statement.
	 * That does not require transaction id or snapshot, so does not send them
	 * here, postpone till bind.
	 */
	if (node->execOnAll)
	{
		PGXCNodeAllHandles *pgxc_connections;
		pgxc_connections = get_handles(node->execNodes, NIL, false);
		combiner->conn_count = pgxc_connections->dn_conn_count;
		combiner->connections = pgxc_connections->datanode_handles;
		combiner->current_conn = 0;
		pfree(pgxc_connections);
	}
	else
	{
		combiner->connections = (PGXCNodeHandle **) palloc(sizeof(PGXCNodeHandle *));
		combiner->connections[0] = get_any_handle(node->execNodes);
		combiner->conn_count = 1;
		combiner->current_conn = 0;
	}

	/*
	 * If we are getting xid on a datanode we may fall into locking problem.
	 * When xid is allocated by a session it is locked but a datanode may run
	 * multiple session within the same transaction.
	 * Missing xid should not be a problem on a secondary datanode session,
	 * it is only reading data.
	 */
	if (IS_PGXC_COORDINATOR)
	{
		gxid = GetCurrentTransactionId();
		if (!GlobalTransactionIdIsValid(gxid))
		{
			combiner->conn_count = 0;
			pfree(combiner->connections);
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("Failed to get next transaction ID")));
		}
	}

	/* extract parameter data types */
	if (node->nParamRemote > 0)
	{
		paramtypes = (Oid *) palloc(node->nParamRemote * sizeof(Oid));
		for (i = 0; i < node->nParamRemote; i++)
			paramtypes[i] = node->remoteparams[i].paramtype;
	}
	/* send down subplan */
	snapshot = GetActiveSnapshot();
	timestamp = GetCurrentGTMStartTimestamp();
	/*
	 * Datanode should not send down statements that may modify
	 * the database. Potgres assumes that all sessions under the same
	 * postmaster have different xids. That may cause a locking problem.
	 * Shared locks acquired for reading still work fine.
	 */
	is_read_only = IS_PGXC_DATANODE ||
			!IsA(outerPlan(plan), ModifyTable);

	for (i = 0; i < combiner->conn_count; i++)
	{
		if (pgxc_node_begin(1, &combiner->connections[i], gxid, true,
							is_read_only, PGXC_NODE_DATANODE))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("Could not begin transaction on data node.")));

		if (pgxc_node_send_timestamp(combiner->connections[i], timestamp))
		{
			combiner->conn_count = 0;
			pfree(combiner->connections);
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("Failed to send command to data nodes")));
		}
		if (snapshot && pgxc_node_send_snapshot(combiner->connections[i], snapshot))
		{
			combiner->conn_count = 0;
			pfree(combiner->connections);
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("Failed to send command to data nodes")));
		}
		pgxc_node_send_plan(combiner->connections[i], plan->cursor,
							"Remote Subplan", node->subplanstr,
							node->nParamRemote, paramtypes);
		if (pgxc_node_flush(combiner->connections[i]))
		{
			combiner->conn_count = 0;
			pfree(combiner->connections);
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("Failed to send subplan to data nodes")));
		}
	}
}


static void
append_param_data(StringInfo buf, Oid ptype, Datum value, bool isnull)
{
	uint32 n32;

	if (isnull)
	{
		n32 = htonl(-1);
		appendBinaryStringInfo(buf, (char *) &n32, 4);
	}
	else
	{
		Oid		typOutput;
		bool	typIsVarlena;
		Datum	pval;
		char   *pstring;
		int		len;

		/* Get info needed to output the value */
		getTypeOutputInfo(ptype, &typOutput, &typIsVarlena);

		/*
		 * If we have a toasted datum, forcibly detoast it here to avoid
		 * memory leakage inside the type's output routine.
		 */
		if (typIsVarlena)
			pval = PointerGetDatum(PG_DETOAST_DATUM(value));
		else
			pval = value;

		/* Convert Datum to string */
		pstring = OidOutputFunctionCall(typOutput, pval);

		/* copy data to the buffer */
		len = strlen(pstring);
		n32 = htonl(len);
		appendBinaryStringInfo(buf, (char *) &n32, 4);
		appendBinaryStringInfo(buf, pstring, len);
	}
}


static int encode_parameters(int nparams, RemoteParam *remoteparams,
							 PlanState *planstate, char** result)
{
	EState 		   *estate = planstate->state;
	StringInfoData	buf;
	uint16 			n16;
	int 			i;

	initStringInfo(&buf);

	/* Number of parameter values */
	n16 = htons(nparams);
	appendBinaryStringInfo(&buf, (char *) &n16, 2);

	/* Parameter values */
	for (i = 0; i < nparams; i++)
	{
		RemoteParam *rparam = &remoteparams[i];
		int ptype = rparam->paramtype;
		if (rparam->paramkind == PARAM_EXTERN)
		{
			ParamExternData *param;
			param = &(estate->es_param_list_info->params[rparam->paramid - 1]);
			append_param_data(&buf, ptype, param->value, param->isnull);
		}
		else
		{
			ParamExecData *param;
			param = &(estate->es_param_exec_vals[rparam->paramid]);
			if (param->execPlan)
			{
				if (planstate->ps_ExprContext == NULL)
					ExecAssignExprContext(estate, planstate);

				/* Parameter not evaluated yet, so go do it */
				ExecSetParamPlan((SubPlanState *) param->execPlan,
								 planstate->ps_ExprContext);
				/* ExecSetParamPlan should have processed this param... */
				Assert(param->execPlan == NULL);
			}
			append_param_data(&buf, ptype, param->value, param->isnull);
		}
	}

	/* Take data from the buffer */
	*result = palloc(buf.len);
	memcpy(*result, buf.data, buf.len);
	pfree(buf.data);
	return buf.len;
}


TupleTableSlot *
ExecRemoteSubplan(RemoteSubplanState *node)
{
	ResponseCombiner *combiner = (ResponseCombiner *) node;
	RemoteSubplan  *plan = (RemoteSubplan *) combiner->ss.ps.plan;
	EState		   *estate = combiner->ss.ps.state;
	TupleTableSlot *resultslot = combiner->ss.ps.ps_ResultTupleSlot;

	if (!node->bound)
	{
		int fetch = 0;
		int paramlen = 0;
		char *paramdata = NULL;

		if (plan->cursor)
			fetch = 1000;

		/*
		 * Send down all available parameters, if any is used by the plan
		 */
		if (estate->es_param_list_info ||
				!bms_is_empty(plan->scan.plan.allParam))
			paramlen = encode_parameters(node->nParamRemote,
										 node->remoteparams,
										 &combiner->ss.ps,
										 &paramdata);

		/*
		 * The subplan being rescanned, need to restore connections and
		 * re-bind the portal
		 */
		if (combiner->cursor)
		{
			int i;

			combiner->conn_count = combiner->cursor_count;
			memcpy(combiner->connections, combiner->cursor_connections,
						combiner->cursor_count * sizeof(PGXCNodeHandle *));

			for (i = 0; i < combiner->conn_count; i++)
			{
				PGXCNodeHandle *conn = combiner->connections[i];

				CHECK_OWNERSHIP(conn, combiner);
				/* close previous cursor */
				pgxc_node_send_close(conn, false, combiner->cursor);
				/* rebind */
				pgxc_node_send_bind(conn, combiner->cursor, combiner->cursor,
									paramlen, paramdata);
				/* execute */
				pgxc_node_send_execute(conn, combiner->cursor, fetch);
				/* submit */
				if (pgxc_node_send_flush(conn))
				{
					combiner->conn_count = 0;
					pfree(combiner->connections);
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("Failed to send command to data nodes")));
				}
			}
		}
		else if (node->execNodes)
		{
			int 			i;

			/*
			 * There are prepared statement, connections should be already here
			 */
			Assert(combiner->conn_count > 0);

			combiner->extended_query = true;

			for (i = 0; i < combiner->conn_count; i++)
			{
				CHECK_OWNERSHIP(combiner->connections[i], combiner);

				/* bind */
				pgxc_node_send_bind(combiner->connections[i], plan->cursor,
									plan->cursor, paramlen, paramdata);
				/* execute */
				pgxc_node_send_execute(combiner->connections[i], plan->cursor,
									   fetch);
				/* submit */
				if (pgxc_node_send_flush(combiner->connections[i]))
				{
					combiner->conn_count = 0;
					pfree(combiner->connections);
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("Failed to send command to data nodes")));
				}

			}
			combiner->cursor = plan->cursor;
			combiner->cursor_count = combiner->conn_count;
			combiner->cursor_connections = (PGXCNodeHandle **) palloc(
						combiner->conn_count * sizeof(PGXCNodeHandle *));
			memcpy(combiner->cursor_connections, combiner->connections,
						combiner->conn_count * sizeof(PGXCNodeHandle *));
		}

		if (combiner->merge_sort)
		{
			/*
			 * Requests are already made and sorter can fetch tuples to populate
			 * sort buffer.
			 */
			combiner->tuplesortstate = tuplesort_begin_merge(
									   resultslot->tts_tupleDescriptor,
									   plan->sort->numCols,
									   plan->sort->sortColIdx,
									   plan->sort->sortOperators,
									   plan->sort->sortCollations,
									   plan->sort->nullsFirst,
									   combiner,
									   work_mem);
		}
		node->bound = true;
	}

	if (combiner->tuplesortstate)
	{
		if (tuplesort_gettupleslot((Tuplesortstate *) combiner->tuplesortstate,
								   true, resultslot))
			return resultslot;
	}
	else
	{
		TupleTableSlot *slot = FetchTuple(combiner);
		if (!TupIsNull(slot))
			return slot;
	}
	if (combiner->errorMessage)
	{
		char *code = combiner->errorCode;
		if (combiner->errorDetail)
			ereport(ERROR,
					(errcode(MAKE_SQLSTATE(code[0], code[1], code[2], code[3], code[4])),
					 errmsg("%s", combiner->errorMessage), errdetail("%s", combiner->errorDetail) ));
		else
			ereport(ERROR,
					(errcode(MAKE_SQLSTATE(code[0], code[1], code[2], code[3], code[4])),
					 errmsg("%s", combiner->errorMessage)));
	}
	return NULL;
}


void
ExecReScanRemoteSubplan(RemoteSubplanState *node)
{
	ResponseCombiner *combiner = (ResponseCombiner *)node;

	/*
	 * If we haven't queried remote nodes yet, just return. If outerplan'
	 * chgParam is not NULL then it will be re-scanned by ExecProcNode,
	 * else - no reason to re-scan it at all.
	 */
	if (!node->bound)
		return;

	/*
	 * If we execute locally rescan local copy of the plan
	 */
	if (outerPlanState(node))
		ExecReScan(outerPlanState(node));

	/*
	 * Consume any possible pending input
	 */
	pgxc_connections_cleanup(combiner);

	/* misc cleanup */
	combiner->command_complete_count = 0;
	combiner->description_count = 0;

	/*
	 * Force query is re-bound with new parameters
	 */
	node->bound = false;
}


void
ExecEndRemoteSubplan(RemoteSubplanState *node)
{
	ResponseCombiner *combiner = (ResponseCombiner *)node;
	RemoteSubplan    *plan = (RemoteSubplan *) combiner->ss.ps.plan;
	int i;

	if (outerPlanState(node))
		ExecEndNode(outerPlanState(node));
	if (node->locator)
		freeLocator(node->locator);

	/*
	 * Consume any possible pending input
	 */
	if (node->bound)
		pgxc_connections_cleanup(combiner);

	/*
	 * Close portals. While cursors_connections exist there are open portals
	 */
	if (combiner->cursor)
	{
		/* Restore connections where there are active statements */
		combiner->conn_count = combiner->cursor_count;
		memcpy(combiner->connections, combiner->cursor_connections,
					combiner->cursor_count * sizeof(PGXCNodeHandle *));
		for (i = 0; i < combiner->cursor_count; i++)
		{
			PGXCNodeHandle *conn;

			conn = combiner->cursor_connections[i];

			CHECK_OWNERSHIP(conn, combiner);

			if (pgxc_node_send_close(conn, false, combiner->cursor) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("Failed to close data node cursor")));
		}
		/* The cursor stuff is not needed */
		combiner->cursor = NULL;
		combiner->cursor_count = 0;
		pfree(combiner->cursor_connections);
		combiner->cursor_connections = NULL;
	}

	/* Close statements, even if they never were bound */
	for (i = 0; i < combiner->conn_count; i++)
	{
		PGXCNodeHandle *conn;

		conn = combiner->connections[i];

		CHECK_OWNERSHIP(conn, combiner);

		if (pgxc_node_send_close(conn, true, plan->cursor) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("Failed to close data node statement")));
		/* Send SYNC and wait for ReadyForQuery */
		if (pgxc_node_send_sync(conn) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("Failed to synchronize data node")));
		/*
		 * Formally connection is not in QUERY state, we set the state to read
		 * CloseDone and ReadyForQuery responses. Upon receiving ReadyForQuery
		 * state will be changed back to IDLE and conn->coordinator will be
		 * cleared.
		 */
		conn->state = DN_CONNECTION_STATE_QUERY;
	}

	while (combiner->conn_count > 0)
	{
		if (pgxc_node_receive(combiner->conn_count,
							  combiner->connections, NULL))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("Failed to close remote subplan")));
		i = 0;
		while (i < combiner->conn_count)
		{
			int res = handle_response(combiner->connections[i], combiner);
			if (res == RESPONSE_EOF)
			{
				i++;
			}
			else if (res == RESPONSE_READY)
			{
				/* Done, connection is reade for query */
				if (--combiner->conn_count > i)
					combiner->connections[i] =
							combiner->connections[combiner->conn_count];
			}
			/* Ignore other possible responses */
		}
	}

	ValidateAndCloseCombiner(combiner);
	pfree(node);
}
#endif
