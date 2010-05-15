/*-------------------------------------------------------------------------
 *
 * poolmgr.c
 *
 *	  Connection pool manager handles connections to DataNodes
 *
 * The pooler runs as a separate process and is forked off from a 
 * coordinator postmaster. If the coordinator needs a connection from a 
 * data node, it asks for one from the pooler, which maintains separate
 * pools for each data node. A group of connections can be requested in
 * a single request, and the pooler returns a list of file descriptors 
 * to use for the connections.
 *
 * Note the current implementation does not yet shrink the pool over time
 * as connections are idle.  Also, it does not queue requests; if a 
 * connection is unavailable, it will simply fail. This should be implemented
 * one day, although there is a chance for deadlocks. For now, limiting
 * connections should be done between the application and coordinator.
 * Still, this is useful to avoid having to re-establish connections to the
 * data nodes all the time for multiple coordinator backend sessions.
 *
 * The term "agent" here refers to a session manager, one for each backend
 * coordinator connection to the pooler. It will contain a list of connections
 * allocated to a session, at most one per data node.
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 2010 Nippon Telegraph and Telephone Corporation
 *
 * IDENTIFICATION
 *	  $$
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <signal.h>
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgxc/poolmgr.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "pgxc/locator.h"
#include "../interfaces/libpq/libpq-fe.h"
#include "postmaster/postmaster.h"		/* For UnixSocketDir */
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>

/* Configuration options */
int			NumDataNodes = 2;
int			MinPoolSize = 1;
int			MaxPoolSize = 100;
int			PoolerPort = 6667;

bool		PersistentConnections = false;

/* The memory context */
static MemoryContext PoolerMemoryContext = NULL;

/* Connection info */
char	   *DataNodeHosts = NULL;
char	   *DataNodePorts = NULL;
char	   *DataNodeUsers = NULL;
char	   *DataNodePwds = NULL;

/* Connection info list */
static DataNodeConnectionInfo *connectionInfos;

/* Pool to all the databases (linked list) */
static DatabasePool *databasePools = NULL;

/* PoolAgents */
static int	agentCount = 0;
static PoolAgent **poolAgents;

static PoolHandle *Handle = NULL;

static int	server_fd = -1;

static void agent_init(PoolAgent *agent, const char *database, List *nodes);
static void agent_destroy(PoolAgent *agent);
static void agent_create(void);
static void agent_handle_input(PoolAgent *agent, StringInfo s);
static DatabasePool *create_database_pool(const char *database, List *nodes);
static void insert_database_pool(DatabasePool *pool);
static int	destroy_database_pool(const char *database);
static DatabasePool *find_database_pool(const char *database);
static DatabasePool *remove_database_pool(const char *database);
static int *agent_acquire_connections(PoolAgent *agent, List *nodelist);
static DataNodePoolSlot *acquire_connection(DatabasePool *dbPool, int node);
static void agent_release_connections(PoolAgent *agent, bool clean);
static void release_connection(DatabasePool *dbPool, DataNodePoolSlot *slot, int index, bool clean);
static void destroy_slot(DataNodePoolSlot *slot);
static void grow_pool(DatabasePool *dbPool, int index);
static void destroy_node_pool(DataNodePool *node_pool);
static void PoolerLoop(void);

/* Signal handlers */
static void pooler_die(SIGNAL_ARGS);
static void pooler_quickdie(SIGNAL_ARGS);

/* Check status of connection */
extern int	pqReadReady(PGconn *conn);

/*
 * Flags set by interrupt handlers for later service in the main loop.
 */
static volatile sig_atomic_t shutdown_requested = false;


/* 
 * Initialize internal structures 
 */
