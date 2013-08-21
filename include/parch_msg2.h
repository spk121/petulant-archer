/*
 * File:   parch_msg2.h
 * Author: mike
 *
 * Created on August 18, 2013, 7:44 AM
 */

/* Extra functions that aren't yet generated by the codec script.
   These should be moved into codec_c.gsl at some point.  */

#ifndef PARCH_MSG2_H
#define	PARCH_MSG2_H

#ifdef	__cplusplus
extern "C" {
#endif


parch_msg_t *
parch_msg_new_clear_request_msg (zframe_t *address, byte cause, byte diagnostic);
parch_msg_t *
parch_msg_new_reset_request_msg (zframe_t *address, byte cause, byte diagnostic);
parch_msg_t *
parch_msg_new_disconnect_indication_msg (zframe_t *address, byte cause, byte diagnostic);
bool
parch_msg_validate_connect_request(parch_msg_t *self, diagnostic_t *diag);
void
parch_msg_apply_defaults_to_connect_request (parch_msg_t *self);
void
parch_msg_swap_incoming_and_outgoing(parch_msg_t *self);
int
parch_msg_id_is_valid(parch_msg_t *self);

#ifdef	__cplusplus
}
#endif

#endif	/* PARCH_MSG2_H */

