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

/* Testing times:

                        6        7        8       9.2      9.3
   OK                         0:02.66* 0:03.15  0:03.11  0:04.98
   JITTER_SMALL               0:53.40  0:51.97  0:27.97  0:10.15
   JITTER_LARGE      0:06.24           5:50.29  2:44.22  0:15.42
   JITTER_MASSIVE    0:42.19                   10:58.97  0:42.81
   FRAG_SMALL        0:04.84                    0:33.40  0:12.31
   FRAG_LARGE        0:07.68                    3:14.69  0:19.35
   FRAG_MASSIVE      failure                   34:16.89  2:43.02
   BOGUS_BYTES
   DROPDUPE_BYTES    0:19.53* 0:27.91  0:23.54  1:35.97  0:18.78
   SCRATCH           1:07.96*                            0:16.13
   UNDERRUN          0:06.81# failure  1:09.40# 1:07.68  0:17.53
   
   * defect of some sort 
   # not including a hang at end of read

   */

/* Build which test model? */
#undef  CDDA_TEST_OK
#undef  CDDA_TEST_JITTER_SMALL
#undef  CDDA_TEST_JITTER_LARGE
#define CDDA_TEST_JITTER_MASSIVE
#undef  CDDA_TEST_FRAG_SMALL
#undef  CDDA_TEST_FRAG_LARGE
#undef  CDDA_TEST_FRAG_MASSIVE
#undef  CDDA_TEST_BOGUS_BYTES
#undef  CDDA_TEST_DROPDUPE_BYTES
#undef  CDDA_TEST_SCRATCH
#undef  CDDA_TEST_UNDERRUN


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
  long bytestotal;
  static FILE *fd=NULL;

  if(!fd)fd=fdopen(d->cdda_fd,"r");

#ifdef CDDA_TEST_UNDERRUN
  sectors-=1;
#endif

  bytestotal=sectors*CD_FRAMESIZE_RAW;

  begin*=CD_FRAMESIZE_RAW;

  while(bytes_so_far<bytestotal){
    int inner_bytes=bytestotal-bytes_so_far;
    char *inner_buf=p+bytes_so_far;
    long seeki;
    long rbytes;

#ifdef CDDA_TEST_OK
    int this_jitter=0;
    long this_bytes=inner_bytes;
    
#else
#ifdef CDDA_TEST_JITTER_SMALL
    int this_jitter=4*(int)((drand48()-.5)*CD_FRAMESIZE_RAW/8);
    long this_bytes=inner_bytes;

#else
#ifdef CDDA_TEST_JITTER_LARGE
    int this_jitter=32*(int)((drand48()-.5)*CD_FRAMESIZE_RAW/8);
    long this_bytes=inner_bytes;

#else
#ifdef CDDA_TEST_JITTER_MASSIVE
    int this_jitter=128*(int)((drand48()-.5)*CD_FRAMESIZE_RAW/8);
    long this_bytes=inner_bytes;

#else
#ifdef CDDA_TEST_FRAG_SMALL
    int this_jitter=4*(int)((drand48()-.5)*CD_FRAMESIZE_RAW/8);
    long this_bytes=256*(int)(drand48()*CD_FRAMESIZE_RAW/8);

#else
#ifdef CDDA_TEST_FRAG_LARGE
    int this_jitter=32*(int)((drand48()-.5)*CD_FRAMESIZE_RAW/8);
    long this_bytes=256*(int)(drand48()*CD_FRAMESIZE_RAW/8);

#else
#ifdef CDDA_TEST_FRAG_MASSIVE
    int this_jitter=32*(int)((drand48()-.5)*CD_FRAMESIZE_RAW/8);
    long this_bytes=8*(int)(drand48()*CD_FRAMESIZE_RAW/8);

#else
#ifdef CDDA_TEST_DROPDUPE_BYTES
    long this_bytes=CD_FRAMESIZE_RAW;
    int this_jitter;

    if (drand48()>.8)
      this_jitter=32;
    else
      this_jitter=0;

#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif

    if(this_bytes>inner_bytes)this_bytes=inner_bytes;
    if(begin+this_jitter+bytes_so_far<0)this_jitter=0;    
    seeki=begin+bytes_so_far+this_jitter;
    
    if(fseek(fd,seeki,SEEK_SET)<0){
      return(0);
    }
    rbytes=fread(inner_buf,1,this_bytes,fd);
    bytes_so_far+=rbytes;
  }

#ifdef CDDA_TEST_SCRATCH
  {
    long location=300*CD_FRAMESIZE_RAW+(drand48()*56)+512;

    if(begin<=location && begin+bytestotal>location){
      memset(p+location-begin,(int)(drand48()*256),1100);
    }
  }
#endif

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

