/*
 * $Id$
 *
 * Copyright (c) 2004, Raphael Manfredi
 *
 *----------------------------------------------------------------------
 * This file is part of gtk-gnutella.
 *
 *  gtk-gnutella is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  gtk-gnutella is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with gtk-gnutella; if not, write to the Free Software
 *  Foundation, Inc.:
 *      59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *----------------------------------------------------------------------
 */

/**
 * @file
 *
 * Dynamic query hits.
 */

#include "gnutella.h"

RCSID("$Id$");

#include <glib.h>

#include "dh.h"
#include "atoms.h"
#include "misc.h"
#include "walloc.h"
#include "nodes.h"
#include "gmsg.h"
#include "mq.h"
#include "gnet_stats.h"
#include "gnet_net_stats.h"
#include "override.h"		/* Must be the last header included */

#define DH_HALF_LIFE	300		/* 5 minutes */
#define DH_MIN_HITS		250		/* Minimum amount of hits we try to relay */
#define DH_POPULAR_HITS	500		/* Query deemed popular after that many hits */
#define DH_MAX_HITS		1000	/* Maximum hits after which we heavily drop */

/*
 * Information about query hits received.
 */
typedef struct dqhit {
	guint32 msg_recv;		/* Amount of individual messages we got */
	guint32 hits_recv;		/* Total amount of results we saw */
	guint32 hits_sent;		/* Total amount of results we sent back */
	guint32 hits_queued;	/* Amount of hits queued */
} dqhit_t;

/*
 * Meta-information about the query hit message.
 */
struct pmsg_info {
	guint32 hits;			/* Amount of query hits held in message */
};

/*
 * These tables keep track of the association between a MUID and the
 * query hit info.  We keep two hash tables and they are "rotated" every so
 * and then, the current table becoming the old one and the old being
 * cleaned up.
 *
 * The keys are MUIDs (GUID atoms), the values are the dqhit_t object.
 */
static GHashTable *by_muid = NULL;
static GHashTable *by_muid_old = NULL;
static time_t last_rotation;

/**
 * Hashtable iteration callback to free the MUIDs in the `by_muid' table,
 * and the associated dqhit_t objects.
 */
static gboolean
free_muid_true(gpointer key, gpointer value, gpointer udata)
{
	atom_guid_free(key);
	wfree(value, sizeof(dqhit_t));
	return TRUE;
}

/**
 * Clear specified hash table.
 */
static void
dh_table_clear(GHashTable *ht)
{
	g_assert(ht != NULL);

	g_hash_table_foreach_remove(ht, free_muid_true, NULL);
}

/**
 * Free specified hash table.
 */
static void
dh_table_free(GHashTable *ht)
{
	g_assert(ht != NULL);

	g_hash_table_foreach_remove(ht, free_muid_true, NULL);
	g_hash_table_destroy(ht);
}

/**
 * Locate record for query hits for specified MUID.
 *
 * @returns located record, or NULL if not found.
 */
static dqhit_t *
dh_locate(gchar *muid)
{
	gboolean found = FALSE;
	gpointer key;
	gpointer value;

	/*
	 * Look in the old table first.  If we find something there, move it
	 * to the new table to keep te record "alive" since we still get hits
	 * for this query.
	 */

	found = g_hash_table_lookup_extended(by_muid_old, muid, &key, &value);

	if (found) {
		g_hash_table_remove(by_muid_old, key);
		g_hash_table_insert(by_muid, key, value);
		return (dqhit_t *) value;
	}

	return (dqhit_t *) g_hash_table_lookup(by_muid, muid);
}

/**
 * Create new record for query hits for speicifed MUID.
 * New record is registered in the current table.
 */
static dqhit_t *
dh_create(gchar *muid)
{
	dqhit_t *dh;
	gchar *key;

	dh = walloc0(sizeof(*dh));
	key = atom_guid_get(muid);

	g_hash_table_insert(by_muid, key, dh);

	return dh;
}

/**
 * Called every time we successfully parsed a query hit from the network.
 */
void
dh_got_results(gchar *muid, gint count)
{
	dqhit_t *dh;

	g_assert(count > 0);

	dh = dh_locate(muid);
	if (dh == NULL)
		dh = dh_create(muid);

	dh->msg_recv++;
	dh->hits_recv += count;
}

/**
 * Periodic heartbeat, to rotate the hash tables every half-life period.
 */
void
dh_timer(time_t now)
{
	GHashTable *tmp;

	if (delta_time(now, last_rotation) < DH_HALF_LIFE)
		return;

	/*
	 * Rotate the hash tables.
	 */

	tmp = by_muid;
	dh_table_clear(by_muid_old);

	by_muid = by_muid_old;
	by_muid_old = tmp;

	last_rotation = now;

	if (dh_debug > 19)
		printf("DH rotated tables\n");
}

/**
 * Free routine for query hit message.
 */
static void
dh_pmsg_free(pmsg_t *mb, gpointer arg)
{
	struct pmsg_info *pmi = (struct pmsg_info *) arg;
	gchar *muid;
	dqhit_t *dh;

	g_assert(pmsg_is_extended(mb));

	muid = ((struct gnutella_header *) pmsg_start(mb))->muid;
	dh = dh_locate(muid);

	if (dh == NULL)
		goto cleanup;

	g_assert(dh->hits_queued >= pmi->hits);

	dh->hits_queued -= pmi->hits;

	if (pmsg_was_sent(mb))
		dh->hits_sent += pmi->hits;

cleanup:
	wfree(pmi, sizeof(*pmi));
}

/**
 * Route query hits from one node to the other.
 */
