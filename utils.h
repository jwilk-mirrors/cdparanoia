#include <endian.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

/* I wonder how many alignment issues this is gonna trip in the
   future...  it shouldn't trip any...  I guess we'll find out :) */

static inline int bigendianp(void){
  int test=1;
  char *hack=(char *)(&test);
  if(hack[0])return(0);
  return(1);
}

static inline size32 swap32(size32 x){
  return((((unsigned size32)x & 0x000000ffU) << 24) | 
	 (((unsigned size32)x & 0x0000ff00U) <<  8) | 
	 (((unsigned size32)x & 0x00ff0000U) >>  8) | 
	 (((unsigned size32)x & 0xff000000U) >> 24));
}

static inline size16 swap16(size16 x){
  return((((unsigned size16)x & 0x00ffU) <<  8) | 
	 (((unsigned size16)x & 0xff00U) >>  8));
}

#if BYTE_ORDER == LITTLE_ENDIAN

static inline size32 be32_to_cpu(size32 x){
  return(swap32(x));
}

static inline size16 be16_to_cpu(size16 x){
  return(swap16(x));
}

static inline size32 le32_to_cpu(size32 x){
  return(x);
}

static inline size16 le16_to_cpu(size16 x){
  return(x);
}

#else

static inline size32 be32_to_cpu(size32 x){
  return(x);
}

static inline size16 be16_to_cpu(size16 x){
  return(x);
}

static inline size32 le32_to_cpu(size32 x){
  return(swap32(x));
}

static inline size16 le16_to_cpu(size16 x){
  return(swap16(x));
}


#endif

static inline size32 cpu_to_be32(size32 x){
  return(be32_to_cpu(x));
}

static inline size32 cpu_to_le32(size32 x){
  return(le32_to_cpu(x));
}

static inline size16 cpu_to_be16(size16 x){
  return(be16_to_cpu(x));
}

static inline size16 cpu_to_le16(size16 x){
  return(le16_to_cpu(x));
}

static inline char *copystring(const char *s){
  if(s){
    char *ret=malloc((strlen(s)+1)*sizeof(char));
    strcpy(ret,s);
    return(ret);
  }
  return(NULL);
}

static inline char *catstring(char *buff,const char *s){
  if(s){
    if(buff)
      buff=realloc(buff,strlen(buff)+strlen(s)+1);
    else
      buff=calloc(strlen(s)+1,1);
    strcat(buff,s);
  }
  return(buff);
}

