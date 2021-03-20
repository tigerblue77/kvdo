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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/workItemStats.c#20 $
 */

#include "workItemStats.h"

#include "atomicDefs.h"
#include "logger.h"

/**
 * Scan the work queue stats table for the provided work function and
 * priority value. If it's not found, see if an empty slot is
 * available.
 *
 * @param table     The work queue's function table
 * @param work      The function we want to record stats for
 * @param priority  The priority of the work item
 *
 * @return The index of the slot to use (matching or empty), or
 *         NUM_WORK_QUEUE_ITEM_STATS if the table is full of
 *         non-matching entries.
 **/
static inline unsigned int
scan_stat_table(const struct vdo_work_function_table *table,
		vdo_work_function work,
		unsigned int priority)
{
	unsigned int i;
	/*
	 * See comments in get_stat_table_index regarding order of memory
	 * accesses. Work function first, then a barrier, then priority.
	 */
	for (i = 0; i < NUM_WORK_QUEUE_ITEM_STATS; i++) {
		if (table->functions[i] == NULL) {
			return i;
		} else if (table->functions[i] == work) {
			smp_rmb();
			if (table->priorities[i] == priority) {
				return i;
			}
		}
	}
	return NUM_WORK_QUEUE_ITEM_STATS;
}

/**
 * Scan the work queue stats table for the provided work function and
 * priority value. Assign an empty slot if necessary.
 *
 * @param stats     The stats structure
 * @param work      The function we want to record stats for
 * @param priority  The priority of the work item
 *
 * @return The index of the matching slot, or NUM_WORK_QUEUE_ITEM_STATS
 *         if the table is full of non-matching entries.
 **/
static unsigned int get_stat_table_index(struct vdo_work_item_stats *stats,
					 vdo_work_function work,
					 unsigned int priority)
{
	struct vdo_work_function_table *function_table =
		&stats->function_table;
	unsigned int index = scan_stat_table(function_table, work, priority);

	unsigned long flags = 0;

	if (unlikely(index == NUM_WORK_QUEUE_ITEM_STATS) ||
	    likely(function_table->functions[index] != NULL)) {
		return index;
	}

	spin_lock_irqsave(&function_table->lock, flags);
	// Recheck now that we've got the lock...
	index = scan_stat_table(function_table, work, priority);
	if ((index == NUM_WORK_QUEUE_ITEM_STATS) ||
	    (function_table->functions[index] != NULL)) {
		spin_unlock_irqrestore(&function_table->lock, flags);
		return index;
	}

	/*
	 * An uninitialized priority is indistinguishable from a zero
	 * priority. So store the priority first, and enforce the ordering,
	 * so that a non-null work function pointer indicates we've finished
	 * filling in the value. (And, to make this work, we have to read
	 * the work function first and priority second, when comparing.)
	 */
	function_table->priorities[index] = priority;
	smp_wmb();
	function_table->functions[index] = work;
	spin_unlock_irqrestore(&function_table->lock, flags);
	return index;
}

/**
 * Get counters on work items, identified by index into the internal
 * array.
 *
 * @param [in]  stats          The collected statistics
 * @param [in]  index          The index
 * @param [out] enqueued_ptr   The total work items enqueued
 * @param [out] processed_ptr  The number of work items processed
 * @param [out] pending_ptr    The number of work items still pending
 **/
static void
get_work_item_counts_by_item(const struct vdo_work_item_stats *stats,
			     unsigned int index,
			     uint64_t *enqueued_ptr,
			     uint64_t *processed_ptr,
			     unsigned int *pending_ptr)
{
	uint64_t enqueued = atomic64_read(&stats->enqueued[index]);
	uint64_t processed = stats->times[index].count;
	unsigned int pending;

	if (enqueued < processed) {
		// Probably just out of sync.
		pending = 1;
	} else {
		pending = enqueued - processed;
		// Pedantic paranoia: Check for overflow of the 32-bit
		// "pending".
		if ((pending + processed) < enqueued) {
			pending = UINT_MAX;
		}
	}
	*enqueued_ptr = enqueued;
	*processed_ptr = processed;
	*pending_ptr = pending;
}

