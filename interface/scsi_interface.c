/******************************************************************
 * CopyPolicy: GNU Public License 2 applies
 * Original interface.c Copyright (C) 1994-1997 
 *            Eissfeldt heiko@colossus.escape.de
 * Current blenderization Copyright (C) 1998 Monty xiphmont@mit.edu
 * 
 * Generic SCSI interface specific code.
 *
 ******************************************************************/

#include "low_interface.h"
#include "common_interface.h"
#include "utils.h"

/* hook */
static int Dummy (cdrom_drive *d,int s){
  return(0);
}

typedef struct exception {
  char *model;
  int atapi; /* If the ioctl doesn't work */
  unsigned char density;
  int  (*enable)(struct cdrom_drive *,int);
  long (*read)(struct cdrom_drive *,void *,long,long);
  int  bigendianp;
} exception;

static void find_bloody_big_buff_size(cdrom_drive *d){

  /* find the biggest read command that succeeds.  This should be
     safe; the kernel will reject requests that are too big. */

  long begin=cdda_disc_firstsector(d);
  long cur=MAX_BIG_BUFF_SIZE/CD_FRAMESIZE_RAW;
  cdmessage(d,"\nAttempting to autosense SG_BIG_BUFF size...\n");

  d->enable_cdda(d,1);
  if(begin==-1){
    cur=1; /* 4096 is hardwired into linux/drivers/scsi/sg.c */
  }else{

    /* bisection is not fastest here; requests that are too big are 
       rejected quickly */
    while(cur>1){
      d->nsectors=cur;
      
      if(d->read_audio(d,NULL,begin,cur)>0)break;
      cur--;
    }
  }
  d->enable_cdda(d,0);
  {
    char buffer[256];
    sprintf(buffer,"\tSetting read block size at %ld sectors (%ld bytes).\n",
	    cur,cur*CD_FRAMESIZE_RAW);
    cdmessage(d,buffer);
  }
  d->nsectors=cur;
  d->bigbuff=cur*CD_FRAMESIZE_RAW;

}

/* process a complete scsi command. */
static int handle_scsi_cmd(cdrom_drive *d,
			   unsigned int cmd_len, 
			   unsigned int in_size, 
			   unsigned int out_size){
  int status = 0;
  struct sg_header *sg_hd=(struct sg_header *)d->sg;
  long writebytes=SG_OFF+cmd_len+in_size;

  /* generic scsi device services */
  sg_hd->reply_len = SG_OFF + out_size;
  sg_hd->twelve_byte = cmd_len == 12;
  sg_hd->result = 0;

  if(d->clear_buff_via_bug){
    if(out_size>in_size){
      memset(d->sg_buffer+cmd_len+in_size,0377,out_size-in_size);
      writebytes+=(out_size-in_size);
    }
  }

  sigprocmask (SIG_BLOCK, &(d->sigset), NULL );
  status = write(d->cdda_fd, sg_hd, writebytes );
  if (status<0 || status != writebytes ) {
    sigprocmask ( SIG_UNBLOCK, &(d->sigset), NULL );
    return(1);
  }
  
  status = read(d->cdda_fd, sg_hd, SG_OFF + out_size);
  sigprocmask ( SIG_UNBLOCK, &(d->sigset), NULL );

  if (status<0 || status != SG_OFF + out_size || sg_hd->result) return(1);
  if(d->clear_buff_via_bug && in_size<out_size){
    long i,flag=0;
    for(i=in_size;i<out_size;i++)
      if(d->sg_buffer[i]!=(unsigned char)'\377'){
	flag=1;
	break;
      }

    if(!flag)return(1);
  }

  /* Look if we got what we expected to get */
  if (status == SG_OFF + out_size) status = 0; /* got them all */
  return status;
}

/* Group 1 (10b) command */

