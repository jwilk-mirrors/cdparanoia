/******************************************************************
 * CopyPolicy: GNU Public License 2 applies
 * Copyright (C) 1998 Monty xiphmont@mit.edu
 * 
 * Autoscan for or verify presence of a cdrom device
 * 
 ******************************************************************/

#include <stdio.h>
#include <ctype.h>
#include <sys/stat.h>
#include "cdda_interface.h"
#include "low_interface.h"
#include "common_interface.h"
#include "utils.h"

#define MAX_DEV_LEN 20 /* Safe because strings only come from below */
/* must be absolute paths! */
static char *scsi_cdrom_prefixes[3]={"/dev/scd","/dev/sr",NULL};
static char *scsi_generic_prefixes[2]={"/dev/sg",NULL};
static char *cdrom_devices[16]={"/dev/cdrom","/dev/hd?","/dev/scd?",
			  "/dev/sr?","/dev/cdu31a","/dev/cdu535",
			  "/dev/sbpcd","/dev/sbpcd?","/dev/sonycd",
			  "/dev/mcd","/dev/sjcd","/dev/aztcd","/dev/cm206cd",
			  "/dev/gscd","/dev/optcd",NULL};

/* Functions here look for a cdrom drive; full init of a drive type
   happens in interface.c */

cdrom_drive *cdda_find_a_cdrom(int messagedest,char **messages){
  /* Brute force... */
  
  int i=0;
  cdrom_drive *d;

  while(cdrom_devices[i]!=NULL){

    /* is it a name or a pattern? */
    char *pos;
    if((pos=strchr(cdrom_devices[i],'?'))){
      int j;
      /* try first eight of each device */
      for(j=0;j<4;j++){
	char *buffer=copystring(cdrom_devices[i]);

	/* number, then letter */
	
	buffer[pos-(cdrom_devices[i])]=j+48;
	if((d=cdda_identify(buffer,messagedest,messages)))
	  return(d);
	idmessage(messagedest,messages,"",NULL);
	buffer[pos-(cdrom_devices[i])]=j+97;
	if((d=cdda_identify(buffer,messagedest,messages)))
	  return(d);
	idmessage(messagedest,messages,"",NULL);
      }
    }else{
      /* Name.  Go for it. */
      if((d=cdda_identify(cdrom_devices[i],messagedest,messages)))
	return(d);
      
      idmessage(messagedest,messages,"",NULL);
    }
    i++;
  }
  idmessage(messagedest,messages,"\n\nNo cdrom drives accessible to %s found.\n",cuserid(NULL));
  return(NULL);
}

cdrom_drive *cdda_identify(const char *device, int messagedest,char **messages){
  struct stat st;
  cdrom_drive *d=NULL;
  idmessage(messagedest,messages,"Checking %s for cdrom...",device);

  if(stat(device,&st)){
    idperror(messagedest,messages,"\tCould not stat %s",device);
    return(NULL);
  }
    
#ifndef CDDA_TEST
  if (!S_ISCHR(st.st_mode) &&
      !S_ISBLK(st.st_mode)){
    idmessage(messagedest,messages,"\t%s is not a block or character device",device);
    return(NULL);
  }
#endif

  d=cdda_identify_cooked(device,messagedest,messages);
  if(!d)d=cdda_identify_scsi(device,NULL,messagedest,messages);

#ifdef CDDA_TEST
  if(!d)d=cdda_identify_test(device,messagedest,messages);
#endif
  
  return(d);
}

char *test_resolve_symlink(const char *file,int messagedest,char **messages){
  struct stat st;
  if(lstat(file,&st)){
    idperror(messagedest,messages,"\t\tCould not stat %s",file);
    return(NULL);
  }
  if(S_ISLNK(st.st_mode)){
    char buf[1024];
    int status=readlink(file,buf,1023);
    if(status==-1){
      idperror(messagedest,messages,"\t\tCould not resolve symlink %s",file);
      return(NULL);
    }
    buf[status]=0;

    /* Uh, oh Clem... them rustlers might be RELATIVE! */
    if(buf[0]!='/'){
      /* Yupper. */
      char *ret=copystring(file);
      char *pos=strrchr(ret,'/');
      if(pos){
	pos[1]='\0';
	ret=catstring(ret,buf);
	return(ret);
      }
      free(ret);
    }
    return(copystring(buf));

  }
  return(copystring(file));
}

