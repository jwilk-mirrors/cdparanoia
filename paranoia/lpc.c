/* The code here is derived from another one of my (commercial)
software packages, but this version is freely distributable under
terms of the GPL.  The original copyrights are below.  --Monty */

/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE OggSQUISH SOFTWARE CODEC SOURCE CODE.   *
 *                                                                  *
 ********************************************************************

  file: lpc.c
  function: LPC low level routines
  author: Monty <xiphmont@mit.edu>
  modifications by: Monty
  last modification date: Jun 11 1996

 ********************************************************************/

/* Some of these routines (autocorrelator, LPC coefficient estimator)
   are directly derived from and/or modified from code written by
   Jutta Degener and Carsten Bormann; thus we include their copyright
   below.  The entirety of this file is freely redistributable on the
   condition that both of these copyright notices are preserved
   without modification.  */

/* Preserved Copyright: *********************************************/

/* Copyright 1992, 1993, 1994 by Jutta Degener and Carsten Bormann,
Technische Universita"t Berlin

Any use of this software is permitted provided that this notice is not
removed and that neither the authors nor the Technische Universita"t
Berlin are deemed to have made any representations as to the
suitability of this software for any purpose nor are held responsible
for any defects of this software. THERE IS ABSOLUTELY NO WARRANTY FOR
THIS SOFTWARE.

As a matter of courtesy, the authors request to be informed about uses
this software has found, about bugs in this software, and about any
improvements that may be of general interest.

Berlin, 28.11.1994
Jutta Degener
Carsten Bormann

*********************************************************************/

#include <stdio.h>
#include <malloc.h>
#include <math.h>
#include "lpc.h"

/* Algorithm Invented by N. Levinson in 1947, modified by J. Durbin in
   1959. */

float levinson_durbin(float *autoc,float *lpccoeff,int p){
  /*  in: [0...p] autocorrelation values */
  /* out: [0...p-1] LPC coefficients + a[0] constant as return val */
  int i,j;  
  float r,*ref,error=autoc[0];

  if(autoc[0]==0)return 0;

  ref=malloc(sizeof(float)*p);
  for(i=0;i<p;i++){

    /* Sum up this iteration's reflection coefficient. */
    r=-autoc[i+1];
    for(j=0;j<i;j++)r-=lpccoeff[j]*autoc[i-j];
    ref[i]=(r/=error);    

    /* Update LPC coefficients and total error. */
    lpccoeff[i]=r;
    for(j=0;j<(i>>1);j++){
      float tmp=lpccoeff[j];
      lpccoeff[j]+=r*lpccoeff[i-1-j];
      lpccoeff[i-1-j]+=r*tmp;
    }
    if(i%2)lpccoeff[j]+=lpccoeff[j]*r;

    error*=1.0-r*r;
  }
  free(ref);
  return error;
}

/* Compute the autocorrelation for lags between 0 and lag-1, and x==0
   outside 0...n-1 */

void autocorrelation(int n, float *x, int lag, float *ac){
  /*  in: [0...n-1] samples x   */
  /* out: [0...lag-1] ac values */
  float d; 
  int i;
  while(lag--){
    for(i=lag,d=0;i<n;i++)d+=x[i]*x[i-lag];
    ac[lag]=d;
  }
}

void residualD(float *coeff,float *prime,int m,float *data,
	       float *residue,long n){

  /* in: coeff[0...m-1] LPC coefficients 
         prime[0...m-1] initial values 
	 data[0...n-1] data samples 
    out: residue[0...n-1] residuals from LPC prediction */

  long i,j;
  float y;

  for(i=0;i<n;i++){
    y=0;
    for(j=0;j<m;j++)
      y-=prime[i+j]*coeff[m-j-1];

    residue[i]=data[i]-y;
    prime[i+m]=data[i];
  }
}

