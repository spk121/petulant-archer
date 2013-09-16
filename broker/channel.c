#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include "../libjoza/joza_msg.h"
#include "action.h"
#include "cause.h"
#include "channel.h"
#include "diag.h"
#include "lib.h"
#include "poll.h"
#include "state.h"
#include "tput.h"
#include "worker.h"
#include "packet.h"
#include "log.h"
#include "msg.h"
#include <czmq.h>
// #include "../libjoza/joza_lib.h"

static_assert(ACTION_STATE_COUNT == state_last + 1, "Number of state different than action table");
static_assert(ACTION_MESSAGE_COUNT == JOZA_MSG_COUNT, "Number of messages different than action table");
static_assert(sizeof(seq_t) <= offsetof(joza_msg_t,ps) - offsetof(joza_msg_t,pr), "Sequence type too large");

/*
lcn    ukey_t       [primary key]   the unique id for this connection
xzaddr zframe_t *                  a ZMQ frame containing a ZMQ Router identity for X caller
yzaddr zframe_t *                  a ZMQ frame containing a ZMQ Router identity for Y callee
state  state_t                     call status
xps    seq_t                       ID of next packet to be sent by X
xpr    seq_t                       Smallest packet ID X will accept from Y
yps    seq_t                       ID of next packet to be sent by Y
ypr    seq_t                       Smallest packet ID Y will accept from X
window seq_t                       delta between the smallest and largest acceptable ID
tput   tput_t                      throughput allowed on this channel
tok   double                       store for x throughput "leaky bucket" tokens
ctime double                       time this channel was created
mtime double                       time of last message from either peer
*/                                                                


static size_t _count = 0;
static ukey_t _lcn = UKEY_MIN;
ukey_t c_lcn[CHANNEL_COUNT];
void *c_xzaddr[CHANNEL_COUNT]; /* ZMQ address of caller X */
void *c_yzaddr[CHANNEL_COUNT]; /* ZMQ address of callee Y */
size_t c_yidx[CHANNEL_COUNT]; /* index array that sorts ykey array */
char *c_xname[CHANNEL_COUNT];
char *c_yname[CHANNEL_COUNT];
state_t c_state[CHANNEL_COUNT];
static seq_t c_xps[CHANNEL_COUNT]; /* sequence number of packets sent by X */
static seq_t c_xpr[CHANNEL_COUNT]; /* lowest packet sequence permitted to send to X */
static seq_t c_yps[CHANNEL_COUNT]; /* sequence number of packets sent by Y  */
static seq_t c_ypr[CHANNEL_COUNT]; /* lowest packet sequence permitted to send to Y */
seq_t c_window[CHANNEL_COUNT];
packet_t c_pkt[CHANNEL_COUNT];
tput_t c_tput[CHANNEL_COUNT]; /* bits/sec permitted on this channel */
static double c_ctime[CHANNEL_COUNT]; /* time this channel was activated */
static double c_mtime[CHANNEL_COUNT]; /* timestamp of last messate dispatched */

#define C_ADDR(idx,y) ((y)?c_yzaddr[(idx)]:c_xzaddr[(idx)])
#define PUSH(arr,idx,count)                                             \
    memmove(arr + idx + 1, arr + idx, sizeof(arr[0]) * (count - idx))
#define STATE2DIAG(s) ((diag_t)((s) - state_ready + d_invalid_message_for_state_ready))

bool_t channel_available()
{
    if (_count < CHANNEL_COUNT)
        return TRUE;
    return FALSE;
}

ukey_t add_channel(zframe_t *xzaddr, const char *xname, zframe_t *yzaddr, const char *yname)
{
    assert(_count < CHANNEL_COUNT);
    assert(xzaddr);
    assert(yzaddr);

    size_t idx;

    idx = keyfind(c_lcn, _count, _lcn);
    assert (c_lcn[idx] != _lcn);

    if (idx < _count)
    {
        PUSH(c_xzaddr, idx, _count);
        PUSH(c_yzaddr, idx, _count);
        PUSH(c_xname, idx, _count);
        PUSH(c_yname, idx, _count);
        PUSH(c_state, idx, _count);
        PUSH(c_xps, idx, _count);
        PUSH(c_yps, idx, _count);
        PUSH(c_xpr, idx, _count);
        PUSH(c_ypr, idx, _count);
        PUSH(c_window, idx, _count);
        PUSH(c_tput, idx, _count);
    }
    c_xzaddr[idx] = xzaddr;
    c_yzaddr[idx] = yzaddr;
    c_xname[idx] = xname;
    c_yname[idx] = yname;
    c_state[idx] = state_ready;
    c_xps[idx] = SEQ_MIN;
    c_xps[idx] = SEQ_MIN;
    c_ypr[idx] = SEQ_MIN;
    c_ypr[idx] = SEQ_MIN;
    c_window[idx] = WINDOW_DEFAULT;
    c_tput[idx] = t_default;

    _count ++;
};    