int
PoolManagerInit()
{
	char	   *rawstring;
	List	   *elemlist;
	ListCell   *l;
	int			i;
	MemoryContext old_context;

	elog(DEBUG1, "Pooler process is started: %d", getpid());

	/*
	 * Set up memory context for the pooler
	 */
	PoolerMemoryContext = AllocSetContextCreate(TopMemoryContext,
												"PoolerMemoryContext",
												ALLOCSET_DEFAULT_MINSIZE,
												ALLOCSET_DEFAULT_INITSIZE,
												ALLOCSET_DEFAULT_MAXSIZE);

	/*
	 * If possible, make this process a group leader, so that the postmaster
	 * can signal any child processes too.	(pool manager probably never has any
	 * child processes, but for consistency we make all postmaster child
	 * processes do this.)
	 */
#ifdef HAVE_SETSID
	if (setsid() < 0)
		elog(FATAL, "setsid() failed: %m");
#endif
	/*
	 * Properly accept or ignore signals the postmaster might send us
	 */
	pqsignal(SIGINT, pooler_die);
	pqsignal(SIGTERM, pooler_die);
	pqsignal(SIGQUIT, pooler_quickdie);
	pqsignal(SIGHUP, SIG_IGN);
	/* TODO other signal handlers */

	/* We allow SIGQUIT (quickdie) at all times */
#ifdef HAVE_SIGPROCMASK
	sigdelset(&BlockSig, SIGQUIT);
#else
	BlockSig &= ~(sigmask(SIGQUIT));
#endif

	/*
	 * Unblock signals (they were blocked when the postmaster forked us)
	 */
	PG_SETMASK(&UnBlockSig);

	/* Allocate pooler structures in the Pooler context */
	old_context = MemoryContextSwitchTo(PoolerMemoryContext);

	poolAgents = (PoolAgent **) palloc(MaxConnections * sizeof(PoolAgent *));
	if (poolAgents == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}

	connectionInfos = (DataNodeConnectionInfo *) palloc(NumDataNodes * sizeof(DataNodeConnectionInfo));
	if (connectionInfos == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}

	/* Need a modifiable copy */
	rawstring = pstrdup(DataNodeHosts);

	/* Parse string into list of identifiers */
	if (!SplitIdentifierString(rawstring, ',', &elemlist))
	{
		/* syntax error in list */
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid list syntax for \"data_node_hosts\"")));
	}

	i = 0;
	foreach(l, elemlist)
	{
		char	   *curhost = (char *) lfirst(l);

		connectionInfos[i].host = pstrdup(curhost);
		if (connectionInfos[i].host == NULL)
		{
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		}
		/* Ignore extra entries, if any */
		if (++i == NumDataNodes)
			break;
	}
	list_free(elemlist);
	pfree(rawstring);

	/* Validate */
	if (i == 0)
	{
		/* syntax error in list */
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid list syntax for \"data_node_hosts\"")));
	}
	else if (i == 1)
	{
		/* Copy all values from first */
		for (; i < NumDataNodes; i++)
		{
			connectionInfos[i].host = pstrdup(connectionInfos[0].host);
			if (connectionInfos[i].host == NULL)
			{
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory")));
			}
		}
	}
	else if (i < NumDataNodes)
	{
		/* syntax error in list */
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid list syntax for \"data_node_hosts\"")));
	}

	/* Need a modifiable copy */
	rawstring = pstrdup(DataNodePorts);

	/* Parse string into list of identifiers */
	if (!SplitIdentifierString(rawstring, ',', &elemlist))
	{
		/* syntax error in list */
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid list syntax for \"data_node_ports\"")));
	}

	i = 0;
	foreach(l, elemlist)
	{
		char	   *curport = (char *) lfirst(l);

		connectionInfos[i].port = pstrdup(curport);
		if (connectionInfos[i].port == NULL)
		{
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		}
		/* Ignore extra entries, if any */
		if (++i == NumDataNodes)
			break;
	}
	list_free(elemlist);
	pfree(rawstring);

	/* Validate */
	if (i == 0)
	{
		/* syntax error in list */
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid list syntax for \"data_node_ports\"")));
	}
	else if (i == 1)
	{
		/* Copy all values from first */
		for (; i < NumDataNodes; i++)
		{
			connectionInfos[i].port = pstrdup(connectionInfos[0].port);
			if (connectionInfos[i].port == NULL)
			{
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory")));
			}
		}
	}
	else if (i < NumDataNodes)
	{
		/* syntax error in list */
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid list syntax for \"data_node_ports\"")));
	}

	rawstring = pstrdup(DataNodeUsers);

	/* Parse string into list of identifiers */
	if (!SplitIdentifierString(rawstring, ',', &elemlist))
	{
		/* syntax error in list */
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid list syntax for \"data_node_users\"")));
	}

	i = 0;
	foreach(l, elemlist)
	{
		char	   *curuser = (char *) lfirst(l);

		connectionInfos[i].uname = pstrdup(curuser);
		if (connectionInfos[i].uname == NULL)
		{
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		}
		/* Ignore extra entries, if any */
		if (++i == NumDataNodes)
			break;
	}
	list_free(elemlist);
	pfree(rawstring);

	/* Validate */
	if (i == 0)
	{
		/* syntax error in list */
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid list syntax for \"data_node_users\"")));
	}
	else if (i == 1)
	{
		/* Copy all values from first */
		for (; i < NumDataNodes; i++)
		{
			connectionInfos[i].uname = pstrdup(connectionInfos[0].uname);
			if (connectionInfos[i].uname == NULL)
			{
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory")));
			}
		}
	}
	else if (i < NumDataNodes)
	{
		/* syntax error in list */
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid list syntax for \"data_node_users\"")));
	}

	rawstring = pstrdup(DataNodePwds);

	/* Parse string into list of identifiers */
	if (!SplitIdentifierString(rawstring, ',', &elemlist))
	{
		/* syntax error in list */
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid list syntax for \"data_node_passwords\"")));
	}

	i = 0;
	foreach(l, elemlist)
	{
		char	   *curpassword = (char *) lfirst(l);

		connectionInfos[i].password = pstrdup(curpassword);
		if (connectionInfos[i].password == NULL)
		{
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		}
		/* Ignore extra entries, if any */
		if (++i == NumDataNodes)
			break;
	}
	list_free(elemlist);
	pfree(rawstring);

	/* Validate */
	if (i == 0)
	{
		/* syntax error in list */
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid list syntax for \"data_node_passwords\"")));
	}
	else if (i == 1)
	{
		/* Copy all values from first */
		for (; i < NumDataNodes; i++)
		{
			connectionInfos[i].password = pstrdup(connectionInfos[0].password);
			if (connectionInfos[i].password == NULL)
			{
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory")));
			}
		}
	}
	else if (i < NumDataNodes)
	{
		/* syntax error in list */
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid list syntax for \"data_node_passwords\"")));
	}

	PoolerLoop();
	return 0;
}


