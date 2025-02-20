/*
 *
 *		rbtree-strptr.h
 *		红黑树实现头文件
 *
 *		2024/11/24 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，开放所有权利。
 *
 */

#ifndef INCLUDE_RBTREE_STRPTR_H_
#define INCLUDE_RBTREE_STRPTR_H_

#ifndef _RBTREE_ENUM_
#	define _RBTREE_ENUM_

enum {
	RBT_RED,	// 红色节点
	RBT_BLACK	// 黑色节点
};

#endif

#include "ctypes.h"
#include "slist-strptr.h"

typedef struct rbtree_sp *rbtree_sp_t;

struct rbtree_sp {
	uint32_t    color;	/**< 节点颜色，取值为 RED 或 BLACK */
	uint32_t    hash;	/**< 节点键哈希值 */
	slist_sp_t  list;	/**< 节点值 */
	rbtree_sp_t left;	/**< 左子节点指针 */
	rbtree_sp_t right;	/**< 右子节点指针 */
	rbtree_sp_t parent;	/**< 父节点指针 */
};

rbtree_sp_t rbtree_sp_alloc(const char *key, void *value);
void rbtree_sp_free(rbtree_sp_t root) ;
void rbtree_sp_free_with(rbtree_sp_t root, void (*free_value)(void *)) ;
void *rbtree_sp_get(rbtree_sp_t root, const char *key) ;
int rbtree_sp_search(rbtree_sp_t root, void *value, const char **key) ;
rbtree_sp_t rbtree_sp_insert(rbtree_sp_t root, const char *key, void *value);
rbtree_sp_t rbtree_sp_delete(rbtree_sp_t root, const char *key);
void rbtree_sp_print_inorder(rbtree_sp_t root) ;
void rbtree_sp_print_preorder(rbtree_sp_t root) ;
void rbtree_sp_print_postorder(rbtree_sp_t root) ;
rbtree_sp_t rbtree_sp_left_rotate(rbtree_sp_t root, rbtree_sp_t x);
rbtree_sp_t rbtree_sp_right_rotate(rbtree_sp_t root, rbtree_sp_t y);
rbtree_sp_t rbtree_sp_transplant(rbtree_sp_t root, rbtree_sp_t u, rbtree_sp_t v);
rbtree_sp_t rbtree_sp_insert_fixup(rbtree_sp_t root, rbtree_sp_t z);
rbtree_sp_t rbtree_sp_delete_fixup(rbtree_sp_t root, rbtree_sp_t x, rbtree_sp_t x_parent);
uint32_t rbtree_sp_hash(const char *str);
rbtree_sp_t rbtree_sp_get_node(rbtree_sp_t root, const char *key);
rbtree_sp_t rbtree_sp_min(rbtree_sp_t root);
rbtree_sp_t rbtree_sp_max(rbtree_sp_t root);

#endif // INCLUDE_RBTREE_STRPTR_H_