cdrom_drive *cdda_identify_cooked(const char *dev, int messagedest,
				  char **messages){

  cdrom_drive *d=NULL;
  struct stat st;
  int fd=-1;
  int type;
  char *description=NULL;
  char *device;

  idmessage(messagedest,messages,"\tTesting %s for cooked ioctl() interface",dev);

  device=test_resolve_symlink(dev,messagedest,messages);
  if(device==NULL)return(NULL);

  if(stat(device,&st)){
    idperror(messagedest,messages,"\t\tCould not stat %s",device);
    free(device);
    return(NULL);
  }
    
  if (!S_ISCHR(st.st_mode) &&
      !S_ISBLK(st.st_mode)){
    idmessage(messagedest,messages,"\t\t%s is not a block or character device",device);
    free(device);
    return(NULL);
  }

  type=(int)(st.st_rdev>>8);
  switch (type) {
  case IDE0_MAJOR:
  case IDE1_MAJOR:
  case IDE2_MAJOR:
  case IDE3_MAJOR:
    /* Yay, ATAPI... */
    /* Ping for CDROM-ness */
    
    fd=open(device,O_RDONLY);
    if(fd==-1){
      idperror(messagedest,messages,"\t\tUnable to open %s",device);
      free(device);
      return(NULL);
    }
  
    if(ioctl_ping_cdrom(fd)){
      idmessage(messagedest,messages,"\t\tDevice %s is not a CDROM",device);
      close(fd);
      free(device);
      return(NULL);
    }
    description=atapi_drive_info(fd);
    
    break;
  case CDU31A_CDROM_MAJOR:
    /* major indicates this is a cdrom; no ping necessary. */
    description=copystring("Sony CDU31A or compatible");
    break;
  case CDU535_CDROM_MAJOR:
    /* major indicates this is a cdrom; no ping necessary. */
    description=copystring("Sony CDU535 or compatible");
    break;

  case MATSUSHITA_CDROM_MAJOR:
  case MATSUSHITA_CDROM2_MAJOR:
  case MATSUSHITA_CDROM3_MAJOR:
  case MATSUSHITA_CDROM4_MAJOR:
    /* major indicates this is a cdrom; no ping necessary. */
    description=copystring("non-ATAPI IDE-style Matsushita/Panasonic CR-5xx or compatible");
    break;
  case SANYO_CDROM_MAJOR:
    description=copystring("Sanyo proprietary or compatible: NOT CDDA CAPABLE");
    break;
  case MITSUMI_CDROM_MAJOR:
  case MITSUMI_X_CDROM_MAJOR:
    description=copystring("Mitsumi proprietary or compatible: NOT CDDA CAPABLE");
    break;
  case OPTICS_CDROM_MAJOR:
    description=copystring("Optics Dolphin or compatible: NOT CDDA CAPABLE");
    break;
  case AZTECH_CDROM_MAJOR:
    description=copystring("Aztech proprietary or compatible: NOT CDDA CAPABLE");
    break;
  case GOLDSTAR_CDROM_MAJOR:
    description=copystring("Goldstar proprietary: NOT CDDA CAPABLE");
    break;
  case CM206_CDROM_MAJOR:
    description=copystring("Philips/LMS CM206 proprietary: NOT CDDA CAPABLE");
    break;

  case SCSI_CDROM_MAJOR:   
  case SCSI_GENERIC_MAJOR: 
    /* Nope nope nope */
    idmessage(messagedest,messages,"\t\t%s is not a cooked ioctl CDROM.",device);
    free(device);
    return(NULL);
  default:
    /* What the hell is this? */
    idmessage(messagedest,messages,"\t\t%s is not a cooked ioctl CDROM.",device);
    free(device);
    return(NULL);
  }

  if(fd==-1)fd=open(device,O_RDONLY);
  if(fd==-1){
    idperror(messagedest,messages,"\t\tUnable to open %s",device);
    free(device);
    if(description)free(description);
    return(NULL);
  }
  
  /* Minimum init */
  
  d=calloc(1,sizeof(cdrom_drive));
  d->cdda_device_name=device;
  d->ioctl_device_name=copystring(device);
  d->drive_model=description;
  d->drive_type=type;
  d->cdda_fd=fd;
  d->ioctl_fd=fd;
  d->interface=COOKED_IOCTL;
  d->bigendianp=-1; /* We don't know yet... */
  d->nsectors=-1;
  idmessage(messagedest,messages,"\t\tCDROM sensed: %s",description);
  
  return(d);
}

