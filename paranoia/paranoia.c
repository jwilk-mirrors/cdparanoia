/***
 * CopyPolicy: GNU Public License 2 applies
 * Copyright (C) by Monty (xiphmont@mit.edu)
 *
 * Toplevel file for the paranoia abstraction over the cdda lib 
 *
 ***/

/* immediate todo:: */
/* Allow disabling of root fixups? */ 
/* scratch detection/tolerance not implemented yet */

/***************************************************************

  Da new shtick: verification now a two-step assymetric process.
  
  A single 'verified/reconstructed' data segment cache, and then the
  multiple fragment cache 

  verify a newly read fragment against previous fragments; do it only
  this once, and try to use the whole of both fragments.  We maintain
  a list of 'verified sections' from these matches.

  We then glom these verified areas into a new data buffer.
  Fragmentation is allowed here alone.

  We also now track where read boundaries actually happened; do not
  verify across matching boundaries.

  **************************************************************/

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../interface/cdda_interface.h"
#include "../interface/smallft.h"
#include "p_block.h"
#include "cdda_paranoia.h"
#include "overlap.h"
#include "gap.h"
#include "isort.h"

#ifdef  TEST
static void sync_fragment_test(cdrom_paranoia *p);
#endif

/**** matching and analysis code *****************************************/

/* Top level of the first stage matcher */

/* We match each analysis point of new to the preexisting blocks
recursively.  We can also optionally maintain a list of fragments of
the preexisting block that didn't match anything, and match them back
afterward. */

#define OVERLAP_ADJ (MIN_WORDS_OVERLAP/2-1)
#ifndef rv
#  define rv (p->root.vector)
#  define rb (isort_begin(p->root.vector))
#  define re (isort_end(p->root.vector))
#endif

/* smallest power of two >= value */
static int ilog(long value){
  int ret=1;
  value--;

  while(value>0){
    ret<<=1;
    value>>=1;
  }
  return(ret);
}

static inline long do_const_sync(void *A,void *B,
				 char *flagA,char *flagB,
				 long posA,long posB,
				 long *begin,long *end,long *offset){
  long ret=0;
  if(flagB==NULL)
    ret=i_paranoia_overlap(isort_buffer(A),isort_buffer(B),posA,posB,
			   isort_size(A),isort_size(B),begin,end);
  else
    /* only block on unmatchable */
    if((flagB[posB]&2)==0)
      ret=i_paranoia_overlap2(isort_buffer(A),isort_buffer(B),
			      flagA,flagB,posA,posB,isort_size(A),
			      isort_size(B),begin,end);
  
  if(ret>MIN_WORDS_SEARCH){
    *offset=(posA+isort_begin(A))-(posB+isort_begin(B));
    *begin+=isort_begin(A);
    *end+=isort_begin(A);
    return(ret);
  }
  
  return(0);
}

static inline long try_sort_sync(cdrom_paranoia *p,
				 void *old,char *oldflags,
				 void *new,char *newflags,
				 long post,long *begin,long *end,
				 long *offset,void (*callback)(long,int)){
  
  long dynoverlap=p->dynoverlap;
  long *matches=NULL,*ptr=NULL;
  long newbegin=isort_begin(new);
  
  /* block flag matches on 2 (unmatchable) and 4 (already matched) */
  if(newflags==NULL || (newflags[post-newbegin]&6)==0){
    /* always try absolute offset zero first! */
    {
      long zeropos=post-isort_begin(old);
      if(zeropos>=0 && zeropos<isort_size(old)){
	if(do_const_sync(new,old,
			 newflags,oldflags,
			 post-isort_begin(new),zeropos,
			 begin,end,offset)){
	  
	  offset_add_value(p,&(p->stage1),*offset,callback);

	  return(1);
	}
      }
    }
    ptr=matches=isort_matches(post,isort_buffer(new)[post-newbegin],
			      old,dynoverlap);
  }else
    return(0);
  
  if(ptr){
    while(*ptr!=-1){
      if(do_const_sync(new,old,
		       newflags,oldflags,
		       post-isort_begin(new),*ptr,
		       begin,end,offset)){
	
	offset_add_value(p,&(p->stage1),*offset,callback);
	free(matches);
	return(1);
      }
      ptr++;
    }
  }
  
  *begin=-1;
  *end=-1;
  *offset=-1;
  if(matches)
    free(matches);
  return(0);
}

