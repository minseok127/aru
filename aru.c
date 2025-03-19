#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

#include <assert.h>
#include <errno.h>
#include <pthread.h>

#include "aru.h"
#include "atomsnap.h"

#define ARU_NODE_TYPE_UPDATE (0)
#define ARU_NODE_TYPE_READ (1)

/*
 * aru_node - Linked list node containing the user's function
 * @callback: user's callback function
 * @args: callback function's arguments
 * @prev: pointer to the previously inserted node
 * @next: pointer to the next inserted node
 * @user_tag_ptr: pointer fo notifying the user of the node's status
 * @tag: ARU_TAG_PENDING / ARU_TAG_DONE
 * @lock: spinlock to protect the execution of the callback function
 * @type: ARU_NODE_TYPE_UPDATE / ARU_NODE_TYPE_READ
 *
 * The user can execute their function asynchronously through aru using the
 * aru_read() and aru_update() APIs.
 *
 * aru creates these functions as aru_node instances and manages them in a
 * doubly linked list.
 */
struct aru_node {
	void (*callback)(void *args);
	void *args;
	struct aru_node *prev;
	struct aru_node *next;
	aru_tag *user_tag_ptr;
	aru_tag tag;
	pthread_spinlock_t lock;
	int type;
};

/*
 * aru_tail_version - Data structure to cover the lifetime of the nodes.
 * @version: atomsnap_version to manage grace-period
 * @tail_version_prev: previously created tail version
 * @tail_version_next: next tail version
 * @head_node: most recent node covered the lifetime by this tail verison
 * @tail_node: oldest node covered the lifetime by this tail version
 *
 * aru_nodes are managed as a linked list, and the tail of the list is managed
 * in an RCU-like manner. This means that when moving the tail, intermediate
 * nodes are not immediately freed but are given a grace period, which is
 * managed using the atomsnap library.
 *
 * When the tail is updated, a range of nodes covering the lifetime of the
 * previous tail is created. If this range is at the end of the linked list, it
 * indicates that other threads no longer traverse this range of nodes.
 *
 * To verify this, each tail version is also linked via a pointer, and
 * this pointer is used to determine whether the current version represents the
 * last segment of the linked list.
 */
struct aru_tail_version {
	struct atomsnap_version version;
	struct aru_tail_version *tail_version_prev;
	struct aru_tail_version *tail_version_next;
	struct aru_node *head_node;
	struct aru_node *tail_node;
};

/*
 * aru - main data structure to manage functions asynchronously
 * @head: point where a new node is inserted into the linked list
 * @tail: point where the oldest node is located
 * @tail_move_flag: flag that must be acquired to move the tail
 * @tail_init_flag: whether or not the tail is initialized
 *
 * Data structure used when the user calls aru_read() and aru_update(). The
 * critical section of these functions is guaranteed only when aru_read() and
 * aru_update() are used on the same aru instance. If APIs are used on different
 * aru instances, their critical sections are managed separately.
 *
 * Unlike the head of the linked list, the tail is managed in an RCU-like
 * manner. So the atomsnap library is used.
 */
struct aru {
	struct aru_node *head;
	struct atomsnap_gate *tail;
	_Atomic int tail_move_flag;
	_Atomic int tail_init_flag;
};

/* atomsnap_make_version() will call this function */
struct atomsnap_version *aru_tail_version_alloc(
	void *alloc_arg __attribute__((unused)))
{
	struct aru_tail_version *tail_version 
		= calloc(1, sizeof(struct aru_tail_version));
	return (struct atomsnap_version *)tail_version;
}