typedef struct scsi_cdrom_device{
  char *specific_device;
  char *generic_device;
  char *model;
} scsi_cdrom_device;

/* Yeah, 1 char at a time.  It's better than leaking/overrunning.  Sue me. */
static char *getline(int fd){
  char *buffer=NULL;
  long buflen=0;
  char c;
  int status;

  int lastchar=-1;

  while((status=read(fd,&c,1))){
    if(status==-1){
      if(buffer)free(buffer);
      return(NULL);
    }
    if(c=='\n'){
      if(buflen){
	buffer[lastchar+1]=0;
	return(buffer);
      }else
	/* Only return null on hard EOF */
	return(calloc(1,sizeof(char)));
    }
    if(!isspace(c))lastchar=buflen;
    if(lastchar!=-1){
      /* not a critical path */
      buflen++;
      buffer=realloc(buffer,buflen+1);
      buffer[buflen-1]=c;
    }
  }
  if(buflen){
    buffer[lastchar+1]=0;
    return(buffer);
  }else
    return(NULL);
}

/* XXX */
/* This will need to be guarded for a threaded lib */
static struct scsi_cdrom_device *scsi_cdrom_list=NULL;
static int scsi_cdroms=-1;

static int list_scsi_cdrom_devices(int messagedest,char **messages){
  struct stat st;
  
  if(scsi_cdroms==-1){
    scsi_cdroms=0;
    {
      /* /Proc exists? */
      if(stat("/proc",&st)){
	/* Huh.  No /proc filesystem.  Bitch about it. */
	idperror(messagedest,messages,"\t\tCan't autoscan SCSI-- /proc unavailable",NULL);
	return(1);
      }
      if(stat("/proc/scsi/scsi",&st)){
	switch(errno){
	case ENOENT:
	  /* No SCSI on this machine, no SCSI CD drives */
	  return(0);
	default:
	  idperror(messagedest,messages,"\t\tCan't autoscan SCSI-- /proc/scsi/scsi unavailable",NULL);
	  return(1);
	}
      }
    }
	
    {
      /* How many SCSI CDROMs do we have? */
      int fd=open("/proc/scsi/scsi",O_RDONLY);
      if(fd==-1){
	idperror(messagedest,messages,"\t\tCan't autoscan SCSI-- /proc/scsi/scsi unavailable",NULL);
	return(1);
      }
      
      {
	char *line;
	while ((line=getline(fd))){
	  if(strstr(line,"Type:"))
	    if(strstr(line,"CD-ROM") || strstr(line,"WORM"))
	      scsi_cdroms++;
	  free(line);
	}
      }
      close(fd);
    }

    scsi_cdrom_list=calloc(scsi_cdroms,sizeof(scsi_cdrom_device));

    {
      int fd=open("/proc/scsi/scsi",O_RDONLY);
      if(fd==-1){
	idperror(messagedest,messages,"\t\tCan't autoscan SCSI-- /proc/scsi/scsi unavailable",NULL);
	scsi_cdroms=0;
	return(1);
      }
      
      {
	char *line;
	/* Linux fills in the generic and cdrom device names sequentially */
	int cdrom_i=0;
	int device_i=0;
	char *description=NULL;
	char vendor[80];
	char model[80];
	
	while ((line=getline(fd))){
	  if(strstr(line,"Vendor:")){
	    sscanf(line,"Vendor: %79s Model: %79[^\n]",vendor,model);
	    if(description)free(description);
	    description=malloc(strlen(vendor)+strlen(model)+2);
	    sprintf(description,"%s %s",vendor,model);
	  }
	  if(strstr(line,"Type:")){
	    if(strstr(line,"CD-ROM") || strstr(line,"WORM")){
	      char buffer[MAX_DEV_LEN];
	      int pattern=0;
	      
	      scsi_cdrom_list[cdrom_i].model=description;
	      description=NULL;
	      
	      /* try the possible prefixes for this mapping */
	      /* cd specific device mapping */
	      while(scsi_cdrom_prefixes[pattern]!=NULL){
		
		/* number */
		sprintf(buffer,"%s%d",scsi_cdrom_prefixes[pattern],cdrom_i);
		if(!stat(buffer,&st)){
		  scsi_cdrom_list[cdrom_i].specific_device=copystring(buffer);
		  break;
		}
		
		/* letter */
		sprintf(buffer,"%s%c",scsi_cdrom_prefixes[pattern],cdrom_i+97);
		if(!stat(buffer,&st)){
		  scsi_cdrom_list[cdrom_i].specific_device=copystring(buffer);
		  break;
		}
		
		pattern++;
	      }
	      
	      /* cd generic device mapping */
	      pattern=0;
	      while(scsi_generic_prefixes[pattern]!=NULL){
		
		/* number */
		sprintf(buffer,"%s%d",scsi_generic_prefixes[pattern],device_i);
		if(!stat(buffer,&st)){
		  scsi_cdrom_list[cdrom_i].generic_device=copystring(buffer);
		  break;
		}
		
		/* letter */
		sprintf(buffer,"%s%c",scsi_generic_prefixes[pattern],device_i+97);
		if(!stat(buffer,&st)){
		  scsi_cdrom_list[cdrom_i].generic_device=copystring(buffer);
		  break;
		}
		
		pattern++;
	      }
	      cdrom_i++;
	      if(cdrom_i==scsi_cdroms)break;
	    }
	    device_i++;
	  }
	  free(line);
	  line=NULL;
	}
	if(line)free(line);
	if(description)free(description);
	close(fd);
      }
    }
  }
  return(0);
}

