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

  int bytes_so_far=0;
  char *buffer=(char *)p;
  long bytestotal=sectors*CD_FRAMESIZE_RAW;
  begin*=CD_FRAMESIZE_RAW;

  while(bytes_so_far<bytestotal){
    long local_bytes=bytestotal-bytes_so_far;
    int jitter=(int)((drand48()-.5)*CD_FRAMESIZE_RAW*4)*4;
    long rbytes,bytes=8*(int)(drand48()*CD_FRAMESIZE_RAW); /* max 8 sectors */

    char *local_buf=buffer+bytes_so_far;
    if(bytes>local_bytes)bytes=local_bytes;

    if(begin==0)jitter=0;
    if(jitter<0)jitter=0;

    /*    printf("%d %ld sector jitter nbytes\n",jitter,
	   bytes); */
    
    lseek(d->cdda_fd,begin+bytes_so_far+jitter,SEEK_SET);
    rbytes=read(d->cdda_fd,local_buf,bytes);
    if(rbytes<bytes)
      memset(local_buf+rbytes,0,bytes-rbytes);
    bytes_so_far+=bytes;
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

