/*
 * Copyright (c) 2016 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <vlib/vlib.h>
#include <vnet/pg/pg.h>
#include <vnet/lisp-gpe/lisp_gpe.h>

typedef struct
{
  u32 next_index;
  u32 tunnel_index;
  u32 error;
  lisp_gpe_header_t h;
} lisp_gpe_rx_trace_t;

static u8 *
format_lisp_gpe_rx_trace (u8 * s, va_list * args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  lisp_gpe_rx_trace_t * t = va_arg (*args, lisp_gpe_rx_trace_t *);

  if (t->tunnel_index != ~0)
    {
      s = format (s, "LISP-GPE: tunnel %d next %d error %d", t->tunnel_index,
          t->next_index, t->error);
    }
  else
    {
      s = format (s, "LISP-GPE: no tunnel next %d error %d\n", t->next_index,
          t->error);
    }
  s = format (s, "\n  %U", format_lisp_gpe_header_with_length, &t->h,
      (u32) sizeof (t->h) /* max size */);
  return s;
}

static u32
next_proto_to_next_index[LISP_GPE_NEXT_PROTOS] = {
    LISP_GPE_INPUT_NEXT_DROP,
    LISP_GPE_INPUT_NEXT_IP4_INPUT,
    LISP_GPE_INPUT_NEXT_IP6_INPUT,
    LISP_GPE_INPUT_NEXT_DROP
};

static u32
next_protocol_to_next_index (lisp_gpe_header_t * lgh, u8 * next_header)
{
  /* lisp-gpe router */
  if (PREDICT_TRUE((lgh->flags & LISP_GPE_FLAGS_P)
      && lgh->next_protocol < LISP_GPE_NEXT_PROTOS))
    return next_proto_to_next_index[lgh->next_protocol];
  /* legay lisp router */
  else if ((lgh->flags & LISP_GPE_FLAGS_P) == 0)
    {
      ip4_header_t * iph = (ip4_header_t *) next_header;
      if ((iph->ip_version_and_header_length & 0xF0) == 0x40)
        return LISP_GPE_INPUT_NEXT_IP4_INPUT;
      else if ((iph->ip_version_and_header_length & 0xF0) == 0x60)
        return LISP_GPE_INPUT_NEXT_IP6_INPUT;
      else
        return LISP_GPE_INPUT_NEXT_DROP;
    }
  else
    return LISP_GPE_INPUT_NEXT_DROP;
}

