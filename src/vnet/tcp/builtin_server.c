/*
* Copyright (c) 2015-2017 Cisco and/or its affiliates.
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

#include <vnet/vnet.h>
#include <vlibmemory/api.h>
#include <vnet/session/application.h>
#include <vnet/session/application_interface.h>

typedef struct
{
  u8 *rx_buf;
  unix_shared_memory_queue_t **vpp_queue;
  u32 byte_index;
  vlib_main_t *vlib_main;
} builtin_server_main_t;

builtin_server_main_t builtin_server_main;


int
builtin_session_accept_callback (stream_session_t * s)
{
  builtin_server_main_t *bsm = &builtin_server_main;
  clib_warning ("called...");

  bsm->vpp_queue[s->thread_index] =
    session_manager_get_vpp_event_queue (s->thread_index);
  s->session_state = SESSION_STATE_READY;
  bsm->byte_index = 0;
  return 0;
}

void
builtin_session_disconnect_callback (stream_session_t * s)
{
  clib_warning ("called...");

  vnet_disconnect_session (s->session_index, s->thread_index);
}

void
builtin_session_reset_callback (stream_session_t * s)
{
  clib_warning ("called.. ");

  stream_session_cleanup (s);
}


int
builtin_session_connected_callback (u32 client_index,
				    stream_session_t * s, u8 is_fail)
{
  clib_warning ("called...");
  return -1;
}

int
builtin_add_segment_callback (u32 client_index,
			      const u8 * seg_name, u32 seg_size)
{
  clib_warning ("called...");
  return -1;
}

int
builtin_redirect_connect_callback (u32 client_index, void *mp)
{
  clib_warning ("called...");
  return -1;
}

void
test_bytes (builtin_server_main_t * bsm, int actual_transfer)
{
  int i;

  for (i = 0; i < actual_transfer; i++)
    {
      if (bsm->rx_buf[i] != ((bsm->byte_index + i) & 0xff))
	{
	  clib_warning ("at %d expected %d got %d", bsm->byte_index + i,
			(bsm->byte_index + i) & 0xff, bsm->rx_buf[i]);
	}
    }
  bsm->byte_index += actual_transfer;
}

int
builtin_server_rx_callback (stream_session_t * s)
{
  u32 n_written, max_dequeue, max_enqueue, max_transfer;
  int actual_transfer;
  svm_fifo_t *tx_fifo, *rx_fifo;
  builtin_server_main_t *bsm = &builtin_server_main;
  session_fifo_event_t evt;
  static int serial_number = 0;

  max_dequeue = svm_fifo_max_dequeue (s->server_rx_fifo);
  max_enqueue = svm_fifo_max_enqueue (s->server_tx_fifo);

  if (PREDICT_FALSE (max_dequeue == 0))
    {
      return 0;
    }

  tx_fifo = s->server_tx_fifo;
  rx_fifo = s->server_rx_fifo;

  /* Number of bytes we're going to copy */
  max_transfer = (max_dequeue < max_enqueue) ? max_dequeue : max_enqueue;

  /* No space in tx fifo */
  if (PREDICT_FALSE (max_transfer == 0))
    {
      /* XXX timeout for session that are stuck */

    rx_event:
      /* Program self-tap to retry */
      if (svm_fifo_set_event (rx_fifo))
	{
	  evt.fifo = rx_fifo;
	  evt.event_type = FIFO_EVENT_BUILTIN_RX;
	  evt.event_id = 0;
	  unix_shared_memory_queue_add (bsm->vpp_queue[s->thread_index],
					(u8 *) & evt,
					0 /* do wait for mutex */ );
	}

      return 0;
    }

  svm_fifo_unset_event (rx_fifo);

  vec_validate (bsm->rx_buf, max_transfer - 1);
  _vec_len (bsm->rx_buf) = max_transfer;

  actual_transfer = svm_fifo_dequeue_nowait (rx_fifo, 0, max_transfer,
					     bsm->rx_buf);
  ASSERT (actual_transfer == max_transfer);

//  test_bytes (bsm, actual_transfer);

  /*
   * Echo back
   */

  n_written =
    svm_fifo_enqueue_nowait (tx_fifo, 0, actual_transfer, bsm->rx_buf);

  if (n_written != max_transfer)
    clib_warning ("short trout!");

  if (svm_fifo_set_event (tx_fifo))
    {
      /* Fabricate TX event, send to vpp */
      evt.fifo = tx_fifo;
      evt.event_type = FIFO_EVENT_SERVER_TX;
      evt.event_id = serial_number++;

      unix_shared_memory_queue_add (bsm->vpp_queue[s->thread_index],
				    (u8 *) & evt, 0 /* do wait for mutex */ );
    }

  if (PREDICT_FALSE (max_enqueue < max_dequeue))
    goto rx_event;

  return 0;
}

static session_cb_vft_t builtin_session_cb_vft = {
  .session_accept_callback = builtin_session_accept_callback,
  .session_disconnect_callback = builtin_session_disconnect_callback,
  .session_connected_callback = builtin_session_connected_callback,
  .add_segment_callback = builtin_add_segment_callback,
  .redirect_connect_callback = builtin_redirect_connect_callback,
  .builtin_server_rx_callback = builtin_server_rx_callback,
  .session_reset_callback = builtin_session_reset_callback
};

static int
server_create (vlib_main_t * vm)
{
  vnet_bind_args_t _a, *a = &_a;
  u64 options[SESSION_OPTIONS_N_OPTIONS];
  char segment_name[128];
  u32 num_threads;
  vlib_thread_main_t *vtm = vlib_get_thread_main ();

  num_threads = 1 /* main thread */  + vtm->n_threads;
  vec_validate (builtin_server_main.vpp_queue, num_threads - 1);

  memset (a, 0, sizeof (*a));
  memset (options, 0, sizeof (options));

  a->uri = "tcp://0.0.0.0/1234";
  a->api_client_index = ~0;
  a->session_cb_vft = &builtin_session_cb_vft;
  a->options = options;
  a->options[SESSION_OPTIONS_SEGMENT_SIZE] = 128 << 20;
  a->options[SESSION_OPTIONS_RX_FIFO_SIZE] = 1 << 16;
  a->options[SESSION_OPTIONS_TX_FIFO_SIZE] = 1 << 16;
  a->segment_name = segment_name;
  a->segment_name_length = ARRAY_LEN (segment_name);

  return vnet_bind_uri (a);
}

static clib_error_t *
server_create_command_fn (vlib_main_t * vm,
			  unformat_input_t * input, vlib_cli_command_t * cmd)
{
  int rv;
#if 0
  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "whatever %d", &whatever))
	;
      else
	return clib_error_return (0, "unknown input `%U'",
				  format_unformat_error, input);
    }
#endif

  vnet_session_enable_disable (vm, 1 /* turn on TCP, etc. */ );
  rv = server_create (vm);
  switch (rv)
    {
    case 0:
      break;
    default:
      return clib_error_return (0, "server_create returned %d", rv);
    }
  return 0;
}

/* *INDENT-OFF* */
VLIB_CLI_COMMAND (server_create_command, static) =
{
  .path = "test server",
  .short_help = "test server",
  .function = server_create_command_fn,
};
/* *INDENT-ON* */

/*
* fd.io coding-style-patch-verification: ON
*
* Local Variables:
* eval: (c-set-style "gnu")
* End:
*/
