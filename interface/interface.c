/******************************************************************
 * CopyPolicy: GNU Public License 2 applies
 * Copyright (C) 1998 Monty xiphmont@mit.edu
 * 
 * Top-level interface module for cdrom drive access.  SCSI, ATAPI, etc
 *    specific stuff are in other modules.  Note that SCSI does use
 *    specialized ioctls; these appear in common_interface.c where the
 *    generic_scsi stuff is in scsi_interface.c.
 *
 ******************************************************************/

#include "low_interface.h"
#include "common_interface.h"
#include "utils.h"

/* doubles as "cdrom_drive_free()" */
int cdda_close(cdrom_drive *d){
  if(d){
    if(d->opened)
      d->enable_cdda(d,0);

    if(d->cdda_device_name)free(d->cdda_device_name);
    if(d->ioctl_device_name)free(d->ioctl_device_name);
    if(d->drive_model)free(d->drive_model);
    if(d->cdda_fd!=-1)close(d->cdda_fd);
    if(d->ioctl_fd!=-1 && d->ioctl_fd!=d->cdda_fd)close(d->ioctl_fd);
    if(d->sg)free(d->sg);
    
    free(d);
  }
  return(0);
}

/* finish initializing the drive! */
int cdda_open(cdrom_drive *d){
  if(d->opened)return(0);

  switch(d->interface){
  case GENERIC_SCSI:  
    if(scsi_init_drive(d))
      return(1);
    break;
  case COOKED_IOCTL:  
    if(cooked_init_drive(d))
      return(1);
    break;
#ifdef CDDA_TEST
  case TEST_INTERFACE:  
    if(test_init_drive(d))
      return(1);
    break;
#endif
  default:
    cderror(d,"100: Interface not supported\n");
    return(1);
  }
  
  /* Get TOC, enable for CDDA */

  d->nothing_read=-1;
  if(d->enable_cdda(d,1))
    return(1);
    
  /*  d->select_speed(d,d->maxspeed); most drives are full speed by default */
  if(d->bigendianp==-1)d->bigendianp=data_bigendianp(d);
  return(0);
}

long cdda_read(cdrom_drive *d, void *buffer, long beginsector, long sectors){
  if(d->opened){
    if(sectors>0){
      sectors=d->read_audio(d,buffer,beginsector,sectors);

      if(sectors!=-1){
	/* byteswap? */
	if(d->bigendianp==-1) /* not determined yet */
	  d->bigendianp=data_bigendianp(d);
	
	if(d->bigendianp!=bigendianp()){
	  int i;
	  unsigned size16 *p=(unsigned size16 *)buffer;
	  long els=sectors*CD_FRAMESIZE_RAW/2;
	  
	  for(i=0;i<els;i++)p[i]=swap16(p[i]);
	}
      }
    }
    return(sectors);
  }
  
  cderror(d,"400: Device not open\n");
  return(-1);
}

int cdda_speed(cdrom_drive *d, unsigned speed){
  if(!d->opened){
    cderror(d,"400: Device not open\n");
    return(1);
  }
  if(d->select_speed(d,speed))return(1);
  return(0);
}

void cdda_verbose_set(cdrom_drive *d,int err_action, int mes_action){
  d->messagedest=mes_action;
  d->errordest=err_action;
}

extern char *cdda_messages(cdrom_drive *d){
  char *ret=d->messagebuf;
  d->messagebuf=NULL;
  return(ret);
}

extern char *cdda_errors(cdrom_drive *d){
  char *ret=d->errorbuf;
  d->errorbuf=NULL;
  return(ret);
}