static long i_iterate_stage1(cdrom_paranoia *p,c_block *old,c_block *new,
			     void(*callback)(long,int)){

  long matchbegin=-1,matchend=-1,matchoffset;
  long hardbegin=isort_begin(new->vector);
  long hardend=isort_end(new->vector);
  long oldbegin=isort_begin(old->vector);
  long oldend=isort_end(old->vector);
  long searchend=min(hardend,oldend+p->dynoverlap);
  long searchbegin=max(hardbegin,oldbegin-p->dynoverlap);
  long searchsize=searchend-searchbegin;
  long step=ilog(searchsize);
  long ret=0;
  char *already;

  if(searchsize<=0)return(0);
  
  already=calloc(isort_size(new->vector),sizeof(char));

  while(step>0){
    long j;
    for(j=searchend-step;j>=searchbegin;j-=(step<<1)){
      if(already[j-hardbegin]==0 && (new->flags[j-hardbegin]&6)==0){
	if(try_sort_sync(p,old->vector,old->flags,new->vector,new->flags,
			 j,&matchbegin,&matchend,&matchoffset,
			 callback)==1){
	  
	  if(matchbegin!=-1 && matchend-matchbegin>=MIN_WORDS_OVERLAP){
	    long i;
	    long adjbegin=matchbegin-hardbegin;
	    long adjend=matchend-hardbegin;
	    
	    if(matchbegin<=hardbegin ||
	       matchbegin-matchoffset<=oldbegin ||
	       (new->flags[matchbegin-hardbegin]&1) ||
	       (old->flags[matchbegin-matchoffset-oldbegin]&1)){
	      if(matchoffset)
		(*callback)(matchbegin,PARANOIA_CB_FIXUP_EDGE);
	    }else
	      (*callback)(matchbegin,PARANOIA_CB_FIXUP_ATOM);

	    if(matchend>=hardend ||
	       (new->flags[matchend-hardbegin]&1) ||
	       matchend-matchoffset>=oldend ||
	       (old->flags[matchend-matchoffset-oldbegin]&1)){
	      if(matchoffset)
		(*callback)(matchend,PARANOIA_CB_FIXUP_EDGE);
	    }else
	      (*callback)(matchend,PARANOIA_CB_FIXUP_ATOM);

	    for(i=adjbegin;i<adjend;i++){
	      already[i]=1; /* mark verified */
	    }

	    /* Mark the verification flags.  Don't mark the first or last
	       OVERLAP/2 elements so that overlapping fragments have to
	       overlap by OVERLAP to actually merge. */
	    
	    if(adjbegin>0)adjbegin+=OVERLAP_ADJ;
	    if(adjend<isort_size(new->vector))adjend-=OVERLAP_ADJ;
	    for(i=adjbegin;i<adjend;i++)
	      new->flags[i]|=4; /* mark verified */

	    ret++;

	    if((matchbegin==hardbegin || matchbegin-matchoffset==oldbegin)&&
	       (matchend==hardend || matchend-matchoffset==oldend)){
	      goto match_cleanup;
	    }
	  }
	}
      }
    }
    step>>=1;
  }
match_cleanup:

  free(already);
  return(ret);
}

static long frame_current=0;

static long i_stage1(cdrom_paranoia *p,c_block *new,
		     void(*callback)(long,int)){

  long size=isort_size(new->vector);
  c_block *ptr=c_last(p);
  int ret=0;
  long begin=0,end;
  
  while(ptr && ptr!=new){

    (*callback)(isort_begin(new->vector),PARANOIA_CB_VERIFY);
    i_iterate_stage1(p,ptr,new,callback);
    isort_unsort(ptr->vector); /* flush the sort info */

    ptr=c_prev(ptr);
  }

  /* parse the verified areas of new into v_fragments */
  
  begin=0;
  while(begin<size){
    for(;begin<size;begin++)if(new->flags[begin]&4)break;
    for(end=begin;end<size;end++)if((new->flags[end]&4)==0)break;
    if(begin>=size)break;
    
    ret++;

    new_v_fragment(p,new,isort_begin(new->vector)+max(0,begin-OVERLAP_ADJ),
		   isort_begin(new->vector)+min(size,end+OVERLAP_ADJ),
		   (end+OVERLAP_ADJ>=size && new->lastsector));

    begin=end;
  }
      
  return(ret);
}

/* reconcile v_fragments to root buffer.  Free if used, fragment root
   if necessary */

typedef struct sync_result {
  long offset;
  long begin;
  long end;
} sync_result;

/* Reduction of stage 1.  Work backward. */

static long i_iterate_stage2(cdrom_paranoia *p,
			     v_fragment *v,
			     sync_result *r,void(*callback)(long,int)){

  long matchbegin=-1,matchend=-1,offset;
  long hardbegin=v->begin;
  long hardend=hardbegin+v->size;
  long searchend=min(hardend,re+p->dynoverlap);
  long searchbegin=max(hardbegin,rb-p->dynoverlap);
  long searchsize=searchend-searchbegin;
  long step=ilog(searchsize);

  if(searchsize<=0)return(0);
  
  while(step>0){
    long j;
    for(j=searchend-step;j>=searchbegin;j-=(step<<1)){

      if(try_sort_sync(p,rv,NULL,v->one->vector,NULL,j,
		       &matchbegin,&matchend,&offset,callback)==1){
	
	/* faster to do the test this way */
	matchbegin=max(matchbegin,hardbegin); 
	matchend=min(matchend,hardend);
	if(matchbegin!=-1 && matchend-matchbegin>=MIN_WORDS_OVERLAP){
	  
	  /* try_sort_sync returns begin/end that are in terms
	     of v; we need them in terms of root */
	  
	  r->begin=matchbegin-offset;
	  r->end=matchend-offset;
	  r->offset=offset;

	  if(offset)
	    (*callback)(r->begin,PARANOIA_CB_FIXUP_EDGE);

	  return(1);
	  
	}
      }
    }
    step>>=1;
  }

  return(0);
}

