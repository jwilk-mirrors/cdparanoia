/*
 * Copyright: GNU Public License 2 applies
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2, or (at your option)
 *   any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * cdparanoia (C) 1998 Monty <xiphmont@mit.edu>
 *
 * last changes:
 *   22.01.98 - first version
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <getopt.h>
#include <errno.h>
#include <math.h>
#include <sys/time.h>

#include "interface/cdda_interface.h"
#include "paranoia/cdda_paranoia.h"
#include "utils.h"
#include "report.h"
#include "version.h"
#include "header.h"

extern int verbose;
extern int quiet;

static long parse_offset(cdrom_drive *d, char *offset, int begin){
  long track=-1;
  long hours=-1;
  long minutes=-1;
  long seconds=-1;
  long sectors=-1;
  char *time=NULL,*temp=NULL;
  long ret;

  if(offset==NULL)return(-1);

  /* seperate track from time offset */
  temp=strchr(offset,']');
  if(temp){
    *temp='\0';
    temp=strchr(offset,'[');
    if(temp==NULL){
      report("Error parsing span argument");
      exit(1);
    }
    *temp='\0';
    time=temp+1;
  }

  /* parse track */
  {
    int chars=strspn(offset,"0123456789");
    if(chars>0){
      offset[chars]='\0';
      track=atoi(offset);
      if(track<1 || track>d->tracks){
	char buffer[256];
	sprintf(buffer,"Track #%ld does not exist.",track);
	report(buffer);
	exit(1);
      }
    }
  }

  while(time){
    long val,chars;
    char *sec=strrchr(time,'.');
    if(!sec)sec=strrchr(time,':');
    if(!sec)sec=time-1;

    chars=strspn(sec+1,"0123456789");
    if(chars)
      val=atoi(sec+1);
    else
      val=0;
    
    switch(*sec){
    case '.':
      if(sectors!=-1){
	report("Error parsing span argument");
	exit(1);
      }
      sectors=val;
      break;
    default:
      if(seconds==-1)
	seconds=val;
      else
	if(minutes==-1)
	  minutes=val;
	else
	  if(hours==-1)
	    hours=val;
	  else{
	    report("Error parsing span argument");
	    exit(1);
	  }
      break;
    }
	 
    if(sec<=time)break;
    *sec='\0';
  }

  if(track==-1){
    if(seconds==-1 && sectors==-1)return(-1);
    if(begin==-1)
      ret=cdda_disc_firstsector(d);
    else
      ret=begin;
  }else{
    if(seconds==-1 && sectors==-1)
      if(begin==-1) /* first half of a span */
	return(cdda_track_firstsector(d,track));
      else
	return(cdda_track_lastsector(d,track));
    else
      /* relative offset into a track */
      ret=cdda_track_firstsector(d,track);
  }
   
  /* OK, we had some sort of offset into a track */

  if(sectors!=-1)ret+=sectors;
  if(seconds!=-1)ret+=seconds*75;
  if(minutes!=-1)ret+=minutes*60*75;
  if(hours!=-1)ret+=hours*60*60*75;

  /* We don't want to outside of the track; if it's relative, that's OK... */
  if(track!=-1){
    if(cdda_sector_gettrack(d,ret)!=track){
      report("Time/sector offset goes beyond end of specified track.");
      exit(1);
    }
  }

  /* Don't pass up end of session */

  if(ret>cdda_disc_lastsector(d)){
    report("Time/sector offset goes beyond end of disc.");
    exit(1);
  }

  return(ret);
}

static void display_toc(cdrom_drive *d){
  int i;
  report("\nTable of contents (audio tracks only):\n"
	 "track        length               begin                  flags\n"
	 "============================================================================");
  
  for(i=1;i<=d->tracks;i++)
    if(cdda_track_audiop(d,i)){
      char buffer[256];

      long sec=cdda_track_firstsector(d,i);
      long off=cdda_track_lastsector(d,i)-sec+1;
      
      sprintf(buffer,
	      "%3d.  %7ld [%02d:%02d.%02d]  %7ld [%02d:%02d.%02d]  %s, %s, %s",
	      i,
	      off,(int)(off/(60*75)),(int)((off/75)%60),(int)(off%75),
	      sec,(int)(sec/(60*75)),(int)((sec/75)%60),(int)(sec%75),
	      cdda_track_copyp(d,i)?"copy OK":"no copy",
	      cdda_track_preemp(d,i)?"preemp":"no preemp",
	      cdda_track_channels(d,i)==2?"2 channels":"4 channels");
      report(buffer);
    }
  report("");
}

