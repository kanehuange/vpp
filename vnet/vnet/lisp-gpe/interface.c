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

#include <vppinfra/error.h>
#include <vppinfra/hash.h>
#include <vnet/vnet.h>
#include <vnet/ip/ip.h>
#include <vnet/ip/udp.h>
#include <vnet/ethernet/ethernet.h>
#include <vnet/lisp-gpe/lisp_gpe.h>

#define foreach_lisp_gpe_tx_next        \
  _(DROP, "error-drop")                 \
  _(IP4_LOOKUP, "ip4-lookup")           \
  _(IP6_LOOKUP, "ip6-lookup")

typedef enum
{
#define _(sym,str) LISP_GPE_TX_NEXT_##sym,
  foreach_lisp_gpe_tx_next
#undef _
  LISP_GPE_TX_N_NEXT,
} lisp_gpe_tx_next_t;

typedef struct
{
  u32 tunnel_index;
} lisp_gpe_tx_trace_t;

u8 *
format_lisp_gpe_tx_trace (u8 * s, va_list * args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  lisp_gpe_tx_trace_t * t = va_arg (*args, lisp_gpe_tx_trace_t *);

  s = format (s, "LISP-GPE-TX: tunnel %d", t->tunnel_index);
  return s;
}

always_inline void
get_one_tunnel_inline (lisp_gpe_main_t * lgm, vlib_buffer_t * b0,
                       lisp_gpe_tunnel_t ** t0, u8 is_v4)
{
  u32 adj_index0, tunnel_index0;
  ip_adjacency_t * adj0;

  /* Get adjacency and from it the tunnel_index */
  adj_index0 = vnet_buffer(b0)->ip.adj_index[VLIB_TX];

  if (is_v4)
    adj0 = ip_get_adjacency (lgm->lm4, adj_index0);
  else
    adj0 = ip_get_adjacency (lgm->lm6, adj_index0);

  tunnel_index0 = adj0->if_address_index;
  t0[0] = pool_elt_at_index(lgm->tunnels, tunnel_index0);

  ASSERT(t0[0] != 0);
}

always_inline void
encap_one_inline (lisp_gpe_main_t * lgm, vlib_buffer_t * b0,
                  lisp_gpe_tunnel_t * t0, u32 * next0)
{
  ASSERT(sizeof(ip4_udp_lisp_gpe_header_t) == 36);
  ASSERT(sizeof(ip6_udp_lisp_gpe_header_t) == 56);

  lisp_gpe_sub_tunnel_t * st0;
  u32 * sti0;

  sti0 = vec_elt_at_index(t0->sub_tunnels_lbv,
      vnet_buffer(b0)->ip.flow_hash % t0->sub_tunnels_lbv_count);
  st0 = vec_elt_at_index(t0->sub_tunnels, sti0[0]);
  if (st0->is_ip4)
    {
      ip_udp_encap_one (lgm->vlib_main, b0, st0->rewrite, 36, 1);
      next0[0] = LISP_GPE_TX_NEXT_IP4_LOOKUP;
    }
  else
    {
      ip_udp_encap_one (lgm->vlib_main, b0, st0->rewrite, 56, 0);
      next0[0] = LISP_GPE_TX_NEXT_IP6_LOOKUP;
    }

  /* Reset to look up tunnel partner in the configured FIB */
  vnet_buffer(b0)->sw_if_index[VLIB_TX] = t0->encap_fib_index;
}

always_inline void
get_two_tunnels_inline (lisp_gpe_main_t * lgm, vlib_buffer_t * b0,
                        vlib_buffer_t * b1, lisp_gpe_tunnel_t ** t0,
                        lisp_gpe_tunnel_t ** t1, u8 is_v4)
{
  u32 adj_index0, adj_index1, tunnel_index0, tunnel_index1;
  ip_adjacency_t * adj0, * adj1;

  /* Get adjacency and from it the tunnel_index */
  adj_index0 = vnet_buffer(b0)->ip.adj_index[VLIB_TX];
  adj_index1 = vnet_buffer(b1)->ip.adj_index[VLIB_TX];

  if (is_v4)
    {
      adj0 = ip_get_adjacency (lgm->lm4, adj_index0);
      adj1 = ip_get_adjacency (lgm->lm4, adj_index1);
    }
  else
    {
      adj0 = ip_get_adjacency (lgm->lm6, adj_index0);
      adj1 = ip_get_adjacency (lgm->lm6, adj_index1);
    }

  tunnel_index0 = adj0->if_address_index;
  tunnel_index1 = adj1->if_address_index;

  t0[0] = pool_elt_at_index(lgm->tunnels, tunnel_index0);
  t1[0] = pool_elt_at_index(lgm->tunnels, tunnel_index1);

  ASSERT(t0[0] != 0);
  ASSERT(t1[0] != 0);
}