static long i_stage2_each(root_block *root, v_fragment *v,long forcepost,
			  int freeit,void(*callback)(long,int)){

  cdrom_paranoia *p=v->p;
  long dynoverlap=p->dynoverlap/2*2;
  long post=min(re,v->begin+v->size)-2;
  
  if(!v || !v->one)return(0);
  if(forcepost>=0)post=forcepost;

  if(!(root->vector) || !(isort_buffer(root->vector))){
    return(0);
  }else{
    /* sync up as late in the vector as possible */
    sync_result r;

    /* This will trim any matching to the boundaries of v, not c */

    if(i_iterate_stage2(p,v,&r,callback)){

      long begin=r.begin-rb;
      long end=r.end-rb;
      long offset=r.begin+r.offset-v->begin-begin;

      /* we have a match! We don't rematch off rift, we chase the
	 match all the way to both extremes doing rift analysis. */

      /* easier if we copy the v (c) buffer at this point */
      /* we won't need the sort, so no overhead beyond the copy... */

      void *l=isort_alloc();
      isort_append(l,v_buffer(v),v->size);
      isort_set(l,v->begin);

#ifdef NOISY
      fprintf(stderr,"Stage 2 match\n");
#endif
      
      /* chase backward */
      /* note that we don't extend back right now, only forward. */
      while((begin+offset>0 && begin>0)){
	long matchA=0,matchB=0,matchC=0;
	long beginL=begin+offset;

	i_analyze_rift_r(isort_buffer(root->vector),isort_buffer(l),
			 isort_size(root->vector),isort_size(l),
			 begin-1,beginL-1,
			 &matchA,&matchB,&matchC);
	
#ifdef NOISY
	fprintf(stderr,"matching rootR: matchA:%ld matchB:%ld matchC:%ld\n",
		matchA,matchB,matchC);
#endif		
	
	if(matchA){
	  /* a problem with root */
	  if(matchA>0){
	    /* dropped bytes; add back from v */
	    (*callback)(begin+rb-1,PARANOIA_CB_FIXUP_DROPPED);

	    if(rb+begin<p->root.returnedlimit)
	      break;
	    else{
	      isort_insert(root->vector,begin,isort_buffer(l)+beginL-matchA,
			   matchA);
	      offset-=matchA;
	      begin+=matchA;
	      end+=matchA;
	    }
	  }else{
	    /* duplicate bytes; drop from root */
	    (*callback)(begin+rb-1,PARANOIA_CB_FIXUP_DUPED);
	    if(rb+begin+matchA<p->root.returnedlimit) 
	      break;
	    else{
	      isort_remove(root->vector,begin+matchA,-matchA);
	      offset-=matchA;
	      begin+=matchA;
	      end+=matchA;
	    }
	  }
	}else if(matchB){
	  /* a problem with the fragment */
	  if(matchB>0){
	    /* dropped bytes */
	    (*callback)(begin+rb-1,PARANOIA_CB_FIXUP_DROPPED);
	    isort_insert(l,beginL,isort_buffer(root->vector)+begin-matchB,
			 matchB);
	    offset+=matchB;
	  }else{
	    /* duplicate bytes */
	    (*callback)(begin+rb-1,PARANOIA_CB_FIXUP_DUPED);
	    isort_remove(l,beginL+matchB,-matchB);
	    offset+=matchB;
	  }
	}else if(matchC){
	  /* Uhh... problem with both */
	  
	  /* Set 'disagree' flags in root */
	  if(rb+begin-matchC<p->root.returnedlimit)
	    break;
	  isort_overwrite(root->vector,begin-matchC,
			  isort_buffer(l)+beginL-matchC,matchC);
	  
	}else{
	  /* Could not determine nature of difficulty... 
	     report and bail */
	  
	  /*RRR(*callback)(post,PARANOIA_CB_XXX);*/
	  
	  break;
	}
	/* not the most efficient way, but it will do for now */
	beginL=begin+offset;
	i_paranoia_overlap(isort_buffer(root->vector),isort_buffer(l),
			   begin,beginL,
			   isort_size(root->vector),isort_size(l),
			   &begin,&end);	
      }
      
      /* chase forward */
      while(end+offset<isort_size(l) && end<isort_size(root->vector)){
	long matchA=0,matchB=0,matchC=0;
	long beginL=begin+offset;
	long endL=end+offset;
	
	i_analyze_rift_f(isort_buffer(root->vector),isort_buffer(l),
			 isort_size(root->vector),isort_size(l),
			 end,endL,
			 &matchA,&matchB,&matchC);
	
#ifdef NOISY	
	fprintf(stderr,"matching rootF: matchA:%ld matchB:%ld matchC:%ld\n",
		matchA,matchB,matchC);
#endif
	
	if(matchA){
	  /* a problem with root */
	  if(matchA>0){
	    /* dropped bytes; add back from v */
	    (*callback)(end+rb,PARANOIA_CB_FIXUP_DROPPED);
	    if(end+rb<p->root.returnedlimit)
	      break;
	    isort_insert(root->vector,end,isort_buffer(l)+endL,matchA);
	  }else{
	    /* duplicate bytes; drop from root */
	    (*callback)(end+rb,PARANOIA_CB_FIXUP_DUPED);
	    if(end+rb<p->root.returnedlimit)
	      break;
	    isort_remove(root->vector,end,-matchA);
	  }
	}else if(matchB){
	  /* a problem with the fragment */
	  if(matchB>0){
	    /* dropped bytes */
	    (*callback)(end+rb,PARANOIA_CB_FIXUP_DROPPED);
	    isort_insert(l,endL,isort_buffer(root->vector)+end,matchB);
	  }else{
	    /* duplicate bytes */
	    (*callback)(end+rb,PARANOIA_CB_FIXUP_DUPED);
	    isort_remove(l,endL,-matchB);
	  }
	}else if(matchC){
	  /* Uhh... problem with both */
	  
	  /* Set 'disagree' flags in root */
	  if(end+rb<p->root.returnedlimit)
	    break;
	  isort_overwrite(root->vector,end,isort_buffer(l)+endL,matchC);
	}else{
	  /* Could not determine nature of difficulty... 
	     report and bail */
	  
	  /*RRR(*callback)(post,PARANOIA_CB_XXX);*/
	  
	  break;
	}
	/* not the most efficient way, but it will do for now */
	i_paranoia_overlap(isort_buffer(root->vector),isort_buffer(l),
			   begin,beginL,
			   isort_size(root->vector),isort_size(l),
			   NULL,&end);
      }

      /* if this extends our range, let's glom */
      {
	long sizeA=isort_size(root->vector);
	long sizeB=isort_size(l);
	
	if(sizeB-offset>sizeA || v->lastsector){
	  
	  if(v->lastsector){
	    root->lastsector=1;
	  }
	
	  if(end<sizeA)isort_remove(root->vector,end,-1);

	  if(sizeB-offset-end)isort_append(root->vector,
					   isort_buffer(l)+end+offset,
					   sizeB-offset-end);

	  /* add offset into dynoverlap stats */
	  offset_add_value(p,&p->stage2,offset+isort_begin(l)-rb,callback);
	}
      }
      isort_free(l);
      if(freeit)free_v_fragment(v);
      return(1);

    }else{
      /* D'oh.  No match.  What to do with the fragment? */
      if(v->begin+v->size+dynoverlap<re){
	/* It *should* have matched.  No good; free it. */
	if(freeit)free_v_fragment(v);
      }
      /* otherwise, we likely want this for an upcoming match */
      return(0);
      
    }
  }
}

