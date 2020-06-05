/*
 * Copyright (c) 2020 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA. 
 *
 * $Id: //eng/linux-vdo/src/c++/vdo/base/blockMapFormat.c#1 $
 */

#include "blockMapFormat.h"

#include "buffer.h"
#include "permassert.h"

#include "constants.h"
#include "header.h"
#include "types.h"

const struct header BLOCK_MAP_HEADER_2_0 = {
	.id = BLOCK_MAP,
	.version = {
		.major_version = 2,
		.minor_version = 0,
	},
	.size = sizeof(struct block_map_state_2_0),
};

/**
 * Decode block map component state version 2.0 from a buffer.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param state   The state structure to receive the decoded values
 *
 * @return UDS_SUCCESS or an error code
 **/
int decode_block_map_state_2_0(struct buffer *buffer,
			       struct block_map_state_2_0 *state)
{
	struct header header;
	int result = decode_header(buffer, &header);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = validate_header(&BLOCK_MAP_HEADER_2_0, &header, true,
				 __func__);
	if (result != VDO_SUCCESS) {
		return result;
	}

	size_t initial_length = content_length(buffer);
	result = get_uint64_le_from_buffer(buffer, &state->flat_page_origin);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(state->flat_page_origin == BLOCK_MAP_FLAT_PAGE_ORIGIN,
			"Flat page origin must be %u (recorded as %llu)",
			BLOCK_MAP_FLAT_PAGE_ORIGIN,
			state->flat_page_origin);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &state->flat_page_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(state->flat_page_count == 0,
			"Flat page count must be 0 (recorded as %llu)",
			state->flat_page_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &state->root_origin);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &state->root_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	size_t decoded_size = initial_length - content_length(buffer);
	return ASSERT(BLOCK_MAP_HEADER_2_0.size == decoded_size,
		      "decoded block map component size must match header size");
}

/**********************************************************************/
size_t get_block_map_encoded_size(void)
{
	return ENCODED_HEADER_SIZE + sizeof(struct block_map_state_2_0);
}

/**********************************************************************/
int encode_block_map_state_2_0(struct block_map_state_2_0 state,
			       struct buffer *buffer)
{
	int result = encode_header(&BLOCK_MAP_HEADER_2_0, buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	size_t initial_length = content_length(buffer);

	result = put_uint64_le_into_buffer(buffer, state.flat_page_origin);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, state.flat_page_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, state.root_origin);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, state.root_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	size_t encoded_size = content_length(buffer) - initial_length;
	return ASSERT(BLOCK_MAP_HEADER_2_0.size == encoded_size,
		      "encoded block map component size must match header size");
}
