#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "p_block.h"
#include "../interface/cdda_interface.h"
#include "cdda_paranoia.h"
#include "isort.h"

linked_list *new_list(void *(*newp)(void),void (*freep)(void *)){
  linked_list *ret=calloc(1,sizeof(linked_list));
  ret->new_poly=newp;
  ret->free_poly=freep;
  return(ret);
}

linked_element *add_elem(linked_list *l,void *elem){

  linked_element *ret=calloc(1,sizeof(linked_element));
  ret->stamp=l->current++;
  ret->ptr=elem;
  ret->list=l;

  if(l->head)
    l->head->prev=ret;
  else
    l->tail=ret;    
  ret->next=l->head;
  ret->prev=NULL;
  l->head=ret;
  l->active++;

  return(ret);
}

linked_element *new_elem(linked_list *list){
  void *new=list->new_poly();
  return(add_elem(list,new));
}

void free_elem(linked_element *e,int free_ptr){
  linked_list *l=e->list;
  if(free_ptr)l->free_poly(e->ptr);

  if(e==l->head)
    l->head=e->next;
  if(e==l->tail)
    l->tail=e->prev;
    
  if(e->prev)
    e->prev->next=e->next;
  if(e->next)
    e->next->prev=e->prev;

  l->active--;
  free(e);
} 

void free_list(linked_list *list,int free_ptr){
  while(list->head)
    free_elem(list->head,free_ptr);
  free(list);
}

void *get_elem(linked_element *e){
  return(e->ptr);
}

linked_list *copy_list(linked_list *list){
  linked_list *new=new_list(list->new_poly,list->free_poly);
  linked_element *i=list->tail;

  while(i){
    add_elem(new,i->ptr);
    i=i->prev;
  }
  return(new);
}

/**** C_block stuff ******************************************************/

static c_block *i_cblock_constructor(cdrom_paranoia *p){
  c_block *ret=calloc(1,sizeof(c_block));
  return(ret);
}

static void i_cblock_destructor(c_block *c){
  if(c->vector)isort_free(c->vector);
  if(c->flags)free(c->flags);
  c->e=NULL;
  free(c);
}

c_block *new_c_block(cdrom_paranoia *p){
  linked_element *e=new_elem(p->cache);
  c_block *c=e->ptr;
  c->e=e;
  c->p=p;
  return(c);
}

void free_c_block(c_block *c){
  /* also rid ourselves of v_fragments that reference this block */
  v_fragment *v=v_first(c->p);
  
  while(v){
    v_fragment *next=v_next(v);
    if(v->one==c)free_v_fragment(v);
    v=next;
  }    

  free_elem(c->e,1);
}

static v_fragment *i_vfragment_constructor(void){
  v_fragment *ret=calloc(1,sizeof(v_fragment));
  return(ret);
}

static void i_v_fragment_destructor(v_fragment *v){
  free(v);
}

v_fragment *new_v_fragment(cdrom_paranoia *p,c_block *one,
			   long begin, long end, int last){
  linked_element *e=new_elem(p->fragments);
  v_fragment *b=e->ptr;
  
  b->e=e;
  b->p=p;

  b->one=one;
  b->begin=begin;
  b->size=end-begin;
  b->lastsector=last;

  return(b);
}

void free_v_fragment(v_fragment *v){
  free_elem(v->e,1);
}

c_block *c_first(cdrom_paranoia *p){
  if(p->cache->head)
    return(p->cache->head->ptr);
  return(NULL);
}

c_block *c_last(cdrom_paranoia *p){
  if(p->cache->tail)
    return(p->cache->tail->ptr);
  return(NULL);
}

c_block *c_next(c_block *c){
  if(c->e->next)
    return(c->e->next->ptr);
  return(NULL);
}

c_block *c_prev(c_block *c){
  if(c->e->prev)
    return(c->e->prev->ptr);
  return(NULL);
}

v_fragment *v_first(cdrom_paranoia *p){
  if(p->fragments->head)
    return(p->fragments->head->ptr);
  return(NULL);
}

