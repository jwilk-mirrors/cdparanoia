#ifndef _p_block_h_
#define _p_block_h_

extern void release_p_block(p_block *b);
extern p_block *new_p_block(cdrom_paranoia *p);
extern void swap_p_block(p_block *a,p_block *b);
extern void p_buffer(p_block *b,size16 *buffer,long size);

#endif
