/*
 * ngtcp2
 *
 * Copyright (c) 2017 ngtcp2 contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "ngtcp2_acktr.h"

#include <assert.h>

#include "ngtcp2_conn.h"
#include "ngtcp2_macro.h"

int ngtcp2_acktr_entry_new(ngtcp2_acktr_entry **ent, uint64_t pkt_num,
                           ngtcp2_tstamp tstamp, int unprotected,
                           ngtcp2_mem *mem) {
  *ent = ngtcp2_mem_malloc(mem, sizeof(ngtcp2_acktr_entry));
  if (*ent == NULL) {
    return NGTCP2_ERR_NOMEM;
  }

  (*ent)->next = NULL;
  (*ent)->pprev = NULL;
  (*ent)->pkt_num = pkt_num;
  (*ent)->tstamp = tstamp;
  (*ent)->unprotected = (uint8_t)unprotected;

  return 0;
}

void ngtcp2_acktr_entry_del(ngtcp2_acktr_entry *ent, ngtcp2_mem *mem) {
  ngtcp2_mem_free(mem, ent);
}

int ngtcp2_acktr_init(ngtcp2_acktr *acktr, ngtcp2_log *log, ngtcp2_mem *mem) {
  int rv;

  rv = ngtcp2_ringbuf_init(&acktr->acks, 128, sizeof(ngtcp2_acktr_ack_entry),
                           mem);
  if (rv != 0) {
    return rv;
  }

  rv = ngtcp2_ringbuf_init(&acktr->hs_acks, 32, sizeof(ngtcp2_acktr_ack_entry),
                           mem);
  if (rv != 0) {
    return rv;
  }

  acktr->ent = NULL;
  acktr->tail = NULL;
  acktr->log = log;
  acktr->mem = mem;
  acktr->nack = 0;
  acktr->last_hs_ack_pkt_num = UINT64_MAX;
  acktr->flags = NGTCP2_ACKTR_FLAG_NONE;
  acktr->first_unacked_ts = UINT64_MAX;

  return 0;
}

void ngtcp2_acktr_free(ngtcp2_acktr *acktr) {
  ngtcp2_acktr_entry *ent, *next;
  ngtcp2_acktr_ack_entry *ack_ent;
  size_t i;

  if (acktr == NULL) {
    return;
  }

  for (i = 0; i < acktr->hs_acks.len; ++i) {
    ack_ent = ngtcp2_ringbuf_get(&acktr->hs_acks, i);
    ngtcp2_mem_free(acktr->mem, ack_ent->ack);
  }
  ngtcp2_ringbuf_free(&acktr->hs_acks);

  for (i = 0; i < acktr->acks.len; ++i) {
    ack_ent = ngtcp2_ringbuf_get(&acktr->acks, i);
    ngtcp2_mem_free(acktr->mem, ack_ent->ack);
  }
  ngtcp2_ringbuf_free(&acktr->acks);

  for (ent = acktr->ent; ent;) {
    next = ent->next;
    ngtcp2_acktr_entry_del(ent, acktr->mem);
    ent = next;
  }
}

int ngtcp2_acktr_add(ngtcp2_acktr *acktr, ngtcp2_acktr_entry *ent,
                     int active_ack, ngtcp2_tstamp ts) {
  ngtcp2_acktr_entry **pent;
  ngtcp2_acktr_entry *tail;

  for (pent = &acktr->ent; *pent; pent = &(*pent)->next) {
    if ((*pent)->pkt_num > ent->pkt_num) {
      continue;
    }
    /* TODO What to do if we receive duplicated packet number? */
    if ((*pent)->pkt_num == ent->pkt_num) {
      return NGTCP2_ERR_PROTO;
    }
    break;
  }

  ent->next = *pent;
  ent->pprev = pent;
  if (ent->next) {
    ent->next->pprev = &ent->next;
  } else {
    acktr->tail = ent;
  }
  *pent = ent;

  if (active_ack) {
    if (ent->unprotected) {
      /* Should be sent in both protected and unprotected ACK */
      acktr->flags |= NGTCP2_ACKTR_FLAG_ACTIVE_ACK;
    } else {
      acktr->flags |= NGTCP2_ACKTR_FLAG_ACTIVE_ACK_PROTECTED;
    }
    if (acktr->first_unacked_ts == UINT64_MAX) {
      acktr->first_unacked_ts = ts;
    }
  }

  if (++acktr->nack > NGTCP2_ACKTR_MAX_ENT) {
    assert(acktr->tail);

    tail = acktr->tail;
    *tail->pprev = NULL;

    acktr->tail = ngtcp2_struct_of(tail->pprev, ngtcp2_acktr_entry, next);

    ngtcp2_acktr_entry_del(tail, acktr->mem);
    --acktr->nack;
  }

  return 0;
}