/* 
 * Destroy internal structures 
 */
int
PoolManagerDestroy(void)
{
	int			status = 0;

	if (PoolerMemoryContext)
	{
		MemoryContextDelete(PoolerMemoryContext);
		PoolerMemoryContext = NULL;
	}

	return status;
}


/*
 * Get handle to pool manager
 * Invoked from Postmaster's main loop just before forking off new session
 * Returned PoolHandle structure will be inherited by session process
 */
PoolHandle *
GetPoolManagerHandle(void)
{
	PoolHandle *handle;
	int			fdsock;

	/* Connect to the pooler */
	fdsock = pool_connect(PoolerPort, UnixSocketDir);
	if (fdsock < 0)
	{
		int			saved_errno = errno;

		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("failed to connect to pool manager: %m")));
		errno = saved_errno;
		return NULL;
	}

	/* Allocate handle */
	/*
	 * XXX we may change malloc here to palloc but first ensure
	 * the CurrentMemoryContext is properly set.
	 * The handle allocated just before new session is forked off and
	 * inherited by the session process. It should remain valid for all
	 * the session lifetime.
	 */
	handle = (PoolHandle *) malloc(sizeof(PoolHandle));
	if (!handle)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
		return NULL;
	}

	handle->port.fdsock = fdsock;
	handle->port.RecvLength = 0;
	handle->port.RecvPointer = 0;
	handle->port.SendPointer = 0;

	return handle;
}


