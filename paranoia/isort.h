/***
 * CopyPolicy: GNU Public License 2 applies
 * Copyright (C) by Monty (xiphmont@mit.edu)
 ***/

#ifndef _ISORT_H_
#define _ISORT_H_

#include "p_block.h"

typedef struct is_link {
  struct is_link *link;
  long           indice;
  struct is_link *parent;
} is_link;

typedef struct {
  long           begin; /* absolute offset for vectors */
  long           size;
  size16         *vector;

  /* sort structs */
  struct is_link **b_tails;  /* sort buckets */
  struct is_link **b_aux;    /* auxiliary storage */
  struct is_link **revindex; /* for incremental sorts */

  int             *sorted;   /* which sectors are sorted? */
  int             sortdirty;

  /* mem management */
  struct is_link *b_free;
  struct is_link *b_mem;
} is_vector;

extern void *isort_alloc();
extern void *isort_alloc_with_buffer(size16 *b,long size);
extern void isort_unsort(void *in);
extern void isort_free(void *in);
extern void isort_append(void *in, size16 *vector, long size);
extern void isort_insert(void *in,long pos,size16 *vector,long size);
extern void isort_overwrite(void *in,long pos,size16 *vector,long size);
extern void isort_remove(void *in,long cutpos,long cutsize);
extern void isort_removef(void *in, long cutpos);
extern long *isort_matches(long pos,int value, void *match,long overlap);
extern size16 *isort_yank(void *in);

#define isort_buffer(i) ((is_vector *)i?((is_vector *)i)->vector:NULL)
#define isort_size(i) ((is_vector *)i?((is_vector *)i)->size:0)
#define isort_begin(i) ((is_vector *)i?((is_vector *)i)->begin:-1)
#define isort_end(i) ((is_vector *)i?((is_vector *)i)->begin+\
		      ((is_vector *)i)->size:-1)
#define isort_set(i,b) (((is_vector *)i)->begin=b)

#endif

