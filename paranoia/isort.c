/***
 * CopyPolicy: GNU Public License 2 applies
 * Copyright (C) by Monty (xiphmont@mit.edu)
 *
 * Lazy, incremental sorting code for paranoia
 *
 ***/

/* the isort abstraction supports five basic opts; 
   appending new data onto the end of the vector; linear time on appended size
   chopping data from the front of the vector; linear time on remaining size
   inserting data into the middle of the vector; linear time on end size
   deleting data from the middle of the vector; linear time on end size 
   overwriting data in the middle of the vector; linear time on modify size */

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "isort.h"

#define IS_BLOCKSIZE 256
#define IS_BLOCKBITS 8
#define IS_BLOCKMASK 0xff

#ifdef TEST
#define DEBUG
#endif

#ifdef DEBUG
#define SC(i,p) sanity_check(i,p)
#include <stdio.h>

static void sanity_check(is_vector *i,char *prompt){
  long linksseen=0;
  long allocedlinks=0;
  /* All the checks! */

  /* Check free list */
  {
    is_link *ptr=i->b_free;
    while(ptr){
      linksseen++;
      ptr=ptr->link;
    }
  }
    
  /* Check mem list connectivity */
  {
    is_link *ptr=i->b_mem;
    while(ptr){
      allocedlinks+=ptr->indice;
      ptr=ptr->link;
    }
  }

  /* check that aux is all empty */
  if(i->b_aux){
    long j;
    for(j=0;j<65536;j++)
      if(i->b_aux[j]!=NULL){
	fprintf(stderr,"SanityCheck %s: auxiliary list nonempty (%ld)\n",
		prompt,j);
	exit(1);
      }
  }

  /* if no sorted sectors, no structs */

  {
    if(!((i->revindex==NULL && i->b_tails==NULL && i->b_aux==NULL &&
	  i->b_mem==NULL)||
	 (i->revindex!=NULL && i->b_tails!=NULL && i->b_aux!=NULL &&
	  i->b_mem!=NULL))){
      fprintf(stderr,"SanityCheck %s: sorting structs incomplete\n",prompt);
      exit(1);
    }
  }
  
  /* check that *all* members of a sorted sector have revindexes. */
  {
    int sectors=((i->size-1)>>IS_BLOCKBITS)+1;
    long j,k;
    
    for(j=0;j<sectors;j++)
      if(i->sorted[j])
	/* sorted! */
	for(k=0;k<IS_BLOCKSIZE;k++)
	  if((j<<IS_BLOCKBITS)+k<i->size && 
	     i->revindex[(j<<IS_BLOCKBITS)+k]==NULL){
	    fprintf(stderr,"SanityCheck %s: sorted sector contains NULL "
		    "revindex @ %ld\n",prompt,(j<<IS_BLOCKBITS)+k);
	    exit(1);
	  }
  }

  /* Check that revindex points to link that points back */
  if(i->revindex){
    long j;
    for(j=0;j<i->size;j++){
      is_link *ptr=i->revindex[j];
      if(ptr!=NULL && ptr->indice!=j){
	fprintf(stderr,"SanityCheck %s: incorect revindex->link->revindex "
		"connection\n",prompt);
	exit(1);
      }
    }
  }

  /* Check that link points to revindex that points back */
  /* Check link chain integrity */
  /* Check indicies are ascending */
  if(i->b_tails){
    long j;
    for(j=0;j<65536;j++){
      is_link *ptr=i->b_tails[j];

      while(ptr){
	if(i->revindex){
	  if(i->revindex[ptr->indice]!=ptr){
	    fprintf(stderr,"SanityCheck %s: incorrect link->revindex->link "
		    "connection\n",prompt);
	    exit(1);
	  }
	}

	if(ptr->link && ptr->link->parent!=ptr){
	  fprintf(stderr,"SanityCheck %s: incorrect link->link->parent "
		  "connection\n",prompt);
	  exit(1);
	}

	linksseen++;
	ptr=ptr->link;
      }
    }
  }    
  
  if(linksseen!=allocedlinks){
    fprintf(stderr,"SanityCheck %s: link leak (%ld alloced, %ld counted)\n",
	    prompt,allocedlinks,linksseen);
    exit(1);
  }
}

