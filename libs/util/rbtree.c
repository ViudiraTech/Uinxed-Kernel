/*
 *
 *      rbtree.c
 *      Augmented red-black tree implementation
 *
 *      2026/7/21 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <libs/std/stddef.h>
#include <libs/util/rbtree.h>

/* Recompute the augmented value up the chain to the root. */
static void augment_propagate(rb_node_t *node, rb_augment_fn augment, void *data)
{
    while (node) {
        augment(node, data);
        node = node->parent;
    }
}

/* Replace old with replacement in old's parent (or at the tree root). */
static void rb_transplant(rb_root_t *root, rb_node_t *old, rb_node_t *replacement)
{
    if (!old->parent) {
        root->root = replacement;
    } else if (old == old->parent->left) {
        old->parent->left = replacement;
    } else {
        old->parent->right = replacement;
    }
    if (replacement) replacement->parent = old->parent;
}

/*
 * Left rotation: node          right
 *                /  \          /  \
 *             left  right ==> node rr
 *                   /  \      /  \
 *                  rl   rr  left  rl
 */
static void rb_rotate_left(rb_root_t *root, rb_node_t *node, rb_augment_fn augment, void *data)
{
    rb_node_t *right = node->right;

    node->right = right->left;
    if (right->left) right->left->parent = node;

    right->parent = node->parent;
    if (!node->parent) {
        root->root = right;
    } else if (node == node->parent->left) {
        node->parent->left = right;
    } else {
        node->parent->right = right;
    }

    right->left  = node;
    node->parent = right;

    if (augment) {
        augment(node, data);
        augment(right, data);
    }
}

/*
 * Right rotation:       node                 left
 * /    \               /    \
 * left  right   ==>      ll    node
 * /   \                       /   \
 * ll   lr                     lr  right
 */
static void rb_rotate_right(rb_root_t *root, rb_node_t *node, rb_augment_fn augment, void *data)
{
    rb_node_t *left = node->left;

    node->left = left->right;
    if (left->right) left->right->parent = node;

    left->parent = node->parent;
    if (!node->parent) {
        root->root = left;
    } else if (node == node->parent->left) {
        node->parent->left = left;
    } else {
        node->parent->right = left;
    }

    left->right  = node;
    node->parent = left;

    if (augment) {
        augment(node, data);
        augment(left, data);
    }
}

/* Fix red-red violations after insertion */
static void rb_insert_rebalance(rb_root_t *root, rb_node_t *node, rb_augment_fn augment, void *data)
{
    rb_node_t *parent, *grandparent, *uncle;

    while ((parent = node->parent) && parent->color == RB_RED) {
        grandparent = parent->parent;

        if (parent == grandparent->left) {
            uncle = grandparent->right;

            /* Case 1: uncle is RED - recolor and move up */
            if (uncle && uncle->color == RB_RED) {
                parent->color      = RB_BLACK;
                uncle->color       = RB_BLACK;
                grandparent->color = RB_RED;
                node               = grandparent;
                continue;
            }

            /* Case 2: node is right child - rotate left */
            if (node == parent->right) {
                node = parent;
                rb_rotate_left(root, node, augment, data);
                parent      = node->parent;
                grandparent = parent->parent;
            }

            /* Case 3: node is left child - rotate right */
            parent->color      = RB_BLACK;
            grandparent->color = RB_RED;
            rb_rotate_right(root, grandparent, augment, data);
        } else {
            uncle = grandparent->left;

            /* Case 1: uncle is RED - recolor and move up */
            if (uncle && uncle->color == RB_RED) {
                parent->color      = RB_BLACK;
                uncle->color       = RB_BLACK;
                grandparent->color = RB_RED;
                node               = grandparent;
                continue;
            }

            /* Case 2: node is left child - rotate right */
            if (node == parent->left) {
                node = parent;
                rb_rotate_right(root, node, augment, data);
                parent      = node->parent;
                grandparent = parent->parent;
            }

            /* Case 3: node is right child - rotate left */
            parent->color      = RB_BLACK;
            grandparent->color = RB_RED;
            rb_rotate_left(root, grandparent, augment, data);
        }
    }

    root->root->color = RB_BLACK;
}

