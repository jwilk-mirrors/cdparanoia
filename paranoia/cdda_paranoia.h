/***
 * CopyPolicy: GNU Public License 2 applies
 * Copyright (C) by Monty (xiphmont@mit.edu)
 *
 ***/

#ifndef _CDROM_PARANOIA_
#define _CDROM_PARANOIA_

#define CD_FRAMEWORDS (CD_FRAMESIZE_RAW/2)

#define PARANOIA_CB_READ           0
#define PARANOIA_CB_VERIFY         1
#define PARANOIA_CB_FIXUP_EDGE     2
#define PARANOIA_CB_FIXUP_ATOM     3
#define PARANOIA_CB_SCRATCH        4
#define PARANOIA_CB_REPAIR         5
#define PARANOIA_CB_SKIP           6
#define PARANOIA_CB_DRIFT          7
#define PARANOIA_CB_BACKOFF        8

#define PARANOIA_MODE_FULL        0xff
#define PARANOIA_MODE_DISABLE     0

#define PARANOIA_MODE_VERIFY      1
#define PARANOIA_MODE_OVERLAP     2
#define PARANOIA_MODE_SCRATCH     4
#define PARANOIA_MODE_REPAIR      8

typedef struct p_block{
  size16 *buffer;
  long size;
  int stamp;

  long begin;
  long end;
  long verifybegin;
  long verifyend;

  /* silence detection */
  long silence;

  /* end of session cases */
  long lastsector;
  long done;

  /* Keep track of drift */
  long offset;

  /* linked list */
  struct cdrom_paranoia *p;
  struct p_block *prev;
  struct p_block *next;
} p_block;

typedef struct cdrom_paranoia{
  cdrom_drive *d;

  long skiplimit;       /* don;t do fragmentation before this boundary */
  p_block root;         /* the buffer we return; final accumulator */
  p_block *fragments;   /* head of the fragments list read from disc */
  p_block *tail;        /* tail of the fragments list */

  p_block *free;        /* head of free link list */

  p_block **ptr;        /* array of all links */
  long ptrblocks;
  long total_bufsize;   /* how much bufsize is allocated? */

  long cachemark;       /* reclaimation watermark for total bufsize */
  int readahead;        /* sectors of readahead in each readop */

  int jitter;           
  int enable;
  long cursor;
  long current_lastsector;
  long current_firstsector;

  /* statistics for drift/overlap */
  long offpoints;
  long offaccum;
  long offdiff;
  long offmin;
  long offmax;

  long dynoverlap;
  long dyndrift;

  /* statistics for verification */

} cdrom_paranoia;

extern cdrom_paranoia *paranoia_init(cdrom_drive *d,int cache, 
				     int readahead);
extern void paranoia_modeset(cdrom_paranoia *p,int mode);
extern long paranoia_seek(cdrom_paranoia *p,long seek,int mode);
extern size16 *paranoia_read(cdrom_paranoia *p,void(*callback)(long,int));
extern void paranoia_free(cdrom_paranoia *p);

#endif