/* See the comment of the struct aru_tail_version and adjust_tail() */
#define TAIL_VERSION_RELEASE_MASK (0x8000000000000000ULL)
void aru_tail_version_free(struct atomsnap_version *version)
{
	struct aru_tail_version *tail_version = (struct aru_tail_version *)version;
	struct aru_tail_version *next_tail_version = NULL;
	struct aru_tail_version *prev_ptr 
		= (struct aru_tail_version *)atomic_fetch_or(
			&tail_version->tail_version_prev, TAIL_VERSION_RELEASE_MASK);
	struct aru_node *node = NULL;	

	/* This is not the end of linke list, so we cannot free the nodes */
	if (prev_ptr != NULL) {
		return;
	}

	__sync_synchronize();

free_tail_nodes:

	/* This range was the last. So we can free these safely. */
	node = tail_version->tail_node;
	while (node != tail_version->head_node) {
		node = node->next;
		free(node->prev);
	}
	free(tail_version->head_node);

	next_tail_version
		= (struct aru_tail_version *)tail_version->tail_version_next;
	assert(next_tail_version != NULL);

	prev_ptr = (struct aru_tail_version *)atomic_load(
		&next_tail_version->tail_version_prev);

	if (((uintptr_t)prev_ptr & TAIL_VERSION_RELEASE_MASK) != 0) {
		tail_version = next_tail_version;
		goto free_tail_nodes;
	} else if (!atomic_compare_exchange_weak(
			&next_tail_version->tail_version_prev, &prev_ptr, NULL)) {
		tail_version = next_tail_version;
		goto free_tail_nodes;
	}
}

/*
 * Returns pointer to an aru, or NULL on failure.
 */
struct aru *aru_init(void)
{
	struct atomsnap_init_context ctx = {
		.atomsnap_alloc_impl = aru_tail_version_alloc,
		.atomsnap_free_impl = aru_tail_version_free
	};
	struct aru *aru_ptr = calloc(1, sizeof(struct aru));

	if (aru_ptr == NULL) {
		fprintf(stderr, "aru_init: aru allocaation failed\n");
		return NULL;
	}

	aru_ptr->tail = atomsnap_init_gate(&ctx);
	if (aru_ptr->tail == NULL) {
		fprintf(stderr, "aru_init: atomsnap_init_gate() failed\n");
		free(aru_ptr);
		return NULL;
	}

	return aru_ptr;
}

/*
 * Destory the given aru.
 */
void aru_destroy(struct aru *aru)
{
	if (aru == NULL) {
		return;
	}

	atomsnap_destroy_gate(aru->tail);

	if (aru->head != NULL) {
		free(aru->head);
	}

	free(aru);
}

/*
 * adjust_tail - Move the tail
 * @aru: pointer of the aru
 * @new_tail: the aru_node that will become the new tail
 *
 * Calling atomsnap_exchange_version() in this function starts the grace period
 * for the previous tail version. The last thread to release this old tail
 * version will execute aru_tail_version_free().
 *
 * The reference to the old tail version is released after this function
 * returns. This ensures that the deallocation ffor this old version will not be
 * executed until at least that point. So it is safe to link the old version
 * with the newly created version.
 */
static void adjust_tail(struct aru *aru,
	struct aru_tail_version *prev_tail_version, struct aru_node *new_tail_node)
{
	struct aru_tail_version *new_tail_version
		 = (struct aru_tail_version *)atomsnap_make_version(aru->tail,NULL);

	atomic_store(&new_tail_version->tail_version_prev, prev_tail_version);
	atomic_store(&new_tail_version->tail_version_next, NULL);

	new_tail_version->head_node = NULL;
	new_tail_version->tail_node = new_tail_node;

	atomsnap_exchange_version(aru->tail,
		(struct atomsnap_version *)new_tail_version);

	atomic_store(&prev_tail_version->tail_version_next, new_tail_version);
	prev_tail_version->head_node = new_tail_node->prev;
}

#define TRY_NEXT (0)
#define BREAK (1)
/*
 * execute_node - try to execute the callback function of the node
 * @node: pointer of the node
 * @tail_node: the node corresponding to the tail version
 *
 * If this node contains an update function that requires exclusive execution,
 * it checks whether the all previous nodes have completed or not. If it
 * represents a read function, it checks whether the all previous update
 * functions have completed or not.
 *
 * If we can execute this node's callback function, attempt to acquire the
 * spinlock for the node. If successful, execute it. If we failed, return a
 * value indicating to proceed to the next node.
 *
 * Returns TRY_NEXT or BREAK.
 */
