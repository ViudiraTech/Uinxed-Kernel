/*
 *
 *		slist-strptr.h
 *		简单的单链表实现头文件
 *
 *		2024/11/24 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_SLIST_STRPTR_H_
#define INCLUDE_SLIST_STRPTR_H_

#include "ctypes.h"

typedef struct slist_sp *slist_sp_t;

struct slist_sp {
	char *key;
	void *val;
	slist_sp_t next;
};

slist_sp_t slist_sp_alloc(const char *key, void *val);
void slist_sp_free(slist_sp_t list);
void slist_sp_free_with(slist_sp_t list, void (*free_value)(void *));
slist_sp_t slist_sp_append(slist_sp_t list, const char *key, void *val);
slist_sp_t slist_sp_prepend(slist_sp_t list, const char *key, void *val);
void *slist_sp_get(slist_sp_t list, const char *key);
slist_sp_t slist_sp_get_node(slist_sp_t list, const char *key);
int slist_sp_search(slist_sp_t list, void *val, const char **key);
slist_sp_t slist_sp_search_node(slist_sp_t list, void *val);
slist_sp_t slist_sp_delete(slist_sp_t list, const char *key);
slist_sp_t slist_sp_delete_with(slist_sp_t list, const char *key, void (*free_value)(void *));
slist_sp_t slist_sp_delete_node(slist_sp_t list, slist_sp_t node);
slist_sp_t slist_sp_delete_node_with(slist_sp_t list, slist_sp_t node,void (*free_value)(void *));
size_t slist_sp_length(slist_sp_t list);
void slist_sp_print(slist_sp_t list);

#define slist_sp_foreach(list, node) for (slist_sp_t node = (list); node; node = node->next)

#endif // INCLUDE_SLIST_STRPTR_H_
