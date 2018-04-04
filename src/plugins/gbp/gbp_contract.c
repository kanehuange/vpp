/*
 * gbp.h : Group Based Policy
 *
 * Copyright (c) 2018 Cisco and/or its affiliates.
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

#include <plugins/gbp/gbp.h>

/**
 * Single contract DB instance
 */
gbp_contract_db_t gbp_contract_db;

void
gbp_contract_update (epg_id_t src_epg, epg_id_t dst_epg, u32 acl_index)
{
  gbp_contract_key_t key = {
    .gck_src = src_epg,
    .gck_dst = dst_epg,
  };

  hash_set (gbp_contract_db.gc_hash, key.as_u64, acl_index);
}

void
gbp_contract_delete (epg_id_t src_epg, epg_id_t dst_epg)
{
  gbp_contract_key_t key = {
    .gck_src = src_epg,
    .gck_dst = dst_epg,
  };

  hash_unset (gbp_contract_db.gc_hash, key.as_u64);
}

void
gbp_contract_walk (gbp_contract_cb_t cb, void *ctx)
{
  gbp_contract_key_t key;
  u32 acl_index;

  /* *INDENT-OFF* */
  hash_foreach(key.as_u64, acl_index, gbp_contract_db.gc_hash,
  ({
    gbp_contract_t gbpc = {
      .gc_key = key,
      .gc_acl_index = acl_index,
    };

    if (!cb(&gbpc, ctx))
      break;
  }));
  /* *INDENT-ON* */
}

static clib_error_t *
gbp_contract_cli (vlib_main_t * vm,
		  unformat_input_t * input, vlib_cli_command_t * cmd)
{
  epg_id_t src_epg_id = EPG_INVALID, dst_epg_id = EPG_INVALID;
  u32 acl_index = ~0;
  u8 add = 1;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "add"))
	add = 1;
      else if (unformat (input, "del"))
	add = 0;
      else if (unformat (input, "src-epg %d", &src_epg_id))
	;
      else if (unformat (input, "dst-epg %d", &dst_epg_id))
	;
      else if (unformat (input, "acl-index %d", &acl_index))
	;
      else
	break;
    }

  if (EPG_INVALID == src_epg_id)
    return clib_error_return (0, "Source EPG-ID must be specified");
  if (EPG_INVALID == dst_epg_id)
    return clib_error_return (0, "Destination EPG-ID must be specified");

  if (add)
    {
      gbp_contract_update (src_epg_id, dst_epg_id, acl_index);
    }
  else
    {
      gbp_contract_delete (src_epg_id, dst_epg_id);
    }

  return (NULL);
}

/*?
 * Configure a GBP Contract
 *
 * @cliexpar
 * @cliexstart{set gbp contract [del] src-epg <ID> dst-epg <ID> acl-index <ACL>}
 * @cliexend
 ?*/
VLIB_CLI_COMMAND (gbp_contract_cli_node, static) =
{
.path = "gbp contract",.short_help =
    "gbp contract [del] src-epg <ID> dst-epg <ID> acl-index <ACL>",.function
    = gbp_contract_cli,};
/* *INDENT-ON* */

static clib_error_t *
gbp_contract_show (vlib_main_t * vm,
		   unformat_input_t * input, vlib_cli_command_t * cmd)
{
  gbp_contract_key_t key;
  epg_id_t epg_id;

  vlib_cli_output (vm, "Contracts:");

  /* *INDENT-OFF* */
  hash_foreach (key.as_u64, epg_id, gbp_contract_db.gc_hash,
  {
    vlib_cli_output (vm, "  {%d,%d} -> %d", key.gck_src,
                     key.gck_dst, epg_id);
  });
  /* *INDENT-ON* */

  return (NULL);
}

/*?
 * Show Group Based Policy Contracts
 *
 * @cliexpar
 * @cliexstart{show gbp contract}
 * @cliexend
 ?*/
/* *INDENT-OFF* */
VLIB_CLI_COMMAND (gbp_contract_show_node, static) = {
  .path = "show gbp contract",
  .short_help = "show gbp contract\n",
  .function = gbp_contract_show,
};
/* *INDENT-ON* */

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
