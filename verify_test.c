/*
 * Copyright: GNU Public License 2 applies
 *
 * test for debugging purposes with the file based test
 * interface
 */

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include "interface/cdda_interface.h"

int main(void){
  int i=0;
  size16 a;
  size16 b;

  int in=open("cdda.raw",O_RDONLY);
  int in2=open("test.file",O_RDONLY);
  if(in==-1){
    perror("Unable to open cdda.raw");
    exit(1);
  }
  if(in2==-1){
    perror("Unable to open test.file");
    exit(1);
  }

  while(1){
    if(read(in,&a,2)==-1)break;
    if(read(in2,&b,2)==-1)break;
    if(a!=b)goto fail2;
    i+=2;
  }

  printf("All OK.\n\n");
  close(in);
  close(in2);
  return(0);

fail:
  printf("read error\n");
fail2:
  printf("%d!=%d @ byte position %ld\nfail.\n",(int)a,(int)b,i);
  close(in);
  close(in2);
  return(1);
}