void ngtcp2_acktr_forget(ngtcp2_acktr *acktr, ngtcp2_acktr_entry *ent) {
  ngtcp2_acktr_entry *next;

  if (ent->pprev != &acktr->ent) {
    *ent->pprev = NULL;
    acktr->tail = ngtcp2_struct_of(ent->pprev, ngtcp2_acktr_entry, next);
  } else {
    acktr->ent = acktr->tail = NULL;
  }

  for (; ent;) {
    next = ent->next;
    ngtcp2_acktr_entry_del(ent, acktr->mem);
    ent = next;
    --acktr->nack;
  }
}

ngtcp2_acktr_entry **ngtcp2_acktr_get(ngtcp2_acktr *acktr) {
  return &acktr->ent;
}

void ngtcp2_acktr_pop(ngtcp2_acktr *acktr) {
  ngtcp2_acktr_entry *ent = acktr->ent;

  assert(acktr->ent);

  --acktr->nack;
  acktr->ent = acktr->ent->next;
  if (acktr->ent) {
    acktr->ent->pprev = &acktr->ent;
  } else {
    acktr->tail = NULL;
  }

  ngtcp2_acktr_entry_del(ent, acktr->mem);
}

ngtcp2_acktr_ack_entry *ngtcp2_acktr_add_ack(ngtcp2_acktr *acktr,
                                             uint64_t pkt_num, ngtcp2_ack *fr,
                                             ngtcp2_tstamp ts, int unprotected,
                                             int ack_only) {
  ngtcp2_acktr_ack_entry *ent;

  if (unprotected) {
    ent = ngtcp2_ringbuf_push_front(&acktr->hs_acks);
  } else {
    ent = ngtcp2_ringbuf_push_front(&acktr->acks);
  }
  ent->ack = fr;
  ent->pkt_num = pkt_num;
  ent->ts = ts;
  ent->ack_only = (uint8_t)ack_only;

  return ent;
}

static void acktr_remove(ngtcp2_acktr *acktr, ngtcp2_acktr_entry **pent) {
  ngtcp2_acktr_entry *ent;

  ent = *pent;
  *pent = (*pent)->next;
  if (*pent) {
    (*pent)->pprev = pent;
  } else {
    acktr->tail = ngtcp2_struct_of(pent, ngtcp2_acktr_entry, next);
  }

  ngtcp2_acktr_entry_del(ent, acktr->mem);

  --acktr->nack;
}

static void acktr_on_ack(ngtcp2_acktr *acktr, ngtcp2_ringbuf *rb,
                         size_t ack_ent_offset) {
  ngtcp2_acktr_ack_entry *ent;
  ngtcp2_ack *fr;
  ngtcp2_acktr_entry **pent;
  uint64_t largest_ack, min_ack;
  size_t i;

  ent = ngtcp2_ringbuf_get(rb, ack_ent_offset);
  fr = ent->ack;
  largest_ack = fr->largest_ack;

  if (ent->pkt_num >= acktr->last_hs_ack_pkt_num) {
    acktr->flags |= NGTCP2_ACKTR_FLAG_ACK_FINISHED_ACK;
    acktr->last_hs_ack_pkt_num = UINT64_MAX;

    ngtcp2_log_info(acktr->log, NGTCP2_LOG_EVENT_CON,
                    "packet after last handshake packet was acknowledged");
  }

  /* Assume that ngtcp2_pkt_validate_ack(fr) returns 0 */
  for (pent = &acktr->ent; *pent; pent = &(*pent)->next) {
    if (largest_ack >= (*pent)->pkt_num) {
      break;
    }
  }
  if (*pent == NULL) {
    goto fin;
  }

  min_ack = largest_ack - fr->first_ack_blklen;

  for (; *pent;) {
    if (min_ack <= (*pent)->pkt_num && (*pent)->pkt_num <= largest_ack) {
      acktr_remove(acktr, pent);
      continue;
    }
    break;
  }

  for (i = 0; i < fr->num_blks && *pent;) {
    largest_ack = min_ack - fr->blks[i].gap - 2;
    min_ack = largest_ack - fr->blks[i].blklen;

    for (; *pent;) {
      if ((*pent)->pkt_num > largest_ack) {
        pent = &(*pent)->next;
        continue;
      }
      if ((*pent)->pkt_num < min_ack) {
        break;
      }
      acktr_remove(acktr, pent);
    }

    ++i;
  }

fin:
  for (i = ack_ent_offset; i < rb->len; ++i) {
    ent = ngtcp2_ringbuf_get(rb, i);
    ngtcp2_mem_free(acktr->mem, ent->ack);
  }
  ngtcp2_ringbuf_resize(rb, ack_ent_offset);
}

