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
#include <math.h>
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
  int max_retries=20;
  float median;
  int offset;

  /* set up a default pessimal take on drive behavior */
  //d->private->cache_backseekflush=0;
  //d->private->cache_sectors=1200;

  reportC("\n=================== Checking drive cache/timing behavior ===================\n");

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
     eyes.  Take readings ten minutes apart (45000 sectors) and at end of disk. */
  {
    int x;
    int current=1000;
    int latency[current];
    int sectors[current];
    double sum;
    double sumsq;
    int sofar;
    double best=0;
    int bestcount=0;

    offset = lastsector-firstsector-current-1;

    reportC("\nSeek/read timing:\n");

    while(offset>=firstsector){
      int m = offset/4500;
      int s = (offset-m*4500)/75;
      int f = offset-m*4500-s*75;
      if(bestcount==10){
	reportC("\n");
      }else{
	printC("\r");
	logC("\n");
      }
      reportC("\t[%02d:%02d.%02d]: ",m,s,f);
      sum=0;
      sumsq=0;

      /* initial seek to put at at a small offset past end of upcoming reads */
      if((ret=cdda_read(d,NULL,offset+current+1,1))<0){
	/* media error! grr!  retry elsewhere */
	if(ret==-404)return -1;
	reportC("\n\tWARNING: media error during setup; continuing at next offset...");
	goto next;
      }

      logC("\n");
      
      for(i=0,sofar=0;sofar<current;i++){
	int toread = (i==0?1:current-sofar);
	int ret;
	/* first read should also trigger a short seek; one sector so seek duration dominates */
	if((ret=cdda_read(d,NULL,offset+sofar,toread))<=0){
	  /* media error! grr!  retry elsewhere */
	if(ret==-404)return -1;
	  reportC("\n\tWARNING: media error during read; continuing at next offset...");
	  goto next;
	}

	x = cdda_milliseconds(d);
	if(x>9999)x=9999;
	if(x<0)x=0;
	logC("%d:%d ",ret,x);

	latency[i]=x;
	sectors[i]=ret;
	sofar+=ret;
	if(i){
	  sum+=x;
	  sumsq+= x*x /(float)ret;
	}
      }
      
      /* ignore upper outliers; we may have gotten random bursts of latency */
      {
	double mean = sum/(float)(current-1);
	double stddev = sqrt( (sumsq/(float)(current-1) - mean*mean));
	double upper= mean+((isnan(stddev) || stddev<1.)?1.:stddev);
	int j;
	
	mean=0;
	sofar=0;
	for(j=1;j<i;j++){
	  double per = latency[j]/(double)sectors[j];
	  if(per<=upper){
	    mean+=latency[j];
	    sofar+=sectors[j];
	  }
	}
	mean/=sofar;
	
	printC("%4dms seek, %.2fms/sec read [%.1fx]",latency[0],mean,1000./75./mean);
	logC("\n\tInitial seek latency (%d sectors): %dms",current,latency[0]);
	logC("\n\tAverage read latency: %.2fms/sector (raw speed: %.1fx)",mean,1000./75./mean);
	logC("\n\tRead latency standard deviation: %.2fms/sector",stddev);

	if(bestcount<10){
	  if(1./mean>best){
	    best=1./mean;
	    bestcount=0;
	  }else
	    bestcount++;
	}
      }
    next:

      if(bestcount==10){
	offset = (offset-firstsector+44999)/45000*45000+firstsector;
	offset-=45000;
	printC("               ");
      }else{
	offset--;
	printC(" spinning up...");
      }
    }
  }

  reportC("\n\nAnalyzing readahead cache access...\n");
  
  /* search on cache size; cache hits are fast, seeks are not, so a
     linear search through cache hits up to a miss are faster than a
     bisection */

  int hi=15000;
  int current=0;
  int under=1;
  offset = firstsector+1000;

  while(current <= hi && under){
    int i,j;
    under=0;
    current++;

    printC("\r");
    reportC("\tFast search for approximate cache size... %d sectors            ",current-1);
    logC("\n");

    for(i=0;i<15 && !under;i++){
      for(j=0;;j++){
	int ret1,ret2;
	if(i>=5){
	  int sofar=0;
	  
	  if(i==5){
	    printC("\r");
	    reportC("\tSlow verify for approximate cache size... %d sectors",current-1);
	    logC("\n");

	    logC("\tAttempting to reduce read speed to 1x... ");
	    if(cdda_speed_set(d,1)){
	      logC("failed.\n");
	    }else{
	      logC("drive said OK\n");
	    }
	  }
	  printC(".");
	  logC("\t\t>>> ");

	  while(sofar<current){
	    ret1 = cdda_read(d,NULL,offset+sofar,current-sofar);
	    logC("slow_read=%d:%d ",ret1,cdda_milliseconds(d));
	    if(ret1<=0)break;
	    sofar+=ret1;
	  }
	}else{
	  ret1 = cdda_read(d,NULL,offset+current-1,1);
	  logC("\t\t>>> fast_read=%d:%d ",ret1,cdda_milliseconds(d));
	}
	ret2 = cdda_read(d,NULL,offset,1);
	logC("seek_read=%d:%d\n",ret2,cdda_milliseconds(d));
	
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
	    if(cdda_milliseconds(d)<9)under=1;
	    break;
	  }
	}
      }
    }
  } 

  printC("\r");
  if(current>1){
    reportC("\tApproximate random access cache size: %d sectors                 \n",current-1);
  }else{
    reportC("\tDrive does not cache nonlinear access                            \n");
    return 0;
  }
   
  /* this drive caches; Determine if the detailed caching behavior fits our model. */

  /* does the readahead cache exceed the maximum Paranoia currently expects? */

  /* Verify that a read that begins before the cached readahead dumps
     the entire readahead cache */

  /* Verify that reads that begin after the apparently cached
     readahead either dump the cache *or* cause the cached area to
     shift later in one contiguous piece */
  
  /* Check to see that cdda_clear_cache clears the specified cache area */

  /* XXXXXX IN PROGRESS */
  reportC("\n");
  return 0;
}