v_fragment *v_last(cdrom_paranoia *p){
  if(p->fragments->tail)
    return(p->fragments->tail->ptr);
  return(NULL);
}

v_fragment *v_next(v_fragment *v){
  if(v->e->next)
    return(v->e->next->ptr);
  return(NULL);
}

v_fragment *v_prev(v_fragment *v){
  if(v->e->prev)
    return(v->e->prev->ptr);
  return(NULL);
}

void recover_cache(cdrom_paranoia *p){
  linked_list *l=p->cache;

  /* Are we at/over our allowed cache size? */
  while(l->active>p->cache_limit)
    /* cull from the tail of the list */
    free_c_block(c_last(p));

}

size16 *v_buffer(v_fragment *v){
  size16 *buffer=isort_buffer(v->one->vector);
  long begin=isort_begin(v->one->vector);
    
  if(!buffer)return(NULL);
  return(buffer+v->begin-begin);
}

/**** Initialization *************************************************/

void i_paranoia_firstlast(cdrom_paranoia *p){
  int i;
  cdrom_drive *d=p->d;
  p->current_lastsector=-1;
  for(i=cdda_sector_gettrack(d,p->cursor);i<cdda_tracks(d);i++)
    if(!cdda_track_audiop(d,i))
      p->current_lastsector=cdda_track_lastsector(d,i-1);
  if(p->current_lastsector==-1)
    p->current_lastsector=cdda_disc_lastsector(d);

  p->current_firstsector=-1;
  for(i=cdda_sector_gettrack(d,p->cursor);i>0;i--)
    if(!cdda_track_audiop(d,i))
      p->current_firstsector=cdda_track_firstsector(d,i+1);
  if(p->current_firstsector==-1)
    p->current_firstsector=cdda_disc_firstsector(d);

}

cdrom_paranoia *paranoia_init(cdrom_drive *d){
  cdrom_paranoia *p=calloc(1,sizeof(cdrom_paranoia));

  p->cache=new_list((void *)&i_cblock_constructor,
		    (void *)&i_cblock_destructor);

  p->fragments=new_list((void *)&i_vfragment_constructor,
			(void *)&i_v_fragment_destructor);

  p->d=d;
  p->readahead=150;
  p->dynoverlap=512;
  p->cache_limit=JIGGLE_MODULO;
  p->enable=PARANOIA_MODE_FULL;
  p->cursor=cdda_disc_firstsector(d);
  p->lastread=LONG_MAX;

  /* One last one... in case data and audio tracks are mixed... */
  i_paranoia_firstlast(p);

  return(p);
}

#ifdef TEST

#undef TEST
#include "isort.c"

typedef struct dummyel{
  int foo;
} dummyel;

dummyel *new_dummy(){
  return(calloc(1,sizeof(dummyel)));
}

int main(){
  linked_list *tl;
  linked_element *one,*two,*three,*four;

  tl=new_list((void *)&new_dummy,&free);
  one=new_elem(tl);
  two=new_elem(tl);
  three=new_elem(tl);
  four=new_elem(tl);

  if(tl->head!=four){
    printf("linked lists: failed test 1\n");
    return(1);
  }
  if(tl->tail!=one){
    printf("linked lists: failed test 2\n");
    return(1);
  }
  if(tl->active!=4){
    printf("linked lists: failed test 3\n");
    return(1);
  }

  free_elem(one,1);
  free_elem(four,1);

  if(tl->head!=three){
    printf("linked lists: failed test 4\n");
    return(1);
  }
  if(tl->tail!=two){
    printf("linked lists: failed test 5\n");
    return(1);
  }


  if(tl->active!=2){
    printf("linked lists: failed test 6\n");
    return(1);
  }
  one=new_elem(tl);
  if(tl->active!=3){
    printf("linked lists: failed test 7\n");
    return(1);
  }

  free_list(tl,1);

  return(0);
}

#endif
