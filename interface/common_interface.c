/******************************************************************
 * CopyPolicy: GNU Lesser Public License 2.1 applies
 * Copyright (C) 1998-2008 Monty monty@xiph.org
 *
 * CDROM communication common to all interface methods is done here 
 * (mostly ioctl stuff, but not ioctls specific to the 'cooked'
 * interface) 
 *
 ******************************************************************/

#include <math.h>
#include "low_interface.h"
#include "utils.h"
#include "smallft.h"

#include <linux/hdreg.h>

/* Test for presence of a cdrom by pinging with the 'CDROMVOLREAD' ioctl() */
/* Also test using CDROM_GET_CAPABILITY (if available) as some newer DVDROMs will
   reject CDROMVOLREAD ioctl for god-knows-what reason */
int ioctl_ping_cdrom(int fd){
  struct cdrom_volctrl volctl;
  if (ioctl(fd, CDROMVOLREAD, &volctl) &&
      ioctl(fd, CDROM_GET_CAPABILITY, NULL)<0)
    return(1); /* failure */

  return(0);
  /* success! */
}


/* Use the ioctl thingy above ping the cdrom; this will get model info */
char *atapi_drive_info(int fd){
  /* Work around the fact that the struct grew without warning in
     2.1/2.0.34 */
  
  struct hd_driveid *id=malloc(512); /* the size in 2.0.34 */
  char *ret;

  if (!(ioctl(fd, HDIO_GET_IDENTITY, id))) {

    if(id->model==0 || id->model[0]==0)
      ret=copystring("Generic Unidentifiable ATAPI CDROM");
    else
      ret=copystring(id->model);
  }else
    ret=copystring("Generic Unidentifiable CDROM");

  free(id);
  return(ret);
}

int data_bigendianp(cdrom_drive *d){
  float lsb_votes=0;
  float msb_votes=0;
  int i,checked;
  int endiancache=d->bigendianp;
  float *a=calloc(1024,sizeof(float));
  float *b=calloc(1024,sizeof(float));
  long readsectors=5;
  int16_t *buff=malloc(readsectors*CD_FRAMESIZE_RAW);

  /* look at the starts of the audio tracks */
  /* if real silence, tool in until some static is found */

  /* Force no swap for now */
  d->bigendianp=-1;
  
  cdmessage(d,"\nAttempting to determine drive endianness from data...");
  d->enable_cdda(d,1);
  for(i=0,checked=0;i<d->tracks;i++){
    float lsb_energy=0;
    float msb_energy=0;
    if(cdda_track_audiop(d,i+1)==1){
      long firstsector=cdda_track_firstsector(d,i+1);
      long lastsector=cdda_track_lastsector(d,i+1);
      int zeroflag=-1;
      long beginsec=0;
      
      /* find a block with nonzero data */
      
      while(firstsector+readsectors<=lastsector){
	int j;
	
	if(d->read_audio(d,buff,firstsector,readsectors)>0){
	  
	  /* Avoid scanning through jitter at the edges */
	  for(beginsec=0;beginsec<readsectors;beginsec++){
	    int offset=beginsec*CD_FRAMESIZE_RAW/2;
	    /* Search *half* */
	    for(j=460;j<128+460;j++)
	      if(buff[offset+j]!=0){
		zeroflag=0;
		break;
	      }
	    if(!zeroflag)break;
	  }
	  if(!zeroflag)break;
	  firstsector+=readsectors;
	}else{
	  d->enable_cdda(d,0);
	  free(a);
	  free(b);
	  free(buff);
	  return(-1);
	}
      }

      beginsec*=CD_FRAMESIZE_RAW/2;
      
      /* un-interleave for an fft */
      if(!zeroflag){
	int j;
	
	for(j=0;j<128;j++)a[j]=le16_to_cpu(buff[j*2+beginsec+460]);
	for(j=0;j<128;j++)b[j]=le16_to_cpu(buff[j*2+beginsec+461]);
	fft_forward(128,a,NULL,NULL);
	fft_forward(128,b,NULL,NULL);
	for(j=0;j<128;j++)lsb_energy+=fabs(a[j])+fabs(b[j]);
	
	for(j=0;j<128;j++)a[j]=be16_to_cpu(buff[j*2+beginsec+460]);
	for(j=0;j<128;j++)b[j]=be16_to_cpu(buff[j*2+beginsec+461]);
	fft_forward(128,a,NULL,NULL);
	fft_forward(128,b,NULL,NULL);
	for(j=0;j<128;j++)msb_energy+=fabs(a[j])+fabs(b[j]);
      }
    }
    if(lsb_energy<msb_energy){
      lsb_votes+=msb_energy/lsb_energy;
      checked++;
    }else
      if(lsb_energy>msb_energy){
	msb_votes+=lsb_energy/msb_energy;
	checked++;
      }

    if(checked==5 && (lsb_votes==0 || msb_votes==0))break;
    cdmessage(d,".");
  }

  free(buff);
  free(a);
  free(b);
  d->bigendianp=endiancache;
  d->enable_cdda(d,0);

  /* How did we vote?  Be potentially noisy */
  if(lsb_votes>msb_votes){
    char buffer[256];
    cdmessage(d,"\n\tData appears to be coming back little endian.\n");
    sprintf(buffer,"\tcertainty: %d%%\n",(int)
	    (100.*lsb_votes/(lsb_votes+msb_votes)+.5));
    cdmessage(d,buffer);
    return(0);
  }else{
    if(msb_votes>lsb_votes){
      char buffer[256];
      cdmessage(d,"\n\tData appears to be coming back big endian.\n");
      sprintf(buffer,"\tcertainty: %d%%\n",(int)
	      (100.*msb_votes/(lsb_votes+msb_votes)+.5));
      cdmessage(d,buffer);
      return(1);
    }

    cdmessage(d,"\n\tCannot determine CDROM drive endianness.\n");
    return(bigendianp());
    return(-1);
  }
}