/*
 * Close handle
 */
void
PoolManagerCloseHandle(PoolHandle *handle)
{
	close(Socket(handle->port));
	free(handle);
}


/*
 * Create agent
 */
static void
agent_create(void)
{
	int			new_fd;
	PoolAgent  *agent;

	new_fd = accept(server_fd, NULL, NULL);
	if (new_fd < 0)
	{
		int			saved_errno = errno;

		ereport(LOG,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("pool manager failed to accept connection: %m")));
		errno = saved_errno;
		return;
	}

	/* Allocate agent */
	agent = (PoolAgent *) palloc(sizeof(PoolAgent));
	if (!agent)
	{
		close(new_fd);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
		return;
	}

	agent->port.fdsock = new_fd;
	agent->port.RecvLength = 0;
	agent->port.RecvPointer = 0;
	agent->port.SendPointer = 0;
	agent->pool = NULL;
	agent->connections = NULL;

	/* Append new agent to the list */
	poolAgents[agentCount++] = agent;
}


/*
 * Associate session with specified database and respective connection pool
 * Invoked from Session process
 */
void
PoolManagerConnect(PoolHandle *handle, const char *database)
{
	Assert(handle);
	Assert(database);

	/* Save the handle */
	Handle = handle;

	/* Send database name followed by \0 terminator */
	pool_putmessage(&handle->port, 'c', database, strlen(database) + 1);
	pool_flush(&handle->port);
}


/* 
 * Init PoolAgent 
*/
static void
agent_init(PoolAgent *agent, const char *database, List *nodes)
{
	Assert(agent);
	Assert(database);
	Assert(list_length(nodes) > 0);

	/* disconnect if we still connected */
	if (agent->pool)
		agent_release_connections(agent, false);

	/* find database */
	agent->pool = find_database_pool(database);

	/* create if not found */
	if (agent->pool == NULL)
		agent->pool = create_database_pool(database, nodes);
}


/* 
 * Destroy PoolAgent 
 */
static void
agent_destroy(PoolAgent *agent)
{
	int			i;

	Assert(agent);

	close(Socket(agent->port));

	/* Discard connections if any remaining */
	if (agent->pool)
		agent_release_connections(agent, false);

	/* find agent in the list */
	for (i = 0; i < agentCount; i++)
	{
		if (poolAgents[i] == agent)
		{
			/* free memory */
			if (agent->connections)
			{
				pfree(agent->connections);
				agent->connections = NULL;
			}
			pfree(agent);
			/* shrink the list and move last agent into the freed slot */
			if (i < --agentCount)
				poolAgents[i] = poolAgents[agentCount];
			/* only one match is expected so exit */
			break;
		}
	}
}


/* 
 * Release handle to pool manager 
 */
void
PoolManagerDisconnect(PoolHandle *handle)
{
	Assert(handle);

	pool_putmessage(&handle->port, 'd', NULL, 0);
	pool_flush(&Handle->port);

	close(Socket(handle->port));

	pfree(handle);
}


/* 
 * Get pooled connections 
 */
int *
PoolManagerGetConnections(List *nodelist)
{
	int			i;
	ListCell   *nodelist_item;
	int		   *fds;
	int			nodes[list_length(nodelist) + 1];

	Assert(Handle);
	Assert(list_length(nodelist) > 0);

	/* Prepare end send message to pool manager */
	nodes[0] = htonl(list_length(nodelist));
	i = 1;
	foreach(nodelist_item, nodelist)
	{
		nodes[i++] = htonl(lfirst_int(nodelist_item));
	}
	pool_putmessage(&Handle->port, 'g', (char *) nodes, sizeof(int) * (list_length(nodelist) + 1));
	pool_flush(&Handle->port);
	/* Receive response */
	fds = (int *) palloc(sizeof(int) * list_length(nodelist));
	if (fds == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}
	if (pool_recvfds(&Handle->port, fds, list_length(nodelist)))
	{
		pfree(fds);
		return NULL;
	}
	return fds;
}