#else
#define SC(i,p) 
#endif

/* Get some new data onto the free list.  This has a cute tracking hack */
static inline void isort_expand(is_vector *i,long addto){
  is_link *ptr=calloc(addto+1,sizeof(is_link));
  long j;

  /* The head of this thing is linked onto b_mem such that they can be
     freed later.  The rest are chained onto b_free. */

  ptr[0].link=i->b_mem;
  ptr[0].indice=addto;
  i->b_mem=ptr;

  ptr[addto].link=i->b_free;
  i->b_free=&(ptr[1]);
  for(j=1;j<addto;j++)ptr[j].link=&(ptr[j+1]);
}

static inline is_link *isort_newlink(is_vector *i){
  is_link *ret;
  if(i->b_free==NULL)isort_expand(i,1176);

  ret=i->b_free;
  i->b_free=ret->link;
  return(ret);
}

/* if we want to free list member (n), we pass a ptr to (n-1) */
static inline void isort_freelink(is_vector *i,int value,is_link *link){
  is_link *prev=link->parent;

  if(prev==NULL)
    i->b_tails[value]=link->link;
  else
    prev->link=link->link;

  if(link->link)link->link->parent=prev;
  link->link=i->b_free;
  i->b_free=link;  
  SC(i,"freelink");
}

/* frees a single link or links */
static void inline isort_freefrom_heads(is_vector *i,long value,
					is_link *head, is_link *link){
  if(link->parent==NULL)
    i->b_tails[value]=NULL;
  else
    link->parent->link=NULL;

  head->link=i->b_free;
  i->b_free=link;  
}

static int range_index(long el){
  return(el>>IS_BLOCKBITS);
}

static int range_blocks(long size){
  return(((size-1)>>IS_BLOCKBITS)+1);
}

static void update_range(is_vector *i){
  long blocks=range_blocks(i->size);
  long j,acc=0;
  
  for(j=blocks-1;j>=0;j--)
    if(i->sorted[j])
      i->sorted[j]=++acc;
    else 
      acc=0;
  i->sortdirty=0;
}

static void range_alloc(is_vector *i,long addto){
  long newsize=i->size+addto;
  long oldblocks=range_blocks(i->size);
  long newblocks=range_blocks(newsize);
  
  if(newblocks>oldblocks)
    i->sorted=realloc(i->sorted,newsize*sizeof(int));

}

/* added to the end of the vector 

A)
|1234|5678|9012|345-|
|1234|5678|9012|3456|

B)
|1234|5678|9012|34--|
|1234|5678|9012|345-|

C)
|1234|5678|9012|34--|
|1234|5678|9012|3456|7890|1---|
   
*/

static void isort_append_range(is_vector *i,long addto){
  long newsize=i->size+addto;
  long oldblocks=range_blocks(i->size);
  long newblocks=range_blocks(newsize);
  long j;
  
  if(i->size&IS_BLOCKMASK)i->sorted[oldblocks-1]=0; /* added new unsorted
						       stuff to a partial 
						       sector */
  range_alloc(i,addto);
  for(j=oldblocks;j<newblocks;j++)i->sorted[j]=0;
  i->sortdirty=1;
}

/* vector addition; we need to check/update sorted in addition to
   performing sort 

A)
|1234|5678|9012|345-|
|12--|3456|7890|1234|5---|

B)
|1234|5678|9012|34--|
|12--|3456|7890|1234|

C)
|1234|5678|9012|34--|
|12--|----|-345|6789|0123|4---|
   ^ibegin                ^end
   
   */