static int i_init_root(root_block *root, v_fragment *v,long begin,
		       void(*callback)(long,int)){
  long vbegin=v->begin,vend=v->begin+v->size;

#ifdef NOISY
  fprintf(stderr,"init attempt: post:%ld [%ld-%ld]\n",begin,vbegin,vend);
#endif

  if(vbegin<=begin && vend>begin){
    
    root->lastsector=v->lastsector;
    root->returnedlimit=begin;

    if(root->vector)isort_free(root->vector);

    root->vector=isort_alloc();
    isort_append(root->vector,v_buffer(v),vend-vbegin);
    isort_set(root->vector,vbegin);
    return(1);
  }else
    return(0);
}

static int i_stage2(cdrom_paranoia *p,long beginword,long endword,
			  void(*callback)(long,int)){

  int flag=1;
  int count=0;
  root_block *root=&(p->root);

#ifdef NOISY
  fprintf(stderr,"Fragments:%ld blocks: %ld\n",p->fragments.active,p->fragments.blocks);
  fflush(stderr);
#endif

  while(flag){
    /* loop through all the current fragments */
    v_fragment *first=v_first(p);
    flag=0;
    count++;

    while(first){
      v_fragment *next=v_next(first);
      
      if(first->one){
	(*callback)(isort_begin(first->one->vector),PARANOIA_CB_VERIFY);
	if(root->vector==NULL){
	  if(i_init_root(&(p->root),first,beginword,callback)){
	    free_v_fragment(first);
	    flag=1;
	  }
	}else{
	  flag|=i_stage2_each(root,first,-1,1,callback);
	}
      }
      first=next;
    }
  }
  return(count);
}

static void i_end_case(cdrom_paranoia *p,long endword,
			    void(*callback)(long,int)){

  root_block *root=&p->root;

  /* have an 'end' flag; if we've just read in the last sector in a
     session, set the flag.  If we verify to the end of a fragment
     which has the end flag set, we're done (set a done flag).  Pad
     zeroes to the end of the read */
  
  if(root->lastsector==0)return;
  if(endword<re)return;
  
  {
    long addto=endword-re;
    char *temp=calloc(addto,sizeof(char)*2);

    isort_append(root->vector,(void *)temp,addto);
    free(temp);

    /* trash da cache */
    paranoia_resetcache(p);

  }
}

/* We want to add a sector. Look through the caches for something that
   spans.  Also look at the flags on the c_block... if this is an
   obliterated sector, get a bit of a chunk past the obliteration. */

static void verify_skip_case(cdrom_paranoia *p,void(*callback)(long,int)){

  long target;
  root_block *root=&(p->root);
  c_block *graft=NULL;
  int vflag=0;
  int gend=0;
  long post;
  
#ifdef NOISY
	fprintf(stderr,"\nskipping\n");
#endif

  if(root->vector==NULL)root->vector=isort_alloc();
  post=re;
  if(post==-1)post=0;

  (*callback)(post,PARANOIA_CB_SKIP);
  
  /* We want to add a sector.  Look for a c_block that spans,
     preferrably a verified area */

  {
    c_block *c=c_first(p);
    while(c){
      long cbegin=isort_begin(c->vector);
      long cend=isort_end(c->vector);
      if(cbegin<=post && cend>post){
	long vend=post;

	if(c->flags[post-cbegin]&4){
	  /* verified area! */
	  while(vend<cend && (c->flags[vend-cbegin]&4))vend++;
	  if(!vflag || vend>vflag){
	    graft=c;
	    gend=vend;
	  }
	  vflag=1;
	}else{
	  /* not a verified area */
	  if(!vflag){
	    while(vend<cend && (c->flags[vend-cbegin]&4)==0)vend++;
	    if(graft==NULL || gend>vend){
	      /* smallest unverified area */
	      graft=c;
	      gend=vend;
	    }
	  }
	}
      }
      c=c_next(c);
    }

    if(graft){
      long cbegin=isort_begin(graft->vector);
      long cend=isort_end(graft->vector);

      while(gend<cend && (graft->flags[gend-cbegin]&4))gend++;
      gend=min(gend+OVERLAP_ADJ,cend);

      isort_append(root->vector,isort_buffer(graft->vector)+post-cbegin,
		   gend-post);

      root->returnedlimit=isort_end(root->vector);
      return;
    }
  }

  /* No?  Fine.  Great.  Write in some zeroes :-P */
  {
    void *temp=calloc(CD_FRAMESIZE_RAW,sizeof(size16));
    isort_append(root->vector,temp,CD_FRAMESIZE_RAW);
  
    root->returnedlimit=isort_end(root->vector);
  }
}    