/* Fix double-black violations after erase */
static void rb_erase_rebalance(rb_root_t *root, rb_node_t *node, rb_node_t *parent, rb_augment_fn augment, void *data)
{
    rb_node_t *sibling;

    while ((!node || node->color == RB_BLACK) && node != root->root) {
        /* A valid tree always supplies parent here; stop safely if damaged. */
        if (!parent) break;

        if (node == parent->left) {
            sibling = parent->right;

            /* Case 1: sibling is RED */
            if (sibling && sibling->color == RB_RED) {
                sibling->color = RB_BLACK;
                parent->color  = RB_RED;
                rb_rotate_left(root, parent, augment, data);
                sibling = parent->right;
            }

            /* A NULL sibling is a black leaf with two black children. */
            if (!sibling) {
                node   = parent;
                parent = node->parent;
                continue;
            }

            /* Case 2: sibling's children are both BLACK */
            if ((!sibling->left || sibling->left->color == RB_BLACK) && (!sibling->right || sibling->right->color == RB_BLACK)) {
                sibling->color = RB_RED;
                node           = parent;
                parent         = node->parent;
            } else {
                /* Case 3: sibling's right child is BLACK */
                if (!sibling->right || sibling->right->color == RB_BLACK) {
                    if (sibling->left) sibling->left->color = RB_BLACK;
                    sibling->color = RB_RED;
                    rb_rotate_right(root, sibling, augment, data);
                    sibling = parent->right;
                }

                /* Case 4: sibling's right child is RED */
                sibling->color = parent->color;
                parent->color  = RB_BLACK;
                if (sibling->right) sibling->right->color = RB_BLACK;
                rb_rotate_left(root, parent, augment, data);
                node = root->root;
                break;
            }
        } else {
            sibling = parent->left;

            /* Case 1: sibling is RED */
            if (sibling && sibling->color == RB_RED) {
                sibling->color = RB_BLACK;
                parent->color  = RB_RED;
                rb_rotate_right(root, parent, augment, data);
                sibling = parent->left;
            }

            /* A NULL sibling is a black leaf with two black children. */
            if (!sibling) {
                node   = parent;
                parent = node->parent;
                continue;
            }

            /* Case 2: sibling's children are both BLACK */
            if ((!sibling->left || sibling->left->color == RB_BLACK) && (!sibling->right || sibling->right->color == RB_BLACK)) {
                sibling->color = RB_RED;
                node           = parent;
                parent         = node->parent;
            } else {
                /* Case 3: sibling's left child is BLACK */
                if (!sibling->left || sibling->left->color == RB_BLACK) {
                    if (sibling->right) sibling->right->color = RB_BLACK;
                    sibling->color = RB_RED;
                    rb_rotate_left(root, sibling, augment, data);
                    sibling = parent->left;
                }

                /* Case 4: sibling's left child is RED */
                sibling->color = parent->color;
                parent->color  = RB_BLACK;
                if (sibling->left) sibling->left->color = RB_BLACK;
                rb_rotate_right(root, parent, augment, data);
                node = root->root;
                break;
            }
        }
    }

    if (node) node->color = RB_BLACK;
}

/* Return the node with the minimum value in the subtree */
static rb_node_t *rb_subtree_min(rb_node_t *node)
{
    while (node->left) node = node->left;
    return node;
}

/* Initialize an empty red-black tree. */
void rb_init_root(rb_root_t *root)
{
    if (!root) return;
    root->root     = NULL;
    root->leftmost = NULL;
}

/* Initialize a detached red-black tree node. */
void rb_init_node(rb_node_t *node)
{
    if (!node) return;
    node->parent       = NULL;
    node->left         = NULL;
    node->right        = NULL;
    node->owner        = NULL;
    node->min_vruntime = 0;
    node->color        = RB_BLACK;
}