static int execute_node(struct aru_node *node, struct aru_node *tail_node)
{
	struct aru_node *prev_node = node->prev;

	if (node->type == ARU_NODE_TYPE_UPDATE) {
		while (prev_node != NULL && prev_node != tail_node) {
			if (prev_node->tag != ARU_TAG_DONE) {
				return BREAK;
			}

			prev_node = prev_node->prev;
		}

		if (tail_node->tag != ARU_TAG_DONE) {
			return BREAK;
		}
	} else {
		while (prev_node != NULL && prev_node != tail_node) {
			if (prev_node->type == ARU_NODE_TYPE_UPDATE &&
					prev_node->tag != ARU_TAG_DONE) {
				return BREAK;
			}

			prev_node = prev_node->prev;
		}

		if (tail_node->type == ARU_NODE_TYPE_UPDATE &&
				tail_node->tag != ARU_TAG_DONE) {
			return BREAK;
		}
	}

	if (pthread_spin_trylock(&node->lock) == 0) {
		node->callback(node->args);
		node->tag = ARU_TAG_DONE;

		if (node->user_tag_ptr != NULL) {
			*node->user_tag_ptr = ARU_TAG_DONE;
		}
	}

	return TRY_NEXT;
}

/*
 * execute_nodes_and_adjust_tail - try to execute nodes and adjust tail
 * @aru: pointer of the aru
 * @tail_version: the tail version referenced by this function
 * @tail_move_flag: if 0, we are allowed to move the tail
 * @inserted_node: pointer to the node inserted by the caller
 *
 * Traverse from the tail to the most recent node, attempting to execute
 * callback functions. Since node insertion is lock-free, the next pointer may
 * temporarily appear as null.
 *
 * If the noe was inserted before the current execution flow's inserted node,
 * its next pointer will be set soon, so wait for it. Otherwise, if the node is
 * likely the most recent one, terminate immediately.
 *
 * We ensure that aru-head never becomes null. So when traversing nodes, track
 * the previous node and use it to update the tail.
 */
static void execute_nodes_and_adjust_tail(struct aru *aru, 
	struct aru_tail_version *tail_version, int tail_move_flag,
	struct aru_node *inserted_node)
{
	struct aru_node *node = tail_version->tail_node;
	struct aru_node *prev_node = node;
	bool after_inserted_node = false;

	while (node != NULL) {
		if (node->tag == ARU_TAG_PENDING &&
				execute_node(node, tail_version->tail_node) == BREAK) {
			break;
		}

		if (after_inserted_node) {
			prev_node = node;
			node = node->next;
		} else {
			/*
			 * If the node was inserted before the inserted_node, its next
			 * pointer will be set soon.
			 */
			while (node->next == NULL) {
				__asm__ __volatile__("pause");
			}

			prev_node = node;
			node = node->next;

			/*
			 * From this point, it is not guaranteed that the node's next
			 * pointer will be set soon.
			 */
			if (node == inserted_node) {
				after_inserted_node = true;
			}
		}
	}

	if (tail_move_flag == 0 && prev_node != tail_version->tail_node) {
		adjust_tail(aru, tail_version, prev_node);
	}
}

/*
 * insert_node_and_execute - Insert the node and execute functions from tail
 * @aru: pointer of the aru
 * @node: pointer of the aru_node to insert
 *
 * Atomically insert the given node at the head of aru's linked list and execute
 * as many node functions as possible starating from the tail.
 */
