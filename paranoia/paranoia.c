/***
 * CopyPolicy: GNU Public License 2 applies
 * Copyright (C) by Monty (xiphmont@mit.edu)
 *
 * Toplevel file for the paranoia abstraction over the cdda lib 
 *
 ***/

/* immediate todo:: */
/* optimize readahead given the average number of sectors are matched
   good at a time (ie, errors are rare on a Teac CD48-E.  A 150
   readahead works well.  On a NEC 464, you want a small readahead) */
/* scratch detection/tolerance not implemented yet */
/* Skip correctly; don't replace the whole sector */

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
#include <limits.h>
#include "../interface/cdda_interface.h"
#include "../interface/smallft.h"
#include "cdda_paranoia.h"
#include "p_block.h"

#define MIN_WORDS_OVERLAP    64     /* 16 bit words */
#define MIN_WORDS_SEARCH     64     /* 16 bit words */
#define MIN_WORDS_RIFT       16     /* 16 bit words */
#define MAX_SECTOR_OVERLAP   32     /* sectors */
#define MIN_SECTOR_EPSILON  128     /* words */
#define MIN_SECTOR_BACKUP    16     /* sectors */
#define JIGGLE_MODULO         8     /* sectors */

#define min(x,y) ((x)>(y)?(y):(x))
#define max(x,y) ((x)<(y)?(y):(x))

/*#define TEST
extern void sync_fragment_test(cdrom_paranoia *p);
*/

/**** extra TOC stuff ****************************************************/

