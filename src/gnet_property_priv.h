/*
 * Copyright (c) 2001-2002, Richard Eckart
 *
 * THIS FILE IS AUTOGENERATED! DO NOT EDIT!
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

#ifndef __gnet_property_priv_h__
#define __gnet_property_priv_h__

#include <glib.h>

extern gboolean reading_hostfile;
extern gboolean ancient_version;
extern gchar   *new_version_str;
extern guint32  up_connections;
extern guint32  max_connections;
extern guint32  max_downloads;
extern guint32  max_host_downloads;
extern guint32  max_uploads;
extern guint32  max_uploads_ip;
extern guint32  local_ip;
extern guint32  listen_port;
extern guint32  forced_local_ip;
extern guint32  connection_speed;
extern guint32  search_max_items;
extern guint32  ul_usage_min_percentage;
extern guint32  download_connecting_timeout;
extern guint32  download_push_sent_timeout;
extern guint32  download_connected_timeout;
extern guint32  download_retry_timeout_min;
extern guint32  download_retry_timeout_max;
extern guint32  download_max_retries;
extern guint32  download_retry_timeout_delay;
extern guint32  download_retry_busy_delay;
extern guint32  download_retry_refused_delay;
extern guint32  download_retry_stopped_delay;
extern guint32  download_overlap_range;
extern guint32  upload_connecting_timeout;
extern guint32  upload_connected_timeout;
extern guint32  search_reissue_timeout;
extern guint32  ban_ratio_fds;
extern guint32  ban_max_fds;
extern guint32  incoming_connecting_timeout;
extern guint32  node_connecting_timeout;
extern guint32  node_connected_timeout;
extern guint32  node_sendqueue_size;
extern guint32  node_tx_flowc_timeout;
extern guint32  max_ttl;
extern guint32  my_ttl;
extern guint32  hard_ttl_limit;
extern guint32  dbg;
extern gboolean stop_host_get;
extern gboolean bws_in_enabled;
extern gboolean bws_out_enabled;
extern gboolean bws_gin_enabled;
extern gboolean bws_gout_enabled;
extern gboolean bw_ul_usage_enabled;
extern gboolean clear_downloads;
extern gboolean search_remove_downloaded;
extern gboolean force_local_ip;
extern gboolean use_netmasks;
extern gboolean download_delete_aborted;
extern gboolean proxy_connections;
extern gboolean proxy_auth;
extern gchar   *socks_user;
extern gchar   *socks_pass;
extern guint32  proxy_ip;
extern guint32  proxy_port;
extern guint32  proxy_protocol;
extern guint32  max_hosts_cached;
extern guint32  hosts_in_catcher;
extern guint32  max_high_ttl_msg;
extern guint32  max_high_ttl_radius;
extern guint32  bw_http_in;
extern guint32  bw_http_out;
extern guint32  bw_gnet_in;
extern guint32  bw_gnet_out;
extern guint32  search_queries_forward_size;
extern guint32  search_queries_kick_size;
extern guint32  search_answers_forward_size;
extern guint32  search_answers_kick_size;
extern guint32  other_messages_kick_size;
extern guint32  hops_random_factor;
extern gboolean send_pushes;
extern guint32  min_dup_msg;
extern guint32  min_dup_ratio;
extern gchar   *scan_extensions;
extern gchar   *save_file_path;
extern gchar   *move_file_path;
extern gchar   *bad_file_path;
extern gchar   *shared_dirs_paths;
extern gchar   *local_netmasks_string;
extern guint32  total_downloads;
extern guint32  total_uploads;
extern guint8   guid[16];
extern gboolean use_swarming;
extern gboolean use_aggressive_swarming;
extern guint32  dl_minchunksize;
extern guint32  dl_maxchunksize;
extern gboolean auto_download_identical;
extern gboolean strict_sha1_matching;
extern gboolean use_fuzzy_matching;
extern guint32  fuzzy_threshold;
extern gboolean is_firewalled;
extern gboolean is_inet_connected;
extern gboolean gnet_compact_query;
extern gboolean download_optimistic_start;
extern gboolean mark_ignored;
extern gboolean library_rebuilding;
extern gboolean sha1_rebuilding;
extern gboolean sha1_verifying;
extern gboolean prefer_compressed_gnet;
extern gboolean online_mode;


prop_set_t *gnet_prop_init(void);
void gnet_prop_shutdown(void);

#endif /* __gnet_property_priv_h__ */

