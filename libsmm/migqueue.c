/*
 * migqueue.c
 *
 *  Created on: Jun 29, 2015
 *      Author: xzl
 *
 *  The lockfree queue implementation for memif.
 *
 *  The implementation is shared by both user and kernel.
 */

#include <stddef.h>
#include "mig.h"
#include "log-xzl.h"

const char *clr_strings[] = {
		"none",
		"usr",
		"ker"
};

/* Create and init a mig queue. Must be synchronized.
 *
 * @queue: IN/OUT the queue meta data to be initialized.
 * @dummy: IN the dummy node used as the Head (we must have one)
 * @color: the color of the queue.
 * return: NULL on failure, otherwise the queue metadata
 */
mig_queue *migqueue_create_color(mig_region *r, mig_queue *queue, mig_desc *dummy,
		color_t color)
{
	mig_desc *node = dummy;

	assert(queue && dummy);
//	node->refcnt = 1;
	node->next.index = BADINDEX;
//	node->next.qflag = 0;
//	if (color != COLOR_NONE) {
		node->next.color = color;
//		node->next.qflag |= (1 << PTR_FLAG_CLR);
//	} else {
//		node->next.qflag &= ~(1 << PTR_FLAG_CLR);
//		W("no color. qflag %x", node->next.qflag);
//	}

	queue->head.index = queue->tail.index = desc_to_index(r, node);
	D("head.index %d color %d", queue->tail.index, color);

	LFDS611_BARRIER_STORE; // necessary?

	return queue;
}

mig_queue *migqueue_create(mig_region *r, mig_queue *queue, mig_desc *dummy)
{
	return migqueue_create_color(r, queue, dummy, COLOR_NONE);
}

/* By design, when a desc is just dequeued from a queue, it still serves on the
 * queue as the head node until yet another desc is dequeued. This will create
 * problems if the former desc is added to another queue, as the desc's
 * original queue metadata (link/color) will be overwritten.
 *
 * Thus, when dequeueing a desc, we shall return a replica. The original copy
 * stays on the queue as the head node.
 *
 * return: the pointer to the replica desc
 */
mig_desc *migdesc_replicate(mig_desc *new, mig_desc *old)
{
	/* desc content */
	new->word0 = old->word0;
	new->word1 = old->word1;
	/* color from the link, which may be examined by the dequeue caller.  */
	new->next.color = old->next.color;

//	new->virt_base = old->virt_base;
//	new->order = old->order;
//	new->flag = old->flag;
	return new;
}

/*
 * return: -1 if queue is not empty
 * otherwise set @color and return the old color
 */
color_t migqueue_set_color_if_empty(mig_region *r, mig_queue *queue, color_t color)
{
	mig_desc_ptr head, next, tail, newnext;
	assert(r);
	assert(queue);

	/* Basic idea:
	 * rewrite the ->next field in the head node with the right color
	 *
	 * The head node will not be retired between checkpoint and commit.
	 * On an empty queue, nobody can retire the head node without enqueuing
	 * extra nodes, an operation changes the head node's -> next and increases
	 * next's ABA counter.
	 *
     * We atomically update ->next to make sure no changes happen between our
     * checkpoint and a successful CAS.
	 *
	 * CAS fails if any update to ->next happens under us, i.e., enqueuing.
	 * In this case, the head from last checkpoint may even have been retired.
	 * Thus we should retry with a new checkpoint of Head.
	 * */

	while (1) {
		/* checkpointing */
		head = queue->head;
		tail = queue->tail;
		assert(head.index != BADINDEX); /* a queue should always have a head node */
		/* @next: a checkpoint of head node's ->next field */
		next = index_to_desc(r, head.index)->next;
//		assert(next.qflag & (1 << PTR_FLAG_CLR)); /* queue must be colored. */

		__sync_synchronize();

		if (head.raw == queue->head.raw) { /* checkpoints still consistent */
			if(head.index != tail.index)
				/* queue not empty. abort */
				return -1;

			if (next.index != BADINDEX)
				/* Seems Tail falls behind, meaning queue not empty. abort.
				 * (We don't swing the tail here.) */
				return -1;

			/* queue is empty. Try to taint the @next field with new color */
			newnext = next;  /* copy the entire link first */
			newnext.color = color;
			newnext.count = next.count + 1;
			if (__sync_bool_compare_and_swap(
					(uint64_t *) (&(index_to_desc(r, head.index)->next)),
					next.raw,
					newnext.raw)) {
				/* commit okay */
//				W("head.index %d", head.index);
				return next.color; /* old color */
			}
		}
	}

	/* never reach here */
	assert(0);
	return -1;
}

/* Enqueue a desc node that is already initialized. Willl fix its link though.
 *
 * return: on success, return the color of queue (this is also the color that
 * the new tail inherits for the old one).
 * Only meaningful when the queue is colored.
 *
 * -1 if fail. */
