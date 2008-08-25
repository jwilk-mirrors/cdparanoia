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
 * cdparanoia (C) 2008 Monty <monty@xiph.org>
 *
 */

/* we can ask most drives what their various caches' sizes are, but no
   drive will tell if it caches redbook data.  None should, many do,
   and there's no way in (eg) MMAC/ATAPI to tell a drive not to.  SCSI
   drives have a FUA facility, but it's not clear how many ignore it.
   MMC does specify some cache side effect as part of SET READ AHEAD,
   but it's not clear we can rely on them.  For that reason, we need
   to empirically determine cache size and strategy used for reads. */

#include <string.h>
#include <stdio.h>
#include "interface/cdda_interface.h"
#include "report.h"
#include "cachetest.h"

int analyze_timing_and_cache(cdrom_drive *d){

  /* Some assumptions about timing: 

     We can't perform cache determination timing based on looking at
     average transfer times; on slow setups, the speed of a drive
     reading sectors via PIO will not be reliably distinguishable from
     the same drive returning data from the cache via pio.  We need
     something even more noticable and reliable: the seek time. It is
     unlikely we'd ever see a seek latency of under ~10ms given the
     synchronization requirements of a CD and the maximum possible
     rotational velocity. A cache hit would always be faster, even
     with PIO.

     Further complicating things, we have to watch the data collection
     carefully as we're not always going to be on an unloaded system,
     and we even have to guard against other apps accessing the drive
     (something that should never happen on purpose, but could happen
     by accident).  As we know in our testing when seeks should never
     occur, a sudden seek-sized latency popping up in the middle of a
     collection is an indication that collection is possibly invalid.

     A second cause of 'spurious latency' would be media damage; if
     we're consistently hitting latency on the same sector during
     initial collection, may need to move past it. */

  int i,ret;
  int firstsector=-1;
  int lastsector=-1;
  int firsttest=-1;
  int lasttest=-1;
  char buffer[80];
  int max_retries=20;
  float median;
  int offset;

  /* set up a default pessimal take on drive behavior */
  //d->private->cache_backseekflush=0;
  //d->private->cache_sectors=1200;

  reportC("\nChecking drive timing behavior...");

  /* find the longest stretch of available audio data */

  for(i=0;i<d->tracks;i++){
    if(cdda_track_audiop(d,i+1)==1){
      if(firsttest == -1)
	firsttest=cdda_track_firstsector(d,i+1);
      lasttest=cdda_track_lastsector(d,i+1);
      if(lasttest-firsttest > lastsector-firstsector){
	firstsector=firsttest;
	lastsector=lasttest;
      }
    }else{
      firsttest=-1;
      lasttest=-1;
    }
  }

  if(firstsector==-1){
    reportC("\n\tNo audio on disc; Cannot determine timing behavior...");
    return -1;
  }

  /* Dump some initial timing data to give a little context for human
     eyes.  This isn't actually used in timing anywhere. */
  if(verbose){
    int x;
    int current=300;
    int acc=0;
    int prev=0;

    offset = firstsector;

    if((ret=cdda_read(d,NULL,offset+current+1,1))<0){
      /* media error! grr!  retry elsewhere */
      reportC("\n\tWARNING: media error; picking new location and trying again.");
      continue;
    }

      reportC("\n\tSector timings (ms):\n\t");

      for(i=0;i<current;i++){
	if(cdda_read(d,NULL,offset+i,1)<0){
	  /* media error! grr!  retry elsewhere */
	  reportC("\n\tWARNING: media error; picking new location and trying again.");
	  break;
	}
	x = cdda_milliseconds(d);
	if(x>9999)x=9999;
	if(x<0)x=0;
	reportC("%d ",x);

	histogram[x]++;
	latency[i]=x;
      }
      if(i<current){
	offset-=current+1;
	continue;
      }	

      for(i=0;i<10000;i++){
	prev=acc;
	acc+=histogram[i];
	if(acc>current/2){
	  reportC("\n\tSurrounding histogram: ");
	  if(i)
	    reportC("%dms:%d ",i-1,acc-histogram[i]);
	  
	  reportC("%dms:%d ",i,acc);
	  if(i<999)
	    reportC("%dms:%d ",i+1,acc+histogram[i+1]);
	  reportC("\n");
	  break;
	}
      }

      median = (i*(acc-prev) + (i-1)*prev)/(float)acc;
      
      reportC("\n\tsmall seek latency (%d sectors): %d ms",current,latency[0]);
      reportC("\n\tmedian read latency per sector: %.1f ms",median);

      /* verify slow spinup did not compromise median */
      for(i=1;i<current;i++)
	if(latency[i]>latency[i-1] || latency[i]<=(median+1.))break;
      if(i>5){
	reportC("\n\tDrive appears to spin up slowly... retrying...");
	offset-=current+1;
	continue;
      }

      /* verify against spurious latency; any additional 5x blocks that
	 are not continuous with read start */
      acc=0;
      if(median<.6)median=.6;
      for(i=5;i<current;i++)
	if(latency[i]>median*10)acc++;

      if(acc){
	report("\n\tWARNING: Read timing displayed bursts of unexpected"
	       "\n\tlatency; retrying for a clean read.\n");
	continue;
      }
	
      break;
    }

    if(offset<firstsector){
      report("\n500: Unable to find sufficiently large area of"
	      "\n\tgood media to perform timing tests.  Aborting.\n");
      return -500;
    }
    if(retry==max_retries){
      report("\n500: Too many retries; aborting analysis.\n");
      return -500;
    }    
  }
  
  /* look to see if drive is caching at all; read first sector N
     times, if any reads are near or under the median latency, we're
     caching */
  {
    for(i=0;i<max_retries;i++){
      if(d->read_audio(d,NULL,offset,1)==1){
	if(cdda_milliseconds(d)<median*10) break;

      }else{
	/* error handling */
      }
    }

    if(i<max_retries){
      reportC("\n\tCaching test result: DRIVE IS CACHING (bad)\n");
    }else{
      reportC("\n\tCaching test result: Drive is not caching (good)\n");
      return 0;
    }
  }




      


  /* search on cache size; cache hits are fast, seeks are not, so a
     linear search through cache hits up to a miss are faster than a
     bisection */

  int lo=1;
  int hi=15000;
  int current=lo;
  int under=1;
  offset = firstsector;

  reportC("\n");
    
  while(current <= hi && under){
    int i,j;
    under=0;

    reportC("\r\tInitial fast search for cache size... %d",current-1);

    for(i=0;i<10 && !under;i++){
      for(j=0;;j++){	  
	int ret1 = cdda_read(d,NULL,offset+current-1,1);
	int ret2 = cdda_read(d,NULL,offset,1);
	if(ret1<=0 || ret2<=0){
	  if(j==2){
	    reportC("\n\tRead error while performing drive cache checks; aborting test.\n");
	    return(-1);
	  }
	}else{
	  if(cdda_milliseconds(d)==-1){
	    if(j==2){
	      reportC("\n\tTiming error while performing drive cache checks; aborting test.\n");
	      return(-1);
	    }
	  }else{
	    if(cdda_milliseconds(d)<10)under=1;
	    break;
	  }
	}
      }
    }
    current++;
  } 



   

  /* XXXXXX IN PROGRESS */
  reportC("\n");
  return 0;
}