/**** toplevel ****************************************/

void paranoia_free(cdrom_paranoia *p){
  /*  paranoia_resetall(p);*/
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
    sector=seek;
    break;
  case SEEK_END:
    sector=cdda_disc_lastsector(p->d)+seek;
    break;
  default:
    sector=p->cursor+seek;
    break;
  }
  
  if(cdda_sector_gettrack(p->d,sector)==-1)return(-1);

  isort_free(p->root.vector);
  p->root.vector=NULL;
  p->root.lastsector=0;
  p->root.returnedlimit=0;

  ret=p->cursor;
  p->cursor=sector;

  i_paranoia_firstlast(p);

  return(ret);
}

/* returns last block read, -1 on error */
c_block *i_read_c_block(cdrom_paranoia *p,long beginword,long endword,
		     void(*callback)(long,int)){

/* why do it this way?  We need to read lots of sectors to kludge
   around stupid read ahead buffers on cheap drives, as well as avoid
   expensive back-seeking. We also want to 'jiggle' the start address
   to try to break borderline drives more noticeably (and make broken
   drives with unaddressable sectors behave more often). */
      
  long readat,firstread;
  long totaltoread=p->readahead;
  long sectatonce=p->d->nsectors;
  long driftcomp=(float)p->dyndrift/CD_FRAMEWORDS+.5;
  c_block *new=NULL;
  root_block *root=&p->root;
  size16 *buffer=NULL;
  char *flags=NULL;
  long sofar;
  long dynoverlap=(p->dynoverlap+CD_FRAMEWORDS-1)/CD_FRAMEWORDS; 
  long anyflag=0;

  /* What is the first sector to read?  want some pre-buffer if
     we're not at the extreme beginning of the disc */
  
  if(p->enable&(PARANOIA_MODE_VERIFY|PARANOIA_MODE_OVERLAP)){
    
    /* we want to jitter the read alignment boundary */
    long target;
    if(root->vector==NULL || isort_begin(root->vector)>beginword)
      target=p->cursor-dynoverlap; 
    else
      target=isort_end(root->vector)/(CD_FRAMEWORDS)-dynoverlap;
	
    if(p->enable&PARANOIA_MODE_VERIFY){
	  
      if(target+MIN_SECTOR_BACKUP>p->lastread && target<=p->lastread)
	target=p->lastread-MIN_SECTOR_BACKUP;
      
      /* we want to jitter the read alignment boundary, as some
	 drives, beginning from a specific point, will tend to
	 lose bytes between sectors in the same place.  Also, as
	 our vectors are being made up of multiple reads, we want
	 the overlap boundaries to move.... */
      
      readat=(target&(~((long)JIGGLE_MODULO-1)))+p->jitter;
      if(readat>target)readat-=JIGGLE_MODULO;
      p->jitter++;
      if(p->jitter>=JIGGLE_MODULO)p->jitter=0;
      
    }else{
      readat=target;
    }
  }else{
    readat=p->cursor; 
  }
  
  readat+=driftcomp;
  
  if(p->enable&(PARANOIA_MODE_OVERLAP|PARANOIA_MODE_VERIFY)){
    flags=calloc(totaltoread*CD_FRAMEWORDS,1);
    new=new_c_block(p);
    recover_cache(p);
  }else{
    /* in the case of root it's just the buffer */
    paranoia_resetall(p);	
    new=new_c_block(p);
  }

  buffer=malloc(totaltoread*CD_FRAMESIZE_RAW);
  sofar=0;
  firstread=-1;
  
  /* actual read loop */

  while(sofar<totaltoread){
    long secread=sectatonce;
    long adjread=readat;
    long thisread;

    /* don't under/overflow the audio session */
    if(adjread<p->current_firstsector){
      secread-=p->current_firstsector-adjread;
      adjread=p->current_firstsector;
    }
    if(adjread+secread-1>p->current_lastsector)
      secread=p->current_lastsector-adjread+1;
    
    if(sofar+secread>totaltoread)secread=totaltoread-sofar;
    
    if(secread>0){
      
      if(firstread<0)firstread=adjread;
      if((thisread=cdda_read(p->d,buffer+sofar*CD_FRAMEWORDS,adjread,
			    secread))<secread){

	if(thisread<0)thisread=0;

	/* Uhhh... right.  Make something up. But don't make us seek
           backward! */

	(*callback)((adjread+thisread)*CD_FRAMEWORDS,PARANOIA_CB_READERR);  
	memset(buffer+(sofar+thisread)*CD_FRAMEWORDS,0,
	       CD_FRAMESIZE_RAW*(secread-thisread));
	if(flags)memset(flags+(sofar+thisread)*CD_FRAMEWORDS,2,
	       CD_FRAMEWORDS*(secread-thisread));
      }
      if(thisread!=0)anyflag=1;
      
      if(flags && sofar!=0){
	/* Don't verify across overlaps that are too close to one
           another */
	int i=0;
	for(i=-MIN_WORDS_OVERLAP/2;i<MIN_WORDS_OVERLAP/2;i++)
	  flags[sofar*CD_FRAMEWORDS+i]|=1;
      }

      p->lastread=adjread+secread;
      
      if(adjread+secread-1==p->current_lastsector)
	new->lastsector=-1;
      
      (*callback)((adjread+secread-1)*CD_FRAMEWORDS,PARANOIA_CB_READ);
      
      sofar+=secread;
      readat=adjread+secread; 
    }else
      if(readat<0)
	readat+=sectatonce; /* due to being before the readable area */
      else
	break; /* due to being past the readable area */
  }

  if(anyflag){
    new->vector=isort_alloc_with_buffer(buffer,sofar*CD_FRAMEWORDS);
    new->flags=flags;
    isort_set(new->vector,firstread*CD_FRAMEWORDS-p->dyndrift);
  }else{
    if(new)free_c_block(new);
    free(buffer);
    free(flags);
    new=NULL;
  }
  return(new);
}

