/******************************************************************
 * CopyPolicy: GNU Public License 2 applies
 * Original interface.c Copyright (C) 1994-1997 
 *            Eissfeldt heiko@colossus.escape.de
 * Current blenderization Copyright (C) 1998 Monty xiphmont@mit.edu
 * 
 * Generic SCSI interface specific code.
 *
 *    NOTE: a bug/misfeature in the kernel requires blocking signal
 *          SIGINT during SCSI command handling. Once this flaw has
 *          been removed, the sigprocmask SIG_BLOCK and SIG_UNBLOCK 
 *          calls should removed, thus saving context switches.
 *
 ******************************************************************/

#include "low_interface.h"
#include "common_interface.h"
#include "utils.h"

#define MAX_BIG_BUFF_SIZE 65536
#define MIN_BIG_BUFF_SIZE 4096
#define SG_OFF sizeof(struct sg_header)

/* this may be bogus; SCSI spec isn't very explicit on the point.
   NEC drives are known to take the bit, but then return all zero
   data :-P */

static void fua_supportp(cdrom_drive *d){

  if(d->fua==-1){
    long begin=cdda_disc_firstsector(d);
    /* try reading the first sector with the FUA bit set.  if the
       command succeeds, set our flag */
    
    /* Why not just query the drive for FUA bit support?  That doesn;t
       tell us if:
       
       a) the firmware is broken
       b) the drive supports FUA, but our read proprietary read command does not
       (see above)
       
       */
    cdmessage(d,"Checking for support of FUA bit in SCSI read command...\n");
    
    d->fua=-1;
    if(d->read_audio(d,NULL,begin,1)==-1){
      cdmessage(d,"\tdrive rejected FUA bit.\n");
      d->fua=0;
      if(d->read_audio(d,NULL,begin,1)==-1){
	cdmessage(d,"\tHuh.  Drive also rejected without FUA bit.\n");
	cdmessage(d,"\tYou'd better send mail to xiphmont@mit.edu\n");
	cdmessage(d,"\tContinuing...\n");
      }
      return;
    }
    
    
    
    cdmessage(d,"\tdrive accepted FUA bit.\n");
    d->fua=1;
  }
}

static void find_bloody_big_buff_size(cdrom_drive *d){

  /* find the biggest read command that succeeds.  This should be
     safe; the kernel will reject requests that are too big. */

  long begin=cdda_disc_firstsector(d);
  long cur=MAX_BIG_BUFF_SIZE/CD_FRAMESIZE_RAW;
  cdmessage(d,"Attempting to autosense SG_BIG_BUFF size...\n");

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
  {
    char buffer[256];
    sprintf(buffer,"\tSetting read block size at %ld sectors (%ld bytes).\n",
	    cur,cur*CD_FRAMESIZE_RAW);
    cdmessage(d,buffer);
  }
  d->nsectors=cur;
  d->bigbuff=cur*CD_FRAMESIZE_RAW;

  d->sg=realloc(d->sg,d->nsectors*CD_FRAMESIZE_RAW + SG_OFF);
  d->sg_buffer=d->sg+SG_OFF;
  
}

/* process a complete scsi command. */
static int handle_scsi_cmd(cdrom_drive *d,
			   unsigned int cmd_len, 
			   unsigned int in_size, 
			   unsigned int out_size){
  int status = 0;
  struct sg_header *sg_hd=(struct sg_header *)d->sg;
  
  /* generic scsi device services */
  sg_hd->reply_len = SG_OFF + out_size;
  sg_hd->twelve_byte = cmd_len == 12;
  sg_hd->result = 0;

  sigprocmask (SIG_BLOCK, &(d->sigset), NULL );
  status = write(d->cdda_fd, sg_hd, SG_OFF + cmd_len + in_size );
  if (status<0 || status != SG_OFF + cmd_len + in_size ) {
    sigprocmask ( SIG_UNBLOCK, &(d->sigset), NULL );
    return(1);
  }
  
  status = read(d->cdda_fd, sg_hd, SG_OFF + out_size);
  sigprocmask ( SIG_UNBLOCK, &(d->sigset), NULL );

  if (status<0 || status != SG_OFF + out_size || sg_hd->result) 
    return 1;

  /* Look if we got what we expected to get */
  if (status == SG_OFF + out_size) status = 0; /* got them all */
  return status;
}

/* get current sector size from SCSI cdrom drive */
static unsigned int get_orig_sectorsize(cdrom_drive *d){

  memcpy(d->sg_buffer,  
	 (char [])  {0x1A, /* MODE_SENSE */
		     0x00, /* return block descriptor */
		     0x01, /* page control current values, page 1 */
		     0, /* reserved */
		     12, /* sizeof(modesense - SG_OFF) */
		     0}, /* reserved */ 
         6);

  if (handle_scsi_cmd (d, 6, 0, 12)) return(1);

  d->orgmode4 = d->sg_buffer[4];
  d->orgmode10 = d->sg_buffer[10];
  d->orgmode11 = d->sg_buffer[11];

  return (((int)(d->sg_buffer[10]) << 8) + d->sg_buffer[11]);
}

