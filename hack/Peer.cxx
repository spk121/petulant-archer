#include "Msg.hxx"
#include "Peer.hxx"
#include <czmq.h>
#include <map>
#include <algorithm>
#include "../libjoza/joza_lib.h"

using namespace std;
extern void *g_sock;

int Peer::_count = 0;

Peer::Peer(const zframe_t *pzf, string name, IoDir dir)
    : _address(pzf), _name(name), _ioDir(dir), _refcount(0)
{
    _id = _count++;
}

// Create a new entry in the peer store
void PeerStore::insert(const zframe_t *pzf, string name, IoDir dir)
{
    char *key;
    key = zframe_strhex((zframe_t *) pzf);
    pair<string, Peer> v(key, Peer(pzf, name, dir));
    free (key);

    auto ret = _map.insert(v);

    // The check to see if we're re-adding a Peer belongs in dispatch().
    assert(ret.second == true);
};

void PeerStore::disconnect(string key) 
{
    _map.erase(_map.find(key));
}

size_t PeerStore::count(string key)
{
    return _map.count(key);
}

size_t PeerStore::count_by_address(string addr)
{
    //return std::count(_map.begin(), _map.end(), [addr](pair<string, Peer> x){return x.second._name == addr;});
    for (auto it = _map.begin();
         it != _map.end();
         ++it) {
        if (it->second._name == addr)
            return 1;
    }
    return 0;
}

// Tries to handle the given message. 
// return 0 if not handled
//        1 if partially handled
//        2 if handled
int PeerStore::dispatch(Msg& msg)
{
    int ret = 0;
    // Here we are going to handle messages that modify or query the PeerStore.
    // Really this is just CONNECT and DISCONNECT.
    // In the future, it will also include PHONEBOOK.

    if (msg.is_a(JOZA_MSG_CONNECT))
        ret = do_connect(msg);
    else if (msg.is_a(JOZA_MSG_DISCONNECT))
        ret = do_disconnect(msg);
    return ret;
}

int PeerStore::do_connect(Msg& msg)
{
    int ret = 0;

    if (count(msg.key()) != 0) {
        // Connected peer is trying to connect again.
        joza_msg_send_addr_diagnostic (g_sock, msg.address(), c_local_procedure_error, d_unspecified);
        ret = 2;
    }
    else if (_map.size() > MAX_PEER_COUNT) {
        // Too many peers already.  Reject this guy.
        joza_msg_send_addr_diagnostic(g_sock, msg.address(), c_network_congestion, d_unspecified);
        ret = 2;
    }
    else if (!address_validate(msg.calling_address().c_str())) {
        joza_msg_send_addr_diagnostic(g_sock, msg.address(),
                                      c_local_procedure_error,
                                      d_invalid_calling_address);
        ret = 2;            
    }
    else if (!direction_validate((direction_t) msg.ioDir())) {
        joza_msg_send_addr_diagnostic(g_sock, msg.address(),
                                      c_invalid_facility_request,
                                      d_facility_parameter_not_allowed);
        ret = 2;
    }
    else if (count_by_address(msg.calling_address()) != 0) {
        // Duplicate address
        joza_msg_send_addr_diagnostic(g_sock, msg.address(),
                                      c_local_procedure_error,
                                      d_invalid_calling_address);
        ret = 2;            
    }
    else {
        // Message from a new peer
        insert (msg.address(), msg.calling_address(), msg.ioDir());
        joza_msg_send_addr_connect_indication(g_sock, msg.address());
        ret = 2;
    }
    return ret;
}

int PeerStore::do_disconnect(Msg& msg)
{
    int ret = 0;
    if (count(msg.key()) == 0) {
        // Unknown peer is trying to disconnect
        joza_msg_send_addr_diagnostic (g_sock, msg.address(), c_local_procedure_error, d_unspecified);
        ret = 2;
    }
    else {
        disconnect(msg.key());
        joza_msg_send_addr_connect_indication(g_sock, msg.address());
        ret = 2;
    }
    return ret;
}

// Handling a CALL_REQUEST message is special because it requires both
// PeerStore and ChannelStore.  This is the first half of the
// CALL_REQUEST processing.
pair<int, string> PeerStore::do_call_request_step_1(Msg& msg)
{
    pair<int, string> ret;
    size_t X_count = count(msg.key());

    assert(msg.is_a(JOZA_MSG_CALL_REQUEST));
    assert (X_count <= 1);

    if (X_count == 0) {
        // If we don't know who this is from, give up.
        joza_msg_send_addr_diagnostic (g_sock, msg.address(), c_local_procedure_error, d_unspecified);
        ret = {2, ""};
    }
    else {
        Peer& X = get(msg.key());

        // This message from from outside, to be paranoid and check it.

        if (X._name != msg.calling_address()) {
            // Bad calling address
        }
        else if (!packet_validate(msg.packet())) {
        }
        else if (!packet_validate(msg.window())) {
        }
        else if (!packet_validate(msg.throughput())) {
        }
        else if (msg.data_size() > MAX_CALL_REQUEST_DATA_SIZE) {
        }
        else {
            Y_count = count_by_name(msg.called_address());

            assert (Y_count <= 1);
            if (Y_count == 0) {
                // Connect request to unknown peer
            }
            else {
                Peer& Y = get_by_name(msg.called_address());

                if (X.busy()) {
                    // X is already on a call.  At this point I know it will get rejected, but,
                    // I let ChannelStore do it so it can send a "d_packet_type_invalid_for_sX"
                    // message.
                    ret = {1, Y.key()};
                }
                else if (Y.busy()) {
                    // Y is on a call and X is not.  Thus, Y is busy.
                    joza_msg_send_addr_diagnostic (g_sock, msg.address(), c_number_busy, d_call_collision);
                    ret = {2, ""};
                }
                else {
                    // OK finally. This is a valid message on valid free peers.
                    ret = {1, Y.key()};
                }
            }
        }
    }

    // Unreachable
    abort ();
    return {0, ""};
}

#if 0
int main()
{
    char data[6] = "ABCDE";
    zframe_t *pzf = zframe_new(data, 5);
    char *hex = zframe_strhex(pzf);
    Peer p (pzf, "name", 1);

    pair<string, Peer> v(hex,p);
    peers.insert(v);
    free(hex);
                 
    return 0;
}

#endif