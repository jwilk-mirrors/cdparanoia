/******************************************************************
 * CopyPolicy: GNU Public License 2 applies
 * Copyright (C) Monty xiphmont@mit.edu
 *
 * CDROM code specific to the cooked ioctl interface
 *
 ******************************************************************/

#include "low_interface.h"
#include "common_interface.h"
#include "utils.h"

static int cooked_readtoc (cdrom_drive *d){
  int i;
  int tracks;
  struct cdrom_tochdr hdr;
  struct cdrom_tocentry entry;

  /* get TocHeader to find out how many entries there are */
  if(ioctl(d->ioctl_fd, CDROMREADTOCHDR, &hdr ))
    switch(errno){
    case EPERM:
      cderror(d,"102: Permision denied on cdrom (ioctl) device\n");
      return(-1);
    default:
      cderror(d,"004: Unable to read table of contents header\n");
      return(-1);
    }

  /* get all TocEntries */
  for(i=0;i<hdr.cdth_trk1;i++){
    entry.cdte_track= i+1;
    entry.cdte_format = CDROM_LBA;
    if(ioctl(d->ioctl_fd,CDROMREADTOCENTRY,&entry)){
      cderror(d,"005: Unable to read table of contents entry\n");
      return(-1);
    }
      
    d->disc_toc[i].bFlags = (entry.cdte_adr << 4) | (entry.cdte_ctrl & 0x0f);
    d->disc_toc[i].bTrack = i+1;
    d->disc_toc[i].dwStartSector = entry.cdte_addr.lba;
  }

  entry.cdte_track = CDROM_LEADOUT;
  entry.cdte_format = CDROM_LBA;
  if(ioctl(d->ioctl_fd, CDROMREADTOCENTRY, &entry)){
    cderror(d,"005: Unable to read table of contents entry\n");
    return(-1);
  }
  d->disc_toc[i].bFlags = (entry.cdte_adr << 4) | (entry.cdte_ctrl & 0x0f);
  d->disc_toc[i].bTrack = entry.cdte_track;
  d->disc_toc[i].dwStartSector = entry.cdte_addr.lba;

  tracks=hdr.cdth_trk1+1;
  d->cd_extra=FixupTOC(d,tracks);
  return(--tracks);  /* without lead-out */
}

/* read 'SectorBurst' adjacent sectors of audio sectors 
 * to Buffer '*p' beginning at sector 'lSector'
 */

static long cooked_read (cdrom_drive *d, void *p, long begin, long sectors){
  int retry_count,err;
  struct cdrom_read_audio arg;
  char *buffer=(char *)p;

  /* read d->nsectors at a time, max. */
  sectors=(sectors>d->nsectors?d->nsectors:sectors);

  arg.addr.lba = begin;
  arg.addr_format = CDROM_LBA;
  arg.nframes = sectors;
  arg.buf=buffer;
  retry_count=0;

  do {
    if((err=ioctl(d->ioctl_fd, CDROMREADAUDIO, &arg))){
      switch(errno){
      case ENOMEM:
	/* D'oh.  Possible kernel error. Keep limping */
	if(sectors==1){
	  /* Nope, can't continue */
	  cderror(d,"300: Kernel memory error\n");
	  return(-1);  
	}
      default:
      case EIO:
      case EINVAL:
	if(sectors==1){
	  if(d->nothing_read){
	    /* Can't read the *first* sector?! Ouch! */
	    cderror(d,"006: Could not read any data from drive\n");
	    return(-1);
	  }
	  if(errno==EIO){
	    /* *Could* be I/O or media error.  I think.  If we're at
	       30 retries, we better skip this unhappy little
	       sector. */
	    if(retry_count==MAX_RETRIES-1){
	      /* OK, skip.  We need to make the scratch code pick
		 up the blank sector tho. */
	      char b[256];
	      sprintf(b,"Unable to find sector %ld: skipping...\n",
		      begin);
	      cdmessage(d,b);
	      memset(arg.buf,-1,CD_FRAMESIZE_RAW);
	      err=0;
	    }
	    break;
	  }
	  /* OK, ok, bail. */
	  cderror(d,"007: Unknown, unrecoverable error reading data\n");
	  return(-1);
	}
      }
      if(retry_count>4==0)
	if(sectors>1)
	  sectors>>=1;
      retry_count++;
      if(retry_count>MAX_RETRIES){
	cderror(d,"007: Unknown, unrecoverable error reading data\n");
	return(-1);
      }
    }else
      break;
  } while (err);
  
  d->nothing_read=0;
  return(sectors);
}

/* Speed control */
static int cooked_speed(cdrom_drive *d,int speed){

  if (ioctl(d->ioctl_fd, CDROM_SELECT_SPEED, &speed)){
    switch (errno){
    case EPERM:
      cderror(d,"102: Permision denied on cdrom (ioctl) device\n");
      return(1);
    default:
      cderror(d,"201: Speed select failed\n");
      return(1);
    }
  }
  return(0);

}

/* hook */
static int Dummy (cdrom_drive *d,int Switch){
  return(0);
}

/* set function pointers to use the ioctl routines */
int cooked_init_drive (cdrom_drive *d){
  int i;

  switch(d->drive_type){
  case MATSUSHITA_CDROM_MAJOR:	/* sbpcd 1 */
  case MATSUSHITA_CDROM2_MAJOR:	/* sbpcd 2 */
  case MATSUSHITA_CDROM3_MAJOR:	/* sbpcd 3 */
  case MATSUSHITA_CDROM4_MAJOR:	/* sbpcd 4 */
    /* Our driver isn't very smart; don't make the buffer too big. */
    i=25;
    
    while(ioctl(d->ioctl_fd, CDROMAUDIOBUFSIZ, d->nsectors)){
      d->nsectors>>=1;
      if(d->nsectors==0){
	d->nsectors=10;
	break; /* Oh, well.  Try to read anyway.*/
      }
    }
    break;
  case IDE0_MAJOR:
  case IDE1_MAJOR:
  case IDE2_MAJOR:
  case IDE3_MAJOR:
    d->nsectors=8; /* it's a define in the linux kernel; we have no
		      way of determining other than this guess tho */
    d->bigendianp=0;
    break;
  default:
    d->nsectors=40; 
  }
  d->enable_cdda = Dummy;
  d->read_audio = cooked_read;
  d->read_toc = cooked_readtoc;
  d->select_speed = cooked_speed;
  d->tracks=d->read_toc(d);
  if(d->tracks==-1)
    return(1);
  d->opened=1;
  return(0);
}

