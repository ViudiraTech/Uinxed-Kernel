/*
 *
 *      doubly_list.h
 *      Doubly linked list header file
 *
 *      2025/11/2 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DOUBLY_LIST_H_
#define INCLUDE_DOUBLY_LIST_H_

#include <stddef.h>

#define clist_foreach_cnt(clist, i, node, code)                                  \
    ({                                                                           \
        size_t i = 0;                                                            \
        for (clist_t node = (clist); node; (node) = (node)->next, (i)++) (code); \
    })

#define clist_first_node(clist, node, expr)                         \
    ({                                                              \
        clist_t _match_ = 0;                                        \
        for (clist_t node = (clist); node; (node) = (node)->next) { \
            if ((expr)) {                                           \
                _match_ = node;                                     \
                break;                                              \
            }                                                       \
        }                                                           \
        _match_;                                                    \
    })

#define clist_first(clist, _data_, expr)                        \
    ({                                                          \
        void *_match_ = 0;                                      \
        for (clist_t node = (clist); node; node = node->next) { \
            void *(_data_) = node->data;                        \
            if ((expr)) {                                       \
                _match_ = _data_;                               \
                break;                                          \
            }                                                   \
        }                                                       \
        _match_;                                                \
    })

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

/* Allocate and initialize a new doubly linked list node with the given data */
clist_t clist_alloc(void *data);

/* Free all nodes in the doubly linked list */
clist_t clist_free(clist_t clist);

/* Free all nodes in the doubly linked list and their associated data using a callback */
clist_t clist_free_with(clist_t clist, void (*free_data)(void *));

/* Append a new node with the given data to the end of the doubly linked list */
clist_t clist_append(clist_t clist, void *data);

/* Prepend a new node with the given data to the beginning of the doubly linked list */
clist_t clist_prepend(clist_t clist, void *data);

/* Remove and free the last node (tail) of the doubly linked list and return its data */
void *clist_pop(clist_t *clist_p);

/* Find and return the head (first node) of the doubly linked list */
clist_t clist_head(clist_t clist);

/* Find and return the tail (last node) of the doubly linked list */
clist_t clist_tail(clist_t clist);

/* Get the nth node in the doubly linked list (0-based index) */
clist_t clist_nth(clist_t clist, size_t n);

/* Get the nth node from the end of the doubly linked list (0-based index) */
clist_t clist_nth_last(clist_t clist, size_t n);

/* Search for a node containing the specified data in the doubly linked list */
int clist_search(clist_t clist, void *data);

/* Delete the first node containing the specified data from the doubly linked list */
clist_t clist_delete(clist_t clist, void *data);

/* Delete the first node containing the specified data, using a callback to free the data */
clist_t clist_delete_with(clist_t clist, void *data, free_t callback);

/* Delete a specific node from the doubly linked list */
clist_t clist_delete_node(clist_t clist, clist_t node);

/* Delete a specific node, using a callback to free its data */
clist_t clist_delete_node_with(clist_t clist, clist_t node, free_t callback);

/* Calculate and return the number of nodes in the doubly linked list */
size_t clist_length(clist_t clist);

#endif // INCLUDE_DOUBLY_LIST_H_
