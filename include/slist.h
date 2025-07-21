/*
 *
 *      slist.h
 *      Singly Linked List Header File
 *
 *      2025/7/21 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_SLIST_H_
#define INCLUDE_SLIST_H_

typedef struct slist_node {
        void *data;
        struct slist_node *next;
} slist_node_t;

/* Create a new singly linked list node with given data */
slist_node_t *slist_create_node(void *data);

/* Insert a new node with given data at the head of the singly linked list */
int slist_insert_head(slist_node_t **head, void *data);

/* Insert a new node with given data at the tail of the singly linked list */
int slist_insert_tail(slist_node_t **head, void *data);

/* Insert a new node with given data at the specified position in the singly linked list */
int slist_insert_at(slist_node_t **head, void *data, int pos);

/* Find a node with the specified data in the singly linked list */
slist_node_t *slist_find_node(slist_node_t *head, void *data);

/* Reverse the singly linked list and return the new head node */
slist_node_t *slist_reverse_node(slist_node_t *head);

/* Delete the first node with the specified data from the singly linked list */
int slist_delete_node(slist_node_t **head, void *data);

/* Free all nodes in the singly linked list */
int slist_free_list(slist_node_t *head);

#endif // INCLUDE_SLIST_H_