static void i_paranoia_firstlast(cdrom_paranoia *p){
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

/**** Internal data structure management *********************************/

static void paranoia_resetcache(cdrom_paranoia *p){
  {
    c_block *next,*current=p->cache.head;
    while(current){
      next=current->next;
      release_c_block(current);
      current=next;
    }
  }

  {
    v_fragment *next,*current=p->fragments.head;
    while(current){
      next=current->next;
      release_v_fragment(current);
      current=next;
    }
  }
}

static void paranoia_resetall(cdrom_paranoia *p){
  p->root.returnedlimit=0;
  p->root.done=0;
  p->root.done=0;
  p->dyndrift=0;

  if(p->root.buffer){
    free(p->root.buffer);
    p->root.buffer=NULL;
  }

  paranoia_resetcache(p);
}

/**** Statistical and heuristic[al? :-] management ************************/

static void offset_adjust_settings(cdrom_paranoia *p, 
				   void(*callback)(long,int)){
  {
    /* drift: look at the average offset value.  If it's over one
       sector, frob it.  We just want a little hysteresis [sp?]*/
    long av=(p->offpoints?p->offaccum/p->offpoints:0);
    
    if(abs(av)>p->dynoverlap/4){
      av=(av/MIN_SECTOR_EPSILON)*MIN_SECTOR_EPSILON;

#ifdef NOISY
      fprintf(stderr,"Adjusting drift: offpoints:%ld offaccum:%ld av:%ld\n",
	      p->offpoints,p->offaccum,av);
#endif

      (*callback)(p->root.end,PARANOIA_CB_DRIFT);
      p->dyndrift+=av;
      
      /* Adjust all the values in the cache otherwise we get a
	 (potentially unstable) feedback loop */
      {
	c_block *c=p->cache.head;
	v_fragment *v=p->fragments.head;

	while(v && v->one){
	  /* safeguard beginning bounds case with a hammer */
	  if(v->begin<av || v->one->begin<av || v->two->begin<av){
	    v->one=NULL;
	    v->two=NULL;
	  }else{
	    v->begin-=av;
	    v->end-=av;
	  }
	  v=v->next;
	}
	while(c){
	  long adj=min(av,c->begin);
	  c->begin-=adj;
	  c->end-=adj;
	  c=c->next;
	}
      }


      /* adjust other statistics to be consistent with new drift val */
      p->offaccum-=(av*p->offpoints);
      p->offmin-=av;
      p->offmax-=av;
    }
  }

  {
    /* dynoverlap: we arbitrarily set it to 4x the running difference
       value, unless mix/max are more */

    p->dynoverlap=(p->offpoints?p->offdiff/p->offpoints*4:CD_FRAMEWORDS);

    if(p->dynoverlap<-p->offmin*4)
      p->dynoverlap=-p->offmin*4;
						     
    if(p->dynoverlap<p->offmax*4)
      p->dynoverlap=p->offmax*4;

    if(p->dynoverlap<MIN_SECTOR_EPSILON)p->dynoverlap=MIN_SECTOR_EPSILON;
    if(p->dynoverlap>MAX_SECTOR_OVERLAP*CD_FRAMEWORDS)
      p->dynoverlap=MAX_SECTOR_OVERLAP*CD_FRAMEWORDS;
    			     
    (*callback)(p->dynoverlap,PARANOIA_CB_OVERLAP);

  }
}

static void offset_clear_settings(cdrom_paranoia *p){
  p->offpoints=0;
  p->offaccum=0;
  p->offdiff=0;
  p->offmin=0;
  p->offmax=0;
}

static void offset_add_value(cdrom_paranoia *p,long value,
			     void(*callback)(long,int)){
  if(p->offpoints){
    long av=p->offaccum/p->offpoints;
    p->offdiff+=abs(value-av);
  }

  p->offpoints++;
  p->offaccum+=value;
  if(value<p->offmin)p->offmin=value;
  if(value>p->offmax)p->offmax=value;

  if(p->offpoints>=10){
    offset_adjust_settings(p,callback);
    offset_clear_settings(p);
  }
}

/**** Gap analysis code ***************************************************/

static long i_paranoia_overlap(size16 *buffA,size16 *buffB,
			       long offsetA, long offsetB,
			       long sizeA,long sizeB,
			       long *ret_begin, long *ret_end){
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
  
  if(ret_begin)*ret_begin=beginA;
  if(ret_end)*ret_end=endA;
  return(endA-beginA);
}

static long i_paranoia_overlap2(size16 *buffA,size16 *buffB,
				char *flagsA,char *flagsB,
				long offsetA, long offsetB,
				long sizeA,long sizeB,
				long *ret_begin, long *ret_end){
  long beginA=offsetA,endA=offsetA;
  long beginB=offsetB,endB=offsetB;

  for(;beginA>=0 && beginB>=0;beginA-=2,beginB-=2){
    if(buffA[beginA]!=buffB[beginB] ||
       buffA[beginA+1]!=buffB[beginB+1])break;
    /* don't allow matching across matching sector boundaries */
    if(flagsA[beginA]&flagsB[beginB]&1){
      
      beginA-=2;
      beginB-=2;
      break;
    }
  }
  beginA+=2;
  beginB+=2;
  
  for(;endA+1<sizeA && endB+1<sizeB;endA+=2,endB+=2){
    if(buffA[endA]!=buffB[endB] ||
       buffA[endA+1]!=buffB[endB+1])break;
    /* don't allow matching across matching sector boundaries */
    if((flagsA[endA]&flagsB[endB]&1)&&endA!=beginA){
      break;
    }
  }

  if(ret_begin)*ret_begin=beginA;
  if(ret_end)*ret_end=endA;
  return(endA-beginA);
}

static long i_paranoia_overlap_r(size16 *buffA,size16 *buffB,
				 long offsetA, long offsetB){
  long beginA=offsetA;
  long beginB=offsetB;

  for(;beginA>=0 && beginB>=0;beginA-=2,beginB-=2)
    if(buffA[beginA]!=buffB[beginB] ||
       buffA[beginA+1]!=buffB[beginB+1])break;
  beginA+=2;
  beginB+=2;
  
  return(offsetA-beginA);
}

static long i_paranoia_overlap_f(size16 *buffA,size16 *buffB,
				 long offsetA, long offsetB,
				 long sizeA,long sizeB){
  long endA=offsetA;
  long endB=offsetB;

  for(;endA+1<sizeA && endB+1<sizeB;endA+=2,endB+=2)
    if(buffA[endA]!=buffB[endB] ||
       buffA[endA+1]!=buffB[endB+1])break;
  
  return(endA-offsetA);
}

static int set_sort_pointers(size16 *A,size16 *B,
			     long *indexA,long *indexB,
			     long sizeA,long sizeB,
			     long *lowA,long *highA,
			     long *lowB,long *highB){
  int Aval,Bval;

  while(*highA<sizeA && indexA[*highA]==-1)(*highA)++;
  while(*highB<sizeB && indexB[*highB]==-1)(*highB)++;

  if(*highA>=sizeA)return(0);
  if(*highB>=sizeB)return(0);

  while((Aval=A[indexA[*highA]])!=(Bval=B[indexB[*highB]])){
    if(Aval>Bval)
      while(++*highB<sizeB && (indexB[*highB]==-1 || B[indexB[*highB]]<Aval));
    else
      while(++*highA<sizeA && (indexA[*highA]==-1 || A[indexA[*highA]]<Bval));

    if(*highA>=sizeA)return(0);
    if(*highB>=sizeB)return(0);

  }

  *lowA=*highA;*lowB=*highB;

  /* how high does this block go? */

  while(++*highA<sizeA && (indexA[*highA]==-1 || A[indexA[*highA]]<=Aval));
  while(++*highB<sizeB && (indexB[*highB]==-1 || B[indexB[*highB]]<=Bval));
  return(1);
}

/* Use a sort to short-circuit a convolution */

/* We're sorting 16 bit values; given 16 bits * 4 bytes of working
   space, we can cheat bigtime and do this in linear time.  Qsort is
   still faster for one or two sectors at a time, bigger than that
   bucket_sort is faster (this is due to the large constant time hit
   from 16 bit vals) */

/* A radix/bucket sort may be faster yet... try it later */

static void bucket_sort(size16 *v,long *indexes,long size){
  long *work=calloc(65536,sizeof(long));
  long i,accum;
  
  for(i=0;i<size;i++)
    work[v[i]+32768]++;

  accum=0;

  for(i=0;i<65536;i++)
    work[i]=accum+=work[i];

  for(i=0;i<size;i++)
    indexes[--work[v[i]+32768]]=i;

  free(work);
}

static int fragsort(const void *a,const void *b){
  long sizeA=(*(v_fragment **)a)->end-(*(v_fragment **)a)->begin;
  long sizeB=(*(v_fragment **)b)->end-(*(v_fragment **)b)->begin;
   return(sizeB-sizeA);
}

static size16 *fragptr=NULL;
static int fragcomp(const void *a,const void *b){
  int foo=*(long *)a;
  int bar=*(long *)b;
  size16 baz=fragptr[*((long *)a)];
  size16 quux=fragptr[*((long *)b)];
  return((int)(fragptr[*((long *)a)])-(int)(fragptr[*((long *)b)]));
}

static long i_make_v_fragments(c_block *a,c_block *b,
			       void(*callback)(long,int)){
  cdrom_paranoia *p=a->p;
  long dynoverlap=p->dynoverlap;
  long i,j,k;

  /* OK!  We only comapre and contrast within a dynoverlap range at a
     time.  When done with that, we march forward if end hasn't
     actually extended to the end */

  long post=max(a->begin,b->begin);
  long beginA=max(0,post-a->begin-dynoverlap/2);
  long beginB=max(0,post-b->begin-dynoverlap/2);
  long endA=min(a->end-a->begin,post-a->begin+dynoverlap/2);
  long endB=min(b->end-b->begin,post-b->begin+dynoverlap/2);

  long maxA=a->end-a->begin;
  long maxB=b->end-b->begin;
  long matches=0;
  long finalmatches=0;
  long begin,end,offset;

  long *indexA=malloc(dynoverlap*sizeof(long));
  long *indexB=malloc(dynoverlap*sizeof(long));
  long *revindexA=malloc(dynoverlap*sizeof(long));
  long *revindexB=malloc(dynoverlap*sizeof(long));

#ifdef NOISY
  fprintf(stderr,"Trying %d[%ld-%ld]<->%d[%ld-%ld]  dynoverlap:%ld\n",
	  a->stamp,a->begin,a->end,
	  b->stamp,b->begin,b->end,dynoverlap);

#endif

  /* Some drives are perfect; try the zero offset right off.  In
     addition to speed, we make sure perfect drives don't report
     mismatches on zero vectors */
  
  if(post>=a->begin && post<a->end &&
     post>=b->begin && post<b->end){
    long ret=i_paranoia_overlap2(a->buffer,b->buffer,
			a->flags,b->flags,
			post-a->begin,post-b->begin,
			a->end-a->begin,b->end-b->begin,
			&begin,&end);

    if(ret>0){
      if(begin==a->begin || begin==b->begin)
	if(end==a->end || end==b->end){
	  
	  v_fragment *f=new_v_fragment(p);
#ifdef NOISY
	  fprintf(stderr,"offset:0");
	  fprintf(stderr," match:ALL\n");
#endif
	  
	  f->one=a;
	  f->two=b;
	  f->begin=begin+a->begin;
	  f->end=end+a->begin;
	  f->offset=0;
	  
	  if((a->lastsector && f->end==a->end)||
	     (b->lastsector && f->end==b->end-offset)){
	    f->lastsector=1;
	  }
	  return(1);
	}
    }
  }
  
  while(beginA<maxA && beginB<maxB){

    size16 *A=a->buffer+beginA;
    size16 *B=b->buffer+beginB;
    long sizeA=endA-beginA;
    long sizeB=endB-beginB;
    long maxendA=endA;
    long maxendB=endB;

    if(sizeA>CD_FRAMEWORDS*6){
      bucket_sort(A,indexA,sizeA);
      bucket_sort(B,indexB,sizeB);
    }else{
      fragptr=A;
      for(i=0;i<sizeA;i++)indexA[i]=i;
      qsort(indexA,sizeA,sizeof(long),fragcomp);
      fragptr=B;
      for(i=0;i<sizeB;i++)indexB[i]=i;
      qsort(indexB,sizeB,sizeof(long),fragcomp);
    }

    for(i=0;i<sizeA;i++)revindexA[indexA[i]]=i;
    for(i=0;i<sizeB;i++)revindexB[indexB[i]]=i;

    /* Crawl up both chimneys... */

    {
      long lowindexA=-1,highindexA=0,lowindexB=-1,highindexB=0;
      long doneflag=0;
      long tries=0;
      while(set_sort_pointers(A,B,indexA,indexB,sizeA,sizeB,
			      &lowindexA,&highindexA,
			      &lowindexB,&highindexB) && !doneflag){

	if(callback)(*callback)(post,PARANOIA_CB_VERIFY);

	for(i=lowindexA;i<highindexA && !doneflag;i++)
	  if(indexA[i]!=-1)
	    for(j=lowindexB;j<highindexB && !doneflag;j++)
	      if(indexB[j]!=-1){
		if(!((indexA[i]^indexB[j])&1)){
		  long begin,end;
		  long localoffset=indexB[j]+beginB-indexA[i]-beginA;
		  long ret; 


		  offset=(b->begin+beginB+indexB[j])-
		    (a->begin+beginA+indexA[i]);
		
		  tries++;
		  ret=i_paranoia_overlap2(a->buffer,b->buffer,
					  a->flags,b->flags,
					  beginA+(indexA[i]&0x7ffffffe),
					  beginB+(indexB[j]&0x7ffffffe),
					  maxA,maxB,&begin,&end);

		  if(ret>=MIN_WORDS_SEARCH){

		    if(end>maxendA)maxendA=end;
		    if(end+localoffset>maxendB)maxendB=end+localoffset;
		    
		    if(ret>=MIN_WORDS_OVERLAP){
		      v_fragment *f=new_v_fragment(p);
		      matches++;
		      
		      f->one=a;
		      f->two=b;
		      f->begin=begin+a->begin;
		      f->end=end+a->begin;
		      f->offset=offset;
		      

		      /* this isn't exactly correct as the boundary
                         locations are not currently cemented to the
                         root buffer; fix it up in real logging later */

		      if((a->flags[begin]&1) ||
			 (b->flags[begin+localoffset]&1)){
			if(offset)
			  (*callback)(a->begin,PARANOIA_CB_FIXUP_EDGE);
		      }else{
			(*callback)(a->begin,PARANOIA_CB_FIXUP_ATOM);
		      }

		      if(end>=maxA || (a->flags[end]&1) ||
			 end+localoffset>=maxB || 
			 (b->flags[end+localoffset]&1)){
			if(offset)
			  (*callback)(a->end,PARANOIA_CB_FIXUP_EDGE);
		      }else{
			(*callback)(a->end,PARANOIA_CB_FIXUP_ATOM);
		      }

		      if((a->lastsector && f->end==a->end)||
			 (b->lastsector && f->end==b->end-offset)){
			f->lastsector=1;
		      }
		      
		      if((begin<=beginA || begin+localoffset<=beginB)&&
			 (end>=endA || end+localoffset>=endB)){
			/* we're done. */
			doneflag=1;
		      }
		    }
		    
		    /* Optimization: we need to mark all the other values
		       in the begin->end range with -1 so that they are
		       *not* checked (they'll probably arrive at the same
		       answer */
		    
		    for(k=max(begin,beginA);k<end && k<endA;k++)
		      indexA[revindexA[k-beginA]]=-1;
		    for(k=max(begin+localoffset,beginB);
			k<end+localoffset && k<endB;k++)
		      indexB[revindexB[k-beginB]]=-1;
		    if(indexA[i]==-1)break;
		  }		  
		}
	      }
      }
    }


    beginA=maxendA;
    beginB=maxendB;
    endA=min(beginA+dynoverlap,maxA);
    endB=min(beginB+dynoverlap,maxB);

  }

  /* We now (possibly) have a whole huge slew of new fragments.  Sort
     them for size, weight for non-zero matches and offset, and 'tile'
     them to avoid overlap.  Greedy approach, 'biggest' frags first. */
  
  free(indexA);
  free(indexB);
  free(revindexA);
  free(revindexB);
  
  if(matches>0){
    v_fragment **fragments=malloc(sizeof(v_fragment *)*matches);
    v_fragment *this=p->fragments.head;
    for(i=0;i<matches;i++){
      fragments[i]=this;
      this=this->next;
    }
      
    qsort(fragments,matches,sizeof(v_fragment *),fragsort);

    for(i=0;i<matches;i++){
      v_fragment *a=fragments[i];
      int flag=0;
      
      for(j=0;j<i;j++){
	v_fragment *b=fragments[j];
	if(a && b){
	  long ao=a->offset;
	  long bo=b->offset;
	  
	  /*check overlap with *and* without offsets.  Duplicated bytes will
	    cause one but not both to match*/ 
	  
	  if(min(a->end,b->end)>=max(a->begin,b->begin) &&
	     min(a->end+ao,b->end+bo)>=max(a->begin+ao,b->begin+bo)){
	    flag=1;
	    break;
	  }
	}
	if(flag)break;
      }

      if(flag){

        release_v_fragment(a);
	fragments[i]=NULL;
      }else{
	finalmatches++;
      }
    }
    free(fragments);
  }
  
#ifdef NOISY
  fprintf(stderr,"initial matches: %ld\n",matches);
  fprintf(stderr,"final matches: %ld\n",finalmatches);
  fprintf(stderr,"-------------------\n");
#endif
  return(matches);
}


static long i_sort_sync(size16 *A,long maxA,long beginA,long sizeA,
			size16 *B,long maxB,long beginB,long sizeB,
			long *begin,long *end,void(*callback)(long,int),
			long post){

  long i,j;
  long *indexA=malloc(sizeA*sizeof(long));
  long *indexB=malloc(sizeB*sizeof(long));
  long *revindexA=malloc(sizeA*sizeof(long));
  long *revindexB=malloc(sizeB*sizeof(long));

  if(sizeA>CD_FRAMEWORDS*6)
    bucket_sort(A+beginA,indexA,sizeA);
  else{
    fragptr=A+beginA;
    for(i=0;i<sizeA;i++)indexA[i]=i;
    qsort(indexA,sizeA,sizeof(long),fragcomp);
  }

  if(sizeB>CD_FRAMEWORDS*6)
    bucket_sort(B+beginB,indexB,sizeB);
  else{
    fragptr=B+beginB;
    for(i=0;i<sizeB;i++)indexB[i]=i;
    qsort(indexB,sizeB,sizeof(long),fragcomp);
  }

  for(i=0;i<sizeA;i++)revindexA[indexA[i]]=i;
  for(i=0;i<sizeB;i++)revindexB[indexB[i]]=i;

  /* Crawl up both chimneys... */
  
  {
    long lowindexA=-1,highindexA=0,lowindexB=-1,highindexB=0;
    long doneflag=0;
    while(set_sort_pointers(A+beginA,B+beginB,indexA,indexB,sizeA,sizeB,
			    &lowindexA,&highindexA,
			    &lowindexB,&highindexB) && !doneflag){
	
    if(callback)(*callback)(post,PARANOIA_CB_VERIFY);

    for(i=lowindexA;i<highindexA && !doneflag;i++)
      if(indexA[i]!=-1)
	for(j=lowindexB;j<highindexB && !doneflag;j++)
	  if(indexB[j]!=-1){
	    if(!((indexA[i]^indexB[j])&1)){
	      long localoffset=indexB[j]+beginB-indexA[i]-beginA;
	      
	      if(i_paranoia_overlap(A,B,
				    beginA+(indexA[i]&0x7ffffffe),
				    beginB+(indexB[j]&0x7ffffffe),
				    maxA,maxB,begin,end)>=
		 MIN_WORDS_OVERLAP){
		free(indexA);
		free(indexB);
		free(revindexA);
		free(revindexB);
		return(localoffset);
	      }
	    }
	  }
    }
  }
  *begin=-1;
  *end=-1;
  free(indexA);
  free(indexB);
  free(revindexA);
  free(revindexB);
  return(0);
}

static int i_stutter_or_gap(size16 *A, size16 *B,long offA, long offB,
			    long gap){
  long a1=offA;
  long b1=offB;
 
  if(a1<0){
    b1-=a1;
    gap+=a1;
    a1=0;
  }

  return(memcmp(A+a1,B+b1,gap*2));
}

/* riftv is the first value into the rift -> or <- */
static void i_analyze_rift_f(size16 *A,size16 *B,
			     long sizeA, long sizeB,
			     long aoffset, long boffset, 
			     long *matchA,long *matchB,long *matchC){

  long apast=sizeA-aoffset;
  long bpast=sizeB-boffset;
  long i;

  *matchA=0, *matchB=0, *matchC=0;

  /* Look for three possible matches... (A) Ariftv->B, (B) Briftv->A and 
     (c) AB->AB. */
  
  for(i=0;;i+=2){
    if(i<bpast) /* A */
      if(i_paranoia_overlap_f(A,B,aoffset,boffset+i,sizeA,sizeB)>=MIN_WORDS_RIFT){
	*matchA=i;
	break;
      }

    if(i<apast){ /* B */
      if(i_paranoia_overlap_f(A,B,aoffset+i,boffset,sizeA,sizeB)>=MIN_WORDS_RIFT){
	*matchB=i;
	break;
      }
      if(i<bpast) /* C */
	if(i_paranoia_overlap_f(A,B,aoffset+i,boffset+i,sizeA,sizeB)>=MIN_WORDS_OVERLAP){
	  *matchC=i;
	  break;
	}
    }else
      if(i>=bpast)break;
    
  }
  
  if(*matchA==0 && *matchB==0 && *matchC==0)return;
  
  if(*matchC)return;
  if(*matchA){
    if(i_stutter_or_gap(A,B,aoffset-*matchA,boffset,*matchA))
      return;
    *matchB=-*matchA; /* signify we need to remove n bytes from B */
    *matchA=0;
    return;
  }else{
    if(i_stutter_or_gap(B,A,boffset-*matchB,aoffset,*matchB))
      return;
    *matchA=-*matchB;
    *matchB=0;
    return;
  }
}

/* riftv must be first even val of rift moving back */

static void i_analyze_rift_r(size16 *A,size16 *B,
			     long sizeA, long sizeB,
			     long aoffset, long boffset, 
			     long *matchA,long *matchB,long *matchC){

  long apast=aoffset+2;
  long bpast=boffset+2;
  long i;
  
  *matchA=0, *matchB=0, *matchC=0;

  /* Look for three possible matches... (A) Ariftv->B, (B) Briftv->A and 
     (c) AB->AB. */
  
  for(i=0;;i+=2){
    if(i<bpast) /* A */
      if(i_paranoia_overlap_r(A,B,aoffset,boffset-i)>=MIN_WORDS_RIFT){
	*matchA=i;
	break;
      }
    if(i<apast){ /* B */
      if(i_paranoia_overlap_r(A,B,aoffset-i,boffset)>=MIN_WORDS_RIFT){
	*matchB=i;
	break;
      }      
      if(i<bpast) /* C */
	if(i_paranoia_overlap_r(A,B,aoffset-i,boffset-i)>=MIN_WORDS_OVERLAP){
	  *matchC=i;
	  break;
	}
    }else
      if(i>=bpast)break;
    
  }
  
  if(*matchA==0 && *matchB==0 && *matchC==0)return;
  
  if(*matchC)return;

  if(*matchA){
    if(i_stutter_or_gap(A,B,aoffset+2,boffset-*matchA+2,*matchA))
      return;
    *matchB=-*matchA; /* signify we need to remove n bytes from B */
    *matchA=0;
    return;
  }else{
    if(i_stutter_or_gap(B,A,boffset+2,aoffset-*matchB+2,*matchB))
      return;
    *matchA=-*matchB;
    *matchB=0;
    return;
  }
}

static int i_init_root(root_block *root, v_fragment *v,long begin,
		       void(*callback)(long,int)){
  long vbegin=v->begin,vend=v->end;

  if(v->offset<0){
    vbegin+=v->offset;
    vend+=v->offset;
  }

#ifdef NOISY
  fprintf(stderr,"init attempt: post:%ld [%ld-%ld]\n",begin,vbegin,vend);
#endif

  if(vbegin<=begin && vend>begin){
    root->begin=vbegin;
    root->end=vend;
    root->done=v->lastsector;
    root->returnedlimit=begin;

    root->buffer=malloc((root->end-root->begin)*sizeof(size16));
    memcpy(root->buffer,v_buffer(v),(root->end-root->begin)*sizeof(size16));

    return(1);

  }else
    return(0);
}

/* slices length words into r at point rbegin from l at lbegin */ 
static int root_expand(root_block *r,long rbegin,
			root_block *l,long lbegin,long length){

  rbegin+=r->begin;
  lbegin+=l->begin;

  if(rbegin<r->returnedlimit)return(1);
  if(rbegin>r->end || rbegin<r->begin)return(1);
  if(lbegin+length>l->end || lbegin<l->begin)return(1);
  if(length<=0)return(1);

  r->buffer=realloc(r->buffer,(r->end-r->begin+length)*sizeof(size16));
#ifdef NOISY
  fprintf(stderr,"memcpy re1\n");
#endif
  memmove(r->buffer+(rbegin-r->begin)+length,
	  r->buffer+(rbegin-r->begin),(r->end-rbegin)*sizeof(size16));
#ifdef NOISY
  fprintf(stderr,"memcpy re2\n");
#endif
  memcpy(r->buffer+(rbegin-r->begin),
	 l->buffer+(lbegin-l->begin),length*sizeof(size16));

  r->end+=length;
  return(0);
}

/* removes -length words from r starting at rbegin */
static int root_shrink(root_block *r,long rbegin,long length){
  rbegin+=r->begin;
  if(rbegin<r->returnedlimit)return(1);
  if(rbegin<r->begin)return(1);
  if(rbegin>r->end+length)return(1);

  memmove(r->buffer+(rbegin-r->begin),r->buffer+(rbegin-r->begin)-length,
	  (r->end-rbegin+length)*sizeof(size16));
  r->end+=length;
  return(0);
}

static int root_spackle(root_block *r,long rbegin,
			root_block *l,long lbegin,long length){
  rbegin+=r->begin;
  lbegin+=l->begin;
  if(rbegin<r->returnedlimit)return(1);
  if(rbegin<r->begin)return(1);
  if(rbegin+length>r->end)return(1);
  if(lbegin<l->begin)return(1);
  if(lbegin+length>l->end)return(1);

#ifdef NOISY
  fprintf(stderr,"memcpy rs\n");
#endif
  memcpy(r->buffer+rbegin-r->begin,l->buffer+lbegin-l->begin,
	 length*sizeof(size16));
  return(0);

}

/* reconcile v_fragments to root buffer.  Free if used, fragment root
   if necessary */

static int i_sync_fragment(root_block *root, v_fragment *v, long forcepost,
			void(*callback)(long,int)){

  cdrom_paranoia *p=v->p;
  long dynoverlap=p->dynoverlap/2;
  long post=min(root->end,v->end)-2;
  
  if(!v || !v->one)return(0);
  if(forcepost>=0)post=forcepost;

  if(!root->buffer){
    return(0);
  }else{

    long begin,end;
    long offset;

    long maxB=v->end-v->begin;
    long beginB=max(post-dynoverlap-v->begin,0);
    long endB=min(post+dynoverlap-v->begin,maxB);
    long sizeB=endB-beginB;
    
    long maxA=root->end-root->begin;
    long beginA=max(post-dynoverlap-root->begin,0);
    long endA=min(post+dynoverlap-root->begin,maxA);
    long sizeA=endA-beginA;
    size16 *A=root->buffer;
    size16 *B=v_buffer(v);
      
    if(post<0 ||
       sizeA<MIN_WORDS_OVERLAP ||
       sizeB<MIN_WORDS_OVERLAP)return(0);
    
    if(callback)(*callback)(post,PARANOIA_CB_VERIFY);

    /* Some drives are perfect; try the zero offset right off.  In
       addition to speed, we make sure perfect drives don't report
       mismatches on zero vectors */

    if(post>=root->begin && post<root->end &&
       post>=v->begin && post<v->end){
      long ret=i_paranoia_overlap(root->buffer,v_buffer(v),
				  post-root->begin,post-v->begin,
				  root->end-root->begin,v->end-v->begin,
				  &begin,&end);
      
      if(ret>0 &&
	 (begin==root->begin || begin==v->begin) &&
	 (end==root->end || end==v->end)){
	
	offset=root->begin-v->begin;
	
      }else{
	
	offset=i_sort_sync(A,maxA,beginA,sizeA,B,maxB,beginB,sizeB,
			   &begin,&end,callback,post);
      }
    }else{
      
      offset=i_sort_sync(A,maxA,beginA,sizeA,B,maxB,beginB,sizeB,
			 &begin,&end,callback,post);
    }
      
    if(begin!=-1){

      /* we have a match! We don't rematch off rift, we chase the
	 match all the way to both extremes doing rift analysis. */
      
      /* easier if we copy the v buffer at this point */
      root_block localb;
      root_block *l=&localb;
      l->returnedlimit=0;
      l->begin=v->begin;
      l->end=v->end;
      l->done=v->lastsector;
      l->buffer=malloc((l->end-l->begin)*sizeof(size16));
      memcpy(l->buffer,v_buffer(v),(l->end-l->begin)*sizeof(size16));
      
#ifdef NOISY
      fprintf(stderr,"Dynoverlap: %ld\n",dynoverlap);
      fprintf(stderr,"Stage 2 match beginA:%ld beginB:%ld post:%ld\n",beginA,beginB,post);
      fprintf(stderr,"                endA:%ld endB:%ld offset:%ld\n",endA,endB,offset);
#endif

      /* chase backward */
      /* note that we don't extend back right now, only forward. */
      while((begin+offset>1 && begin>1)){ /* 1 to be safe */
	long matchA=0,matchB=0,matchC=0;
	long beginL=begin+offset;

	i_analyze_rift_r(root->buffer,l->buffer,
			 root->end-root->begin,l->end-l->begin,
			 begin-2,beginL-2,
			 &matchA,&matchB,&matchC);

#ifdef NOISY
	fprintf(stderr,"matching rootR: matchA:%ld matchB:%ld matchC:%ld\n",
		matchA,matchB,matchC);
#endif		

	if(matchA){
	  /* a problem with root */
	  if(matchA>0){
	    /* dropped bytes; add back from v */
	    (*callback)(begin+root->begin-2,PARANOIA_CB_FIXUP_DROPPED);
	    if(root_expand(root,begin,l,beginL-matchA,matchA))
	      break;
	    else{
	      offset-=matchA;
	      begin+=matchA;
	      end+=matchA;
	    }
	  }else{
	    /* duplicate bytes; drop from root */
	    (*callback)(begin+root->begin-2,PARANOIA_CB_FIXUP_DUPED);
	    if(root_shrink(root,begin+matchA,matchA))
	      break;
	    else{
	      offset-=matchA;
	      begin+=matchA;
	      end+=matchA;
	    }
	  }
	}else if(matchB){
	  /* a problem with the fragment */
	  if(matchB>0){
	    /* dropped bytes */
	    (*callback)(begin+root->begin-2,PARANOIA_CB_FIXUP_DROPPED);
	    if(root_expand(l,beginL,root,begin-matchB,matchB))
	      break;
	    else
	      offset+=matchB;
	  }else{
	    /* duplicate bytes */
	    (*callback)(begin+root->begin-2,PARANOIA_CB_FIXUP_DUPED);
	    if(root_shrink(l,beginL+matchB,matchB))
	      break;
	    else
	      offset+=matchB;
	  }
	}else if(matchC){
	  /* Uhh... problem with both */
	  
	  /* Set 'disagree' flags in root */
	  if(root_spackle(root,begin-matchC,l,beginL-matchC,matchC))
	    break;
	  
	}else{
	  /* Could not determine nature of difficulty... 
	     report and bail */
	  
	  /*RRR(*callback)(post,PARANOIA_CB_XXX);*/
	  
	  break;
	}
	/* not the most efficient way, but it will do for now */
	beginL=begin+offset;
	i_paranoia_overlap(root->buffer,l->buffer,
			   begin,beginL,
			   root->end-root->begin,l->end-l->begin,
			   &begin,&end);	
      }
      
      /* chase forward */
      while((end<l->end-l->begin && end<root->end-root->begin)){
	long matchA=0,matchB=0,matchC=0;
	long beginL=begin+offset;
	long endL=end+offset;
	
	i_analyze_rift_f(root->buffer,l->buffer,
			 root->end-root->begin,l->end-l->begin,
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
	    (*callback)(end+root->begin,PARANOIA_CB_FIXUP_DROPPED);
	    if(root_expand(root,end,l,endL,matchA))
	      break;
	  }else{
	    /* duplicate bytes; drop from root */
	    (*callback)(end+root->begin,PARANOIA_CB_FIXUP_DUPED);
	    if(root_shrink(root,end,matchA))
	      break;
	  }
	}else if(matchB){
	  /* a problem with the fragment */
	  if(matchB>0){
	    /* dropped bytes */
	    (*callback)(end+root->begin,PARANOIA_CB_FIXUP_DROPPED);
	    if(root_expand(l,endL,root,end,matchB))
	      break;
	  }else{
	    /* duplicate bytes */
	    (*callback)(end+root->begin,PARANOIA_CB_FIXUP_DUPED);
	    if(root_shrink(l,endL,matchB))
	      break;
	  }
	}else if(matchC){
	  /* Uhh... problem with both */
	  
	  /* Set 'disagree' flags in root */
	  if(root_spackle(root,end,l,endL,matchC))
	    break;
	  
	}else{
	  /* Could not determine nature of difficulty... 
	     report and bail */
	  
	  /*RRR(*callback)(post,PARANOIA_CB_XXX);*/
	  
	  break;
	}
	/* not the most efficient way, but it will do for now */
	i_paranoia_overlap(root->buffer,l->buffer,
			   begin,beginL,
			   root->end-root->begin,l->end-l->begin,
			   NULL,&end);
      }
      
      /* if this extends our range, let's glom */
      
      sizeA=root->end-root->begin;
      sizeB=l->end-l->begin;


      if(sizeB-offset>sizeA){
	
	if(v->lastsector){
	  root->done=1;
	}

	root->buffer=realloc(root->buffer,(sizeB-offset)*sizeof(size16));
	
#ifdef NOISY
	fprintf(stderr,"memcpy glom\n");
#endif

	memcpy(root->buffer+end,l->buffer+end+offset,
	       (sizeB-offset-end)*sizeof(size16));
	root->end=root->begin+sizeB-offset;

#ifdef NOISY
	fprintf(stderr,"Stage 2 glom ->[%ld,%ld]\n",root->end,sizeB-offset);
#endif
	
	/* add both offsets into dynoverlap stats */
	offset_add_value(p,offset+l->begin-root->begin,callback);
	offset_add_value(p,offset+l->begin-root->begin+v->offset,callback);
	if(l->buffer)free(l->buffer);
	return(1);
      }
      
      if(l->buffer)free(l->buffer);
      return(1); /* not really right, but we need to free it */
    }
  } 
  return(0);
}
static void verify_stage1(cdrom_paranoia *p,
			  c_block *firstverify,
			  void(*callback)(long,int)){

  c_block *firstcompare=firstverify->next;

  /* we read in order; new block A does not need to be compared to new
     block B */

  while(firstverify){
    c_block *next=firstverify->prev;
    c_block *compare=firstcompare;
    
    while(compare){
      c_block *next=compare->next;
      i_make_v_fragments(firstverify,compare,callback);
      compare=next;
    }
    firstverify=next;
  }
}