static int mode_sense_atapi(cdrom_drive *d,int size,int page){ 
  memcpy(d->sg_buffer,  
	 (char [])  {0x5A,   /* MODE_SENSE */
		       0x00, /* reserved */
		       0x00, /* page */
		       0,    /* reserved */
		       0,    /* reserved */
		       0,    /* reserved */
		       0,    /* reserved */
		       0,    /* MSB (0) */
		       0,    /* sizeof(modesense - SG_OFF) */
		       0},   /* reserved */ 
         10);
  
  d->sg_buffer[2]=0x3F&page;
  d->sg_buffer[8]=size+4;

  if (handle_scsi_cmd (d, 10, 0, size+4)) return(1);

  {
    char *b=d->sg_buffer;
    if(b[0])return(1); /* Handles only up to 256 bytes */
    if(b[6])return(1); /* Handles only up to 256 bytes */

    b[0]=b[1]-3;
    b[1]=b[2];
    b[2]=b[3];
    b[3]=b[7];

    memmove(b+4,b+8,size);
  }
  return(0);
}

/* group 0 (6b) command */

static int mode_sense_scsi(cdrom_drive *d,int size,int page){  
  memcpy(d->sg_buffer,  
	 (char [])  {0x1A,   /* MODE_SENSE */
		       0x00, /* return block descriptor */
		       0x00, /* page */
		       0,    /* reserved */
		       0,   /* sizeof(modesense - SG_OFF) */
		       0},   /* control */ 
         6);
  
  d->sg_buffer[2]=(0x3F&page);
  d->sg_buffer[4]=size;

  if (handle_scsi_cmd (d, 6, 0, size)) return(1);
  return(0);
}

static int mode_sense(cdrom_drive *d,int size,int page){
  if(d->is_atapi)
    return(mode_sense_atapi(d,size,page));
  return(mode_sense_scsi(d,size,page));
}

static int mode_select(cdrom_drive *d,int density,int secsize){
  /* short circut the way Heiko does it; less flexible, but shorter */
  if(d->is_atapi){
    unsigned char *mode = d->sg_buffer + 18;

    memcpy(d->sg_buffer,
	   (char []) { 0x55, /* MODE_SELECT */
			 0x10, /* no save page */
			 0, /* reserved */
			 0, /* reserved */
			 0, /* reserved */
			 0, /* reserved */
			 0, /* reserved */
			 0, /* reserved */
			 12, /* sizeof(mode) */
			 0, /* reserved */

			 /* mode parameter header */
			 0, 0, 0, 0,  0, 0, 0, 
			 8, /* Block Descriptor Length */

			 /* descriptor block */
			 0,       /* Density Code */
			 0, 0, 0, /* # of Blocks */
			 0,       /* reserved */
			 0, 0, 0},/* Blocklen */
	   26);

    /* prepare to read cds in the previous mode */
    mode [0] = density;
    mode [6] =  secsize >> 8;   /* block length "msb" */
    mode [7] =  secsize & 0xFF; /* block length lsb */

    /* do the scsi cmd */
    return(handle_scsi_cmd (d,10, 16, 0));

  }else{
    unsigned char *mode = d->sg_buffer + 10;

    memcpy(d->sg_buffer,
	   (char []) { 0x15, /* MODE_SELECT */
			 0x10, /* no save page */
			 0, /* reserved */
			 0, /* reserved */
			 12, /* sizeof(mode) */
			 0, /* reserved */
			 /* mode section */
			 0, 
			 0, 0, 
			 8,       /* Block Descriptor Length */
			 0,       /* Density Code */
			 0, 0, 0, /* # of Blocks */
			 0,       /* reserved */
			 0, 0, 0},/* Blocklen */
	   18);

    /* prepare to read cds in the previous mode */
    mode [0] = density;
    mode [6] =  secsize >> 8;   /* block length "msb" */
    mode [7] =  secsize & 0xFF; /* block length lsb */

    /* do the scsi cmd */
    return(handle_scsi_cmd (d,6, 12, 0));
  }
}

/* get current sector size from SCSI cdrom drive */
static unsigned int get_orig_sectorsize(cdrom_drive *d){
  if(mode_sense(d,12,0x01))return(-1);

  d->orgdens = d->sg_buffer[4];
  return(d->orgsize = ((int)(d->sg_buffer[10])<<8)+d->sg_buffer[11]);
}

/* switch CDROM scsi drives to given sector size  */
static int set_sectorsize (cdrom_drive *d,unsigned int secsize){
  return(mode_select(d,d->orgdens,secsize));
}