always_inline void
encap_two_inline (lisp_gpe_main_t * lgm, vlib_buffer_t * b0, vlib_buffer_t * b1,
                  lisp_gpe_tunnel_t * t0, lisp_gpe_tunnel_t * t1, u32 * next0,
                  u32 * next1)
{
  ASSERT(sizeof(ip4_udp_lisp_gpe_header_t) == 36);
  ASSERT(sizeof(ip6_udp_lisp_gpe_header_t) == 56);

  lisp_gpe_sub_tunnel_t * st0, * st1;
  u32 * sti0, * sti1;
  sti0 = vec_elt_at_index(t0->sub_tunnels_lbv,
      vnet_buffer(b0)->ip.flow_hash % t0->sub_tunnels_lbv_count);
  sti1 = vec_elt_at_index(t1->sub_tunnels_lbv,
      vnet_buffer(b1)->ip.flow_hash % t1->sub_tunnels_lbv_count);
  st0 = vec_elt_at_index(t0->sub_tunnels, sti0[0]);
  st1 = vec_elt_at_index(t1->sub_tunnels, sti1[0]);

  if (PREDICT_TRUE(st0->is_ip4 == st1->is_ip4))
    {
      if (st0->is_ip4)
        {
          ip_udp_encap_one (lgm->vlib_main, b0, st0->rewrite, 36, 1);
          ip_udp_encap_one (lgm->vlib_main, b1, st1->rewrite, 36, 1);
          next0[0] = next1[0] = LISP_GPE_TX_NEXT_IP4_LOOKUP;
        }
      else
        {
          ip_udp_encap_one (lgm->vlib_main, b0, st0->rewrite, 56, 0);
          ip_udp_encap_one (lgm->vlib_main, b1, st1->rewrite, 56, 0);
          next0[0] = next1[0] = LISP_GPE_TX_NEXT_IP6_LOOKUP;
        }
    }
  else
    {
      if (st0->is_ip4)
        {
          ip_udp_encap_one (lgm->vlib_main, b0, st0->rewrite, 36, 1);
          ip_udp_encap_one (lgm->vlib_main, b1, st1->rewrite, 56, 1);
          next0[0] = LISP_GPE_TX_NEXT_IP4_LOOKUP;
          next1[0] = LISP_GPE_TX_NEXT_IP6_LOOKUP;
        }
      else
        {
          ip_udp_encap_one (lgm->vlib_main, b0, st0->rewrite, 56, 1);
          ip_udp_encap_one (lgm->vlib_main, b1, st1->rewrite, 36, 1);
          next0[0] = LISP_GPE_TX_NEXT_IP6_LOOKUP;
          next1[0] = LISP_GPE_TX_NEXT_IP4_LOOKUP;
        }
    }

  /* Reset to look up tunnel partner in the configured FIB */
  vnet_buffer(b0)->sw_if_index[VLIB_TX] = t0->encap_fib_index;
  vnet_buffer(b1)->sw_if_index[VLIB_TX] = t1->encap_fib_index;
}

#define is_v4_packet(_h) ((*(u8*) _h) & 0xF0) == 0x40