static void verify_stage2(cdrom_paranoia *p,long beginword,long endword,
			  void(*callback)(long,int)){

  int flag=1;
  int count=0;

#ifdef NOISY
  fprintf(stderr,"Fragments:%ld blocks: %ld\n",p->fragments.active,p->fragments.blocks);
  fflush(stderr);
#endif

  while(flag){
    /* loop through all the current fragments */
    v_fragment *first=p->fragments.head;
    flag=0;
    count++;

    while(first){
      v_fragment *next=first->next;
      
      if(first->one && first->two){
	if(p->root.end==-1){
	  if(i_init_root(&(p->root),first,beginword,callback)){
	    release_v_fragment(first);
	    flag=1;
	  }
	}else{
	  if(i_sync_fragment(&(p->root),first,-1,callback)){
	    if(first->one || first->two)release_v_fragment(first);
	    flag=1;
	  }
	}
      }
      first=next;
    }
  }
}

static void verify_end_case(cdrom_paranoia *p,long endword,
			    void(*callback)(long,int)){

  /* have an 'end' flag; if we've just read in the last sector in a
     session, set the flag.  If we verify to the end of a fragment
     which has the end flag set, we're done (set a done flag).  Pad
     zeroes to the end of the read */
  
  if(p->root.done==0)return;
  if(endword<p->root.end)return;
  
  {
    long addto=(endword-p->root.end)*2;
    long newsize=(endword-p->root.begin)*2;
    
    p->root.buffer=realloc(p->root.buffer,newsize);
    memset(p->root.buffer+p->root.end-p->root.begin,0,addto);
    p->root.end=endword;

    /* trash da cache */
    paranoia_resetcache(p);

  }
}