void predictD(float *coeff,float *prime,int m,float *data,float *residue,
		     long n){
  /* in: coeff[0...m-1] LPC coefficients 
         prime[0...m-1] ... n+m-1]  initial values (allocated size of n+m-1)
         residue[0...n-1] residuals from LPC prediction   
    out: data[0...n-1] data samples */
  long i,j,o,p;
  float y;
  
  for(i=0;i<n;i++){
    if(residue)
      y=residue[i];
    else
      y=0;
    o=i;
    p=m;
    for(j=0;j<m;j++)
      y-=prime[o++]*coeff[--p];
      
    data[i]=prime[o]=y;
  }
}

float *coeffsD(float *data,long n,int coeffs){
  float *work=calloc(coeffs+1,sizeof(float));
  float *ret=calloc(coeffs,sizeof(float));

  autocorrelation(n,data,coeffs+1,work);
  levinson_durbin(work,ret,coeffs);
  free(work);
  return(ret);
}

void predict_forward(long *data,long presamples,int coeffs,
		     long *hole,long samples){
  short *wd=(short *)data;
  short *wh=(short *)hole;
  long count;
  
  if(coeffs>presamples)coeffs=presamples;

  {
    float *cF;
    float *dataF=malloc((presamples+samples)*sizeof(float));
    float *holeF=malloc(samples*sizeof(float));

    for(count=0;count<presamples;count++)dataF[count]=wd[count<<1];
    while(count<presamples+samples)dataF[count++]=0;

    cF=coeffsD(dataF,presamples,coeffs);
    predictD(cF,dataF+presamples-coeffs,coeffs,holeF,NULL,samples);
    
    for(count=0;count<samples;count++)wh[count<<1]=holeF[count];
    free(cF);

    for(count=0;count<presamples;count++)dataF[count]=wd[(count<<1)+1];
    while(count<presamples+samples)dataF[count++]=0;

    cF=coeffsD(dataF,presamples,coeffs);
    predictD(cF,dataF+presamples-coeffs,coeffs,holeF,NULL,samples);
    
    for(count=0;count<samples;count++)wh[(count<<1)+1]=holeF[count];
    free(cF);
    free(dataF);
    free(holeF);
  }
} 

/* Here, hole points to the beginning of the hole (if increasing time
   is forward).  data points to the first sample */

void predict_backward(long *data,long presamples,int coeffs,
		     long *hole,long samples){
  short *wd=(short *)data;
  short *wh=(short *)hole;
  long count;
  
  if(coeffs>presamples)coeffs=presamples;

  {
    float *cF;
    float *dataF=malloc((presamples+samples)*sizeof(float));
    float *holeF=malloc(samples*sizeof(float));

    for(count=0;count<presamples;count++)
      dataF[count]=wd[(presamples-count-1)<<1];
    while(count<presamples+samples)
      dataF[count++]=0;

    cF=coeffsD(dataF,presamples,coeffs);
    predictD(cF,dataF+presamples-coeffs,coeffs,holeF,NULL,samples);
    
    for(count=0;count<samples;count++)wh[count<<1]=holeF[samples-count-1];
    free(cF);

    for(count=0;count<presamples;count++)
      dataF[count]=wd[((presamples-count-1)<<1)+1];
    while(count<presamples+samples)
      dataF[count++]=0;

    cF=coeffsD(dataF,presamples,coeffs);
    predictD(cF,dataF+presamples-coeffs,coeffs,holeF,NULL,samples);
    
    for(count=0;count<samples;count++)wh[(count<<1)+1]=holeF[samples-count-1];
    free(cF);
    free(dataF);
    free(holeF);
  }
} 

long *for_hole=NULL;
long *bac_hole=NULL;