static uword
lisp_gpe_interface_tx (vlib_main_t * vm, vlib_node_runtime_t * node,
                       vlib_frame_t * from_frame)
{
  u32 n_left_from, next_index, * from, * to_next;
  lisp_gpe_main_t * lgm = &lisp_gpe_main;

  from = vlib_frame_vector_args (from_frame);
  n_left_from = from_frame->n_vectors;

  next_index = node->cached_next_index;

  while (n_left_from > 0)
    {
      u32 n_left_to_next;

      vlib_get_next_frame (vm, node, next_index,
                           to_next, n_left_to_next);

      while (n_left_from >= 4 && n_left_to_next >= 2)
        {
          u32 bi0, bi1;
          vlib_buffer_t * b0, * b1;
          u32 next0, next1;
          lisp_gpe_tunnel_t * t0 = 0, * t1 = 0;
          u8 is_v4_eid0, is_v4_eid1;

          next0 = next1 = LISP_GPE_TX_NEXT_IP4_LOOKUP;

          /* Prefetch next iteration. */
            {
              vlib_buffer_t * p2, *p3;

              p2 = vlib_get_buffer (vm, from[2]);
              p3 = vlib_get_buffer (vm, from[3]);

              vlib_prefetch_buffer_header(p2, LOAD);
              vlib_prefetch_buffer_header(p3, LOAD);

              CLIB_PREFETCH(p2->data, 2*CLIB_CACHE_LINE_BYTES, LOAD);
              CLIB_PREFETCH(p3->data, 2*CLIB_CACHE_LINE_BYTES, LOAD);
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

          is_v4_eid0 = is_v4_packet(vlib_buffer_get_current (b0));
          is_v4_eid1 = is_v4_packet(vlib_buffer_get_current (b1));

          if (PREDICT_TRUE(is_v4_eid0 == is_v4_eid1))
            {
              get_two_tunnels_inline (lgm, b0, b1, &t0, &t1,
                                      is_v4_eid0 ? 1 : 0);
            }
          else
            {
              get_one_tunnel_inline (lgm, b0, &t0, is_v4_eid0 ? 1 : 0);
              get_one_tunnel_inline (lgm, b1, &t1, is_v4_eid1 ? 1 : 0);
            }

          encap_two_inline (lgm, b0, b1, t0, t1, &next0, &next1);

          if (PREDICT_FALSE(b0->flags & VLIB_BUFFER_IS_TRACED))
            {
              lisp_gpe_tx_trace_t *tr = vlib_add_trace (vm, node, b0,
                                                           sizeof(*tr));
              tr->tunnel_index = t0 - lgm->tunnels;
            }
          if (PREDICT_FALSE(b1->flags & VLIB_BUFFER_IS_TRACED))
            {
              lisp_gpe_tx_trace_t *tr = vlib_add_trace (vm, node, b1,
                                                           sizeof(*tr));
              tr->tunnel_index = t1 - lgm->tunnels;
            }

          vlib_validate_buffer_enqueue_x2(vm, node, next_index, to_next,
                                          n_left_to_next, bi0, bi1, next0,
                                          next1);
        }

      while (n_left_from > 0 && n_left_to_next > 0)
        {
          vlib_buffer_t * b0;
          u32 bi0, next0 = LISP_GPE_TX_NEXT_IP4_LOOKUP;
          lisp_gpe_tunnel_t * t0 = 0;
          u8 is_v4_0;

          bi0 = from[0];
          to_next[0] = bi0;
          from += 1;
          to_next += 1;
          n_left_from -= 1;
          n_left_to_next -= 1;

          b0 = vlib_get_buffer (vm, bi0);

          is_v4_0 = is_v4_packet(vlib_buffer_get_current (b0));
          get_one_tunnel_inline (lgm, b0, &t0, is_v4_0 ? 1 : 0);

          encap_one_inline (lgm, b0, t0, &next0);

          if (PREDICT_FALSE(b0->flags & VLIB_BUFFER_IS_TRACED)) 
            {
              lisp_gpe_tx_trace_t *tr = vlib_add_trace (vm, node, b0,
                                                           sizeof(*tr));
              tr->tunnel_index = t0 - lgm->tunnels;
            }
          vlib_validate_buffer_enqueue_x1(vm, node, next_index, to_next,
                                          n_left_to_next, bi0, next0);
        }

      vlib_put_next_frame (vm, node, next_index, n_left_to_next);
    }

  return from_frame->n_vectors;
}

static u8 *
format_lisp_gpe_name (u8 * s, va_list * args)
{
  u32 dev_instance = va_arg (*args, u32);
  return format (s, "lisp_gpe%d", dev_instance);
}

VNET_DEVICE_CLASS (lisp_gpe_device_class,static) = {
  .name = "LISP_GPE",
  .format_device_name = format_lisp_gpe_name,
  .format_tx_trace = format_lisp_gpe_tx_trace,
  .tx_function = lisp_gpe_interface_tx,
  .no_flatten_output_chains = 1,
};

static uword
dummy_set_rewrite (vnet_main_t * vnm, u32 sw_if_index, u32 l3_type,
                   void * dst_address, void * rewrite, uword max_rewrite_bytes)
{
  return 0;
}

u8 *
format_lisp_gpe_header_with_length (u8 * s, va_list * args)
{
  lisp_gpe_header_t * h = va_arg (*args, lisp_gpe_header_t *);
  u32 max_header_bytes = va_arg (*args, u32);
  u32 header_bytes;

  header_bytes = sizeof (h[0]);
  if (max_header_bytes != 0 && header_bytes > max_header_bytes)
    return format (s, "lisp-gpe header truncated");

  s = format (s, "flags: ");
#define _(n,v) if (h->flags & v) s = format (s, "%s ", #n);
  foreach_lisp_gpe_flag_bit;
#undef _

  s = format (s, "\n  ver_res %d res %d next_protocol %d iid %d(%x)",
              h->ver_res, h->res, h->next_protocol,
              clib_net_to_host_u32 (h->iid),
              clib_net_to_host_u32 (h->iid));
  return s;
}

VNET_HW_INTERFACE_CLASS (lisp_gpe_hw_class) = {
  .name = "LISP_GPE",
  .format_header = format_lisp_gpe_header_with_length,
  .set_rewrite = dummy_set_rewrite,
};

int
add_del_ip_prefix_route (ip_prefix_t * dst_prefix, u32 table_id,
                         ip_adjacency_t * add_adj, u8 is_add, u32 * adj_index)
{
  uword * p;

  if (ip_prefix_version(dst_prefix) == IP4)
    {
      ip4_main_t * im4 = &ip4_main;
      ip4_add_del_route_args_t a;
      ip4_address_t addr = ip_prefix_v4(dst_prefix);

      memset(&a, 0, sizeof(a));
      a.flags = IP4_ROUTE_FLAG_TABLE_ID;
      a.table_index_or_table_id = table_id;
      a.adj_index = ~0;
      a.dst_address_length = ip_prefix_len(dst_prefix);
      a.dst_address = addr;
      a.flags |= is_add ? IP4_ROUTE_FLAG_ADD : IP4_ROUTE_FLAG_DEL;
      a.add_adj = add_adj;
      a.n_add_adj = is_add ? 1 : 0;

      ip4_add_del_route (im4, &a);

      if (is_add)
        {
          p = ip4_get_route (im4, table_id, 0, addr.as_u8,
                             ip_prefix_len(dst_prefix));
          if (p == 0)
            {
              clib_warning("Failed to insert route for eid %U!",
                           format_ip4_address_and_length, addr.as_u8,
                           ip_prefix_len(dst_prefix));
              return -1;
            }
          adj_index[0] = p[0];
        }
    }
  else
    {
      ip6_main_t * im6 = &ip6_main;
      ip6_add_del_route_args_t a;
      ip6_address_t addr = ip_prefix_v6(dst_prefix);

      memset(&a, 0, sizeof(a));
      a.flags = IP6_ROUTE_FLAG_TABLE_ID;
      a.table_index_or_table_id = table_id;
      a.adj_index = ~0;
      a.dst_address_length = ip_prefix_len(dst_prefix);
      a.dst_address = addr;
      a.flags |= is_add ? IP6_ROUTE_FLAG_ADD : IP6_ROUTE_FLAG_DEL;
      a.add_adj = add_adj;
      a.n_add_adj = is_add ? 1 : 0;

      ip6_add_del_route (im6, &a);

      if (is_add)
        {
          adj_index[0] = ip6_get_route (im6, table_id, 0, &addr,
                                        ip_prefix_len(dst_prefix));
          if (adj_index[0] == 0)
            {
              clib_warning("Failed to insert route for eid %U!",
                           format_ip6_address_and_length, addr.as_u8,
                           ip_prefix_len(dst_prefix));
              return -1;
            }
        }
    }
  return 0;
}

static void
add_del_lisp_gpe_default_route (u32 table_id, u8 is_v4, u8 is_add)
{
  lisp_gpe_main_t * lgm = &lisp_gpe_main;
  ip_adjacency_t adj;
  ip_prefix_t prefix;
  u32 adj_index = 0;

  /* setup adjacency */
  memset (&adj, 0, sizeof(adj));

  adj.n_adj = 1;
  adj.explicit_fib_index = ~0;
  adj.lookup_next_index = is_v4 ? lgm->ip4_lookup_next_lgpe_ip4_lookup :
                                  lgm->ip6_lookup_next_lgpe_ip6_lookup;
  /* default route has tunnel_index ~0 */
  adj.rewrite_header.sw_if_index = ~0;

  /* set prefix to 0/0 */
  memset(&prefix, 0, sizeof(prefix));
  ip_prefix_version(&prefix) = is_v4 ? IP4 : IP6;

  /* add/delete route for prefix */
  add_del_ip_prefix_route (&prefix, table_id, &adj, is_add, &adj_index);
}

static void
lisp_gpe_iface_set_table (u32 sw_if_index, u32 table_id, u8 is_ip4)
{
  if (is_ip4)
    {
      ip4_main_t * im4 = &ip4_main;
      ip4_fib_t * fib;
      fib = find_ip4_fib_by_table_index_or_id (im4, table_id,
                                               IP4_ROUTE_FLAG_TABLE_ID);

      /* fib's created if it doesn't exist */
      ASSERT(fib != 0);

      vec_validate(im4->fib_index_by_sw_if_index, sw_if_index);
      im4->fib_index_by_sw_if_index[sw_if_index] = fib->index;
    }
  else
    {
      ip6_main_t * im6 = &ip6_main;
      ip6_fib_t * fib;
      fib = find_ip6_fib_by_table_index_or_id (im6, table_id,
                                               IP6_ROUTE_FLAG_TABLE_ID);

      /* fib's created if it doesn't exist */
      ASSERT(fib != 0);

      vec_validate(im6->fib_index_by_sw_if_index, sw_if_index);
      im6->fib_index_by_sw_if_index[sw_if_index] = fib->index;
    }
}

#define foreach_l2_lisp_gpe_tx_next     \
  _(DROP, "error-drop")                 \
  _(IP4_LOOKUP, "ip4-lookup")           \
  _(IP6_LOOKUP, "ip6-lookup")           \
  _(LISP_CP_LOOKUP, "lisp-cp-lookup")

typedef enum
{
#define _(sym,str) L2_LISP_GPE_TX_NEXT_##sym,
  foreach_l2_lisp_gpe_tx_next
#undef _
  L2_LISP_GPE_TX_N_NEXT,
} l2_lisp_gpe_tx_next_t;

typedef struct
{
  u32 tunnel_index;
} l2_lisp_gpe_tx_trace_t;

u8 *
format_l2_lisp_gpe_tx_trace (u8 * s, va_list * args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  l2_lisp_gpe_tx_trace_t * t = va_arg (*args, l2_lisp_gpe_tx_trace_t *);

  s = format (s, "L2-LISP-GPE-TX: tunnel %d", t->tunnel_index);
  return s;
}

always_inline void
l2_process_tunnel_action (vlib_buffer_t * b0, u8 action, u32 * next0)
{
  if (LISP_SEND_MAP_REQUEST == action)
    {
      next0[0] = L2_LISP_GPE_TX_NEXT_LISP_CP_LOOKUP;
      vnet_buffer(b0)->lisp.overlay_afi = LISP_AFI_MAC;
    }
  else
    {
      next0[0] = L2_LISP_GPE_TX_NEXT_DROP;
    }
}

always_inline u32
ip_flow_hash (void * data)
{
  ip4_header_t * iph = (ip4_header_t *) data;

  if ((iph->ip_version_and_header_length & 0xF0) == 0x40)
    return ip4_compute_flow_hash (iph, IP_FLOW_HASH_DEFAULT);
  else
    return ip6_compute_flow_hash ((ip6_header_t *) iph, IP_FLOW_HASH_DEFAULT);
}

always_inline u32
l2_flow_hash (vlib_buffer_t * b0)
{
  ethernet_header_t * eh;
  u64 a, b, c;
  uword is_ip, eh_size;
  u16 eh_type;

  eh = vlib_buffer_get_current (b0);
  eh_type = clib_net_to_host_u16(eh->type);
  eh_size = ethernet_buffer_header_size(b0);

  is_ip = (eh_type == ETHERNET_TYPE_IP4 || eh_type == ETHERNET_TYPE_IP6);

  /* since we have 2 cache lines, use them */
  if (is_ip)
    a = ip_flow_hash ((u8 *) vlib_buffer_get_current (b0) + eh_size);
  else
    a = eh->type;

  b = mac_to_u64((u8 *)eh->dst_address);
  c = mac_to_u64((u8 *)eh->src_address);
  hash_mix64 (a, b, c);

  return (u32) c;
}

always_inline void
l2_process_one (lisp_gpe_main_t * lgm, vlib_buffer_t * b0, u32 ti0, u32 * next0)
{
  lisp_gpe_tunnel_t * t0;

  t0 = pool_elt_at_index(lgm->tunnels, ti0);
  ASSERT(0 != t0);

  if (PREDICT_TRUE(LISP_NO_ACTION == t0->action))
    {
      /* compute 'flow' hash */
      if (PREDICT_TRUE(t0->sub_tunnels_lbv_count > 1))
        vnet_buffer(b0)->ip.flow_hash = l2_flow_hash (b0);
      encap_one_inline (lgm, b0, t0, next0);
    }
  else
    {
      l2_process_tunnel_action(b0, t0->action, next0);
    }
}

always_inline void
l2_process_two (lisp_gpe_main_t * lgm, vlib_buffer_t * b0, vlib_buffer_t * b1,
                u32 ti0, u32 ti1, u32 * next0, u32 * next1)
{
  lisp_gpe_tunnel_t * t0, * t1;

  t0 = pool_elt_at_index(lgm->tunnels, ti0);
  t1 = pool_elt_at_index(lgm->tunnels, ti1);

  ASSERT(0 != t0 && 0 != t1);

  if (PREDICT_TRUE(LISP_NO_ACTION == t0->action
                   && LISP_NO_ACTION == t1->action))
    {
      if (PREDICT_TRUE(t0->sub_tunnels_lbv_count > 1))
        vnet_buffer(b0)->ip.flow_hash = l2_flow_hash(b0);
      if (PREDICT_TRUE(t1->sub_tunnels_lbv_count > 1))
        vnet_buffer(b1)->ip.flow_hash = l2_flow_hash(b1);
      encap_two_inline (lgm, b0, b1, t0, t1, next0, next1);
    }
  else
    {
      if (LISP_NO_ACTION == t0->action)
        {
          if (PREDICT_TRUE(t0->sub_tunnels_lbv_count > 1))
            vnet_buffer(b0)->ip.flow_hash = l2_flow_hash(b0);
          encap_one_inline (lgm, b0, t0, next0);
          l2_process_tunnel_action (b1, t1->action, next1);
        }
      else if (LISP_NO_ACTION == t1->action)
        {
          if (PREDICT_TRUE(t1->sub_tunnels_lbv_count > 1))
            vnet_buffer(b1)->ip.flow_hash = l2_flow_hash(b1);
          encap_one_inline (lgm, b1, t1, next1);
          l2_process_tunnel_action (b0, t0->action, next0);
        }
      else
        {
          l2_process_tunnel_action (b0, t0->action, next0);
          l2_process_tunnel_action (b1, t1->action, next1);
        }
    }
}

static uword
l2_lisp_gpe_interface_tx (vlib_main_t * vm, vlib_node_runtime_t * node,
                          vlib_frame_t * from_frame)
{
  u32 n_left_from, next_index, * from, * to_next;
  lisp_gpe_main_t * lgm = &lisp_gpe_main;

  from = vlib_frame_vector_args (from_frame);
  n_left_from = from_frame->n_vectors;

  next_index = node->cached_next_index;

  while (n_left_from > 0)
    {
      u32 n_left_to_next;

      vlib_get_next_frame (vm, node, next_index,
                           to_next, n_left_to_next);

      while (n_left_from >= 4 && n_left_to_next >= 2)
        {
          u32 bi0, bi1;
          vlib_buffer_t * b0, * b1;
          u32 next0, next1, ti0, ti1;
          lisp_gpe_tunnel_t * t0 = 0, * t1 = 0;
          ethernet_header_t * e0, * e1;

          next0 = next1 = L2_LISP_GPE_TX_NEXT_LISP_CP_LOOKUP;

          /* Prefetch next iteration. */
            {
              vlib_buffer_t * p2, *p3;

              p2 = vlib_get_buffer (vm, from[2]);
              p3 = vlib_get_buffer (vm, from[3]);

              vlib_prefetch_buffer_header(p2, LOAD);
              vlib_prefetch_buffer_header(p3, LOAD);

              CLIB_PREFETCH(p2->data, 2*CLIB_CACHE_LINE_BYTES, LOAD);
              CLIB_PREFETCH(p3->data, 2*CLIB_CACHE_LINE_BYTES, LOAD);
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

          e0 = vlib_buffer_get_current (b0);
          e1 = vlib_buffer_get_current (b1);

          /* lookup dst + src mac */
          ti0 = lisp_l2_fib_lookup (lgm, vnet_buffer(b0)->l2.bd_index,
                                    e0->src_address, e0->dst_address);
          ti1 = lisp_l2_fib_lookup (lgm, vnet_buffer(b1)->l2.bd_index,
                                    e1->src_address, e1->dst_address);

          if (PREDICT_TRUE((u32)~0 != ti0) && (u32)~0 != ti1)
            {
              /* process both tunnels */
              l2_process_two (lgm, b0, b1, ti0, ti1, &next0, &next1);
            }
          else
            {
              if ((u32)~0 != ti0)
                {
                  /* process tunnel for b0 */
                  l2_process_one (lgm, b0, ti0, &next0);

                  /* no tunnel found for b1, send to control plane */
                  next1 = L2_LISP_GPE_TX_NEXT_LISP_CP_LOOKUP;
                  vnet_buffer(b1)->lisp.overlay_afi = LISP_AFI_MAC;
                }
              else if ((u32)~0 != ti1)
                {
                  /* process tunnel for b1 */
                  l2_process_one (lgm, b1, ti1, &next1);

                  /* no tunnel found b0, send to control plane */
                  next0 = L2_LISP_GPE_TX_NEXT_LISP_CP_LOOKUP;
                  vnet_buffer(b0)->lisp.overlay_afi = LISP_AFI_MAC;
                }
              else
                {
                  /* no tunnels found */
                  next0 = L2_LISP_GPE_TX_NEXT_LISP_CP_LOOKUP;
                  vnet_buffer(b0)->lisp.overlay_afi = LISP_AFI_MAC;
                  next1 = L2_LISP_GPE_TX_NEXT_LISP_CP_LOOKUP;
                  vnet_buffer(b1)->lisp.overlay_afi = LISP_AFI_MAC;
                }
            }

          if (PREDICT_FALSE(b0->flags & VLIB_BUFFER_IS_TRACED))
            {
              l2_lisp_gpe_tx_trace_t *tr = vlib_add_trace (vm, node, b0,
                                                           sizeof(*tr));
              tr->tunnel_index = t0 - lgm->tunnels;
            }
          if (PREDICT_FALSE(b1->flags & VLIB_BUFFER_IS_TRACED))
            {
              l2_lisp_gpe_tx_trace_t *tr = vlib_add_trace (vm, node, b1,
                                                           sizeof(*tr));
              tr->tunnel_index = t1 - lgm->tunnels;
            }

          vlib_validate_buffer_enqueue_x2(vm, node, next_index, to_next,
                                          n_left_to_next, bi0, bi1, next0,
                                          next1);
        }

      while (n_left_from > 0 && n_left_to_next > 0)
        {
          vlib_buffer_t * b0;
          u32 bi0, ti0, next0 = L2_LISP_GPE_TX_NEXT_LISP_CP_LOOKUP;
          ethernet_header_t * e0;

          bi0 = from[0];
          to_next[0] = bi0;
          from += 1;
          to_next += 1;
          n_left_from -= 1;
          n_left_to_next -= 1;

          b0 = vlib_get_buffer (vm, bi0);
          e0 = vlib_buffer_get_current (b0);

          /* lookup dst + src mac */
          ti0 = lisp_l2_fib_lookup (lgm, vnet_buffer(b0)->l2.bd_index,
                                    e0->src_address, e0->dst_address);

          if (PREDICT_TRUE((u32)~0 != ti0))
            {
              l2_process_one (lgm, b0, ti0, &next0);
            }
          else
            {
              /* no tunnel found send to control plane */
              next0 = L2_LISP_GPE_TX_NEXT_LISP_CP_LOOKUP;
              vnet_buffer(b0)->lisp.overlay_afi = LISP_AFI_MAC;
            }

          if (PREDICT_FALSE(b0->flags & VLIB_BUFFER_IS_TRACED))
            {
              l2_lisp_gpe_tx_trace_t *tr = vlib_add_trace (vm, node, b0,
                                                           sizeof(*tr));
              tr->tunnel_index = ti0 ? ti0 : ~0;
            }
          vlib_validate_buffer_enqueue_x1(vm, node, next_index, to_next,
                                          n_left_to_next, bi0, next0);
        }

      vlib_put_next_frame (vm, node, next_index, n_left_to_next);
    }

  return from_frame->n_vectors;
}

static u8 *
format_l2_lisp_gpe_name (u8 * s, va_list * args)
{
  u32 dev_instance = va_arg (*args, u32);
  return format (s, "l2_lisp_gpe%d", dev_instance);
}

VNET_DEVICE_CLASS (l2_lisp_gpe_device_class,static) = {
  .name = "L2_LISP_GPE",
  .format_device_name = format_l2_lisp_gpe_name,
  .format_tx_trace = format_lisp_gpe_tx_trace,
  .tx_function = l2_lisp_gpe_interface_tx,
  .no_flatten_output_chains = 1,
};


static vnet_hw_interface_t *
create_lisp_gpe_iface (lisp_gpe_main_t * lgm, u32 vni, u32 dp_table,
                       vnet_device_class_t * dev_class,
                       tunnel_lookup_t * tuns)
{
  u32 flen;
  u32 hw_if_index = ~0;
  u8 * new_name;
  vnet_hw_interface_t * hi;
  vnet_main_t * vnm = lgm->vnet_main;

  /* create hw lisp_gpeX iface if needed, otherwise reuse existing */
  flen = vec_len(lgm->free_tunnel_hw_if_indices);
  if (flen > 0)
    {
      hw_if_index = lgm->free_tunnel_hw_if_indices[flen - 1];
      _vec_len(lgm->free_tunnel_hw_if_indices) -= 1;

      hi = vnet_get_hw_interface (vnm, hw_if_index);

      /* rename interface */
      new_name = format (0, "%U", dev_class->format_device_name,
                         vni);

      vec_add1(new_name, 0);
      vnet_rename_interface (vnm, hw_if_index, (char *) new_name);
      vec_free(new_name);

      /* clear old stats of freed interface before reuse */
      vnet_interface_main_t * im = &vnm->interface_main;
      vnet_interface_counter_lock (im);
      vlib_zero_combined_counter (
          &im->combined_sw_if_counters[VNET_INTERFACE_COUNTER_TX],
          hi->sw_if_index);
      vlib_zero_combined_counter (
          &im->combined_sw_if_counters[VNET_INTERFACE_COUNTER_RX],
          hi->sw_if_index);
      vlib_zero_simple_counter (
          &im->sw_if_counters[VNET_INTERFACE_COUNTER_DROP],
          hi->sw_if_index);
      vnet_interface_counter_unlock (im);
    }
  else
    {
      hw_if_index = vnet_register_interface (vnm, dev_class->index, vni,
                                             lisp_gpe_hw_class.index, 0);
      hi = vnet_get_hw_interface (vnm, hw_if_index);
    }

  hash_set(tuns->hw_if_index_by_dp_table, dp_table, hw_if_index);

  /* set tunnel termination: post decap, packets are tagged as having been
   * originated by lisp-gpe interface */
  hash_set(tuns->sw_if_index_by_vni, vni, hi->sw_if_index);
  hash_set(tuns->vni_by_sw_if_index, hi->sw_if_index, vni);

  return hi;
}

static void
remove_lisp_gpe_iface (lisp_gpe_main_t * lgm, u32 hi_index, u32 dp_table,
                       tunnel_lookup_t * tuns)
{
  vnet_main_t * vnm = lgm->vnet_main;
  vnet_hw_interface_t * hi;
  uword * vnip;

  hi = vnet_get_hw_interface (vnm, hi_index);

  /* disable interface */
  vnet_sw_interface_set_flags (vnm, hi->sw_if_index, 0/* down */);
  vnet_hw_interface_set_flags (vnm, hi->hw_if_index, 0/* down */);
  hash_unset(tuns->hw_if_index_by_dp_table, dp_table);
  vec_add1(lgm->free_tunnel_hw_if_indices, hi->hw_if_index);

  /* clean tunnel termination and vni to sw_if_index binding */
  vnip = hash_get(tuns->vni_by_sw_if_index, hi->sw_if_index);
  if (0 == vnip)
    {
      clib_warning ("No vni associated to interface %d", hi->sw_if_index);
      return;
    }
  hash_unset(tuns->sw_if_index_by_vni, vnip[0]);
  hash_unset(tuns->vni_by_sw_if_index, hi->sw_if_index);
}

static int
lisp_gpe_add_del_l3_iface (lisp_gpe_main_t * lgm,
                           vnet_lisp_gpe_add_del_iface_args_t * a)
{
  vnet_main_t * vnm = lgm->vnet_main;
  tunnel_lookup_t * l3_ifaces = &lgm->l3_ifaces;
  vnet_hw_interface_t * hi;
  u32 lookup_next_index4, lookup_next_index6;
  uword * hip, * si;

  hip = hash_get(l3_ifaces->hw_if_index_by_dp_table, a->table_id);

  if (a->is_add)
    {
      if (hip)
        {
          clib_warning ("vrf %d already mapped to a vni", a->table_id);
          return -1;
        }

      si = hash_get(l3_ifaces->sw_if_index_by_vni, a->vni);
      if (si)
        {
          clib_warning ("Interface for vni %d already exists", a->vni);
          return -1;
        }

      /* create lisp iface and populate tunnel tables */
      hi = create_lisp_gpe_iface (lgm, a->vni, a->table_id,
                                  &lisp_gpe_device_class, l3_ifaces);

      /* set ingress arc from lgpe_ipX_lookup */
      lookup_next_index4 = vlib_node_add_next (lgm->vlib_main,
                                               lgpe_ip4_lookup_node.index,
                                               hi->output_node_index);
      lookup_next_index6 = vlib_node_add_next (lgm->vlib_main,
                                               lgpe_ip6_lookup_node.index,
                                               hi->output_node_index);
      hash_set(lgm->lgpe_ip4_lookup_next_index_by_table_id, a->table_id,
               lookup_next_index4);
      hash_set(lgm->lgpe_ip6_lookup_next_index_by_table_id, a->table_id,
               lookup_next_index6);

      /* insert default routes that point to lgpe-ipx-lookup */
      add_del_lisp_gpe_default_route (a->table_id, /* is_v4 */1, 1);
      add_del_lisp_gpe_default_route (a->table_id, /* is_v4 */0, 1);

      /* set egress arcs */
#define _(sym,str) vlib_node_add_named_next_with_slot (vnm->vlib_main, \
                    hi->tx_node_index, str, LISP_GPE_TX_NEXT_##sym);
          foreach_lisp_gpe_tx_next
#undef _

      /* set interface in appropriate v4 and v6 FIBs */
      lisp_gpe_iface_set_table (hi->sw_if_index, a->table_id, 1);
      lisp_gpe_iface_set_table (hi->sw_if_index, a->table_id, 0);

      /* enable interface */
      vnet_sw_interface_set_flags (vnm, hi->sw_if_index,
                                   VNET_SW_INTERFACE_FLAG_ADMIN_UP);
      vnet_hw_interface_set_flags (vnm, hi->hw_if_index,
                                   VNET_HW_INTERFACE_FLAG_LINK_UP);
    }
  else
    {
      if (hip == 0)
        {
          clib_warning("The interface for vrf %d doesn't exist", a->table_id);
          return -1;
        }

      remove_lisp_gpe_iface (lgm, hip[0], a->table_id, &lgm->l3_ifaces);

      /* unset default routes */
      add_del_lisp_gpe_default_route (a->table_id, /* is_v4 */1, 0);
      add_del_lisp_gpe_default_route (a->table_id, /* is_v4 */0, 0);
    }

  return 0;
}

static int
lisp_gpe_add_del_l2_iface (lisp_gpe_main_t * lgm,
                           vnet_lisp_gpe_add_del_iface_args_t * a)
{
  vnet_main_t * vnm = lgm->vnet_main;
  tunnel_lookup_t * l2_ifaces = &lgm->l2_ifaces;
  vnet_hw_interface_t * hi;
  uword * hip, * si;
  u16 bd_index;

  bd_index = bd_find_or_add_bd_index(&bd_main, a->bd_id);
  hip = hash_get(l2_ifaces->hw_if_index_by_dp_table, bd_index);

  if (a->is_add)
    {
      if (hip)
        {
          clib_warning("bridge domain %d already mapped to a vni", a->bd_id);
          return -1;
        }

      si = hash_get(l2_ifaces->sw_if_index_by_vni, a->vni);
      if (si)
        {
          clib_warning ("Interface for vni %d already exists", a->vni);
          return -1;
        }

      /* create lisp iface and populate tunnel tables */
      hi = create_lisp_gpe_iface (lgm, a->vni, bd_index,
                                  &l2_lisp_gpe_device_class, &lgm->l2_ifaces);

      /* add iface to l2 bridge domain */
      set_int_l2_mode (lgm->vlib_main, vnm, MODE_L2_BRIDGE, hi->sw_if_index,
                       bd_index, 0, 0, 0);

      /* set egress arcs */
#define _(sym,str) vlib_node_add_named_next_with_slot (vnm->vlib_main, \
                    hi->tx_node_index, str, L2_LISP_GPE_TX_NEXT_##sym);
          foreach_l2_lisp_gpe_tx_next
#undef _

      /* enable interface */
      vnet_sw_interface_set_flags (vnm, hi->sw_if_index,
                                   VNET_SW_INTERFACE_FLAG_ADMIN_UP);
      vnet_hw_interface_set_flags (vnm, hi->hw_if_index,
                                   VNET_HW_INTERFACE_FLAG_LINK_UP);
    }
  else
    {
      if (hip == 0)
        {
          clib_warning("The interface for bridge domain %d doesn't exist",
                       a->bd_id);
          return -1;
        }
      remove_lisp_gpe_iface (lgm, hip[0], bd_index, &lgm->l2_ifaces);
    }

  return 0;
}

int
vnet_lisp_gpe_add_del_iface (vnet_lisp_gpe_add_del_iface_args_t * a,
                             u32 * hw_if_indexp)
{
  lisp_gpe_main_t * lgm = &lisp_gpe_main;

  if (vnet_lisp_gpe_enable_disable_status() == 0)
    {
      clib_warning ("LISP is disabled!");
      return VNET_API_ERROR_LISP_DISABLED;
    }

  if (!a->is_l2)
    return lisp_gpe_add_del_l3_iface (lgm, a);
  else
    return lisp_gpe_add_del_l2_iface (lgm, a);
}

static clib_error_t *
lisp_gpe_add_del_iface_command_fn (vlib_main_t * vm, unformat_input_t * input,
                                   vlib_cli_command_t * cmd)
{
  unformat_input_t _line_input, * line_input = &_line_input;
  u8 is_add = 1;
  clib_error_t * error = 0;
  int rv = 0;
  u32 table_id, vni, bd_id;
  u8 vni_is_set = 0, vrf_is_set = 0, bd_index_is_set = 0;

  vnet_lisp_gpe_add_del_iface_args_t _a, * a = &_a;

  /* Get a line of input. */
  if (! unformat_user (input, unformat_line_input, line_input))
    return 0;

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "add"))
        is_add = 1;
      else if (unformat (line_input, "del"))
        is_add = 0;
      else if (unformat (line_input, "vrf %d", &table_id))
        {
          vrf_is_set = 1;
        }
      else if (unformat (line_input, "vni %d", &vni))
        {
          vni_is_set = 1;
        }
      else if (unformat (line_input, "bd %d", &bd_id))
        {
          bd_index_is_set = 1;
        }
      else
        {
          return clib_error_return (0, "parse error: '%U'",
                                   format_unformat_error, line_input);
        }
    }

  if (vrf_is_set && bd_index_is_set)
    return clib_error_return(0, "Cannot set both vrf and brdige domain index!");

  if (!vni_is_set)
    return clib_error_return(0, "vni must be set!");

  if (!vrf_is_set && !bd_index_is_set)
    return clib_error_return(0, "vrf or bridge domain index must be set!");

  a->is_add = is_add;
  a->dp_table = vrf_is_set ? table_id : bd_id;
  a->vni = vni;
  a->is_l2 = bd_index_is_set;

  rv = vnet_lisp_gpe_add_del_iface (a, 0);
  if (0 != rv)
    {
      error = clib_error_return(0, "failed to %s gpe iface!",
                                is_add ? "add" : "delete");
    }

  return error;
}

VLIB_CLI_COMMAND (add_del_lisp_gpe_iface_command, static) = {
  .path = "lisp gpe iface",
  .short_help = "lisp gpe iface add/del vni <vni> vrf <vrf>",
  .function = lisp_gpe_add_del_iface_command_fn,
};
