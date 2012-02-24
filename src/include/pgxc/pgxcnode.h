/*-------------------------------------------------------------------------
 *
 * pgxcnode.h
 *
 *	  Utility functions to communicate to Datanodes and Coordinators
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group ?
 * Portions Copyright (c) 2010-2012 Nippon Telegraph and Telephone Corporation
 *
 * IDENTIFICATION
 *	  $$
 *
 *-------------------------------------------------------------------------
 */

#ifndef DATANODE_H
#define DATANODE_H
#include "postgres.h"
#include "gtm/gtm_c.h"
#include "utils/timestamp.h"
#include "nodes/pg_list.h"
#include "utils/snapshot.h"
#include <unistd.h>

#define NO_SOCKET -1

/* Connection to data node maintained by Pool Manager */
typedef struct PGconn NODE_CONNECTION;
typedef struct PGcancel NODE_CANCEL;

/* Helper structure to access data node from Session */
typedef enum
{
	DN_CONNECTION_STATE_IDLE,			/* idle, ready for query */
	DN_CONNECTION_STATE_QUERY,			/* query is sent, response expected */
	DN_CONNECTION_STATE_ERROR_FATAL,	/* fatal error */
	DN_CONNECTION_STATE_COPY_IN,
	DN_CONNECTION_STATE_COPY_OUT
}	DNConnectionState;

typedef enum
{
	HANDLE_IDLE,
	HANDLE_ERROR,
	HANDLE_DEFAULT
}	PGXCNode_HandleRequested;

#define DN_CONNECTION_STATE_ERROR(dnconn) \
		((dnconn)->state == DN_CONNECTION_STATE_ERROR_FATAL \
			|| (dnconn)->transaction_status == 'E')

#define HAS_MESSAGE_BUFFERED(conn) \
		((conn)->inCursor + 4 < (conn)->inEnd \
			&& (conn)->inCursor + ntohl(*((uint32_t *) ((conn)->inBuffer + (conn)->inCursor + 1))) < (conn)->inEnd)

struct pgxc_node_handle
{
	Oid			nodeoid;

	/* fd of the connection */
	int		sock;
	/* Connection state */
	char		transaction_status;
	DNConnectionState state;
#ifdef XCP
	struct ResponseCombiner *combiner;
#else
	struct RemoteQueryState *combiner;
#endif
#ifdef DN_CONNECTION_DEBUG
	bool		have_row_desc;
#endif
	char		*error;
	/* Output buffer */
	char		*outBuffer;
	size_t		outSize;
	size_t		outEnd;
	/* Input buffer */
	char		*inBuffer;
	size_t		inSize;
	size_t		inStart;
	size_t		inEnd;
	size_t		inCursor;
};
typedef struct pgxc_node_handle PGXCNodeHandle;

/* Structure used to get all the handles involved in a transaction */
typedef struct
{
	PGXCNodeHandle	   *primary_handle;	/* Primary connection to PGXC node */
	int					dn_conn_count;	/* number of Datanode Handles including primary handle */
	PGXCNodeHandle	  **datanode_handles;	/* an array of Datanode handles */
	int					co_conn_count;	/* number of Coordinator handles */
	PGXCNodeHandle	  **coord_handles;	/* an array of Coordinator handles */
} PGXCNodeAllHandles;

extern void InitMultinodeExecutor(bool is_force);

/* Open/close connection routines (invoked from Pool Manager) */
#ifdef XCP
extern char *PGXCNodeConnStr(char *host, int port, char *dbname, char *user,
							 char *remote_type, char *parent_node);
#else
extern char *PGXCNodeConnStr(char *host, int port, char *dbname, char *user,
							 char *remote_type);
#endif
extern NODE_CONNECTION *PGXCNodeConnect(char *connstr);
extern int PGXCNodeSendSetQuery(NODE_CONNECTION *conn, const char *sql_command);
extern void PGXCNodeClose(NODE_CONNECTION * conn);
extern int PGXCNodeConnected(NODE_CONNECTION * conn);
extern int PGXCNodeConnClean(NODE_CONNECTION * conn);
extern void PGXCNodeCleanAndRelease(int code, Datum arg);

