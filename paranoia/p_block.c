#include <stdlib.h>
#include <stdio.h>
#include "../interface/cdda_interface.h"
#include "cdda_paranoia.h"
#include "p_block.h"

/* Get a new block and chain it */
c_block *new_c_block(cdrom_paranoia *p){
  c_block *b;
  c_list  *l=&(p->cache);
  long size_ptr=sizeof(c_block *);
  long size=sizeof(c_block);

  if(!l->free){
    /* Oops, need to add a link block. */
    int addto=32;

    if(l->pool)
      l->pool=realloc(l->pool,size_ptr*(l->blocks+1));
    else
      l->pool=malloc(size_ptr);

    l->pool[l->blocks]=l->free=calloc(addto,size);

    {
      int i;
      for(i=0;i<addto-1;i++){
	l->free[i].next=l->free+i+1;
	l->free[i].p=p;
	l->free[i].stamp=l->blocks*addto+i+1;
      }
      l->free[i].p=p;
      l->free[i].stamp=l->blocks*addto+i+1;
    }
    l->blocks++;
  }

  b=l->free;
  l->free=b->next;
  if(l->head)
    l->head->prev=b;
  else
    l->tail=b;    
  b->next=l->head;
  b->prev=NULL;
  l->head=b;
  l->current++;

  b->lastsector=0;
  return(b);
}

v_fragment *new_v_fragment(cdrom_paranoia *p){
  v_fragment *b;
  v_list *l=&(p->fragments);
  long size_ptr=sizeof(v_fragment *);
  long size=sizeof(v_fragment);

  l->active++;

  if(!l->free){
    /* Oops, need to add a link block. */
    int addto=32;

    if(l->pool)
      l->pool=realloc(l->pool,size_ptr*(l->blocks+1));
    else
      l->pool=malloc(size_ptr);

    l->pool[l->blocks]=l->free=calloc(addto,size);

    {
      int i;
      for(i=0;i<addto-1;i++){
	l->free[i].next=l->free+i+1;
	l->free[i].p=p;
	l->free[i].stamp=l->blocks*addto+i+1;
      }
      l->free[i].p=p;
      l->free[i].stamp=l->blocks*addto+i+1;
    }
    l->blocks++;
  }

  b=l->free;
  l->free=b->next;
  if(l->head)
    l->head->prev=b;
  else
    l->tail=b;    
  b->next=l->head;
  b->prev=NULL;
  l->head=b;

  b->lastsector=0;
  return(b);
}

void release_c_block(c_block *b){
  cdrom_paranoia *p=b->p;
  c_list *l=&(p->cache);

#ifdef NOISY
  fprintf(stderr,"Releasing c_block %d [%ld-%ld]\n",b->stamp,b->begin,b->end);
#endif

  if(b->buffer){
    free(b->buffer);
    free(b->flags);
    l->current--;
    b->buffer=NULL;
    b->flags=NULL;
  }

  if(b==l->head)
    l->head=b->next;
  if(b==l->tail)
    l->tail=b->prev;
    
  if(b->prev)
    b->prev->next=b->next;
  if(b->next)
    b->next->prev=b->prev;
    
  b->next=l->free;
  l->free=b;
}

void release_v_fragment(v_fragment *b){
  cdrom_paranoia *p=b->p;
  v_list *l=&(p->fragments);

  l->active--;

#ifdef NOISY
  fprintf(stderr,"Releasing v_fragment %d [%ld-%ld]\n",b->stamp,b->begin,
	  b->end);
#endif

   if(b==l->head)
    l->head=b->next;
  if(b==l->tail)
    l->tail=b->prev;
    
  if(b->prev)
    b->prev->next=b->next;
  if(b->next)
    b->next->prev=b->prev;
    
  b->next=l->free;
  b->one=NULL;
  b->two=NULL;
  l->free=b;
}

void recover_cache(cdrom_paranoia *p){
  c_list *l=&(p->cache);

  /* Are we at/over our allowed cache size? */
  while(l->current>l->limit){
    /* cull from the tail of the list */
    release_c_block(l->tail);
  }    
}

void recover_fragments(cdrom_paranoia *p){
  v_list *l=&(p->fragments);
  v_fragment *v=l->head;

  /* Is this verified thingie for a cached block that's been freed? */
  while(v){
    v_fragment *next=v->next;

    if(v->one->buffer==NULL)
      release_v_fragment(v);

    v=next;
  }    
}

size16 *v_buffer(v_fragment *v){
  size16 *buffer=v->one->buffer;
  long begin=v->one->begin;
    
  if(!buffer)return(NULL);
  return(buffer+v->begin-begin);
}
