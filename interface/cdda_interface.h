/******************************************************************
 * CopyPolicy: GNU Public License 2 applies
 * Copyright (C) 1998 Monty xiphmont@mit.edu
 * and Heiko Eissfeldt heiko@escape.colossus.de
 *
 * Toplevel interface header; applications include this
 *
 ******************************************************************/

#ifndef _cdda_interface_h_
#define _cdda_interface_h_

#ifndef CD_FRAMESIZE
#define CD_FRAMESIZE 2048
#endif
#ifndef CD_FRAMESIZE_RAW
#define CD_FRAMESIZE_RAW 2352
#endif
#define CD_FRAMESAMPLES (CD_FRAMESIZE_RAW / 4)

#include <sys/types.h>
#include <signal.h>

#define MAXTRK 100

typedef struct TOC {	/* structure of table of contents */
  unsigned char bFlags;
  unsigned char bTrack;
  size32 dwStartSector;
} TOC;

/* interface types */
#define GENERIC_SCSI	0
#define COOKED_IOCTL	1
#define TEST_INTERFACE	2

#define CDDA_MESSAGE_FORGETIT 0
#define CDDA_MESSAGE_PRINTIT 1
#define CDDA_MESSAGE_LOGIT 2

/* cdrom access function pointer */

void SetupInterface( unsigned char *int_name );

typedef struct cdrom_drive{

  int opened; /* This struct may just represent a candidate for opening */

  char *cdda_device_name;
  char *ioctl_device_name;

  int cdda_fd;
  int ioctl_fd;

  char *drive_model;
  int drive_type;
  int interface;
  int bigendianp;
  int nsectors;

  int cd_extra;
  int tracks;
  TOC disc_toc[MAXTRK];
  long audio_first_sector;
  long audio_last_sector;

  int errordest;
  int messagedest;
  char *errorbuf;
  char *messagebuf;

  /* functions specific to particular drives/interrfaces */

  int  (*enable_cdda)  (struct cdrom_drive *d, int onoff);
  int  (*read_toc)     (struct cdrom_drive *d);
  long (*read_audio)   (struct cdrom_drive *d, void *p, long begin, 
		       long sectors);
  int error_retry;

  int is_atapi;
  int is_mmc;

  /* SCSI command buffer and offset pointers */
  unsigned char *sg;
  unsigned char *sg_buffer;
  int clear_buff_via_bug;
  unsigned char inqbytes[4];

  /* Scsi parameters and state */
  unsigned char density;
  unsigned char orgdens;
  unsigned int orgsize;
  long bigbuff;
  int adjust_ssize;
  int fua;
  sigset_t sigset;

} cdrom_drive;

#define IS_AUDIO(d,i) (!(d->disc_toc[i].bFlags & 0x04))

/******** Identification/autosense functions */

extern cdrom_drive *cdda_find_a_cdrom(int messagedest, char **message);
extern cdrom_drive *cdda_identify(const char *device, int messagedest,
				  char **message);
extern cdrom_drive *cdda_identify_cooked(const char *device,int messagedest,
					 char **message);
extern cdrom_drive *cdda_identify_scsi(const char *generic_device, 
				       const char *ioctl_device,
				       int messagedest, char **message);
#ifdef CDDA_TEST
extern cdrom_drive *cdda_identify_test(const char *filename,
				       int messagedest, char **message);
#endif

/******** Drive oriented functions */

extern void cdda_verbose_set(cdrom_drive *d,int err_action, int mes_action);
extern char *cdda_messages(cdrom_drive *d);
extern char *cdda_errors(cdrom_drive *d);

extern int cdda_close(cdrom_drive *d);
extern int cdda_open(cdrom_drive *d);
extern long cdda_read(cdrom_drive *d, void *buffer,
		       long beginsector, long sectors);

extern long cdda_track_firstsector(cdrom_drive *d,int track);
extern long cdda_track_lastsector(cdrom_drive *d,int track);
extern long cdda_tracks(cdrom_drive *d);
extern int cdda_sector_gettrack(cdrom_drive *d,long sector);
extern int cdda_track_channels(cdrom_drive *d,int track);
extern int cdda_track_audiop(cdrom_drive *d,int track);
extern int cdda_track_copyp(cdrom_drive *d,int track);
extern int cdda_track_preemp(cdrom_drive *d,int track);
extern long cdda_disc_firstsector(cdrom_drive *d);
extern long cdda_disc_lastsector(cdrom_drive *d);

/* Errors returned by lib: 

001: Unable to set CDROM to read audio mode
002: Unable to read table of contents
003: CDROM reporting illegal number of tracks
004: Unable to read table of contents header
005: Unable to read table of contents entry
006: Could not read any data from drive
007: Unknown, unrecoverable error reading data
008: Unable to identify CDROM model
009: CDROM reporting illegal table of contents

100: Interface not supported
101: Drive is neither a CDROM nor a WORM device
102: Permision denied on cdrom (ioctl) device
103: Permision denied on cdrom (data) device

300: Kernel memory error

400: Device not open
401: Invalid track number
402: Track not audio data
403: No audio tracks on disc

*/
#endif

