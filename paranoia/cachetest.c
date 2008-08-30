/*
 * GNU Lesser General Public License 2.1 applies
 * Copyright (C) 2008 Monty <monty@xiph.org>
 *
 */

/* this is in the paranoia library because the analysis is matched to
   cache modelling of a specific library version, not matched to the
   specific application (eg, cdparanoia version, which is simply a
   wrapper around the libs) */

/* we can ask most drives what their various caches' sizes are, but no
   drive will tell if it caches redbook data.  None should, many do,
   and there's no way in (eg) MMC/ATAPI to tell a cdrom drive not to
   cache when accessing audio.  SCSI drives have a FUA facility, but
   it's not clear how many ignore it.  MMC does specify some cache
   side effect as part of SET READ AHEAD, but it's not clear we can
   rely on them.  For that reason, we need to empirically determine
   cache size and strategy used for reads. */

#include <string.h>
#include <stdio.h>
#include <math.h>
#include "../interface/cdda_interface.h"
#include "p_block.h"

#define reportC(...) {if(progress){fprintf(progress, __VA_ARGS__);}	\
    if(log){fprintf(log, __VA_ARGS__);}}
#define printC(...) {if(progress){fprintf(progress, __VA_ARGS__);}}
#define logC(...) {if(log){fprintf(log, __VA_ARGS__);}}

static float ms_per_sector_at(int lba, float *waypoint){
  int waypos = (lba+44999)/45000;
  if(waypos<0)waypos=0;
  while(waypos>=0 && waypoint[waypos]==-1)waypoint--;
  return waypoint[waypos];
}