/*
 * Handle messages to agent
 */
static void
agent_handle_input(PoolAgent * agent, StringInfo s)
{
	int			qtype;
	const char *database;
	int			nodecount;
	List	   *nodelist = NIL;
	int		   *fds;
	int			i;

	qtype = pool_getbyte(&agent->port);
	/*
	 * We can have multiple messages, so handle them all
	 */
	for (;;)
	{
		switch (qtype)
		{
			case 'c':			/* CONNECT */
				pool_getmessage(&agent->port, s, 0);
				database = pq_getmsgstring(s);
				agent_init(agent, database, GetAllNodes());
				pq_getmsgend(s);
				break;
			case 'd':			/* DISCONNECT */
				pool_getmessage(&agent->port, s, 4);
				agent_destroy(agent);
				pq_getmsgend(s);
				break;
			case 'g':			/* GET CONNECTIONS */
				pool_getmessage(&agent->port, s, 4 * NumDataNodes + 8);
				nodecount = pq_getmsgint(s, 4);
				for (i = 0; i < nodecount; i++)
				{
					nodelist = lappend_int(nodelist, pq_getmsgint(s, 4));
				}
				pq_getmsgend(s);
				/*
				 * In case of error agent_acquire_connections will log
				 * the error and return NULL
				 */
				fds = agent_acquire_connections(agent, nodelist);
				list_free(nodelist);
				pool_sendfds(&agent->port, fds, fds ? nodecount : 0);
				if (fds)
					pfree(fds);
				break;
			case 'r':			/* RELEASE CONNECTIONS */
				pool_getmessage(&agent->port, s, 4);
				pq_getmsgend(s);
				agent_release_connections(agent, true);
				break;
			default:			/* EOF or protocol violation */
				agent_destroy(agent);
				return;
		}
		/* avoid reading from connection */
		if ((qtype = pool_pollbyte(&agent->port)) == EOF)
			break;
	}
}


/* 
 * acquire connection
 */
static int *
agent_acquire_connections(PoolAgent *agent, List *nodelist)
{
	int			i;
	int		   *result;
	ListCell   *nodelist_item;

	Assert(agent);
	Assert(nodelist);

	/* Allocate memory */
	result = (int *) palloc(list_length(nodelist) * sizeof(int));
	if (result == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}

	/* initialize connection if it is not initialized yet */
	if (!agent->connections)
	{
		agent->connections = (DataNodePoolSlot **) palloc(NumDataNodes * sizeof(DataNodePoolSlot *));
		if (!agent->connections)
		{
			pfree(result);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
			return NULL;
		}

		for (i = 0; i < NumDataNodes; i++)
			agent->connections[i] = NULL;
	}

	/* Initialize result */
	i = 0;
	foreach(nodelist_item, nodelist)
	{
		int			node = lfirst_int(nodelist_item);

		/* Acquire from the pool if none */
		if (agent->connections[node - 1] == NULL)
		{
			DataNodePoolSlot *slot = acquire_connection(agent->pool, node);

			/* Handle failure */
			if (slot == NULL)
			{
				pfree(result);
				return NULL;
			}

			/* Store in the descriptor */
			agent->connections[node - 1] = slot;
		}

		result[i++] = PQsocket((PGconn *) agent->connections[node - 1]->conn);
	}

	return result;
}


/* 
 * Retun connections back to the pool 
 */
void
PoolManagerReleaseConnections(void)
{
	Assert(Handle);

	pool_putmessage(&Handle->port, 'r', NULL, 0);
	pool_flush(&Handle->port);
}


/*
 * Release connections
 */
static void
agent_release_connections(PoolAgent *agent, bool clean)
{
	int			i;

	if (!agent->connections)
		return;

	/* Enumerate connections */
	for (i = 0; i < NumDataNodes; i++)
	{
		DataNodePoolSlot *slot;

		slot = agent->connections[i];

		/* Release connection */
		if (slot)
			release_connection(agent->pool, slot, i, clean);
		agent->connections[i] = NULL;
	}
}