void lpc_smart_spackle(long *data,long *suspect,long samples){
  /* Multistep process....*/
  /* Predict forward and backward over holes */
  /* Reconcile each prediction with end value immediately */
  /* Then mix fore and back values with a window */

  long count;
  int coeffs=64;
  int pre=256;

  if(!for_hole){
    for_hole=malloc((samples+1)*sizeof(long));
    bac_hole=malloc((samples+1)*sizeof(long));
  }

  memcpy(for_hole,data,sizeof(long)*samples);
  memcpy(bac_hole,data,sizeof(long)*samples);

  /* Forward */

  /* We don't need to scan forward for the first string of values to prime 
     prediction at the overlapped side; only on the forward side */
  count=coeffs;

  while(count<samples){
    long holesize=0;

    /* Find the beginning and size of the hole */
    while(!suspect[count] && count<samples)count++;
    while(suspect[count+holesize] && count+holesize<samples)holesize++;

    if(holesize==1){

      /* This isn't the best way to do it.  It's adequate */
      short *shole=(short *)for_hole;
      shole[count<<1]=(shole[(count-1)<<1]+shole[(count+1)<<1])>>1;
      shole[(count<<1)+1]=(shole[((count-1)<<1)+1]+shole[((count+1)<<1)+1])>>1;

    }else{
      /* We probably don't need to worry about the end boundary case with
	 our big paranoia endlap */
      if(count+holesize+1<=samples){
	/* Predict a replacement for lost data */

	predict_forward(for_hole+count-min(count,pre),min(count,pre),coeffs,
			for_hole+count,holesize+1);
	
	{
	  /* Reconcile with end boundary (currently a little cheezy) */
	  
	  short *sdata=(short *)data;
	  short *shole=(short *)for_hole;
	  long hix=(count+holesize)<<1;
	  long dix=(count-1)<<1;
	  long ix;
	  
	  float missL=(float)sdata[hix]-shole[hix];
	  float missR=(float)sdata[hix+1]-shole[hix+1];
	  
	  for(ix=1;ix<holesize+2;ix++){
	    float mult=(1-cos(ix*M_PI/(holesize+1)))/2;
	    float gapL=(missL-1)*mult+1;
	    float gapR=(missR-1)*mult+1;
	    shole[dix+ix*2]+=gapL;
	    shole[dix+ix*2+1]+=gapR;
	    shole[dix+ix*2]*=1-mult;
	    shole[dix+ix*2+1]*=1-mult;
	  }
	}
      }
    }
    count+=holesize;
  }

  /* Do it all backward now */
  /* scan for the first string of OK values */

  count=samples-1;
  {
    long acc=0;
    long innersamples;
    while(acc<16 && count>0){
      if(suspect[count])
	acc=0;
      else
	acc++;
      count--;
    }
    innersamples=count+acc;

    while(count>=0){
      long holesize=0;

      /* Find the beginning and size of the hole */
      while(!suspect[count] && count>=0)count--;
      while(suspect[count-holesize] && count-holesize>=0)holesize++;
      
      /* We probably don't need to worry about the end boundary case with
	 overlap */

      if(holesize==1)
	bac_hole[count]=0; /* We interpolated above */
      else{

	if(count-holesize-1>=0){
	  /* Predict a replacement for lost data */

	  predict_backward(bac_hole+count+1,min(innersamples-count-1,pre),
			   coeffs,bac_hole+count-holesize,holesize+1);
	  
	  {
	    /* Reconcile with end boundary (currently a little cheezy) */
	    
	    short *sdata=(short *)data;
	    short *shole=(short *)bac_hole;
	    long hix=(count-holesize)<<1;
	    long dix=(count+1)<<1;
	    long ix;
	    
	    float missL=(float)sdata[hix]-shole[hix];
	    float missR=(float)sdata[hix+1]-shole[hix+1];
	    
	    for(ix=1;ix<holesize+2;ix++){
	      float mult=(1-cos(ix*M_PI/(holesize+1)))/2;
	      float gapL=(missL-1)*mult+1;
	      float gapR=(missR-1)*mult+1;
	      shole[dix-ix*2]+=gapL;
	      shole[dix-ix*2+1]+=gapR;
	      shole[dix-ix*2]*=1-mult;
	      shole[dix-ix*2+1]*=1-mult;
	    }
	  }
	}
      }
      count-=holesize;
    }
  }

  {
    short *sdata=(short *)data;
    short *shole=(short *)for_hole;
    short *shole2=(short *)bac_hole;

    for(count=0;count<samples;count++){
      if(suspect[count]){
	sdata[count<<1]=shole[count<<1]+shole2[count<<1];
	sdata[(count<<1)+1]=shole[(count<<1)+1]+shole2[(count<<1)+1];
      }
    }
  }
}