void reset_flow_by_idx(unsigned int idx)
{
    c_xps[idx] = 0;
    c_xpr[idx] = 0;
    c_yps[idx] = 0;
    c_ypr[idx] = 0;
}


// This punishment action is a result of a message received a worker
// that is incorrect for the current state.
void do_reset(joza_msg_t *M, int I, bool_t me)
{
    int id;
    int state = c_state[I];

    RESET_REQUEST(C_ADDR(I, me), c_local_procedure_error, STATE2DIAG(state));
    RESET_REQUEST(C_ADDR(I, !me), c_remote_procedure_error, STATE2DIAG(state));
    c_state[I] = state_y_reset_request;
}

// This punishement action is a result of a message received from a
// worker that is incorrect for the current state.
void do_clear(joza_msg_t *M, int I, bool_t me)
{
    int id;
    int state = c_state[I];

    CLEAR_REQUEST(C_ADDR(I, me), c_local_procedure_error, STATE2DIAG(state));
    CLEAR_REQUEST(C_ADDR(I, !me), c_remote_procedure_error, STATE2DIAG(state));

    // Unlike CLEARs requested by workers, a broker-initiated CLEAR
    // closes the channel immediately.
    remove_channel_by_idx(I);
}

// Caller is doing a hard stop. I send a CLEAR_REQUEST to callee,
// close the channel immediately, and disconnect caller
void do_i_disconnect(joza_msg_t *M, int I, bool_t me)
{
    uint32_t hash;
    int ret;
    ret = CLEAR_REQUEST(C_ADDR(I, !me), c_worker_originated, d_worker_originated);
    if (ret == -1)
        DIAGNOSTIC(C_ADDR(I, me), c_zmq_sendmsg_err, errno2diag());
    hash = addr2hash(C_ADDR(I,me));
    remove_channel_by_idx(I);
    remove_worker_by_hash(hash);
}