static void isort_insert_range(is_vector *i,long ibegin,long size){
  int ibeginsec=range_index(ibegin);
  int endsec=range_index(i->size+size-1);
  int j;

  range_alloc(i,size);
  for(j=ibeginsec;j<=endsec;j++)i->sorted[j]=0;
  i->sortdirty=1;
}

/* spackle

A)
|1234|5678|9012|345-|
|123X|XXXX|X012|345-|
    ^      ^
*/

static void isort_overwrite_range(is_vector *i,long ibegin,long size){
  int ibeginsec=range_index(ibegin);
  int iendsec=range_index(ibegin+size-1);
  int j;

  for(j=ibeginsec;j<=iendsec;j++)i->sorted[j]=0;
  i->sortdirty=1;
}

/* 
A)
|123X|XXXX|X012|345-|
|1230|1234|5---|
    ^      ^ 
*/

static void isort_remove_range(is_vector *i,long ibegin,long size){
  int ibeginsec=range_index(ibegin);
  int endsec=range_index(i->size-size-1);
  int j;

  for(j=ibeginsec;j<=endsec;j++)i->sorted[j]=0;
  i->sortdirty=1;
}

/* does the actual sorting work */
static void isort_work(is_vector *i,long begin,long end){
  long j;
  int *marker=malloc(65536*sizeof(int));
  long markptr=0;
    
  if(end>i->size)end=i->size; 
  for(j=begin;j<end;j++){
    if(i->revindex[j]==0){
      /* this guy needs to be sorted */
      int value=i->vector[j]+32768;
      is_link *link=isort_newlink(i);

      if(i->b_aux[value]==NULL){

	/* our aux list points to the old tail.  tails points to the
	   insertion point. This is so aux is always nonnull for a
	   value we've already searched */

	is_link *prev=NULL,*ptr=i->b_aux[value]=i->b_tails[value];
	marker[markptr++]=value;
	
	while(ptr && ptr->indice>j){
	  prev=ptr;
	  ptr=ptr->link;
	}
	
	i->b_tails[value]=prev;
      }

      if(i->b_tails[value]==NULL){
	/* we need to insert at the beginning of the list */

	link->link=i->b_aux[value];
	i->b_aux[value]=link;
	link->parent=NULL;
	if(link->link)link->link->parent=link;

      }else{
	link->parent=i->b_tails[value];
	link->link=link->parent->link;
	link->parent->link=link;
	if(link->link)link->link->parent=link;
      }
      link->indice=j;

      i->revindex[j]=link;
    }
  }

  /* move the sorted lists back. */
      
  for(j=0;j<markptr;j++){
    int value=marker[j];
    i->b_tails[value]=i->b_aux[value];
    i->b_aux[value]=NULL;
  }
  free(marker);
}

/* no vector additions, sorted is up to date, but a request requires
   sorted data from an area that is potentially unsorted as yet */
static void isort_assert_sort(is_vector *i,
				       long begin,long size){
  long spanbegin=range_index(begin);
  long spanend=range_index(begin+size-1)+1;
  long anyflag=0;

  if(i->sortdirty)update_range(i);
  if(i->sorted[spanbegin]<spanend-spanbegin){
    if(i->revindex==NULL)i->revindex=calloc(i->size,sizeof(is_link *));

    if(i->b_tails==NULL)i->b_tails=calloc(65536,sizeof(is_link *));
    if(i->b_aux==NULL)i->b_aux=calloc(65536,sizeof(is_link *));

    while(spanbegin<spanend){
      long thisend=spanbegin+=i->sorted[spanbegin];
      while(thisend<spanend && i->sorted[thisend]==0){
	i->sorted[thisend]=1;
	thisend++;
	i->sortdirty=1;
      }

      if(spanbegin<spanend){
	isort_work(i,spanbegin<<IS_BLOCKBITS,thisend<<IS_BLOCKBITS);
	anyflag=1;
      }

      spanbegin=thisend;
    }
  }
  if(anyflag)SC(i,"perform_sort");
}

