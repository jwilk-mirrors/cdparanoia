/***
 * CopyPolicy: GNU Public License 2 applies
 * Copyright (C) by Monty (xiphmont@mit.edu)
 *
 * Toplevel file for the paranoia abstraction over the cdda lib 
 *
 ***/

/* scratch detection/tolerance not implemented yet */
/* Skip correctly; don't replace the whole sector */

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include "../interface/cdda_interface.h"
#include "cdda_paranoia.h"
#include "p_block.h"

#define MIN_WORDS_OVERLAP    64     /* 16 bit words */
#define MIN_WORDS_RIFT       64     /* 16 bit words */
#define MAX_SECTOR_BACKCACHE 15      /* sectors */
#define MAX_SECTOR_PRECACHE  15      /* sectors */
#define MAX_SECTOR_OVERLAP   15      /* sectors */

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

void paranoia_resetcache(cdrom_paranoia *p){
  p_block *next,*current=p->fragments;
  p->skiplimit=0;
  release_p_block(&(p->root));
  while(current){
    next=current->next;
    release_p_block(current);
    current=next;
  }
  p->root.done=0;
  p->root.lastsector=0;
}

static void i_dump_chains(cdrom_paranoia *p){
  p_block *b;
  int i=0;

  printf("\n    ROOT:  %7ld %7ld -- %7ld %7ld  off %ld s %ld l %ld d %ld\n",
	 p->root.begin,p->root.verifybegin,p->root.verifyend,p->root.end,
	 p->root.offset,p->root.silence,p->root.lastsector,p->root.done);
  
  /*  {
    long i;
    for(i=p->root.verifybegin;i<p->root.verifyend;i+=2)
      printf("%d\n",p->root.buffer[i-p->root.begin]);
  }
  printf("-99999\n");
  printf("\n");*/

  b=p->fragments;

  while(b){
    printf(" link %2d:  %7ld %7ld -- %7ld %7ld  off %ld s %ld l %ld d %ld\n",b->stamp,
	   b->begin,b->verifybegin,b->verifyend,b->end,b->offset,b->silence,
	   b->lastsector,b->done);
  
    /*    {
      long i;
      for(i=b->verifybegin;i<b->verifyend;i+=2)
	printf("%d\n",b->buffer[i-b->begin]);
    }
    printf("-99999\n");

    printf("\n");*/
    b=b->next;
    i++;
  }
}

/**** fragment data manipulation *****************************************/

static void i_paranoia_trim(p_block *b,long beginword, long endword){
  p_block *root=&(b->p->root);
  
  /* If we are too far behind the desired range, free up space */
  if(b==root){
    if(b->begin==-1)return;
    if(b->begin+CD_FRAMEWORDS*MAX_SECTOR_BACKCACHE<beginword){
      long newbegin=beginword-CD_FRAMEWORDS*MAX_SECTOR_BACKCACHE;
      long offset=newbegin-b->begin;
      
      /* copy and realloc (well, realloc may not really be useful) */
      memmove(b->buffer,b->buffer+offset,(b->end-newbegin)*2);
      b->begin=newbegin;
      /*b->buffer=realloc(b->buffer,(b->end-newbegin)*2);
	we'll have to adjust size & total if this is reenabled */
      
      if(b->verifyend!=-1)
	b->verifyend=(b->verifyend>newbegin?b->verifyend:-1);
      if(b->verifyend==-1)
	b->verifybegin=-1;
      else
	b->verifybegin=(b->verifybegin>newbegin?b->verifybegin:newbegin);
      b->silence=-1;
    }
  }else{
    if(b->buffer){
      
      /* if the cache is entirely ahead of the current range, blast it;
	 we seeked back (even if 'seeked' isn't a word)  */
      
      /* or if it's a really old fragment */
      
      if(b->begin>endword+(CD_FRAMEWORDS)*  /* too far ahead? */
	 (b->p->readahead+b->p->d->nsectors) || 
	 b->end<beginword-(CD_FRAMEWORDS)*b->p->dynoverlap ||
	 b->end<root->begin){
	
	release_p_block(b);
	
	return;
	
      }
    }
  }
}

/* enlarge (if necessary) a and update verified areas from b */

/* Now this is an odd problem below; if we create new rifts by blindly
   copying around verified areas, the new rifts (where the verified
   areas don't match the preceeding or following unverified areas)
   have an uncanny knack for matching up later despite being jittered.
   So we do this:

                     1          |  2
A:           |--------|=========|-----|
B:                |+++++|=======|==|++++++++|
                     3          |  4
becomes                 
                     1          |  4
A:           |--------|=========|===|++++++++|
B:                |+++++|=======|-----|    (or just free it)
                     3          |  2

clever, eh? :-) Do only if we're extending a's verification area forward. */

