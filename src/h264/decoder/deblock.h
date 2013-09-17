/*
 * ===========================================================================
 *
 *   This confidential and proprietary software may be used only
 *  as authorized by a licensing agreement from Thumb o'Cat Inc.
 *  In the event of publication, the following notice is applicable:
 * 
 *       Copyright (C) 2013 - 2013 Thumb o'Cat
 *                     All right reserved.
 * 
 *   The entire notice above must be reproduced on all authorized copies.
 *
 * ===========================================================================
 *
 *  File      : deblock.h
 *  Author(s) : Luuvish
 *  Version   : 1.0
 *  Revision  :
 *      1.0 June 16, 2013    first release
 *
 * ===========================================================================
 */

#ifndef _DEBLOCK_H_
#define _DEBLOCK_H_


#include "global.h"
#include "dpb.h"


namespace vio  {
namespace h264 {


struct deblock_t {
	void init();
	void deblock(VideoParameters *p_Vid, storable_picture *p);

private:
	int  compare_mvs(const MotionVector* mv0, const MotionVector* mv1, int mvlimit);
	int  bs_compare_mvs(const pic_motion_params* mv_info_p, const pic_motion_params* mv_info_q, int mvlimit);

	void strength_vertical  (mb_t* MbQ, int edge);
	void strength_horizontal(mb_t* MbQ, int edge);
	void strength           (mb_t* mb);

	void filter_strong(imgpel *pixP, imgpel *pixQ, int widthP, int widthQ, int alpha, int beta, int bS, bool chromaStyleFilteringFlag);
	void filter_normal(imgpel *pixP, imgpel *pixQ, int widthP, int widthQ, int alpha, int beta, int bS, bool chromaStyleFilteringFlag, int tc0, int BitDepth);
	void filter_edge  (mb_t* MbQ, bool chromaEdgeFlag, ColorPlane pl, bool verticalEdgeFlag, bool fieldModeInFrameFilteringFlag, int edge);

	void filter_vertical  (mb_t* MbQ);
	void filter_horizontal(mb_t* MbQ);

	void init_neighbors       (VideoParameters *p_Vid);
	void make_frame_picture_JV(VideoParameters *p_Vid);
	void deblock_pic          (VideoParameters *p_Vid);
};


extern deblock_t deblock;


}
}


#endif /* _DEBLOCK_H_ */