/**** Sorting code ********************************************************/

/* frees and resets sort related storage */
void isort_unsort(void *in){
  is_vector *i=(is_vector *)in;
  if(i->revindex){
    free(i->revindex);
    i->revindex=NULL;
  }
  {
    is_link *ptr;
    
    /* free all the link memory we alloced */
    ptr=i->b_mem;
    while(ptr){
      is_link *next=ptr->link;
      free(ptr);
      ptr=next;
    }

    i->b_mem=NULL;
    i->b_free=NULL;
  }
  if(i->b_aux){
    free(i->b_aux);
    i->b_aux=NULL;
  }
  if(i->b_tails){
    free(i->b_tails);
    i->b_tails=NULL;
  }
  {
    long j=range_blocks(i->size);
    for(;j>0;j--)i->sorted[j-1]=0;
  }
  i->sortdirty=0;

  SC(i,"unsort");
}

void *isort_alloc(){
  is_vector *ret=calloc(1,sizeof(is_vector));
  SC(ret,"alloc");
  return((void *)ret);
}

void *isort_alloc_with_buffer(size16 *buffer,long size){
  is_vector *ret=calloc(1,sizeof(is_vector));
  isort_append_range(ret,size);
  ret->vector=buffer;
  ret->size=size;
  SC(ret,"alloc_with_buffer");
  return((void *)ret);
}

void isort_free(void *in){
  is_vector *i=(is_vector *)in;
 
  if(i){
    isort_unsort(i);
    if(i->vector)free(i->vector);
    if(i->sorted)free(i->sorted);
    free(i);
  }
}

size16 *isort_yank(void *in){
  is_vector *i=(is_vector *)in;
  size16 *ret=i->vector;
  i->vector=NULL;
  isort_free(i);
  return(ret);
}

void isort_append(void *in, size16 *vector, long size){
  is_vector *i=(is_vector *)in;
  isort_append_range(i,size);

  /* update the vector */
  if(i->vector)
    i->vector=realloc(i->vector,sizeof(size16)*(size+i->size));
  else
    i->vector=malloc(sizeof(size16)*size);
  memcpy(i->vector+i->size,vector,sizeof(size16)*size);

  if(i->revindex){
    i->revindex=realloc(i->revindex,sizeof(is_link *)*(size+i->size));
    bzero(i->revindex+i->size,size*sizeof(is_link *));
  }

  i->size+=size;
  SC(i,"append");
}

void isort_insert(void *in,long pos,size16 *vector,long size){
  is_vector *i=(is_vector *)in;

  if(pos<0 || pos>i->size)return;
  if(pos==i->size)return(isort_append(i,vector,size));

  isort_insert_range(i,pos,size);

  if(i->vector)
    i->vector=realloc(i->vector,sizeof(size16)*(size+i->size));
  else
    i->vector=malloc(sizeof(size16)*size);
  if(pos<i->size)memmove(i->vector+pos+size,i->vector+pos,
			 (i->size-pos)*sizeof(size16));
  memcpy(i->vector+pos,vector,size*sizeof(size16));
  
  if(i->revindex){
    /* Preupdate trailing revindex */
    {
      long j;
      for(j=pos;j<i->size;j++)
	if(i->revindex[j])i->revindex[j]->indice+=size;
    }

    i->revindex=realloc(i->revindex,sizeof(is_link *)*(size+i->size));
    if(pos<i->size)memmove(i->revindex+pos+size,i->revindex+pos,
			     (i->size-pos)*sizeof(is_link *));
    bzero(i->revindex+pos,size*sizeof(is_link *));

  }

  i->size+=size;
  SC(i,"insert");
}

void isort_overwrite(void *in,long pos,size16 *vector,long size){
  is_vector *i=(is_vector *)in;

  if(pos<0)return;
  if(pos+size>i->size)size=i->size-pos;

  /* optimize me later */
  isort_remove(i,pos,size);
  isort_insert(i,pos,vector,size);
  SC(i,"overwrite");
}

