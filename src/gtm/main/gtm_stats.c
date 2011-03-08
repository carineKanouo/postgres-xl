/*-------------------------------------------------------------------------
 *
 * gtm_stats.c
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2010-2011 Nippon Telegraph and Telephone Corporation
 *
 *
 * IDENTIFICATION
 *	  src/gtm/main/gtm_stats.c
 *
 *-------------------------------------------------------------------------
 */
typedef struct GTM_Stats
{
	int 	GTM_RecvMessages[GTM_MAX_MESSAGE_TYPE];
	int 	GTM_SentMessages[GTM_MAX_MESSAGE_TYPE];
	float	GTM_RecvBytes;
	float	GTM_SentBytes;
} GTM_Stats;

