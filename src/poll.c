/*
  poll.c - event loop

  Copyright 2013, 2014 Michael L. Gran <spk121@yahoo.com>

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
#include <signal.h>

#include "poll.h"

#include "worker.h"
#include "workers_table.h"
#include "channel.h"
#include "channels_table.h"
#include "msg.h"
#include "lib.h"

#include "joza_msg.h"

#define POLL_WORKER_PING_TIMER (10000)  /* milliseconds */
#define POLL_WORKER_REMOVAL_TIMEOUT (30000)  /* milliseconds */

// This flag controls the termination of the loop
volatile sig_atomic_t shutdown_now = 0;

static lcn_t _lcn = LCN_MIN;

static int s_recv_(zloop_t *loop, zmq_pollitem_t *item, void *data);
static int s_process_(joza_msg_t *msg, void *sock,
                      workers_table_t *workers_table,
                      channels_table_t *channels_table);
static void s_process_msg_from_known_worker(worker_t *worker,
        joza_msg_t *msg, void *sock, gint key,
        workers_table_t *workers_table,
        channels_table_t *channels_table);
static void s_process_msg_from_connected_worker(worker_t *worker,
        joza_msg_t *msg, void *sock,
        workers_table_t *workers_table,
        channels_table_t *channels_table);
static void s_process_msg_from_unconnected_worker(worker_t *worker,
        joza_msg_t *msg, void *sock,
        gint key,
        workers_table_t *workers_table,
        channels_table_t *channels_table);
static void s_process_msg_from_unknown_worker(joza_msg_t *msg, void *sock,
        gint key,
        workers_table_t *workers_table);
static void
s_process_call_request_from_unconnected_worker(worker_t *worker,
        joza_msg_t *msg, void *sock,
        workers_table_t *workers_table,
        channels_table_t *channels_table);
static int s_ping_(zloop_t *loop, zmq_pollitem_t *item, void *arg);
static void s_disconnect_idle_channel_(void *sock, worker_t *worker,
                                       workers_table_t *workers_table,
                                       channels_table_t *channels_table);

// This signal handler shuts down the main loop
static void
catch_signal (int sig)
{
    shutdown_now = 1;
    signal (sig, catch_signal);
}

static void
s_process_directory_request_from_unconnected_worker(void *sock,
        const zframe_t *zaddr,
        workers_table_t *workers_table)
{
    // Fill a hash table with the current directory information
    zhash_t *dir = workers_table_create_directory_zhash(workers_table);
    directory_request(sock, zaddr, dir);
    zhash_destroy(&dir);
}


joza_poll_t *poll_create(gboolean verbose, const char *endpoint)
{
    g_debug("In %s(verbose = %d, endpoint = %s)",
            __FUNCTION__, verbose, endpoint);

    // Initialize the several structures that make a broker
    zctx_t *context = zctx_new_or_die();

    void *socket = zsocket_new_or_die(context, ZMQ_ROUTER);
    zsocket_bind(socket, endpoint);

    zloop_t *loop = zloop_new_or_die();
    zloop_set_verbose(loop, verbose);

    joza_poll_t *poll = g_new0(joza_poll_t, 1);
    poll->ctx = context;
    poll->sock = socket;
    poll->loop = loop;
    poll->timer = 0;
    poll->pollitem.socket = socket;
    poll->pollitem.fd = 0;
    poll->pollitem.events = ZMQ_POLLIN;
    poll->pollitem.revents = 0;
    poll->channels_table = channels_table_create();
    poll->workers_table = workers_table_create();

    // Here we kick off the event-driven loop, where
    // the s_recv_ callback will handle each socket event.
    int rc = zloop_poller(poll->loop, &(poll->pollitem), s_recv_, poll);
    if (rc == -1) {
        g_error("failed to start a new ZeroMQ main loop poller");
        exit(1);
    }

    // And here we kick off the timer-driven loop, where
    // the s_ping_ callback will be called at regular intervals.
    int timer = zloop_timer(loop, POLL_WORKER_PING_TIMER, 0, s_ping_,
                            poll);
    if (timer == -1) {
        g_error("failed to start a new ZeroMQ main loop timer");
        exit(1);
    }

    poll->timer = timer;
    g_message("constructed a poll loop on socket %s", endpoint);
    return poll;
}