int lookup_scsi_drive_pair(char **cdrom,char **generic, int messagedest,
			   char **messages){
  /* complete the pair */
  int i;

  if(scsi_cdroms==-1)
    if(list_scsi_cdrom_devices(messagedest,messages))
      return(1);

  if(*cdrom){
    for(i=0;i<scsi_cdroms;i++){
      if(!strcmp(scsi_cdrom_list[i].specific_device,*cdrom))
	if(scsi_cdrom_list[i].generic_device){
	  *generic=copystring(scsi_cdrom_list[i].generic_device);
	  return(0);
	}
    }

    return(1);
  }

  if(*generic){
    for(i=0;i<scsi_cdroms;i++){
      if(!strcmp(scsi_cdrom_list[i].generic_device,*generic))
	if(scsi_cdrom_list[i].specific_device){
	  *cdrom=copystring(scsi_cdrom_list[i].specific_device);
	  return(0);
	}
    }
  }
  return(1);
}

/* takes the specific device, not the generic */
char *lookup_scsi_description(const char *cdrom,int messagedest,
			      char **messages){
  int i;

  if(scsi_cdroms==-1)
    if(list_scsi_cdrom_devices(messagedest,messages))
      return(NULL);
  
  for(i=0;i<scsi_cdroms;i++){
    if(!strcmp(scsi_cdrom_list[i].specific_device,cdrom))
      return(scsi_cdrom_list[i].model);
  }
  return(NULL);
}

