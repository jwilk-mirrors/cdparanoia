
#ifndef _p_block_h_
#define _p_block_h_

extern void release_c_block(c_block *c);
extern c_block *new_c_block(cdrom_paranoia *p);
extern void release_v_fragment(v_fragment *c);
extern v_fragment *new_v_fragment(cdrom_paranoia *p);
extern size16 *v_buffer(v_fragment *v);
extern void recover_cache(cdrom_paranoia *p);
extern void recover_fragments(cdrom_paranoia *p);

#endif
