/**
 * paxos_msgpack.c - Msgpack helpers.
 */

#include <glib.h>
#include <msgpack.h>
#include <assert.h>

#include "paxos.h"
#include "paxos_msgpack.h"

///////////////////////////////////////////////////////////////////////////
//
//  Artificial msgpack primitives.
//

void
msgpack_pack_paxid(msgpack_packer *pk, paxid_t paxid)
{
  msgpack_pack_uint32(pk, paxid);
}

void
msgpack_pack_pax_uuid(msgpack_packer *pk, pax_uuid_t uuid)
{
  msgpack_pack_uint64(pk, uuid);
}


///////////////////////////////////////////////////////////////////////////
//
//  Paxos yak utilities.
//

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


///////////////////////////////////////////////////////////////////////////
//
//  Packing and unpacking.
//

void
paxos_paxid_pack(struct paxos_yak *py, paxid_t paxid)
{
  msgpack_pack_paxid(py->pk, paxid);
}

void
paxos_paxid_unpack(paxid_t *paxid, msgpack_object *o)
{
  assert(o->type == MSGPACK_OBJECT_POSITIVE_INTEGER);
  *paxid = o->via.u64;
}

void
paxos_uuid_pack(struct paxos_yak *py, pax_uuid_t uuid)
{
  msgpack_pack_pax_uuid(py->pk, uuid);
}

void
paxos_uuid_unpack(pax_uuid_t *uuid, msgpack_object *o)
{
  assert(o->type == MSGPACK_OBJECT_POSITIVE_INTEGER);
  *uuid = o->via.u64;
}

void
paxos_header_pack(struct paxos_yak *py, struct paxos_header *hdr)
{
  msgpack_pack_array(py->pk, 5);
  paxos_uuid_pack(py, hdr->ph_session);
  msgpack_pack_paxid(py->pk, hdr->ph_ballot.id);
  msgpack_pack_paxid(py->pk, hdr->ph_ballot.gen);
  msgpack_pack_int(py->pk, hdr->ph_opcode);
  msgpack_pack_paxid(py->pk, hdr->ph_inum);
}

void
paxos_header_unpack(struct paxos_header *hdr, msgpack_object *o)
{
  msgpack_object *p;

  // Make sure the input is well-formed.
  assert(o->type == MSGPACK_OBJECT_ARRAY);
  assert(o->via.array.size == 5);

  p = o->via.array.ptr;
  paxos_uuid_unpack(&hdr->ph_session, p++);
  assert(p->type == MSGPACK_OBJECT_POSITIVE_INTEGER);
  hdr->ph_ballot.id = (p++)->via.u64;
  assert(p->type == MSGPACK_OBJECT_POSITIVE_INTEGER);
  hdr->ph_ballot.gen = (p++)->via.u64;
  assert(p->type == MSGPACK_OBJECT_POSITIVE_INTEGER);
  hdr->ph_opcode = (p++)->via.u64;
  assert(p->type == MSGPACK_OBJECT_POSITIVE_INTEGER);
  hdr->ph_inum = (p++)->via.u64;
}

void
paxos_value_pack(struct paxos_yak *py, struct paxos_value *val)
{
  msgpack_pack_array(py->pk, 4);
  msgpack_pack_int(py->pk, val->pv_dkind);
  msgpack_pack_paxid(py->pk, val->pv_reqid.id);
  msgpack_pack_paxid(py->pk, val->pv_reqid.gen);
  msgpack_pack_paxid(py->pk, val->pv_extra);
}

void
paxos_value_unpack(struct paxos_value *val, msgpack_object *o)
{
  msgpack_object *p;

  // Make sure the input is well-formed.
  assert(o->type == MSGPACK_OBJECT_ARRAY);
  assert(o->via.array.size == 4);

  p = o->via.array.ptr;
  assert(p->type == MSGPACK_OBJECT_POSITIVE_INTEGER);
  val->pv_dkind = (p++)->via.u64;
  assert(p->type == MSGPACK_OBJECT_POSITIVE_INTEGER);
  val->pv_reqid.id = (p++)->via.u64;
  assert(p->type == MSGPACK_OBJECT_POSITIVE_INTEGER);
  val->pv_reqid.gen = (p++)->via.u64;
  assert(p->type == MSGPACK_OBJECT_POSITIVE_INTEGER);
  val->pv_extra = (p++)->via.u64;
}

void
paxos_request_pack(struct paxos_yak *py, struct paxos_request *req)
{
  msgpack_pack_array(py->pk, 2);
  paxos_value_pack(py, &req->pr_val);
  msgpack_pack_raw(py->pk, req->pr_size);
  msgpack_pack_raw_body(py->pk, req->pr_data, req->pr_size);
}

void
paxos_request_unpack(struct paxos_request *req, msgpack_object *o)
{
  msgpack_object *p;

  // Make sure the input is well-formed.
  assert(o->type == MSGPACK_OBJECT_ARRAY);
  assert(o->via.array.size == 2);

  // Unpack the value.
  p = o->via.array.ptr;
  paxos_value_unpack(&req->pr_val, p++);

  // Unpack the raw data.
  assert(p->type == MSGPACK_OBJECT_RAW);
  req->pr_size = p->via.raw.size;
  req->pr_data = g_memdup(p->via.raw.ptr, p->via.raw.size);
}

void
paxos_acceptor_pack(struct paxos_yak *py, struct paxos_acceptor *acc)
{
  msgpack_pack_array(py->pk, 2);
  msgpack_pack_paxid(py->pk, acc->pa_paxid);
  msgpack_pack_raw(py->pk, acc->pa_size);
  msgpack_pack_raw_body(py->pk, acc->pa_desc, acc->pa_size);
}

void
paxos_acceptor_unpack(struct paxos_acceptor *acc, msgpack_object *o)
{
  msgpack_object *p;

  // Make sure the input is well-formed.
  assert(o->type == MSGPACK_OBJECT_ARRAY);
  assert(o->via.array.size == 2);

  p = o->via.array.ptr;

  acc->pa_peer = NULL;
  assert(p->type == MSGPACK_OBJECT_POSITIVE_INTEGER);
  acc->pa_paxid = (p++)->via.u64;
  assert(p->type == MSGPACK_OBJECT_RAW);
  acc->pa_size = p->via.raw.size;
  acc->pa_desc = g_memdup(p->via.raw.ptr, p->via.raw.size);
}

void
paxos_instance_pack(struct paxos_yak *py, struct paxos_instance *inst)
{
  msgpack_pack_array(py->pk, 3);
  paxos_header_pack(py, &inst->pi_hdr);
  inst->pi_committed ? msgpack_pack_true(py->pk) : msgpack_pack_false(py->pk);
  paxos_value_pack(py, &inst->pi_val);
}

void
paxos_instance_unpack(struct paxos_instance *inst, msgpack_object *o)
{
  msgpack_object *p;

  // Make sure the input is well-formed.
  assert(o->type == MSGPACK_OBJECT_ARRAY);
  assert(o->via.array.size == 3);

  p = o->via.array.ptr;

  // Unpack the header, committed flag, and value.
  paxos_header_unpack(&inst->pi_hdr, p++);
  assert(p->type == MSGPACK_OBJECT_BOOLEAN);
  inst->pi_committed = (p++)->via.boolean;
  paxos_value_unpack(&inst->pi_val, p++);

  // Set everything else to 0.
  inst->pi_cached = false;
  inst->pi_learned = false;
  inst->pi_votes = 0;
  inst->pi_rejects = 0;
}