cdrom_drive *cdda_identify_scsi(const char *generic_device, 
				const char *ioctl_device, int messagedest,
				char **messages){
  
  cdrom_drive *d=NULL;
  struct stat i_st;
  struct stat g_st;
  int i_fd=-1;
  int g_fd=-1;
  int type;

  if(generic_device)
    idmessage(messagedest,messages,"\tTesting %s for SCSI interface",
	      generic_device);
  else
    if(ioctl_device)
    idmessage(messagedest,messages,"\tTesting %s for SCSI interface",
	      ioctl_device);


  /* Do this first; it's wasteful, but the messages make more sense */
  if(generic_device){
    if(stat(generic_device,&g_st)){
      idperror(messagedest,messages,"\t\tCould not access device %s",
	       generic_device);
      return(NULL);
    }
    if((int)(g_st.st_rdev>>8)!=SCSI_GENERIC_MAJOR)
      if((int)(g_st.st_rdev>>8)!=SCSI_CDROM_MAJOR){
	idmessage(messagedest,messages,"\t\t%s is not a SCSI device",
		  generic_device);
	return(NULL);
      }else{
	char *temp=generic_device;
	generic_device=ioctl_device;
	ioctl_device=temp;
      }
  }
  if(ioctl_device){
    if(stat(ioctl_device,&i_st)){
      idperror(messagedest,messages,"\t\tCould not access device %s",
	       ioctl_device);
      return(NULL);
    }
    if((int)(i_st.st_rdev>>8)!=SCSI_CDROM_MAJOR)
      if((int)(i_st.st_rdev>>8)!=SCSI_GENERIC_MAJOR){
	idmessage(messagedest,messages,"\t\t%s is not a SCSI device",
		  ioctl_device);
	return(NULL);
      }else{
	char *temp=generic_device;
	generic_device=ioctl_device;
	ioctl_device=temp;
      }
  }

  /* we need to resolve any symlinks for the lookup code to work */

  if(generic_device){
    generic_device=test_resolve_symlink(generic_device,messagedest,messages);
    if(generic_device==NULL)return(NULL);
  }
  if(ioctl_device){
    ioctl_device=test_resolve_symlink(ioctl_device,messagedest,messages);
    if(ioctl_device==NULL)goto cdda_identify_scsi_fail;
  }

  if(!generic_device || !ioctl_device){
    if(generic_device)
      idmessage(messagedest,messages,"\t\tLooking for companion device to %s",
		generic_device);
    else
      if(ioctl_device)idmessage(messagedest,messages,"\t\tLooking for "
				"companion device to %s",ioctl_device);

    if(lookup_scsi_drive_pair((char **)&ioctl_device,(char **)&generic_device,
			      messagedest,messages))
      /* Huh.  Switched maybe? */
      if(lookup_scsi_drive_pair((char **)&generic_device,(char **)&ioctl_device,
				messagedest,messages)){
	/* Not switched.  Absent */
	if(generic_device)
	  idmessage(messagedest,messages,"\t\tCould not find SCSI device pair"
		    " for %s",generic_device);
	else
	  if(ioctl_device)	  
	    idmessage(messagedest,messages,"\t\tCould not find SCSI device "
		      "pair for %s",ioctl_device);
	goto cdda_identify_scsi_fail;
      }else{
	/* Yea, switched */
	const char *temp=ioctl_device;
	ioctl_device=generic_device;
	generic_device=temp;
      }
    idmessage(messagedest,messages,"\t\tFound scsi device pair:",NULL);
    idmessage(messagedest,messages,"\t\t\tgeneric device: %s",generic_device);
    idmessage(messagedest,messages,"\t\t\tioctl device: %s",ioctl_device);
  }

  
  if(stat(generic_device,&g_st)){
    idperror(messagedest,messages,"\t\tCould not access generic SCSI device "
	     "%s",generic_device);

    goto cdda_identify_scsi_fail;
  }
  if(stat(ioctl_device,&i_st)){
    idperror(messagedest,messages,"\t\tCould not access SCSI cdrom device "
	     "%s",generic_device);
    goto cdda_identify_scsi_fail;
  }

  i_fd=open(ioctl_device,O_RDONLY);
  g_fd=open(generic_device,O_RDWR);
  
  if(i_fd==-1){
    idperror(messagedest,messages,"\t\tCould not open SCSI cdrom device "
	     "%s",ioctl_device);
    goto cdda_identify_scsi_fail;
  }
  if(g_fd==-1){
    idperror(messagedest,messages,"\t\tCould not open generic SCSI device "
	     "%s",generic_device);
    goto cdda_identify_scsi_fail;
  }

  type=(int)(i_st.st_rdev>>8);

  if(type==SCSI_CDROM_MAJOR){
    if (!S_ISBLK(i_st.st_mode)) {
      idmessage(messagedest,messages,"\t\tSCSI CDROM device %s not a "
		"block device",ioctl_device);
      goto cdda_identify_scsi_fail;
    }
  }else{
    idmessage(messagedest,messages,"\t\tSCSI CDROM device %s has wrong "
	      "major number",ioctl_device);
    goto cdda_identify_scsi_fail;
  }

  if((int)(g_st.st_rdev>>8)==SCSI_GENERIC_MAJOR){
    if (!S_ISCHR(g_st.st_mode)) {
      idmessage(messagedest,messages,"\t\tGeneric SCSI device %s not a "
		"char device",generic_device);
      goto cdda_identify_scsi_fail;
    }
  }else{
    idmessage(messagedest,messages,"\t\tGeneric SCSI device %s has wrong "
	      "major number",generic_device);
    goto cdda_identify_scsi_fail;
  }
  
  d=calloc(1,sizeof(cdrom_drive));
  d->cdda_device_name=copystring(generic_device);
  d->ioctl_device_name=copystring(ioctl_device);
  d->drive_type=type;
  d->cdda_fd=g_fd;
  d->ioctl_fd=i_fd;
  d->interface=GENERIC_SCSI;
  d->bigendianp=-1; /* We don't know yet... */
  d->nsectors=-1;
  d->drive_model=copystring(lookup_scsi_description(ioctl_device,messagedest,
						    messages));
  idmessage(messagedest,messages,"\t\tCDROM sensed: %s",d->drive_model);
  
  return(d);
  
cdda_identify_scsi_fail:
  if(generic_device)free((char *)generic_device);
  if(ioctl_device)free((char *)ioctl_device);
  if(i_fd==-1)close(i_fd);
  if(g_fd==-1)close(g_fd);
  return(NULL);
}

