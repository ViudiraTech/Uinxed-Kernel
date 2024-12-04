/*
 *
 *		slist-strptr.c
 *		简单的单链表实现
 *
 *		2024/11/24 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "slist-strptr.h"
#include "string.h"
#include "memory.h"
#include "printk.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wint-conversion"

slist_sp_t slist_sp_alloc(const char *key, void *val)
{
	slist_sp_t node = kmalloc(sizeof(*node));
	if (node == 0) return 0;
	node->key  = key ? strdup(key) : 0;
	node->val  = val;
	node->next = 0;
	return node;
}

#pragma GCC diagnostic pop

void slist_sp_free(slist_sp_t list)
{
	while (list != 0) {
		slist_sp_t next = list->next;
		kfree(list->key);
		kfree(list);
		list = next;
	}
}

void slist_sp_free_with(slist_sp_t list, void (*free_value)(void *))
{
	while (list != 0) {
		slist_sp_t next = list->next;
		kfree(list->key);
		free_value(list->val);
		kfree(list);
		list = next;
	}
}

slist_sp_t slist_sp_append(slist_sp_t list, const char *key, void *val)
{
	slist_sp_t node = slist_sp_alloc(key, val);
	if (node == 0) return list;
	if (list == 0) {
		list = node;
	} else {
		slist_sp_t current = list;
		while (current->next != 0) {
			current = current->next;
		}
		current->next = node;
	}
	return list;
}

slist_sp_t slist_sp_prepend(slist_sp_t list, const char *key, void *val)
{
	slist_sp_t node = slist_sp_alloc(key, val);
	if (node == 0) return list;
	node->next = list;
	list       = node;
	return list;
}

void *slist_sp_get(slist_sp_t list, const char *key)
{
	for (slist_sp_t current = list; current; current = current->next) {
		if (streq(current->key, key)) return current->val;
	}
	return 0;
}

slist_sp_t slist_sp_get_node(slist_sp_t list, const char *key)
{
	for (slist_sp_t current = list; current; current = current->next) {
		if (streq(current->key, key)) return current;
	}
	return 0;
}

int slist_sp_search(slist_sp_t list, void *val, const char **key)
{
	for (slist_sp_t current = list; current; current = current->next) {
		if (current->val == val) {
			if (key) *key = current->key;
			return 1;
		}
	}
	return 0;
}

slist_sp_t slist_sp_search_node(slist_sp_t list, void *val)
{
	for (slist_sp_t current = list; current; current = current->next) {
		if (current->val == val) return current;
	}
	return 0;
}

slist_sp_t slist_sp_delete(slist_sp_t list, const char *key)
{
	if (list == NULL) return 0;
	if (streq(list->key, key)) {
		slist_sp_t temp = list;
		list            = list->next;
		kfree(temp->key);
		kfree(temp);
		return list;
	}
	slist_sp_t prev = list;
	for (slist_sp_t current = list->next; current != 0; current = current->next) {
		if (streq(current->key, key)) {
			prev->next = current->next;
			kfree(current->key);
			kfree(current);
			break;
		}
		prev = current;
	}
	return list;
}

slist_sp_t slist_sp_delete_with(slist_sp_t list, const char *key, void (*free_value)(void *))
{
	if (list == 0) return 0;
	if (streq(list->key, key)) {
		slist_sp_t temp = list;
		list            = list->next;
		kfree(temp->key);
		free_value(temp->val);
		kfree(temp);
		return list;
	}
	slist_sp_t prev = list;
	for (slist_sp_t current = list->next; current != 0; current = current->next) {
		if (streq(current->key, key)) {
			prev->next = current->next;
			kfree(current->key);
			free_value(current->val);
			kfree(current);
			break;
		}
		prev = current;
	}
	return list;
}

slist_sp_t slist_sp_delete_node(slist_sp_t list, slist_sp_t node)
{
	if (list == 0 || node == 0) return list;
	if (list == node) {
		slist_sp_t temp = list;
		list            = list->next;
		kfree(temp->key);
		kfree(temp);
		return list;
	}
	slist_sp_t prev = list;
	for (slist_sp_t current = list->next; current != 0; current = current->next) {
		if (current == node) {
			prev->next = current->next;
			kfree(current->key);
			kfree(current);
			break;
		}
		prev = current;
	}
	return list;
}

slist_sp_t slist_sp_delete_node_with(slist_sp_t list, slist_sp_t node, void (*free_value)(void *))
{
	if (list == 0 || node == 0) return list;
	if (list == node) {
		slist_sp_t temp = list;
		list            = list->next;
		kfree(temp->key);
		free_value(temp->val);
		kfree(temp);
		return list;
	}
	slist_sp_t prev = list;
	for (slist_sp_t current = list->next; current != 0; current = current->next) {
		if (current == node) {
			prev->next = current->next;
			kfree(current->key);
			free_value(current->val);
			kfree(current);
			break;
		}
		prev = current;
	}
	return list;
}

size_t slist_sp_length(slist_sp_t slist_sp)
{
	size_t count   = 0;
	slist_sp_t current = slist_sp;
	while (current != 0) {
		count++;
		current = current->next;
	}
	return count;
}

void slist_sp_print(slist_sp_t slist_sp)
{
	slist_sp_t current = slist_sp;
	while (current != 0) {
		printk("%p -> ", current->val);
		current = current->next;
	}
	printk("NULL\n");
}
