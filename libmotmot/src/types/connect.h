/**
 * connect.h - Type representing a Paxos connection.
 */
#ifndef __PAXOS_TYPES_CONNECT_H__
#define __PAXOS_TYPES_CONNECT_H__

#include "paxos_io.h"

#include "containers/hashtable_factory.h"
#include "types/primitives.h"
#include "types/core.h"

/* Connection to another client; shared among sessions. */
struct paxos_connect {
  struct paxos_peer *pc_peer;         // wrapper around I/O channel
  bool pc_pending;                    // pending reconnection?
  pax_str_t pc_id;                    // string identifying the client
};

HASHTABLE_DECLARE(connect);

/* Paxos connection GLib hashtable utilities. */
void connect_hashinit(void);
unsigned connect_key_hash(const void *);
int connect_key_equals(const void *, const void *);

#endif /* __PAXOS_TYPES_CONNECT_H__ */
