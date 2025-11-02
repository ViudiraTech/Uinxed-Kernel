/*
 *
 *      circular_list.h
 *      Doubly circular linked list header file
 *
 *      2025/11/2 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_CIRCULAR_LIST_H_
#define INCLUDE_CIRCULAR_LIST_H_

#include "stddef.h"

typedef void (*free_t)(void *ptr);
typedef void *(*alloc_t)(size_t size);

typedef struct clist *clist_t;
struct clist {
        union {
                void   *data;
                ssize_t idata;
                size_t  udata;
        };
        clist_t prev;
        clist_t next;
};

/* Allocate and initialize a new circular linked list node with the given data */
clist_t clist_alloc(void *data);

/* Free all nodes in the circular linked list */
clist_t clist_free(clist_t clist);

/* Free all nodes in the circular linked list and their associated data using a callback */
clist_t clist_free_with(clist_t clist, void (*free_data)(void *));

/* Append a new node with the given data to the end of the circular linked list */
clist_t clist_append(clist_t clist, void *data);

/* Prepend a new node with the given data to the beginning of the circular linked list */
clist_t clist_prepend(clist_t clist, void *data);

/* Remove and free the last node (tail) of the circular linked list and return its data */
void *clist_pop(clist_t *clist_p);

/* Find and return the head (first node) of the circular linked list */
clist_t clist_head(clist_t clist);

/* Find and return the tail (last node) of the circular linked list */
clist_t clist_tail(clist_t clist);

/* Get the nth node in the circular linked list (0-based index) */
clist_t clist_nth(clist_t clist, size_t n);

/* Get the nth node from the end of the circular linked list (0-based index) */
clist_t clist_nth_last(clist_t clist, size_t n);

/* Search for a node containing the specified data in the circular linked list */
int clist_search(clist_t clist, void *data);

/* Delete the first node containing the specified data from the circular linked list */
clist_t clist_delete(clist_t clist, void *data);

/* Delete the first node containing the specified data, using a callback to free the data */
clist_t clist_delete_with(clist_t clist, void *data, free_t callback);

/* Delete a specific node from the circular linked list */
clist_t clist_delete_node(clist_t clist, clist_t node);

/* Delete a specific node, using a callback to free its data */
clist_t clist_delete_node_with(clist_t clist, clist_t node, free_t callback);

/* Calculate and return the number of nodes in the circular linked list */
size_t clist_length(clist_t clist);

#endif // INCLUDE_CIRCULAR_LIST_H_