static void verify_skip_case(cdrom_paranoia *p,void(*callback)(long,int)){

  long post=p->root.end;
  long target;
  root_block *root=&(p->root);
  /*  long min=CD_FRAMESIZE_RAW/4;*/
  
#ifdef NOISY
	fprintf(stderr,"\nskipping\n");
#endif

  if(post==-1)post=0;
  target=post+CD_FRAMEWORDS;
  
  (*callback)(post,PARANOIA_CB_SKIP);
  
  root->buffer=realloc(root->buffer,
		       (target-root->begin)*sizeof(size16));
  
  /* We want to add a sector.  Look through v_fragments for something
   that spans. */
  
  {
    v_fragment *v=p->fragments.head;
    while(v){
      if(v->begin<=post && v->end>=target){
	
	memcpy(root->buffer+post-root->begin,
	       v_buffer(v)+post-v->begin,
	       (target-post)*sizeof(size16));
	post=root->end=target;
	break;
      }
      v=v->next;
    }    
  }
  
  if(post<target){
    
    /* No?  Look through c_blocks for a span */
    
    c_block *c=p->cache.head;
    while(c){
      if(c->begin<=post && c->end>=target){
	
	memcpy(root->buffer+post-root->begin,
	       c->buffer+post-c->begin,
	       (target-post)*sizeof(size16));
	post=root->end=target;
	break;
	
      }
      c=c->next;
    }
  }

  if(post<target){
    /* No?  Fine.  Great.  Write in some zeroes :-P */
    
    memset(root->buffer+post-root->begin,
	   0,
	   (target-post)*sizeof(size16));
    root->end=target;

  }
  
  root->returnedlimit=target;

}    