color_t mig_enqueue(mig_region *r, mig_queue *queue, mig_desc *desc)
{
	mig_desc_ptr tail, next, tmp;

	assert(queue && desc);

	desc->next.index = BADINDEX;
	/* XXX? desc->next.count = 0; */
	desc_get(desc);

	LFDS611_BARRIER_LOAD;

	do {
		// checkpoint Tail & tail->next
		tail = queue->tail;
		next = index_to_desc(r, tail.index)->next;

		LFDS611_BARRIER_LOAD;

		if (tail.raw == queue->tail.raw) { // still consistent?
			if (next.index == BADINDEX) {
				/* @tmp: the new link to replace tail's link */
				tmp.index = desc_to_index(r, desc);
//				tmp.qflag = next.qflag;
				/* taint the new node? */
//				if (next.qflag & (1 << PTR_FLAG_CLR)) {
					// the replaced link to the new node should keep color
					tmp.color = next.color;
					// the new node's next link should inherit color
					desc->next.color = next.color;
//					I("set node idx %d color %d", tmp.index, tmp.color);
					if (tmp.color < 0) {
						E("bug: color %d", tmp.color);
						xzl_bug();
					}
//				}
				tmp.count = next.count + 1;  // since we are going to replace it
				// commit the new node
				if (__sync_bool_compare_and_swap(
						(uint64_t *) (&((index_to_desc(r, tail.index)->next))),
						next.raw,
						tmp.raw))
					break;
			} else {
				// Tail not pointing to the last node. Try swinging it
				// -- by updating queue->tail.
				// This is done with best efforts. Okay if we fail.
				tmp.index = next.index;
				tmp.count = tail.count + 1;
				__sync_bool_compare_and_swap(
										(uint64_t *) (&(queue->tail)),
										tail.raw,
										tmp.raw);
			}
		}
	} while (1);

	/* enqueue is done. try to swing Tail to @tmp, the newly inserted node.
	 * XXX: Okay not to care Tail's flag and color?
	 */
	tmp.index = desc_to_index(r, desc); 	// necessary?
	tmp.count = tail.count + 1;
	__sync_bool_compare_and_swap(
							(uint64_t *) (&(queue->tail)),
							tail.raw,
							tmp.raw);

//	I("set node idx %d color %d", tmp.index, tmp.color);
	return tmp.color;
}

/* On success, a replica of the dequeued node will be made to return.  The
 * dequeued desc will remain on the queue used as Head until next dequeue.
 *
 * NB: because of this, this function needs allocation from freelist to work.
 *
 * See migdesc_replicate() for more.
 *
 * Return: the dequeue'd desc on success, otherwise NULL
 */
mig_desc *mig_dequeue(mig_region *r, mig_queue *queue)
{
	mig_desc_ptr head, tail, next;
	mig_desc *deq, *ret = NULL;

	assert(queue);

	LFDS611_BARRIER_LOAD;

	while (1) {
		/* check pointing.
		 * we need to check both q->head and q->tail.
		 * @q->head: this node does not contain useful value.
		 * 		it is either a dummy node created by queue creation,
		 * 		or a previously used node that we defer deleting.
		 * 		After yet another node is dequeued and used as the new head,
		 * 		we can retire the old Head.
		 *
		 * 		instead. @q->head.ptr->next contains the value we will dequeue.
		 *
		 * @q->tail: to see whether queue is empty or q->tail needs a kick */
		head = queue->head;
		tail = queue->tail;
		assert(head.index != BADINDEX);
		/* NB: @head points to the dummy node. */
		next = index_to_desc(r, head.index)->next;

		/* w/o this barrier, we seem to have deadlock in the dequeue test. why?*/
		LFDS611_BARRIER_LOAD;
//		__sync_synchronize();

		/* are checkpoints still consistent? (a safe guard?) */
		if (head.raw == queue->head.raw) {
			if (tail.index == head.index) {
				/* queue seems empty. but we still need to check if Tail
				 * falls behind.
				 */
				if (next.index == BADINDEX) {
					/* NB: it may well be possible that the queue becomes
					 * nonempty since the checkpoint. but we don't care.
					 */
					if (ret)
						freelist_add(r, ret);
					return NULL;
				}
				/* Tail seems to fall behind. advance it with best effort */
				next.count = tail.count + 1;
				__sync_bool_compare_and_swap(
										(uint64_t *) (&(queue->tail)),
										tail.raw,
										next.raw);
			} else { /* no need to deal with tail */
				/* Try to swing Head to the next node; when succeed,
				 * retire the old head node. */
				assert(next.index != BADINDEX);
				next.count = head.count + 1;

                /* We replicate the dequeued desc for returning.  the original
                 * copy stays on the queue as the head node. By doing so, we
                 * make sure the dequeued desc won't show up on two queues, a
                 * nasty situation.  The replication must happen before CAS.
                 * After CAS, the dequeued node (src) may be retired, freed, and
                 * reused.
				 */
				if (!ret) {
					if (!(ret = freelist_remove(r))) {
						E("failed to get new desc as a replica");
						xzl_bug();
						return NULL;
					}
				}
				deq = index_to_desc(r, next.index);
				migdesc_replicate(ret, deq);

				if (__sync_bool_compare_and_swap(
						(uint64_t *) (&(queue->head)),
						head.raw,
						next.raw)) {
                        /* Commit okay. Note that the valued dequeued will be
                         * @next, instead of @head, as @head is a dummy node by
                         * design whose value was dequeued already.
                         *
                         * Now it is safe to retire the head node.
						 */
						freelist_add(r, index_to_desc(r, head.index));
//						ret = desc_put(r, index_to_desc(r, head.index));
//						assert(ret == 0);
						return ret;
				}
			}
		}

	}

	assert(0); /* never reach here */
}

