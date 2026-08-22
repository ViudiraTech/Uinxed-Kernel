/*
 *
 *      intrusive_list.c
 *      Intrusive linked list
 *
 *      2025/7/21 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <libs/list/intrusive_list.h>
#include <libs/std/stddef.h>

/*
 * State predicates.  See intrusive_list.h for the lifecycle contract.
 *
 * A LINKED node must also satisfy the local ring consistency check
 * prev->next == node && next->prev == node; operations verify this before
 * committing so a corrupted neighbour can never be dereferenced into a
 * write, and a failed operation leaves every pointer untouched.
 */

/* Node is a member of exactly one circular list */
static int node_linked_consistent(const struct ilist_node *node)
{
    if (!node || !node->prev || !node->next) return 0;
    if (node->prev == node || node->next == node) return 0;
    return node->prev->next == node && node->next->prev == node;
}

/* Node may legally be passed as new_node to an insert */
static int node_insertable(const struct ilist_node *node)
{
    if (!node) return 0;
    /* DETACHED (fresh or removed) or INITED (self-linked but unqueued) */
    if (!node->prev && !node->next) return 1;
    return node->prev == node && node->next == node;
}

/* Position may legally receive an insertion */
static int position_valid(const struct ilist_node *node)
{
    if (!node) return 0;
    /* An empty head is self-linked and consistent by definition */
    if (node->next == node && node->prev == node) return 1;
    return node_linked_consistent(node);
}

/* Initialize the intrusive linked list header node */
int ilist_init(struct ilist_node *list)
{
    if (!list) return 1;
    list->prev = list;
    list->next = list;
    return 0;
}

/* True when the node is currently linked into some list */
int ilist_is_linked(const struct ilist_node *node)
{
    return node_linked_consistent(node);
}

/* Insert a new node after the specified node */
int ilist_insert_after(struct ilist_node *node, struct ilist_node *new_node)
{
    if (!position_valid(node)) return 1;
    if (!node_insertable(new_node)) return 2;

    new_node->prev   = node;
    new_node->next   = node->next;
    node->next->prev = new_node;
    node->next       = new_node;
    return 0;
}

/* Insert a new node before the specified node */
int ilist_insert_before(struct ilist_node *node, struct ilist_node *new_node)
{
    if (!position_valid(node)) return 1;
    if (!node_insertable(new_node)) return 2;

    /*
     * Inlined insert_after(position->prev): position->prev has already been
     * validated as part of the ring consistency check above.
     */
    struct ilist_node *prev = node->prev;

    new_node->prev = prev;
    new_node->next = node;
    node->prev     = new_node;
    prev->next     = new_node;
    return 0;
}

/* Remove the specified node from the intrusive linked list */
int ilist_remove(struct ilist_node *node)
{
    if (!node_linked_consistent(node)) return 1; // DETACHED, INITED (list head), double-remove, or corrupt

    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev       = NULL;
    node->next       = NULL;
    return 0;
}

/* Check if the intrusive linked list is empty */
int ilist_is_empty(const struct ilist_node *list)
{
    if (!list) return 1;

    /* A never-initialized (zeroed) head reports empty instead of faulting */
    if (!list->next) return 1;
    return list->next == list;
}