void do_y_call_accepted(joza_msg_t *M, int I)
{
    zframe_t *addr       = joza_msg_address(M);
    char     *xname      = joza_msg_calling_address(M);
    char     *yname      = joza_msg_called_address(M);
    packet_t pkt         = (packet_t) joza_msg_packet(M);
    int      pkt_rcheck  = rngchk_packet(pkt);
    tput_t   tput        = (tput_t) joza_msg_throughput(M);
    int      tput_rcheck = rngchk_tput(tput);
    seq_t    window      = joza_msg_window(M);
    int      window_rcheck = rngchk_window(window);
    uint8_t  *data       = zframe_data(joza_msg_data(M));
    size_t   data_len    = zframe_size(joza_msg_data(M));
    int      me          = 1;
    
    // Validate the message
    if (strnlen(xname, NAME_LEN + 1) == 0)
        DIAGNOSTIC(C_ADDR(I, !me), c_malformed_message, d_calling_address_too_short);
    else if (strnlen(xname, NAME_LEN + 1) > NAME_LEN)
        DIAGNOSTIC(C_ADDR(I, !me), c_malformed_message, d_calling_address_too_long);
    else if (!safeascii(xname, NAME_LEN))
        DIAGNOSTIC(C_ADDR(I, !me), c_malformed_message, d_calling_address_format_invalid);
    else if (strnlen(yname, NAME_LEN + 1) == 0)
        DIAGNOSTIC(C_ADDR(I, !me), c_malformed_message, d_calling_address_too_short);
    else if (strnlen(yname, NAME_LEN + 1) > NAME_LEN)
        DIAGNOSTIC(C_ADDR(I, !me), c_malformed_message, d_calling_address_too_long);
    else if (!safeascii(yname, NAME_LEN))
        DIAGNOSTIC(C_ADDR(I, !me), c_malformed_message, d_calling_address_format_invalid);
    else if (pkt_rcheck < 0)
        DIAGNOSTIC(C_ADDR(I, !me), c_malformed_message, d_packet_facility_too_small);
    else if (pkt_rcheck > 0)
        DIAGNOSTIC(C_ADDR(I, !me), c_malformed_message, d_packet_facility_too_large);
    else if (tput_rcheck < 0)
        DIAGNOSTIC(C_ADDR(I, !me), c_malformed_message, d_throughput_facility_too_small);
    else if (tput_rcheck > 0)
        DIAGNOSTIC(C_ADDR(I, !me), c_malformed_message, d_throughput_facility_too_large);
    else if (window_rcheck < 0)
        DIAGNOSTIC(C_ADDR(I, !me), c_malformed_message, d_window_facility_too_small);
    else if (window_rcheck > 0)
        DIAGNOSTIC(C_ADDR(I, !me), c_malformed_message, d_window_facility_too_large);
    else if (data_len > CALL_REQUEST_DATA_LEN)
        DIAGNOSTIC(C_ADDR(I, !me), c_malformed_message, d_data_too_long);
    
    // If the caller has modified this channel's facilities,
    // ensure that the caller has obey the negotiation rules
    else if (!packet_negotiate(pkt, c_pkt[I]))
        DIAGNOSTIC(C_ADDR(I, !me), c_invalid_facility_request, d_invalid_packet_facility_negotiation);
    else if (!window_negotiate(window, c_window[I]))
        DIAGNOSTIC(C_ADDR(I, !me), c_invalid_facility_request, d_invalid_window_facility_negotiation);
    else if (!tput_negotiate(tput, c_tput[I]))
        DIAGNOSTIC(C_ADDR(I, !me), c_invalid_facility_request, d_invalid_throughput_facility_negotiation);
    
    // TODO: Validate that the addresses still match.
    // If the callee's address has changed, this worker is requesting that the broker forward
    // this message to another worker for processing.  If the caller's address has changed, well, 
    // I don't know what that means, yet.
    else if(strcmp(xname, c_xname[I]) != 0) 
        DIAGNOSTIC(C_ADDR(I, !me), c_invalid_forwarding_request, d_caller_forwarding_not_allowed);
    else if(strcmp(xname, c_yname[I]) != 0) 
        DIAGNOSTIC(C_ADDR(I, !me), c_invalid_forwarding_request, d_callee_forwarding_not_allowed);

    else {

        // Update the channel
        c_pkt[I] = pkt;
        c_window[I] = window;
        c_tput[I] = tput;

        c_state[I] = state_data_transfer;
        
        joza_msg_send_addr_call_accepted (g_poll_sock, C_ADDR(I, !me), xname, yname, pkt, window, tput, 
                                     zframe_dup(joza_msg_data(M)));
    }
 }

// CALL COLLISION -- To get here, X has sent a call request, and the
// broker has joined X and Y in a call.  X is waiting for a
// CALL_ACCEPTED, but, Y has sent a CALL_REQUEST instead, probably
// because X and Y tried to join at the same time.
//
// To resolve this, broker informs Y of the call collision.  Y is
// supposed to then send a CALL_ACCEPTED.
void do_y_call_collision(joza_msg_t *M, int I)
{
    int me = 1;
    DIAGNOSTIC(C_ADDR(I, me), c_call_collision, d_call_collision);
    c_state[I] = state_call_collision;
}

// Caller is closing down gracefully
void do_i_clear_request(joza_msg_t *M, int I, bool_t me)
{
    cause_t c;
    diag_t d;
    // The caller's clear request shall only use the diagnostic D_WORKER_REQUESTED.
    if (joza_msg_cause(M) != c_worker_originated)
        DIAGNOSTIC(C_ADDR(I, me), c_malformed_message, d_invalid_cause);
    else if (joza_msg_diagnostic(M) != d_worker_originated)
        DIAGNOSTIC(C_ADDR(I, me), c_malformed_message, d_invalid_diagnostic);
    else {
        CLEAR_REQUEST(C_ADDR(I, !me), c_worker_originated, d_worker_originated);
        c_state[I] = STATE_CLEAR_REQUEST(me);
    }
}