#ifdef CDDA_TEST

cdrom_drive *cdda_identify_test(const char *filename, int messagedest,
				char **messages){
  
  cdrom_drive *d=NULL;
  struct stat st;
  int fd=-1;

  idmessage(messagedest,messages,"\tTesting %s for file/test interface",
	    filename);

  if(stat(filename,&st)){
    idperror(messagedest,messages,"\t\tCould not access file %s",
	     filename);
    return(NULL);
  }

  if(!S_ISREG(st.st_mode)){
    idmessage(messagedest,messages,"\t\t%s is not a regular file",
		  filename);
    return(NULL);
  }

  fd=open(filename,O_RDONLY);
  
  if(fd==-1){
    idperror(messagedest,messages,"\t\tCould not open file %s",filename);
    return(NULL);
  }
  
  d=calloc(1,sizeof(cdrom_drive));
  d->cdda_device_name=copystring(filename);
  d->ioctl_device_name=copystring(filename);
  d->drive_type=-1;
  d->cdda_fd=fd;
  d->ioctl_fd=fd;
  d->interface=TEST_INTERFACE;
  d->bigendianp=-1; /* We don't know yet... */
  d->nsectors=-1;
  d->drive_model=copystring("File based test interface");
  idmessage(messagedest,messages,"\t\tCDROM sensed: %s",d->drive_model);
  
  return(d);
}

#endif
