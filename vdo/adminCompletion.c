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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/adminCompletion.c#30 $
 */

#include "adminCompletion.h"

#include "atomicDefs.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "completion.h"
#include "types.h"
#include "vdoInternal.h"

/**********************************************************************/
void assert_admin_operation_type(struct admin_completion *completion,
				 enum admin_operation_type expected)
{
	ASSERT_LOG_ONLY(completion->type == expected,
			"admin operation type is %u instead of %u",
			completion->type,
			expected);
}

/**********************************************************************/
struct admin_completion *
admin_completion_from_sub_task(struct vdo_completion *completion)
{
	struct vdo_completion *parent = completion->parent;
	assert_completion_type(completion->type, SUB_TASK_COMPLETION);
	assert_completion_type(parent->type, ADMIN_COMPLETION);
	return container_of(parent, struct admin_completion, completion);
}

/**********************************************************************/
void assert_admin_phase_thread(struct admin_completion *admin_completion,
			       const char *what,
			       const char *phase_names[])
{
	thread_id_t expected =
		admin_completion->get_thread_id(admin_completion);
	ASSERT_LOG_ONLY((get_callback_thread_id() == expected),
			"%s on correct thread for %s",
			what,
			phase_names[admin_completion->phase]);
}

/**********************************************************************/
struct vdo *vdo_from_admin_sub_task(struct vdo_completion *completion,
				    enum admin_operation_type expected)
{
	struct admin_completion *admin_completion =
		admin_completion_from_sub_task(completion);
	assert_admin_operation_type(admin_completion, expected);
	return admin_completion->vdo;
}

/**********************************************************************/
void initialize_admin_completion(struct vdo *vdo,
				 struct admin_completion *admin_completion)
{
	admin_completion->vdo = vdo;
	initialize_vdo_completion(&admin_completion->completion, vdo,
				  ADMIN_COMPLETION);
	initialize_vdo_completion(&admin_completion->sub_task_completion, vdo,
				  SUB_TASK_COMPLETION);
	atomic_set(&admin_completion->busy, 0);
}

/**********************************************************************/
struct vdo_completion *reset_admin_sub_task(struct vdo_completion *completion)
{
	struct admin_completion *admin_completion =
		admin_completion_from_sub_task(completion);
	reset_completion(completion);
	completion->callback_thread_id =
		admin_completion->get_thread_id(admin_completion);
	return completion;
}

/**********************************************************************/
void prepare_admin_sub_task_on_thread(struct vdo *vdo,
				      vdo_action *callback,
				      vdo_action *error_handler,
				      thread_id_t thread_id)
{
	prepare_for_requeue(&vdo->admin_completion.sub_task_completion,
			    callback,
			    error_handler,
			    thread_id,
			    &vdo->admin_completion.completion);
}

/**********************************************************************/
void prepare_admin_sub_task(struct vdo *vdo,
			    vdo_action *callback,
			    vdo_action *error_handler)
{
	struct admin_completion *admin_completion = &vdo->admin_completion;
	prepare_admin_sub_task_on_thread(vdo,
					 callback,
					 error_handler,
					 admin_completion->completion.callback_thread_id);
}

/**
 * Callback for admin operations which will notify the layer that the operation
 * is complete.
 *
 * @param completion  The admin completion
 **/
static void admin_operation_callback(struct vdo_completion *completion)
{
	completion->vdo->layer->completeAdminOperation(completion->vdo->layer);
}

/**********************************************************************/
int perform_admin_operation(struct vdo *vdo,
			    enum admin_operation_type type,
			    thread_id_getter_for_phase *thread_id_getter,
			    vdo_action *action,
			    vdo_action *error_handler)
{
	int result;
	struct admin_completion *admin_completion = &vdo->admin_completion;
	if (atomic_cmpxchg(&admin_completion->busy, 0, 1) != 0) {
		return log_error_strerror(VDO_COMPONENT_BUSY,
					  "Can't start admin operation of type %u, another operation is already in progress",
					  type);
	}

	prepare_completion(&admin_completion->completion,
			   admin_operation_callback,
			   admin_operation_callback,
			   get_admin_thread(get_thread_config(vdo)),
			   NULL);
	admin_completion->type = type;
	admin_completion->get_thread_id = thread_id_getter;
	admin_completion->phase = 0;
	prepare_admin_sub_task(vdo, action, error_handler);

	enqueue_completion(&admin_completion->sub_task_completion);
	vdo->layer->waitForAdminOperation(vdo->layer);
	result = admin_completion->completion.result;
	smp_wmb();
	atomic_set(&admin_completion->busy, 0);
	return result;
}