static void usage(void){
  fprintf( stderr,
VERSION"\n\n"

"USAGE:\n"
"  cdda_paranoia [options] <span> [outfile]\n\n"

"OPTIONS:\n"
"  -v --verbose                    : extra verbose operation\n"
"  -q --quiet                      : quiet operation\n"
"  -V --version                    : print version info and quit\n"
"  -Q --query                      : autosense drive, query disc and quit\n"
"  -h --help                       : print help\n\n"

"  -p --output-raw                 : output raw 16 bit PCM in host byte \n"
"                                    order\n"
"  -r --output-raw-little-endian   : output raw 16 bit little-endian PCM\n"
"  -R --output-raw-big-endian      : output raw 16 bit big-endian PCM\n"
"  -w --output-wav                 : output as wav file (default)\n"
"  -a --output-aifc                : output as aifc file\n"
"  -i --output-info <file>         : output human readable ripping info to\n"
"                                    file\n\n"

"  -c --force-cdrom-little-endian  : force treating drive as little endian\n"
"  -C --force-cdrom-big-endian     : force treating drive as big endian\n"
"  -n --force-default-sectors  <n> : force default number of sectors in read\n"
"                                    to n sectors\n"
"  -d --force-cdrom-device   <dev> : use specified device; disallow \n"
"                                    autosense\n"
"  -g --force-generic-device <dev> : use specified generic scsi device\n\n"

"  -Z --disable-paranoia           : disable all paranoia checking\n"
"  -Y --disable-extra-paranoia     : only do cdda2wav-style overlap checking\n"
"  -X --disable-scratch-detection  : do not look for scratches\n"
"  -W --disable-scratch-repair     : disable scratch repair (still detect)\n\n"

"The span argument may be a simple track number or a offset/span\n"
"specification.  The syntax of an offset/span takes the rough form:\n\n"
  
"                       1[ww:xx:yy.zz]-2[aa:bb:cc.dd] \n\n"

"Here, 1 and 2 are track numbers; the numbers in brackets provide a\n"
"finer grained offset within a particular track. [aa:bb:cc.dd] is in\n"
"hours/minutes/seconds/sectors format. Zero fields need not be\n"
"specified: [::20], [:20], [20], [20.], etc, would be interpreted as\n"
"twenty seconds, [10:] would be ten minutes, [.30] would be thirty\n"
"sectors (75 sectors per second).\n\n"

"When only a single offset is supplied, it is interpreted as a starting\n"
"offset and ripping will continue to the end of he track.  If a single\n"
"offset is preceeded or followed by a hyphen, the implicit missing\n"
"offset is taken to be the start or end of the disc, respectively. Thus:\n\n"

"    1:[20.35]    Specifies ripping from track 1, second 20, sector 35 to \n"
"                 the end of track 1.\n\n"

"    1:[20.35]-   Specifies ripping from 1[20.35] to the end of the disc\n\n"

"    -2           Specifies ripping from the beginning of the disc up to\n"
"                 (and including) track 2\n\n"

"    -2:[30.35]   Specifies ripping from the beginning of the disc up to\n"
"                 2:[30.35]\n\n"

"    2-4          Specifies ripping from the beginning of track two to the\n"
"                 end of track 4.\n\n"

"Don't forget to protect square brackets and preceeding hyphens from\n"
"the shell...\n\n"

"Bug reports should go to xiphmont@mit.edu\n\n");
}

long callbegin;
long callend;