// This is the main callback that gets called whenever a
// message is received from the ZeroMQ loop.
static int s_recv_(zloop_t *loop, zmq_pollitem_t *item, void *data)
{
    joza_poll_t *poll = data;
    joza_msg_t *msg = NULL;
    int ret = 0;

    g_debug("In %s(%p,%p,%p)", __FUNCTION__, loop, item, poll);

    if (item->revents & ZMQ_POLLIN) {
        msg = joza_msg_recv(poll->sock);
        if (msg != NULL) {
            // Process the message
            ret = s_process_(msg, poll->sock, poll->workers_table,
                             poll->channels_table);
            joza_msg_destroy(&msg);
        }
    }

    g_debug("%s(%p,%p,%p) returns %d", __FUNCTION__, loop, item, poll, ret);
    return ret;
}

// This is entry point for message processing.  Every message
// being processed start here.
static int s_process_(joza_msg_t *msg, void *sock,
                      workers_table_t *workers_table,
                      channels_table_t *channels_table)
{
    int ret = 0;
    diag_t diag;

    g_debug("In %s(%p)", __FUNCTION__, msg);
    // channels_table_dump(channels_table);

    if ((diag = prevalidate_message(msg)) != d_unspecified) {
        // If the message fails basic sanity checks, send a diagnostic
        // back to the sender.
        diagnostic(sock, joza_msg_const_address(msg), NULL,
                   c_malformed_message, diag);
    } else {
        gint key;
        worker_t *worker;

        key = msg_addr2key(joza_msg_address(msg));
        worker = workers_table_lookup_by_key(workers_table, key);

        if (worker != NULL)
            s_process_msg_from_known_worker(worker, msg, sock, key,
                                            workers_table, channels_table);
        else
            s_process_msg_from_unknown_worker (msg, sock, key, workers_table);
    }

    g_debug("%s(%p) returns %d", __FUNCTION__, (void *)msg, ret);
    return ret;
}

static void s_process_msg_from_known_worker(worker_t *worker,
        joza_msg_t *msg, void *sock,
        gint key,
        workers_table_t *workers_table,
        channels_table_t *channels_table)
{
    worker_update_atime(worker);

    switch (worker_get_role(worker)) {
    case X_CALLER:
    case Y_CALLEE:
        s_process_msg_from_connected_worker(worker,
                                            msg, sock, workers_table,
                                            channels_table);
        break;

    case _READY:
        s_process_msg_from_unconnected_worker(worker,
                                              msg, sock,
                                              key,
                                              workers_table, channels_table);
        break;
    default:
        g_assert_not_reached ();
        break;
    }
}

static void s_remove_channel (worker_t *worker, worker_t *other, channels_table_t *channels_table)
{
    channels_table_remove_by_lcn(channels_table,
                                 worker_get_lcn(worker));
    worker_set_role_to_ready(worker);
    worker_set_role_to_ready(other);
}

static void
s_process_msg_from_connected_worker(worker_t *worker,
                                    joza_msg_t *msg, void *sock,
                                    workers_table_t *workers_table,
                                    channels_table_t *channels_table)
{
    channel_t *channel;
    worker_t *other;

    // If this worker is connected and part of a virtual call,
    // the call's state machine processes the message.
    channel = channels_table_lookup_by_lcn(channels_table,
                                           worker_get_lcn(worker));
    other = workers_table_lookup_other(workers_table, worker);

    g_return_val_if_fail(channel != NULL, 0);
    g_return_val_if_fail(other != NULL, 0);

    if (worker_is_x_caller(worker))
        channel_dispatch(channel, sock, msg, 0);
    else
        channel_dispatch(channel, sock, msg, 1);

    if (channel_is_closed(channel)) {
        // This channel is no longer connected; remove it.
        g_message("removing channel %s/%s because it is closed",
                  channel->xname, channel->yname);
        s_remove_channel (channel, other, channels_table);
    }
}

static void s_process_msg_from_unconnected_worker(worker_t *worker,
        joza_msg_t *msg, void *sock,
        gint key,
        workers_table_t *workers_table,
        channels_table_t *channels_table)
{
    // If this worker is known, but, not part of a call, we handle it
    // here because it involves the whole channel store.
    if ((joza_msg_const_id(msg) == JOZA_MSG_CALL_REQUEST) && !shutdown_now) {
        s_process_call_request_from_unconnected_worker(worker, msg, sock,
                workers_table,
                channels_table);
    } else if ((joza_msg_const_id(msg) == JOZA_MSG_DIRECTORY_REQUEST)
               && !shutdown_now) {
        // send directory request back to worker
        s_process_directory_request_from_unconnected_worker(sock,
                joza_msg_const_address(msg),
                workers_table);
    } else if (joza_msg_const_id(msg) == JOZA_MSG_DISCONNECT) {
        // remove the worker
        workers_table_remove_by_key(workers_table, key);
    }
}

