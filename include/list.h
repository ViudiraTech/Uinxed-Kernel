/*
 *
 *		list.h
 *		基础链表管理头文件
 *
 *		2024/7/12 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_LIST_H_
#define INCLUDE_LIST_H_

#include "types.h"

struct ListCtl {
	struct List *start;
	struct List *end;
	int all;
};
struct List {
	struct ListCtl *ctl;
	struct List *prev;
	uintptr_t val;
	struct List *next;
};

typedef struct List List;

/* 根据给定的计数找到链表中对应位置的元素 */
struct List* FindForCount(size_t count, struct List* Obj);

/* 返回链表中的元素数量 */
int GetLastCount(struct List* Obj);

/* 销毁整个链表 */
void DeleteList(struct List* Obj);

/* 创建一个新的链表 */
struct List* NewList(void);

/* 更改链表中指定位置的元素值 */
void Change(size_t count, struct List* Obj, uintptr_t val);

/* 根据给定的计数删除链表中的元素 */
void DeleteVal(size_t count, struct List* Obj);

/* 在链表末尾添加一个新元素 */
void AddVal(uintptr_t val, struct List* Obj);

#endif // INCLUDE_LIST_H_