static void i_update_verified(p_block *a,p_block *b){
  long divpoint=a->verifyend;

  if(divpoint<b->begin || divpoint>=b->end)
    return;
  else{
    long size1=divpoint-a->begin;
    long size2=a->end-divpoint;
    long size3=divpoint-b->begin;
    long size4=b->end-divpoint;
    
    long newsizeA=size1+size4;
    size16 *bufferA=malloc(newsizeA*2);
    
    long newsizeB=size3+size2;
    size16 *bufferB=malloc(newsizeB*2);
    
    long endA,endB,doneA,doneB,lastA,lastB;
    long verifyendA,verifyendB;
    long sofarA=0,sofarB=0;
    
    memmove(bufferA,a->buffer,size1*2);
    memmove(bufferB,b->buffer,size3*2);
    sofarA+=size1;
    sofarB+=size3;
    
    endB=a->end;
    endA=b->end;
    
    if(b->verifyend!=-1 && divpoint<=b->end){
      verifyendB=a->verifyend;
      verifyendA=b->verifyend;
    }else{
      verifyendB=-1;
      verifyendA=divpoint;
    }
    doneA=b->done;
    doneB=a->done;
    lastA=b->lastsector;
    lastB=a->lastsector;
    
    memmove(bufferB+sofarB,a->buffer+divpoint-a->begin,size2*2);
    memmove(bufferA+sofarA,b->buffer+divpoint-b->begin,size4*2);
    
    p_buffer(a,bufferA,newsizeA*2);
    a->end=endA;
    a->verifyend=verifyendA;
    a->done=doneA;
    a->lastsector=lastA;
    a->silence=-1;
    
    p_buffer(b,bufferB,newsizeB*2);
    b->end=endB;
    b->verifyend=verifyendB;
    b->done=doneB;
    b->lastsector=lastB;
    b->silence=-1;
  }
}

static void three_way_split(p_block *a,long riftr,long riftf){
  cdrom_paranoia *p=a->p;
  p_block *pre=(riftr>-1?new_p_block(p):NULL);
  p_block *post=(riftf>-1?new_p_block(p):NULL);

  if(!a->buffer){ /* did grabbing new blocks trigger a reap? */
    if(pre)release_p_block(pre);
    if(pre)release_p_block(post);
    return;
  }

  if(pre){
    long size=(riftr-a->begin)*2;
    p_buffer(pre,malloc(size),size);
    memmove(pre->buffer,a->buffer,size);
    pre->begin=a->begin;
    pre->end=riftr;
    pre->offset=a->offset;
  }

  if(post){
    long size=(a->end-riftf)*2;
    p_buffer(post,malloc(size),size);
    memmove(post->buffer,a->buffer+riftf-a->begin,size);
    post->begin=riftf;
    post->end=a->end;
    post->offset=a->offset;
    post->lastsector=a->lastsector;
    post->done=a->done;
    a->lastsector=0;
    a->done=0;
  }

  if(pre || post){
    if(!pre)riftr=a->begin;
    if(!post)riftf=a->end;
    {
      long size=(riftf-riftr)*2;
      size16 *buffer=malloc(size);
      memmove(buffer,a->buffer+riftr-a->begin,size);
      p_buffer(a,buffer,size);
      a->begin=riftr;
      a->end=riftf;
      
      if(a->verifybegin!=-1 && a->verifybegin<riftr)a->verifybegin=riftr;
      if(a->verifyend!=-1 && a->verifyend>riftf)a->verifyend=riftf;
    }
  }

  if(a==&(a->p->root) && pre)
    swap_p_block(pre,a);

}

static void i_unverify_fragments(cdrom_paranoia *p){
  p_block *a=p->fragments;

  while(a){
    a->verifybegin=-1;
    a->verifyend=-1;
    a->silence=-1;
    a->done=0;
    a=a->next;
  }
}

/**** Statistical and heuristic[al? :-] management ************************/