/**
 * Get counters on work items not covered by any index value.
 *
 * @param [in]  stats          The collected statistics
 * @param [out] enqueued_ptr   The total work items enqueued
 * @param [out] processed_ptr  The number of work items processed
 **/
static void get_other_work_item_counts(const struct vdo_work_item_stats *stats,
				       uint64_t *enqueued_ptr,
				       uint64_t *processed_ptr)
{
	unsigned int pending;

	get_work_item_counts_by_item(stats,
				     NUM_WORK_QUEUE_ITEM_STATS,
				     enqueued_ptr,
				     processed_ptr,
				     &pending);
}

/**
 * Get timing stats on work items, identified by index into the
 * internal array.
 *
 * @param [in]  stats  The collected statistics
 * @param [in]  index  The index into the array
 * @param [out] min    The minimum execution time
 * @param [out] mean   The mean execution time
 * @param [out] max    The maximum execution time
 **/
static void
get_work_item_times_by_item(const struct vdo_work_item_stats *stats,
			    unsigned int index,
			    uint64_t *min,
			    uint64_t *mean,
			    uint64_t *max)
{
	*min = stats->times[index].min;
	*mean = get_sample_average(&stats->times[index]);
	*max = stats->times[index].max;
}

/**********************************************************************/
void update_work_item_stats_for_enqueue(struct vdo_work_item_stats *stats,
					struct vdo_work_item *item,
					int priority)
{
	item->stat_table_index = get_stat_table_index(stats,
						      item->stats_function,
						      priority);
	atomic64_inc(&stats->enqueued[item->stat_table_index]);
}

/**********************************************************************/
char *get_function_name(void *pointer, char *buffer, size_t buffer_length)
{
	if (pointer == NULL) {
		/*
		 * Format "%ps" logs a null pointer as "(null)" with a bunch of
		 * leading spaces. We sometimes use this when logging lots of
		 * data; don't be so verbose.
		 */
		strncpy(buffer, "-", buffer_length);
	} else {
		/*
		 * Use a non-const array instead of a string literal below to
		 * defeat gcc's format checking, which doesn't understand that
		 * "%ps" actually does support a precision spec in Linux kernel
		 * code.
		 */
		static char truncated_function_name_format_string[] = "%.*ps";
		char *space;

		snprintf(buffer,
			 buffer_length,
			 truncated_function_name_format_string,
			 buffer_length - 1,
			 pointer);

		space = strchr(buffer, ' ');

		if (space != NULL) {
			*space = '\0';
		}
	}

	return buffer;
}