// Caller is responding to a peer's request to close the channel
void do_i_clear_confirmation(joza_msg_t *M, int I, bool_t me)
{
    joza_msg_send_addr_clear_confirmation (g_poll_sock, C_ADDR(I, !me));
    c_state[I] = state_ready;
    reset_flow_by_idx(I);
    remove_channel_by_idx(I);
}

// To do this comparison, we use double-wide integer types to avoid worrying about
// numerical overflow.
bool_t flow_sequence_in_range(seq_t x, seq_t lo, seq_t hi)
{
    if (hi < lo) {
        if (x <= hi || x >= lo)
            return TRUE;
    }
    else if (hi > lo) {
        if (x >= lo && x <= hi)
            return TRUE;
    }
    return FALSE;
}

void do_i_data(joza_msg_t *M, int I, bool_t me)
{
    seq_t pr = joza_msg_pr(M);
    seq_t ps = joza_msg_ps(M);
    uint8_t  *data       = zframe_data(joza_msg_data(M));
    size_t   data_len    = zframe_size(joza_msg_data(M));

    // First, check if the message is valid
    if (ps >= SEQ_MAX)
        DIAGNOSTIC(C_ADDR(I, me), c_malformed_message, d_ps_too_large);
    else if (pr >= SEQ_MAX)
        DIAGNOSTIC(C_ADDR(I, me), c_malformed_message, d_pr_too_large);
    else if (data_len == 0)
        DIAGNOSTIC(C_ADDR(I, me), c_malformed_message, d_data_too_short);
    else if (data_len > packet_bytes(p_last))
        DIAGNOSTIC(C_ADDR(I, me), c_malformed_message, d_data_too_long);
    else if (data_len > packet_bytes(c_pkt[I]))
        DIAGNOSTIC(C_ADDR(I, me), c_local_procedure_error, d_data_too_long_for_packet_facility);

    // When caller sends a message, its packet number should match my
    // expected packet number for this caller, and should be in the
    // window of packet numbers that callee has said it will accept.
    if (ps != C_PS(I, me))
        DIAGNOSTIC(C_ADDR(I, me), c_local_procedure_error, d_ps_out_of_order);
    else if (!flow_sequence_in_range(ps, C_PR(I, !me), ((dseq_t)C_PR(I, !me) + (dseq_t)c_window[I]) % SEQ_MAX))
        DIAGNOSTIC(C_ADDR(I, me), c_local_procedure_error, d_ps_not_in_window);
    // When X updates its own window of packets that it will accept,
    // its new lowest packet number that X will allow should be
    // between the previous lowest packet number that X would allow
    // and the next packet number that will be sent by Y.
    else if (!flow_sequence_in_range(pr, C_PR(I, me), C_PS(I, !me)))
        DIAGNOSTIC(C_ADDR(I, me), c_local_procedure_error, d_pr_invalid_window_update);
    else {
        joza_msg_send_addr_data (g_poll_sock, C_ADDR(I, !me), joza_msg_q(M), pr, ps, data);
        SET_C_PS(I, me, (C_PS(I, me) + 1) % SEQ_MAX);
        SET_C_PR(I, me, pr);
    }
}


// Caller tells callee that it is updating the range of packet numbers
// it will allow.
void do_i_rr(joza_msg_t *M, int I, bool_t me)
{
    seq_t pr = joza_msg_pr(M);

    if (pr >= SEQ_MAX)
        DIAGNOSTIC(C_ADDR(I, me), c_malformed_message, d_pr_too_large);
    else if (!flow_sequence_in_range(pr, C_PR(I, me), C_PS(I, !me)))
        DIAGNOSTIC(C_ADDR(I, me), c_local_procedure_error, d_pr_invalid_window_update);
    else {
        joza_msg_send_addr_rr (g_poll_sock, C_ADDR(I, !me), pr);
        SET_C_PR(I, me, pr);
    }
}


// Caller tells callee that it is updating the range of packet numbers
// it will allow, and that it should stop sending data as soon as possible.
void do_i_rnr(joza_msg_t *M, int I, bool_t me)
{
    seq_t pr = joza_msg_pr(M);

    // First, check if the message is valid

    if (pr >= SEQ_MAX)
        DIAGNOSTIC(C_ADDR(I, me), c_malformed_message, d_pr_too_large);
    else if (!flow_sequence_in_range(pr, C_PR(I, me), C_PS(I, !me)))
        DIAGNOSTIC(C_ADDR(I, me), c_local_procedure_error, d_pr_invalid_window_update);
    else {
        joza_msg_send_addr_rnr (g_poll_sock, C_ADDR(I, !me), pr);
        SET_C_PR(I, me, pr);
    }
}