/* Report whether a node currently belongs to a tree. */
int rb_is_linked(const rb_node_t *node)
{
    return node && node->owner != NULL;
}

/* Return the smallest node in the tree, or NULL when empty. */
rb_node_t *rb_first(rb_root_t *root)
{
    return root ? root->leftmost : NULL;
}

/* Return the in-order successor of node, or NULL at the last node. */
rb_node_t *rb_next(rb_node_t *node)
{
    if (!node || !node->owner) return NULL;

    /* If right subtree exists, return leftmost of right subtree */
    if (node->right) return rb_subtree_min(node->right);

    /* Otherwise, go up until we find a node that is a left child */
    rb_node_t *parent = node->parent;
    while (parent && node == parent->right) {
        node   = parent;
        parent = parent->parent;
    }
    return parent;
}

/* Report whether the tree is empty. */
int rb_is_empty(rb_root_t *root)
{
    return !root || root->root == NULL;
}

/* Insert a node and restore red-black and augmentation invariants. */
int rb_insert_augmented(rb_root_t *root, rb_node_t *node, rb_less_fn less, rb_augment_fn augment, void *data)
{
    if (!root || !node || !less || node->owner) return 1;

    rb_node_t **link     = &root->root;
    rb_node_t  *parent   = NULL;
    int         leftmost = 1;

    /* BST search for insertion point */
    while (*link) {
        parent = *link;
        if (less(node, parent)) {
            link = &parent->left;
        } else {
            link     = &parent->right;
            leftmost = 0;
        }
    }

    /* Link the node */
    node->parent       = parent;
    node->left         = NULL;
    node->right        = NULL;
    node->owner        = root;
    node->color        = RB_RED;
    node->min_vruntime = 0;
    *link              = node;

    /* Update cached leftmost */
    if (leftmost) root->leftmost = node;

    /* Rotations must never observe an uninitialized augmented value. */
    if (augment) augment(node, data);

    /* Fix red-black violations */
    rb_insert_rebalance(root, node, augment, data);

    /* Propagate augmentation up from the inserted node */
    if (augment) augment_propagate(node, augment, data);
    return 0;
}

/* Remove a node and restore red-black and augmentation invariants. */
int rb_erase_augmented(rb_root_t *root, rb_node_t *node, rb_augment_fn augment, void *data)
{
    if (!root || !node || node->owner != root || !root->root) return 1;

    rb_node_t *child;
    rb_node_t *rebalance_parent;
    rb_node_t *successor     = node;
    rb_color_t removed_color = successor->color;

    /* Update cached leftmost */
    if (root->leftmost == node) root->leftmost = rb_next(node);

    if (!node->left) {
        child            = node->right;
        rebalance_parent = node->parent;
        rb_transplant(root, node, node->right);
    } else if (!node->right) {
        child            = node->left;
        rebalance_parent = node->parent;
        rb_transplant(root, node, node->left);
    } else {
        /* Move the in-order successor into node's position. */
        successor     = rb_subtree_min(node->right);
        removed_color = successor->color;
        child         = successor->right;

        if (successor->parent == node) {
            /*
             * This is the subtle direct-successor case: child used to point
             * back at node in the old implementation, leaving a detached
             * scheduler entity in the ancestry chain after erase.
             */
            rebalance_parent = successor;
            if (child) child->parent = successor;
        } else {
            rebalance_parent = successor->parent;
            rb_transplant(root, successor, successor->right);
            successor->right         = node->right;
            successor->right->parent = successor;
        }

        rb_transplant(root, node, successor);
        successor->left         = node->left;
        successor->left->parent = successor;
        successor->color        = node->color;
    }

    /* The old successor path reaches the transplanted successor and root. */
    if (augment) augment_propagate(rebalance_parent, augment, data);

    /* Fix double-black violations */
    if (removed_color == RB_BLACK) rb_erase_rebalance(root, child, rebalance_parent, augment, data);

    /* Make double erase/double insert detectable and safe. */
    rb_init_node(node);
    if (!root->root) root->leftmost = NULL;
    return 0;
}
