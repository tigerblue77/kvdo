/*
 * Copyright Red Hat
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vio.c#26 $
 */

#include "vio.h"

#include "logger.h"

#include "dataVIO.h"
#include "vdoInternal.h"

#include <linux/ratelimit.h>

/**********************************************************************/
void free_vio(struct vio **vio_ptr)
{
	struct vio *vio = *vio_ptr;
	if (vio == NULL) {
		return;
	}

	destroy_vio(vio_ptr);
}

/**********************************************************************/
void initialize_vio(struct vio *vio,
		    vio_type type,
		    vio_priority priority,
		    struct vdo_completion *parent,
		    struct vdo *vdo,
		    PhysicalLayer *layer)
{
	struct vdo_completion *completion = vio_as_completion(vio);

	vio->vdo = vdo;
	vio->type = type;
	vio->priority = priority;

	initialize_completion(completion, VIO_COMPLETION, layer);
	completion->parent = parent;
}

/**********************************************************************/
void vio_done_callback(struct vdo_completion *completion)
{
	struct vio *vio = as_vio(completion);
	completion->callback = vio->callback;
	completion->error_handler = vio->error_handler;
	complete_completion(completion);
}

/**********************************************************************/
const char *get_vio_read_write_flavor(const struct vio *vio)
{
	if (is_read_vio(vio)) {
		return "read";
	}
	return (is_write_vio(vio) ? "write" : "read-modify-write");
}

/**********************************************************************/
void update_vio_error_stats(struct vio *vio, const char *format, ...)
{
	static DEFINE_RATELIMIT_STATE(errorLimiter,
				      DEFAULT_RATELIMIT_INTERVAL,
				      DEFAULT_RATELIMIT_BURST);

	va_list args;
	int priority;

	int result = vio_as_completion(vio)->result;
	switch (result) {
	case VDO_READ_ONLY:
		atomic64_add(1, &vio->vdo->error_stats.read_only_error_count);
		return;

	case VDO_NO_SPACE:
		atomic64_add(1, &vio->vdo->error_stats.no_space_error_count);
		priority = LOG_DEBUG;
		break;

	default:
		priority = LOG_ERR;
	}

	if (!__ratelimit(&errorLimiter)) {
		return;
	}

	va_start(args, format);
	vlog_strerror(priority, result, format, args);
	va_end(args);
}

/**
 * Handle an error from a metadata I/O.
 *
 * @param completion  The vio
 **/
static void handle_metadata_io_error(struct vdo_completion *completion)
{
	struct vio *vio = as_vio(completion);
	update_vio_error_stats(vio,
			       "Completing %s vio of type %u for physical block %llu with error",
			       get_vio_read_write_flavor(vio),
			       vio->type,
			       vio->physical);
	vio_done_callback(completion);
}

/**********************************************************************/
void launch_metadata_vio(struct vio *vio,
			 physical_block_number_t physical,
			 vdo_action *callback,
			 vdo_action *error_handler,
			 vio_operation operation)
{
	struct vdo_completion *completion = vio_as_completion(vio);

	vio->operation = operation;
	vio->physical = physical;
	vio->callback = callback;
	vio->error_handler = error_handler;

	reset_completion(completion);
	completion->callback = vio_done_callback;
	completion->error_handler = handle_metadata_io_error;

	submit_metadata_vio(vio);
}

/**
 * Handle a flush error.
 *
 * @param completion  The flush vio
 **/
static void handle_flush_error(struct vdo_completion *completion)
{
	log_error_strerror(completion->result, "Error flushing layer");
	completion->error_handler = as_vio(completion)->error_handler;
	complete_completion(completion);
}

/**********************************************************************/
void launch_flush(struct vio *vio,
		  vdo_action *callback,
		  vdo_action *error_handler)
{
	struct vdo_completion *completion = vio_as_completion(vio);

	ASSERT_LOG_ONLY(get_write_policy(vio->vdo) != WRITE_POLICY_SYNC,
			"pure flushes should not currently be issued in sync mode");

	reset_completion(completion);
	completion->callback = callback;
	completion->error_handler = handle_flush_error;
	vio->error_handler = error_handler;
	vio->operation = VIO_FLUSH_BEFORE;
	vio->physical = ZERO_BLOCK;

	completion->layer->flush(vio);
}
