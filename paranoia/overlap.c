/***
 * CopyPolicy: GNU Public License 2 applies
 * Copyright (C) by Monty (xiphmont@mit.edu)
 *
 * Statistic code and cache management for overlap settings
 *
 ***/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "p_block.h"
#include "cdda_paranoia.h"
#include "overlap.h"
#include "isort.h"

/**** Internal cache management *****************************************/

void paranoia_resetcache(cdrom_paranoia *p){
  c_block *c=c_first(p);
  v_fragment *v=v_first(p);

  while(c){
    free_c_block(c);
    c=c_first(p);
  }
  while(v){
    free_v_fragment(v);
    v=v_first(p);
  }
}

void paranoia_resetall(cdrom_paranoia *p){
  p->root.returnedlimit=0;
  p->dyndrift=0;
  p->root.lastsector=0;

  if(p->root.vector){
    isort_free(p->root.vector);
    p->root.vector=NULL;
  }

  paranoia_resetcache(p);
}

void i_paranoia_trim(cdrom_paranoia *p,long beginword,long endword){
  root_block *root=&(p->root);
  if(root->vector!=NULL){
    long target=beginword-MAX_SECTOR_OVERLAP*CD_FRAMEWORDS;
    long rbegin=isort_begin(root->vector);
    long rend=isort_end(root->vector);

    if(rbegin>beginword)
      goto rootfree;
    
    if(rbegin+MAX_SECTOR_OVERLAP*CD_FRAMEWORDS<beginword){
      if(target+MIN_WORDS_OVERLAP>rend)
	goto rootfree;

      {
	long offset=target-rbegin;
	isort_removef(root->vector,offset);
      }
    }

    {
      c_block *c=c_first(p);
      while(c){
	c_block *next=c_next(c);
	if(isort_end(c->vector)<beginword-MAX_SECTOR_OVERLAP*CD_FRAMEWORDS)
	  free_c_block(c);
	c=next;
      }
    }

  }
  return;
  
rootfree:

  isort_free(root->vector);
  root->vector=NULL;
  root->returnedlimit=-1;
  root->lastsector=0;
  
}

/**** Statistical and heuristic[al? :-] management ************************/

#ifndef rv
#  define rv (p->root.vector)
#  define rb (isort_begin(p->root.vector))
#  define re (isort_end(p->root.vector))
#endif

void offset_adjust_settings(cdrom_paranoia *p, void(*callback)(long,int)){
  if(p->stage2.offpoints>=10){
    /* drift: look at the average offset value.  If it's over one
       sector, frob it.  We just want a little hysteresis [sp?]*/
    long av=(p->stage2.offpoints?p->stage2.offaccum/p->stage2.offpoints:0);
    
    if(abs(av)>p->dynoverlap/4 && abs(av)>CD_FRAMEWORDS){
      av=(av/MIN_SECTOR_EPSILON)*MIN_SECTOR_EPSILON;
      
      if(callback)(*callback)(re,PARANOIA_CB_DRIFT);
      p->dyndrift+=av;
      
      /* Adjust all the values in the cache otherwise we get a
	 (potentially unstable) feedback loop */
      {
	c_block *c=c_first(p);
	v_fragment *v=v_first(p);

	while(v && v->one){
	  /* safeguard beginning bounds case with a hammer */
	  if(v->begin<av || isort_begin(v->one->vector)<av){
	    v->one=NULL;
	  }else{
	    v->begin-=av;
	  }
	  v=v_next(v);
	}
	while(c){
	  long adj=min(av,isort_begin(c->vector));
	  isort_set(c->vector,isort_begin(c->vector)-adj);
	  c=c_next(c);
	}
      }

      p->stage2.offaccum=0;
      p->stage2.offmin=0;
      p->stage2.offmax=0;
      p->stage2.offpoints=0;
      p->stage2.newpoints=0;
      p->stage2.offdiff=0;
    }
  }

  if(p->stage1.offpoints){
    /* dynoverlap: we arbitrarily set it to 4x the running difference
       value, unless min/max are more */

    p->dynoverlap=(p->stage1.offpoints?p->stage1.offdiff/
		   p->stage1.offpoints*3:CD_FRAMEWORDS);

    if(p->dynoverlap<-p->stage1.offmin*1.5)
      p->dynoverlap=-p->stage1.offmin*1.5;
						     
    if(p->dynoverlap<p->stage1.offmax*1.5)
      p->dynoverlap=p->stage1.offmax*1.5;

    if(p->dynoverlap<MIN_SECTOR_EPSILON)p->dynoverlap=MIN_SECTOR_EPSILON;
    if(p->dynoverlap>MAX_SECTOR_OVERLAP*CD_FRAMEWORDS)
      p->dynoverlap=MAX_SECTOR_OVERLAP*CD_FRAMEWORDS;
    			     
    if(callback)(*callback)(p->dynoverlap,PARANOIA_CB_OVERLAP);

    if(p->stage1.offpoints>50){
      p->stage1.offpoints/=2;
      p->stage1.offaccum/=2;
      p->stage1.offdiff/=2;
    }
    p->stage1.offmin=0;
    p->stage1.offmax=0;
    p->stage1.newpoints=0;
  }
}

void offset_add_value(cdrom_paranoia *p,offsets *o,long value,
			     void(*callback)(long,int)){
  if(o->offpoints)
    o->offdiff+=abs(value);

  o->offpoints++;
  o->newpoints++;
  o->offaccum+=value;
  if(value<o->offmin)o->offmin=value;
  if(value>o->offmax)o->offmax=value;

  if(o->newpoints>=10)offset_adjust_settings(p,callback);
}

