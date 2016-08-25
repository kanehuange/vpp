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
#include <vnet/ip/format.h>
#include <vnet/ip/lookup.h>
#include <vnet/adj/adj.h>

#include "fib_entry_src.h"
#include "fib_entry_cover.h"
#include "fib_entry.h"
#include "fib_table.h"

/*
 * fib_entry_src_rr_resolve_via_connected
 *
 * Resolve via a connected cover.
 */
static void
fib_entry_src_rr_resolve_via_connected (fib_entry_src_t *src,
					const fib_entry_t *fib_entry,
					const fib_entry_t *cover)
{
    const fib_route_path_t path = {
	.frp_proto = fib_entry->fe_prefix.fp_proto,
	.frp_addr = fib_entry->fe_prefix.fp_addr,
	.frp_sw_if_index = fib_entry_get_resolving_interface(
	                       fib_entry_get_index(cover)),
	.frp_fib_index = ~0,
	.frp_weight = 1,
    };
    fib_route_path_t *paths = NULL;
    vec_add1(paths, path);

    /*
     * since the cover is connected, the address this entry corresponds
     * to is a peer (ARP-able for) on the interface to which the cover is
     * connected. The fact we resolve via the cover, just means this RR
     * source is the first SRC to use said peer. The ARP source will be along
     * shortly to over-rule this RR source.
     */
    src->fes_pl = fib_path_list_create(FIB_PATH_LIST_FLAG_NONE, paths);
    src->fes_entry_flags = fib_entry_get_flags(fib_entry_get_index(cover));

    vec_free(paths);
}

/**
 * Source initialisation Function 
 */
static void
fib_entry_src_rr_init (fib_entry_src_t *src)
{
    src->rr.fesr_cover = FIB_NODE_INDEX_INVALID;
    src->rr.fesr_sibling = FIB_NODE_INDEX_INVALID;
}

/*
 * Source activation. Called when the source is the new best source on the entry
 */
static int
fib_entry_src_rr_activate (fib_entry_src_t *src,
			   const fib_entry_t *fib_entry)
{
    fib_entry_t *cover;

    /*
     * find the covering prefix. become a dependent thereof.
     * there should always be a cover, though it may be the default route.
     */
    src->rr.fesr_cover = fib_table_get_less_specific(fib_entry->fe_fib_index,
						     &fib_entry->fe_prefix);

    ASSERT(FIB_NODE_INDEX_INVALID != src->rr.fesr_cover);

    cover = fib_entry_get(src->rr.fesr_cover);

    src->rr.fesr_sibling =
	fib_entry_cover_track(cover, fib_entry_get_index(fib_entry));

    /*
     * if the ocver is attached then install an attached-host path
     * (like an adj-fib). Otherwise inherit the forwarding from the cover
     */
    if (FIB_ENTRY_FLAG_ATTACHED & fib_entry_get_flags_i(cover))
    {
	fib_entry_src_rr_resolve_via_connected(src, fib_entry, cover);
    }
    else
    {
	src->fes_pl = cover->fe_parent;
    }
    fib_path_list_lock(src->fes_pl);

    /*
     * return go for install
     */
    return (!0);
}

/**
 * Source Deactivate. 
 * Called when the source is no longer best source on the entry
 */
static void
fib_entry_src_rr_deactivate (fib_entry_src_t *src,
			     const fib_entry_t *fib_entry)
{
    fib_entry_t *cover;

    /*
     * remove the depednecy on the covering entry
     */
    ASSERT(FIB_NODE_INDEX_INVALID != src->rr.fesr_cover);
    cover = fib_entry_get(src->rr.fesr_cover);

    fib_entry_cover_untrack(cover, src->rr.fesr_sibling);

    src->rr.fesr_cover = FIB_NODE_INDEX_INVALID;

    fib_path_list_unlock(src->fes_pl);
    src->fes_pl = FIB_NODE_INDEX_INVALID;
    src->fes_entry_flags = FIB_ENTRY_FLAG_NONE;
}

static fib_entry_src_cover_res_t
fib_entry_src_rr_cover_change (fib_entry_src_t *src,
			       const fib_entry_t *fib_entry)
{
    fib_entry_src_cover_res_t res = {
	.install = !0,
	.bw_reason = FIB_NODE_BW_REASON_FLAG_NONE,
    };

    if (FIB_NODE_INDEX_INVALID == src->rr.fesr_cover)
    {
	/*
	 * the source may be added, but it is not active
	 * if it is not tracking the cover.
	 */
	return (res);
    }

    /*
     * this function is called when this entry's cover has a more specific
     * entry inserted benaeth it. That does not necessarily mean that this
     * entry is covered by the new prefix. check that
     */
    if (src->rr.fesr_cover != fib_table_get_less_specific(fib_entry->fe_fib_index,
							  &fib_entry->fe_prefix))
    {
	fib_entry_src_rr_deactivate(src, fib_entry);
	fib_entry_src_rr_activate(src, fib_entry);

	/*
	 * dependent children need to re-resolve to the new forwarding info
	 */
	res.bw_reason = FIB_NODE_BW_REASON_FLAG_EVALUATE;
    }
    return (res);
}

/*
 * fib_entry_src_rr_cover_update
 *
 * This entry's cover has updated its forwarding info. This entry
 * will need to re-inheret.
 */
static fib_entry_src_cover_res_t
fib_entry_src_rr_cover_update (fib_entry_src_t *src,
			       const fib_entry_t *fib_entry)
{
    fib_entry_src_cover_res_t res = {
	.install = !0,
	.bw_reason = FIB_NODE_BW_REASON_FLAG_NONE,
    };
    fib_node_index_t old_path_list;
    fib_entry_t *cover;

    if (FIB_NODE_INDEX_INVALID == src->rr.fesr_cover)
    {
	/*
	 * the source may be added, but it is not active
	 * if it is not tracking the cover.
	 */
	return (res);
    }

    cover = fib_entry_get(src->rr.fesr_cover);
    old_path_list = src->fes_pl;

    /*
     * if the ocver is attached then install an attached-host path
     * (like an adj-fib). Otherwise inherit the forwarding from the cover
     */
    if (FIB_ENTRY_FLAG_ATTACHED & fib_entry_get_flags_i(cover))
    {
	fib_entry_src_rr_resolve_via_connected(src, fib_entry, cover);
    }
    else
    {
	src->fes_pl = cover->fe_parent;
    }
    fib_path_list_lock(src->fes_pl);
    fib_path_list_unlock(old_path_list);

    /*
     * dependent children need to re-resolve to the new forwarding info
     */
    res.bw_reason = FIB_NODE_BW_REASON_FLAG_EVALUATE;

    return (res);
}

static u8*
fib_entry_src_rr_format (fib_entry_src_t *src,
			 u8* s)
{
    return (format(s, "cover:%d", src->rr.fesr_cover));
}

const static fib_entry_src_vft_t rr_src_vft = {
    .fesv_init = fib_entry_src_rr_init,
    .fesv_activate = fib_entry_src_rr_activate,
    .fesv_deactivate = fib_entry_src_rr_deactivate,
    .fesv_cover_change = fib_entry_src_rr_cover_change,
    .fesv_cover_update = fib_entry_src_rr_cover_update,
    .fesv_format = fib_entry_src_rr_format,
};

void
fib_entry_src_rr_register (void)
{
    fib_entry_src_register(FIB_SOURCE_RR, &rr_src_vft);    
}
