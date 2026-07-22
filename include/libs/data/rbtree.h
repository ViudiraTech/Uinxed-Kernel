/*
 *
 *      rbtree.h
 *      Augmented red-black tree with cached leftmost pointer
 *
 *      2026/7/21 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_RBTREE_H_
#define INCLUDE_RBTREE_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

typedef enum { RB_RED, RB_BLACK } rb_color_t;

typedef struct rb_node {
        struct rb_node *parent;
        struct rb_node *left;
        struct rb_node *right;
        uint64_t        min_vruntime;
        rb_color_t      color;
} rb_node_t;

typedef struct {
        rb_node_t *root;
        rb_node_t *leftmost;
} rb_root_t;

typedef int (*rb_less_fn)(const rb_node_t *a, const rb_node_t *b);
typedef void (*rb_augment_fn)(rb_node_t *node, void *data);

#define RB_ROOT_INIT \
    {                \
        NULL, NULL   \
    }
#define rb_entry(ptr, type, member) ((type *)((uint8_t *)(ptr) - offsetof(type, member)))

/* Initialize an empty red-black tree root */
void rb_init_root(rb_root_t *root);

/* Insert a node with augmentation support */
void rb_insert_augmented(rb_root_t *root, rb_node_t *node, rb_less_fn less, rb_augment_fn augment, void *data);

/* Erase a node with augmentation support */
void rb_erase_augmented(rb_root_t *root, rb_node_t *node, rb_augment_fn augment, void *data);

/* Return the leftmost (smallest) node, or NULL if the tree is empty */
rb_node_t *rb_first(rb_root_t *root);

/* Return the in-order successor of a node, or NULL if none */
rb_node_t *rb_next(rb_node_t *node);

/* Return 1 if the tree is empty, 0 otherwise */
int rb_is_empty(rb_root_t *root);

#endif /* INCLUDE_RBTREE_H_ */