/* The returned buffer is *not* to be freed by the caller.  It will
   persist only until the next call to paranoia_read() for this p */

size16 *paranoia_read(cdrom_paranoia *p, void(*callback)(long,int)){

  long beginword=p->cursor*(CD_FRAMEWORDS);
  long endword=beginword+CD_FRAMEWORDS;
  long retry_count=0,lastend=-2;

  if(beginword>p->root.returnedlimit)p->root.returnedlimit=beginword;
  lastend=isort_end(p->root.vector);
  
  /* First, is the sector we want already in the root? */
  while(rv==NULL ||
	rb>beginword || 
	(re<endword+(MAX_SECTOR_OVERLAP*CD_FRAMEWORDS) &&
	 p->enable&(PARANOIA_MODE_VERIFY|PARANOIA_MODE_OVERLAP)) ||
	re<endword){
    
    /* Nope; we need to build or extend the root verified range */
    
    if(p->enable&(PARANOIA_MODE_VERIFY|PARANOIA_MODE_OVERLAP)){
      i_paranoia_trim(p,beginword,endword);
      recover_cache(p);
      if(rb!=-1 && p->root.lastsector)
	i_end_case(p,endword+(MAX_SECTOR_OVERLAP*CD_FRAMEWORDS),
			callback);
      else
	i_stage2(p,beginword,
		      endword+(MAX_SECTOR_OVERLAP*CD_FRAMEWORDS),
		      callback);
    }else
      i_end_case(p,endword+(MAX_SECTOR_OVERLAP*CD_FRAMEWORDS),
		 callback); /* only trips if we're already done */
    
    if(!(rb==-1 || rb>beginword || 
	 re<endword+(MAX_SECTOR_OVERLAP*CD_FRAMEWORDS))) 
      break;
    
    /* Hmm, need more.  Read another block */

    {    
      c_block *new=i_read_c_block(p,beginword,endword,callback);
      
      if(new){
	if(p->enable&(PARANOIA_MODE_OVERLAP|PARANOIA_MODE_VERIFY)){
      
	  if(p->enable&PARANOIA_MODE_VERIFY)
	    i_stage1(p,new,callback);
	  else{
	    /* just make v_fragments from the boundary information. */
	    long begin=0,end=0;
	    
	    while(begin<isort_size(new->vector)){
	      end=begin+1;
	      while(end<isort_size(new->vector)&&(new->flags[end]&1)==0)end++;
	      {
		v_fragment *f=new_v_fragment(p,new,begin+
					     isort_begin(new->vector),
					     end+isort_begin(new->vector),
					     (new->lastsector && 
					      isort_begin(new->vector)+end==
					      isort_end(new->vector)));
	      }
	      begin=end;
	    }
	  }
	  
	}else{

	  if(p->root.vector)isort_free(p->root.vector);
	  p->root.vector=new->vector;
	  new->vector=NULL;
	  free_c_block(new);

	  i_end_case(p,endword+(MAX_SECTOR_OVERLAP*CD_FRAMEWORDS),
			  callback);
      
	}
      }
    }

    /* Are we doing lots of retries?  **************************************/
    
    /* Check unaddressable sectors first.  There's no backoff here; 
       jiggle and minimum backseek handle that for us */
    
    if(rb!=-1 && lastend<re){
      lastend=re;
      retry_count=0;
    }else{
      /* increase overlap or bail */
      retry_count++;
      
      /* The better way to do this is to look at how many actual
	 matches we're getting and what kind of gap */

      if(retry_count%5==0){
	if(p->dynoverlap==MAX_SECTOR_OVERLAP*CD_FRAMEWORDS ||
	   retry_count==20){
	  verify_skip_case(p,callback);
	  retry_count=0;
	}else{
	  p->dynoverlap*=1.5;
	  if(p->dynoverlap>MAX_SECTOR_OVERLAP*CD_FRAMEWORDS)
	    p->dynoverlap=MAX_SECTOR_OVERLAP*CD_FRAMEWORDS;
	  (*callback)(p->dynoverlap,PARANOIA_CB_OVERLAP);
	  
	}
      }
    }
  }
  p->cursor++;

  return(isort_buffer(rv)+(beginword-rb));
}