// Caller is requesting that callee reset flow control
void do_i_reset(joza_msg_t *M, int I, bool_t me)
{
    diag_t d;

    if (joza_msg_cause(M) != c_worker_originated)
        DIAGNOSTIC(C_ADDR(I, me), c_malformed_message, d_invalid_cause);
    else if (joza_msg_diagnostic(M) != d_worker_originated)
        DIAGNOSTIC(C_ADDR(I, me), c_malformed_message, d_invalid_diagnostic);
    else {
        RESET_REQUEST (C_ADDR(I, !me), c_worker_originated, d_worker_originated);
    }
}

// Caller has confirmed callee's request for a reset.
void do_i_reset_confirmation(joza_msg_t *M, int I, bool_t me)
{
    diag_t d;

    // The worker's reset request shall only use the diagnostic D_WORKER_REQUESTED.
    joza_msg_send_addr_reset_confirmation (g_poll_sock, C_ADDR(I, !me));
    reset_flow_by_idx(I);
    c_state[I] = STATE_FLOW_CONTROL(me);
}



#undef PUSH


/* Handle message send a worker on this connected channel */
void channel_dispatch_by_lcn(joza_msg_t *M, ukey_t LCN, role_t R)
{
    const char *cmdname = NULL, *xname = NULL, *yname = NULL;
    bool_index_t bi;
    size_t I;
    state_t state_orig;
    action_t a;
    
    cmdname = joza_msg_const_address(M);
    I = ifind(c_lcn, _count, LCN);
    assert (c_lcn[I] == LCN);

    c_mtime[I] = now();

    xname = c_xname[I];
    yname = c_yname[I];
    state_orig = c_state[I];

    if (R == X_CALLER)
        INFO("%s/%s handling %s from %s", xname, yname, cmdname, xname);
    else if (R == Y_CALLEE)
        INFO("%s/%s handling %s from %s", xname, yname, cmdname, yname);
    else
        abort();

    /* Big fat dispatch table */
	a = action_get(c_state[I], joza_msg_const_id(M), R == Y_CALLEE ? 1 : 0);
	INFO("%s/%s dispatching %s in %d", xname, yname, action_name(a), state_orig);

	switch (a) {
	case a_unspecified:
	case a_disconnect:
	case a_x_connect:
    case a_x_call_request:
	case a_x_call_accepted:
	case a_y_call_request:
		// None of these should ever happen.  X is always connected
        // before Y. And this channel is already set-up by the time
        // this function is called.
		abort();
		break;

	case a_discard:
		break;

	case a_reset:
        do_reset(M, I, R == Y_CALLEE ? 1 : 0);
		break;
	case a_clear: 
        do_clear(M, I, R == Y_CALLEE ? 1 : 0);
		break;

	case a_x_disconnect:
        do_x_disconnect(M, I);
		break;
	case a_x_clear_request:
        do_x_clear_request(M, I);
		break;
	case a_x_clear_confirmation:
		do_x_clear_confirmation(M, I);
		break;
	case a_x_data:
        do_x_data(M, I);
		break;
	case a_x_rr:
        do_x_rr(M, I);
		break;
	case a_x_rnr:
		do_x_rnr(M, I);
		break;
	case a_x_reset:
		do_x_reset(M, I);
		break;
	case a_x_reset_confirmation:
		do_x_reset_confirmation(M, I);
		break;

	case a_y_disconnect:
		do_y_disconnect(M, I);
		break;
	case a_y_call_accepted:
		do_y_call_accepted(M, I);
		break;
	case a_y_call_collision:
		do_y_call_collision(M, I);
		break;
	case a_y_clear_request:
		do_y_clear_request(M, I);
		break;
	case a_y_clear_confirmation:
		do_y_clear_confirmation(M, I);
		break;
	case a_y_data:
		do_y_data(M, I);
		break;
	case a_y_rr:
		do_y_rr(M, I);
		break;
	case a_y_rnr:
		do_y_rnr(M, I);
		break;
	case a_y_reset:
		do_y_reset(M, I);
		break;
	case a_y_reset_confirmation:
		do_y_reset_confirmation(M, I);
		break;
	}
}
