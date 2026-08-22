/*
 *
 *      intrusive_list.h
 *      Intrusive linked list header file
 *
 *      2025/7/21 By MicroFish
 *
 */

#ifndef INCLUDE_INTRUSIVE_LIST_H_
#define INCLUDE_INTRUSIVE_LIST_H_

typedef struct ilist_node {
        struct ilist_node *prev;
        struct ilist_node *next;
} ilist_node_t;

/*
 * Node lifecycle and invariants
 *
 * Every node is always in exactly one of three states, encoded entirely in
 * {prev, next}:
 *
 *   DETACHED  prev == next == NULL
 *       Fresh zeroed memory, or the state after a successful ilist_remove().
 *       The node is not a member of any list.
 *
 *   INITED    prev == next == self
 *       Produced by ilist_init().  A list head stays usable as an insertion
 *       position for its whole life; a plain node in this state may be
 *       inserted into exactly one list.
 *
 *   LINKED    prev and next point at other nodes of one circular list.
 *       The node is a member of at most ONE list.  Re-inserting it is
 *       rejected instead of silently corrupting the ring.
 *
 * Rules enforced by this implementation:
 *  - insert validates both the position's ring consistency and that the new
 *    node is DETACHED or INITED before touching any pointer; on rejection no
 *    memory is modified and the error is reported to the caller.
 *  - remove requires a LINKED node whose neighbours still point back at it;
 *    removing a DETACHED/INITED node (including a list head) returns an
 *    error instead of dereferencing stale pointers.  After removal the node
 *    is DETACHED again.
 *  - Callers must serialise concurrent operations on the same list with an
 *    external lock; these primitives are not atomic by themselves.
 *  - Return values: 0 = success, nonzero = operation refused because a rule
 *    above would be violated.  Critical callers treat refusal as proof of a
 *    broken invariant and must stop/panic rather than continue scheduling.
 */

/* Initialize a list head (or reset a plain node to INITED state) */
int ilist_init(struct ilist_node *list);

/* True when the node is currently LINKED into some list */
int ilist_is_linked(const struct ilist_node *node);

/* Insert new_node after position; refuses an already-linked new_node */
int ilist_insert_after(struct ilist_node *node, struct ilist_node *new_node);

/* Insert new_node before position; refuses an already-linked new_node */
int ilist_insert_before(struct ilist_node *node, struct ilist_node *new_node);

/* Unlink a LINKED node; refuses head/double-remove; leaves node DETACHED */
int ilist_remove(struct ilist_node *node);

/* True when the list headed by @list holds no members */
int ilist_is_empty(const struct ilist_node *list);

#endif // INCLUDE_INTRUSIVE_LIST_H_