#ifdef XCP
extern PGXCNodeHandle *get_any_handle(List *datanodelist);
#endif
/* Look at information cached in node handles */
extern int PGXCNodeGetNodeId(Oid nodeoid, char node_type);
extern Oid PGXCNodeGetNodeOid(int nodeid, char node_type);
extern int PGXCNodeGetNodeIdFromName(char *node_name, char node_type);

extern PGXCNodeAllHandles *get_handles(List *datanodelist, List *coordlist, bool is_query_coord_only);
extern void pfree_pgxc_all_handles(PGXCNodeAllHandles *handles);

extern void release_handles(void);
extern void cancel_query(void);
extern void clear_all_data(void);


extern int get_transaction_nodes(PGXCNodeHandle ** connections,
								  char client_conn_type,
								  PGXCNode_HandleRequested type_requested);
extern char* collect_pgxcnode_names(char *nodestring, int conn_count, PGXCNodeHandle ** connections, char client_conn_type);
extern char* collect_localnode_name(char *nodestring);
extern int	get_active_nodes(PGXCNodeHandle ** connections);

extern int	ensure_in_buffer_capacity(size_t bytes_needed, PGXCNodeHandle * handle);
extern int	ensure_out_buffer_capacity(size_t bytes_needed, PGXCNodeHandle * handle);

extern int	pgxc_node_send_query(PGXCNodeHandle * handle, const char *query);
extern int	pgxc_node_send_describe(PGXCNodeHandle * handle, bool is_statement,
						const char *name);
extern int	pgxc_node_send_execute(PGXCNodeHandle * handle, const char *portal, int fetch);
extern int	pgxc_node_send_close(PGXCNodeHandle * handle, bool is_statement,
					 const char *name);
extern int	pgxc_node_send_sync(PGXCNodeHandle * handle);
extern int	pgxc_node_send_bind(PGXCNodeHandle * handle, const char *portal,
								const char *statement, int paramlen, char *params);
extern int	pgxc_node_send_parse(PGXCNodeHandle * handle, const char* statement,
								 const char *query, short num_params, Oid *param_types);
extern int	pgxc_node_send_flush(PGXCNodeHandle * handle);
extern int	pgxc_node_send_query_extended(PGXCNodeHandle *handle, const char *query,
							  const char *statement, const char *portal,
							  int num_params, Oid *param_types,
							  int paramlen, char *params,
							  bool send_describe, int fetch_size);
#ifdef XCP
extern int  pgxc_node_send_plan(PGXCNodeHandle * handle, const char *statement,
					const char *query, const char *planstr,
					short num_params, Oid *param_types);
#endif
extern int	pgxc_node_send_gxid(PGXCNodeHandle * handle, GlobalTransactionId gxid);
extern int	pgxc_node_send_snapshot(PGXCNodeHandle * handle, Snapshot snapshot);
extern int	pgxc_node_send_timestamp(PGXCNodeHandle * handle, TimestampTz timestamp);

extern bool	pgxc_node_receive(const int conn_count,
				  PGXCNodeHandle ** connections, struct timeval * timeout);
extern int	pgxc_node_read_data(PGXCNodeHandle * conn, bool close_if_error);
extern int	pgxc_node_is_data_enqueued(PGXCNodeHandle *conn);

extern int	send_some(PGXCNodeHandle * handle, int len);
extern int	pgxc_node_flush(PGXCNodeHandle *handle);
extern void	pgxc_node_flush_read(PGXCNodeHandle *handle);

extern int	pgxc_all_handles_send_gxid(PGXCNodeAllHandles *pgxc_handles, GlobalTransactionId gxid, bool stop_at_error);
extern int	pgxc_all_handles_send_query(PGXCNodeAllHandles *pgxc_handles, const char *buffer, bool stop_at_error);

extern char get_message(PGXCNodeHandle *conn, int *len, char **msg);

extern void add_error_message(PGXCNodeHandle * handle, const char *message);

extern Datum pgxc_execute_on_nodes(int numnodes, Oid *nodelist, char *query);

#endif