/* if we have ourselves in an odd tight spot */

static void verify_backoff(cdrom_paranoia *p,void(*callback)(long,int)){
  long newpos=p->root.end-CD_FRAMEWORDS/3*2;

#ifdef NOISY
	fprintf(stderr,"\nbacking off\n");
#endif

  (*callback)(newpos,PARANOIA_CB_BACKOFF);

  paranoia_resetcache(p); /*** temporary attempt to ignore real work ***/

  /* back off root and trim it */
  if(newpos>p->root.returnedlimit && newpos>p->root.begin){
    p->root.end=newpos; /* not whole frames */
  }else
    verify_skip_case(p,callback);
}

static void i_paranoia_trim(cdrom_paranoia *p,long beginword,long endword){
  root_block *root=&(p->root);
  if(root->end!=-1){
    long target=beginword-MAX_SECTOR_OVERLAP*CD_FRAMEWORDS;

    if(root->begin>beginword)
      goto rootfree;

    if(root->begin+MAX_SECTOR_OVERLAP*CD_FRAMEWORDS<beginword){
      if(target+MIN_WORDS_OVERLAP>root->end)
	goto rootfree;

      {
	long offset=target-root->begin;
	long tomove=root->end-target;
	memmove(root->buffer,root->buffer+offset,tomove*2);
	root->begin=target;
      }
    }

    {
      c_block *c=p->cache.head;
      while(c){
	c_block *next=c->next;
	if(c->end<beginword-MAX_SECTOR_OVERLAP*CD_FRAMEWORDS)
	  release_c_block(c);
	c=next;
      }
      recover_fragments(p);

    }

  }
  return;

rootfree:
  if(root->buffer)free(root->buffer);
  root->buffer=NULL;
  root->begin=-1;
  root->end=-1;
  root->returnedlimit=-1;
  root->done=0;
  
}

