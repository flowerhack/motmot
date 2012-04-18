/**
 * paxos_msgpack.c - Msgpack helpers.
 */
#include "paxos.h"
#include "paxos_msgpack.h"

#include <glib.h>
#include <msgpack.h>
#include <assert.h>

static void
msgpack_pack_paxid(msgpack_packer *pk, paxid_t paxid)
{
  msgpack_pack_uint32(pk, paxid);
}

void
paxos_payload_init(struct paxos_yak *py, size_t n)
{
  py->buf = msgpack_sbuffer_new();
  py->pk = msgpack_packer_new(py->buf, msgpack_sbuffer_write);

  msgpack_pack_array(py->pk, n);
}

void
paxos_payload_begin_array(struct paxos_yak *py, size_t n)
{
  msgpack_pack_array(py->pk, n);
}

void
paxos_payload_destroy(struct paxos_yak *py)
{
  msgpack_packer_free(py->pk);
  msgpack_sbuffer_free(py->buf);

  py->pk = NULL;
  py->buf = NULL;
}

char *
paxos_payload_data(struct paxos_yak *py)
{
  return py->buf->data;
}

size_t
paxos_payload_size(struct paxos_yak *py)
{
  return py->buf->size;
}

void
paxos_header_pack(struct paxos_yak *py, struct paxos_header *hdr)
{
  msgpack_pack_array(py->pk, 4);
  msgpack_pack_paxid(py->pk, hdr->ph_ballot.id);
  msgpack_pack_paxid(py->pk, hdr->ph_ballot.gen);
  msgpack_pack_int(py->pk, hdr->ph_opcode);
  msgpack_pack_paxid(py->pk, hdr->ph_inum);
}

void
paxos_header_unpack(struct paxos_header *hdr, msgpack_object *o)
{
  struct msgpack_object *p;

  // Make sure the input is well-formed
  assert(o->type == MSGPACK_OBJECT_ARRAY);
  assert(o->via.array.size == 4);

  p = o->via.array.ptr;
  // TODO: check types of these
  hdr->ph_ballot.id = p->via.u64;
  hdr->ph_ballot.gen = (++p)->via.u64;
  hdr->ph_opcode = (++p)->via.u64;
  hdr->ph_inum = (++p)->via.u64;
}

void
paxos_value_pack(struct paxos_yak *py, struct paxos_value *val)
{
  msgpack_pack_array(py->pk, 3);
  msgpack_pack_int(py->pk, val->pv_dkind);
  msgpack_pack_paxid(py->pk, val->pv_srcid);
  msgpack_pack_paxid(py->pk, val->pv_reqid);
}

void
paxos_value_unpack(struct paxos_value *val, msgpack_object *o)
{
  struct msgpack_object *p;

  // Make sure the input is well-formed
  assert(o->type == MSGPACK_OBJECT_ARRAY);
  assert(o->via.array.size == 3);

  p = o->via.array.ptr;
  // TODO: check types of these
  assert(p->type == MSGPACK_OBJECT_POSITIVE_INTEGER);
  val->pv_dkind = (p++)->via.u64;
  assert(p->type == MSGPACK_OBJECT_POSITIVE_INTEGER);
  val->pv_srcid = (p++)->via.u64;
  assert(p->type == MSGPACK_OBJECT_POSITIVE_INTEGER);
  val->pv_reqid = (p++)->via.u64;
}

void
paxos_raw_pack(struct paxos_yak *py, const char *buf, size_t n)
{
  msgpack_pack_raw(py->pk, n);
  msgpack_pack_raw_body(py->pk, buf, n);
}
