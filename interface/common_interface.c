/******************************************************************
 * CopyPolicy: GNU Public License 2 applies
 * Copyright (C) 1998 Monty xiphmont@mit.edu
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
int ioctl_ping_cdrom(int fd){
  struct cdrom_volctrl volctl;
  if (ioctl(fd, CDROMVOLREAD, &volctl)) 
    return(1); /* failure */

  return(0);
  /* success! */
}


/* Use the ioctl thingy above ping the cdrom; this will get model info */
char *atapi_drive_info(int fd){
  static struct hd_driveid id;

  if (!(ioctl(fd, HDIO_GET_IDENTITY, &id))) {

    if(id.model==0 || id.model[0]==0)
      return(copystring("Generic Unidentifiable ATAPI CDROM"));
    else
      return(copystring(id.model));
  }
  return(copystring("Generic Unidentifiable CDROM"));
}

int data_bigendianp(cdrom_drive *d){
  float lsb_votes=0;
  float msb_votes=0;
  int i,checked;
  int endiancache=d->bigendianp;
  float *a=calloc(1024,sizeof(float));
  float *b=calloc(1024,sizeof(float));
  long readsectors=5;
  size16 *buff=malloc(readsectors*CD_FRAMESIZE_RAW);

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
	  return(-1);
	}
      }

      beginsec*=CD_FRAMESIZE_RAW/2;
      
      /* un-interleave for an fft */
      if(!zeroflag){
	int j;
	
	for(j=0;j<128;j++)a[j]=le16_to_cpu(buff[j*2+beginsec+460]);
	for(j=0;j<128;j++)b[j]=le16_to_cpu(buff[j*2+beginsec+461]);
	fft_forward(128,a);
	fft_forward(128,b);
	for(j=0;j<128;j++)lsb_energy+=fabs(a[j])+fabs(b[j]);
	
	for(j=0;j<128;j++)a[j]=be16_to_cpu(buff[j*2+beginsec+460]);
	for(j=0;j<128;j++)b[j]=be16_to_cpu(buff[j*2+beginsec+461]);
	fft_forward(128,a);
	fft_forward(128,b);
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

/************************************************************************/

int FixupTOC(cdrom_drive *d,int tracks){
  struct cdrom_multisession ms_str;

  /* For a scsi device, the ioctl must got to the specialized SCSI
     CDROM device, not the generic device. */

  if (d->ioctl_fd != -1) {
    int result;

    ms_str.addr_format = CDROM_LBA;
    result = ioctl(d->ioctl_fd, CDROMMULTISESSION, &ms_str);
    if (result == -1) return -1;

    if (ms_str.addr.lba > 100) {
      int j;

      /* This is an odd little piece of code --Monty */

      /* believe the multisession offset :-) */
      /* adjust end of last audio track to be in the first session */
      for (j = tracks-1; j >= 0; j--) {
	if (j > 0 && !IS_AUDIO(d,j) && IS_AUDIO(d,j-1)) {
	  if (d->disc_toc[j].dwStartSector > ms_str.addr.lba - 11400) 
	    d->disc_toc[j].dwStartSector = ms_str.addr.lba - 11400;
	  break;
	}
      }
      return 1;
    }
  }
  return 0;
}


