/**
 * paxos_helper.c - Helper functions for Paxos.
 */
#include "paxos.h"
#include "paxos_io.h"
#include "list.h"

#include <stdio.h>
#include <glib.h>

///////////////////////////////////////////////////////////////////////////
//
//  Paxos state helpers
//

/**
 * Check if we think we are the proposer.
 */
int
is_proposer()
{
  return pax.proposer != NULL && pax.self_id == pax.proposer->pa_paxid;
}

/**
 * Realias the proposer after an update to the acceptor list.
 */
void
reset_proposer()
{
  struct paxos_acceptor *it;

  LIST_FOREACH(it, &pax.alist, pa_le) {
    if (it->pa_paxid == pax.self_id || it->pa_peer != NULL) {
      pax.proposer = it;
      break;
    }
  }
}

/**
 * Gets the next free instance number.
 */
paxid_t
next_instance()
{
  return LIST_EMPTY(&pax.ilist) ? 1 : LIST_LAST(&pax.ilist)->pi_hdr.ph_inum + 1;
}

/**
 * Convenience function for denoting which dkinds are requests.
 */
int
request_needs_cached(dkind_t dkind)
{
  return (dkind == DEC_CHAT || dkind == DEC_JOIN);
}

/**
 * Compare two paxid's.
 */
int
paxid_compare(paxid_t x, paxid_t y)
{
  if (x < y) {
    return -1;
  } else if (x > y) {
    return 1;
  } else {
    return 0;
  }
}

/**
 * Compare two paxid pairs.
 *
 * The first coordinate is subordinate to the second.  The second compares
 * normally (x.gen < y.gen ==> -1) but the first compares inversely (x.id
 * < y.id ==> 1)
 */
int
ppair_compare(ppair_t x, ppair_t y)
{
  // Compare generation first.
  if (x.gen < y.gen) {
    return -1;
  } else if (x.gen > y.gen) {
    return 1;
  }

  /* x.gen == y.gen */
  if (x.id < y.id) {
    return 1;
  } else if (x.id > y.id) {
    return -1;
  } else /* x.id == y.id */  {
    return 0;
  }
}

int
ballot_compare(ballot_t x, ballot_t y)
{
  return ppair_compare(x, y);
}

int
reqid_compare(reqid_t x, reqid_t y)
{
  return ppair_compare(x, y);
}


///////////////////////////////////////////////////////////////////////////
//
//  Struct destructors.
//

void
acceptor_destroy(struct paxos_acceptor *acc)
{
  if (acc != NULL) {
    paxos_peer_destroy(acc->pa_peer);
    g_free(acc->pa_desc);
  }
  g_free(acc);
}

void
instance_destroy(struct paxos_instance *inst)
{
  g_free(inst);
}

void
request_destroy(struct paxos_request *req)
{
  if (req != NULL) {
    g_free(req->pr_data);
  }
  g_free(req);
}


///////////////////////////////////////////////////////////////////////////
//
//  List operations.
//

#define XLIST_FIND_IMPL(name, id_t, le_field, id_field, compare)      \
  struct paxos_##name *                                               \
  name##_find(struct name##_list *head, id_t id)                      \
  {                                                                   \
    int cmp;                                                          \
    struct paxos_##name *it;                                          \
                                                                      \
    LIST_FOREACH(it, head, le_field) {                                \
      cmp = compare(id, it->id_field);                                \
      if (cmp == 0) {                                                 \
        return it;                                                    \
      } else if (cmp < 0) {                                           \
        break;                                                        \
      }                                                               \
    }                                                                 \
                                                                      \
    return NULL;                                                      \
  }

#define XLIST_FIND_REV_IMPL(name, id_t, le_field, id_field, compare)  \
  struct paxos_##name *                                               \
  name##_find(struct name##_list *head, id_t id)                      \
  {                                                                   \
    int cmp;                                                          \
    struct paxos_##name *it;                                          \
                                                                      \
    LIST_FOREACH_REV(it, head, le_field) {                            \
      cmp = compare(id, it->id_field);                                \
      if (cmp == 0) {                                                 \
        return it;                                                    \
      } else if (cmp > 0) {                                           \
        break;                                                        \
      }                                                               \
    }                                                                 \
                                                                      \
    return NULL;                                                      \
  }

#define XLIST_INSERT_IMPL(name, le_field, id_field, compare)          \
  struct paxos_##name *                                               \
  name##_insert(struct name##_list *head, struct paxos_##name *elt)   \
  {                                                                   \
    int cmp;                                                          \
    struct paxos_##name *it;                                          \
                                                                      \
    LIST_FOREACH(it, head, le_field) {                                \
      cmp = compare(elt->id_field, it->id_field);                     \
      if (cmp == 0) {                                                 \
        return it;                                                    \
      } else if (cmp < 0) {                                           \
        LIST_INSERT_BEFORE(head, it, elt, le_field);                  \
        return elt;                                                   \
      }                                                               \
    }                                                                 \
                                                                      \
    LIST_INSERT_TAIL(head, elt, le_field);                            \
    return elt;                                                       \
  }