/*
 * Create new empty pool for a database and insert into the list
 * Returns POOL_OK if operation succeed POOL_FAIL in case of OutOfMemory
 * error and POOL_WEXIST if poll for this database already exist
 */
static DatabasePool *
create_database_pool(const char *database, List *nodes)
{
	DatabasePool *databasePool;
	int			i;
	ListCell   *l;

	Assert(nodes && list_length(nodes) > 0);

	/* check if exist */
	databasePool = find_database_pool(database);
	if (databasePool)
	{
		/* already exist */
		return databasePool;
	}

	/* Allocate memory */
	databasePool = (DatabasePool *) palloc(sizeof(DatabasePool));
	if (!databasePool)
	{
		/* out of memory */
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
		return NULL;
	}

	 /* Copy the database name */ ;
	databasePool->database = pstrdup(database);
	if (!databasePool->database)
	{
		/* out of memory */
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
		pfree(databasePool);
		return NULL;
	}

	/* Init next reference */
	databasePool->next = NULL;

	/* Init data node pools */
	databasePool->nodePools = (DataNodePool **) palloc(NumDataNodes * sizeof(DataNodePool **));
	if (!databasePool->nodePools)
	{
		/* out of memory */
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
		pfree(databasePool->database);
		pfree(databasePool);
		return NULL;
	}
	for (i = 0; i < NumDataNodes; i++)
		databasePool->nodePools[i] = NULL;

	foreach(l, nodes)
	{
		int			nodeid = lfirst_int(l);

		grow_pool(databasePool, nodeid - 1);
	}

	/* Insert into the list */
	insert_database_pool(databasePool);

	return databasePool;
}


/*
 * Destroy the pool and free memory
 */
static int
destroy_database_pool(const char *database)
{
	DatabasePool *databasePool;
	int			i;

	/* Delete from the list */
	databasePool = remove_database_pool(database);
	if (databasePool)
	{
		if (databasePool->nodePools)
		{
			for (i = 0; i < NumDataNodes; i++)
				if (databasePool->nodePools[i])
					destroy_node_pool(databasePool->nodePools[i]);
			pfree(databasePool->nodePools);
		}
		/* free allocated memory */
		pfree(databasePool->database);
		pfree(databasePool);
		return 1;
	}
	return 0;
}


/* 
 * Insert new database pool to the list 
 */
static void
insert_database_pool(DatabasePool *databasePool)
{
	Assert(databasePool);

	/* Reference existing list or null the tail */
	if (databasePools)
		databasePool->next = databasePools;
	else
		databasePool->next = NULL;

	/* Update head pointer */
	databasePools = databasePool;
}


/* 
 * Find pool for specified database in the list 
 */
static DatabasePool
*
find_database_pool(const char *database)
{
	DatabasePool *databasePool;

	/* Scan the list */
	databasePool = databasePools;
	while (databasePool)
	{

		/* if match break the loop and return */
		if (strcmp(database, databasePool->database) == 0)
			break;
		databasePool = databasePool->next;

	}
	return databasePool;
}


/* 
 * Remove pool for specified database from the list 
 */
static DatabasePool *
remove_database_pool(const char *database)
{
	DatabasePool *databasePool,
			   *prev;

	/* Scan the list */
	databasePool = databasePools;
	prev = NULL;
	while (databasePool)
	{

		/* if match break the loop and return */
		if (strcmp(database, databasePool->database) == 0)
			break;
		prev = databasePool;
		databasePool = databasePool->next;
	}

	/* if found */
	if (databasePool)
	{

		/* Remove entry from chain or update head */
		if (prev)
			prev->next = databasePool->next;
		else
			databasePools = databasePool->next;


		databasePool->next = NULL;
	}
	return databasePool;
}

/*
 * Acquire connection
 */
