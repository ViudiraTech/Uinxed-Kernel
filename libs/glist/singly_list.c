/*
 *
 *      singly_list.c
 *      Singly linked list
 *
 *      2025/7/21 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <heap.h>
#include <singly_list.h>

/* Initialize a singly linked list */
int slist_init(slist_t *list)
{
    if (!list) return 1;

    list->head = 0;
    list->tail = 0;
    list->size = 0;
    return 0;
}

/* Insert a node at the head of a singly linked list */
int slist_insert_head(slist_t *list, void *data)
{
    if (!list) return 1;

    slist_node_t *new_node = (slist_node_t *)malloc(sizeof(slist_node_t));
    if (!new_node) return 1;

    new_node->data = data;
    new_node->next = list->head;

    list->head = new_node;
    if (!list->tail) list->tail = new_node;
    list->size++;
    return 0;
}

/* Insert a node at the tail of a singly linked list */
int slist_insert_tail(slist_t *list, void *data)
{
    if (!list) return 1;

    slist_node_t *new_node = (slist_node_t *)malloc(sizeof(slist_node_t));
    if (!new_node) return 1;

    new_node->data = data;
    new_node->next = 0;

    if (list->tail == 0) {
        list->head = new_node;
        list->tail = new_node;
    } else {
        list->tail->next = new_node;
        list->tail       = new_node;
    }
    list->size++;
    return 0;
}

/* Delete the head node of a singly linked list */
int slist_remove_head(slist_t *list, void **data)
{
    if (!list || !list->head) return 1;
    slist_node_t *old_head = list->head;

    if (data) *data = old_head->data;
    list->head = old_head->next;

    if (!list->head) list->tail = 0;
    free(old_head);

    list->size--;
    return 0;
}

/* Delete the tail node of a singly linked list */
int slist_remove_tail(slist_t *list, void **data)
{
    if (!list || !list->tail) return 1;
    slist_node_t *old_tail = list->tail;

    if (data) *data = old_tail->data;
    if (list->head == list->tail) {
        list->head = 0;
        list->tail = 0;
    } else {
        slist_node_t *current = list->head;
        while (current->next != list->tail) current = current->next;
        current->next = 0;
        list->tail    = current;
    }
    free(old_tail);
    list->size--;
    return 0;
}

/* Get the number of nodes in a singly linked list */
size_t slist_size(const slist_t *list)
{
    if (!list) return 1;
    return list->size;
}

/* Destroy a singly linked list */
int slist_destroy(slist_t *list, void (*free_data)(void *))
{
    if (!list) return 1;

    slist_node_t *current = list->head;
    slist_node_t *next;

    while (current != 0) {
        next = current->next;
        if (free_data != 0 && current->data != 0) free_data(current->data);
        free(current);
        current = next;
    }
    list->head = 0;
    list->tail = 0;
    list->size = 0;
    return 0;
}