#define XLIST_INSERT_REV_IMPL(name, le_field, id_field, compare)      \
  struct paxos_##name *                                               \
  name##_insert(struct name##_list *head, struct paxos_##name *elt)   \
  {                                                                   \
    int cmp;                                                          \
    struct paxos_##name *it;                                          \
                                                                      \
    LIST_FOREACH_REV(it, head, le_field) {                            \
      cmp = compare(elt->id_field, it->id_field);                     \
      if (cmp == 0) {                                                 \
        return it;                                                    \
      } else if (cmp > 0) {                                           \
        LIST_INSERT_AFTER(head, it, elt, le_field);                   \
        return elt;                                                   \
      }                                                               \
    }                                                                 \
                                                                      \
    LIST_INSERT_HEAD(head, elt, le_field);                            \
    return elt;                                                       \
  }

#define XLIST_DESTROY_IMPL(name, le_field, destroy)                   \
  void                                                                \
  name##_list_destroy(struct name##_list *head)                       \
  {                                                                   \
    struct paxos_##name *it, *prev;                                   \
                                                                      \
    prev = NULL;                                                      \
    LIST_FOREACH(it, head, le_field) {                                \
      if (prev != NULL) {                                             \
        LIST_REMOVE(head, prev, le_field);                            \
        destroy(prev);                                                \
      }                                                               \
      prev = it;                                                      \
    }                                                                 \
                                                                      \
    if (prev != NULL) {                                               \
      LIST_REMOVE(head, prev, le_field);                              \
      destroy(prev);                                                  \
    }                                                                 \
  }

XLIST_FIND_IMPL(acceptor, paxid_t, pa_le, pa_paxid, paxid_compare);
XLIST_FIND_REV_IMPL(instance, paxid_t, pi_le, pi_hdr.ph_inum, paxid_compare);
XLIST_FIND_IMPL(request, reqid_t, pr_le, pr_val.pv_reqid, reqid_compare);

XLIST_INSERT_REV_IMPL(acceptor, pa_le, pa_paxid, paxid_compare);
XLIST_INSERT_REV_IMPL(instance, pi_le, pi_hdr.ph_inum, paxid_compare);
XLIST_INSERT_REV_IMPL(request, pr_le, pr_val.pv_reqid, reqid_compare);

XLIST_DESTROY_IMPL(acceptor, pa_le, acceptor_destroy);
XLIST_DESTROY_IMPL(instance, pi_le, instance_destroy);
XLIST_DESTROY_IMPL(request, pr_le, request_destroy);

/**
 * Helper routine to obtain the instance on ilist with the closest instance
 * number >= inum.  We are passed in an iterator to simulate a continuation.
 */
struct paxos_instance *
get_instance_lub(struct paxos_instance *it, struct instance_list *ilist,
    paxid_t inum)
{
  for (; it != (void *)ilist; it = LIST_NEXT(it, pi_le)) {
    if (inum <= it->pi_hdr.ph_inum) {
      break;
    }
  }

  return it;
}

/**
 * Starting at a given instance number, crawl along an ilist until we find
 * a hole, i.e., an instance which has either not been recorded or not been
 * committed.  We return its instance number, along with closest-numbered
 * instance structure that has number <= the hole.
 */
paxid_t
ilist_first_hole(struct paxos_instance **inst, struct instance_list *ilist,
    paxid_t start)
{
  paxid_t inum;
  struct paxos_instance *it;

  // Obtain the first instance with inum >= start.
  it = get_instance_lub(LIST_FIRST(ilist), ilist, start);

  // If its inum != start, then start itself represents a hole.
  if (it->pi_hdr.ph_inum != start) {
    *inst = LIST_PREV(it, pi_le);
    if (*inst == (void *)ilist) {
      *inst = NULL;
    }
    return start;
  }

  // If the start instance is uncommitted, it's the hole we want.
  if (it->pi_votes != 0) {
    *inst = it;
    return start;
  }

  // We let inum lag one list entry behind the iterator in our loop to
  // detect holes; we use *inst to detect success.
  inum = start - 1;
  *inst = NULL;

  // Identify our first uncommitted or unrecorded instance.
  LIST_FOREACH(it, ilist, pi_le) {
    if (it->pi_hdr.ph_inum != inum + 1) {
      // We know there exists a previous element because start corresponded
      // to some existing instance.
      *inst = LIST_PREV(it, pi_le);
      return inum + 1;
    }
    if (it->pi_votes != 0) {
      // We found an uncommitted instance, so return it.
      *inst = it;
      return it->pi_hdr.ph_inum;
    }
    inum = it->pi_hdr.ph_inum;
  }

  // Default to the next unused instance.
  if (*inst == NULL) {
    *inst = LIST_LAST(ilist);
    return LIST_LAST(ilist)->pi_hdr.ph_inum + 1;
  }

  // Impossible.
  return 0;
}


///////////////////////////////////////////////////////////////////////////
//
//  Message delivery wrappers
//

/**
 * Broadcast a message to all acceptors.
 */
int
paxos_broadcast(const char *buffer, size_t length)
{
  struct paxos_acceptor *acc;
  int retval;

  LIST_FOREACH(acc, &(pax.alist), pa_le) {
    if (acc->pa_peer == NULL) {
      continue;
    }

    retval = paxos_peer_send(acc->pa_peer, buffer, length);
    if (retval) {
      return retval;
    }
  }

  return 0;
}

/**
 * Send a message to any acceptor.
 */
int
paxos_send(struct paxos_acceptor *acc, const char *buffer, size_t length)
{
  return paxos_peer_send(acc->pa_peer, buffer, length);
}

/**
 * Send a message to the proposer.
 */
int
paxos_send_to_proposer(const char *buffer, size_t length)
{
  if (pax.proposer == NULL) {
    return 1;
  }

  return paxos_peer_send(pax.proposer->pa_peer, buffer, length);
}