static DataNodePoolSlot *
acquire_connection(DatabasePool *dbPool, int node)
{
	DataNodePool *nodePool;
	DataNodePoolSlot *slot;

	Assert(dbPool);
	Assert(0 < node && node <= NumDataNodes);

	slot = NULL;
	/* Find referenced node pool */
	nodePool = dbPool->nodePools[node - 1];
	if (nodePool == NULL || nodePool->freeSize == 0)
	{
		grow_pool(dbPool, node - 1);
		nodePool = dbPool->nodePools[node - 1];
	}

	/* Check available connections */
	if (nodePool && nodePool->freeSize > 0)
	{
		int			poll_result;

		while (nodePool->freeSize > 0)
		{
			slot = nodePool->slot[--(nodePool->freeSize)];

	retry:
			/* Make sure connection is ok */
			poll_result = pqReadReady(slot->conn);

			if (poll_result == 0)
				break; 		/* ok, no data */
			else if (poll_result < 0)
			{
				if (errno == EAGAIN || errno == EINTR)
					goto retry;

				elog(WARNING, "Error in checking connection, errno = %d", errno);
			}
			else
				elog(WARNING, "Unexpected data on connection, cleaning.");

			destroy_slot(slot);
			/* Decrement current max pool size */
			(nodePool->size)--;
			/* Ensure we are not below minimum size */
			grow_pool(dbPool, node - 1);
		}
	}
	else
		ereport(LOG,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("connection pool is empty")));
	return slot;
}


/*
 * release connection from specified pool and slot
 */
static void
release_connection(DatabasePool * dbPool, DataNodePoolSlot * slot, int index, bool clean)
{
	DataNodePool *nodePool;

	Assert(dbPool);
	Assert(slot);
	Assert(0 <= index && index < NumDataNodes);

	/* Find referenced node pool */
	nodePool = dbPool->nodePools[index];
	if (nodePool == NULL)
	{
		/* report problem */
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("database does not use node %d", (index + 1))));
		return;
	}

	/* return or discard */
	if (clean)
	{
		/* Insert the slot into the array and increase pool size */
		nodePool->slot[(nodePool->freeSize)++] = slot;
	}
	else
	{
		elog(DEBUG1, "Cleaning up connection from pool %s, closing", nodePool->connstr);
		destroy_slot(slot);
		/* Decrement pool size */
		(nodePool->size)--;
		/* Ensure we are not below minimum size */
		grow_pool(dbPool, index);
	}
}


/*
 * Increase database pool size
 */
static void
grow_pool(DatabasePool * dbPool, int index)
{
	DataNodePool *nodePool;

	Assert(dbPool);
	Assert(0 <= index && index < NumDataNodes);

	/* Find referenced node pool */
	nodePool = dbPool->nodePools[index];
	if (!nodePool)
	{
		/* Allocate new DBNode Pool */
		nodePool = (DataNodePool *) palloc(sizeof(DataNodePool));
		if (!nodePool)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));

		/* initialize it */
		nodePool->connstr = DataNodeConnStr(
											connectionInfos[index].host,
											connectionInfos[index].port,
											dbPool->database,
											connectionInfos[index].uname,
											connectionInfos[index].password);

		if (!nodePool->connstr)
		{
			pfree(nodePool);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		}

		nodePool->slot = (DataNodePoolSlot **) palloc(MaxPoolSize * sizeof(DataNodePoolSlot *));
		if (!nodePool->slot)
		{
			pfree(nodePool);
			pfree(nodePool->connstr);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		}
		memset(nodePool->slot, 0, MaxPoolSize * sizeof(DataNodePoolSlot *));
		nodePool->freeSize = 0;
		nodePool->size = 0;

		/* and insert into the array */
		dbPool->nodePools[index] = nodePool;
	}

	while (nodePool->size < MinPoolSize || (nodePool->freeSize == 0 && nodePool->size < MaxPoolSize))
	{
		DataNodePoolSlot *slot;

		/* Allocate new slot */
		slot = (DataNodePoolSlot *) palloc(sizeof(DataNodePoolSlot));
		if (slot == NULL)
		{
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		}

		/* Establish connection */
		slot->conn = DataNodeConnect(nodePool->connstr);
		if (!DataNodeConnected(slot->conn))
		{
			destroy_slot(slot);
			ereport(LOG,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("failed to connect to data node")));
			break;
		}

		/* Insert at the end of the pool */
		nodePool->slot[(nodePool->freeSize)++] = slot;

		/* Increase count of pool size */
		(nodePool->size)++;
		elog(DEBUG1, "Pooler: increased pool size to %d for pool %s",
			 nodePool->size,
			 nodePool->connstr);
	}
}