static void callback(long sector, int function){

  /* (== PROGRESS == [--+:---x-------------->           ] == 000000 == . ==) */

  char buffer[256];
  static long c_sector=0;
  static char dispcache[30]="                              ";
  static int last=0;
  static long lasttime=0;
  struct timeval thistime;
  int graph=30;
  char heartbeat;
  int position=0,aheadposition=0;

  sector/=CD_FRAMESIZE_RAW/2;

  position=((float)(sector-callbegin)/
		    (callend-callbegin))*graph;

  if(function==PARANOIA_CB_READ)if(sector>c_sector)c_sector=sector;

  aheadposition=((float)(c_sector-callbegin)/
		 (callend-callbegin))*graph;


  if(position<graph && position>=0)
    switch(function){
    case PARANOIA_CB_FIXUP_EDGE:
      if(dispcache[position]==' ') 
	dispcache[position]='-';
      break;
    case PARANOIA_CB_FIXUP_ATOM:
      if(dispcache[position]==' ' ||
	 dispcache[position]=='-')
	dispcache[position]='+';
      break;
    case PARANOIA_CB_SKIP:
      dispcache[position]='X';
      break;
    }

  switch(last){
  case 0:
    heartbeat=' ';
    break;
  case 1:case 7:
    heartbeat='.';
    break;
  case 2:case 6:
    heartbeat='o';
    break;
  case 3:case 5:  
    heartbeat='0';
    break;
  case 4:
    heartbeat='O';
    break;
  }
  
  if(!quiet){
    long test;
    gettimeofday(&thistime,NULL);
    test=thistime.tv_sec*10+thistime.tv_usec/100000;
    
    if(lasttime!=test){
      last++;
      lasttime=test;
      if(last>7)last=0;

      sprintf(buffer,
	      "\r  (== PROGRESS == [%s] == %06ld == %c ==)     ",
	      dispcache,c_sector,heartbeat);
      
      if(aheadposition>=0 && aheadposition<graph)
	buffer[aheadposition+20]='>';
      
      fprintf(stderr,buffer);
    }
  }
}

const char *optstring = "cCn:d:g:prRwavqVQhZYXW";

struct option options [] = {
	{"force-cdrom-little-endian",no_argument,NULL,'c'},
	{"force-cdrom-big-endian",no_argument,NULL,'C'},
	{"force-default-sectors",required_argument,NULL,'n'},
	{"force-cdrom-device",required_argument,NULL,'d'},
	{"force-generic-device",required_argument,NULL,'g'},
	{"output-raw",no_argument,NULL,'p'},
	{"output-raw-little-endian",no_argument,NULL,'r'},
	{"output-raw-big-endian",no_argument,NULL,'R'},
	{"output-wav",no_argument,NULL,'w'},
	{"output-aifc",no_argument,NULL,'a'},
	{"verbose",no_argument,NULL,'v'},
	{"quiet",no_argument,NULL,'q'},
	{"version",no_argument,NULL,'V'},
	{"query",no_argument,NULL,'Q'},
	{"help",no_argument,NULL,'h'},
	{"disable-paranoia",no_argument,NULL,'Z'},
	{"disable-extra-paranoia",no_argument,NULL,'Y'},
	{"disable-scratch-detection",no_argument,NULL,'X'},
	{"disable-scratch-repair",no_argument,NULL,'W'},
	{"output-info",required_argument,NULL,'i'},

	{NULL,0,NULL,0}
};

long blocking_write(int outf, char *buffer, long num){
  long words=0,temp;

  while(words<num){
    temp=write(outf,buffer+words,num-words);
    if(temp==-1 && errno!=EINTR && errno!=EAGAIN)
      return(-1);
    words+=temp;
  }
  return(0);
}

