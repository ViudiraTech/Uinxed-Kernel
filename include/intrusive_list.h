/*
 *
 *      intrusive_list.h
 *      Intrusive Linked List Header File
 *
 *      2025/7/21 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_INTRUSIVE_LIST_H_
#define INCLUDE_INTRUSIVE_LIST_H_

typedef struct ilist_node {
        struct ilist_node *prev;
        struct ilist_node *next;
} ilist_node_t;

/* Initialize the intrusive linked list header node */
int ilist_init(struct ilist_node *list);

/* Insert a new node after the specified node */
int ilist_insert_after(struct ilist_node *node, struct ilist_node *new_node);

/* Insert a new node before the specified node */
int ilist_insert_before(struct ilist_node *node, struct ilist_node *new_node);

/* Remove the specified node from the intrusive linked list */
int ilist_remove(struct ilist_node *node);

/* Check if the intrusive linked list is empty */
int ilist_is_empty(const struct ilist_node *list);

#endif // INCLUDE_INTRUSIVE_LIST_H_