#ifdef TEST

#undef TEST
#define DEBUG
#include "p_block.c"
#include "gap.c"
#include "overlap.c"
#include "isort.c"
#define TEST
/****************  Testing ********************/

static void sft_setup(size16 *master,
		      root_block *a,v_fragment *b,c_block *c,
		      long begins,long s){
  isort_free(a->vector);
  a->p->root.vector=a->vector=isort_alloc();

  isort_append(a->vector,master,s*CD_FRAMEWORDS);
  isort_set(a->vector,begins*CD_FRAMEWORDS);

  isort_free(c->vector);
  c->vector=isort_alloc();
  isort_append(c->vector,master,s*CD_FRAMEWORDS);
  isort_set(c->vector,begins*CD_FRAMEWORDS);

  b->begin=begins*CD_FRAMEWORDS;
  b->size=s*CD_FRAMEWORDS;
}

static void sft_shift(v_fragment *b, c_block *c,long s){
  isort_set(c->vector,isort_begin(c->vector)+s);
  b->begin+=s;
}

static void sft_verify(size16 *master,
		      root_block *a,v_fragment *b,c_block *c,
		      long shift,char *prompt){

  /* make sure master and a are perfect matches, b/c is a perfect match for
     the existing overlap */
  long count;
  long size=isort_size(a->vector);
  /*long size2=b->size;*/
  size16 *Abuf=isort_buffer(a->vector);
  /*size16 *Bbuf=v_buffer(b);*/

  for(count=0;count<size;count++){
    if(Abuf[count]!=master[count]){
      printf("%s master/A buffer mismatch at position %ld\n",prompt,count);
      exit(0);
    }
  }
}

/* not stream position, buffer position */
static void sft_v_remove(v_fragment *b,c_block *c,long start,long leng){
  isort_remove(c->vector,start,leng);
  b->size-=leng;
}

/* not stream position, buffer position */
static void sft_v_dup(v_fragment *b,c_block *c,long start,long leng){
  size16 *temp=malloc(leng*sizeof(size16));
  memcpy(temp,isort_buffer(c->vector)+start,leng*sizeof(size16));
  isort_insert(c->vector,start,temp,leng);
  free(temp);
  b->size+=leng;
}

static void sft_r_remove(root_block *a,long start,long leng){
  isort_remove(a->vector,start,leng);
}

/* not stream position, buffer position */
static void sft_r_dup(root_block *a,long start,long leng){
  size16 *temp=malloc(leng*sizeof(size16));
  memcpy(temp,isort_buffer(a->vector)+start,leng*sizeof(size16));
  isort_insert(a->vector,start,temp,leng);
  free(temp);
}

static void dummy(long foo,int bar){
}