void isort_removef(void *in, long cutpos){
  is_vector *i=(is_vector *)in;

  if(cutpos>=i->size || cutpos<0)cutpos=i->size;

  isort_remove_range(i,0,cutpos);

  /* first update the buckets */
  if(i->revindex){
    long j;

    /* buckets first */
    for(j=cutpos-1;j>=0;j--){
      if(i->revindex[j]!=NULL){
	int value=i->vector[j]+32768;
	is_link *t=i->revindex[j];
	is_link *ptr=t;

	while(ptr){
	  i->revindex[ptr->indice]=NULL;
	  if(ptr->link==NULL)break;
	  ptr=ptr->link;
	}	

	isort_freefrom_heads(i,value,ptr,t);
      }
    }

    memmove(i->revindex,i->revindex+cutpos,(i->size-cutpos)*
	    sizeof(is_link *));
    
    /* Follow revindex to update bucket forward indices */
    for(j=0;j<i->size-cutpos;j++)
      if(i->revindex[j])i->revindex[j]->indice=j;
  }

  /* Move vector memory */
  if(i->size-cutpos>0)
    memmove(i->vector,i->vector+cutpos,(i->size-cutpos)*sizeof(size16));

  i->size-=cutpos;
  i->begin+=cutpos;

  SC(i,"removef");
}

void isort_remove(void *in,long cutpos,long cutsize){
  is_vector *i=(is_vector *)in;

  if(cutpos<0)cutpos=0;
  if(cutsize<0 || cutpos+cutsize>i->size)cutsize=i->size-cutpos;
  if(cutsize<1)return;

  isort_remove_range(i,cutpos,cutsize);

  if(i->revindex){
    /* easier than middle insert.  We have the revindex pointing
       backward */
    {
      long j;
      for(j=cutpos;j<cutpos+cutsize;j++)
	if(i->revindex[j])
	  isort_freelink(i,i->vector[j]+32768,i->revindex[j]);
    }

    /* update trailing revindex */
    {
      long j;
      for(j=cutpos+cutsize;j<i->size;j++)
	if(i->revindex[j])
	i->revindex[j]->indice-=cutsize;
    }

    if(cutpos+cutsize<i->size)
      memmove(i->revindex+cutpos,i->revindex+cutpos+cutsize,
	      (i->size-cutpos-cutsize)*sizeof(is_link *));
    
  }

  if(cutpos+cutsize<i->size)
    memmove(i->vector+cutpos,i->vector+cutpos+cutsize,
	    (i->size-cutpos-cutsize)*sizeof(size16));
  
  i->size-=cutsize;
  
  SC(i,"remove");
}