/*
 * Destroy pool slot
 */
static void
destroy_slot(DataNodePoolSlot *slot)
{
	DataNodeClose(slot->conn);
	pfree(slot);
}


/*
 * Destroy node pool
 */
static void
destroy_node_pool(DataNodePool *node_pool)
{
	int			i;

	/*
	 * At this point all agents using connections from this pool should be already closed
	 * If this not the connections to the data nodes assigned to them remain open, this will
	 * consume data node resources.
	 * I believe this is not the case because pool is only destroyed on coordinator shutdown.
	 * However we should be careful when changing thinds
	 */
	elog(DEBUG1, "About to destroy node pool %s, current size is %d, %d connections are in use",
		 node_pool->connstr, node_pool->freeSize, node_pool->size - node_pool->freeSize);
	if (node_pool->connstr)
		pfree(node_pool->connstr);

	if (node_pool->slot)
	{
		for (i = 0; i < node_pool->freeSize; i++)
			destroy_slot(node_pool->slot[i]);
		pfree(node_pool->slot);
	}
}


/*
 * Main handling loop
 */
static void
PoolerLoop(void)
{
	StringInfoData input_message;

	server_fd = pool_listen(PoolerPort, UnixSocketDir);
	if (server_fd == -1)
	{
		/* log error */
		return;
	}
	initStringInfo(&input_message);
	for (;;)
	{
		int			nfds;
		fd_set		rfds;
		int			retval;
		int			i;

		/*
		 * Emergency bailout if postmaster has died.  This is to avoid the
		 * necessity for manual cleanup of all postmaster children.
		 */
		if (!PostmasterIsAlive(true))
			exit(1);

		/* watch for incoming connections */
		FD_ZERO(&rfds);
		FD_SET(server_fd, &rfds);

		nfds = server_fd;

		/* watch for incoming messages */
		for (i = 0; i < agentCount; i++)
		{
			PoolAgent  *agent = poolAgents[i];
			int			sockfd = Socket(agent->port);
			FD_SET		(sockfd, &rfds);

			nfds = Max(nfds, sockfd);
		}

		/* wait for event */
		retval = select(nfds + 1, &rfds, NULL, NULL, NULL);
		if (shutdown_requested)
		{
			for (i = agentCount - 1; i >= 0; i--)
			{
				PoolAgent  *agent = poolAgents[i];

				agent_destroy(agent);
			}
			while (databasePools)
				if (destroy_database_pool(databasePools->database) == 0)
					break;
			close(server_fd);
			exit(0);
		}
		if (retval > 0)
		{
			/*
			 * Agent may be removed from the array while processing
			 * and trailing items are shifted, so scroll downward
			 * to avoid problem
			 */
			for (i = agentCount - 1; i >= 0; i--)
			{
				PoolAgent  *agent = poolAgents[i];
				int			sockfd = Socket(agent->port);

				if (FD_ISSET(sockfd, &rfds))
					agent_handle_input(agent, &input_message);
			}
			if (FD_ISSET(server_fd, &rfds))
				agent_create();
		}
	}
}


/*
 *
 */
static void
pooler_die(SIGNAL_ARGS)
{
	shutdown_requested = true;
}


/*
 *
 */
static void
pooler_quickdie(SIGNAL_ARGS)
{
	PG_SETMASK(&BlockSig);
	exit(2);
}
