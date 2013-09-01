#ifndef _IMAGE_H_
#define _IMAGE_H_


#include "dpb.h"

struct slice_t;

void init_picture(VideoParameters *p_Vid, struct slice_t *currSlice, InputParameters *p_Inp);

void calculate_frame_no(VideoParameters *p_Vid, StorablePicture *p);

int  read_new_slice    (struct slice_t *currSlice);
void exit_picture      (VideoParameters *p_Vid, StorablePicture **dec_picture);
int  decode_one_frame  (DecoderParams *pDecoder);

void decode_picture(VideoParameters *p_Vid);

#if (MVC_EXTENSION_ENABLE)
extern int GetVOIdx(VideoParameters *p_Vid, int iViewId);
#endif


#endif