/* switch Toshiba/DEC and HP drives from/to cdda density */
static int scsi_enable_cdda (cdrom_drive *d, int fAudioMode){
  if (fAudioMode) {
    if(mode_select(d,d->density,CD_FRAMESIZE_RAW)){
      if(d->error_retry)
	cderror(d,"001: Unable to set CDROM to read audio mode\n");
      return(-1);
    }
  } else {
    if(mode_select(d,d->orgdens,d->orgsize)){
      if(d->error_retry)
	cderror(d,"001: Unable to set CDROM to read audio mode\n");
      return(-1);
    }
  }
  return(0);
}

typedef struct scsi_TOC {  /* structure of scsi table of contents (cdrom) */
  unsigned char reserved1;
  unsigned char bFlags;
  unsigned char bTrack;
  unsigned char reserved2;
  u_int32_t dwStartSector;
} scsi_TOC;


/* read the table of contents from the cd and fill the TOC array */
static int scsi_read_toc (cdrom_drive *d){
  int i;
  unsigned tracks;

  /* READTOC, MSF format flag, res, res, res, res, Start track, len msb,
     len lsb, flags */

  memcpy(d->sg_buffer, 
	 (char []){ 0x43, 0, 0, 0, 0, 0, 1, CD_FRAMESIZE >> 8, 
		      CD_FRAMESIZE & 0xFF, 0},
	 10);

  /* do the scsi cmd (read table of contents) */
  if (handle_scsi_cmd (d,10, 0, CD_FRAMESIZE)){
    cderror(d,"002: Unable to read table of contents\n");
    return(-2);
  }

  /* copy to our structure and convert start sector */
  tracks = ((d->sg_buffer[0] << 8) + d->sg_buffer[1] - 2) / 8;
  if (tracks > MAXTRK) {
    cderror(d,"003: CDROM reporting illegal number of tracks\n");
    return(-3);
  }

  for (i = 0; i < tracks; i++) {
    scsi_TOC *toc=(scsi_TOC *)(d->sg_buffer+4+8*i);

    d->disc_toc[i].bFlags=toc->bFlags;
    d->disc_toc[i].bTrack=i+1;
    d->disc_toc[i].dwStartSector= d->adjust_ssize * 
      be32_to_cpu(toc->dwStartSector);
  }
  d->cd_extra = FixupTOC(d,tracks);
  return(--tracks);           /* without lead-out */
}