static void
s_process_call_request_from_unconnected_worker(worker_t *worker,
        joza_msg_t *msg, void *sock,
        workers_table_t *workers_table,
        channels_table_t *channels_table)
{
    if (channels_table_is_full(channels_table))
        diagnostic(sock, joza_msg_const_address(msg), NULL,
                   c_network_congestion, d_no_channels_available);
    else if (!worker_is_allowed_outgoing_call (worker)) {
        diagnostic (sock, joza_msg_const_address(msg), NULL,
                    c_access_barred, d_output_barred);
    } else {
        worker_t *other;

        other = workers_table_lookup_by_address(workers_table,
                                                joza_msg_called_address(msg));
        if (other == NULL) {
            diagnostic (sock, joza_msg_const_address(msg), NULL,
                        c_unknown_address, d_unknown_worker_address);
        } else if (!worker_is_allowed_incoming_call (other)) {
            diagnostic(sock, joza_msg_const_address(msg), NULL,
                       c_access_barred, d_input_barred);
        } else if (!worker_is_available_for_call (worker)) {
            diagnostic(sock, joza_msg_const_address(msg), NULL,
                       c_number_busy, d_number_busy);
        } else {
            // Find an unused logical channel number (aka hash
            // table key) for the new channel.
            _lcn = channels_table_find_free_lcn(channels_table,
                                                _lcn);
            // Make a new channel and stuff it in the hash
            // table.
            worker_set_role_to_x_caller(worker, _lcn);
            worker_set_role_to_y_callee(other, _lcn);
            channels_table_add_new_channel (channels_table,
                                            _lcn,
                                            worker_get_zaddr(worker),
                                            worker_get_address(worker),
                                            worker_get_zaddr(other),
                                            worker_get_address(other),
                                            joza_msg_const_packet(msg),
                                            joza_msg_const_window(msg),
                                            joza_msg_const_throughput(msg));
            joza_msg_send_addr_call_request (sock,
                                             worker_get_zaddr(other),
                                             joza_msg_calling_address(msg),
                                             joza_msg_called_address(msg),
                                             joza_msg_packet(msg),
                                             joza_msg_window(msg),
                                             joza_msg_throughput(msg),
                                             joza_msg_data(msg));
            // FIXME: Set time-limit T11, which is the maximum time this
            // channel will wait for a CALL_ACCEPTED or CLEAR_REQUST,
            // before it considers it a procedure error and clears the
            // virtual call.

            // NOTE: There is also time-limit T21 which is the maximum
            // time that the channel has to send out the CALL_ACCEPTED
            // back to peer X.
        }
    }
}

static void s_process_msg_from_unknown_worker(joza_msg_t *msg, void *sock,
        gint key,
        workers_table_t *workers_table)
{

    // If this worker is heretofore unknown, we handle it here.  The
    // only message we handle from unconnected workers are connection
    // requests.
    if (joza_msg_id(msg) == JOZA_MSG_CONNECT) {
        g_message("handling %s from unconnected worker %s",
                  joza_msg_const_command(msg),
                  joza_msg_const_calling_address(msg));
        if (workers_table_is_full(workers_table)) {
            diagnostic(sock, joza_msg_const_address(msg), NULL,
                       c_network_congestion, d_no_connections_available);
            g_warning("cannot add new worker. No free worker slots");
        } else if (workers_table_lookup_by_address (workers_table, joza_msg_calling_address (msg)) != NULL) {
            diagnostic(sock, joza_msg_const_address(msg), NULL,
                       c_address_in_use, d_address_in_use);
        } else {
            worker_t *new_worker;
            new_worker = workers_table_add_new_worker(workers_table,
                         key,
                         joza_msg_address(msg),
                         joza_msg_calling_address(msg),
                         joza_msg_host_name(msg),
                         (iodir_t) joza_msg_directionality(msg));
            if (new_worker) {
                const zframe_t *addr = joza_msg_const_address(msg);
                joza_msg_send_addr_connect_indication(sock, addr);
            }
        }
    } else {
        if (joza_msg_id(msg) == JOZA_MSG_CALL_REQUEST)
            g_message("ignoring %s from unconnected worker %s to %s",
                      joza_msg_const_command(msg),
                      joza_msg_const_calling_address(msg),
                      joza_msg_const_called_address(msg));
        else
            g_message("ignoring %s (%d) from unconnected worker",
                      joza_msg_const_command(msg), joza_msg_id(msg));
    }
}