static void insert_node_and_execute(struct aru *aru, struct aru_node *node)
{
	struct aru_node *prev_head = NULL;
	struct aru_tail_version *tail = NULL;
	int fetched_tail_move_flag = 1;

	/*
	 * To move the tail in a consistent direction, the flag must be acquired
	 * before obtaining the tail version.
	 *
	 * If the order is reversed, the movement of the tail by another thread may
	 * be ignored because the obtained tail may be an old version if we don't
	 * have the flag.
	 */
	if (aru->tail_move_flag == 0) {
		fetched_tail_move_flag = atomic_fetch_or(&aru->tail_move_flag, 1);
	}

	__sync_synchronize();

	node->next = NULL;
	prev_head = atomic_exchange(&aru->head, node);

	/*
	 * prev_head is NULL only for the first node inserted after aru is
	 * initialized. After initialization, aru->head is never NULL.
	 */
	if (prev_head == NULL) {
		tail = (struct aru_tail_version *)atomsnap_make_version(aru->tail,NULL);
		
		tail->tail_version_prev = NULL;
		tail->tail_version_next = NULL;

		tail->head_node = NULL;
		tail->tail_node = node;

		atomsnap_exchange_version(aru->tail, (struct atomsnap_version *)tail);

		atomic_store(&aru->tail_init_flag, 1);
	} else {
		prev_head->next = node;
		node->prev = prev_head;

		/* Initial state */
		while (atomic_load(&aru->tail_init_flag) == 0) {
			__asm__ __volatile__("pause");
		}
	}

	tail = (struct aru_tail_version *)atomsnap_acquire_version(aru->tail);

	execute_nodes_and_adjust_tail(aru, tail, fetched_tail_move_flag, node);

	atomsnap_release_version((struct atomsnap_version *)tail);

	if (fetched_tail_move_flag == 0) {
		atomic_store(&aru->tail_move_flag, 0);
	}
}

/*
 * aru_update - Update API provided to the user
 * @aru: pointer of the aru
 * @tag: status representing progress or result
 * @update: user's update function
 * @args: update function's arguments
 *
 * The user expects the update logic passed to this function to be executed
 * asynchronously, ensuring a critical section without interference from other
 * threads.
 *
 * The user can pass a tag as an argument to track the update status. If the tag
 * is NULL, no status will be provided.
 */
void aru_update(struct aru *aru, aru_tag *tag,
	void (*update)(void *args), void *args)
{
	struct aru_node *node = calloc(1, sizeof(struct aru_node));

	if (node == NULL) {
		fprintf(stderr, "aru_update(): aru_node allocation failed\n");
		return;
	}

	node->callback = update;
	node->args = args;
	node->user_tag_ptr = tag;

	node->tag = ARU_TAG_PENDING;
	if (tag != NULL) {
		*tag = node->tag;
	}

	pthread_spin_init(&node->lock, PTHREAD_PROCESS_PRIVATE);

	node->type = ARU_NODE_TYPE_UPDATE;

	insert_node_and_execute(aru, node);
}

/*
 * aru_read - Read API provided to the user
 * @aru: pointer of the aru
 * @tag: status representing progress or result
 * @read: user's read function
 * @args: read function's arguments
 *
 * The user expects the read logic passed to this function to be executed
 * asynchronously. The read function will be executed after all previously
 * requested updates have been applied. Unlike the update logic, multiple read
 * operations can run concurrently, but update operations are not to be executed
 * simultaneously.
 *
 * The user can pass a tag as an argument to track the update status. If the tag
 * is NULL, no status will be provided.
 */
void aru_read(struct aru *aru, aru_tag *tag,
	void (*read)(void *args), void *args)
{
	struct aru_node *node = calloc(1, sizeof(struct aru_node));

	if (node == NULL) {
		fprintf(stderr, "aru_update(): aru_node allocation failed\n");
		return;
	}

	node->callback = read;
	node->args = args;
	node->user_tag_ptr = tag;

	node->tag = ARU_TAG_PENDING;
	if (tag != NULL) {
		*tag = node->tag;
	}

	pthread_spin_init(&node->lock, PTHREAD_PROCESS_PRIVATE);

	node->type = ARU_NODE_TYPE_READ;

	insert_node_and_execute(aru, node);
}

