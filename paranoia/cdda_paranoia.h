/***
 * CopyPolicy: GNU Public License 2 applies
 * Copyright (C) by Monty (xiphmont@mit.edu)
 *
 ***/

#define PARANOIA_CB_READ           0
#define PARANOIA_CB_VERIFY         1
#define PARANOIA_CB_FIXUP_EDGE     2
#define PARANOIA_CB_FIXUP_ATOM     3
#define PARANOIA_CB_SCRATCH        4
#define PARANOIA_CB_REPAIR         5
#define PARANOIA_CB_SKIP           6
#define PARANOIA_CB_DRIFT          7

#define PARANOIA_MODE_FULL        0xff
#define PARANOIA_MODE_DISABLE     0

#define PARANOIA_MODE_VERIFY      1
#define PARANOIA_MODE_OVERLAP     2
#define PARANOIA_MODE_SCRATCH     4
#define PARANOIA_MODE_REPAIR      8

typedef struct p_block{
  size16 *buffer;
  int stamp;

  long begin;
  long end;
  long verifybegin;
  long verifyend;

  struct cdrom_paranoia *p;
  struct p_block *prev;
  struct p_block *next;
} p_block;

typedef struct cdrom_paranoia{
  cdrom_drive *d;

  p_block root;
  p_block *fragments;
  p_block *tail;

  p_block *free;

  p_block *ptr;
  int cache;
  int readahead;
  int jitter;
  int enable;
  long cursor;

} cdrom_paranoia;

extern cdrom_paranoia *paranoia_init(cdrom_drive *d,int cache, 
				     int readahead);
extern void paranoia_modeset(cdrom_paranoia *p,int mode);
extern long paranoia_seek(cdrom_paranoia *p,long seek,int mode);
extern size16 *paranoia_read(cdrom_paranoia *p,long sectors,
			     void(*callback)(long,int));
extern void paranoia_free(cdrom_paranoia *p);