int ngtcp2_acktr_recv_ack(ngtcp2_acktr *acktr, const ngtcp2_ack *fr,
                          int unprotected, ngtcp2_conn *conn,
                          ngtcp2_tstamp ts) {
  ngtcp2_acktr_ack_entry *ent;
  uint64_t largest_ack = fr->largest_ack, min_ack;
  size_t i, j;
  ngtcp2_ringbuf *rb = unprotected ? &acktr->hs_acks : &acktr->acks;
  size_t nacks = ngtcp2_ringbuf_len(rb);

  /* Assume that ngtcp2_pkt_validate_ack(fr) returns 0 */
  for (j = 0; j < nacks; ++j) {
    ent = ngtcp2_ringbuf_get(rb, j);
    if (largest_ack >= ent->pkt_num) {
      break;
    }
  }
  if (j == nacks) {
    return 0;
  }

  min_ack = largest_ack - fr->first_ack_blklen;

  for (;;) {
    if (min_ack <= ent->pkt_num && ent->pkt_num <= largest_ack) {
      acktr_on_ack(acktr, rb, j);
      if (conn && largest_ack == ent->pkt_num && ent->ack_only) {
        ngtcp2_conn_update_rtt(conn, ts - ent->ts, fr->ack_delay_unscaled,
                               ent->ack_only);
      }
      return 0;
    }
    break;
  }

  for (i = 0; i < fr->num_blks && j < nacks;) {
    largest_ack = min_ack - fr->blks[i].gap - 2;
    min_ack = largest_ack - fr->blks[i].blklen;

    for (;;) {
      if (ent->pkt_num > largest_ack) {
        ++j;
        if (j == nacks) {
          return 0;
        }
        ent = ngtcp2_ringbuf_get(rb, j);
        continue;
      }
      if (ent->pkt_num < min_ack) {
        break;
      }
      acktr_on_ack(acktr, rb, j);
      return 0;
    }

    ++i;
  }

  return 0;
}

void ngtcp2_acktr_commit_ack(ngtcp2_acktr *acktr, int unprotected) {
  if (unprotected) {
    acktr->flags &= (uint8_t)~NGTCP2_ACKTR_FLAG_ACTIVE_ACK_UNPROTECTED;
  } else {
    acktr->flags &= (uint8_t)~NGTCP2_ACKTR_FLAG_ACTIVE_ACK_PROTECTED;
    acktr->first_unacked_ts = UINT64_MAX;
  }
}

int ngtcp2_acktr_require_active_ack(ngtcp2_acktr *acktr, int unprotected,
                                    uint64_t max_ack_delay, ngtcp2_tstamp ts) {
  if (unprotected) {
    return acktr->flags & NGTCP2_ACKTR_FLAG_ACTIVE_ACK_UNPROTECTED;
  }
  return (acktr->flags & NGTCP2_ACKTR_FLAG_ACTIVE_ACK_PROTECTED) &&
         acktr->first_unacked_ts <= ts - max_ack_delay;
}

int ngtcp2_acktr_include_protected_pkt(ngtcp2_acktr *acktr) {
  ngtcp2_acktr_entry *ent;

  for (ent = acktr->ent; ent; ent = ent->next) {
    if (!ent->unprotected) {
      return 1;
    }
  }

  return 0;
}