void
dh_route(gnutella_node_t *src, gnutella_node_t *dest, gint count)
{
	pmsg_t *mb;
	struct pmsg_info *pmi;
	gchar *muid;
	dqhit_t *dh;
	mqueue_t *mq;

	g_assert(src->header.function == GTA_MSG_SEARCH_RESULTS);

	if (!NODE_IS_WRITABLE(dest))
		return;

	muid = src->header.muid;
	dh = dh_locate(muid);

	if (dh_debug > 19) {
		printf("DH %s got %d hit%s: "
			"msg=%u, hits_recv=%u, hits_sent=%u, hits_queued=%u\n",
			guid_hex_str(muid), count, count == 1 ? "" : "s",
			dh->msg_recv, dh->hits_recv, dh->hits_sent,
			dh->hits_queued);
	}

	g_assert(dh != NULL);		/* Must have called dh_got_results() first! */

	/*
	 * The heart of the "dynamic hit routing" algorithm is here.
	 *
	 * Based on the information we have on the query hits we already
	 * seen or enqueued, determine whether we're going to drop this
	 * message on the floor or forward it.
	 */

	mq = dest->outq;

	g_assert(mq != NULL);

	if (mq_is_swift_controlled(mq)) {
		/*
		 * We're currently severely dropping messages from the queue.
		 * Don't enqueue if we already sent a hit for this query or
		 * have one queued.
		 */

		if (dh->hits_sent || dh->hits_queued) {
			if (dh_debug > 19) printf("DH queue in SWIFT mode, dropping\n");
			goto drop_flow_control;
		}
	}

	if (mq_is_flow_controlled(mq)) {
		/*
		 * Queue is flow-controlled, don't add to its burden if we
		 * already have hits enqueued for this query.
		 */

		if (dh->hits_queued) {
			if (dh_debug > 19) printf("DH queue in FLOWC mode, dropping\n");
			goto drop_flow_control;
		}
	}

	/*
	 * If we sent more than DH_MIN_HITS already, drop this hit
	 * if the queue already has more bytes queued than its high-watermark,
	 * meaning it is in the dangerous zone.
	 */

	if (dh->hits_sent >= DH_MIN_HITS && mq_size(mq) > mq_hiwat(mq)) {
		if (dh_debug > 19) printf("DH queue size > hiwat, dropping\n");
		goto drop_flow_control;
	}

	/*
	 * If we sent more then DH_POPULAR_HITS already, drop this hit
	 * if the queue has more bytes than its low-watermark, meaning
	 * it is in the warning zone.
	 */

	if (dh->hits_sent >= DH_POPULAR_HITS && mq_size(mq) > mq_lowat(mq)) {
		if (dh_debug > 19) printf("DH queue size > lowat, dropping\n");
		goto drop_flow_control;
	}

	/*
	 * If we sent more than DH_MIN_HITS and we saw more than DH_POPULAR_HITS
	 * and we have the difference in the queue, don't add more and throttle.
	 */

	if (
		dh->hits_sent >= DH_MIN_HITS &&
		dh->hits_recv >= DH_POPULAR_HITS &&
		dh->hits_queued >= (DH_POPULAR_HITS - DH_MIN_HITS)
	) {
		if (dh_debug > 19) printf("DH enough hits queued, throttling\n");
		goto drop_throttle;
	}

	/*
	 * If we successfully sent more than DH_POPULAR_HITS, drop the current
	 * hit if we have already something in the queue.
	 */

	if (dh->hits_sent >= DH_POPULAR_HITS && dh->hits_queued >= DH_MIN_HITS) {
		if (dh_debug > 19) printf("DH popular hits, more queued, throttling\n");
		goto drop_throttle;
	}

	/*
	 * Finally, if what we have sent makes up for more than DH_MAX_HITS and
	 * we have anything queued for that query, drop.
	 */

	if (dh->hits_sent >= DH_MAX_HITS && dh->hits_queued) {
		if (dh_debug > 19) printf("DH max sendable hits reached, throttling\n");
		goto drop_throttle;
	}

	/*
	 * Allow message through.
	 */

	pmi = walloc(sizeof(*pmi));
	pmi->hits = count;

	/*
	 * Magic: we create an extended version of a pmsg_t that contains a
	 * free routine, which will be invoked when the message queue frees
	 * the message.
	 *
	 * This enables us to track how much results we already queued/sent.
	 */

	mb = gmsg_split_to_pmsg_extend(
		(guchar *) &src->header, src->data,
		src->size + sizeof(struct gnutella_header),
		dh_pmsg_free, pmi);

	dh->hits_queued += count;
	mq_putq(mq, mb);

	if (dh_debug > 19)
		printf("DH enqueued %d hit%s for %s\n",
			count, count == 1 ? "" : "s", guid_hex_str(muid));

	return;

drop_flow_control:
	src->rx_dropped++;
	gnet_stats_count_dropped(src, MSG_DROP_FLOW_CONTROL);
	return;
	
drop_throttle:
	src->rx_dropped++;
	gnet_stats_count_dropped(src, MSG_DROP_THROTTLE);
	return;
}

/**
 * Initialize dynamic hits.
 */
void
dh_init(void)
{
	extern guint guid_hash(gconstpointer key);
	extern gint guid_eq(gconstpointer a, gconstpointer b);

	by_muid = g_hash_table_new(guid_hash, guid_eq);
	by_muid_old = g_hash_table_new(guid_hash, guid_eq);
	last_rotation = time(NULL);
}

/**
 * Cleanup data structures used by dynamic querying.
 */
void
dh_close(void)
{
	dh_table_free(by_muid);
	dh_table_free(by_muid_old);
}

