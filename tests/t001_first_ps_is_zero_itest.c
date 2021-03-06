#include "joza_msg.h"
#include "diag.h"
#include "cause.h"
#include "iodir.h"
#include "packet.h"
#include "tput.h"

#include "initialize.h"
#include "call_setup.h"

int main(int argc, char** argv)
{
    int verbose = getenv("JOZA_VERBOSE_TEST");
    char *broker = "tcp://localhost:5555";
    char *calling_address1 = "ADAM";
    char *calling_address2 = "EVE";
    char *name1 = "Adam Smith";
    char *name2 = "Eve Plum";
    iodir_t dir = io_bidirectional;
    zctx_t *ctx1 = NULL;
    zctx_t *ctx2 = NULL;
    void *sock1 = NULL;
    void *sock2 = NULL;
    tput_t thru = t_2048kbps;
    packet_t packet = p_4_Kbytes;
    uint16_t window = 2;
    int ret;
    char *data = "DATA";

    initialize (verbose, "peer X", &ctx1, &sock1, broker, calling_address1, name1, dir);
    initialize (verbose, "peer Y", &ctx2, &sock2, broker, calling_address2, name2, dir);
    call_setup (verbose, sock1, sock2, calling_address1, calling_address2, packet, window, thru);

    if (verbose)
        printf("peer X: sending data to broker with bad initial PS = 2\n");
    ret = joza_msg_send_data(sock1, 0, 0, 2, zframe_new(data, 4));
    if (ret != 0) {
        if (verbose)
            printf("peer X: joza_msg_send_data (...) returned %d\n", ret);
        exit(1);
    }

    if (verbose)
        printf("peer X: waiting for diagnostic\n");
    joza_msg_t *response = joza_msg_recv(sock1);
    if (joza_msg_id(response) != JOZA_MSG_DIAGNOSTIC) {
        if (verbose) {
            printf("peer X: did not receive diagnostic\n");
            joza_msg_dump(response);
        }
        exit (1);
    } else if(joza_msg_cause(response) != c_local_procedure_error
              || joza_msg_diagnostic(response) != d_ps_out_of_order) {
        if (verbose)
            printf("peer X: did not received correct diagnostic (%s/%s)\n",
                   cause_name(joza_msg_cause(response)),
                   diag_name(joza_msg_diagnostic(response)));
        exit(1);
    }

    if (verbose)
        printf("peer Y: sending data to broker with bad initial PS = 2\n");
    ret = joza_msg_send_data(sock2, 0, 0, 2, zframe_new(data, 4));
    if (ret != 0) {
        if (verbose)
            printf("peer Y: joza_msg_send_data (...) returned %d\n", ret);
        exit(1);
    }

    if (verbose)
        printf("peer Y: waiting for diagnostic\n");
    response = joza_msg_recv(sock2);
    if (joza_msg_id(response) != JOZA_MSG_DIAGNOSTIC) {
        if (verbose) {
            printf("peer Y: did not receive diagnostic\n");
            joza_msg_dump(response);
        }
        exit (1);
    } else if(joza_msg_cause(response) != c_local_procedure_error
              || joza_msg_diagnostic(response) != d_ps_out_of_order) {
        if (verbose)
            printf("peer Y: did not received correct diagnostic (%s/%s)\n",
                   cause_name(joza_msg_cause(response)),
                   diag_name(joza_msg_diagnostic(response)));
        exit(1);
    }

    if (verbose)
        printf("SUCCESS\n");
    return (EXIT_SUCCESS);
}

