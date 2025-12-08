/*
 *
 *      singly_list.h
 *      Singly linked list header file
 *
 *      2025/7/21 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SINGLY_LIST_H_
#define INCLUDE_SINGLY_LIST_H_

#include <stddef.h>

typedef struct slist_node {
        void              *data;
        struct slist_node *next;
} slist_node_t;

typedef struct slist {
        slist_node_t *head;
        slist_node_t *tail;
        size_t        size;
} slist_t;

/* Initialize a singly linked list */
int slist_init(slist_t *list);

/* Insert a node at the head of a singly linked list */
int slist_insert_head(slist_t *list, void *data);

/* Insert a node at the tail of a singly linked list */
int slist_insert_tail(slist_t *list, void *data);

/* Delete the head node of a singly linked list */
int slist_remove_head(slist_t *list, void **data);

/* Delete the tail node of a singly linked list */
int slist_remove_tail(slist_t *list, void **data);

/* Get the number of nodes in a singly linked list */
size_t slist_size(const slist_t *list);

/* Destroy a singly linked list */
int slist_destroy(slist_t *list, void (*free_data)(void *));

#endif // INCLUDE_SINGLY_LIST_H_
