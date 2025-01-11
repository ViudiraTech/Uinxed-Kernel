#ifndef INCLUDE_ILIST_H_
#define INCLUDE_ILIST_H_

// Intrusive Linked List

#include "types.h"

struct ilist_node {
  struct ilist_node *prev;
  struct ilist_node *next;
};

#define container_of(ptr,type,member) ((type *)(((uintptr_t)ptr)-__builtin_offsetof(type,member)))

#define ILIST_FOREACH(var,list) for (struct ilist_node *var=list.next; var!=&list; var=var->next)

void ilist_init(struct ilist_node *list);
void ilist_insert_before(struct ilist_node *anchor, struct ilist_node *node);
void ilist_remove(struct ilist_node *node);

#endif // INCLUDE_ILIST_H_