static uword
lisp_gpe_input_inline (vlib_main_t * vm, vlib_node_runtime_t * node,
                       vlib_frame_t * from_frame, u8 is_v4)
{
  u32 n_left_from, next_index, * from, * to_next;
  u32 pkts_decapsulated = 0;
  lisp_gpe_main_t * lgm = &lisp_gpe_main;

  from = vlib_frame_vector_args (from_frame);
  n_left_from = from_frame->n_vectors;

  next_index = node->cached_next_index;

  while (n_left_from > 0)
    {
      u32 n_left_to_next;

      vlib_get_next_frame(vm, node, next_index, to_next, n_left_to_next);

      while (n_left_from >= 4 && n_left_to_next >= 2)
        {
          u32 bi0, bi1;
          vlib_buffer_t * b0, * b1;
          ip4_udp_lisp_gpe_header_t * iul4_0, * iul4_1;
          ip6_udp_lisp_gpe_header_t * iul6_0, * iul6_1;
          lisp_gpe_header_t * lh0, * lh1;
          u32 next0, next1, error0, error1;
          uword * si0, * si1;

          /* Prefetch next iteration. */
          {
            vlib_buffer_t * p2, * p3;

            p2 = vlib_get_buffer (vm, from[2]);
            p3 = vlib_get_buffer (vm, from[3]);

            vlib_prefetch_buffer_header (p2, LOAD);
            vlib_prefetch_buffer_header (p3, LOAD);

            CLIB_PREFETCH (p2->data, 2*CLIB_CACHE_LINE_BYTES, LOAD);
            CLIB_PREFETCH (p3->data, 2*CLIB_CACHE_LINE_BYTES, LOAD);
          }

          bi0 = from[0];
          bi1 = from[1];
          to_next[0] = bi0;
          to_next[1] = bi1;
          from += 2;
          to_next += 2;
          n_left_to_next -= 2;
          n_left_from -= 2;

          b0 = vlib_get_buffer (vm, bi0);
          b1 = vlib_get_buffer (vm, bi1);

          /* udp leaves current_data pointing at the lisp header */
          if (is_v4)
            {
              vlib_buffer_advance (
                  b0, -(word) (sizeof(udp_header_t) + sizeof(ip4_header_t)));
              vlib_buffer_advance (
                  b1, -(word) (sizeof(udp_header_t) + sizeof(ip4_header_t)));

              iul4_0 = vlib_buffer_get_current (b0);
              iul4_1 = vlib_buffer_get_current (b1);

              /* pop (ip, udp, lisp-gpe) */
              vlib_buffer_advance (b0, sizeof(*iul4_0));
              vlib_buffer_advance (b1, sizeof(*iul4_1));

              lh0 = &iul4_0->lisp;
              lh1 = &iul4_1->lisp;
            }
          else
            {
              vlib_buffer_advance (
                  b0, -(word) (sizeof(udp_header_t) + sizeof(ip6_header_t)));
              vlib_buffer_advance (
                  b1, -(word) (sizeof(udp_header_t) + sizeof(ip6_header_t)));

              iul6_0 = vlib_buffer_get_current (b0);
              iul6_1 = vlib_buffer_get_current (b1);

              /* pop (ip, udp, lisp-gpe) */
              vlib_buffer_advance (b0, sizeof(*iul6_0));
              vlib_buffer_advance (b1, sizeof(*iul6_1));

              lh0 = &iul6_0->lisp;
              lh1 = &iul6_1->lisp;
            }

          /* determine next_index from lisp-gpe header */
          next0 = next_protocol_to_next_index (lh0,
                                               vlib_buffer_get_current (b0));
          next1 = next_protocol_to_next_index (lh1,
                                               vlib_buffer_get_current (b1));

          /* Required to make the l2 tag push / pop code work on l2 subifs */
          vnet_update_l2_len (b0);
          vnet_update_l2_len (b1);

          /* map iid/vni to lisp-gpe sw_if_index which is used by ipx_input to
           * decide the rx vrf and the input features to be applied */
          si0 = hash_get(lgm->tunnel_term_sw_if_index_by_vni,
                         clib_net_to_host_u32 (lh0->iid));
          si1 = hash_get(lgm->tunnel_term_sw_if_index_by_vni,
                         clib_net_to_host_u32 (lh1->iid));

          if (si0)
            {
              vnet_buffer(b0)->sw_if_index[VLIB_RX] = si0[0];
              pkts_decapsulated++;
              error0 = 0;
            }
          else
            {
              next0 = LISP_GPE_INPUT_NEXT_DROP;
              error0 = LISP_GPE_ERROR_NO_SUCH_TUNNEL;
            }

          if (si1)
            {
              vnet_buffer(b1)->sw_if_index[VLIB_RX] = si1[0];
              pkts_decapsulated++;
              error1 = 0;
            }
          else
            {
              next1 = LISP_GPE_INPUT_NEXT_DROP;
              error1 = LISP_GPE_ERROR_NO_SUCH_TUNNEL;
            }

          b0->error = error0 ? node->errors[error0] : 0;
          b1->error = error1 ? node->errors[error1] : 0;

          if (PREDICT_FALSE(b0->flags & VLIB_BUFFER_IS_TRACED))
            {
              lisp_gpe_rx_trace_t *tr = vlib_add_trace (vm, node, b0,
                                                        sizeof(*tr));
              tr->next_index = next0;
              tr->error = error0;
              tr->h = lh0[0];
            }

          if (PREDICT_FALSE(b1->flags & VLIB_BUFFER_IS_TRACED))
            {
              lisp_gpe_rx_trace_t *tr = vlib_add_trace (vm, node, b1,
                                                        sizeof(*tr));
              tr->next_index = next1;
              tr->error = error1;
              tr->h = lh1[0];
            }

          vlib_validate_buffer_enqueue_x2(vm, node, next_index, to_next,
                                          n_left_to_next, bi0, bi1, next0,
                                          next1);
        }
    
      while (n_left_from > 0 && n_left_to_next > 0)
        {
          u32 bi0;
          vlib_buffer_t * b0;
          u32 next0;
          ip4_udp_lisp_gpe_header_t * iul4_0;
          ip6_udp_lisp_gpe_header_t * iul6_0;
          lisp_gpe_header_t * lh0;
          u32 error0;
          uword * si0;

          bi0 = from[0];
          to_next[0] = bi0;
          from += 1;
          to_next += 1;
          n_left_from -= 1;
          n_left_to_next -= 1;

          b0 = vlib_get_buffer (vm, bi0);

          /* udp leaves current_data pointing at the lisp header
           * TODO: there's no difference in processing between v4 and v6
           * encapsulated packets so the code should be simplified if ip header
           * info is not going to be used for dp smrs/dpsec */
          if (is_v4)
            {
              vlib_buffer_advance (
                  b0, -(word) (sizeof(udp_header_t) + sizeof(ip4_header_t)));

              iul4_0 = vlib_buffer_get_current (b0);

              /* pop (ip, udp, lisp-gpe) */
              vlib_buffer_advance (b0, sizeof(*iul4_0));

              lh0 = &iul4_0->lisp;
            }
          else
            {
              vlib_buffer_advance (
                  b0, -(word) (sizeof(udp_header_t) + sizeof(ip6_header_t)));

              iul6_0 = vlib_buffer_get_current (b0);

              /* pop (ip, udp, lisp-gpe) */
              vlib_buffer_advance (b0, sizeof(*iul6_0));

              lh0 = &iul6_0->lisp;
            }

          /* TODO if security is to be implemented, something similar to RPF,
           * probably we'd like to check that the peer is allowed to send us
           * packets. For this, we should use the tunnel table OR check that
           * we have a mapping for the source eid and that the outer source of
           * the packet is one of its locators */

          /* determine next_index from lisp-gpe header */
          next0 = next_protocol_to_next_index (lh0,
                                               vlib_buffer_get_current (b0));

          /* Required to make the l2 tag push / pop code work on l2 subifs */
          vnet_update_l2_len (b0);

          /* map iid/vni to lisp-gpe sw_if_index which is used by ipx_input to
           * decide the rx vrf and the input features to be applied */
          si0 = hash_get(lgm->tunnel_term_sw_if_index_by_vni,
                         clib_net_to_host_u32 (lh0->iid));

          if (si0)
            {
              vnet_buffer(b0)->sw_if_index[VLIB_RX] = si0[0];
              pkts_decapsulated++;
              error0 = 0;
            }
          else
            {
              next0 = LISP_GPE_INPUT_NEXT_DROP;
              error0 = LISP_GPE_ERROR_NO_SUCH_TUNNEL;
            }

          /* TODO error handling if security is implemented */
          b0->error = error0 ? node->errors[error0] : 0;

          if (PREDICT_FALSE(b0->flags & VLIB_BUFFER_IS_TRACED))
            {
              lisp_gpe_rx_trace_t *tr = vlib_add_trace (vm, node, b0,
                                                        sizeof(*tr));
              tr->next_index = next0;
              tr->error = error0;
              tr->h = lh0[0];
            }

          vlib_validate_buffer_enqueue_x1(vm, node, next_index, to_next,
                                          n_left_to_next, bi0, next0);
        }

      vlib_put_next_frame (vm, node, next_index, n_left_to_next);
    }
  vlib_node_increment_counter (vm, lisp_gpe_ip4_input_node.index,
                               LISP_GPE_ERROR_DECAPSULATED, 
                               pkts_decapsulated);
  return from_frame->n_vectors;
}