int paranoia_analyze_verify(cdrom_drive *d, FILE *progress, FILE *log){

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

  int i,j,ret;
  int firstsector=-1;
  int lastsector=-1;
  int firsttest=-1;
  int lasttest=-1;
  int max_retries=20;
  float median;
  int offset;
  int warn=0;
  int current=1000;
  int hi=15000;
  int cachesize;
  int readahead;
  int rollbehind;
  int cachegran;
  float waypoint[10]={-1,-1,-1,-1, -1,-1,-1,-1, -1,-1};

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
    int latency[current];
    int sectors[current];
    double sum;
    double sumsq;
    int sofar;
    int best=0;
    int bestcount=0;
    int iterating=0;

    offset = lastsector-firstsector-current-1;

    reportC("\nSeek/read timing:\n");

    while(offset>=firstsector){
      int m = offset/4500;
      int s = (offset-m*4500)/75;
      int f = offset-m*4500-s*75;
      if(iterating){
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
	double upper= mean+((isnan(stddev) || stddev*2<1.)?1.:stddev*2);
	
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
	
	{
	  int waypos = offset/45000;
	  if(waypoint[waypos]==-1 || waypoint[waypos]>mean)
	    waypoint[waypos]=mean;
	}

	printC("%4dms seek, %.2fms/sec read [%.1fx]",latency[0],mean,1000./75./mean);
	logC("\n\tInitial seek latency (%d sectors): %dms",current,latency[0]);
	logC("\n\tAverage read latency: %.2fms/sector (raw speed: %.1fx)",mean,1000./75./mean);
	logC("\n\tRead latency standard deviation: %.2fms/sector",stddev);

	sofar=mean*current;
	if(!iterating){
	  if(best==0 || sofar*1.01<best){
	    best= sofar;
	    bestcount=0;
	  }else{
	    bestcount+=sofar;
	    if(bestcount>sofar && bestcount>4000)
	      iterating=1;
	  }
	}
      }
    next:

      if(iterating){
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
  {
    int under=1;
    current=0;
    offset = firstsector+10;
    
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
	    offset+=current+100;
	    if(j==10 || offset+current>lastsector){
	      reportC("\n\tToo many read errors while performing drive cache checks;"
		      "\n\t  aborting test.\n\n");
	      return(-1);
	    }
	    reportC("\n\tRead error while performing drive cache checks;"
		    "\n\t  choosing new offset and trying again.\n");
	  }else{
	    if(cdda_milliseconds(d)==-1){
	      reportC("\n\tTiming error while performing drive cache checks; aborting test.\n");
	      return(-1);
	    }else{
	      if(cdda_milliseconds(d)<9)under=1;
	      break;
	    }
	  }
	}
      }
    } 
  }
  cachesize=current-1;

  printC("\r");
  if(cachesize==hi){
    reportC("\tWARNING: Cannot determine drive cache size or behavior!          \n");
    return 1;
  }else if(cachesize){
    reportC("\tApproximate random access cache size: %d sector(s)               \n",cachesize);
  }else{
    reportC("\tDrive does not cache nonlinear access                            \n");
    return 0;
  }
   
  /* The readahead cache size ascertained above is likely qualified by
     background 'rollahead'; that is, the drive's readahead process is
     often working ahead of our actual linear reads, and if reads stop
     or are interrupted, readahead continues and overflows the cache.
     It is also the case that the cache size we determined above is
     slightly too low because readahead is probably always working
     ahead of reads. 

     Determine the rollahead size a few ways (which may disagree:
     1) Read number of sectors equal to cache size; pause; read backward until seek
     2) Read sectors equal to cache-rollahead; verify reading back to beginning does not seek 
     3) Read sectors equal to cache; pause; read ahead until seek delay
  */

  {
    int under=1;
    readahead=0;
    
    while(under && readahead<hi){
      under=0;
      readahead++;
      printC("\r");
      reportC("\tTesting background readahead past read cursor... %d",readahead-1);
      
      for(i=0;i<10;i++){
	int sofar=0,ret,retry=0;
	printC(".          \b\b\b\b\b\b\b\b\b\b");
	logC("\n\t\t%d >>> ",i);
	
	while(sofar<cachesize){
	  ret = cdda_read(d,NULL,offset+sofar,1);
	  if(ret<=0)goto error;
	  logC("%d:%d ",ret,cdda_milliseconds(d));

	  /* some drives can lose sync and perform an internal resync,
	     which can also cause readahead to restart.  If we see
	     seek-like delays during the initial cahe load, retry the
	     preload */

	  sofar+=ret;
	}
	
	/* Pause 5x what we'd predict is needed to let the readahead process work. */
	{
	  int usec=ms_per_sector_at(offset,waypoint)*(cachesize+readahead)*(2+i)*1000;
	  logC("sleep=%dus ",usec);
	  usleep(usec);
	}
	
	/* seek to offset+cachesize+readahead */
	ret = cdda_read(d,NULL,offset+cachesize+readahead-1,1);
	if(ret<=0)break;
	logC("ahead=%d:%d\n",cachesize+readahead-1,cdda_milliseconds(d));
	if(cdda_milliseconds(d)<9){
	  under=1;
	  break;
	}
      }
    }
  }
  readahead--;
  logC("\n");
  printC("\r");
  if(readahead==0){
    reportC("\tDrive does not read ahead past read cursor (very strange)  \n");
  }else{
    reportC("\tDrive readahead past read cursor: %d sector(s)             \n",readahead);
  }
  
  reportC("\tTesting cache tail cursor");
  rollbehind=cachesize;
  
  for(i=0;i<10 && rollbehind;i++){
    int sofar=0,ret,retry=0;
    logC("\n\t\t>>> ");
    printC(".");
    while(sofar<cachesize){
      ret = cdda_read(d,NULL,offset+sofar,cachesize-sofar);
      if(ret<=0)goto error;
      logC("%d:%d ",ret,cdda_milliseconds(d));
      sofar+=ret;
    }
    
    /* Pause 5x what we'd predict is needed to read cachesize more sectors to let the readahead process work. */
    {
      int usec=ms_per_sector_at(offset+cachesize,waypoint)*cachesize*5000;
      logC("\n\t\tsleeping %d microseconds",usec);
      usleep(usec);
    }
    
    /* read backwards until we seek */
    logC("\n\t\t<<< ");
    sofar=rollbehind;
    while(sofar>0){
      sofar--;
      ret = cdda_read(d,NULL,offset+sofar,1);
      if(ret<=0)break;
      logC("%d:%d ",sofar,cdda_milliseconds(d));
      if(cdda_milliseconds(d)>8){
	rollbehind=sofar+1;
	break;
      }
      rollbehind=sofar;
    }
  error:
    if(ret<=0){
      offset+=cachesize;
      retry++;
      if(retry>10 || offset+cachesize>lastsector){
	reportC("\n\tToo many read errors while performing drive cache checks;"
		"\n\t  aborting test.\n\n");
	return(-1);
      }
      reportC("\n\tRead error while performing drive cache checks;"
	      "\n\t  choosing new offset and trying again.\n");
      continue;
    }
  }
  
  logC("\n");
  printC("\r");
  if(rollbehind==0){
    reportC("\tCache tail cursor tied to read cursor                      \n");
  }else{
    reportC("\tCache tail rollbehind: %d sector(s)                        \n",rollbehind);
  }
  
  reportC("\tTesting granularity of cache tail");
  cachegran=cachesize+1;
  
  for(i=0;i<10 && cachegran;i++){
    int sofar=0,ret,retry=0;
    logC("\n\t\t>>> ");
    printC(".");
    while(sofar<cachesize+1){
      ret = cdda_read(d,NULL,offset+sofar,cachesize-sofar+1);
      if(ret<=0)goto error2;
      logC("%d:%d ",ret,cdda_milliseconds(d));
      sofar+=ret;
    }
    
    /* Pause 5x what we'd predict is needed to read cachesize more sectors to let the readahead process work. */
    {
      int usec=ms_per_sector_at(offset+cachesize,waypoint)*cachesize*5000;
      logC("\n\t\tsleeping %d microseconds",usec);
      usleep(usec);
    }
    
    /* read backwards until we seek */
    logC("\n\t\t<<< ");
    sofar=cachegran;
    while(sofar>1){
      sofar--;
      ret = cdda_read(d,NULL,offset+sofar,1);
      if(ret<=0)break;
      logC("%d:%d ",sofar,cdda_milliseconds(d));
      if(cdda_milliseconds(d)>8) break;
    }
    cachegran=sofar;
  error2:
    if(ret<=0){
      offset+=cachesize;
      retry++;
      if(retry>10 || offset+cachesize>lastsector){
	reportC("\n\tToo many read errors while performing drive cache checks;"
		"\n\t  aborting test.\n\n");
	return(-1);
      }
      reportC("\n\tRead error while performing drive cache checks;"
	      "\n\t  choosing new offset and trying again.\n");
      continue;
    }
  }
  
  logC("\n");
  printC("\r");
  reportC("\tCache tail granularity: %d sector(s)                      \n",cachegran);


  /* this drive caches; Determine if the detailed caching behavior fits our model. */

  /* does the readahead cache exceed the maximum Paranoia currently expects? */
  if(cachesize > CACHEMODEL_SECTORS){
    reportC("\nWARNING: This drive appears to be caching more sectors of\n"
	    "           readahead than Paranoia can currently handle!\n");
    warn=1;

  }

  /* This is similar to the Fast search above, but just in case the
     cache is being tracked as multiple areas that are treated
     differently if non-contiguous.... */
  {
    int seekoff = cachesize*3;
    int under=0;
    reportC("\nVerifying that readahead cache is contiguous...\n");
    printC("\ttesting");
  
    for(i=0;i<30 && !under;i++){
      printC(".");
      for(j=0;;j++){
	int ret1,ret2;

	if(offset+seekoff>lastsector){
	  reportC("\n\tOut of readable space on CDROM while performing drive checks;"
		  "\n\t  aborting test.\n\n");
	  return(-1);
	}
	

	ret1 = cdda_read(d,NULL,offset+seekoff,1);
	logC("\t\t>>> %d:%d ",offset+seekoff,cdda_milliseconds(d));
	ret2 = cdda_read(d,NULL,offset,1);
	logC("seek_read:%d\n",cdda_milliseconds(d));
	
	if(ret1<=0 || ret2<=0){
	  offset+=cachesize+100;
	  if(j==10){
	    reportC("\n\tToo many read errors while performing drive cache checks;"
		    "\n\t  aborting test.\n\n");
	    return(-1);
	  }
	  reportC("\n\tRead error while performing drive cache checks;"
		  "\n\t  choosing new offset and trying again.\n");
	}else{
	  if(cdda_milliseconds(d)==-1){
	    reportC("\n\tTiming error while performing drive cache checks; aborting test.\n");
	    return(-1);
	  }else{
	    if(cdda_milliseconds(d)<9)under=1;
	    break;
	  }
	}
      }
    }
    printC("\r");
    if(under){
      reportC("WARNING: Drive cache does not appear to be contiguous!\n");
      warn=1;
    }else{
      reportC("\tDrive cache tests as contiguous.                \n");
    }
  }

  /* Verify that a read that begins before the cached readahead dumps
     the entire readahead cache */

  /* This is tricky because we can't simply read a one sector
     back seek, then rely on timing/seeking of subsequent sectors; the
     drive may well not seek ahead if reading linearly would be faster
     (and it often will be), and simply reading haead after the seek
     and watching timing will be inaccurate because the drive may roll
     some readahead into the initial seek before returning the first
     block. */

  /* we will need to use the timing of reading from media in one form
     or another and thus need to guard against slow bus transfer times
     [eg, no DMA] swamping the actual read time from media. */

  /* sample cache access for ten realtime seconds. */
  //{
    //int cachems;

    //reportC("\nVerifying that seeking before cache dumps readahead...");
    //reportC("\n\tSampling cache timing... ");
  //}


  /* Check to see that cdda_clear_cache clears the specified cache area */

  /* Does cdda_clear_cache result in noncontiguous cache areas? */

  return warn;
}


