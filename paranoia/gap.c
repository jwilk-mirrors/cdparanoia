/***
 * CopyPolicy: GNU Public License 2 applies
 * Copyright (C) by Monty (xiphmont@mit.edu)
 *
 * Gapa analysis support code for paranoia
 *
 ***/

#include "p_block.h"
#include "cdda_paranoia.h"
#include "gap.h"

/**** Gap analysis code ***************************************************/

long i_paranoia_overlap(size16 *buffA,size16 *buffB,
			       long offsetA, long offsetB,
			       long sizeA,long sizeB,
			       long *ret_begin, long *ret_end){
  long beginA=offsetA,endA=offsetA;
  long beginB=offsetB,endB=offsetB;

  for(;beginA>=0 && beginB>=0;beginA--,beginB--)
    if(buffA[beginA]!=buffB[beginB])break;
  beginA++;
  beginB++;
  
  for(;endA<sizeA && endB<sizeB;endA++,endB++)
    if(buffA[endA]!=buffB[endB])break;
  
  if(ret_begin)*ret_begin=beginA;
  if(ret_end)*ret_end=endA;
  return(endA-beginA);
}

long i_paranoia_overlap2(size16 *buffA,size16 *buffB,
			 char *flagsA,char *flagsB,
			 long offsetA, long offsetB,
			 long sizeA,long sizeB,
			 long *ret_begin, long *ret_end){
  long beginA=offsetA,endA=offsetA;
  long beginB=offsetB,endB=offsetB;
  
  for(;beginA>=0 && beginB>=0;beginA--,beginB--){
    if(buffA[beginA]!=buffB[beginB])break;
    /* don't allow matching across matching sector boundaries */
    /* don't allow matching through known missing data */
    if(flagsA[beginA]&flagsB[beginB]&1){
      beginA--;
      beginB--;
      break;
    }
    if((flagsA[beginA]|flagsB[beginB])&2)break;
  }
  beginA++;
  beginB++;
  
  for(;endA<sizeA && endB<sizeB;endA++,endB++){
    if(buffA[endA]!=buffB[endB])break;
    /* don't allow matching across matching sector boundaries */
    if((flagsA[endA]&flagsB[endB]&1)&&endA!=beginA){
      break;
    }

    /* don't allow matching through known missing data */
    if((flagsA[endA]|flagsB[endB])&2)break;
  }

  if(ret_begin)*ret_begin=beginA;
  if(ret_end)*ret_end=endA;
  return(endA-beginA);
}

long i_paranoia_overlap_r(size16 *buffA,size16 *buffB,
			  long offsetA, long offsetB){
  long beginA=offsetA;
  long beginB=offsetB;

  for(;beginA>=0 && beginB>=0;beginA--,beginB--)
    if(buffA[beginA]!=buffB[beginB])break;
  beginA++;
  beginB++;
  
  return(offsetA-beginA);
}

long i_paranoia_overlap_f(size16 *buffA,size16 *buffB,
			  long offsetA, long offsetB,
			  long sizeA,long sizeB){
  long endA=offsetA;
  long endB=offsetB;
  
  for(;endA<sizeA && endB<sizeB;endA++,endB++)
    if(buffA[endA]!=buffB[endB])break;
  
  return(endA-offsetA);
}

