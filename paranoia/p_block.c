#include <stdlib.h>
#include "../interface/cdda_interface.h"
#include "cdda_paranoia.h"
#include "p_block.h"

void release_p_block(p_block *b){
  cdrom_paranoia *p=b->p;

  /* yeah, not really needed */
  b->begin=-1;
  b->end=-1;
  b->verifybegin=-1;
  b->verifyend=-1;

  if(b->buffer){
    free(b->buffer);
    b->p->total_bufsize-=b->size;
    b->buffer=NULL;
  }
  b->size=0;

  if(b!=&(p->root)){
    if(b==p->fragments)
      p->fragments=b->next;
    if(b==p->tail)
      p->tail=b->prev;
    
    if(b->prev)
      b->prev->next=b->next;
    if(b->next)
      b->next->prev=b->prev;
    
    b->next=p->free;
    p->free=b;
  }
}

/* Get a new block and chain it */
p_block *new_p_block(cdrom_paranoia *p){
  p_block *b;

  /* Are we at/over our allowed cache size? */
  while(p->total_bufsize>p->cachemark){
    /* cull from the tail of the list */
    release_p_block(p->tail);
  }    

  if(!p->free){
    /* Oops, need to add a link block. */
    int addto=32;

    if(p->ptr)
      /* add link blocks */
      p->ptr=realloc(p->ptr,sizeof(p_block *)*(p->ptrblocks+1));
    else
      /* no link blocks yet */
      p->ptr=malloc(sizeof(p_block *));

    p->ptr[p->ptrblocks]=p->free=calloc(addto,sizeof(p_block));

    {
      int i;
      for(i=0;i<addto-1;i++){
	p->free[i].next=p->free+i+1;
	p->free[i].p=p;
	p->free[i].stamp=p->ptrblocks*addto+i+1;
      }
      p->free[i].p=p;
      p->free[i].stamp=p->ptrblocks*addto+i+1;
    }
    p->ptrblocks++;
  }

  b=p->free;
  p->free=b->next;

  if(p->fragments)
    p->fragments->prev=b;
  else
    p->tail=b;
    
  b->next=p->fragments;
  b->prev=NULL;
  p->fragments=b;

  b->begin=-1;
  b->end=-1;
  b->verifybegin=-1;
  b->verifyend=-1;
  b->size=0;
  b->silence=-1;
  b->offset=0;
  b->lastsector=0;
  b->done=0;

  return(b);
}

void swap_p_block(p_block *a,p_block *b){
  p_block t[1];

  /* I set things up so a straight memcpy isn't easy.  I'll fix that */
  t->buffer=a->buffer;
  t->size=a->size;
  t->begin=a->begin;
  t->end=a->end;
  t->verifybegin=a->verifybegin;
  t->verifyend=a->verifyend;
  t->silence=a->silence;
  t->lastsector=a->lastsector;
  t->done=a->done;
  t->offset=a->offset;

  a->buffer=b->buffer;
  a->size=b->size;
  a->begin=b->begin;
  a->end=b->end;
  a->verifybegin=b->verifybegin;
  a->verifyend=b->verifyend;
  a->silence=b->silence;
  a->lastsector=b->lastsector;
  a->done=b->done;
  a->offset=b->offset;

  b->buffer=t->buffer;
  b->size=t->size;
  b->begin=t->begin;
  b->end=t->end;
  b->verifybegin=t->verifybegin;
  b->verifyend=t->verifyend;
  b->silence=t->silence;
  b->lastsector=t->lastsector;
  b->done=t->done;
  b->offset=t->offset;
}

void p_buffer(p_block *b,size16 *buffer,long size){
  if(b->buffer){
    free(b->buffer);
    b->p->total_bufsize-=b->size;
  }

  b->buffer=buffer;
  b->size=size;
  b->p->total_bufsize+=size;
}

