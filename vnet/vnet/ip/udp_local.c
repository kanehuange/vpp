/*
 * node.c: udp packet processing
 *
 * Copyright (c) 2013 Cisco and/or its affiliates.
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
#include <vnet/ip/udp.h>
#include <vnet/ip/udp_packet.h>
#include <vppinfra/sparse_vec.h>

udp_main_t udp_main;

#define foreach_udp_input_next			\
  _ (PUNT, "error-punt")			\
  _ (DROP, "error-drop")

typedef enum {
#define _(s,n) UDP_INPUT_NEXT_##s,
  foreach_udp_input_next
#undef _
  UDP_INPUT_N_NEXT,
} udp_input_next_t;

typedef struct {
  u16 src_port;
  u16 dst_port;
} udp_rx_trace_t;

u8 * format_udp_rx_trace (u8 * s, va_list * args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  udp_rx_trace_t * t = va_arg (*args, udp_rx_trace_t *);
    
  s = format (s, "UDP: src-port %d dst-port %d",
              clib_net_to_host_u16(t->src_port),
              clib_net_to_host_u16(t->dst_port));
  return s;
}

typedef struct {
  /* Sparse vector mapping udp dst_port in network byte order
     to next index. */
  u16 * next_by_dst_port;

  u32 * sparse_index_by_next_index;
} udp_input_runtime_t;

vlib_node_registration_t udp4_input_node;
vlib_node_registration_t udp6_input_node;