static void offset_adjust_settings(cdrom_paranoia *p, 
				   void(*callback)(long,int)){
  {
    /* drift: look at the average offset value.  If it's over one
       sector, frob it.  We just want a little hysteresis [sp?] */
    long av=(p->offpoints?p->offaccum/p->offpoints:0);
    
    if(abs(av)>CD_FRAMEWORDS){
      (*callback)(p->root.verifyend,PARANOIA_CB_DRIFT);
      p->dyndrift+=av;
      
      /* adjust other statistics to be consistent with new drift val */
      p->offaccum-=(av*p->offpoints);
      p->offmin-=av;
      p->offmax-=av;
    }
  }

  {
    /* dynoverlap: we arbitrarily set it to 4x the running difference
       value, unless mix/max are more */

    p->dynoverlap=(p->offpoints?p->offdiff/p->offpoints:CD_FRAMEWORDS)/
      (CD_FRAMESIZE_RAW/2)*4;

    if((p->dynoverlap+1)*CD_FRAMEWORDS<-p->offmin)
      p->dynoverlap=-p->offmin/CD_FRAMEWORDS+1;
						     
    if((p->dynoverlap+1)*CD_FRAMEWORDS<p->offmax)
      p->dynoverlap=p->offmax/CD_FRAMEWORDS+1;

    if(p->dynoverlap<1)p->dynoverlap=1;
    if(p->dynoverlap>MAX_SECTOR_OVERLAP)p->dynoverlap=MAX_SECTOR_OVERLAP;
    			     
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

  if(p->offpoints>=20){
    offset_adjust_settings(p,callback);
    offset_clear_settings(p);
  }
}

/**** Gap analysis code ***************************************************/

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

static long i_paranoia_overlap_r(p_block *a,p_block *b,
				 long offsetA, long offsetB){
  size16 *buffA=a->buffer;
  size16 *buffB=b->buffer;
  long beginA=offsetA;
  long beginB=offsetB;

  for(;beginA>=0 && beginB>=0;beginA-=2,beginB-=2)
    if(buffA[beginA]!=buffB[beginB] ||
       buffA[beginA+1]!=buffB[beginB+1])break;
  beginA+=2;
  beginB+=2;
  
  return(offsetA-beginA);
}

static long i_paranoia_overlap_f(p_block *a,p_block *b,
				long offsetA, long offsetB){
  size16 *buffA=a->buffer;
  size16 *buffB=b->buffer;
  long sizeA=a->end-a->begin;
  long sizeB=b->end-b->begin;
  long endA=offsetA;
  long endB=offsetB;

  for(;endA+1<sizeA && endB+1<sizeB;endA+=2,endB+=2)
    if(buffA[endA]!=buffB[endB] ||
       buffA[endA+1]!=buffB[endB+1])break;
  
  return(endA-offsetA);
}

static int i_stutter_or_gap(p_block *a, p_block *b, long offA, long offB,
			    long gap){
  long a1=offA;
  long b1=offB;
 
  if(a1<0){
    b1-=a1;
    gap+=a1;
    a1=0;
  }

  return(memcmp(a->buffer+a1,b->buffer+b1,gap*2));
}

/* riftv is the first value into the rift -> or <- */
static void i_analyze_rift_f(p_block *a,p_block *b,long riftv,
			     long *matchA,long *matchB,long *matchC){

  long apast=a->end-riftv;
  long bpast=b->end-riftv;
  long aoffset=riftv-a->begin;
  long boffset=riftv-b->begin;
  long i;
  long limit=a->p->dynoverlap*(CD_FRAMEWORDS);
  
  *matchA=0, *matchB=0, *matchC=0;

  /* Look for three possible matches... (A) Ariftv->B, (B) Briftv->A and 
     (c) AB->AB. */
  
  for(i=0;i<limit;i+=2){
    if(i<bpast) /* A */
      if(i_paranoia_overlap_f(a,b,aoffset,boffset+i)>=MIN_WORDS_RIFT){
	*matchA=i;
	break;
      }

    if(i<apast){ /* B */
      if(i_paranoia_overlap_f(a,b,aoffset+i,boffset)>=MIN_WORDS_RIFT){
	*matchB=i;
	break;
      }
      if(i<bpast) /* C */
	if(i_paranoia_overlap_f(a,b,aoffset+i,boffset+i)>=MIN_WORDS_OVERLAP){
	  *matchC=i;
	  break;
	}
    }else
      if(i>=bpast)break;
    
  }
  
  if(*matchA==0 && *matchB==0 && *matchC==0)return;
  
  if(*matchC)return;
  if(*matchA){
    if(i_stutter_or_gap(a,b,aoffset-*matchA,boffset,*matchA))
      return;
    *matchB=-*matchA; /* signify we need to remove n bytes from B */
    *matchA=0;
    return;
  }else{
    if(i_stutter_or_gap(b,a,boffset-*matchB,aoffset,*matchB))
      return;
    *matchA=-*matchB;
    *matchB=0;
    return;
  }
}

/* riftv must be first even val of rift moving back */

static void i_analyze_rift_r(p_block *a,p_block *b,long riftv,
			   long *matchA,long *matchB,long *matchC){

  long apast=riftv-a->begin+2;
  long bpast=riftv-b->begin+2;
  long aoffset=riftv-a->begin;
  long boffset=riftv-b->begin;
  long i;
  long limit=a->p->dynoverlap*(CD_FRAMEWORDS);
  
  *matchA=0, *matchB=0, *matchC=0;

  /* Look for three possible matches... (A) Ariftv->B, (B) Briftv->A and 
     (c) AB->AB. */
  
  for(i=0;i<limit;i+=2){
    if(i<bpast) /* A */
      if(i_paranoia_overlap_r(a,b,aoffset,boffset-i)>=MIN_WORDS_RIFT){
	*matchA=i;
	break;
      }
    if(i<apast){ /* B */
      if(i_paranoia_overlap_r(a,b,aoffset-i,boffset)>=MIN_WORDS_RIFT){
	*matchB=i;
	break;
      }      
      if(i<bpast) /* C */
	if(i_paranoia_overlap_r(a,b,aoffset-i,boffset-i)>=MIN_WORDS_OVERLAP){
	  *matchC=i;
	  break;
	}
    }else
      if(i>=bpast)break;
    
  }
  
  if(*matchA==0 && *matchB==0 && *matchC==0)return;
  
  if(*matchC)return;

  if(*matchA){
    if(i_stutter_or_gap(a,b,aoffset+2,boffset-*matchA+2,*matchA))
      return;
    *matchB=-*matchA; /* signify we need to remove n bytes from B */
    *matchA=0;
    return;
  }else{
    if(i_stutter_or_gap(b,a,boffset+2,aoffset-*matchB+2,*matchB))
      return;
    *matchA=-*matchB;
    *matchB=0;
    return;
  }
}

/* returns nonzero if 'a' fragmented */ 
static int i_overlap_analyze(p_block *a,p_block *b,long firstword,
			     long endword,void(*callback)(long,int)){

  /* Assume we are synced already. Look for verification overlap, push
     forward, back */
    
  if(a->verifyend==-1 || b->verifyend==-1)return(0);

  {
    long beginvo=(a->verifybegin>b->verifybegin?a->verifybegin:
		  b->verifybegin);
    long endvo=(a->verifyend<b->verifyend?a->verifyend:
		b->verifyend);
    long post=endvo-2;
    long begin,end;
    long rmatchA=0,rmatchB=0,rmatchC=0;
    long fmatchA=0,fmatchB=0,fmatchC=0;
    long postA=post-a->begin;
    long postB=post-b->begin;

    if(beginvo>=endvo)return(0); 
    
    if(i_paranoia_overlap(a,b,postA,postB,&begin,&end)>MIN_WORDS_OVERLAP){
      
      /* is the backward check all the way to the wall? */
      if(!(begin==a->begin || begin==b->begin) && a->p->root.silence!=2){
	/* nope.  There's a rift. */
	
	(*callback)(post,PARANOIA_CB_FIXUP_ATOM);
	i_analyze_rift_r(a,b,begin-2,&rmatchA,&rmatchB,&rmatchC);

	/* we don't split root if it's a value that has already been
	   returned to the application */

	if(rmatchA && begin<a->p->skiplimit)rmatchA=0;
      }

      /* is the forward check all the way to the wall? */
      if(!(end==a->end || end==b->end) && a->p->root.silence!=2){
	/* nope.  There's a rift. */
	
	i_analyze_rift_f(a,b,end,&fmatchA,&fmatchB,&fmatchC);
	(*callback)(post,PARANOIA_CB_FIXUP_ATOM);

	/* we don't split root if it's a value that has already been
	   returned to the application */

	if(fmatchA && end<a->p->skiplimit)fmatchA=0;
      }

      /* does end reach all the way to the end of a fragment which
         sets lastsector? */
      if((end==a->end && a->lastsector) || 
	 (end==b->end && b->lastsector)){
	a->done=1;
	b->done=1;
      }

      three_way_split(a,(rmatchA?begin:-1),(fmatchA?end:-1));
      if(a->buffer && a->verifybegin!=-1 /* could have been a root swap. 
					    RTSL */){
	if(a->verifybegin>begin)a->verifybegin=begin;
	if(a->verifyend<end){
	  a->verifyend=end;
	  a->silence=-1;
	}
      }

      if(b->buffer)
	three_way_split(b,(rmatchB?begin:-1),(fmatchB?end:-1));
      
      if(b->buffer){
	if(b->verifybegin>begin)b->verifybegin=begin;
	if(b->verifyend<end){
	  b->verifyend=end;
	  b->silence=-1;
	}
      }
      if(rmatchA || fmatchA)return(1);
    }
  } 
  return(0);
}

static long i_paranoia_sync(p_block *a,p_block *b,long *post,long *begin,
			    long *end,void (*callback)(long,int)){
  int attempts,i;
  long overlap=a->p->dynoverlap*(CD_FRAMEWORDS);
  
  if(begin)*begin=-1;
  if(end)*end=-1;
  if(a->end+overlap<b->begin)goto sync_failure;
  if(b->end+overlap<a->begin)goto sync_failure;
  if(*post+a->p->dynoverlap*(CD_FRAMEWORDS)<a->begin)goto sync_failure;
  if(*post-a->p->dynoverlap*(CD_FRAMEWORDS)>a->end)goto sync_failure;
  if(*post+a->p->dynoverlap*(CD_FRAMEWORDS)<b->begin)goto sync_failure;
  if(*post-a->p->dynoverlap*(CD_FRAMEWORDS)>b->end)goto sync_failure;
  
  for(attempts=0;
      *post>=a->begin && attempts<3;
      *post-=MIN_WORDS_OVERLAP,attempts++){
    
    long count;
    long best=0;
    long offset=0;
    size16 *buffA=a->buffer;
    size16 *buffB=b->buffer;
    long sizeA=a->end-a->begin;
    long sizeB=b->end-b->begin;
    long postA=*post-a->begin;
    long initB=*post-b->begin;
    long adjB=0;
    
    long ret;
    long limit=overlap;
    
    if(postA<0 || postA>=sizeA)goto sync_failure; /* Can't post if out 
						     of A's range */
    
    /* If post is out of B's range, we just have a single reconciliation
       case is all. */
    
    if(initB<MIN_WORDS_OVERLAP){
      adjB=-initB;
      initB=0;
    }

    if(initB>=sizeB){
      adjB=(sizeB-2)-initB;
      initB=sizeB-2;
    }
    
    /* Is the area of A we're posting from silence? */
    if(a->silence==-1){
      long count=0,zeroes=0;
      for(i=postA;i>1 && count<MIN_WORDS_OVERLAP;i--,count++)
	if(buffA[i]==buffA[i-2])zeroes++;

      a->silence=0;
      if(zeroes>count/2)a->silence=1;
      if(zeroes==count)a->silence=2;
    }

    if(a->silence==2){
      /* An optimization to speed things up a bit */
      
      for(;postA<sizeA-3 && initB<sizeB-3; postA+=2, initB+=2)
	if(buffA[postA]!=buffA[postA+2] ||
	   buffA[postA+1]!=buffA[postA+3])
	  break;

      if(postA>=sizeA-3 || initB>=sizeB-3){
	/* complete silence, both halves.  Believe the current alignment */

	long begino=(a->begin>b->begin?a->begin:
		      b->begin);
	long endo=(a->end<b->end?a->end:
		    b->end);

	if(begino>endo){
	  *post=-1;
	  return(0);
	}

	if(i_paranoia_overlap(a,b,postA,initB,begin,end)>MIN_WORDS_OVERLAP)
	  return(adjB);

	*post=-1;
	return(0);
      }

      postA+=2; /* gotta have a nonzero 'A' */
      initB+=2;
    }
    
    /* Search the whole range, spreading from initB */
    /* This wastes 2*MIN_WORDS_OVERLAP matches right now :-( */
    for(count=0;count<limit;count+=2){
      int flag=0;

      if(initB+count<sizeB){ /* OK, in range */
	if(buffA[postA]==buffB[initB+count])
	  if((ret=i_paranoia_overlap(a,b,postA,initB+count,NULL,NULL))>best){
	    best=ret;
	    offset=count;
	    if(best>=MIN_WORDS_OVERLAP && a->silence==0)break;
	  }
      }else
	flag=1;
      
      if(count>0 && initB-count>=0){ /* OK, in range */
	if(buffA[postA]==buffB[initB-count])
	  if((ret=i_paranoia_overlap(a,b,postA,initB-count,NULL,NULL))>best){
	    best=ret;
	    offset= -count;
	    if(best>=MIN_WORDS_OVERLAP && a->silence==0)break;
	  }
      }else
	if(flag)break;
    }
    
    if(best>=MIN_WORDS_OVERLAP){
      i_paranoia_overlap(a,b,postA,initB+offset,begin,end);
      offset+=adjB;

      return(offset);
      /*      b->begin-=offset;
	      b->end-=offset;*/
    }
  }
  
sync_failure:
  *post=-1;
  return(0);
}

static void i_paranoia_glom(p_block *a,p_block *b){

  if(a->verifybegin!=-1 && b->verifybegin!=-1){

    /* we assume sync */

    long beginoverlap=(a->verifybegin>b->verifybegin?
		       a->verifybegin:b->verifybegin);
    long endoverlap=(a->verifyend<b->verifyend?a->verifyend:b->verifyend);
    
    long overlap=endoverlap-beginoverlap;

    if(overlap>MIN_WORDS_OVERLAP)
      /* glom */
	i_update_verified(a,b);
  }
}

static void verify_init_case(cdrom_paranoia *p,long beginword,
			void(*callback)(long,int)){

  /* We find sync between fragments and look for matching range, but
     we do *not* move their offsets, just set verify range. */
  /* make the first proper range root */

  p_block *a=p->fragments;

  while(a){
    p_block *b=p->fragments;
    p_block *next=a->next;
    while(b){
      p_block *next=b->next;
      if(a!=b){ /* sync is not symmetric */
	long post=beginword;
	long offset=i_paranoia_sync(a,b,&post,NULL,NULL,callback);
	p_block *ptr;
	p_block *root=&(p->root);

	if(offset>0)
	  ptr=a;
	else
	  ptr=b;
	
	if(post==beginword){
	  /* We have a winner! */

	  if(root->verifyend==-1){
	    /* No data yet */
	    release_p_block(&(p->root)); /* just in case */
	    root->verifybegin=-1;
	    root->verifyend=-1;
	    
	    /* Use most tolerant */
	    root->buffer=ptr->buffer;
	    root->size=ptr->size;
	    root->begin=ptr->begin;
	    root->end=ptr->end;
	    root->lastsector=ptr->lastsector;
	    root->done=ptr->done;
	    ptr->buffer=NULL;
	    ptr->size=0;
	    release_p_block(ptr);
	    return;
	  }else{
	    /* Ah, we're tacking data onto silence */
	    ptr->verifyend=-1;
	    ptr->verifybegin=-1;
	    i_update_verified(root,ptr);
	    release_p_block(ptr);
	    return;
	  }
	}
      }
      b=next;
    }
    a=next;
  }
}

/* This is currently pretty conservative (read: slow) */

static void verify_main_case(cdrom_paranoia *p,long firstword,long endword,
			     void(*callback)(long,int)){
  int fragmentflag=1;
  int prev=-1;

  while(fragmentflag || prev!=p->root.verifyend){
    p_block *current;
    fragmentflag=0;
    prev=p->root.verifyend;

    /* re-sync each fragment.  Most will probably be pre-sunk :-) */
    if(p->root.verifyend==-1)i_unverify_fragments(p);
    current=p->tail;

    /* resyncing will continue until there is no fragmentation of root */

    while(current){
      p_block *next=current->prev;
      long post=(p->root.verifyend<2?firstword:p->root.verifyend-2);
      
      i_paranoia_trim(current,post,endword);
      if(current->buffer)
	if(current->verifyend==-1 || current->verifyend<post){
	  long begin,end;
	  long offset=i_paranoia_sync(&(p->root),current,&post,&begin,&end,
				      callback);
	  if(post==-1){
	    /* clear the verify flags */
	    current->verifybegin=-1;
	    current->verifyend=-1;
	    current->silence=-1;
	  }else{
	    if(offset)
	      (*callback)(post,PARANOIA_CB_FIXUP_EDGE);
	    
	    current->begin-=offset;
	    current->end-=offset;
	    if(current->verifybegin!=-1)current->verifybegin-=offset;
	    if(current->verifyend!=-1)current->verifyend-=offset;

	    current->offset-=offset;
	    offset_add_value(p,current->offset,callback);

	    if((p->enable&PARANOIA_MODE_VERIFY)==0){
	      p->root.verifybegin=p->root.begin;
	      p->root.verifyend=p->root.end;
	      current->verifybegin=current->begin;
	      current->verifyend=current->end;
	      
	      if(p->root.lastsector)p->root.done=1;
	      if(current->lastsector)current->done=1;
	      
	    }else{
	      
	      /* If we're just starting out... */
	      if(p->root.verifybegin==-1){
		p->root.verifybegin=begin;
		p->root.verifyend=end;
		p->root.silence=-1;
	      }
	      if(current->verifybegin==-1){
		current->verifybegin=begin;
		current->verifyend=end;
		current->silence=-1;
	      }
	      
	      if(i_overlap_analyze(&(p->root),current,firstword,endword,callback)){
		fragmentflag=1;
		i_unverify_fragments(p);
	      }
	    }
	  }
	}
      current=next;
    }
    
    if(!fragmentflag){
      if(p->enable&PARANOIA_MODE_VERIFY){
	current=p->tail;
	
	while(current){
	  p_block *compare=current->prev;
	  p_block *next=current->prev;
	  while(compare){ /* compare here *is* symmetric */
	    p_block *next=compare->next;
	    if(current!=compare)
	      if(current->begin!=current->verifybegin ||
		 current->end!=current->verifyend)
		i_overlap_analyze(current,compare,firstword,endword,callback);
	    compare=next;
	  }
	  current=next;
	}
      }

      /* glom */
      
      {
	p_block *largest=NULL;
	long bestend=0;
	current=p->fragments;
      
	while(current){
	  p_block *next=current->next;
	  if(current->begin!=-1 && current->verifybegin!=-1 &&
	     current->verifybegin+MIN_WORDS_OVERLAP<p->root.verifyend &&
	     current->verifyend>bestend &&
	     current->verifyend>p->root.verifyend){
	    bestend=current->verifyend;
	    largest=current;
	  }
	  current=next;
	}
	
	if(largest)
	  i_paranoia_glom(&(p->root),largest);	  
	
      }
    }
  }
  {
    p_block *a=p->fragments;
    while(a){
      p_block *next=a->next;
      i_paranoia_trim(a,firstword,endword);
      a=next;
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
  if(endword<p->root.verifyend)return;
  
  {
    long addto=(endword-p->root.verifyend)*2;
    long newsize=(endword-p->root.begin)*2;
    
    p->root.buffer=realloc(p->root.buffer,newsize);
    p->total_bufsize=newsize-p->root.size;
    p->root.size=newsize;
    memset(p->root.buffer+p->root.verifyend-p->root.begin,0,addto);
    p->root.end=p->root.verifyend=endword;

    /* trash all fragments */
    {
      p_block *a=p->fragments;
      while(a){
	p_block *next=a->next;
	release_p_block(a);
	a=next;
      }
    }
  }
}

static void verify_skip_case(cdrom_paranoia *p,void(*callback)(long,int)){
  /* force a skip.  Grab the first in range block */

  long post=p->root.verifyend;
  long min=CD_FRAMESIZE_RAW/4;

  (*callback)(post,PARANOIA_CB_SKIP);
  offset_clear_settings(p);

  /* Add half a block to it. */

  if(post+min<=p->root.end){
    p->root.verifyend=post+min;
    p->skiplimit=post+min;
    p->root.silence=-1;
    if(p->root.verifyend==p->root.end && p->root.lastsector)p->root.done=-1;
  }else{

    p_block *a=&(p->root);
    p_block *b=p->fragments;
    
    while(b){
      if(b->begin<=post && post+min<=b->end){
	/* Good enough.  Go */
	
	b->verifybegin=a->verifybegin;
	b->verifyend=post+min;

	i_update_verified(a,b);
	p->skiplimit=post+min;
	release_p_block(b);
	return;
      }
      b=b->next;
    }
  }
}    

/* if we have ourselves in an odd tight spot */

static void verify_backoff(cdrom_paranoia *p,void(*callback)(long,int)){
  /* Kill any member of the cache that is synced but incompletely 
     reconciled */

  p_block *current=p->fragments;
  p_block *root=&(p->root);

  while(current){
    p_block *next=current->next;
    if(current->verifybegin!=-1)
      if(current->begin!=current->verifybegin ||
		 current->end!=current->verifyend)
	release_p_block(current);
    current=next;
  }

  /* back off root and trim it */
  if(root->verifyend-CD_FRAMEWORDS>p->skiplimit && 
     root->verifyend-CD_FRAMEWORDS>root->verifybegin){
    root->end=root->verifyend-=CD_FRAMEWORDS/3*2; /* not whole frames */
  }else
    verify_skip_case(p,callback);

}

static void paranoia_verifyloop(cdrom_paranoia *p,long beginword,long endword,
		       void(*callback)(long,int)){

  if(p->root.begin==-1 || p->root.silence==2)
    verify_init_case(p,beginword,callback);
  else
    if(p->root.done)
      verify_end_case(p,endword,callback);
    else
      verify_main_case(p,beginword,endword,callback);

}


/**** initialization and toplevel ****************************************/

cdrom_paranoia *paranoia_init(cdrom_drive *d,int cache,int readahead){
  cdrom_paranoia *p=calloc(1,sizeof(cdrom_paranoia));

  p->skiplimit=0;
  p->root.begin=-1;
  p->root.end=-1;
  p->root.verifybegin=-1;
  p->root.verifyend=-1;
  p->root.silence=-1;
  p->root.p=p;

  p->d=d;
  p->readahead=readahead;
  p->dynoverlap=1;
  if(cache<readahead*CD_FRAMESIZE_RAW)cache=readahead*CD_FRAMESIZE_RAW;
  p->cachemark=cache;
  p->enable=PARANOIA_MODE_FULL;
  p->cursor=cdda_disc_firstsector(d);

  /* One last one... in case data and audio tracks are mixed... */
  i_paranoia_firstlast(p);

  return(p);
}

void paranoia_free(cdrom_paranoia *p){
  int i;
  p_block *b;

  /* release all the pool blocks */
  release_p_block(&(p->root));

  b=p->fragments;
  while(b){
    p_block *next=b->next;
    release_p_block(b);
    b=next;
  }

  b=p->free;
  while(b){
    p_block *next=b->next;
    release_p_block(b);
    b=next;
  }

  for(i=0;i<p->ptrblocks;i++)free(p->ptr[i]);
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
  release_p_block(&(p->root));
  ret=p->cursor;
  p->cursor=sector;
  p->skiplimit=0;

  i_paranoia_firstlast(p);

  return(ret);
}

/* The returned buffer is *not* to be freed by the caller.  It will
   persist only until the next call to paranoia_read() for this p */
size16 *paranoia_read(cdrom_paranoia *p, void(*callback)(long,int)){

  long beginword=p->cursor*(CD_FRAMEWORDS);
  long endword=beginword+CD_FRAMEWORDS;
  long currword=beginword;
  long retry_count=0,lastend=-2;

  if(beginword>p->skiplimit)p->skiplimit=beginword;

  /* First, is the sector we want already in the root? */
  while(p->root.verifyend==-1 || p->root.verifybegin>beginword || 
	p->root.verifyend<endword+(MAX_SECTOR_PRECACHE*CD_FRAMEWORDS)){

    if(!(p->enable&(PARANOIA_MODE_VERIFY|PARANOIA_MODE_OVERLAP)))
      if(p->root.verifyend!=-1 && p->root.verifybegin<=beginword && 
	 p->root.verifyend>=endword)break;

    lastend=p->root.verifyend;

    /* Nope; we need to build or extend the root verified range */

    if(p->enable&(PARANOIA_MODE_VERIFY|PARANOIA_MODE_OVERLAP))
      paranoia_verifyloop(p,beginword,endword+(MAX_SECTOR_PRECACHE*CD_FRAMEWORDS),callback);

    if(!(p->root.verifyend==-1 || p->root.verifybegin>beginword || 
	 p->root.verifyend<endword+(MAX_SECTOR_PRECACHE*CD_FRAMEWORDS))) 
      break;
    i_paranoia_trim(&(p->root),beginword,endword);

    /* read more! *******************************************/
    {
      /* why do it this way?  We read, at best guess, atomic-sized
	 read blocks, but we need to read lots of sectors to kludge
	 around stupid read ahead buffers on cheap drives, as well as
	 avoid expensize back-seeking. */
      
      long readat;
      long totaltoread=p->readahead;
      long sectatonce=p->d->nsectors;
      long driftcomp=(float)p->dyndrift/CD_FRAMEWORDS+.5;

      /* What is the first sector to read?  want some pre-buffer if
	 we're not at the extreme beginning of the disc */

      if(p->enable&(PARANOIA_MODE_VERIFY|PARANOIA_MODE_OVERLAP)){
	if(p->enable&PARANOIA_MODE_VERIFY)p->jitter++;
	if(p->jitter>2)p->jitter=0; /* this is not arbitrary; think about it */
	
	if(p->root.verifyend==-1 || p->root.verifybegin>beginword)
	  readat=p->cursor-p->jitter-p->dynoverlap; 
	else
	  readat=p->root.verifyend/(CD_FRAMEWORDS)-p->jitter-
	    p->dynoverlap;
      }else{
	readat=p->cursor; 
	totaltoread=sectatonce;
      }

      readat-=driftcomp;

      while(totaltoread>0){
	long secread=sectatonce;
	long adjread=readat;
	p_block *new;

	/* don't under/overflow the audio session */
	if(adjread<p->current_firstsector){
	  secread-=p->current_firstsector-adjread;
	  adjread=p->current_firstsector;
	}
	if(adjread+secread-1>p->current_lastsector)
	  secread=p->current_lastsector-adjread+1;

	if(secread>0){
	  if(p->enable&(PARANOIA_MODE_OVERLAP|PARANOIA_MODE_VERIFY))
	    new=new_p_block(p);
	  else{
	    new=&(p->root);
	    release_p_block(new); /* in the case of root it's just the buffer */
	  }

	  /* The Linux native ATAPI driver has an off by one bug
	     bounds checking the lba address */

	  if(adjread==p->current_lastsector){
	    adjread--;
	    secread++;
	  }
	  
	  p_buffer(new,
		   malloc(secread*CD_FRAMESIZE_RAW),
		   secread*CD_FRAMESIZE_RAW);
	  
	  if((secread=cdda_read(p->d,new->buffer,adjread,secread))<=0){
	    /* cdda_read only bails on *really* serious errors */
	    release_p_block(new);
	    return(NULL);
	  }
	  
	  if(adjread+secread-1==p->current_lastsector)
	    new->lastsector=-1;

	  (*callback)((adjread+secread-1)*CD_FRAMEWORDS,PARANOIA_CB_READ);
	  new->begin=(adjread+driftcomp)*CD_FRAMEWORDS; 
	  new->end=new->begin+secread*CD_FRAMEWORDS;
	  if(!(p->enable&(PARANOIA_MODE_VERIFY|PARANOIA_MODE_OVERLAP))){
	    new->verifybegin=new->begin;
	    new->verifyend=new->end;
	    if(new->lastsector){
	      new->done=1;
	      verify_end_case(p,endword+
			      (MAX_SECTOR_PRECACHE*CD_FRAMEWORDS),
			      callback);


	    }
	  }

	  totaltoread-=secread;
	  readat+=sectatonce; 
	}else
	  break;
      }

    }

    if(p->root.verifyend==-1 || p->root.verifybegin<beginword)
      currword=beginword;
    else
      currword=p->root.verifybegin;

    /* Are we doing lots of retries?  **************************************/

    if(lastend<p->root.verifyend){
      lastend=p->root.verifyend;
      retry_count=0;
    }else{
      /* increase overlap or bail */
      retry_count++;
      
      if(retry_count>20)verify_skip_case(p,callback);

      if(retry_count>=7)p->dynoverlap=MAX_SECTOR_OVERLAP;

      if(retry_count%10==0){
	/* D'oh.  Back off  */
	verify_backoff(p,callback);
      }
    }
  }
  p->cursor++;

  return(p->root.buffer+(beginword-p->root.begin));
}


