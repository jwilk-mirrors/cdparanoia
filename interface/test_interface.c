/******************************************************************
 * CopyPolicy: GNU Public License 2 applies
 * Copyright (C) Monty xiphmont@mit.edu
 *
 * Fake interface backend for testing paranoia layer
 *
 ******************************************************************/

#ifdef CDDA_TEST
#include "low_interface.h"
#include "utils.h"

#define CDDA_TEST_JITTER
#define CDDA_TEST_BOGUSBYTES
#define CDDA_TEST_SCRATCHES

static int test_readtoc (cdrom_drive *d){
  int tracks=0;
  long bytes;
  long sectors;

  /* only one track, as many sectors as the file */

  bytes=lseek(d->cdda_fd,0,SEEK_END);
  lseek(d->cdda_fd,0,SEEK_SET);
  sectors=bytes/CD_FRAMESIZE_RAW;

  d->disc_toc[0].bFlags = 0;
  d->disc_toc[0].bTrack = 1;
  d->disc_toc[0].dwStartSector = 0;

  d->disc_toc[1].bFlags = 0x4;
  d->disc_toc[1].bTrack = CDROM_LEADOUT;
  d->disc_toc[1].dwStartSector = sectors;

  tracks=2;
  d->cd_extra=0;
  return(--tracks);  /* without lead-out */
}

/* we emulate jitter, scratches, atomic jitter and bogus bytes on
   boundaries, etc */

static long test_read(cdrom_drive *d, void *p, long begin, long sectors){

  int bytes_so_far=0,rbytes;
  char *buffer=(char *)p;
  long bytestotal=sectors*CD_FRAMESIZE_RAW;
  int jitter=4*(int)((drand48()-.5)*CD_FRAMESIZE_RAW/8);
 
  /*if(begin>=200 && begin<=220){
    errno=EIO;
    return(-1);
  }*/

  begin*=CD_FRAMESIZE_RAW;
  /*  while(bytes_so_far<bytestotal){
    long local_bytes=bytestotal-bytes_so_far;
    long rbytes,bytes=bytestotal;/*(int)(CD_FRAMESIZE_RAW*drand48())*4;*/

  /*    char *local_buf=buffer+bytes_so_far;
    if(bytes>local_bytes)bytes=local_bytes;*/

  /*    if(begin==0)jitter=0;*/
  if(begin+jitter<0)jitter=0;
  
  {
    long seeki;
    /*      long bound=23520;
	    long nextbound=begin+bytes_so_far+bound;
	    nextbound=nextbound-(nextbound%bound)+12;
	    
	    if(begin+bytes_so_far+bytes>nextbound){
	    bytes=nextbound-begin-bytes_so_far;
	    }*/
    
    /*      if(drand48()>.5){
	    seeki=begin+bytes_so_far+jitter-8;
	    }else{*/
    seeki=begin+jitter+8;
    
    
    if(lseek(d->cdda_fd,seeki,SEEK_SET)<0){
      return(0);
    }
    
    rbytes=read(d->cdda_fd,buffer,bytestotal);
    return(rbytes/CD_FRAMESIZE_RAW);
    /*
      bytes_so_far+=rbytes;
      }*/
  }
  return(sectors);
  
}

/* hook */
static int Dummy (cdrom_drive *d,int Switch){
  return(0);
}

/* set function pointers to use the ioctl routines */
int test_init_drive (cdrom_drive *d){

  d->nsectors=8;
  d->enable_cdda = Dummy;
  d->read_audio = test_read;
  d->read_toc = test_readtoc;
  d->tracks=d->read_toc(d);
  if(d->tracks==-1)
    return(d->tracks);
  d->opened=1;
  srand48(0);
  return(0);
}

#endif