static gboolean s_ping_worker_(void *packed_key G_GNUC_UNUSED,
                               worker_t *worker,
                               gpointer user_data)
{
    // gint key = GPOINTER_TO_INT(packed_key);
    joza_poll_t *poll = user_data;
    gint64 idle_time = ((g_get_monotonic_time() - worker_get_atime(worker))
                        / 1000);

    g_message("checking worker %s: %" PRId64 " ms since last access",
              worker_get_address(worker), idle_time);
    if (idle_time > POLL_WORKER_REMOVAL_TIMEOUT || shutdown_now == 1) {
        if (worker_get_role(worker) == _READY) {
            g_message("removing worker %s: %" PRId64 " ms since last access",
                      worker_get_address(worker),
                      idle_time);
            // delete worker
            return TRUE;
        } else {
            // send a disconnect to the channel on behalf of the worker.
            g_message("disconnecting worker %s: %" PRId64 " ms since last access",
                      worker_get_address(worker),
                      idle_time);
            s_disconnect_idle_channel_(poll->sock, worker,
                                       poll->workers_table,
                                       poll->channels_table);
        }
    } else if (idle_time > POLL_WORKER_PING_TIMER) {
        g_message("pinging worker %s", worker_get_address(worker));
        joza_msg_send_addr_enq(poll->sock, worker_get_zaddr(worker));
    }

    // Don't delete worker
    return FALSE;
}

static void s_disconnect_idle_channel_(void *sock, worker_t *worker,
                                       workers_table_t *workers_table,
                                       channels_table_t *channels_table)
{
    channel_t *channel;
    joza_msg_t *disconnect_msg;

    // send a disconnect to the channel on behalf of the worker.
    channel = channels_table_lookup_by_lcn(channels_table,
                                           worker_get_lcn(worker));
    // create disconnect message
    disconnect_msg = joza_msg_new(JOZA_MSG_DISCONNECT);
    if (worker_is_x_caller(worker))
        channel_dispatch(channel, sock, disconnect_msg, 0);
    else
        channel_dispatch(channel, sock, disconnect_msg, 1);

    if (channel_is_closed(channel)) {
        worker_t *other;
        other = workers_table_lookup_other(workers_table, worker);

        s_remove_channel (channel, other, channels_table);

        // reset access timer for worker
        worker_update_atime(worker);
        worker_update_atime(other);
    }
    // destroy message
    joza_msg_destroy(&disconnect_msg);
}


static int s_ping_(zloop_t *loop G_GNUC_UNUSED,
                   zmq_pollitem_t *item G_GNUC_UNUSED, void *arg)
{
    joza_poll_t *poll = arg;

    // workers_table_dump(poll->workers_table);
    // channels_table_dump(poll->channels_table);

    // Ping workers that haven't been accessed for a while.

    //workers_table_foreach(poll->workers_table, s_ping_worker_, poll);
    g_hash_table_foreach_remove(poll->workers_table, s_ping_worker_, poll);

    // Close channels whose workers haven't been accessed in a while.

    // channels_table_closed_unused(poll->channels_table);
    // workers_table_remove_unused(poll->workers_table);

    if (shutdown_now) {
        if (workers_table_is_empty(poll->workers_table)) {
            poll_destroy(poll);
            exit (1);
        }
    }
    return 0;
}

void poll_start(zloop_t *loop)
{
    // Takes over control of the thread.
    g_debug("In %s()", __FUNCTION__);

    g_message("starting main loop");
    signal(SIGHUP, catch_signal);
    zloop_start(loop);
}

void poll_destroy(joza_poll_t *poll)
{
    channels_table_destroy(&(poll->channels_table));
    workers_table_destroy(&(poll->workers_table));
    zloop_destroy(&(poll->loop));
    zsocket_destroy(poll->ctx, poll->sock);
    zctx_destroy(&(poll->ctx));
    g_free(poll);
}