/* switch CDROM scsi drives to given sector size  */
static int set_sectorsize (cdrom_drive *d,unsigned int secsize){
  unsigned char *mode = d->sg_buffer + 6;

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
  mode [4] = d->orgmode4;      /* normal density */
  mode [10] =  secsize >> 8;   /* block length "msb" */
  mode [11] =  secsize & 0xFF; /* block length lsb */

  /* do the scsi cmd */
  return(handle_scsi_cmd (d,6, 12, 0));
}


/* switch Toshiba/DEC and HP drives from/to cdda density */
static int scsi_enable_cdda (cdrom_drive *d, int fAudioMode){
  unsigned char *mode = d->sg_buffer+6;

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
		       /* block descriptor */
		       0,       /* Density Code */
		       0, 0, 0, /* # of Blocks */
		       0,       /* reserved */
		       0, 0, 0},/* Blocklen */
	 18);

  if (fAudioMode) {
    /* prepare to read audio cdda */
    mode [4] = d->density;  			/* cdda density */
    mode [10] = (CD_FRAMESIZE_RAW >> 8);   /* block length "msb" */
    mode [11] = (CD_FRAMESIZE_RAW & 0xFF);
  } else {
    /* prepare to read cds in the previous mode */
    mode [4] = d->orgmode4; /* 0x00; 			\* normal density */
    mode [10] = d->orgmode10; /* (CD_FRAMESIZE >> 8);  \* block length "msb" */
    mode [11] = d->orgmode11; /* (CD_FRAMESIZE & 0xFF); \* block length lsb */
  }

  if(handle_scsi_cmd (d,6, 12,0)){
    cderror(d,"001: Unable to set CDROM to read audio mode\n");
    return(1);
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
    return(-1);
  }

  /* copy to our structure and convert start sector */
  tracks = ((d->sg_buffer[0] << 8) + d->sg_buffer[1] - 2) / 8;
  if (tracks > MAXTRK) {
    cderror(d,"003: CDROM reporting illegal number of tracks\n");
    return(-1);
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
    return(-1);
  }

  /* copy to our structure and convert start sector */
  tracks = d->sg_buffer[1];
  if (tracks > MAXTRK) {
    cderror(d,"003: CDROM reporting illegal number of tracks\n");
    return(-1);
  }

  for (i = 0; i < tracks; i++){

      memcpy(d->sg_buffer, (char[]){ 0xe5, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 10);
      d->sg_buffer[5]=i+1;
      d->sg_buffer[8]=255;

      if (handle_scsi_cmd (d,10, 0, 256)){
	cderror(d,"005: Unable to read table of contents entry\n");
	return(-1);
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
  if(handle_scsi_cmd(d,10, 0, sectors * CD_FRAMESIZE_RAW))
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
  if(handle_scsi_cmd(d,10, 0, sectors * CD_FRAMESIZE_RAW))
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
  if(handle_scsi_cmd(d,12, 0, sectors * CD_FRAMESIZE_RAW))
    return(1);
  if(p)memcpy(p,d->sg_buffer,sectors*CD_FRAMESIZE_RAW);
  return(0);
}

static int i_read_sonyMSF (cdrom_drive *d, void *p, long begin, long sectors){
  int end=begin+sectors;
  begin+=150;
  end+=150;

  memcpy(d->sg_buffer,(char []){0xd9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},12);

  if(d->fua)
    d->sg_buffer[1]=0x08;

  d->sg_buffer[3] = begin/(60*75);
  d->sg_buffer[4] = (begin/75)%60;
  d->sg_buffer[5] = begin%75;
  d->sg_buffer[7] = end/(60*75); 
  d->sg_buffer[8] = (end/75)%60;
  d->sg_buffer[9] = end%75;
  if(handle_scsi_cmd(d,12, 0, sectors * CD_FRAMESIZE_RAW))
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

  retry_count=0;
  
  do {
    if((err=map(d,(p?buffer:NULL),begin,sectors))){
      switch(errno){
      case ENOMEM:
	if(d->bigbuff==0){
	  /* just testing for SG_BIG_BUFF */
	  return(-1);
	}else{
	  /* D'oh.  Possible kernel error. Keep limping */
	  if(sectors==1){
	    /* Nope, can't continue */
	    cderror(d,"300: Kernel memory error\n");
	    return(-1);  
	  }
	}
      case EIO:
      case EINVAL:
      default:
	if(sectors==1){
	  if(d->nothing_read){
	    /* Can't read the *first* sector?! Ouch! */
	    cderror(d,"006: Could not read any data from drive\n");
	    return(-1);
	  }
	  if(errno==EIO){
	    if(d->fua==-1) /* testing for FUA support */
	      return(-1);
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
	  return(-1);
	}
      }
      if(retry_count>4==0)
	if(sectors>1)sectors>>=1;
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

static long scsi_read_10 (cdrom_drive *d, void *p, long begin, 
			       long sectors){
  return(scsi_read_map(d,p,begin,sectors,i_read_10));
}

static long scsi_read_nec (cdrom_drive *d, void *p, long begin, 
			       long sectors){
  return(scsi_read_map(d,p,begin,sectors,i_read_nec));
}

static long scsi_read_sony (cdrom_drive *d, void *p, long begin, 
			       long sectors){
  return(scsi_read_map(d,p,begin,sectors,i_read_sony));
}

static long scsi_read_sonyMSF (cdrom_drive *d, void *p, long begin, 
			       long sectors){
  return(scsi_read_map(d,p,begin,sectors,i_read_sonyMSF));
}

/********* non standardized speed selects ***********************/

static int scsi_speed_toshiba(cdrom_drive *d, int speed){
  memcpy(d->sg_buffer,(char[]){ 0x15,0x10,0,0,7,0, 0,0,0,0, 0x20,1,0}, 13);    

  if (speed != 1 && speed != 2) {
    cderror(d,"200: Invalid speed setting for drive\n");
    return(1);
  }

  d->sg_buffer[12] = speed == 1 ? 0 : 1;   /* 0 for single speed */

  /* do the scsi cmd */
  if (handle_scsi_cmd (d,6, 7, 0)){
    cderror(d,"201: Speed select failed\n");
    return(1);
  }
  return(0);
}

static int scsi_speed_nec(cdrom_drive *d, int speed){
  memcpy(d->sg_buffer,(char[]){ 0xc5,0x10,0,0,0,0,0,0,12,0, 
				  0,0,0,0, 0x0f,6,0,0,0,0,0,0}, 22);    

  if (speed != 1 && speed != 2) {
    cderror(d,"200: Invalid speed setting for drive\n");
    return(1);
  }

  /* bit 5 == 1 for single speed, otherwise double speed */
  d->sg_buffer[16] = speed == 1 ? 1 << 5 : 0;

  /* do the scsi cmd */
  if (handle_scsi_cmd (d,10, 12, 0)){
    cderror(d,"201: Speed select failed\n");
    return(1);
  }
  return(0);
}

static int scsi_speed_philips(cdrom_drive *d, int speed){
  memcpy(d->sg_buffer,(char[]){ 0x15,0x10,0,0,12,0,
				  0,0,0,0, 0x23,6,0,1,0,0,0,0}, 18);    

  if (speed != 1 && speed != 2 && speed != 4) {
    cderror(d,"200: Invalid speed setting for drive\n");
    return(1);
  }

  d->sg_buffer[12] = d->sg_buffer[14] = speed ;

  /* do the scsi cmd */
  if (handle_scsi_cmd (d,6, 12, 0)){
    cderror(d,"201: Speed select failed\n");
    return(1);
  }
  return(0);
}

static int scsi_speed_sony(cdrom_drive *d, int speed){
  memcpy(d->sg_buffer,(char[]){ 0x15,0x10,0,0,8,0,
				  0,0,0,0, 0x31,2,0,0}, 14);    

  /* speed values > 1 are drive dependent */

  if (speed > 4) speed = 8;
  d->sg_buffer[12]=speed/2;

  /* do the scsi cmd */
  if (handle_scsi_cmd (d,6, 8, 0)){
    cderror(d,"201: Speed select failed\n");
    return(1);
  }
  return(0);
}

static int scsi_speed_yamaha(cdrom_drive *d, int speed){
  memcpy(d->sg_buffer,(char[]){ 0x15,0x10,0,0,8,0,
				  0,0,0,0, 0x31,2,0,0}, 14);    

  /* speed values > 1 are drive dependent */
  if (speed > 4) speed = 8;
  d->sg_buffer[12]=(speed/2)<<4;

  /* do the scsi cmd */
  if (handle_scsi_cmd (d,6, 8, 0)){
    cderror(d,"201: Speed select failed\n");
    return(1);
  }
  return(0);
}

/* request vendor brand and model */
static unsigned char *scsi_inquiry(cdrom_drive *d){
  memcpy(d->sg_buffer,(char[]){ 0x12,0,0,0,56,0},6);

  if(handle_scsi_cmd(d,6, 0, 56 )) {
    cderror(d,"008: Unable to identify CDROM model\n");
    return(NULL);
  }
  return (d->sg_buffer);
}

/* hook */
static int Dummy (cdrom_drive *d,int s){
  return(0);
}

int scsi_init_drive(cdrom_drive *d){
  struct sigaction sigac;
  unsigned char *p;

  /* build signal set to block for during generic scsi */
  sigemptyset (&(d->sigset));
  sigaddset (&(d->sigset), SIGINT);
  sigaddset (&(d->sigset), SIGPIPE);
  sigac.sa_handler = exit;
  sigemptyset(&sigac.sa_mask);
  sigac.sa_flags = 0;
  sigaction( SIGINT, &sigac, NULL);
  sigaction( SIGQUIT, &sigac, NULL);
  sigaction( SIGTERM, &sigac, NULL);
  sigaction( SIGHUP, &sigac, NULL);
  sigaction( SIGPIPE, &sigac, NULL);
  sigaction( SIGTRAP, &sigac, NULL);
  sigaction( SIGIOT, &sigac, NULL);

  /* malloc our big buffer for scsi commands */
  if(d->nsectors==-1)
    d->sg=malloc(MAX_BIG_BUFF_SIZE);
  else
    d->sg=malloc(d->nsectors*CD_FRAMESIZE_RAW);
  d->sg_buffer=d->sg+SG_OFF;

  /* set the correct command set for *different*
   * vendor specific implementations. Was this really necessary, folks?
   */
  
  p = scsi_inquiry(d);

  if (*p != TYPE_ROM && *p != TYPE_WORM) {
    cderror(d,"101: Drive is neither a CDROM nor a WORM device\n");
    free(d->sg);
    d->sg=NULL;
    return(1);
  }

  /* generic Sony type defaults */
  d->density = 0x0;
  d->enable_cdda = Dummy;
  d->read_audio = scsi_read_sony;
  d->select_speed = scsi_speed_sony;
  d->fua=0;
  d->maxspeed=2;

  /* check for brands and adjust special peculiaritites */

  /* If your drive is not treated correctly, you can adjust some things
     here (but please mail me so I can add it for everyone else!)

     d->bigendianp: should be to 0, if the CDROM drive or CD-Writer
     delivers the samples in the native byteorder of the audio cd (LSB
     first). The SCSI-II spec says this should be (bigendian) for all
     SCSI-II drives, but many drives are little endian anyway.

     NOTE: If you get correct wav files when using sox with the '-x' option,
     the endianess is wrong. You can specify endianness ont he commandline
     
     */

  if (!memcmp(p+8,"TOSHIBA", 7) ||
      !memcmp(p+8,"IBM", 3) ||
      !memcmp(p+8,"DEC", 3)) {

    d->density = 0x82;
    d->enable_cdda= scsi_enable_cdda;
    d->read_audio= scsi_read_10;
    d->select_speed= scsi_speed_toshiba;
    d->bigendianp=0;
    d->fua=1;
  
  } else if (!memcmp(p+8,"IMS",3) ||
	     !memcmp(p+8,"KODAK",5) ||
	     !memcmp(p+8,"RICOH",5) ||
	     !memcmp(p+8,"HP",2) ||
	     !memcmp(p+8,"PHILIPS",7) ||
	     !memcmp(p+8,"PLASMON",7) ||
	     !memcmp(p+8,"GRUNDIG CDR100IPW",17) ||
	     !memcmp(p+8,"MITSUMI CD-R ",13)) {

    d->enable_cdda= scsi_enable_cdda;
    d->read_audio= scsi_read_10;
    d->select_speed= scsi_speed_philips;
    d->bigendianp=1;
    d->fua=1;

  } else if (!memcmp(p+8,"YAMAHA",6)) {

    d->enable_cdda= scsi_enable_cdda;
    d->select_speed= scsi_speed_yamaha;
    d->bigendianp=0;
    d->maxspeed=8;

  } else if (!memcmp(p+8,"PLEXTOR",7) ||
	     !memcmp(p+8,"SONY",4)) {

    d->bigendianp=0;
    d->maxspeed=8;

  } else if (!memcmp(p+8,"NEC",3)) {

    d->read_audio= scsi_read_nec;
    d->select_speed= scsi_speed_nec;

    d->bigendianp=0;
    d->maxspeed=2;
  }

  d->read_toc = (!memcmp(p+8, "IMS", 3)) ? scsi_read_toc2 : scsi_read_toc;

  {
    unsigned sector_size= get_orig_sectorsize(d);
    
    if(sector_size!=2048 && set_sectorsize(d,2048))
      d->adjust_ssize = 2048 / sector_size;
    else
      d->adjust_ssize = 1;
  }

  d->tracks=d->read_toc(d);
  if(d->tracks==-1)
    return(1);

  d->opened=1;

  if(d->nsectors<1)find_bloody_big_buff_size(d);
  /* fua_supportp(d); does the command support the FUA bit? */

  return(0);
}



