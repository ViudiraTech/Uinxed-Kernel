#include "ilist.h"

void
ilist_init(struct ilist_node *list) {
  list->prev = list;
  list->next = list;
}

void
ilist_insert_before(struct ilist_node *anchor, struct ilist_node *node) {
  struct ilist_node *prev = anchor->prev;
  node->prev = prev;
  node->next = anchor;
  prev->next = node;
  anchor->prev = node;
}

void
ilist_remove(struct ilist_node *node) {
  node->prev->next = node->next;
  node->next->prev = node->prev;
}
