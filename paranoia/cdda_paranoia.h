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
#define PARANOIA_CB_OVERLAP        9
#define PARANOIA_CB_FIXUP_DROPPED 10
#define PARANOIA_CB_FIXUP_DUPED   11
#define PARANOIA_CB_READERR       12

#define PARANOIA_MODE_FULL        0xff
#define PARANOIA_MODE_DISABLE     0

#define PARANOIA_MODE_VERIFY      1
#define PARANOIA_MODE_FRAGMENT    2
#define PARANOIA_MODE_OVERLAP     4
#define PARANOIA_MODE_SCRATCH     8
#define PARANOIA_MODE_REPAIR      16

typedef struct c_block{
  int stamp;

  char *flags;
  size16 *buffer;
  long begin;
  long end;

  /* end of session cases */
  long lastsector;

  /* linked list */
  struct cdrom_paranoia *p;
  struct c_block *prev;
  struct c_block *next;
} c_block;

typedef struct v_fragment{
  c_block *one;
  c_block *two;
  
  int stamp;
  long begin;
  long end;
  long offset;

  /* end of session cases */
  long lastsector;

  /* linked list */
  struct cdrom_paranoia *p;
  struct v_fragment *prev;
  struct v_fragment *next;

} v_fragment;

/* I want polymorphism!! */

typedef struct c_list{
  c_block *head;
  c_block *tail;
  c_block *free;
  c_block **pool;
  long blocks;
  long limit;
  long current;
} c_list;

typedef struct v_list{
  v_fragment *head;
  v_fragment *tail;
  v_fragment *free;
  v_fragment **pool;
  long blocks;
  long active;
} v_list;

typedef struct root_block{
  long returnedlimit;   
  size16 *buffer;    /* verified/reconstructed cached data */
  long begin;
  long end;
  long done;
} root_block;

typedef struct cdrom_paranoia{
  cdrom_drive *d;

  root_block root;     /* verified/reconstructed cached data */
  c_list cache;        /* our data as read from the cdrom */
  v_list fragments;    /* fragments of blocks that have been 'verified' */

  int readahead;        /* sectors of readahead in each readop */
  int jitter;           
  long lastread;

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

extern cdrom_paranoia *paranoia_init(cdrom_drive *d);
extern void paranoia_modeset(cdrom_paranoia *p,int mode);
extern long paranoia_seek(cdrom_paranoia *p,long seek,int mode);
extern size16 *paranoia_read(cdrom_paranoia *p,void(*callback)(long,int));
extern void paranoia_free(cdrom_paranoia *p);

#endif
