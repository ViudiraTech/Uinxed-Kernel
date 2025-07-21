/*
 *
 *      singly_list.c
 *      Singly Linked List
 *
 *      2025/7/21 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "singly_list.h"
#include "alloc.h"

/* Create a new singly linked list node with given data */
slist_node_t *slist_create_node(void *data)
{
    slist_node_t *new_node = (slist_node_t *)malloc(sizeof(slist_node_t));
    if (!new_node) return 0;

    new_node->data = data;
    new_node->next = 0;
    return new_node;
}

/* Insert a new node with given data at the head of the singly linked list */
int slist_insert_head(slist_node_t **head, void *data)
{
    slist_node_t *new_node = slist_create_node(data);
    if (!new_node) return 1;

    new_node->next = *head;
    *head          = new_node;
    return 0;
}

/* Insert a new node with given data at the tail of the singly linked list */
int slist_insert_tail(slist_node_t **head, void *data)
{
    slist_node_t *new_node = slist_create_node(data);
    if (!new_node) return 1;
    if (*head == 0) {
        *head = new_node;
        return 0;
    }
    slist_node_t *temp = *head;
    while (temp->next != 0) temp = temp->next;

    temp->next = new_node;
    return 0;
}

/* Insert a new node with given data at the specified position in the singly linked list */
int slist_insert_at(slist_node_t **head, void *data, int pos)
{
    if (pos <= 0 || *head == 0) {
        slist_insert_head(head, data);
        return 0;
    }

    slist_node_t *temp = *head;
    int index          = 0;

    while (temp->next != 0 && index < pos - 1) {
        temp = temp->next;
        index++;
    }

    slist_node_t *new_node = slist_create_node(data);
    if (!new_node) return 1;

    new_node->next = temp->next;
    temp->next     = new_node;
    return 0;
}

/* Find a node with the specified data in the singly linked list */
slist_node_t *slist_find_node(slist_node_t *head, void *data)
{
    while (head != 0) {
        if (head->data == data) return head;
        head = head->next;
    }
    return 0;
}

/* Reverse the singly linked list and return the new head node */
slist_node_t *slist_reverse_node(slist_node_t *head)
{
    slist_node_t *prev = 0;
    slist_node_t *curr = head;
    slist_node_t *next = 0;

    while (curr != 0) {
        next       = curr->next;
        curr->next = prev;
        prev       = curr;
        curr       = next;
    }
    return prev;
}

/* Delete the first node with the specified data from the singly linked list */
int slist_delete_node(slist_node_t **head, void *data)
{
    slist_node_t *temp = *head;
    slist_node_t *prev = 0;

    while (temp != 0) {
        if (temp->data == data) {
            if (prev == 0) {
                *head = temp->next;
            } else {
                prev->next = temp->next;
            }
            free(temp);
            return 0;
        }
        prev = temp;
        temp = temp->next;
    }
    return 1;
}

/* Free all nodes in the singly linked list */
int slist_free_list(slist_node_t *head)
{
    if (head == 0) return 1;

    slist_node_t *temp;
    while (head != 0) {
        temp = head;
        head = head->next;
        free(temp);
    }
    return 0;
}