/* --------------------------------------------------------- */

/* given a free memory region (a desc array), clear the region and
 * add all descs to the new free list.
 *
 * the region @r must have been initialized already.
 *
 * @list: the pre-allocated metadata for the list
 * @size: the number of descs
 */
mig_free_list *freelist_init(mig_region *r)
{
	int i;
	mig_desc *base;
	int size;

	assert(r);
	base = r->desc_array;
	size = r->ndescs;

	assert(base && (size > 0));

	r->freelist.element_counter = 0;
	r->freelist.aba_counter = 0;
	r->freelist.top.index = BADINDEX;
	r->freelist.top.count = 0;
	r->freelist.top.color = COLOR_NONE;  // no need?

	for (i = 0; i < size; i++) {
		base[i].refcnt = 0;
		/* more init? */
		freelist_add(r, base + i);
	}

	return &(r->freelist);
}

static mig_free_list *__freelist_add(mig_region *region, mig_free_list *list,
		mig_desc *desc)
{
	mig_desc_ptr top, tmp;  /* @tmp: the new ptr value to be committed */

	LFDS611_BARRIER_LOAD;

	assert(region);
	assert(desc->refcnt == 0);

	/* assemble the new top */
	tmp.count =
			__sync_add_and_fetch(&(list->aba_counter), 1); /* @count gets the old value */
	tmp.index = desc_to_index(region, desc);

	do {
		/* checkpoint the old top  */
		top = list->top;
		/* prepare the new top based on my checkpoint
		 * (both ptr and count copied) */
		index_to_desc(region, tmp.index)->next = top;

		if (__sync_bool_compare_and_swap(
				(uint64_t *) (&(list->top)),
				top.raw,
				tmp.raw))
			break;
	} while (1);

	/* XXX increase list->element_counter? XXX */
	return list;
}

mig_free_list *freelist_add(mig_region *r, mig_desc *desc)
{
	assert(r);
	return __freelist_add(r, &(r->freelist), desc);
}

static mig_desc *__freelist_remove(mig_region *r, mig_free_list *list)
{
	mig_desc_ptr top, tmp;

	assert(r);

	LFDS611_BARRIER_LOAD;

	/* as long as we get a fresh @count, it is fine. so do it once */
//	tmp.count = __sync_add_and_fetch(&(list->aba_counter), 1); /* @count gets the old value */

	do {
		/* checkpoint the old top */
		top = list->top;
		if (top.index == BADINDEX)
			/* even if someone stepped in to add nodes and make the list non-empty,
			 * we don't care -- just return NULL. */
			return NULL;

		/* build a new top based on the checkpoint */
		tmp = index_to_desc(r, top.index)->next;	/* by construction, their @count will differ */

		/* commit */
		if (__sync_bool_compare_and_swap(
				(uint64_t *) (&(list->top)),
				top.raw,
				tmp.raw)) {
				/* XXX return list->element_counter? XXX */
				assert(index_to_desc(r, top.index)->refcnt == 0);
				return index_to_desc(r, top.index);
		}
	} while (1);

	/* never reach here */
	assert(0);
}

mig_desc *freelist_remove(mig_region *r)
{
	assert(r);
	return __freelist_remove(r, &(r->freelist));
}

/* --------------------------------------------------------- */

/* init a memory region: create/init freelist and various queues.
 * put all descs into the freelist.
 *
 * Note: this func is not lockfree -- callers should be serialized.
 *
 * @page: the pointer to the uninitialized memory region
 * @size: total size of the region, in bytes. this decides how many mig
 * descs can this region hold.
 *
 * return: NULL if failed
 */
mig_region *init_migregion(void *pages, int size)
{
	mig_region * region;
	int ndescs;
	mig_desc * head;

	assert(pages);
	assert(size % SZ_4K == 0);

	region = (mig_region *)(pages);

	ndescs = (size - offsetof(mig_region, desc_array)) / sizeof(mig_desc);
	region->ndescs = ndescs;

	// freelist
	freelist_init(region);

	/* various queues
	 * NB: don't call desc_get(head), so that it will be recycled once it has been
     * dequeued.
	 */

	// req queue
    head = freelist_remove(region);
    assert(head);
    migqueue_create_color(region, &(region->qreq), head, COLOR_USR);

    // issued queue
    head = freelist_remove(region);
	assert(head);
	migqueue_create(region, &(region->qissued), head);

    // comp queue
    head = freelist_remove(region);
    assert(head);
    migqueue_create(region, &(region->qcomp), head);

    // XXX more queues
	return region;
}
