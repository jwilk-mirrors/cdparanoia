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

int analyze_timing_and_cache(cdrom_drive *d){

  /* Some assumptions about timing: 

     We can't perform cache determination timing based on looking at
     average transfer times; on slow setups, the speed of a drive
     reading sectors via PIO will not be reliably distinguishable from
     the same drive returning data from the cache via pio.  We need
     something even more noticable and reliable: the seek time.  A
     seek will reliably be approximately 1.5 orders of magnitude
     faster than a sequential sector access or cache hit, and slower
     systems will also tend to have slower seeks.  It is unlikely we'd
     ever see a seek latency of under ~10ms given the synchronization
     requirements of a CD and the maximum possible rotational
     velocity.

     Further complicating things, we have to watch the data collection
     carefully as we're not always going to be on an unloaded system,
     and we even have to guard against other apps accessing the drive
     (something that should never happen on purpose, but could happen
     by accident).  As we know in our testing when seeks should never
     occur, a sudden seek-sized latency popping up in the middle of a
     collection is an indication that collection is possibly invalid.

     A second cause of 'spurious latency' would be media damage; if
     we're consistently hitting latency on the same sector during
     initial collection, may need to move past it. */

  int i,ret;
  int firstsector=-1;
  int lastsector=-1;
  int firsttest=-1;
  int lasttest=-1;
  char buffer[80];
  int max_retries=20;
  float median;
  int offset;
  int debug = d->private->cache_debug;

  /* set up a default pessimal take on drive behavior */
  d->private->cache_backseekflush=0;
  d->private->cache_sectors=1200;

  cdmessage(d,"\nChecking drive timing behavior...");

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
    cdmessage(d,"\n\tNo audio on disc; Cannot determine timing behavior...");
    return -1;
  }

  /* initial timing data collection of 100 sequential sectors; we need a median, verify an initial seek */
  {
    int x;
    int current=100;
    int histogram[10000];
    int latency[current];
    int retry;
    offset = (lastsector - firstsector - current)*2/3 + firstsector;

    for(retry=0;retry<max_retries;retry++){
      int acc=0;
      int prev=0;

      if(retry){
	offset-=current+1;
	offset-=offset/32;
      }
      if(offset<firstsector)break;
      
      memset(histogram,0,sizeof(histogram));
      if((ret=d->read_audio(d,NULL,offset+current+1,1))<0){
	/* media error! grr!  retry elsewhere */
	cdmessage(d,"\n\tWARNING: media error; picking new location and trying again.");
	continue;
      }

      if(debug)
	cdmessage(d,"\n\tSector timings (ms):\n\t");

      for(i=0;i<current;i++){
	if(d->read_audio(d,NULL,offset+i,1)<0){
	  /* media error! grr!  retry elsewhere */
	  cdmessage(d,"\n\tWARNING: media error; picking new location and trying again.");
	  break;
	}
	x = d->private->last_milliseconds;
	if(x>9999)x=9999;
	if(x<0)x=0;
	if(debug){
	  snprintf(buffer,80,"%d ",x);
	  cdmessage(d,buffer);
	}

	histogram[x]++;
	latency[i]=x;
      }
      if(i<current){
	offset-=current+1;
	continue;
      }	

      for(i=0;i<10000;i++){
	prev=acc;
	acc+=histogram[i];
	if(acc>current/2){
	  if(debug){
	    cdmessage(d,"\n\tSurrounding histogram: ");
	    if(i){
	      snprintf(buffer,80,"%dms:%d ",i-1,acc-histogram[i]);
	      cdmessage(d,buffer);
	    }
	    snprintf(buffer,80,"%dms:%d ",i,acc);
	    cdmessage(d,buffer);
	    if(i<999){
	      snprintf(buffer,80,"%dms:%d ",i+1,acc+histogram[i+1]);
	      cdmessage(d,buffer);
	    }
	    cdmessage(d,"\n");
	  }
	  break;
	}
      }

      median = (i*(acc-prev) + (i-1)*prev)/(float)acc;
      
      if(debug){
	snprintf(buffer,80,"\n\tsmall seek latency (%d sectors): %d ms",current,latency[0]);
	cdmessage(d,buffer);
	snprintf(buffer,80,"\n\tmedian read latency per sector: %.1f ms",median);
	cdmessage(d,buffer);
      }

      /* verify slow spinup did not compromise median */
      for(i=1;i<current;i++)
	if(latency[i]>latency[i-1] || latency[i]<=(median+1.))break;
      if(i>5){
	if(debug)
	  cdmessage(d,"\n\tDrive appears to spin up slowly... retrying...");
	offset-=current+1;
	continue;
      }

      /* verify against spurious latency; any additional 5x blocks that
	 are not continuous with read start */
      acc=0;
      if(median<.6)median=.6;
      for(i=5;i<current;i++)
	if(latency[i]>median*10)acc++;

      if(acc){
	cderror(d,"\n\tWARNING: Read timing displayed bursts of unexpected"
		"\n\tlatency; retrying for a clean read.\n");
	continue;
      }
	
      break;
    }

    if(offset<firstsector){
      cderror(d,"\n500: Unable to find sufficiently large area of"
	      "\n\tgood media to perform timing tests.  Aborting.\n");
      return -500;
    }
    if(retry==max_retries){
      cderror(d,"\n500: Too many retries; aborting analysis.\n");
      return -500;
    }    
  }
  
  /* look to see if drive is caching at all; read first sector N
     times, if any reads are near or under the median latency, we're
     caching */
  {
    for(i=0;i<max_retries;i++){
      if(d->read_audio(d,NULL,offset,1)==1){
	if(d->private->last_milliseconds<median*10) break;

      }else{
	/* error handling */
      }
    }

    if(i<max_retries){
      cdmessage(d,"\n\tCaching test result: DRIVE IS CACHING (bad)\n");
    }else{
      cdmessage(d,"\n\tCaching test result: Drive is not caching (good)\n");
      d->private->cache_sectors=0;
      return 0;
    }
  }




      


  /* bisection search on cache size */

  int lo=1;
  int hi=15000;
  int current=lo;
  int under=1;
  d->nsectors=1;
  while(current <= hi && under){
    int offset = (lastsector - firstsector - (current+1))/2+firstsector; 
    int i,j;
    under=0;

    {
      char buffer[80];
      snprintf(buffer,80,"\n\tTesting reads for caching (%d sectors):\n\t",current);
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
	    if(d->private->last_milliseconds==-1){
	      if(j==2){
		d->enable_cdda(d,0);
		cdmessage(d,"\n\tTiming error while performing drive cache checks; aborting test.\n");
		return(-1);
	      }
	    }else{

	      if(sofar==0){
		fprintf(stderr,">%d:%dms ",readsectors, d->private->last_milliseconds);
		fulltime = d->private->last_milliseconds;
	      }
	      sofar+=readsectors;
	      break;
	    }
	  }
	}
      }
      if(fulltime < median*10) under=1;
    }
    cdmessage(d,"\n");

    current*=2;
  } 



   

  /* XXXXXX IN PROGRESS */
  cdmessage(d,"\n");
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


