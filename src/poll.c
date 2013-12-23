/*
    poll.c - event loop

    Copyright 2013 Michael L. Gran <spk121@yahoo.com>

    This file is part of Jozabad.

    Jozabad is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Jozabad is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Jozabad.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <glib.h>
#include <czmq.h>

#include "poll.h"

#include "worker.h"
#include "workers_table.h"
#include "channel.h"
#include "msg.h"

#include "joza_msg.h"

#define POLL_WORKER_PING_TIMER (10000)  /* milliseconds */


static lcn_t _lcn = LCN_MIN;

// Zero MQ socket
static zmq_pollitem_t poll_input = {NULL, 0, ZMQ_POLLIN, 0};

static int s_recv_(zloop_t *loop, zmq_pollitem_t *item, void *data);
static int s_process_(joza_msg_t *msg, void *sock, workers_table_t *workers_table, GHashTable *channels_table);
static int s_ping_(zloop_t *loop, zmq_pollitem_t *item, void *arg);

static zctx_t *zctx_new_or_die (void)
{
    zctx_t *c = NULL;

    c = zctx_new();
    if (c == NULL) {
        g_print("failed to create a ZeroMQ context");
        exit(1);
    }
    return c;
}

static void *zsocket_new_or_die(zctx_t *ctx, int type)
{
    void *sock;

    g_assert(ctx != NULL);

    sock = zsocket_new(ctx, type);
    if (sock == NULL) {
        g_print("failed to create a new ZeroMQ socket");
        exit(1);
    }
    return sock;
}

static zloop_t *zloop_new_or_die(void)
{
    zloop_t *L = NULL;

    L = zloop_new();
    if (L == NULL) {
        g_error("failed to create a new ZeroMQ main loop");
        exit (1);
    }
    return L;
}

static void bad_free (gpointer data __attribute__ ((unused)))
{
    g_assert_not_reached();
}

static void
add_directory_entry_to_zhash (gpointer key __attribute__ ((unused)),
                              gpointer value,
                              gpointer user_data)
{
    worker_t *worker = value;
    zhash_t *hash = user_data;
    gchar *str = g_strdup_printf("%s,%s",iodir_name(worker->iodir), worker->role == READY ? "AVAILABLE" : "BUSY");
    zhash_insert(hash, worker->name, str);
    g_free(str);
}

static void do_directory_request(void *sock, const zframe_t *zaddr, GHashTable *channels_table)
{
    zhash_t  *dir       = zhash_new();

    // Fill a hash table with the current directory information
    // FIXME: probably could put something useful in the "value" part of the
    // hash table.
    g_hash_table_foreach (channels_table, add_directory_entry_to_zhash, dir);
    directory_request(sock, zaddr, dir);
    zhash_destroy(&dir);
}


joza_poll_t *poll_create(gboolean verbose, const char *endpoint)
{
    g_message("In %s(verbose = %d, endpoint = %s)", __FUNCTION__, verbose, endpoint);

    joza_poll_t *poll = g_new0(joza_poll_t, 1);

    //  Initialize broker state
    poll->ctx = zctx_new_or_die();
    poll->sock = zsocket_new_or_die(poll->ctx, ZMQ_ROUTER);
    g_message("binding ROUTER socket to %s", endpoint);
    zsocket_bind(poll->sock, endpoint);
    poll->loop = zloop_new_or_die();
    zloop_set_verbose(poll->loop, verbose);
    poll_input.socket = poll->sock;
    int rc = zloop_poller(poll->loop, &poll_input, s_recv_, poll);
    if (rc == -1) {
        g_error("failed to start a new ZeroMQ main loop poller");
        exit(1);
    }
    poll->channels_table = g_hash_table_new_full (g_int_hash, g_int_equal, bad_free, bad_free);
    poll->workers_table = workers_table_create();
    zloop_timer(poll->loop, POLL_WORKER_PING_TIMER, 0, s_ping_, poll);
    return poll;
}

// This is the main callback that gets called whenever a
// message is received from the ZeroMQ loop.
static int s_recv_(zloop_t *loop, zmq_pollitem_t *item, void *data)
{
    joza_poll_t *poll = data;
    joza_msg_t *msg = NULL;
    int ret = 0;

    g_message("In %s(%p,%p,%p)", __FUNCTION__, (void *)loop, (void *)item, (void *)poll);

    if (item->revents & ZMQ_POLLIN) {
        msg = joza_msg_recv(poll->sock);
        if (msg != NULL) {
            // Process the valid message
            ret = s_process_(msg, poll->sock, poll->workers_table, poll->channels_table);
            joza_msg_destroy(&msg);
        }
    }

    g_message("%s(%p,%p,%p) returns %d", __FUNCTION__, (void *)loop, (void *)item, (void *)poll, ret);
    return ret;
}