long *isort_matches(long pos,int value,void *match,long dyno){
  is_vector *v=(is_vector *)match;
  long begin=(pos-dyno>v->begin?pos-dyno:v->begin);
  long end=(pos+dyno<v->begin+v->size?pos+dyno:v->begin+v->size);
  long size=end-begin;

  if(size<1)return(NULL);
  isort_assert_sort(v,begin-v->begin,size);
  
  {
    /* This is a glorified selection; find the closest distance match
       to value/pos, then find the other matches in successive
       distance order. */
    
    /* which values match? */
    
    is_link *matchtail=v->b_tails[value+32768];
    if(matchtail==NULL)return(NULL);
    {
      
      /* find the closest match; bisection is faster, but this is simpler.
	 the indicies will still be in ascending order in the range
	 because the radix sort was stable. */
      
      is_link *matchcenter=NULL,*ptr=matchtail;
      long distance=dyno+1;
      long count=0;
      long prevdistance=LONG_MAX;

      /* Optimization for later: Don't search the whole thing; find the
	 beginning, middle and end, then bail. */      
      
      for(;ptr;ptr=ptr->link){
	long newpos=ptr->indice+v->begin;
	long newdistance=labs(pos-newpos);
	if(newdistance<distance){
	  distance=newdistance;
	  matchcenter=ptr;
	}
	if(newdistance<=dyno)
	  count++;
	else 
	  if(prevdistance<newdistance)break;
	prevdistance=newdistance;
      }
      
      if(matchcenter==NULL)return(NULL);
      
      /* Sort the rest by moving out in both directions from center */
      /* Oh, and stay within the dynoverlap setting, but this is
	 inherent.  No wasted time. */
      
      {
	is_link *matchlow=matchcenter->link;
	is_link *matchhigh=matchcenter->parent;
	
	long *ret=malloc(sizeof(long)*(count+1));      
	long twocount=1;
	ret[0]=matchcenter->indice;
	
	while(twocount<count){
	  if(!matchlow){
	    if(!matchhigh){
	      /* Done */
	      break;
	    }else{
	      /* Take matchhigh */
	      ret[twocount++]=matchhigh->indice;
	      matchhigh=matchhigh->parent;
	    }
	  }else{
	    if(!matchhigh){
	      /* Take matchlow */
	      ret[twocount++]=matchlow->indice;
	      matchlow=matchlow->link;
	    }else{
	      /* have to check both */
	      long lowdistance=labs(pos-(matchlow->indice+v->begin));
	      long highdistance=labs(pos-(matchhigh->indice+v->begin));
	      if(lowdistance<highdistance){
		/* Take matchlow */
		ret[twocount++]=matchlow->indice;
		matchlow=matchlow->link;
	      }else{
		/* Take matchhigh */
		ret[twocount++]=matchhigh->indice;
		matchhigh=matchhigh->parent;
	      }
	    }
	  }
	}
	
	ret[twocount]=-1;
	return(ret);
      }
    }
  }
  return(NULL);
}

#ifdef TEST

#include<stdio.h>

static void check_sort(is_vector *v,size16 *ov,char *prompt){
  long count=0,i;

  for(i=0;i<65536;i++){
    is_link *head=v->b_tails[i];

    while(head && head->link)head=head->link;

    while(head){
      if(v->vector[head->indice]!=ov[count++]){
	printf("%s: sort failed %d!=%d @ value:%ld count:%ld\n",prompt,
	       (int)v->vector[head->indice],(int)ov[count-1],i-32768,count-1);
	exit(1);
      }
      head=head->parent;
    }
  }
}