int main(long argc,char *argv[]){
  int force_cdrom_endian=-1;
  int force_cdrom_sectors=-1;
  char *force_cdrom_device=NULL;
  char *force_generic_device=NULL;
  char *span=NULL;
  int output_type=1; /* 0=raw, 1=wav, 2=aifc */
  int output_endian=0; /* -1=host, 0=little, 1=big */
  int query_only=0;

  int paranoia_mode=PARANOIA_MODE_FULL; /* full paranoia */

  char *info_file=NULL;
  int out;

  cdrom_drive *d;
  int c,long_option_index;

  while((c=getopt_long(argc,argv,optstring,options,&long_option_index))!=EOF){
    switch(c){
    case 'c':
      force_cdrom_endian=0;
      break;
    case 'C':
      force_cdrom_endian=1;
      break;
    case 'n':
      force_cdrom_sectors=atoi(optarg);
      break;
    case 'd':
      if(force_cdrom_device)free(force_cdrom_device);
      force_cdrom_device=copystring(optarg);
      break;
    case 'g':
      if(force_generic_device)free(force_generic_device);
      force_generic_device=copystring(optarg);
      break;
    case 'p':
      output_type=0;
      output_endian=-1;
      break;
    case 'r':
      output_type=0;
      output_endian=0;
      break;
    case 'R':
      output_type=0;
      output_endian=1;
      break;
    case 'w':
      output_type=1;
      output_endian=0;
      break;
    case 'a':
      output_type=2;
      output_endian=1;
      break;
    case 'v':
      verbose=CDDA_MESSAGE_PRINTIT;
      quiet=0;
      break;
    case 'q':
      verbose=CDDA_MESSAGE_FORGETIT;
      quiet=1;
      break;
    case 'V':
      fprintf(stderr,VERSION);
      exit(0);
      break;
    case 'Q':
      query_only=1;
      break;
    case 'h':
      usage();
      exit(0);
    case 'Z':
      paranoia_mode=PARANOIA_MODE_DISABLE; 
      break;
    case 'Y':
      paranoia_mode=PARANOIA_MODE_OVERLAP; /* cdda2wav style overlap 
						check only */
      break;
    case 'X':
      paranoia_mode&=~(PARANOIA_MODE_SCRATCH|PARANOIA_MODE_REPAIR);
      break;
    case 'W':
      paranoia_mode&=PARANOIA_MODE_REPAIR;
      break;
    case 'i':
      if(info_file)free(info_file);
      info_file=copystring(info_file);
      break;
    default:
      usage();
      exit(1);
    }
  }

  if(optind>=argc && !query_only){
    /* D'oh.  No span. Fetch me a brain, Igor. */
    usage();
    exit(1);
  }
  span=copystring(argv[optind]);

  report(VERSION"\n");

  /* Query the cdrom/disc; we may need to override some settings */

  if(force_generic_device)
    d=cdda_identify_scsi(force_generic_device,force_cdrom_device,verbose,NULL);
  else
    if(force_cdrom_device)
      d=cdda_identify(force_cdrom_device,verbose,NULL);
    else
      d=cdda_find_a_cdrom(verbose,NULL);

  if(!d){
    if(!verbose)
      report("Unable to open cdrom drive; -v will give more information.");
    exit(1);
  }

  if(verbose)
    cdda_verbose_set(d,CDDA_MESSAGE_PRINTIT,CDDA_MESSAGE_PRINTIT);
  else
    cdda_verbose_set(d,CDDA_MESSAGE_PRINTIT,CDDA_MESSAGE_FORGETIT);

  /* possibly force hand on endianness of drive, sector request size */
  if(force_cdrom_endian!=-1){
    d->bigendianp=force_cdrom_endian;
    switch(force_cdrom_endian){
    case 0:
      report("Forcing CDROM sense to little-endian; ignoring preset and autosense");
      break;
    case 1:
      report("Forcing CDROM sense to big-endian; ignoring preset and autosense");
      break;
    }
  }
  if(force_cdrom_sectors!=-1){
    if(force_cdrom_sectors<0 || force_cdrom_sectors>100){
      report("Default sector read size must be 1<= n <= 10\n");
      cdda_close(d);
      exit(1);
    }
    {
      char buffer[256];
      sprintf(buffer,"Forcing default to read %d sectors; "
	      "ignoring preset and autosense",force_cdrom_sectors);
      report(buffer);
      d->nsectors=force_cdrom_sectors;
    }
  }

  if(cdda_open(d)){
    report("Unable to open disc.  Is there an audio CD in the drive?");
    exit(1);
  }

  /* Dump the TOC */
  if(query_only || verbose)display_toc(d);
  if(query_only)exit(0);

  if(d->interface==GENERIC_SCSI && d->bigbuff==CD_FRAMESIZE_RAW){
    report("WARNING: You kernel does not have generic SCSI 'SG_BIG_BUFF'\n"
	   "         set, or it is set to a very small value.  Paranoia\n"
	   "         will only be able to perform single sector reads\n"
	   "         making it very unlikely Paranoia can work.\n\n"
	   "         To correct this problem, the SG_BIG_BUFF define\n"
	   "         must be set in /usr/src/linux/include/scsi/sg.h\n"
	   "         by placing, for example, the following line just\n"
	   "         before the last #endif:\n\n"
	   "         #define SG_BIG_BUFF 65536\n\n"
	   "         and then recompiling the kernel.\n\n"
	   "         Attempting to continue...\n\n");
  }

  if(d->nsectors==1){
    report("WARNING: The autosensed/selected sectors per read value is\n"
	   "         one sector, making it very unlikely Paranoia can \n"
	   "         work.\n\n"
	   "         Attempting to continue...\n\n");
  }

  /* argv[optind] is the span, argv[optind+1] (if exists) is the outfile */

  if(optind+1<argc){
    if(!strcmp(argv[optind+1],"-")){
      out=dup(fileno(stdout));
      report("outputting to stdout");
    }else{
      out=open(argv[optind+1],O_RDWR|O_CREAT|O_TRUNC,0777);
      if(out==-1){
	report3("Cannot open specified output file %s: %s",argv[optind+1],
		strerror(errno));
	cdda_close(d);
	exit(1);
      }
      report2("outputting to %s",argv[optind+1]);
    }
  }else{
    /* default */
    char *def;

    switch(output_type){
    case 0: /* raw */
      def="cdda.raw";
      break;
    case 1:
      def="cdda.wav";
      break;
    case 2:
      def="cdda.aifc";
      break;
    }

    out=open(def,O_RDWR|O_CREAT|O_TRUNC,0777);
    if(out==-1){
      report3("Cannot open default output file %s: %s",def,
	      strerror(errno));
      cdda_close(d);
      exit(1);
    }
    report2("outputting to %s",def);
  }

  /* parse the span, set up begin and end sectors */

  {
    long first_sector;
    long last_sector;

    /* look for the hyphen */ 
    char *span2=strchr(span,'-');
    if(strrchr(span,'-')!=span2){
      report("Error parsing span argument");
      cdda_close(d);
      exit(1);
    }

    if(span2!=NULL){
      *span2='\0';
      span2++;
    }

    first_sector=parse_offset(d,span,-1);
    if(first_sector==-1)
      last_sector=parse_offset(d,span2,cdda_disc_firstsector(d));
    else
      last_sector=parse_offset(d,span2,first_sector);

    if(first_sector==-1)
      if(last_sector==-1){
	report("Error parsing span argument");
	cdda_close(d);
	exit(1);
      }else
	first_sector=cdda_disc_firstsector(d);
    else
      if(last_sector==-1)
	if(span2) /* There was a hyphen */
	  last_sector=cdda_disc_lastsector(d);
	else
	  last_sector=
	    cdda_track_lastsector(d,cdda_sector_gettrack(d,first_sector));

    {
      char buffer[250];
      int track1=cdda_sector_gettrack(d,first_sector);
      int track2=cdda_sector_gettrack(d,last_sector);
      long off1=first_sector-cdda_track_firstsector(d,track1);
      long off2=last_sector-cdda_track_firstsector(d,track2);

      sprintf(buffer,"Ripping from sector %7ld (track %2d [%d:%02d.%02d])\n"
	      "\t  to sector %7ld (track %2d [%d:%02d.%02d])\n",first_sector,
	      track1,(int)(off1/(60*75)),(int)((off1/75)%60),(int)(off1%75),
	      last_sector,
	      track2,(int)(off2/(60*75)),(int)((off2/75)%60),(int)(off2%75));
      report(buffer);
      
    }

    callbegin=first_sector;
    callend=last_sector;

    /* Off we go! */

    if(verbose)
      cdda_verbose_set(d,CDDA_MESSAGE_LOGIT,CDDA_MESSAGE_LOGIT);
    else
      cdda_verbose_set(d,CDDA_MESSAGE_LOGIT,CDDA_MESSAGE_FORGETIT);

    {
      long cursor;
      cdrom_paranoia *p=paranoia_init(d,10,150); /* big! ~5M av */
      paranoia_modeset(p,paranoia_mode);

      switch(output_type){
      case 0: /* raw */
	break;
      case 1: /* wav */
	WriteWav(out,(last_sector-first_sector+1)*CD_FRAMESIZE_RAW);
	break;
      case 2: /* aifc */
	WriteAifc(out,(last_sector-first_sector+1)*CD_FRAMESIZE_RAW);
	break;
      }

      paranoia_seek(p,cursor=first_sector,SEEK_SET);
      
      while(cursor<=last_sector){
	/* read a sector */
	size16 *readbuf=paranoia_read(p,1,callback);
	char *err=cdda_errors(d);
	char *mes=cdda_messages(d);
	
	if(mes || err)
	  fprintf(stderr,"\r                               "
		  "                                           \r%s%s\n",
		  mes?mes:"",err?err:"");

	if(err)free(err);
	if(mes)free(mes);
	if(readbuf==NULL)break;
	
	cursor++;

	if(output_endian!=bigendianp()){
	  int i;
	  for(i=0;i<CD_FRAMESIZE_RAW/2;i++)readbuf[i]=swap16(readbuf[i]);
	}

	if(blocking_write(out,(char *)readbuf,CD_FRAMESIZE_RAW)){
	  report2("Error writing output: %s",strerror(errno));
	  exit(1);
	}

	if(output_endian!=bigendianp()){
	  int i;
	  for(i=0;i<CD_FRAMESIZE_RAW/2;i++)readbuf[i]=swap16(readbuf[i]);
	}

      }
      paranoia_free(p);
    }
  }
  report("\n\nDone.\n\n");

  cdda_close(d);
  return 0;
}