always_inline uword
udp46_input_inline (vlib_main_t * vm,
                    vlib_node_runtime_t * node,
                    vlib_frame_t * from_frame,
                    int is_ip4)
{
  udp_input_runtime_t * rt = is_ip4 ?
    (void *) vlib_node_get_runtime_data (vm, udp4_input_node.index)
    : (void *) vlib_node_get_runtime_data (vm, udp6_input_node.index);
  __attribute__((unused)) u32 n_left_from, next_index, i_next, * from, * to_next;

  from = vlib_frame_vector_args (from_frame);
  n_left_from = from_frame->n_vectors;

  next_index = node->cached_next_index;
  i_next = vec_elt (rt->sparse_index_by_next_index, next_index);

  while (n_left_from > 0)
    {
      u32 n_left_to_next;

      vlib_get_next_frame (vm, node, next_index,
			   to_next, n_left_to_next);

      while (n_left_from >= 4 && n_left_to_next >= 2)
	{
	  u32 bi0, bi1;
	  vlib_buffer_t * b0, * b1;
	  udp_header_t * h0 = 0, * h1 = 0;
	  u32 i0, i1, dst_port0, dst_port1;
          u32 advance0, advance1;
          u32 error0, next0, error1, next1;

	  /* Prefetch next iteration. */
	  {
	    vlib_buffer_t * p2, * p3;

	    p2 = vlib_get_buffer (vm, from[2]);
	    p3 = vlib_get_buffer (vm, from[3]);

	    vlib_prefetch_buffer_header (p2, LOAD);
	    vlib_prefetch_buffer_header (p3, LOAD);

	    CLIB_PREFETCH (p2->data, sizeof (h0[0]), LOAD);
	    CLIB_PREFETCH (p3->data, sizeof (h1[0]), LOAD);
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

          /* ip4/6_local hands us the ip header, not the udp header */
          if (is_ip4) 
            {
              advance0 = sizeof(ip4_header_t);
              advance1 = sizeof(ip4_header_t);
            }
          else
            {
              advance0 = sizeof(ip6_header_t);
              advance1 = sizeof(ip6_header_t);
            }          

          if (PREDICT_FALSE(b0->current_length < advance0 + sizeof (*h0)))
            {
              error0 = UDP_ERROR_LENGTH_ERROR;
              next0 = UDP_INPUT_NEXT_DROP;
            }
          else
            {
              vlib_buffer_advance (b0, advance0);
              h0 = vlib_buffer_get_current (b0);
              error0 = next0 = 0;
	      if (PREDICT_FALSE(clib_net_to_host_u16(h0->length) >
				vlib_buffer_length_in_chain(vm, b0)))
	        {
		  error0 = UDP_ERROR_LENGTH_ERROR;
		  next0 = UDP_INPUT_NEXT_DROP;
	        }
            }

          if (PREDICT_FALSE(b1->current_length < advance1 + sizeof (*h1)))
            {
              error1 = UDP_ERROR_LENGTH_ERROR;
              next1 = UDP_INPUT_NEXT_DROP;
            }
          else
            {
              vlib_buffer_advance (b1, advance1);
              h1 = vlib_buffer_get_current (b1);
              error1 = next1 = 0;
	      if (PREDICT_FALSE(clib_net_to_host_u16(h1->length) >
			    vlib_buffer_length_in_chain(vm, b1)))
	        {
		  error1 = UDP_ERROR_LENGTH_ERROR;
		  next1 = UDP_INPUT_NEXT_DROP;
	        }
            }

	  /* Index sparse array with network byte order. */
	  dst_port0 = (error0 == 0) ? h0->dst_port : 0;
	  dst_port1 = (error1 == 0) ? h1->dst_port : 0;
	  sparse_vec_index2 (rt->next_by_dst_port, dst_port0, dst_port1,
                             &i0, &i1);
          next0 = (error0 == 0) ? vec_elt(rt->next_by_dst_port, i0) : next0;
          next1 = (error1 == 0) ? vec_elt(rt->next_by_dst_port, i1) : next1;

	  b0->error = node->errors[next0 == SPARSE_VEC_INVALID_INDEX ? 
				   UDP_ERROR_NO_LISTENER : error0];
	  b1->error = node->errors[next1 == SPARSE_VEC_INVALID_INDEX ? 
				   UDP_ERROR_NO_LISTENER : error1];
          
          if (PREDICT_FALSE(b0->flags & VLIB_BUFFER_IS_TRACED)) 
            {
              udp_rx_trace_t *tr = vlib_add_trace (vm, node, 
                                                   b0, sizeof (*tr));
              if (b0->error != node->errors[UDP_ERROR_LENGTH_ERROR])
                {
                  tr->src_port = h0->src_port;
                  tr->dst_port = h0->dst_port;
                }
            }
          if (PREDICT_FALSE(b1->flags & VLIB_BUFFER_IS_TRACED)) 
            {
              udp_rx_trace_t *tr = vlib_add_trace (vm, node, 
                                                   b1, sizeof (*tr));
              if (b1->error != node->errors[UDP_ERROR_LENGTH_ERROR])
                {
                  tr->src_port = h1->src_port;
                  tr->dst_port = h1->dst_port;
                }
            }

          vlib_buffer_advance (b0, sizeof (*h0));
          vlib_buffer_advance (b1, sizeof (*h1));

	  vlib_validate_buffer_enqueue_x2 (vm, node, next_index,
					   to_next, n_left_to_next,
					   bi0, bi1, next0, next1);
	}
    
      while (n_left_from > 0 && n_left_to_next > 0)
	{
	  u32 bi0;
	  vlib_buffer_t * b0;
	  udp_header_t * h0 = 0;
	  u32 i0, next0;
          u32 advance0;

	  bi0 = from[0];
	  to_next[0] = bi0;
	  from += 1;
	  to_next += 1;
	  n_left_from -= 1;
	  n_left_to_next -= 1;

	  b0 = vlib_get_buffer (vm, bi0);

          /* ip4/6_local hands us the ip header, not the udp header */
          if (is_ip4) 
            advance0 = sizeof(ip4_header_t);
          else
            advance0 = sizeof(ip6_header_t);

          if (PREDICT_FALSE(b0->current_length < advance0 + sizeof (*h0)))
            {
              b0->error = node->errors[UDP_ERROR_LENGTH_ERROR];
              next0 = UDP_INPUT_NEXT_DROP;
              goto trace_x1;
            }

          vlib_buffer_advance (b0, advance0);

	  h0 = vlib_buffer_get_current (b0);

          if (PREDICT_TRUE(clib_net_to_host_u16(h0->length) <= 
			   vlib_buffer_length_in_chain(vm, b0)))
            {
              i0 = sparse_vec_index (rt->next_by_dst_port, h0->dst_port);
              next0 = vec_elt(rt->next_by_dst_port, i0);

              b0->error = node->errors [next0 == SPARSE_VEC_INVALID_INDEX ? UDP_ERROR_NO_LISTENER : UDP_ERROR_NONE];
            }
          else
            {
              b0->error = node->errors[UDP_ERROR_LENGTH_ERROR];
              next0 = UDP_INPUT_NEXT_DROP;
            }

        trace_x1:
          if (PREDICT_FALSE(b0->flags & VLIB_BUFFER_IS_TRACED)) 
            {
              udp_rx_trace_t *tr = vlib_add_trace (vm, node, 
                                                   b0, sizeof (*tr));
              if (b0->error != node->errors[UDP_ERROR_LENGTH_ERROR])
                {
                  tr->src_port = h0->src_port;
                  tr->dst_port = h0->dst_port;
                }
            }
          vlib_buffer_advance (b0, sizeof (*h0));
	  vlib_validate_buffer_enqueue_x1 (vm, node, next_index,
					   to_next, n_left_to_next,
					   bi0, next0);
	}

      vlib_put_next_frame (vm, node, next_index, n_left_to_next);
    }
  return from_frame->n_vectors;
}

static char * udp_error_strings[] = {
#define udp_error(n,s) s,
#include "udp_error.def"
#undef udp_error
};

static uword
udp4_input (vlib_main_t * vm,
            vlib_node_runtime_t * node,
            vlib_frame_t * from_frame)
{
  return udp46_input_inline (vm, node, from_frame, 1 /* is_ip4 */);
}

static uword
udp6_input (vlib_main_t * vm,
            vlib_node_runtime_t * node,
            vlib_frame_t * from_frame)
{
  return udp46_input_inline (vm, node, from_frame, 0 /* is_ip4 */);
}


VLIB_REGISTER_NODE (udp4_input_node) = {
  .function = udp4_input,
  .name = "ip4-udp-lookup",
  /* Takes a vector of packets. */
  .vector_size = sizeof (u32),

  .runtime_data_bytes = sizeof (udp_input_runtime_t),

  .n_errors = UDP_N_ERROR,
  .error_strings = udp_error_strings,

  .n_next_nodes = UDP_INPUT_N_NEXT,
  .next_nodes = {
#define _(s,n) [UDP_INPUT_NEXT_##s] = n,
    foreach_udp_input_next
#undef _
  },

  .format_buffer = format_udp_header,
  .format_trace = format_udp_rx_trace,
  .unformat_buffer = unformat_udp_header,
};

VLIB_NODE_FUNCTION_MULTIARCH (udp4_input_node, udp4_input)

VLIB_REGISTER_NODE (udp6_input_node) = {
  .function = udp6_input,
  .name = "ip6-udp-lookup",
  /* Takes a vector of packets. */
  .vector_size = sizeof (u32),

  .runtime_data_bytes = sizeof (udp_input_runtime_t),

  .n_errors = UDP_N_ERROR,
  .error_strings = udp_error_strings,

  .n_next_nodes = UDP_INPUT_N_NEXT,
  .next_nodes = {
#define _(s,n) [UDP_INPUT_NEXT_##s] = n,
    foreach_udp_input_next
#undef _
  },

  .format_buffer = format_udp_header,
  .format_trace = format_udp_rx_trace,
  .unformat_buffer = unformat_udp_header,
};

VLIB_NODE_FUNCTION_MULTIARCH (udp6_input_node, udp6_input)

static void add_dst_port (udp_main_t * um,
                          udp_dst_port_t dst_port,
			  char * dst_port_name, u8 is_ip4)
{
  udp_dst_port_info_t * pi;
  u32 i;

  vec_add2 (um->dst_port_infos[is_ip4], pi, 1);
  i = pi - um->dst_port_infos[is_ip4];

  pi->name = dst_port_name;
  pi->dst_port = dst_port;
  pi->next_index = pi->node_index = ~0;
  
  hash_set (um->dst_port_info_by_dst_port[is_ip4], dst_port, i);

  if (pi->name)
    hash_set_mem (um->dst_port_info_by_name[is_ip4], pi->name, i);
}

void
udp_register_dst_port (vlib_main_t * vm,
                       udp_dst_port_t dst_port,
                       u32 node_index, u8 is_ip4)
{
  udp_main_t * um = &udp_main;
  udp_dst_port_info_t * pi;
  udp_input_runtime_t * rt;
  u16 * n;
  u32 i;

  {
    clib_error_t * error = vlib_call_init_function (vm, udp_local_init);
    if (error)
      clib_error_report (error);
  }

  pi = udp_get_dst_port_info (um, dst_port, is_ip4);
  if (! pi) 
    {
      add_dst_port (um, dst_port, 0, is_ip4);
      pi = udp_get_dst_port_info (um, dst_port, is_ip4);
      ASSERT (pi);
    }
      
  pi->node_index = node_index;
  pi->next_index = vlib_node_add_next (vm, 
                                       is_ip4 ? udp4_input_node.index
                                       : udp6_input_node.index,
				       node_index);

  /* Setup udp protocol -> next index sparse vector mapping. */
  rt = vlib_node_get_runtime_data 
    (vm, is_ip4 ? udp4_input_node.index: udp6_input_node.index);
  n = sparse_vec_validate (rt->next_by_dst_port, 
                           clib_host_to_net_u16 (dst_port));
  n[0] = pi->next_index;

  /* Rebuild next index -> sparse index inverse mapping when sparse vector
     is updated. */
  vec_validate (rt->sparse_index_by_next_index, pi->next_index);
  for (i = 1; i < vec_len (rt->next_by_dst_port); i++)
    rt->sparse_index_by_next_index[rt->next_by_dst_port[i]] = i;
}

/* Parse a UDP header. */
uword unformat_udp_header (unformat_input_t * input, va_list * args)
{
  u8 ** result = va_arg (*args, u8 **);
  udp_header_t * udp;
  __attribute__((unused)) int old_length;
  u16 src_port, dst_port;

  /* Allocate space for IP header. */
  {
    void * p;

    old_length = vec_len (*result);
    vec_add2 (*result, p, sizeof (ip4_header_t));
    udp = p;
  }

  memset (udp, 0, sizeof (udp[0]));
  if (unformat (input, "src-port %d dst-port %d", 
                &src_port, &dst_port)) 
    {
      udp->src_port = clib_host_to_net_u16 (src_port);
      udp->dst_port = clib_host_to_net_u16 (dst_port);
      return 1;
    }
  return 0;
}

static void
udp_setup_node (vlib_main_t * vm, u32 node_index)
{
  vlib_node_t * n = vlib_get_node (vm, node_index);
  pg_node_t * pn = pg_get_node (node_index);

  n->format_buffer = format_udp_header;
  n->unformat_buffer = unformat_udp_header;
  pn->unformat_edit = unformat_pg_udp_header;
}

clib_error_t * udp_local_init (vlib_main_t * vm)
{
  udp_input_runtime_t * rt;
  udp_main_t * um = &udp_main;
  int i;

  {
    clib_error_t * error; 
    error = vlib_call_init_function (vm, udp_init);
    if (error)
      clib_error_report (error);
  }


  for (i = 0; i < 2; i++)
    {
      um->dst_port_info_by_name[i] = hash_create_string (0, sizeof(uword));
      um->dst_port_info_by_dst_port[i] = hash_create (0, sizeof(uword));
    }

  udp_setup_node (vm, udp4_input_node.index);
  udp_setup_node (vm, udp6_input_node.index);

  rt = vlib_node_get_runtime_data (vm, udp4_input_node.index);

  rt->next_by_dst_port = sparse_vec_new
    (/* elt bytes */ sizeof (rt->next_by_dst_port[0]),
     /* bits in index */ BITS (((udp_header_t *) 0)->dst_port));

  vec_validate (rt->sparse_index_by_next_index, UDP_INPUT_NEXT_DROP);
  vec_validate (rt->sparse_index_by_next_index, UDP_INPUT_NEXT_PUNT);
  rt->sparse_index_by_next_index[UDP_INPUT_NEXT_DROP]
    = SPARSE_VEC_INVALID_INDEX;
  rt->sparse_index_by_next_index[UDP_INPUT_NEXT_PUNT]
    = SPARSE_VEC_INVALID_INDEX;

#define _(n,s) add_dst_port (um, UDP_DST_PORT_##s, #s, 1 /* is_ip4 */);
  foreach_udp4_dst_port
#undef _

  rt = vlib_node_get_runtime_data (vm, udp6_input_node.index);

  rt->next_by_dst_port = sparse_vec_new
    (/* elt bytes */ sizeof (rt->next_by_dst_port[0]),
     /* bits in index */ BITS (((udp_header_t *) 0)->dst_port));

  vec_validate (rt->sparse_index_by_next_index, UDP_INPUT_NEXT_DROP);
  vec_validate (rt->sparse_index_by_next_index, UDP_INPUT_NEXT_PUNT);
  rt->sparse_index_by_next_index[UDP_INPUT_NEXT_DROP]
    = SPARSE_VEC_INVALID_INDEX;
  rt->sparse_index_by_next_index[UDP_INPUT_NEXT_PUNT]
    = SPARSE_VEC_INVALID_INDEX;

#define _(n,s) add_dst_port (um, UDP_DST_PORT_##s, #s, 0 /* is_ip4 */);
  foreach_udp6_dst_port
#undef _

  ip4_register_protocol (IP_PROTOCOL_UDP, udp4_input_node.index);
  /* Note: ip6 differs from ip4, UDP is hotwired to ip6-udp-lookup */
  return 0;
}

VLIB_INIT_FUNCTION (udp_local_init);