/* a contribution from Boris for IMS cdd 522 */
static int scsi_read_toc2 (cdrom_drive *d){
  unsigned size32 foo,bar;

  int i;
  unsigned tracks;

  memcpy(d->sg_buffer, (char[]){ 0xe5, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 10);
  d->sg_buffer[5]=1;
  d->sg_buffer[8]=255;

  if (handle_scsi_cmd (d,10, 0, 256)){
    cderror(d,"004: Unable to read table of contents header\n");
    return(-4);
  }

  /* copy to our structure and convert start sector */
  tracks = d->sg_buffer[1];
  if (tracks > MAXTRK) {
    cderror(d,"003: CDROM reporting illegal number of tracks\n");
    return(-3);
  }

  for (i = 0; i < tracks; i++){

      memcpy(d->sg_buffer, (char[]){ 0xe5, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 10);
      d->sg_buffer[5]=i+1;
      d->sg_buffer[8]=255;

      if (handle_scsi_cmd (d,10, 0, 256)){
	cderror(d,"005: Unable to read table of contents entry\n");
	return(-5);
      }

      d->disc_toc[i].bFlags = d->sg_buffer[10];
      d->disc_toc[i].bTrack = i + 1;
      memcpy (&foo, d->sg_buffer+2, 4);
      d->disc_toc[i].dwStartSector = d->adjust_ssize * be32_to_cpu(foo);
    }

  d->disc_toc[i].bFlags = 0;
  d->disc_toc[i].bTrack = i + 1;
  memcpy (&foo, d->sg_buffer+2, 4);
  memcpy (&bar, d->sg_buffer+6, 4);
  d->disc_toc[i].dwStartSector = d->adjust_ssize * (be32_to_cpu(foo) +
						    be32_to_cpu(bar));

  d->cd_extra = FixupTOC(d,tracks+1);
  return(tracks);
}

/* These do one 'extra' copy in the name of clean code */

static int i_read_10 (cdrom_drive *d, void *p, long begin, long sectors){
  memcpy(d->sg_buffer,(char []){0x28, 0, 0, 0, 0, 0, 0, 0, 0, 0},10);

  if(d->fua)
    d->sg_buffer[1]=0x08;

  d->sg_buffer[3] = (begin >> 16) & 0xFF;
  d->sg_buffer[4] = (begin >> 8) & 0xFF;
  d->sg_buffer[5] = begin & 0xFF;
  d->sg_buffer[8] = sectors;
  if(handle_scsi_cmd(d,10,0,sectors * CD_FRAMESIZE_RAW))
    return(1);
  if(p)memcpy(p,d->sg_buffer,sectors*CD_FRAMESIZE_RAW);
  return(0);
}

static int i_read_12 (cdrom_drive *d, void *p, long begin, long sectors){
  memcpy(d->sg_buffer,(char []){0xA8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},12);

  if(d->fua)
    d->sg_buffer[1]=0x08;

  d->sg_buffer[3] = (begin >> 16) & 0xFF;
  d->sg_buffer[4] = (begin >> 8) & 0xFF;
  d->sg_buffer[5] = begin & 0xFF;
  d->sg_buffer[9] = sectors;
  if(handle_scsi_cmd(d,12,0,sectors * CD_FRAMESIZE_RAW))
    return(1);
  if(p)memcpy(p,d->sg_buffer,sectors*CD_FRAMESIZE_RAW);
  return(0);
}

static int i_read_nec (cdrom_drive *d, void *p, long begin, long sectors){
  memcpy(d->sg_buffer,(char []){0xd4, 0, 0, 0, 0, 0, 0, 0, 0, 0},10);

  if(d->fua)
    d->sg_buffer[1]=0x08;

  d->sg_buffer[3] = (begin >> 16) & 0xFF;
  d->sg_buffer[4] = (begin >> 8) & 0xFF;
  d->sg_buffer[5] = begin & 0xFF;
  d->sg_buffer[8] = sectors;
  if(handle_scsi_cmd(d,10,0,sectors * CD_FRAMESIZE_RAW))
    return(1);
  if(p)memcpy(p,d->sg_buffer,sectors*CD_FRAMESIZE_RAW);
  return(0);
}

static int i_read_sony (cdrom_drive *d, void *p, long begin, long sectors){
  memcpy(d->sg_buffer,(char []){0xd8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},12);

  if(d->fua)
    d->sg_buffer[1]=0x08;

  d->sg_buffer[3] = (begin >> 16) & 0xFF;
  d->sg_buffer[4] = (begin >> 8) & 0xFF;
  d->sg_buffer[5] = begin & 0xFF;
  d->sg_buffer[9] = sectors;
  if(handle_scsi_cmd(d,12,0,sectors * CD_FRAMESIZE_RAW))
    return(1);
  if(p)memcpy(p,d->sg_buffer,sectors*CD_FRAMESIZE_RAW);
  return(0);
}

static int i_read_mmc (cdrom_drive *d, void *p, long begin, long sectors){
  memcpy(d->sg_buffer,(char []){0xbe, 4, 0, 0, 0, 0, 0, 0, 0, 0x10, 0, 0},12);

  d->sg_buffer[3] = (begin >> 16) & 0xFF;
  d->sg_buffer[4] = (begin >> 8) & 0xFF;
  d->sg_buffer[5] = begin & 0xFF;
  d->sg_buffer[8] = sectors;
  if(handle_scsi_cmd(d,12,0,sectors * CD_FRAMESIZE_RAW))
    return(1);
  if(p)memcpy(p,d->sg_buffer,sectors*CD_FRAMESIZE_RAW);
  return(0);
}

static int i_read_mmc2 (cdrom_drive *d, void *p, long begin, long sectors){
  memcpy(d->sg_buffer,(char []){0xbe, 4, 0, 0, 0, 0, 0, 0, 0, 0xf8, 0, 0},12);

  d->sg_buffer[3] = (begin >> 16) & 0xFF;
  d->sg_buffer[4] = (begin >> 8) & 0xFF;
  d->sg_buffer[5] = begin & 0xFF;
  d->sg_buffer[8] = sectors;
  if(handle_scsi_cmd(d,12,0,sectors * CD_FRAMESIZE_RAW))
    return(1);
  if(p)memcpy(p,d->sg_buffer,sectors*CD_FRAMESIZE_RAW);
  return(0);
}

static long scsi_read_map (cdrom_drive *d, void *p, long begin, long sectors,
			  int (*map)(cdrom_drive *, void *, long, long)){
  int retry_count,err;
  char *buffer=(char *)p;

  /* read d->nsectors at a time, max. */
  sectors=(sectors>d->nsectors?d->nsectors:sectors);
  sectors=(sectors<1?1:sectors);

  retry_count=0;
  
  do {
    if((err=map(d,(p?buffer:NULL),begin,sectors))){
      if(!d->error_retry)return(-7);
      switch(errno){
      case ENOMEM:
	if(d->bigbuff==0){
	  /* just testing for SG_BIG_BUFF */
	  return(-7);
	}else{
	  /* D'oh.  Possible kernel error. Keep limping */
	  if(sectors==1){
	    /* Nope, can't continue */
	    cderror(d,"300: Kernel memory error\n");
	    return(-300);  
	  }
	}
      case EIO:
      case EINVAL:
      default:
	if(sectors==1){
	  if(errno==EIO){
	    if(d->fua==-1) /* testing for FUA support */
	      return(-7);
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
	      memset(buffer,-1,CD_FRAMESIZE_RAW);
	      err=0;
	    }
	    break;
	  }
	  /* OK, ok, bail. */
	  cderror(d,"007: Unknown, unrecoverable error reading data\n");
	  return(-7);
	}
      }
      if(retry_count>4==0)
	if(sectors>1)sectors>>=1;
      retry_count++;
      if(retry_count>MAX_RETRIES){
	cderror(d,"007: Unknown, unrecoverable error reading data\n");
	return(-7);
      }
    }else
      break;
  } while (err);
  
  return(sectors);
}

static long scsi_read_10 (cdrom_drive *d, void *p, long begin, 
			       long sectors){
  return(scsi_read_map(d,p,begin,sectors,i_read_10));
}

static long scsi_read_12 (cdrom_drive *d, void *p, long begin, 
			       long sectors){
  return(scsi_read_map(d,p,begin,sectors,i_read_12));
}

static long scsi_read_nec (cdrom_drive *d, void *p, long begin, 
			       long sectors){
  return(scsi_read_map(d,p,begin,sectors,i_read_nec));
}

static long scsi_read_sony (cdrom_drive *d, void *p, long begin, 
			       long sectors){
  return(scsi_read_map(d,p,begin,sectors,i_read_sony));
}

static long scsi_read_mmc (cdrom_drive *d, void *p, long begin, 
			       long sectors){
  return(scsi_read_map(d,p,begin,sectors,i_read_mmc));
}

static long scsi_read_mmc2 (cdrom_drive *d, void *p, long begin, 
			       long sectors){
  return(scsi_read_map(d,p,begin,sectors,i_read_mmc2));
}

/* Some drives, given an audio read command, return only 2048 bytes
   of data as opposed to 2352 bytes.  Look for -1s at the end of the
   single sector verification read */

static int verify_2352_bytes(cdrom_drive *d){
  if(d->clear_buff_via_bug){
    long i,flag=0;
    for(i=2100;i<2352;i++)
      if(d->sg_buffer[i]!=(unsigned char)'\377'){
	flag=1;
	break;
      }

    if(!flag)return(0);
  }
  return(1);
}

/* So many different read commands, densities, features...
   Verify that our selected 'read' command actually reads 
   nonzero data, else search through other possibilities */

static int verify_read_command(cdrom_drive *d){
  int i,j,k;

  int  (*enablecommand)  (struct cdrom_drive *d, int speed);
  long (*readcommand)   (struct cdrom_drive *d, void *p, long begin, 
		       long sectors);
  unsigned char density;
  
  size16 *buff=malloc(CD_FRAMESIZE_RAW);

  cdmessage(d,"\nVerifying drive can read CDDA...\n");

  /* try the expected command set; grab the center of each track, look
     for data */

  /* The following is one of the scariest hacks I've ever had to use.
     The idea is this: We want to know if a command fails.  The
     generic scsi driver (as of now) won't tell us; it hands back the
     uninitialized contents of the preallocated kernel buffer.  We
     force this buffer to a known value via another bug (nonzero data
     length for a command that doesn't take data) such that we can
     tell if the command failed.  It's even atomic.  Scared yet? */

  if(d->enable_cdda(d,1)==0){
    
    for(i=1;i<=d->tracks;i++){
      if(cdda_track_audiop(d,i)==1){
	long firstsector=cdda_track_firstsector(d,i);
	long lastsector=cdda_track_lastsector(d,i);
	long sector=(firstsector+lastsector)>>1;
	
	if(d->read_audio(d,buff,sector,1)>0){
	  if(verify_2352_bytes(d)){
	    cdmessage(d,"\tExpected command set reads OK.\n");
	    d->enable_cdda(d,0);
	    free(buff);
	    return(0);
	  }
	}
      }
    }
    
    d->enable_cdda(d,0);
  }

  {
    char *es="",*rs="";
    d->bigendianp=-1;
    density=d->density;
    readcommand=d->read_audio;
    enablecommand=d->enable_cdda;


    /* No nonzeroes?  D'oh.  Exhaustive search */
    cdmessage(d,"\tFAILED.  Perhaps this drive cannot read CDDA?\n"
	      "\tTrying for luck: searching for working command set...\n");
    
    /* loops:  
       density/enable no,  0x0/org,  0x04/org, 0x82/org
       read command read_10 read_12 read_sony read_mmc read_mmc2 read_nec */
    

    for(j=0;j>=0;j++){
      
      switch(j){
      case 0:
	d->read_audio=scsi_read_10;
	rs="0x28";
	break;
      case 1:
	d->read_audio=scsi_read_12;
	rs="0xa8";
	break;
      case 2:
	d->read_audio=scsi_read_mmc;
	rs="0xbe";
	break;
      case 3:
	d->read_audio=scsi_read_mmc2;
	rs="be+f";
	break;
      case 4:
	d->read_audio=scsi_read_nec;
	rs="0xd4";
	break;
      case 5:
	d->read_audio=scsi_read_sony;
	rs="0xd8";
	j=-2;
	break;
      }
      
      for(i=0;i>=0;i++){
	switch(i){
	case 0:
	  d->density=0;
	  d->enable_cdda=Dummy;
	  es="none    ";
	  break;
	case 1:
	  d->density=0;
	  d->enable_cdda=scsi_enable_cdda;
	  es="yes/0x00";
	  break;
	case 2:
	  d->density=0x04;
	  d->enable_cdda=scsi_enable_cdda;
	  es="yes/0x04";
	  break;
	case 3:
	  d->density=0x82;
	  d->enable_cdda=scsi_enable_cdda;
	  es="yes/0x82";
	  i=-2;
	  break;
	}

	cdmessage(d,"\r\tscanning --> density enable: [");
	cdmessage(d,es);
	cdmessage(d,"]  command: [");
	cdmessage(d,rs);
	cdmessage(d,"]        ");

	if(d->enable_cdda(d,1)==0){
	  for(k=1;k<=d->tracks;k++){
	    if(cdda_track_audiop(d,k)==1){
	      long firstsector=cdda_track_firstsector(d,k);
	      long lastsector=cdda_track_lastsector(d,k);
	      long sector=(firstsector+lastsector)>>1;
	      
	      if(d->read_audio(d,buff,sector,1)>0){
		if(verify_2352_bytes(d)){
		  cdmessage(d,"\r\tFound a potentially working command set:                    \n"
			    "\t\tEnable/Density:");
		  cdmessage(d,es);
		  cdmessage(d,"  Read command:");
		  cdmessage(d,rs);
		  cdmessage(d,"\n\t\t(please email a copy of this "
			    "output to xiphmont@mit.edu)\n");
		  
		  free(buff);
		  d->enable_cdda(d,0);
		  return(0);
		}
	      }else
		break;
	    }
	  }
	  d->enable_cdda(d,0);
	}
      }
    }

    /* D'oh. */
    d->density=density;
    d->read_audio=readcommand;
    d->enable_cdda=enablecommand;

    cdmessage(d,"\r\tUnable to find any command set; "
	      "drive probably not CDDA capable.\n");

    cderror(d,"006: Could not read any data from drive\n");

  }
  free(buff);
  return(-6);
}

#include "scsi_exceptions.h"

static int guess_atapi(cdrom_drive *d,int reportp){

  /* Use an Inquiry request to guess drive type. */
  
  /* the fields in byte 3 of the response serve different purposes in
     ATAPI an SCSI and we can probably distinguish the two, but not
     always */

  /* Check our known weird drives.... */
  {
    int i=0;
    while(atapi_list[i].model){
      if(!strncmp(atapi_list[i].model,d->drive_model,strlen(atapi_list[i].model)))
	if(atapi_list[i].atapi!=-1){
	  if(reportp)
	    cdmessage(d,"\tThis drive appears on the 'known exceptions' list:\n");
	  if(atapi_list[i].atapi){
	    if(reportp)
	      cdmessage(d,"\tDrive is ATAPI (using SCSI host adaptor emulation)\n");
	  }else{
	    if(reportp)cdmessage(d,"\tDrive is SCSI\n");
	  }
	  return(atapi_list[i].atapi);
	}
      i++;
    }
  }

  /* No exception listing.  What does byte 3 say? */

  /* Low 4 bits are data format; 0 is SCSI-I or early ATAPI,
                                 1 is modern ATAPI or SCSI-1 CCS
				 2 is SCSI-II (or future ATAPI?)
				 3 is reserved (SCSI-III?) */

  /* high 4 bits are ATAPI version in ATAPI and AENC/TrmIOP/Reserved in SCSI */

  /* How I see it:
     (&0x3f) 0x00 SCSI-I
     (&0x3f) 0x01 SCSI-1 CCS 
     (&0x3f) 0x02 SCSI-II
     0x20 early ATAPI
     0x21 modern ATAPI
     0x31 Mt FUJI ATAPI
     0x03 SCSI-III?  dunno... */

  if(reportp){
    char buffer[256];
    sprintf(buffer,"\tInquiry bytes: 0x%02x 0x%02x 0x%02x 0x%02x\n",
	    (int)d->inqbytes[0],
	    (int)d->inqbytes[1],
	    (int)d->inqbytes[2],
	    (int)d->inqbytes[3]);
    cdmessage(d,buffer);
  }

  switch(d->inqbytes[3]){
  case 0x20:
    /* early ATAPI */
    if(reportp){
      cderror(d,"\tDrive appears to be early (pre-draft) ATAPI\n");
      cderror(d,"\tThis drive will probably break cdparanoia; please send\n");
      cderror(d,"\temail to xiphmont@mit.edu\n");
    }
    return(1);
  case 0x21:
    /* modern ATAPI */
    if(reportp)
      cdmessage(d,"\tDrive appears to be standard ATAPI\n");
    return(1);
  case 0x30:
    /* Old Mt Fuji? */
    if(reportp)
      cdmessage(d,"\tDrive appears to be an early Mt. Fuji ATAPI C/DVD\n");
    return(1);
  case 0x31:
    /* Mt Fuji */
    if(reportp)
      cdmessage(d,"\tDrive appears to be Mt. Fuji ATAPI C/DVD\n");
    return(1);
  default:
    if(reportp)
      switch(d->inqbytes[3]&0x0f){
      case 0x0:
	cdmessage(d,"\tDrive appears to be SCSI-1\n");
	break;
      case 0x1:
	cdmessage(d,"\tDrive appears to be SCSI-1-CCS\n");
	break;
      case 0x2:
	cdmessage(d,"\tDrive appears to be SCSI-2\n");
	break;
      case 0x3:
	cdmessage(d,"\tUnknown type, perhaps SCSI-3?\n");
	break;
      default:
	cdmessage(d,"\tUnknown drive type; assuming SCSI\n");
	break;
      }
  }
  return(0);
}

static int check_atapi(cdrom_drive *d){
  int atapiret=-1;
  int fd = d->cdda_fd; /* this is the correct fd (not ioctl_fd), as the 
			  generic device is the device we need to check */
			  
  cdmessage(d,"\nChecking for SCSI emulation and transport revision...\n");

  /* This isn't as strightforward as it should be */

  /* first, we try the SG_EMULATED_HOST ioctl; this only exists in new
     kernels though */

  if (ioctl(fd,SG_EMULATED_HOST,&atapiret))
    cdmessage(d,"\tNo SG_EMULATED_HOST ioctl(); Checking inquiry command...\n");
  else {
    if(atapiret==1){
      cdmessage(d,"\tDrive is ATAPI (using SCSI host adaptor emulation)\n");
      /* Disable kernel SCSI command translation layer for access through sg */
      if (ioctl(fd,SG_SET_TRANSFORM,0))
	cderror(d,"\tCouldn't disable kernel command translation layer\n");
      d->is_atapi=1;
    }else{
      cdmessage(d,"\tDrive is SCSI\n");
      d->is_atapi=0;
    }

    if(guess_atapi(d,0)!=d->is_atapi){
      cderror(d,"\tNOTE: Our ATAPI/SCSI guessing algorithm will detect the\n");
      cderror(d,"\tdrive type incorrectly on older kernels; please e-mail the\n");
      cderror(d,"\toutput of 'cdparanoia -vQ' to xiphmont@mit.edu\n");
    }
    return(d->is_atapi);
  }
  
  return(d->is_atapi=guess_atapi(d,1));
}  

static int check_mmc(cdrom_drive *d){
  char *b;
  cdmessage(d,"\nChecking for MMC style command set...\n");

  d->is_mmc=0;
  if(mode_sense(d,22,0x2A)==0){
  
    b=d->sg_buffer;
    b+=b[3]+4;
    
    if((b[0]&0x3F)==0x2A){
      /* MMC style drive! */
      d->is_mmc=1;
      
      if(b[1]>=4)
	if(b[5]&0x1){
	  cdmessage(d,"\tDrive is MMC style\n");
	  return(1);
	}else{
	  cdmessage(d,"\tDrive is MMC, but reports CDDA incapable.\n");
	  cdmessage(d,"\tIt will likely not be able to read audio data.\n");
	  return(1);
	}
    }
  }
  
  cdmessage(d,"\tDrive does not have MMC CDDA support\n");
  return(0);
}

static void check_exceptions(cdrom_drive *d,exception *list){

  int i=0;
  while(list[i].model){
    if(!strncmp(list[i].model,d->drive_model,strlen(list[i].model))){
      if(list[i].density)d->density=list[i].density;
      if(list[i].enable)d->enable_cdda=list[i].enable;
      if(list[i].read)d->read_audio=list[i].read;
      if(list[i].bigendianp!=-1)d->bigendianp=list[i].bigendianp;
      return;
    }
    i++;
  }
}

/* request vendor brand and model */
unsigned char *scsi_inquiry(cdrom_drive *d){
  memcpy(d->sg_buffer,(char[]){ 0x12,0,0,0,56,0},6);

  if(handle_scsi_cmd(d,6, 0, 56 )) {
    cderror(d,"008: Unable to identify CDROM model\n");
    return(NULL);
  }
  return (d->sg_buffer);
}

int scsi_init_drive(cdrom_drive *d){
  int ret;

  check_atapi(d);
  check_mmc(d);

  /* set the correct command set for *different* vendor specific
   * implementations. Was this really necessary, folks?  */

  /* generic Sony type defaults */
  d->density = 0x0;
  d->enable_cdda = Dummy;
  d->read_audio = scsi_read_sony;
  d->fua=0;
      
  if(d->is_mmc){

    d->read_audio = scsi_read_mmc;
    d->bigendianp=0;

    check_exceptions(d,mmc_list);

  }else{
    
    if(d->is_atapi){
      /* Not MMC maybe still uses 0xbe */

      d->read_audio = scsi_read_mmc;
      d->bigendianp=0;

      check_exceptions(d,atapi_list);

    }else{

      check_exceptions(d,scsi_list);

    }
  }

  d->read_toc = (!memcmp(d->drive_model, "IMS", 3) && !d->is_atapi) ? scsi_read_toc2 : 
    scsi_read_toc;
  
  d->enable_cdda(d,0);

  if(!d->is_atapi){
    unsigned sector_size= get_orig_sectorsize(d);
    
    if(sector_size<2048 && set_sectorsize(d,2048))
      d->adjust_ssize = 2048 / sector_size;
    else
      d->adjust_ssize = 1;
  }else
    d->adjust_ssize = 1;
  
  d->tracks=d->read_toc(d);
  if(d->tracks<1)
    return(d->tracks);

  d->opened=1;

  if((ret=verify_read_command(d)))return(ret);
  d->error_retry=1;
  d->clear_buff_via_bug=0;

  if(d->nsectors<1)find_bloody_big_buff_size(d);
  d->sg=realloc(d->sg,d->nsectors*CD_FRAMESIZE_RAW + SG_OFF);
  d->sg_buffer=d->sg+SG_OFF;
 
  /* fua_supportp(d); does the command support the FUA bit? */

  return(0);
}