/* we can ask most drives what their various caches' sizes are, but no
   drive will tell if it caches redbook data.  None should, many do,
   and there's no way in (eg) MMAC/ATAPI to tell a drive not to.  SCSI
   drives have a FUA facility, but it's not clear how many ignore it.
   For that reason, we need to empirically determine cache size used
   for reads */

int cdda_cache_sectors(cdrom_drive *d){

  /* Some assumptions about timing: 

     The physically fastest drives are about 50x, and usually only at
     the rim.  This has been stable for nearly ten years at this
     point.  It's possible to make faster drives using multiple read
     pickups and interleaving, but it doesn't appear anyone cares to.

     The slowest interface we're likely to see is UDMA40, which would
     probably be able to maintain 100x in practice to a chain with a
     single device on an otherwise unloaded machine.  This is a bit
     too close for comfort to rely on simple timing thresholding,
     especially as the kernel is going to be inserting its own
     unpredictable timing latencies.

     The bus itself adds a timing overhead; the SATA 150 machines at my disposal appear to fairly universally insert a roughly .06ms/sector (~200x) transfer latency. PATA UDMA 133 appears to be ~ .1 ms/sector.

It's possible to make
     drives faster (multiread, etc), but actual bus throughput at the
     moment only abounts to 100x-200x,

  /* let's assume the (physically) fastest drives are 60x; this is true in practice, and drives that fast are usually only that fast out at the rim */
  float ms_per_sector = 1./75./100.*1000;
  int i;
  int lo = 75;
  int current = lo;
  int max = 75*256;
  int firstsector=-1;
  int lastsector=-1;
  int firsttest=-1;
  int lasttest=-1;
  int under=1;

  cdmessage(d,"\nChecking CDROM drive cache behavior...\n");

  /* find the longest stretch of available audio data */

  for(i=0;i<d->tracks;i++){
    if(cdda_track_audiop(d,i+1)==1){
      if(firsttest == -1)
	firsttest=cdda_track_firstsector(d,i+1);
      lasttest=cdda_track_lastsector(d,i+1);
      if(lasttest-firsttest > lastsector-firstsector){
	firstsector=firsttest;
	lastsector=lasttest;
      }
    }else{
      firsttest=-1;
      lasttest=-1;
    }
  }

  if(firstsector==-1){
    cdmessage(d,"\n\tNo audio on disc; Cannot determine audio caching behavior.\n");
    return -1;
  }

  while(current <= max && under){
    int offset = (lastsector - firstsector - current)/2; 
    int i,j;
    under=0;

    {
      char buffer[80];
      snprintf(buffer,80,"\tTesting reads for caching (%d sectors):\n\t",current);
      cdmessage(d,buffer);
    }

    for(i=0;i<10;i++){
      int sofar=0;
      int fulltime=0;
      
      while(sofar<current){
	for(j=0;;j++){
	  
	  int readsectors = d->read_audio(d,NULL,offset+sofar,current-sofar);
	  if(readsectors<=0){
	    if(j==2){
	      d->enable_cdda(d,0);
	      cdmessage(d,"\n\tRead error while performing drive cache checks; aborting test.\n");
	      return(-1);
	    }
	  }else{
	    sofar+=readsectors;
	    if(d->private->last_milliseconds==-1){
	      if(j==2){
		d->enable_cdda(d,0);
		cdmessage(d,"\n\tTiming error while performing drive cache checks; aborting test.\n");
		return(-1);
	      }
	    }else{

	      fprintf(stderr,">%d:%dms ",readsectors, d->private->last_milliseconds);
	      fulltime += d->private->last_milliseconds;
	      break;
	    }
	  }
	}
      }
      {
	char buffer[80];
	snprintf(buffer,80," %d:%fms/sec",i,(float)fulltime/current);
	cdmessage(d,buffer);
      }
      if((float)fulltime/current < ms_per_sector) under=1;
    }
    cdmessage(d,"\n");

    current*=2;
  } 

#if 0
  if(current > max){
    cdmessage(d,"\nGiving up; drive cache is too large to defeat using overflow.\n");
    cdmessage(d,"\n(Drives should not cache redbook reads, this drive does anyway."
	        "\n Worse, the cache is too large to have any hope of defeating."
	        "\n Cdparanoia has no chance of catching errors from this drive.\n");

    return INT_MAX;
  }
#endif
  return 0;
}


