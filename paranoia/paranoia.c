/***
 * CopyPolicy: GNU Public License 2 applies
 * Copyright (C) by Monty (xiphmont@mit.edu)
 *
 * Toplevel file for the paranoia abstraction over the cdda lib 
 *
 ***/

/* scratch detection/tolerance not implemented yet */
/* drift management (really problematic...) */
/* ??? Don't post from a single point; if we're in the abyss, we won;t
   match anything */

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include "../interface/cdda_interface.h"
#include "cdda_paranoia.h"

#define MIN_WORDS_OVERLAP 32         /* 32 16 bit words */
#define MAX_SECTOR_BACKCACHE 10      /* 10 sectors */
#define MAX_SECTOR_OVERLAP   10      /* 10 sectors */

static void release_p_block(p_block *b){
  cdrom_paranoia *p=b->p;

  /* yeah, not really needed */
  b->begin=-1;
  b->end=-1;
  b->verifybegin=-1;
  b->verifyend=-1;

  if(b->buffer)free(b->buffer);
  b->buffer=NULL;

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
static p_block *new_p_block(cdrom_paranoia *p){
  p_block *b;

  if(!p->free)
    /* gotta free one up.  cull from the tail of fragments; it's oldest */
    release_p_block(p->tail);

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

  return(b);
}

cdrom_paranoia *paranoia_init(cdrom_drive *d,int cache,int readahead){
  cdrom_paranoia *p=calloc(1,sizeof(cdrom_paranoia));

  p->root.begin=-1;
  p->root.end=-1;
  p->root.verifybegin=-1;
  p->root.verifyend=-1;
  p->root.p=p;

  p->d=d;
  p->readahead=readahead;
  p->cache=(readahead+d->nsectors-1)/d->nsectors*cache;
  p->enable=PARANOIA_MODE_FULL;
  p->cursor=cdda_disc_firstsector(d);

  p->free=p->ptr=calloc(p->cache,sizeof(p_block));
  {
    int i;
    for(i=0;i<p->cache-1;i++){
      p->free[i].next=p->free+i+1;
      p->free[i].p=p;
      p->free[i].stamp=i+1;
    }
    p->free[i].p=p;
    p->free[i].stamp=i+1;
  }

  return(p);
}

void paranoia_free(cdrom_paranoia *p){
  int i;

  /* release all the pool blocks */
  release_p_block(&(p->root));
  for(i=0;i<p->cache;i++)release_p_block(p->ptr+i);

  /* free the pool blocks */
  free(p->ptr);
  free(p);

}

void paranoia_modeset(cdrom_paranoia *p,int enable){
  p->enable=enable;
}

long paranoia_seek(cdrom_paranoia *p,long seek,int mode){
  long sector;
  long ret;
  switch(mode){
  case SEEK_SET:
    sector=cdda_disc_firstsector(p->d)+seek;
    break;
  case SEEK_END:
    sector=cdda_disc_lastsector(p->d)+seek;
    break;
  default:
    sector=p->cursor+seek;
    break;
  }
  
  if(cdda_sector_gettrack(p->d,sector)==-1)return(-1);
  ret=p->cursor;
  p->cursor=sector;
  return(ret);
}

static void i_dump_chains(cdrom_paranoia *p){
  p_block *b;
  int i=0;

  printf("\n    ROOT:  %7ld %7ld -- %7ld %7ld\n",
	 p->root.begin,p->root.verifybegin,p->root.verifyend,p->root.end);
  
  b=p->fragments;

  while(b){
    printf(" link %2d:  %7ld %7ld -- %7ld %7ld\n",b->stamp,
	   b->begin,b->verifybegin,b->verifyend,b->end);
  
    b=b->next;
    i++;
  }
}

static void i_paranoia_trim(p_block *b,long beginword, long endword){
  /* If we are too far behind the desired range, free up space */
  if(b->buffer){

    /* if the cache is entirely ahead of the current range, blast it;
       we seeked back ('seeked' isn't a word!)  */

    /* or it's a really old fragment */

    if(b->begin>endword+(CD_FRAMESIZE_RAW/2)*
       (b->p->readahead+b->p->d->nsectors) || 
       b->end<beginword-(CD_FRAMESIZE_RAW/2)*3){ /* a comfortable buffer */

      release_p_block(b);

      return;

    }
      
    /* Next test; the cache is mostly readahead; does this fragment have
       too much preceeding data? */
    
    if(b==&(b->p->root))
      if(b->begin+CD_FRAMESIZE_RAW/2*3<beginword){
	long newbegin=beginword-CD_FRAMESIZE_RAW/2*3;
	long offset=newbegin-b->begin;
	
	/* copy and realloc (well, realloc may not really be useful) */
	memmove(b->buffer,b->buffer+offset,(b->end-newbegin)*2);
	b->begin=newbegin;
	b->buffer=realloc(b->buffer,(b->end-newbegin)*2);
	
	if(b->verifyend!=-1)
	  b->verifyend=(b->verifyend>newbegin?b->verifyend:-1);
	if(b->verifyend==-1)
	  b->verifybegin=-1;
	else
	  b->verifybegin=(b->verifybegin>newbegin?b->verifybegin:newbegin);

      }
  }
}

/* enlarge (if necessary) a and update verified areas from b */

/* Now this is an odd problem below; if we create new rifts by blindly
   copying around verified areas, the new rifts (where the verified
   areas don't match the preceeding or following unverified areas)
   have an uncanny knack for matching up later despite being jittered.
   So we do this:

                     1     |     2
A:           |--------|====|=====|-----|
B:                |+++++|==|========|++++++++|
                     3     |     4
becomes                 
                     1     |     4
A:           |--------|====|========|++++++++|
B:                |+++++|==|=====|-----|    (or just free it)
                     3     |     2

clever, eh? :-) Do this only if we're extending a's verification area. */

static void i_update_verified(p_block *a,p_block *b){

  /* if *verified* areas mismatch, that's a scratch (is detect on?) */
  if(b->p->enable&PARANOIA_MODE_SCRATCH){
    
    
    /*XXX*/
    
    
    
  }

  {
    long beginvo=(a->verifybegin>b->verifybegin?a->verifybegin:
		  b->verifybegin);
    long endvo=(a->verifyend<b->verifyend?a->verifyend:
		b->verifyend);
    long divpoint=((beginvo+endvo)>>2)<<1;
    
    long beginve=(a->verifybegin<b->verifybegin?a->verifybegin:
		  b->verifybegin);
    long endve=(a->verifyend>b->verifyend?a->verifyend:
		b->verifyend);
    
    
    if(beginvo>=endvo)return; /* shouldn't ever happen actually */
    if(a->verifybegin==beginve && a->verifyend==endve)return; /* none to do */

    {
      long size1=divpoint-a->begin;
      long size2=a->end-divpoint;
      long size3=divpoint-b->begin;
      long size4=b->end-divpoint;
      
      long newsizeA=(beginve==a->verifybegin?size1:size3)+
	(endve==a->verifyend?size2:size4);
      size16 *bufferA=malloc(newsizeA*2);
      
      long newsizeB=(beginve==a->verifybegin?size3:size1)+
	(endve==a->verifyend?size4:size2);
      size16 *bufferB=malloc(newsizeB*2);
      
      long beginA,endA,beginB,endB;
      long verifybeginA,verifyendA,verifybeginB,verifyendB;
      long sofarA=0,sofarB=0;

      if(beginve==a->verifybegin){
	beginA=a->begin;
	verifybeginA=a->verifybegin;
	beginB=b->begin;
	verifybeginB=b->verifybegin;
	
	memmove(bufferA,a->buffer,size1*2);
	memmove(bufferB,b->buffer,size3*2);
	sofarA+=size1;
	sofarB+=size3;
	
      }else{
	beginA=b->begin;
	verifybeginA=b->verifybegin;
	beginB=a->begin;
	verifybeginB=a->verifybegin;
	
	memmove(bufferB,a->buffer,size1*2);
	memmove(bufferA,b->buffer,size3*2);
	sofarB+=size1;
	sofarA+=size3;
      }
      
      if(endve==a->verifyend){
	endA=a->end;
	verifyendA=a->verifyend;
	endB=b->end;
	verifyendB=b->verifyend;
	
	memmove(bufferA+sofarA,a->buffer+divpoint-a->begin,size2*2);
	memmove(bufferB+sofarB,b->buffer+divpoint-b->begin,size4*2);
      }else{
	endB=a->end;
	verifyendB=a->verifyend;
	endA=b->end;
	verifyendA=b->verifyend;
	
	memmove(bufferB+sofarB,a->buffer+divpoint-a->begin,size2*2);
	memmove(bufferA+sofarA,b->buffer+divpoint-b->begin,size4*2);
      }
      
      free(a->buffer);
      free(b->buffer);
      
      a->buffer=bufferA;
      a->begin=beginA;
      a->end=endA;
      a->verifybegin=verifybeginA;
      a->verifyend=verifyendA;
      
      b->buffer=bufferB;
      b->begin=beginB;
      b->end=endB;
      b->verifybegin=verifybeginB;
      b->verifyend=verifyendB;

    }
  }
}

static long i_paranoia_overlap(p_block *a,p_block *b,
				long offsetA, long offsetB,
				long *ret_begin, long *ret_end){
  size16 *buffA=a->buffer;
  size16 *buffB=b->buffer;
  long sizeA=a->end-a->begin;
  long sizeB=b->end-b->begin;
  long beginA=offsetA,endA=offsetA;
  long beginB=offsetB,endB=offsetB;

  for(;beginA>=0 && beginB>=0;beginA-=2,beginB-=2)
    if(buffA[beginA]!=buffB[beginB] ||
       buffA[beginA+1]!=buffB[beginB+1])break;
  beginA+=2;
  beginB+=2;
  
  for(;endA+1<sizeA && endB+1<sizeB;endA+=2,endB+=2)
    if(buffA[endA]!=buffB[endB] ||
       buffA[endA+1]!=buffB[endB+1])break;
  
  if(ret_begin)*ret_begin=a->begin+beginA;
  if(ret_end)*ret_end=a->begin+endA;
  return(endA-beginA);
}

static void i_paranoia_match(p_block *a,p_block *b,long post,
			     long *begin, long *end,
			     void (*callback)(long,int)){

  long count;
  long best=0;
  long offset=0;
  size16 *buffA=a->buffer;
  size16 *buffB=b->buffer;
  long sizeA=a->end-a->begin;
  long sizeB=b->end-b->begin;
  long postA=post-a->begin+2;
  long postB=post-b->begin+2;
  
  long ret;
  long limit;
  long overlap;
  
  /* if out of range, bail */
  if(postB<0 || postB>=sizeB)return;
  if(postA<0 || postA>=sizeA)return;
  
  /* We need to handle long stretches of silence */
  
  if(buffA[postA]==buffB[postB])
    for(;postA<sizeA && postB<sizeB; postA+=2, postB+=2)
      if(buffA[postA]!=buffA[postA-2] ||
	 buffA[postA+1]!=buffA[postA-1] ||
	 buffB[postB]!=buffB[postB-2] ||
	 buffB[postB+1]!=buffB[postB-1])	 
	break;
  
  /* past the ver range? Uhhh... OK */ 
  if(postA==sizeA || postB==sizeB){
    
    postA=post-a->begin+2;
    postB=post-b->begin+2;
    best=i_paranoia_overlap(a,b,postA,postB,begin,end);
    
  }else{
    limit=sizeB-postB;
    if(postB<limit)limit=postB;
    /* only search one sector; jitter should be less than *that* */
    /*if(limit>CD_FRAMESIZE_RAW/4)limit=CD_FRAMESIZE_RAW/4;*/

    {
      long x1=(a->begin>b->begin?a->begin:b->begin);
      long x2=(a->end<b->end?a->end:b->end);
      overlap=x2-x1;
    }
    
    for(count=0;count<limit;count+=2){
      if((ret=i_paranoia_overlap(a,b,postA,postB+count,NULL,NULL))>best){
	best=ret;
	offset= +count;
      }
      if(count+best>=overlap)break;

      if(count>0){
	if((ret=i_paranoia_overlap(a,b,postA,postB-count,NULL,NULL))>best){
	  best=ret;
	  offset= -count;
	}
	if(count+best>=overlap)break;
      }
    }
  }
  
  if(best>=MIN_WORDS_OVERLAP){

    /* we have the offset, find the match limits */
    b->begin-=offset;
    b->end-=offset;
    if(b->verifybegin!=-1)b->verifybegin-=offset;
    if(b->verifyend!=-1)b->verifyend-=offset;
    
    i_paranoia_overlap(a,b,postA,postB+offset,begin,end);

    if(offset)
      (*callback)(post,PARANOIA_CB_FIXUP_EDGE);

    /* if the match is only part of the overlap, we have a rift *within*
       the ostensibly atomic read */

    if(*begin!=a->begin && *begin!=b->begin)
      (*callback)(*begin,PARANOIA_CB_FIXUP_ATOM);

    if(*end!=a->end && *end!=b->end)
      (*callback)(*end,PARANOIA_CB_FIXUP_ATOM);

  }
}

static void i_paranoia_verified_glom(p_block *a,p_block *b,long initial,
				     void (*callback)(long,int)){

  if(a->verifybegin!=-1 && b->verifybegin!=-1){
    /* get a post in the middle of the alleged overlap and hunt for offset */

    long beginoverlap=(a->verifybegin>b->verifybegin?
		       a->verifybegin:b->verifybegin);
    long endoverlap=(a->verifyend<b->verifyend?a->verifyend:b->verifyend);
    
    long post=((beginoverlap+endoverlap)>>2)<<1;

    if(beginoverlap<endoverlap){

      long count;
      long best=0;
      long offset=0;
      long sizeA=a->verifyend-a->verifybegin;
      long sizeB=b->verifyend-b->verifybegin;
      long postA=post-a->verifybegin;
      long postB=post-b->verifybegin;
      long adjA=post-a->begin;
      long adjB=post-b->begin;

      long ret;
      long limit;
      long overlap; 
      
      /* if out of range, bail */
      if(postB<0 || postB>=sizeB)return;
      if(postA<0 || postA>=sizeA)return;
      
      /* No silence handling needed here */
      
      limit=sizeB-postB;
      if(postB<limit)limit=postB;
      /* only search one sector; jitter should be less than *that* */
      /*if(limit>CD_FRAMESIZE_RAW/4)limit=CD_FRAMESIZE_RAW/4;*/
      
      {
	long x1=(a->verifybegin>b->verifybegin?a->verifybegin:b->verifybegin);
	long x2=(a->verifyend<b->verifyend?a->verifyend:b->verifyend);
	overlap=x2-x1;
      }
      
      /* clip to the verified areas as well */
      for(count=0;count<limit;count+=2){
	if((ret=i_paranoia_overlap(a,b,adjA,adjB+count,NULL,NULL))>best){
	  best=ret;
	  offset= +count;
	}
	if(count+best>=overlap)break;
	
	if(count>0){
	  if((ret=i_paranoia_overlap(a,b,adjA,adjB-count,NULL,NULL))>best){
	    best=ret;
	    offset= -count;
	  }
	  if(count+best>=overlap)break;
	}
      }
      
      if(best>=MIN_WORDS_OVERLAP){

	if(offset)
	  (*callback)(post,PARANOIA_CB_FIXUP_EDGE);

	b->begin-=offset;
	b->end-=offset;
	if(b->verifybegin!=-1)b->verifybegin-=offset;
	if(b->verifyend!=-1)b->verifyend-=offset;

	/* we clipped to verified range.  If we get a match, we overlap OK */
	/* glom */
	i_update_verified(a,b);
	
      }
    }
  }else
    if(a->begin==-1){
      if(b->verifybegin<=initial && b->verifybegin!=-1){
	
	a->buffer=b->buffer;
	b->buffer=NULL;
	a->begin=b->begin;
	a->end=b->end;
	a->verifybegin=b->verifybegin;
	a->verifyend=b->verifyend;
	
	release_p_block(b);
      }
      return;
    }
}

static void i_paranoia_reconcile(p_block *a,p_block *b,long initial,long post,
				 int extendonly, void (*callback)(long,int)){
  /* trivial case; if 'a' is empty return, unles we're glomming, then
     copy it if it's verified. */

  if(a->begin==-1)
    return;

  if(b->begin!=-1){
    /* First, try exact matching, then tolerant matches */
    /* match range in terms of a */
    
    long beginmatch=-1;
    long endmatch=-1;

    i_paranoia_match(a,b,post,&beginmatch,&endmatch,callback);
    
    /* no match? scratch tolerant matching */
    

    /*XXX*/


    /* no matches whatsoever.  bail */
    if(beginmatch==-1)return;

    /* can we extend the matched range with a tolerant match? */

    

    /**XXX*/




    /* take our match range and figure out what to do */


    /* now update the match ranges... */
    /* case 1: block has no match range yet: set it
       case 2: block's range is extended: extend it
       case 3: match range is discontinuous: replace it */

    /* non-obvious twist: we need a minimum verify overlap to extend
       the range to properly play probability . */

    if(a->verifybegin==-1){ /* case 1 */
      a->verifybegin=beginmatch;
      a->verifyend=endmatch;
    }else{
      if(beginmatch+MIN_WORDS_OVERLAP>=a->verifyend || 
	 endmatch-MIN_WORDS_OVERLAP<=a->verifybegin){ /* case 3 */
	if(!extendonly){
	  a->verifybegin=beginmatch;
	  a->verifyend=endmatch;
	}
      }else{ /* case 2 */
	if(a->verifybegin>beginmatch)a->verifybegin=beginmatch;
	if(a->verifyend<endmatch)a->verifyend=endmatch;
      }
    }

    if(b->verifybegin==-1){ /* case 1 */
      b->verifybegin=beginmatch;
      b->verifyend=endmatch;
    }else{
      if(beginmatch+MIN_WORDS_OVERLAP>=b->verifyend || 
	 endmatch-MIN_WORDS_OVERLAP<=b->verifybegin){ /* case 3 */
	if(!extendonly){
	  b->verifybegin=beginmatch;
	  b->verifyend=endmatch;
	}
      }else{ /* case 2 */
	if(b->verifybegin>beginmatch)b->verifybegin=beginmatch;
	if(b->verifyend<endmatch)b->verifyend=endmatch;
      }
    }
  }	 
}

/* The returned buffer is *not* to be freed by the caller.  It will
   persist only until the next call to paranoia_read() for this p */
size16 *paranoia_read(cdrom_paranoia *p,long sectors, 
		      void(*callback)(long,int)){

  long disc_firstsector=cdda_disc_firstsector(p->d);
  long disc_lastsector=cdda_disc_lastsector(p->d);

  long beginword=p->cursor*(CD_FRAMESIZE_RAW/2);
  long endword=beginword+sectors*(CD_FRAMESIZE_RAW/2);
  long currword=beginword;
  long curroverlap=1;
  long retry_count=0,lastend=-2;

  /* First, is the sector we want already in the root? */
  while(p->root.verifyend==-1 || p->root.verifybegin>beginword || 
	p->root.verifyend<endword){
    lastend=p->root.verifyend;

    /* Nope; we need to build or extend the root verified range */

    /* Do we need to trim root? */
    
    i_paranoia_trim(&(p->root),beginword,endword);
      
    if(p->enable&PARANOIA_MODE_VERIFY){
      long querypost=(currword/(CD_FRAMESIZE_RAW/2))*(CD_FRAMESIZE_RAW/2)
	+(CD_FRAMESIZE_RAW/4);
      
      (*callback)(currword,PARANOIA_CB_VERIFY);
	
      /* OK! compare fragments (yeah, n^2, don't worry too much) */
      
      {
	p_block *current=p->fragments;
	
	while(current){
	  p_block *next=current->next;
	  p_block *compare=next;
	  
	  /*	  if(p->root.verifyend!=-1){
	    long backpost=p->root.verifyend-(CD_FRAMESIZE_RAW/4);
	    i_paranoia_reconcile(&(p->root),current,beginword,backpost,1);
	  }*/
	  
	  while(compare){
	    p_block *next=compare->next;
	    
	    i_paranoia_reconcile(current,compare,beginword,querypost,0,
				 callback);
	    compare=next;
	  }
	  current=next;
	}
      } 
    }

    /* compare fragments to root before reading a new chunk */
    {
      long prev=-2; /* p->root.verifyend could be -1 */
      
      while(prev!=p->root.verifyend){
	p_block *current=p->fragments;
	prev=p->root.verifyend;
	
	while(current){
	  p_block *next=current->next;
	  
	  i_paranoia_trim(current,beginword,endword);
	  if(current->buffer)i_paranoia_verified_glom(&(p->root),current,
						      beginword,callback);
	  
	  current=next;
	}
      }
    }
    
    if(!(p->root.verifyend==-1 || p->root.verifybegin>beginword || 
	 p->root.verifyend<endword)) break;

    {
      /* why do it this way?  We read, at best guess, atomic-sized
	 read blocks, but we need to read lots of sectors to kludge
	 around stupid read ahead buffers on cheap drives, as well as
	 avoid expensize back-seeking. */
      
      long readat;
      long totaltoread=p->readahead;
      long sectatonce=p->d->nsectors;

      /* What is the first sector to read?  want some pre-buffer if
	 we're not at the extreme beginning of the disc */

      if(p->enable&PARANOIA_MODE_VERIFY){
	p->jitter++;
	if(p->jitter>2)p->jitter=0; /* this is not arbitrary; think about it */
	
	if(p->root.verifyend==-1 || p->root.verifybegin>beginword)
	  readat=p->cursor-p->jitter-curroverlap; 
	else
	  readat=p->root.verifyend/(CD_FRAMESIZE_RAW/2)-p->jitter-curroverlap;
      }else
	if(p->enable&PARANOIA_MODE_OVERLAP)
	  readat=p->cursor-curroverlap; 
	else{
	  readat=p->cursor; 
	  totaltoread=sectatonce;
	}

      while(totaltoread>0){

	long secread=sectatonce;
	p_block *new;

	if(p->enable&(PARANOIA_MODE_OVERLAP|PARANOIA_MODE_VERIFY))
	  new=new_p_block(p);
	else{
	  new=&(p->root);
	  if(new->buffer)free(new->buffer);
	  new->buffer=NULL;
	}

	/* don't under/overflow the audio session */
	if(readat<disc_firstsector)readat=disc_firstsector;
	if(readat+secread-1>disc_lastsector)secread=disc_lastsector-readat+1;

	new->buffer=malloc(secread*CD_FRAMESIZE_RAW);
	if((secread=cdda_read(p->d,new->buffer,readat,secread))<=0){
	  /* cdda_read only bails on *really* serious errors */
	  free(new->buffer);
	  new->buffer=NULL;
	  return(NULL);
	}

	(*callback)(readat*CD_FRAMESIZE_RAW/2,PARANOIA_CB_READ);
	new->begin=readat*CD_FRAMESIZE_RAW/2; 
	new->end=new->begin+secread*CD_FRAMESIZE_RAW/2;
	if(!(p->enable&PARANOIA_MODE_VERIFY)){
	  new->verifybegin=new->begin;
	  new->verifyend=new->end;
	}

	totaltoread-=secread;
	readat+=secread; 

      }

    }
    
    if(p->root.verifyend==-1 || p->root.verifybegin<beginword)
      currword=beginword;
    else
      currword=p->root.verifybegin;

    /* increase overlap or bail */
    retry_count++;
    if(p->root.verifyend==lastend){
      if(retry_count>=3){
	if(curroverlap==MAX_SECTOR_OVERLAP)return(NULL);
	curroverlap++;
	retry_count=0;
      }

    }else{
      curroverlap=1;
      retry_count=0;
    }

    lastend=p->root.verifyend;
    
    if(curroverlap==MAX_SECTOR_OVERLAP)retry_count++;
    if(retry_count==10)return(NULL);
  }
  p->cursor+=sectors;

  return(p->root.buffer+(beginword-p->root.begin));
}