int i_stutter_or_gap(size16 *A, size16 *B,long offA, long offB,
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
void i_analyze_rift_f(size16 *A,size16 *B,
		      long sizeA, long sizeB,
		      long aoffset, long boffset, 
		      long *matchA,long *matchB,long *matchC){
  
  long apast=sizeA-aoffset;
  long bpast=sizeB-boffset;
  long i;
  
  *matchA=0, *matchB=0, *matchC=0;
  
  /* Look for three possible matches... (A) Ariftv->B, (B) Briftv->A and 
     (c) AB->AB. */
  
  for(i=0;;i++){
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
	if(i_paranoia_overlap_f(A,B,aoffset+i,boffset+i,sizeA,sizeB)>=MIN_WORDS_RIFT){
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

void i_analyze_rift_r(size16 *A,size16 *B,
		      long sizeA, long sizeB,
		      long aoffset, long boffset, 
		      long *matchA,long *matchB,long *matchC){
  
  long apast=aoffset+1;
  long bpast=boffset+1;
  long i;
  
  *matchA=0, *matchB=0, *matchC=0;
  
  /* Look for three possible matches... (A) Ariftv->B, (B) Briftv->A and 
     (c) AB->AB. */
  
  for(i=0;;i++){
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
	if(i_paranoia_overlap_r(A,B,aoffset-i,boffset-i)>=MIN_WORDS_RIFT){
	  *matchC=i;
	  break;
	}
    }else
      if(i>=bpast)break;
    
  }
  
  if(*matchA==0 && *matchB==0 && *matchC==0)return;
  
  if(*matchC)return;
  
  if(*matchA){
    if(i_stutter_or_gap(A,B,aoffset+1,boffset-*matchA+1,*matchA))
      return;
    *matchB=-*matchA; /* signify we need to remove n bytes from B */
    *matchA=0;
    return;
  }else{
    if(i_stutter_or_gap(B,A,boffset+1,aoffset-*matchB+1,*matchB))
      return;
    *matchA=-*matchB;
    *matchB=0;
    return;
  }
}

#ifdef TEST
#include <stdio.h>

int main(){
  {
    size16 buffA[20]={-1,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18};
    size16 buffB[20]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20};
    size16 buffC[20]={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19};
    
    char flags1[20]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    char flags2[20]={0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    char flags3[20]={0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    char flags4[20]={0,0,0,0,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0};
    long begin,end,ret;
    
    /* no match */
    ret=i_paranoia_overlap2(buffA,buffB,flags1,flags1,0,0,20,20,&begin,&end);
    if(ret!=-1 || begin!=1 || end!=0){
      printf("i_paranoia_overlap2: failed test 1 ret:%ld begin:%ld end:%ld\n",
	     ret,begin,end);
      return(1);
    }
    
    /* full match offset 2 */
    ret=i_paranoia_overlap2(buffA,buffB,flags1,flags1,2,0,20,20,&begin,&end);
    if(ret!=18 || begin!=2 || end!=20){
      printf("i_paranoia_overlap2: failed test 2 ret:%ld begin:%ld end:%ld\n",
	     ret,begin,end);
      
      return(1);
    }
    
    /* full match offset 1 */
    ret=i_paranoia_overlap2(buffA,buffC,flags1,flags1,1,0,20,20,&begin,&end);
    if(ret!=19 || begin!=1 || end!=20){
      printf("i_paranoia_overlap2: failed test 3 ret:%ld begin:%ld end:%ld\n",
	     ret,begin,end);
      return(1);
    }
    
    /* bound match */
    ret=i_paranoia_overlap2(buffA,buffC,flags2,flags3,11,10,20,20,&begin,&end);
    if(ret!=15 || begin!=5 || end!=20){
      printf("i_paranoia_overlap2: failed test 4 ret:%ld begin:%ld end:%ld\n",
	     ret,begin,end);
      
      return(1);
    }
    
    /* unmatchable data */
    ret=i_paranoia_overlap2(buffA,buffC,flags1,flags4,11,10,20,20,&begin,&end);
    if(ret!=12 || begin!=8 || end!=20){
      printf("i_paranoia_overlap2: failed test 5 ret:%ld begin:%ld end:%ld\n",
	     ret,begin,end);
      
      return(1);
    }
    
    /* unmatchable data */
    begin=-1;
    end=-1;
    ret=i_paranoia_overlap2(buffA,buffC,flags1,flags4,5,4,20,20,&begin,&end);
    if(ret!=-1 || begin!=6 || end!=5){
      printf("i_paranoia_overlap2: failed test 6 ret:%ld begin:%ld end:%ld\n",
	     ret,begin,end);
      return(1);
    }
  }

  {
    size16 A[40]={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,
		  20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39};
    size16 B[40]={-2,-1,0,1,2,3,4,5,6,7,8,9,11,12,13,14,15,16,17,18,19,
		  20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38};
    size16 C[42]={-2,-1,0,1,2,3,4,5,6,7,8,9,10,10,11,12,13,14,15,16,17,18,19,
		  20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38};
    size16 D[40]={0,1,2,3,4,5,6,7,8,9,100,110,120,13,14,15,16,17,18,19,
		  20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39};
    long matchA,matchB,matchC;

    i_analyze_rift_f(A,B,40,40,10,12,&matchA,&matchB,&matchC);
    if(matchB!=1){
      printf("i_analyze_rift_f: failed test1 matchA:%ld matchB:%ld "
	     "matchC:%ld\n",matchA,matchB,matchC);
      exit(1);
    }
    i_analyze_rift_f(B,A,40,40,12,10,&matchA,&matchB,&matchC);
    if(matchA!=1){
      printf("i_analyze_rift_f: failed test2 matchA:%ld matchB:%ld "
	     "matchC:%ld\n",matchA,matchB,matchC);
      exit(1);
    }

    i_analyze_rift_f(A,C,40,42,11,13,&matchA,&matchB,&matchC);
    if(matchB!=-1){
      printf("i_analyze_rift_f: failed test3 matchA:%ld matchB:%ld "
	     "matchC:%ld\n",matchA,matchB,matchC);
      exit(1);
    }
    i_analyze_rift_f(C,A,42,40,13,11,&matchA,&matchB,&matchC);
    if(matchA!=-1){
      printf("i_analyze_rift_f: failed test4 matchA:%ld matchB:%ld "
	     "matchC:%ld\n",matchA,matchB,matchC);
      exit(1);
    }

    i_analyze_rift_f(A,D,40,40,10,10,&matchA,&matchB,&matchC);
    if(matchC!=3){
      printf("i_analyze_rift_f: failed test5 matchA:%ld matchB:%ld "
	     "matchC:%ld\n",matchA,matchB,matchC);
      exit(1);
    }
    i_analyze_rift_f(D,A,40,40,10,10,&matchA,&matchB,&matchC);
    if(matchC!=3){
      printf("i_analyze_rift_f: failed test6 matchA:%ld matchB:%ld "
	     "matchC:%ld\n",matchA,matchB,matchC);
      exit(1);
    }
  }

  {
    size16 A[40]={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,
		  20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39};
    size16 B[40]={-1,-2,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,
		  20,21,22,23,24,25,26,27,28,29,30,31,32,33,35,36,37,38};
    size16 C[40]={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,
		  20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,34,35,36,37,38};
    size16 D[40]={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,
		  20,21,22,23,24,25,26,27,28,29,3,3,3,33,34,35,36,37,38,39};
   long matchA,matchB,matchC;

    i_analyze_rift_r(A,B,40,40,34,35,&matchA,&matchB,&matchC);
    if(matchB!=1){
      printf("i_analyze_rift_r: failed test1 matchA:%ld matchB:%ld "
	     "matchC:%ld\n",matchA,matchB,matchC);
      exit(1);
    }
    i_analyze_rift_r(B,A,40,40,35,34,&matchA,&matchB,&matchC);
    if(matchA!=1){
      printf("i_analyze_rift_r: failed test2 matchA:%ld matchB:%ld "
	     "matchC:%ld\n",matchA,matchB,matchC);
      exit(1);
    }
    i_analyze_rift_r(A,C,40,40,33,34,&matchA,&matchB,&matchC);
    if(matchB!=-1){
      printf("i_analyze_rift_r: failed test3 matchA:%ld matchB:%ld "
	     "matchC:%ld\n",matchA,matchB,matchC);
      exit(1);
    }
    i_analyze_rift_r(C,A,40,40,34,33,&matchA,&matchB,&matchC);
    if(matchA!=-1){
      printf("i_analyze_rift_r: failed test4 matchA:%ld matchB:%ld "
	     "matchC:%ld\n",matchA,matchB,matchC);
      exit(1);
    }
    i_analyze_rift_r(A,D,40,40,32,32,&matchA,&matchB,&matchC);
    if(matchC!=3){
      printf("i_analyze_rift_r: failed test5 matchA:%ld matchB:%ld "
	     "matchC:%ld\n",matchA,matchB,matchC);
      exit(1);
    }
    i_analyze_rift_r(D,A,40,40,32,32,&matchA,&matchB,&matchC);
    if(matchC!=3){
      printf("i_analyze_rift_r: failed test6 matchA:%ld matchB:%ld "
	     "matchC:%ld\n",matchA,matchB,matchC);
      exit(1);
    }

  }

  return(0);
}

#endif