int main(){
  size16 iv[31]={-32768,0,32767,0,1,
		 6,3,21,1,54,
		 645,221,1,3,2,
		 -1,-5,3,2,1,
		 -1,67,4,23,2,
		 -3,2,1,3,4,2};
  size16 iv3[5]={8,3,3,10,32767};

  size16 ov[31]={-32768,-5,-3,-1,-1,
		 0,0,1,1,1,
		 1,1,2,2,2,
		 2,2,3,3,3,
		 3,4,4,6,21,
		 23,54,67,221,645,32767};

  size16 ov2[41]={-32768,-32768,-5,-3,-1,-1,
		 0,0,0,0,1,1,1,1,1,
		 1,1,2,2,2,
		 2,2,3,3,3,3,
		 3,4,4,6,6,21,21,
		 23,54,54,67,221,645,32767,32767};

  size16 ov3[26]={-32768,-5,-3,-1,-1,
		 0,0,1,1,
		 1,1,2,2,2,
		 2,3,3,
		 3,4,4,6,21,
		 23,54,67,32767};

  size16 ov5[26]={-32768,0,0,1,1,3,6,21,54,32767};

  size16 ov4[36]={-32768,-5,-3,-1,-1,
		 0,0,1,1,1,
		 1,1,2,2,2,
		 2,2,3,3,3,3,3,
		 3,4,4,6,8,10,21,
		 23,54,67,221,645,32767,32767};

  is_vector *v=isort_alloc(1);
  long count=0,i;
  long matchvector1[6]={4,8,12,19,27,-1};
  long matchvector3[6]={12,8,19,4,27,-1};
  long matchvector5[5]={4,8,12,19,-1};
  long *matches;

  isort_append(v,iv,31);
  isort_assert_sort(v,0,31);

  check_sort(v,ov,"isort_append (1)");

  /* add some more */
  isort_append(v,iv,10);
  isort_assert_sort(v,0,41);
  check_sort(v,ov2,"isort_append (2)");

  /* remove some from the beginning of the vector */
  isort_removef(v,10);
  isort_assert_sort(v,0,31);
  check_sort(v,ov,"isort_removef (1)");

  /* truncate the lot */
  isort_removef(v,-1);
  count=0;
  for(i=0;i<65536;i++){
    is_link *ptr=v->b_tails[i];
    while(ptr){
      count++;
      ptr=ptr->link;
    }
  }

  if(count>0){
    printf("isort_removef (2): count nonzero\n");
    return(1);
  }

  /* create new vec; remove from middle */
  isort_set(v,0);
  isort_append(v,iv,31);
  isort_remove(v,10,5);
  isort_assert_sort(v,0,26);
  check_sort(v,ov3,"isort_remove (1)");
  isort_remove(v,10,-1);
  isort_assert_sort(v,0,10);
  check_sort(v,ov5,"isort_remove (2)");

  /* truncate the lot */
  isort_removef(v,-1);
  count=0;
  for(i=0;i<65536;i++){
    is_link *ptr=v->b_tails[i];
    while(ptr){
      count++;
      ptr=ptr->link;
    }
  }

  /* append to middle */
  isort_set(v,0);
  isort_append(v,iv,31);
  isort_assert_sort(v,0,31);
  isort_insert(v,10,iv3,5);
  isort_assert_sort(v,0,36);
  check_sort(v,ov4,"isort_insert (1)");

  /* check the matches engine */

  isort_removef(v,-1); 
  isort_set(v,100);
  isort_append(v,iv,31);

  /* Test inline, no flags */
  matches=isort_matches(90,1,v,100);
  if(matches){
    for(i=0;i<6;i++)
      if(matches[i]!=matchvector1[i]){
	printf("isort_matches: test 1 failed %d != %d @ %ld\n",
	       (int)matches[i],(int)matchvector1[i],i);
	return(1);
      }
    free(matches);
  }else{
    printf("isort_matches: test 1 failed null ret\n");
    return(1);
  }

  /* Test no matches, no flags */
  matches=isort_matches(90,8,v,100);
  if(matches){
    printf("isort_matches: test 2 failed non-null ret\n");
    return(1);
  }else
    free(matches);

  /* Test mid-order, no flags */
  matches=isort_matches(112,1,v,100);
  if(matches){
    for(i=0;i<6;i++)
      if(matches[i]!=matchvector3[i]){
	printf("isort_matches: test 3 failed %d != %d @ %ld\n",
	       (int)matches[i],(int)matchvector3[i],i);
	return(1);
      }
    free(matches);
  }else{
    printf("isort_matches: test 3 failed null ret\n");
    return(1);
  }
  
  /* Test inline, dynoverlap */

  matches=isort_matches(104,1,v,20);
  if(matches){
    for(i=0;i<3;i++)
      if(matches[i]!=matchvector5[i]){
	printf("isort_matches: test 4 failed %d != %d @ %ld\n",
	       (int)matches[i],(int)matchvector5[i],i);
	return(1);
      }
    free(matches);
  }else{
    printf("isort_matches: test 4 failed null ret\n");
    return(1);
  }

  /* no matches, but nonnull b_head */
  matches=isort_matches(90,1,v,1);
  if(matches){
    printf("isort_matches: test 3 failed non-null ret\n");
    return(1);
  }else
    free(matches);
  
  return(0);
}

#endif






