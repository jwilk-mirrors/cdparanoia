/*
 * Copyright: GNU Public License 2 applies
 *
 * Generates a test file for debugging purposes with the file based test
 * interface
 */

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include "interface/cdda_interface.h"

int main(void){
  long i;
  size16 a;

  int out=open("test.file",O_RDWR|O_CREAT|O_TRUNC,0770);
  if(out==-1){
    perror("Unable to open test.file");
    exit(1);
  }

  for(i=0;i<CD_FRAMESIZE_RAW*128;i++){
    a=i%32768;
    write(out,&a,2);
    a=-a;
    write(out,&a,2);
  }

  close(out);
  return(0);
}