static void sync_fragment_test(cdrom_paranoia *p){
  root_block a;
  v_fragment b;
  c_block c;

  long ret;
  long i=0;
  long s=100;
  long postsec=16343-s;
  size16 *master=malloc((s+2)*CD_FRAMESIZE_RAW);
  a.vector=NULL;
  c.vector=NULL;

  b.one=&c;
  b.p=p;
  c.p=p;
  p->dynoverlap=256;

  while(i<s){
    i+=cdda_read(p->d,master+(i*CD_FRAMEWORDS),postsec+i,s-i);
  }

  /* test all of the following:
     multiple resync forward and backward of:
     dropped bytes from root, added bytes to root
     dropped bytes from fragment, added bytes to fragment
     syncup past original area 
     small, preceeding, and growing cases */

  /* ONE; no resync */

  sft_setup(master,&a,&b,&c,postsec,s);
  sft_shift(&b,&c,16);
  ret=i_stage2_each(&a,&b,-1,0,&dummy);
  sft_verify(master,&a,&b,&c,0,"no resync");

  sft_setup(master,&a,&b,&c,postsec,s);
  sft_shift(&b,&c,16);
  sft_v_remove(&b,&c,0,20);
  ret=i_stage2_each(&a,&b,-1,0,&dummy);
  sft_verify(master,&a,&b,&c,-20,"shift/jitter");


  sft_setup(master,&a,&b,&c,postsec,s);
  sft_shift(&b,&c,16);
  sft_v_remove(&b,&c,0,200);
  sft_v_remove(&b,&c,70,2);
  sft_v_remove(&b,&c,102,8);
  sft_v_remove(&b,&c,800,16);
  ret=i_stage2_each(&a,&b,-1,0,&dummy);
  sft_verify(master,&a,&b,&c,-200,"v dropped back");

  sft_setup(master,&a,&b,&c,postsec,s);
  sft_shift(&b,&c,16);
  sft_v_dup(&b,&c,0,200);
  sft_v_dup(&b,&c,420,2);
  sft_v_dup(&b,&c,1102,8);
  sft_v_dup(&b,&c,1800,16);
  ret=i_stage2_each(&a,&b,-1,0,&dummy);
  sft_verify(master,&a,&b,&c,-200,"v duped back");

  sft_setup(master,&a,&b,&c,postsec,s);
  sft_shift(&b,&c,16);
  sft_v_remove(&b,&c,0,200);
  sft_v_dup(&b,&c,70,2);
  sft_v_remove(&b,&c,102,8);
  sft_v_dup(&b,&c,800,16);
  ret=i_stage2_each(&a,&b,-1,0,&dummy);
  sft_verify(master,&a,&b,&c,-200,"v both back");

  sft_setup(master,&a,&b,&c,postsec,s);
  sft_shift(&b,&c,16);
  sft_v_remove(&b,&c,0,200);
  sft_r_remove(&a,270,2);
  sft_r_remove(&a,302,8);
  sft_r_remove(&a,900,16);
  ret=i_stage2_each(&a,&b,-1,0,&dummy);
  sft_verify(master,&a,&b,&c,-200,"root dropped back");

  sft_setup(master,&a,&b,&c,postsec,s);
  sft_shift(&b,&c,16);
  sft_v_remove(&b,&c,0,200);
  sft_r_dup(&a,420,2);
  sft_r_dup(&a,1102,8);
  sft_r_dup(&a,1800,16);
  ret=i_stage2_each(&a,&b,-1,0,&dummy);
  sft_verify(master,&a,&b,&c,-200,"root duped back");

  sft_setup(master,&a,&b,&c,postsec,s);
  sft_shift(&b,&c,16);
  sft_v_remove(&b,&c,0,200);
  sft_r_dup(&a,370,2);
  sft_r_remove(&a,502,8);
  sft_r_dup(&a,800,16);
  ret=i_stage2_each(&a,&b,-1,0,&dummy);
  sft_verify(master,&a,&b,&c,-200,"root both back");

  /******************/

  sft_setup(master,&a,&b,&c,postsec,s);
  sft_shift(&b,&c,16);
  sft_v_remove(&b,&c,0,100);
  sft_v_remove(&b,&c,70,2);
  sft_v_remove(&b,&c,102,8);
  sft_v_remove(&b,&c,800,16);
  sft_v_remove(&b,&c,1000,16);
  ret=i_stage2_each(&a,&b,300+isort_begin(a.vector),0,&dummy);
  sft_verify(master,&a,&b,&c,-100,"v dropped both");

  sft_setup(master,&a,&b,&c,postsec,s);
  sft_shift(&b,&c,16);
  sft_v_dup(&b,&c,50,150);
  sft_v_dup(&b,&c,420,2);
  sft_v_dup(&b,&c,1102,8);
  sft_v_dup(&b,&c,1800,16);
  ret=i_stage2_each(&a,&b,400+isort_begin(a.vector),0,&dummy);
  sft_verify(master,&a,&b,&c,-200,"v duped both");

  sft_setup(master,&a,&b,&c,postsec,s);
  sft_shift(&b,&c,16);
  sft_v_remove(&b,&c,8,192);
  sft_v_dup(&b,&c,70,2);
  sft_v_remove(&b,&c,102,8);
  sft_v_dup(&b,&c,800,16);
  ret=i_stage2_each(&a,&b,400+isort_begin(a.vector),0,&dummy);
  sft_verify(master,&a,&b,&c,-200,"v both both");

  sft_setup(master,&a,&b,&c,postsec,s);
  sft_shift(&b,&c,16);
  sft_v_remove(&b,&c,0,200);
  sft_r_remove(&a,270,2);
  sft_r_remove(&a,302,8);
  sft_r_remove(&a,900,16);
  ret=i_stage2_each(&a,&b,400+isort_begin(a.vector),0,&dummy);
  sft_verify(master,&a,&b,&c,-200,"root dropped both");

  sft_setup(master,&a,&b,&c,postsec,s);
  sft_shift(&b,&c,16);
  sft_v_remove(&b,&c,0,200);
  sft_r_dup(&a,420,2);
  sft_r_dup(&a,1102,8);
  sft_r_dup(&a,1800,16);
  ret=i_stage2_each(&a,&b,400+isort_begin(a.vector),0,&dummy);
  sft_verify(master,&a,&b,&c,-200,"root duped both");

  sft_setup(master,&a,&b,&c,postsec,s);
  sft_shift(&b,&c,16);
  sft_v_remove(&b,&c,0,200);
  sft_r_dup(&a,370,2);
  sft_r_remove(&a,502,8);
  sft_r_dup(&a,800,16);
  isort_remove(a.vector,isort_end(a.vector)-400,-1);
  ret=i_stage2_each(&a,&b,400+isort_begin(a.vector),0,&dummy);
  sft_verify(master,&a,&b,&c,-200,"root both both + glom");

}

int main(){
  cdrom_drive *d=cdda_find_a_cdrom(1,NULL);
  cdrom_paranoia *p;

  if(d==NULL){
    printf("cdda_find_a_cdrom failed\n");
    return(1);
  }
  cdda_verbose_set(d,CDDA_MESSAGE_PRINTIT,CDDA_MESSAGE_PRINTIT);
  switch(cdda_open(d)){
  case -2:case -3:case -4:case -5:
    printf("\nUnable to open disc.  Is there an audio CD in the drive?");
    exit(1);
  case -6:
    printf("\nCdparanoia could not find a way to read audio from this drive.");
    exit(1);
  case 0:
    break;
  default:
    printf("\nUnable to open disc.");
    exit(1);
  }

  p=paranoia_init(d);
  sync_fragment_test(p);
  
  return(0);
}
#endif