static uword
lisp_gpe_ip4_input (vlib_main_t * vm, vlib_node_runtime_t * node,
                    vlib_frame_t * from_frame)
{
  return lisp_gpe_input_inline(vm, node, from_frame, 1);
}

static uword
lisp_gpe_ip6_input (vlib_main_t * vm, vlib_node_runtime_t * node,
                    vlib_frame_t * from_frame)
{
  return lisp_gpe_input_inline(vm, node, from_frame, 0);
}

static char * lisp_gpe_error_strings[] = {
#define lisp_gpe_error(n,s) s,
#include <vnet/lisp-gpe/lisp_gpe_error.def>
#undef lisp_gpe_error
#undef _
};

VLIB_REGISTER_NODE (lisp_gpe_ip4_input_node) = {
  .function = lisp_gpe_ip4_input,
  .name = "lisp-gpe-ip4-input",
  /* Takes a vector of packets. */
  .vector_size = sizeof (u32),

  .n_errors = LISP_GPE_N_ERROR,
  .error_strings = lisp_gpe_error_strings,

  .n_next_nodes = LISP_GPE_INPUT_N_NEXT,
  .next_nodes = {
#define _(s,n) [LISP_GPE_INPUT_NEXT_##s] = n,
    foreach_lisp_gpe_ip_input_next
#undef _
  },

  .format_buffer = format_lisp_gpe_header_with_length,
  .format_trace = format_lisp_gpe_rx_trace,
  // $$$$ .unformat_buffer = unformat_lisp_gpe_header,
};

VLIB_REGISTER_NODE (lisp_gpe_ip6_input_node) = {
  .function = lisp_gpe_ip6_input,
  .name = "lisp-gpe-ip6-input",
  /* Takes a vector of packets. */
  .vector_size = sizeof (u32),

  .n_errors = LISP_GPE_N_ERROR,
  .error_strings = lisp_gpe_error_strings,

  .n_next_nodes = LISP_GPE_INPUT_N_NEXT,
  .next_nodes = {
#define _(s,n) [LISP_GPE_INPUT_NEXT_##s] = n,
    foreach_lisp_gpe_ip_input_next
#undef _
  },

  .format_buffer = format_lisp_gpe_header_with_length,
  .format_trace = format_lisp_gpe_rx_trace,
  // $$$$ .unformat_buffer = unformat_lisp_gpe_header,
};
