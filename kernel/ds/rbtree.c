/*
 *
 *      rbtree.c
 *      Augmented red-black tree implementation
 *
 *      2026/7/21 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <rbtree.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static void augment_propagate(rb_node_t *node, rb_augment_fn augment, void *data)
{
	while (node) {
		augment(node, data);
		node = node->parent;
	}
}

/* Left rotation:     node                     right
 *                   /    \                    /    \
 *                 left  right     ==>       node    rr
 *                       /   \              /   \
 *                      rl   rr          left   rl
 */
static void rb_rotate_left(rb_root_t *root, rb_node_t *node,
                           rb_augment_fn augment, void *data)
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

	right->left   = node;
	node->parent  = right;

	if (augment) {
		augment(node, data);
		augment(right, data);
	}
}

/* Right rotation:       node                 left
 *                      /    \               /    \
 *                   left  right   ==>      ll    node
 *                   /   \                       /   \
 *                  ll   lr                     lr  right
 */
static void rb_rotate_right(rb_root_t *root, rb_node_t *node,
                            rb_augment_fn augment, void *data)
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
static void rb_insert_rebalance(rb_root_t *root, rb_node_t *node,
                                rb_augment_fn augment, void *data)
{
	rb_node_t *parent, *grandparent, *uncle;

	while ((parent = node->parent) && parent->color == RB_RED) {
		grandparent = parent->parent;

		if (parent == grandparent->left) {
			uncle = grandparent->right;

			/* Case 1: uncle is RED — recolor and move up */
			if (uncle && uncle->color == RB_RED) {
				parent->color      = RB_BLACK;
				uncle->color       = RB_BLACK;
				grandparent->color = RB_RED;
				node               = grandparent;
				continue;
			}

			/* Case 2: node is right child — rotate left */
			if (node == parent->right) {
				node = parent;
				rb_rotate_left(root, node, augment, data);
				parent      = node->parent;
				grandparent = parent->parent;
			}

			/* Case 3: node is left child — rotate right */
			parent->color      = RB_BLACK;
			grandparent->color = RB_RED;
			rb_rotate_right(root, grandparent, augment, data);
		} else {
			uncle = grandparent->left;

			/* Case 1: uncle is RED — recolor and move up */
			if (uncle && uncle->color == RB_RED) {
				parent->color      = RB_BLACK;
				uncle->color       = RB_BLACK;
				grandparent->color = RB_RED;
				node               = grandparent;
				continue;
			}

			/* Case 2: node is left child — rotate right */
			if (node == parent->left) {
				node = parent;
				rb_rotate_right(root, node, augment, data);
				parent      = node->parent;
				grandparent = parent->parent;
			}

			/* Case 3: node is right child — rotate left */
			parent->color      = RB_BLACK;
			grandparent->color = RB_RED;
			rb_rotate_left(root, grandparent, augment, data);
		}
	}

	root->root->color = RB_BLACK;
}

/* Fix double-black violations after erase */
static void rb_erase_rebalance(rb_root_t *root, rb_node_t *node,
                               rb_node_t *parent,
                               rb_augment_fn augment, void *data)
{
	rb_node_t *sibling;

	while ((!node || node->color == RB_BLACK) && node != root->root) {
		if (node == parent->left) {
			sibling = parent->right;

			/* Case 1: sibling is RED */
			if (sibling->color == RB_RED) {
				sibling->color = RB_BLACK;
				parent->color  = RB_RED;
				rb_rotate_left(root, parent, augment, data);
				sibling = parent->right;
			}

			/* Case 2: sibling's children are both BLACK */
			if ((!sibling->left || sibling->left->color == RB_BLACK) &&
			    (!sibling->right || sibling->right->color == RB_BLACK)) {
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
			if (sibling->color == RB_RED) {
				sibling->color = RB_BLACK;
				parent->color  = RB_RED;
				rb_rotate_right(root, parent, augment, data);
				sibling = parent->left;
			}

			/* Case 2: sibling's children are both BLACK */
			if ((!sibling->left || sibling->left->color == RB_BLACK) &&
			    (!sibling->right || sibling->right->color == RB_BLACK)) {
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

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

void rb_init_root(rb_root_t *root)
{
	root->root     = NULL;
	root->leftmost = NULL;
}

rb_node_t *rb_first(rb_root_t *root)
{
	return root->leftmost;
}

rb_node_t *rb_next(rb_node_t *node)
{
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

int rb_is_empty(rb_root_t *root)
{
	return root->root == NULL;
}

void rb_insert_augmented(rb_root_t *root, rb_node_t *node,
                         rb_less_fn less, rb_augment_fn augment, void *data)
{
	rb_node_t **link = &root->root;
	rb_node_t  *parent = NULL;

	/* BST search for insertion point */
	while (*link) {
		parent = *link;
		if (less(node, parent)) {
			link = &parent->left;
		} else {
			link = &parent->right;
		}
	}

	/* Link the node */
	node->parent       = parent;
	node->left         = NULL;
	node->right        = NULL;
	node->color        = RB_RED;
	node->min_vruntime = 0;
	*link              = node;

	/* Update cached leftmost */
	if (!root->leftmost || less(node, root->leftmost))
		root->leftmost = node;

	/* Fix red-black violations */
	rb_insert_rebalance(root, node, augment, data);

	/* Propagate augmentation up from the inserted node */
	if (augment) augment_propagate(node, augment, data);
}

void rb_erase_augmented(rb_root_t *root, rb_node_t *node,
                        rb_augment_fn augment, void *data)
{
	rb_node_t *child, *rebalance_parent;
	rb_color_t color;

	/* Update cached leftmost */
	if (root->leftmost == node) root->leftmost = rb_next(node);

	/* Find the node to actually unlink: if node has two children,
	 * swap with the in-order successor (which has at most one child) */
	if (node->left && node->right) {
		rb_node_t *successor = rb_subtree_min(node->right);

		/* Unlink successor from its current position */
		child            = successor->right;
		rebalance_parent = successor->parent;
		color            = successor->color;

		if (child) child->parent = rebalance_parent;
		if (rebalance_parent) {
			if (successor == rebalance_parent->left)
				rebalance_parent->left = child;
			else
				rebalance_parent->right = child;
		}

		/* If successor was node's direct right child, adjust parent */
		if (rebalance_parent == node) rebalance_parent = successor;

		/* Transplant successor into node's position */
		successor->parent = node->parent;
		successor->left   = node->left;
		successor->right  = node->right;
		successor->color  = node->color;

		if (node->left)  node->left->parent  = successor;
		if (node->right) node->right->parent = successor;

		if (!node->parent) {
			root->root = successor;
		} else if (node == node->parent->left) {
			node->parent->left = successor;
		} else {
			node->parent->right = successor;
		}
	} else {
		/* Node has at most one child */
		child            = node->right ? node->right : node->left;
		rebalance_parent = node->parent;
		color            = node->color;

		if (child) child->parent = rebalance_parent;
		if (!rebalance_parent) {
			root->root = child;
		} else if (node == rebalance_parent->left) {
			rebalance_parent->left = child;
		} else {
			rebalance_parent->right = child;
		}
	}

	/* Propagate augmentation up from the rebalance parent */
	if (augment) augment_propagate(rebalance_parent, augment, data);

	/* Fix double-black violations */
	if (color == RB_BLACK)
		rb_erase_rebalance(root, child, rebalance_parent, augment, data);
}