// This is entry point for message processing.  Every message
// being processed start here.
static int s_process_(joza_msg_t *msg, void *sock, workers_table_t *workers_table, GHashTable *channels_table)
{
    gint key;
    int ret = 0;
    diag_t diag;

    g_message("In %s(%p)", __FUNCTION__, (void *) msg);

    ////////////////////////////////////////////////////////////////
    // Validate here all message components
    // that can be validates w/o any specific context
    ////////////////////////////////////////////////////////////////

    if ((diag = prevalidate_message(msg)) != d_unspecified) {
        diagnostic(sock, joza_msg_const_address(msg), NULL, c_malformed_message, diag);
    } else {
        worker_t *worker;
        channel_t *channel;
        key = msg_addr2key(joza_msg_address(msg));
        channel = NULL;
        worker = workers_table_lookup_by_key(workers_table, key);

        if (worker != NULL) {
            worker->atime = g_get_monotonic_time();

            switch (worker->role) {
            case X_CALLER:
            case Y_CALLEE:
                // If this worker is connected and part of a virtual call,
                // the call's state machine processes the message.
                channel = g_hash_table_lookup(channels_table, &(worker->lcn));
                g_return_val_if_fail(channel != NULL, 0);
                if (worker->role == X_CALLER)
                    channel->state = channel_dispatch(channel, sock, msg, 0);
                else
                    channel->state = channel_dispatch(channel, sock, msg, 1);
                if (channel->state == state_ready) {
                    // This channel is no longer connected: remove it
                    g_hash_table_remove(channels_table, &(worker->lcn));
                }
                break;

            case READY:
                // If this worker is connected, but, not part of a call,
                // we handle it here because it involves the whole channel store
                if (joza_msg_const_id(msg) == JOZA_MSG_CALL_REQUEST) {
                    g_assert(g_hash_table_size(channels_table) <= LCN_COUNT);

                    if (g_hash_table_size(channels_table) == LCN_COUNT)
                        // send busy diagnostic
                        ;
                    else {
                        worker_t *other = workers_table_lookup_by_address(workers_table,
                                                                          joza_msg_called_address(msg));
                        if (other == NULL) {
                            // send can't find diagnostic
                        } else {
                            // Find an unused logical channel number (aka hash table key) for the
                            // new channel.
                            while (g_hash_table_lookup(channels_table, &_lcn) != NULL) {
                                _lcn ++;
                                if (_lcn > LCN_MAX)
                                    _lcn = LCN_MIN;
                            }
                            // Make a new channel and stuff it in the hash table
                            worker->role = X_CALLER;
                            worker->lcn = _lcn;
                            other->role = Y_CALLEE;
                            other->lcn = _lcn;
                            channel_t *new_channel = channel_create(_lcn,
                                                                    worker->zaddr,
                                                                    worker->name,
                                                                    other->zaddr,
                                                                    other->name,
                                                                    joza_msg_const_packet(msg),
                                                                    joza_msg_const_window(msg),
                                                                    joza_msg_const_throughput(msg));
                            g_assert(new_channel != NULL);
                            new_channel->state = state_x_call_request;
                            g_hash_table_insert(channels_table, &_lcn, new_channel);
                            joza_msg_send_addr_call_request(sock,
                                                            other->zaddr,
                                                            joza_msg_calling_address(msg),
                                                            joza_msg_called_address(msg),
                                                            joza_msg_packet(msg),
                                                            joza_msg_window(msg),
                                                            joza_msg_throughput(msg),
                                                            joza_msg_data(msg));
                        }
                    }
                } else if (joza_msg_const_id(msg) == JOZA_MSG_DIRECTORY_REQUEST) {
                    // send directory request back to worker
                    do_directory_request(sock, joza_msg_const_address(msg), channels_table);
                } else if (joza_msg_const_id(msg) == JOZA_MSG_DISCONNECT) {
                    // remove the worker
                    workers_table_remove_by_key(workers_table, key);
                }
                break;
            default:
                g_assert_not_reached ();
                break;
            }
        }

        // If this worker is heretofore unknown, we handle it here.  The
        // only message we handle from unconnected workers are connection requests.
        else if (joza_msg_id(msg) == JOZA_MSG_CONNECT) {
            g_message("handling %s from unconnected worker", joza_msg_const_command(msg));
            if (workers_table_is_full(workers_table)) {
                diagnostic(sock,
                           joza_msg_const_address(msg),
                           NULL,
                           c_network_congestion,
                           d_no_connections_available);
                g_warning("cannot add new worker. No free worker slots");
            } else {
                worker_t *new_worker = workers_table_add_new_worker(workers_table,
                                                                    key,
                                                                    joza_msg_address(msg),
                                                                    joza_msg_calling_address(msg),
                                                                    (iodir_t) joza_msg_directionality(msg));
                if (new_worker) {
                    joza_msg_send_addr_connect_indication(sock, joza_msg_const_address(msg));
                }
            }

        } else {
            g_message("ignoring %s from unconnected worker", joza_msg_const_command(msg));
        }
    }

    g_message("%s(%p) returns %d", __FUNCTION__, (void *)msg, ret);
    return ret;
}

static void s_ping_worker_(worker_t *worker, gpointer user_data)
{
    void *sock = user_data;
    g_message("pinging worker %s", worker->name);
    joza_msg_send_addr_enq(sock, worker->zaddr);
}

static int s_ping_(zloop_t *loop G_GNUC_UNUSED, zmq_pollitem_t *item G_GNUC_UNUSED, void *arg)
{
    joza_poll_t *poll = arg;
    workers_table_foreach(poll->workers_table, s_ping_worker_, poll->sock);
    workers_table_remove_unused(poll->workers_table);
    return 0;
}


void poll_start(zloop_t *loop)
{
    // Takes over control of the thread.
    g_message("In %s()", __FUNCTION__);

    g_message("starting main loop");
    zloop_start(loop);
}

void poll_destroy(joza_poll_t *poll)
{
    g_hash_table_destroy(poll->channels_table);
    workers_table_destroy(&(poll->workers_table));
    zloop_destroy(&(poll->loop));
    zsocket_destroy(poll->ctx, poll->sock);
    zctx_destroy(&(poll->ctx));
    g_free(poll);
}