/************************************************************************/
/* Here we fix up a couple of things that will never happen.  yeah,
   right.  The multisession stuff is from Hannu's code; it assumes it
   knows the leadout/leadin size. */

int FixupTOC(cdrom_drive *d,int tracks){
  struct cdrom_multisession ms_str;
  int j;
  
  /* First off, make sure the 'starting sector' is >=0 */
  
  for(j=0;j<tracks;j++){
    if(d->disc_toc[j].dwStartSector<0){
      cdmessage(d,"\n\tTOC entry claims a negative start offset: massaging"
		".\n");
      d->disc_toc[j].dwStartSector=0;
    }
    if(j<tracks-1 && d->disc_toc[j].dwStartSector>
       d->disc_toc[j+1].dwStartSector){
      cdmessage(d,"\n\tTOC entry claims an overly large start offset: massaging"
		".\n");
      d->disc_toc[j].dwStartSector=0;
    }

  }
  /* Make sure the listed 'starting sectors' are actually increasing.
     Flag things that are blatant/stupid/wrong */
  {
    long last=d->disc_toc[0].dwStartSector;
    for(j=1;j<tracks;j++){
      if(d->disc_toc[j].dwStartSector<last){
	cdmessage(d,"\n\tTOC entries claim non-increasing offsets: massaging"
		  ".\n");
	 d->disc_toc[j].dwStartSector=last;
	
      }
      last=d->disc_toc[j].dwStartSector;
    }
  }

  /* For a scsi device, the ioctl must go to the specialized SCSI
     CDROM device, not the generic device. */

  if (d->ioctl_fd != -1) {
    int result;

    ms_str.addr_format = CDROM_LBA;
    result = ioctl(d->ioctl_fd, CDROMMULTISESSION, &ms_str);
    if (result == -1) return -1;

    if (ms_str.addr.lba > 100) {

      /* This is an odd little piece of code --Monty */

      /* believe the multisession offset :-) */
      /* adjust end of last audio track to be in the first session */
      for (j = tracks-1; j >= 0; j--) {
	if (j > 0 && !IS_AUDIO(d,j) && IS_AUDIO(d,j-1)) {
	  if ((d->disc_toc[j].dwStartSector > ms_str.addr.lba - 11400) &&
	      (ms_str.addr.lba - 11400 > d->disc_toc[j-1].dwStartSector))
	    d->disc_toc[j].dwStartSector = ms_str.addr.lba - 11400;
	  break;
	}
      }
      return 1;
    }
  }
  return 0;
}


