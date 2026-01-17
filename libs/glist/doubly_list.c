/*
 *
 *      doubly_list.c
 *      Doubly linked list
 *
 *      2025/11/2 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <doubly_list.h>
#include <heap.h>
#include <stdint.h>
#include <string.h>

/* Allocate and initialize a new doubly linked list node with the given data */
clist_t clist_alloc(void *data)
{
    clist_t node = (clist_t)(malloc((uint32_t)(sizeof(*node))));
    if (!node) return 0;

    node->data = data;
    node->prev = 0;
    node->next = 0;
    return node;
}

/* Free all nodes in the doubly linked list */
clist_t clist_free(clist_t clist)
{
    while (clist) {
        clist_t next = clist->next;
        free(clist);
        clist = next;
    }
    return 0;
}

/* Free all nodes in the doubly linked list and their associated data using a callback */
clist_t clist_free_with(clist_t clist, void (*free_data)(void *))
{
    while (clist) {
        clist_t next = clist->next;
        free_data(clist->data);
        free(clist);
        clist = next;
    }
    return 0;
}

/* Append a new node with the given data to the end of the doubly linked list */
clist_t clist_append(clist_t clist, void *data)
{
    clist_t node = clist_alloc(data);

    if (!node) return clist;
    if (!clist) {
        clist = node;
    } else {
        clist_t current = clist;
        while (current->next) current = current->next;
        current->next = node;
        node->prev    = current;
    }
    return clist;
}

/* Prepend a new node with the given data to the beginning of the doubly linked list */
clist_t clist_prepend(clist_t clist, void *data)
{
    clist_t node = clist_alloc(data);
    if (!node) return clist;

    node->next = clist;
    if (clist) clist->prev = node;

    clist = node;
    return clist;
}

/* Remove and free the last node (tail) of the doubly linked list and return its data */
void *clist_pop(clist_t *clist_p)
{
    if (!clist_p || !*clist_p) return 0;
    clist_t clist = clist_tail(*clist_p);

    if (*clist_p == clist) *clist_p = clist->prev;
    if (clist->prev) clist->prev->next = 0;
    clist_t data = clist->data;

    free(clist);
    return data;
}

/* Find and return the head (first node) of the doubly linked list */
clist_t clist_head(clist_t clist)
{
    if (!clist) return 0;
    for (; clist->prev; clist = clist->prev);
    return clist;
}

/* Find and return the tail (last node) of the doubly linked list */
clist_t clist_tail(clist_t clist)
{
    if (!clist) return 0;
    for (; clist->next; clist = clist->next);
    return clist;
}

/* Get the nth node in the doubly linked list (0-based index) */
clist_t clist_nth(clist_t clist, size_t n)
{
    if (!clist) return 0;
    clist = clist_head(clist);

    for (size_t i = 0; i < n; i++) {
        clist = clist->next;
        if (!clist) return 0;
    }
    return clist;
}

/* Get the nth node from the end of the doubly linked list (0-based index) */
clist_t clist_nth_last(clist_t clist, size_t n)
{
    if (!clist) return 0;
    clist = clist_tail(clist);

    for (size_t i = 0; i < n; i++) {
        clist = clist->prev;
        if (!clist) return 0;
    }
    return clist;
}

/* Search for a node containing the specified data in the doubly linked list */
int clist_search(clist_t clist, void *data)
{
    clist_t current = clist;
    while (current) {
        if (current->data == data) return 1;
        current = current->next;
    }
    return 0;
}

/* Delete the first node containing the specified data from the doubly linked list */
clist_t clist_delete(clist_t clist, void *data)
{
    if (!clist) return 0;
    if (clist->data == data) {
        clist_t temp = clist;
        clist        = clist->next;
        free(temp);
        return clist;
    }
    for (clist_t current = clist->next; current; current = current->next) {
        if (current->data == data) {
            current->prev->next = current->next;
            if (current->next) current->next->prev = current->prev;
            free(current);
            break;
        }
    }
    return clist;
}

/* Delete the first node containing the specified data, using a callback to free the data */
clist_t clist_delete_with(clist_t clist, void *data, free_t callback)
{
    if (!clist) return 0;
    if (clist->data == data) {
        clist_t temp = clist;
        clist        = clist->next;
        if (callback) callback(temp->data);
        free(temp);
        return clist;
    }
    for (clist_t current = clist->next; current; current = current->next) {
        if (current->data == data) {
            current->prev->next = current->next;
            if (current->next != 0) current->next->prev = current->prev;
            if (callback) callback(current->data);
            free(current);
            break;
        }
    }
    return clist;
}

/* Delete a specific node from the doubly linked list */
clist_t clist_delete_node(clist_t clist, clist_t node)
{
    if (!clist || !node) return clist;
    if (clist == node) {
        clist_t temp = clist;
        clist        = clist->next;
        free(temp);
        return clist;
    }
    node->prev->next = node->next;
    if (node->next) node->next->prev = node->prev;
    free(node);
    return clist;
}

/* Delete a specific node, using a callback to free its data */
clist_t clist_delete_node_with(clist_t clist, clist_t node, free_t callback)
{
    if (!clist || !node) return clist;
    if (clist == node) {
        clist_t temp = clist;
        clist        = clist->next;
        if (callback) callback(temp->data);
        free(temp);
        return clist;
    }
    node->prev->next = node->next;
    if (node->next) node->next->prev = node->prev;
    if (callback) callback(node->data);
    free(node);
    return clist;
}

/* Calculate and return the number of nodes in the doubly linked list */
size_t clist_length(clist_t clist)
{
    size_t  count   = 0;
    clist_t current = clist;
    while (current) {
        count++;
        current = current->next;
    }
    return count;
}