/**** initialization and toplevel ****************************************/

cdrom_paranoia *paranoia_init(cdrom_drive *d){
  cdrom_paranoia *p=calloc(1,sizeof(cdrom_paranoia));

  p->root.begin=-1;
  p->root.end=-1;

  p->d=d;
  p->readahead=150;
  p->dynoverlap=4096;
  p->cache.limit=JIGGLE_MODULO;
  p->enable=PARANOIA_MODE_FULL;
  p->cursor=cdda_disc_firstsector(d);
  p->lastread=LONG_MAX;

  /* One last one... in case data and audio tracks are mixed... */
  i_paranoia_firstlast(p);

  return(p);
}

void paranoia_free(cdrom_paranoia *p){
  int i;

  paranoia_resetall(p);
  for(i=0;i<p->cache.blocks;i++)free(p->cache.pool[i]);
  for(i=0;i<p->fragments.blocks;i++)free(p->fragments.pool[i]);
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

  if(p->root.buffer)free(p->root.buffer);
  p->root.buffer=NULL;
  p->root.begin=-1;
  p->root.end=-1;
  p->root.returnedlimit=0;

  ret=p->cursor;
  p->cursor=sector;

  i_paranoia_firstlast(p);

  return(ret);
}

/* returns last block read, -1 on error */
long i_read_c_block(cdrom_paranoia *p,long beginword,long endword ,
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
  size16 *buffer=NULL;
  char *flags=NULL;
  long lastgood=0;
  long sofar;
  long dynoverlap=(p->dynoverlap+CD_FRAMEWORDS-1)/CD_FRAMEWORDS; 

  /* What is the first sector to read?  want some pre-buffer if
     we're not at the extreme beginning of the disc */
  
  if(p->enable&(PARANOIA_MODE_VERIFY|PARANOIA_MODE_OVERLAP)){
    
    /* we want to jitter the read alignment boundary */
    long target;
    if(p->root.end==-1 || p->root.begin>beginword)
      target=p->cursor-dynoverlap; 
    else
      target=p->root.end/(CD_FRAMEWORDS)-dynoverlap;
	
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
    
    flags=calloc(totaltoread*CD_FRAMEWORDS,1);
    
  }else{
    readat=p->cursor; 
  }
  
  readat+=driftcomp;
  
  if(p->enable&(PARANOIA_MODE_OVERLAP|PARANOIA_MODE_VERIFY)){
    new=new_c_block(p);
    recover_cache(p);
    recover_fragments(p);
  }else{
    /* in the case of root it's just the buffer */
    paranoia_resetall(p);	
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

	if(thisread<=0){

	  /* Uhhh... right.  Make something up. */
	  /* This could happen due to an inaccessible sector or 
	     TAO runin-runout. */
	  (*callback)(adjread*CD_FRAMEWORDS,PARANOIA_CB_READERR);
	  
	  memset(buffer+sofar*CD_FRAMEWORDS,0,CD_FRAMESIZE_RAW);
	  if(flags)memset(flags+sofar*CD_FRAMEWORDS,2,CD_FRAMEWORDS);
	  secread=1;
	  if(lastgood==0)lastgood= -adjread;
	  
	}else{
	  secread=thisread;
	  (*callback)((adjread+secread)*CD_FRAMEWORDS,PARANOIA_CB_READERR);
	}
      }

      if(flags)flags[sofar*CD_FRAMEWORDS]|=1;
      
      p->lastread=adjread+secread;
      
      if(adjread+secread-1==p->current_lastsector){
	if(p->enable&(PARANOIA_MODE_OVERLAP|PARANOIA_MODE_VERIFY))
	  new->lastsector=-1;
	else
	  p->root.done=-1;
	totaltoread=0;
      }
      
      (*callback)((adjread+secread-1)*CD_FRAMEWORDS,PARANOIA_CB_READ);
      
      sofar+=secread;
      readat=adjread+secread; 
    }else
      readat+=sectatonce; 
  }

  if(sofar>0){
    if(p->enable&(PARANOIA_MODE_OVERLAP|PARANOIA_MODE_VERIFY)){
      new->buffer=buffer;
      new->flags=flags;
      new->begin=firstread*CD_FRAMEWORDS-p->dyndrift; 
      new->end=new->begin+sofar*CD_FRAMEWORDS;
      
      if(lastgood==0)lastgood=new->end;

      if(p->enable&PARANOIA_MODE_VERIFY)
	verify_stage1(p,new,callback);
      else{
	/* just make v_fragments from the boundary information. */
	long begin=0,end=0;
	
	while(begin<new->end-new->begin){
	  end=begin+1;
	  while(end<new->end-new->begin && (flags[end]&1)==0)end++;
	  {
	    v_fragment *f=new_v_fragment(p);
	    
	    f->one=new;
	    f->two=new;
	    f->offset=0;
	    f->begin=begin+new->begin;
	    f->end=end+new->begin;
	    if(new->lastsector && f->end==new->end)f->lastsector=1;
	  }
	  begin=end;
	}
      }
      
    }else{
      p->root.buffer=buffer;
      p->root.begin=firstread*CD_FRAMEWORDS-p->dyndrift; 
      p->root.end=p->root.begin+sofar*CD_FRAMEWORDS;
      if(lastgood==0)lastgood=p->root.end;
      verify_end_case(p,endword+
		      (MAX_SECTOR_OVERLAP*CD_FRAMEWORDS),
		      callback);
      
    }
    
  }else{
    if(new)release_c_block(new);
    free(buffer);
    free(flags);
  }
  return(lastgood);
}

