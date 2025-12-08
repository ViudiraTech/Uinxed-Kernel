/*
 *
 *      intrusive_list.c
 *      Intrusive linked list
 *
 *      2025/7/21 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <intrusive_list.h>

/* Initialize the intrusive linked list header node */
int ilist_init(struct ilist_node *list)
{
    if (!list) return 1;
    list->prev = list;
    list->next = list;
    return 0;
}

/* Insert a new node after the specified node */
int ilist_insert_after(struct ilist_node *node, struct ilist_node *new_node)
{
    if (!node || !new_node) return 1;
    new_node->next   = node->next;
    new_node->prev   = node;
    node->next->prev = new_node;
    node->next       = new_node;
    return 0;
}

/* Insert a new node before the specified node */
int ilist_insert_before(struct ilist_node *node, struct ilist_node *new_node)
{
    if (!node || !new_node) return 1;
    return ilist_insert_after(node->prev, new_node);
}

/* Remove the specified node from the intrusive linked list */
int ilist_remove(struct ilist_node *node)
{
    if (!node || node->next == node || node->prev == node) return 1;
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev       = 0;
    node->next       = 0;
    return 0;
}

/* Check if the intrusive linked list is empty */
int ilist_is_empty(const struct ilist_node *list)
{
    if (!list) return 1;
    return list->next == list;
}