/**********************************************************************/
size_t format_work_item_stats(const struct vdo_work_item_stats *stats,
			      char *buffer,
			      size_t length)
{
	const struct vdo_work_function_table *function_ids =
		&stats->function_table;
	size_t current_offset = 0;

	uint64_t enqueued, processed;
	int i;

	for (i = 0; i < NUM_WORK_QUEUE_ITEM_STATS; i++) {
		unsigned int pending;
		if (function_ids->functions[i] == NULL) {
			break;
		}
		if (atomic64_read(&stats->enqueued[i]) == 0) {
			continue;
		}
		/*
		 * The reporting of all of "pending", "enqueued" and "processed"
		 * here seems redundant, but "pending" is limited to 0 in the
		 * case where "processed" exceeds "enqueued", either through
		 * current activity and a lack of synchronization when fetching
		 * stats, or a coding bug. This report is intended largely for
		 * debugging, so we'll go ahead and print the
		 * not-necessarily-redundant values.
		 */

		get_work_item_counts_by_item(stats,
					     i,
					     &enqueued,
					     &processed,
					     &pending);

		// Format: fn prio enq proc timeo [ min max mean ]
		if (ENABLE_PER_FUNCTION_TIMING_STATS) {
			uint64_t min, mean, max;

			get_work_item_times_by_item(stats,
						    i,
						    &min,
						    &mean,
						    &max);
			current_offset +=
				scnprintf(buffer + current_offset,
					  length - current_offset,
					  "%-36ps %d %10llu %10llu %10llu %10llu %10llu\n",
					  function_ids->functions[i],
					  function_ids->priorities[i],
					  enqueued,
					  processed,
					  min,
					  max,
					  mean);
		} else {
			current_offset += scnprintf(buffer + current_offset,
						    length - current_offset,
						    "%-36ps %d %10llu %10llu\n",
						    function_ids->functions[i],
						    function_ids->priorities[i],
						    enqueued,
						    processed);
		}
		if (current_offset >= length) {
			break;
		}
	}
	if ((i == NUM_WORK_QUEUE_ITEM_STATS) && (current_offset < length)) {
		uint64_t enqueued, processed;

		get_other_work_item_counts(stats, &enqueued, &processed);
		if (enqueued > 0) {
			current_offset += scnprintf(buffer + current_offset,
						    length - current_offset,
						    "%-36s %d %10llu %10llu\n",
						    "OTHER",
						    0,
						    enqueued,
						    processed);
		}
	}
	return current_offset;
}

/**********************************************************************/
void log_work_item_stats(const struct vdo_work_item_stats *stats)
{
	uint64_t total_enqueued = 0;
	uint64_t total_processed = 0;

	const struct vdo_work_function_table *function_ids =
		&stats->function_table;

	int i;

	for (i = 0; i < NUM_WORK_QUEUE_ITEM_STATS; i++) {
		uint64_t enqueued, processed;
		unsigned int pending;
		static char work[256]; // arbitrary size

		if (function_ids->functions[i] == NULL) {
			break;
		}
		if (atomic64_read(&stats->enqueued[i]) == 0) {
			continue;
		}
		/*
		 * The reporting of all of "pending", "enqueued" and "processed"
		 * here seems redundant, but "pending" is limited to 0 in the
		 * case where "processed" exceeds "enqueued", either through
		 * current activity and a lack of synchronization when fetching
		 * stats, or a coding bug. This report is intended largely for
		 * debugging, so we'll go ahead and print the
		 * not-necessarily-redundant values.
		 */
		get_work_item_counts_by_item(stats,
					     i,
					     &enqueued,
					     &processed,
					     &pending);
		total_enqueued += enqueued;
		total_processed += processed;

		get_function_name(function_ids->functions[i],
				  work,
				  sizeof(work));

		if (ENABLE_PER_FUNCTION_TIMING_STATS) {
			uint64_t min, mean, max;

			get_work_item_times_by_item(stats,
						    i,
						    &min,
						    &mean,
						    &max);
			log_info("  priority %d: %u pending %llu enqueued %llu processed %s times %llu/%llu/%lluns",
				 function_ids->priorities[i],
				 pending,
				 enqueued,
				 processed,
				 work,
				 min,
				 mean,
				 max);
		} else {
			log_info("  priority %d: %u pending %llu enqueued %llu processed %s",
				 function_ids->priorities[i],
				 pending,
				 enqueued,
				 processed,
				 work);
		}
	}
	if (i == NUM_WORK_QUEUE_ITEM_STATS) {
		uint64_t enqueued, processed;

		get_other_work_item_counts(stats, &enqueued, &processed);
		if (enqueued > 0) {
			total_enqueued += enqueued;
			total_processed += processed;
			log_info("  ... others: %llu enqueued %llu processed",
				 enqueued,
				 processed);
		}
	}
	log_info("  total: %llu enqueued %llu processed",
		 total_enqueued,
		 total_processed);
}