/* The returned buffer is *not* to be freed by the caller.  It will
   persist only until the next call to paranoia_read() for this p */

size16 *paranoia_read(cdrom_paranoia *p, void(*callback)(long,int)){

  long beginword=p->cursor*(CD_FRAMEWORDS);
  long endword=beginword+CD_FRAMEWORDS;
  long retry_count=0,lastend=-2;
  long retry_read=0,lastread=-2;
  
  if(beginword>p->root.returnedlimit)p->root.returnedlimit=beginword;
  lastread=lastend=p->root.end;
  
  /* First, is the sector we want already in the root? */
  while(p->root.end==-1 || p->root.begin>beginword || 
	(p->root.end<endword+(MAX_SECTOR_OVERLAP*CD_FRAMEWORDS) &&
	 p->enable&(PARANOIA_MODE_VERIFY|PARANOIA_MODE_OVERLAP)) ||
	p->root.end<endword){
    
    /* Nope; we need to build or extend the root verified range */
    
    if(p->enable&(PARANOIA_MODE_VERIFY|PARANOIA_MODE_OVERLAP)){
      i_paranoia_trim(p,beginword,endword);
      recover_cache(p);
      recover_fragments(p);
      if(p->root.done)
	verify_end_case(p,endword+(MAX_SECTOR_OVERLAP*CD_FRAMEWORDS),
			callback);
      else
	verify_stage2(p,beginword,
		      endword+(MAX_SECTOR_OVERLAP*CD_FRAMEWORDS),
		      callback);
    }else{
      verify_end_case(p,endword+(MAX_SECTOR_OVERLAP*CD_FRAMEWORDS),
		      callback); /* only trips if we're already done */
    }
    
    if(!(p->root.end==-1 || p->root.begin>beginword || 
	 p->root.end<endword+(MAX_SECTOR_OVERLAP*CD_FRAMEWORDS))) 
      break;
    
    /* Hmm, need more.  Read another block */

    {    
      long ret=i_read_c_block(p,beginword,endword,callback);
      
      if(ret>0){
	/* got it all */
	if(ret>lastread)
	  lastread=ret;
	retry_read=0;
      }else{
	ret= -ret;
	if(ret>lastread){
	  lastread=ret;
	  retry_read=0;
	}else
	  retry_read++;
      }
    }

    /* Are we doing lots of retries?  **************************************/
    
    /* Check unaddressable sectors first.  There's no backoff here; 
       jiggle and minimum backseek handle that for us */
    
    if(lastend<p->root.end){
      lastend=p->root.end;
      retry_count=0;
    }else{
      /* increase overlap or bail */
      retry_count++;
      
      if(retry_count>=5){
	if(retry_read>=5 || retry_count>20){
	  
	  verify_skip_case(p,callback);
	  retry_count=0;
	}else{
	  
	  p->dynoverlap*=2;
	  if(p->dynoverlap>MAX_SECTOR_OVERLAP*CD_FRAMEWORDS)
	    p->dynoverlap=MAX_SECTOR_OVERLAP*CD_FRAMEWORDS;
	  (*callback)(p->dynoverlap,PARANOIA_CB_OVERLAP);
	  
	  
	  /*	  if(retry_count%5==0){
	    verify_backoff(p,callback);
	  }*/
	}
      }
    }
  }
  p->cursor++;

  return(p->root.buffer+(beginword-p->root.begin));
}

#ifdef TEST
/****************  Testing ********************/

static void sft_setup(size16 *master,
		      root_block *a,v_fragment *b,c_block *c,
		      long begins,long s){
  
  a->buffer=realloc(a->buffer,(s+2)*CD_FRAMESIZE_RAW);

  memcpy(a->buffer,master,s*CD_FRAMESIZE_RAW);
  memcpy(c->buffer,master,s*CD_FRAMESIZE_RAW);
  c->begin=b->begin=a->begin=begins*CD_FRAMEWORDS;
  c->end=b->end=(a->end=(begins+s)*CD_FRAMEWORDS);

}

static void sft_shift(v_fragment *b, c_block *c,long s){
  c->begin=b->begin+=s;
  c->end=b->end+=s;
}

