#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "csapp.h"
#include "cache.h"


/* cache_init initializes the input cache linked list. */
void cache_init(CacheList *list) {
  list->size = 0;
  list->first = NULL;
  list->last = NULL;
}

/* cache_URL adds a new cached item to the linked list. It takes the
 * URL being cached, a link to the content, the size of the content, and
 * the linked list being used. It creates a struct holding the metadata
 * and adds it at the beginning of the linked list.
 */
void cache_URL(const char *URL, const char *headers, void *item, size_t size, CacheList *list) {
  if (size > MAX_OBJECT_SIZE) {
    free(item);
    return;
  }

  // construct a new CachedItem
  CachedItem* new_item = malloc(sizeof(CachedItem));
  new_item->url = strdup(URL);
  new_item->headers = strdup(headers);
  new_item->item_p = item;
  new_item->size = size;
  new_item->prev = new_item->next = NULL;

  // make sure there are enough space for new node
  while (list->size + size > MAX_CACHE_SIZE) {
    // delete the last node
    list->size -= list->last->size;

    CachedItem* temp_last = list->last;
    list->last = list->last->prev;
    list->last->next = NULL;
    free(temp_last->url);
    free(temp_last->headers);
    free(temp_last->item_p);
    free(temp_last);
  }

  // insert the new node
  if (list->first != NULL) {
    new_item->next = list->first;
    list->first->prev = new_item;
    list->first = new_item;
  } else {
    list->first = new_item;
    list->last = new_item;
  }

  list->size += size;
}


/* find iterates through the linked list and returns a pointer to the
 * struct associated with the requested URL. If the requested URL is
 * not cached, it returns null.
 */
CachedItem *find(const char *URL, CacheList *list) {
  // try to find the url first
  CachedItem *temp = list->first;
  while(temp) {
    if (!strcmp(temp->url, URL)) break;
    temp = temp->next;
  }

  if (temp == NULL) return NULL; // nothing found

  // take it away from middle
  if (temp->prev) temp->prev->next = temp->next;
  if (temp->next) temp->next->prev = temp->prev;

  if (temp == list->first) list->first = temp->next;
  if (temp == list->last) list->last = temp->prev;

  temp->next = temp->prev = NULL;

  // insert it to the front
  if (list->first != NULL) {
    temp->next = list->first;
    list->first->prev = temp;
    list->first = temp;
  } else {
    list->first = temp;
    list->last = temp;
  }

  return temp;
}


/* frees the memory used to store each cached object, and frees the struct
 * used to store its metadata. */
void cache_destruct(CacheList *list) {
  CachedItem *temp = list->first;
  while (temp) {
    free(temp->url);
    free(temp->headers);
    free(temp->item_p);
    if (temp->next) {
      temp = temp->next;
      free(temp->prev);
    } else {
      free(temp);
      break;
    }
  }
}