static void sft_verify(size16 *master,
		      root_block *a,v_fragment *b,c_block *c,
		      long shift,char *prompt){

  /* make sure master and a are perfect matches, b/c is a perfect match for
     the existing overlap */
  long count,count2;
  long size=a->end-a->begin;
  long size2=b->end-b->begin;
  size16 *Abuf=a->buffer;
  size16 *Bbuf=v_buffer(b);

  for(count=0;count<size;count++){
    if(Abuf[count]!=master[count]){
      printf("%s master/A buffer mismatch at position %ld\n",prompt,count);
      exit(0);
    }
  }
}

/* not stream position, buffer position */
static void sft_v_remove(v_fragment *b,c_block *c,long start,long leng){
  long size=c->end-c->begin;
  memmove(c->buffer+start,c->buffer+start+leng,(size-start-leng)*2);
  c->end-=leng;
  b->end-=leng;
}

/* not stream position, buffer position */
static void sft_v_dup(v_fragment *b,c_block *c,long start,long leng){
  long size=c->end-c->begin;
  
  /* already alloced extra space */
  memmove(c->buffer+start+leng,c->buffer+start,(size-start)*2);
  c->end+=leng;
  b->end+=leng;
}

static void sft_r_remove(root_block *a,long start,long leng){
  long size=a->end-a->begin;
  memmove(a->buffer+start,a->buffer+start+leng,(size-start-leng)*2);
  a->end-=leng;
}

/* not stream position, buffer position */
static void sft_r_dup(root_block *a,long start,long leng){
  long size=a->end-a->begin;
  
  memmove(a->buffer+start+leng,a->buffer+start,(size-start)*2);
  a->end+=leng;
}

void dummy(long foo,int bar){
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
  
  a.buffer=malloc((s+2)*CD_FRAMESIZE_RAW);
  b.one=&c;
  b.two=NULL;
  b.stamp=1;
  b.p=p;
  c.p=p;
  c.stamp=1;
  c.buffer=malloc((s+2)*CD_FRAMESIZE_RAW);
  a.returnedlimit=0;
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
  ret=i_sync_fragment(&a,&b,-1,&dummy);
  sft_verify(master,&a,&b,&c,0,"no resync");

  sft_setup(master,&a,&b,&c,postsec,s);
  sft_shift(&b,&c,16);
  sft_v_remove(&b,&c,0,20);
  ret=i_sync_fragment(&a,&b,-1,&dummy);
  sft_verify(master,&a,&b,&c,-20,"shift/jitter");


  sft_setup(master,&a,&b,&c,postsec,s);
  sft_shift(&b,&c,16);
  sft_v_remove(&b,&c,0,200);
  sft_v_remove(&b,&c,70,2);
  sft_v_remove(&b,&c,102,8);
  sft_v_remove(&b,&c,800,16);
  ret=i_sync_fragment(&a,&b,-1,&dummy);
  sft_verify(master,&a,&b,&c,-200,"v dropped back");

  sft_setup(master,&a,&b,&c,postsec,s);
  sft_shift(&b,&c,16);
  sft_v_dup(&b,&c,0,200);
  sft_v_dup(&b,&c,420,2);
  sft_v_dup(&b,&c,1102,8);
  sft_v_dup(&b,&c,1800,16);
  ret=i_sync_fragment(&a,&b,-1,&dummy);
  sft_verify(master,&a,&b,&c,-200,"v duped back");

  sft_setup(master,&a,&b,&c,postsec,s);
  sft_shift(&b,&c,16);
  sft_v_remove(&b,&c,0,200);
  sft_v_dup(&b,&c,70,2);
  sft_v_remove(&b,&c,102,8);
  sft_v_dup(&b,&c,800,16);
  ret=i_sync_fragment(&a,&b,-1,&dummy);
  sft_verify(master,&a,&b,&c,-200,"v both back");

  sft_setup(master,&a,&b,&c,postsec,s);
  sft_shift(&b,&c,16);
  sft_v_remove(&b,&c,0,200);
  sft_r_remove(&a,270,2);
  sft_r_remove(&a,302,8);
  sft_r_remove(&a,900,16);
  ret=i_sync_fragment(&a,&b,-1,&dummy);
  sft_verify(master,&a,&b,&c,-200,"root dropped back");

  sft_setup(master,&a,&b,&c,postsec,s);
  sft_shift(&b,&c,16);
  sft_v_remove(&b,&c,0,200);
  sft_r_dup(&a,420,2);
  sft_r_dup(&a,1102,8);
  sft_r_dup(&a,1800,16);
  ret=i_sync_fragment(&a,&b,-1,&dummy);
  sft_verify(master,&a,&b,&c,-200,"root duped back");

  sft_setup(master,&a,&b,&c,postsec,s);
  sft_shift(&b,&c,16);
  sft_v_remove(&b,&c,0,200);
  sft_r_dup(&a,370,2);
  sft_r_remove(&a,502,8);
  sft_r_dup(&a,800,16);
  ret=i_sync_fragment(&a,&b,-1,&dummy);
  sft_verify(master,&a,&b,&c,-200,"root both back");

  /******************/

  sft_setup(master,&a,&b,&c,postsec,s);
  sft_shift(&b,&c,16);
  sft_v_remove(&b,&c,0,100);
  sft_v_remove(&b,&c,70,2);
  sft_v_remove(&b,&c,102,8);
  sft_v_remove(&b,&c,800,16);
  sft_v_remove(&b,&c,1000,16);
  ret=i_sync_fragment(&a,&b,300+a.begin,&dummy);
  sft_verify(master,&a,&b,&c,-100,"v dropped both");

  sft_setup(master,&a,&b,&c,postsec,s);
  sft_shift(&b,&c,16);
  sft_v_dup(&b,&c,50,150);
  sft_v_dup(&b,&c,420,2);
  sft_v_dup(&b,&c,1102,8);
  sft_v_dup(&b,&c,1800,16);
  ret=i_sync_fragment(&a,&b,400+a.begin,&dummy);
  sft_verify(master,&a,&b,&c,-200,"v duped both");

  sft_setup(master,&a,&b,&c,postsec,s);
  sft_shift(&b,&c,16);
  sft_v_remove(&b,&c,8,192);
  sft_v_dup(&b,&c,70,2);
  sft_v_remove(&b,&c,102,8);
  sft_v_dup(&b,&c,800,16);
  ret=i_sync_fragment(&a,&b,400+a.begin,&dummy);
  sft_verify(master,&a,&b,&c,-200,"v both both");

  sft_setup(master,&a,&b,&c,postsec,s);
  sft_shift(&b,&c,16);
  sft_v_remove(&b,&c,0,200);
  sft_r_remove(&a,270,2);
  sft_r_remove(&a,302,8);
  sft_r_remove(&a,900,16);
  ret=i_sync_fragment(&a,&b,400+a.begin,&dummy);
  sft_verify(master,&a,&b,&c,-200,"root dropped both");

  sft_setup(master,&a,&b,&c,postsec,s);
  sft_shift(&b,&c,16);
  sft_v_remove(&b,&c,0,200);
  sft_r_dup(&a,420,2);
  sft_r_dup(&a,1102,8);
  sft_r_dup(&a,1800,16);
  ret=i_sync_fragment(&a,&b,400+a.begin,&dummy);
  sft_verify(master,&a,&b,&c,-200,"root duped both");

  sft_setup(master,&a,&b,&c,postsec,s);
  sft_shift(&b,&c,16);
  sft_v_remove(&b,&c,0,200);
  sft_r_dup(&a,370,2);
  sft_r_remove(&a,502,8);
  sft_r_dup(&a,800,16);
  a.end-=400;
  ret=i_sync_fragment(&a,&b,400+a.begin,&dummy);
  sft_verify(master,&a,&b,&c,-200,"root both both + glom");


  exit(0);
}

#endif


