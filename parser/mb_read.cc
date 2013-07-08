/*!
 ***********************************************************************
 * \file macroblock.c
 *
 * \brief
 *     Decode a Macroblock
 *
 * \author
 *    Main contributors (see contributors.h for copyright, address and affiliation details)
 *    - Inge Lille-Lang�y               <inge.lille-langoy@telenor.com>
 *    - Rickard Sjoberg                 <rickard.sjoberg@era.ericsson.se>
 *    - Jani Lainema                    <jani.lainema@nokia.com>
 *    - Sebastian Purreiter             <sebastian.purreiter@mch.siemens.de>
 *    - Thomas Wedi                     <wedi@tnt.uni-hannover.de>
 *    - Detlev Marpe
 *    - Gabi Blaettermann
 *    - Ye-Kui Wang                     <wyk@ieee.org>
 *    - Lowell Winger                   <lwinger@lsil.com>
 *    - Alexis Michael Tourapis         <alexismt@ieee.org>
 ***********************************************************************
*/

#include <math.h>

#include "global.h"
#include "slice.h"
#include "dpb.h"
#include "bitstream_elements.h"
#include "bitstream_cabac.h"
#include "bitstream.h"
#include "macroblock.h"
#include "mb_read.h"
#include "fmo.h"
#include "image.h"
#include "neighbour.h"
#include "biaridecod.h"
#include "transform.h"
#include "mv_prediction.h"
#include "intra_prediction.h"
#include "inter_prediction.h"
#include "bitstream_ctx.h"

#define IS_DIRECT(MB)   ((MB)->mb_type==0     && (currSlice->slice_type == B_SLICE ))

/*!
 ************************************************************************
 * \brief
 *    read next VLC codeword for 4x4 Intra Prediction Mode and
 *    map it to the corresponding Intra Prediction Direction
 ************************************************************************
 */
static int GetVLCSymbol_IntraMode (byte buffer[],int totbitoffset,int *info, int bytecount)
{
  int byteoffset = (totbitoffset >> 3);        // byte from start of buffer
  int bitoffset   = (7 - (totbitoffset & 0x07)); // bit from start of byte
  byte *cur_byte  = &(buffer[byteoffset]);
  int ctr_bit     = (*cur_byte & (0x01 << bitoffset));      // control bit for current bit posision

  //First bit
  if (ctr_bit)
  {
    *info = 0;
    return 1;
  }

  if (byteoffset >= bytecount) 
  {
    return -1;
  }
  else
  {
    int inf = (*(cur_byte) << 8) + *(cur_byte + 1);
    inf <<= (sizeof(byte) * 8) - bitoffset;
    inf = inf & 0xFFFF;
    inf >>= (sizeof(byte) * 8) * 2 - 3;

    *info = inf;
    return 4;           // return absolute offset in bit from start of frame
  } 
}

static int readSyntaxElement_Intra4x4PredictionMode(SyntaxElement *sym, Bitstream   *currStream)
{
  sym->len = GetVLCSymbol_IntraMode (currStream->streamBuffer, currStream->frame_bitoffset, &(sym->inf), currStream->bitstream_length);

  if (sym->len == -1)
    return -1;

  currStream->frame_bitoffset += sym->len;
  sym->value1       = (sym->len == 1) ? -1 : sym->inf;
  return 1;
}


/*!
 ************************************************************************
 * \brief
 *    This function is used to arithmetically decode the 8x8 block type.
 ************************************************************************
 */
static void readB8_typeInfo_CABAC_p_slice (Macroblock *currMB, 
                                    SyntaxElement *se,
                                    DecodingEnvironment *dep_dp)
{
  Slice *currSlice = currMB->p_Slice;
  int act_sym = 0;

  MotionInfoContexts *ctx = currSlice->mot_ctx;
  BiContextType *b8_type_contexts = &ctx->b8_type_contexts[0][1];

  if (biari_decode_symbol (dep_dp, b8_type_contexts++))
    act_sym = 0;
  else
  {
    if (biari_decode_symbol (dep_dp, ++b8_type_contexts))
    {
      act_sym = (biari_decode_symbol (dep_dp, ++b8_type_contexts))? 2: 3;
    }
    else
    {
      act_sym = 1;
    }
  } 

  se->value1 = act_sym;
}


/*!
 ************************************************************************
 * \brief
 *    This function is used to arithmetically decode the 8x8 block type.
 ************************************************************************
 */
static void readB8_typeInfo_CABAC_b_slice (Macroblock *currMB, 
                                    SyntaxElement *se,
                                    DecodingEnvironment *dep_dp)
{
  Slice *currSlice = currMB->p_Slice;
  int act_sym = 0;

  MotionInfoContexts *ctx = currSlice->mot_ctx;
  BiContextType *b8_type_contexts = ctx->b8_type_contexts[1];

  if (biari_decode_symbol (dep_dp, b8_type_contexts++))
  {
    if (biari_decode_symbol (dep_dp, b8_type_contexts++))
    {
      if (biari_decode_symbol (dep_dp, b8_type_contexts++))
      {
        if (biari_decode_symbol (dep_dp, b8_type_contexts))
        {
          act_sym = 10;
          if (biari_decode_symbol (dep_dp, b8_type_contexts)) 
            act_sym++;
        }
        else
        {
          act_sym = 6;
          if (biari_decode_symbol (dep_dp, b8_type_contexts)) 
            act_sym += 2;
          if (biari_decode_symbol (dep_dp, b8_type_contexts)) 
            act_sym++;
        }
      }
      else
      {
        act_sym = 2;
        if (biari_decode_symbol (dep_dp, b8_type_contexts)) 
          act_sym += 2;
        if (biari_decode_symbol (dep_dp, b8_type_contexts)) 
          act_sym ++;
      }
    }
    else
    {
      act_sym = (biari_decode_symbol (dep_dp, ++b8_type_contexts)) ? 1: 0;
    }
    ++act_sym;
  }
  else
  {
    act_sym = 0;
  }

  se->value1 = act_sym;
}

/*!
 ************************************************************************
 * \brief
 *    This function is used to arithmetically decode a pair of
 *    intra prediction modes of a given MB.
 ************************************************************************
 */
static void readIntraPredMode_CABAC( Macroblock *currMB, 
                              SyntaxElement *se,
                              DecodingEnvironment *dep_dp)
{
  Slice *currSlice = currMB->p_Slice;
  TextureInfoContexts *ctx     = currSlice->tex_ctx;
  // use_most_probable_mode
  int act_sym = biari_decode_symbol(dep_dp, ctx->ipr_contexts);

  // remaining_mode_selector
  if (act_sym == 1)
    se->value1 = -1;
  else
  {
    se->value1  = (biari_decode_symbol(dep_dp, ctx->ipr_contexts + 1)     );
    se->value1 |= (biari_decode_symbol(dep_dp, ctx->ipr_contexts + 1) << 1);
    se->value1 |= (biari_decode_symbol(dep_dp, ctx->ipr_contexts + 1) << 2);
  }
}




/*!
 ************************************************************************
 * \brief
 *    Set context for reference frames
 ************************************************************************
 */
static inline int BType2CtxRef (int btype)
{
  return (btype >= 4);
}

/*!
 ************************************************************************
 * \brief
 *    Function for reading the reference picture indices using VLC
 ************************************************************************
 */
static char readRefPictureIdx_VLC(Macroblock *currMB, SyntaxElement *currSE, DataPartition *dP, char b8mode, int list)
{
  currSE->context = BType2CtxRef (b8mode);
  currSE->value2 = list;
  dP->readSyntaxElement (currMB, currSE, dP);
  return (char) currSE->value1;
}

/*!
 ************************************************************************
 * \brief
 *    Function for reading the reference picture indices using FLC
 ************************************************************************
 */
static char readRefPictureIdx_FLC(Macroblock *currMB, SyntaxElement *currSE, DataPartition *dP, char b8mode, int list)
{
    currSE->context = BType2CtxRef (b8mode);
    currSE->value1 = 1 - dP->bitstream->f(1);
    return (char) currSE->value1;
}

/*!
 ************************************************************************
 * \brief
 *    Dummy Function for reading the reference picture indices
 ************************************************************************
 */
static char readRefPictureIdx_Null(Macroblock *currMB, SyntaxElement *currSE, DataPartition *dP, char b8mode, int list)
{
  return 0;
}

void read_delta_quant(SyntaxElement *currSE, DataPartition *dP, Macroblock *currMB, const byte *partMap, int type)
{
  Slice *currSlice = currMB->p_Slice;
  sps_t *sps = currSlice->active_sps;
  pps_t *pps = currSlice->active_pps;
 
  currSE->type = type;

  dP = &(currSlice->partArr[partMap[currSE->type]]);

  if (!pps->entropy_coding_mode_flag || dP->bitstream->ei_flag)
  {
    currSE->mapping = linfo_se;
  }
  else
    currSE->reading= read_dQuant_CABAC;

  dP->readSyntaxElement(currMB, currSE, dP);
  currMB->delta_quant = (short) currSE->value1;
  if ((currMB->delta_quant < -(26 + sps->QpBdOffsetY/2)) || (currMB->delta_quant > (25 + sps->QpBdOffsetY/2)))
  {
      printf("mb_qp_delta is out of range (%d)\n", currMB->delta_quant);
      currMB->delta_quant = iClip3(-(26 + sps->QpBdOffsetY/2), (25 + sps->QpBdOffsetY/2), currMB->delta_quant);
  }

  currSlice->SliceQpY = ((currSlice->SliceQpY + currMB->delta_quant + 52 + 2*sps->QpBdOffsetY)%(52+sps->QpBdOffsetY)) - sps->QpBdOffsetY;
  update_qp(currMB, currSlice->SliceQpY);
}

/*!
 ************************************************************************
 * \brief
 *    Function to prepare reference picture indice function pointer
 ************************************************************************
 */
static void prepareListforRefIdx ( Macroblock *currMB, SyntaxElement *currSE, DataPartition *dP, int num_ref_idx_active, int refidx_present)
{  
  if(num_ref_idx_active > 1)
  {
    if (currMB->p_Vid->active_pps->entropy_coding_mode_flag == (Boolean) CAVLC || dP->bitstream->ei_flag)
    {
      currSE->mapping = linfo_ue;
      if (refidx_present)
        currMB->readRefPictureIdx = (num_ref_idx_active == 2) ? readRefPictureIdx_FLC : readRefPictureIdx_VLC;
      else
        currMB->readRefPictureIdx = readRefPictureIdx_Null;
    }
    else
    {
      currSE->reading = readRefFrame_CABAC;
      currMB->readRefPictureIdx = (refidx_present) ? readRefPictureIdx_VLC : readRefPictureIdx_Null;
    }
  }
  else
    currMB->readRefPictureIdx = readRefPictureIdx_Null; 
}

/*!
 ************************************************************************
 * \brief
 *    Function to read reference picture indice values
 ************************************************************************
 */
static void readMBRefPictureIdx (SyntaxElement *currSE, DataPartition *dP, Macroblock *currMB, PicMotionParams **mv_info, int list, int step_v0, int step_h0)
{
  if (currMB->mb_type == 1)
  {
    if ((currMB->b8pdir[0] == list || currMB->b8pdir[0] == BI_PRED))
    {
      int j, i;
      char refframe;
      

      currMB->subblock_x = 0;
      currMB->subblock_y = 0;
      refframe = currMB->readRefPictureIdx(currMB, currSE, dP, 1, list);
      for (j = 0; j <  step_v0; ++j)
      {
        char *ref_idx = &mv_info[j][currMB->block_x].ref_idx[list];
        // for (i = currMB->block_x; i < currMB->block_x + step_h0; ++i)
        for (i = 0; i < step_h0; ++i)
        {
          //mv_info[j][i].ref_idx[list] = refframe;
          *ref_idx = refframe;
          ref_idx += sizeof(PicMotionParams);
        }
      }
    }
  }
  else if (currMB->mb_type == 2)
  {
    int k, j, i, j0;
    char refframe;

    for (j0 = 0; j0 < 4; j0 += step_v0)
    {
      k = j0;

      if ((currMB->b8pdir[k] == list || currMB->b8pdir[k] == BI_PRED))
      {
        currMB->subblock_y = j0 << 2;
        currMB->subblock_x = 0;
        refframe = currMB->readRefPictureIdx(currMB, currSE, dP, currMB->b8mode[k], list);
        for (j = j0; j < j0 + step_v0; ++j)
        {
          char *ref_idx = &mv_info[j][currMB->block_x].ref_idx[list];
          // for (i = currMB->block_x; i < currMB->block_x + step_h0; ++i)
          for (i = 0; i < step_h0; ++i)
          {
            //mv_info[j][i].ref_idx[list] = refframe;
            *ref_idx = refframe;
            ref_idx += sizeof(PicMotionParams);
          }
        }
      }
    }
  }  
  else if (currMB->mb_type == 3)
  {
    int k, j, i, i0;
    char refframe;

    currMB->subblock_y = 0;
    for (i0 = 0; i0 < 4; i0 += step_h0)
    {      
      k = (i0 >> 1);

      if ((currMB->b8pdir[k] == list || currMB->b8pdir[k] == BI_PRED) && currMB->b8mode[k] != 0)
      {
        currMB->subblock_x = i0 << 2;
        refframe = currMB->readRefPictureIdx(currMB, currSE, dP, currMB->b8mode[k], list);
        for (j = 0; j < step_v0; ++j)
        {
          char *ref_idx = &mv_info[j][currMB->block_x + i0].ref_idx[list];
          // for (i = currMB->block_x; i < currMB->block_x + step_h0; ++i)
          for (i = 0; i < step_h0; ++i)
          {
            //mv_info[j][i].ref_idx[list] = refframe;
            *ref_idx = refframe;
            ref_idx += sizeof(PicMotionParams);
          }
        }
      }
    }
  }
  else
  {
    int k, j, i, j0, i0;
    char refframe;

    for (j0 = 0; j0 < 4; j0 += step_v0)
    {
      currMB->subblock_y = j0 << 2;
      for (i0 = 0; i0 < 4; i0 += step_h0)
      {      
        k = 2 * (j0 >> 1) + (i0 >> 1);

        if ((currMB->b8pdir[k] == list || currMB->b8pdir[k] == BI_PRED) && currMB->b8mode[k] != 0)
        {
          currMB->subblock_x = i0 << 2;
          refframe = currMB->readRefPictureIdx(currMB, currSE, dP, currMB->b8mode[k], list);
          for (j = j0; j < j0 + step_v0; ++j)
          {
            char *ref_idx = &mv_info[j][currMB->block_x + i0].ref_idx[list];
            //PicMotionParams *mvinfo = mv_info[j] + currMB->block_x + i0;
            for (i = 0; i < step_h0; ++i)
            {
              //(mvinfo++)->ref_idx[list] = refframe;
              *ref_idx = refframe;
              ref_idx += sizeof(PicMotionParams);
            }
          }
        }
      }
    }
  }
}

/*!
 ************************************************************************
 * \brief
 *    Function to read reference picture indice values
 ************************************************************************
 */
static void readMBMotionVectors (SyntaxElement *currSE, DataPartition *dP, Macroblock *currMB, int list, int step_h0, int step_v0)
{
  if (currMB->mb_type == 1)
  {
    if ((currMB->b8pdir[0] == list || currMB->b8pdir[0]== BI_PRED))//has forward vector
    {
      int i4, j4, ii, jj;
      short curr_mvd[2];
      MotionVector pred_mv, curr_mv;
      short (*mvd)[4][2];
      //VideoParameters *p_Vid = currMB->p_Vid;
      PicMotionParams **mv_info = currMB->p_Slice->dec_picture->mv_info;
      PixelPos block[4]; // neighbor blocks

      currMB->subblock_x = 0; // position used for context determination
      currMB->subblock_y = 0; // position used for context determination
      i4  = currMB->block_x;
      j4  = currMB->block_y;
      mvd = &currMB->mvd [list][0];

      get_neighbors(currMB, block, 0, 0, step_h0 << 2);

      // first get MV predictor
      GetMVPredictor (currMB, block, &pred_mv, mv_info[j4][i4].ref_idx[list], mv_info, list, 0, 0, step_h0 << 2, step_v0 << 2);

      // X component
      currSE->value2 = list; // identifies the component; only used for context determination
      dP->readSyntaxElement(currMB, currSE, dP);
      curr_mvd[0] = (short) currSE->value1;              

      // Y component
      currSE->value2 += 2; // identifies the component; only used for context determination
      dP->readSyntaxElement(currMB, currSE, dP);
      curr_mvd[1] = (short) currSE->value1;              

      curr_mv.mv_x = (short)(curr_mvd[0] + pred_mv.mv_x);  // compute motion vector x
      curr_mv.mv_y = (short)(curr_mvd[1] + pred_mv.mv_y);  // compute motion vector y

      for(jj = j4; jj < j4 + step_v0; ++jj)
      {
        PicMotionParams *mvinfo = mv_info[jj] + i4;
        for(ii = i4; ii < i4 + step_h0; ++ii)
        {
          (mvinfo++)->mv[list] = curr_mv;
        }            
      }

      // Init first line (mvd)
      for(ii = 0; ii < step_h0; ++ii)
      {
        //*((int *) &mvd[0][ii][0]) = *((int *) curr_mvd);
        mvd[0][ii][0] = curr_mvd[0];
        mvd[0][ii][1] = curr_mvd[1];
      }              

      // now copy all other lines
      for(jj = 1; jj < step_v0; ++jj)
      {
        memcpy(mvd[jj][0], mvd[0][0],  2 * step_h0 * sizeof(short));
      }
    }
  }
  else
  {
    int i4, j4, ii, jj;
    short curr_mvd[2];
    MotionVector pred_mv, curr_mv;
    short (*mvd)[4][2];
    //VideoParameters *p_Vid = currMB->p_Vid;
    PicMotionParams **mv_info = currMB->p_Slice->dec_picture->mv_info;
    PixelPos block[4]; // neighbor blocks

    int i, j, i0, j0, kk, k;
    for (j0=0; j0<4; j0+=step_v0)
    {
      for (i0=0; i0<4; i0+=step_h0)
      {       
        kk = 2 * (j0 >> 1) + (i0 >> 1);

        if ((currMB->b8pdir[kk] == list || currMB->b8pdir[kk]== BI_PRED) && (currMB->b8mode[kk] != 0))//has forward vector
        {
          char cur_ref_idx = mv_info[currMB->block_y+j0][currMB->block_x+i0].ref_idx[list];
          int mv_mode  = currMB->b8mode[kk];
          int step_h = BLOCK_STEP [mv_mode][0];
          int step_v = BLOCK_STEP [mv_mode][1];
          int step_h4 = step_h << 2;
          int step_v4 = step_v << 2;

          for (j = j0; j < j0 + step_v0; j += step_v)
          {
            currMB->subblock_y = j << 2; // position used for context determination
            j4  = currMB->block_y + j;
            mvd = &currMB->mvd [list][j];

            for (i = i0; i < i0 + step_h0; i += step_h)
            {
              currMB->subblock_x = i << 2; // position used for context determination
              i4 = currMB->block_x + i;

              get_neighbors(currMB, block, BLOCK_SIZE * i, BLOCK_SIZE * j, step_h4);

              // first get MV predictor
              GetMVPredictor (currMB, block, &pred_mv, cur_ref_idx, mv_info, list, BLOCK_SIZE * i, BLOCK_SIZE * j, step_h4, step_v4);

              for (k=0; k < 2; ++k)
              {
                currSE->value2   = (k << 1) + list; // identifies the component; only used for context determination
                dP->readSyntaxElement(currMB, currSE, dP);
                curr_mvd[k] = (short) currSE->value1;              
              }

              curr_mv.mv_x = (short)(curr_mvd[0] + pred_mv.mv_x);  // compute motion vector 
              curr_mv.mv_y = (short)(curr_mvd[1] + pred_mv.mv_y);  // compute motion vector 

              for(jj = j4; jj < j4 + step_v; ++jj)
              {
                PicMotionParams *mvinfo = mv_info[jj] + i4;
                for(ii = i4; ii < i4 + step_h; ++ii)
                {
                  (mvinfo++)->mv[list] = curr_mv;
                }            
              }

              // Init first line (mvd)
              for(ii = i; ii < i + step_h; ++ii)
              {
                //*((int *) &mvd[0][ii][0]) = *((int *) curr_mvd);
                mvd[0][ii][0] = curr_mvd[0];
                mvd[0][ii][1] = curr_mvd[1];
              }              

              // now copy all other lines
              for(jj = 1; jj < step_v; ++jj)
              {
                memcpy(&mvd[jj][i][0], &mvd[0][i][0],  2 * step_h * sizeof(short));
              }
            }
          }
        }
      }
    }
  }
}

/*!
 ************************************************************************
 * \brief
 *    Read motion info
 ************************************************************************
 */
static void read_motion_info_from_NAL_p_slice (Macroblock *currMB)
{
  VideoParameters *p_Vid = currMB->p_Vid;
  Slice *currSlice = currMB->p_Slice;

  SyntaxElement currSE;
  DataPartition *dP = NULL;
  const byte *partMap       = assignSE2partition[currSlice->dp_mode];
  short partmode        = ((currMB->mb_type == P8x8) ? 4 : currMB->mb_type);
  int step_h0         = BLOCK_STEP [partmode][0];
  int step_v0         = BLOCK_STEP [partmode][1];

  int j4;
  StorablePicture *dec_picture = currSlice->dec_picture;
  PicMotionParams *mv_info = NULL;

  int list_offset = currMB->list_offset;
  StorablePicture **list0 = currSlice->listX[LIST_0 + list_offset];
  PicMotionParams **p_mv_info = &dec_picture->mv_info[currMB->block_y];

  //=====  READ REFERENCE PICTURE INDICES =====
  currSE.type = SE_REFFRAME;
  dP = &(currSlice->partArr[partMap[SE_REFFRAME]]);
  
  //  For LIST_0, if multiple ref. pictures, read LIST_0 reference picture indices for the MB ***********
  prepareListforRefIdx (currMB, &currSE, dP, currSlice->num_ref_idx_l0_active_minus1 + 1, (currMB->mb_type != P8x8) || (!currSlice->allrefzero));
  readMBRefPictureIdx  (&currSE, dP, currMB, p_mv_info, LIST_0, step_v0, step_h0);

  //=====  READ MOTION VECTORS =====
  currSE.type = SE_MVD;
  dP = &(currSlice->partArr[partMap[SE_MVD]]);

  if (p_Vid->active_pps->entropy_coding_mode_flag == (Boolean) CAVLC || dP->bitstream->ei_flag) 
    currSE.mapping = linfo_se;
  else                                                  
    currSE.reading = currSlice->MbaffFrameFlag ? read_mvd_CABAC_mbaff : read_MVD_CABAC;

  // LIST_0 Motion vectors
  readMBMotionVectors (&currSE, dP, currMB, LIST_0, step_h0, step_v0);

  // record reference picture Ids for deblocking decisions  
  for(j4 = 0; j4 < 4;++j4)
  {
    mv_info = &p_mv_info[j4][currMB->block_x];
    mv_info->ref_pic[LIST_0] = list0[(short) mv_info->ref_idx[LIST_0]];
    mv_info++;
    mv_info->ref_pic[LIST_0] = list0[(short) mv_info->ref_idx[LIST_0]];
    mv_info++;
    mv_info->ref_pic[LIST_0] = list0[(short) mv_info->ref_idx[LIST_0]];
    mv_info++;
    mv_info->ref_pic[LIST_0] = list0[(short) mv_info->ref_idx[LIST_0]];
  }
}


/*!
************************************************************************
* \brief
*    Read motion info
************************************************************************
*/
static void read_motion_info_from_NAL_b_slice (Macroblock *currMB)
{
  Slice *currSlice = currMB->p_Slice;
  VideoParameters *p_Vid = currMB->p_Vid;
  StorablePicture *dec_picture = currSlice->dec_picture;
  SyntaxElement currSE;
  DataPartition *dP = NULL;
  const byte *partMap = assignSE2partition[currSlice->dp_mode];
  int partmode        = ((currMB->mb_type == P8x8) ? 4 : currMB->mb_type);
  int step_h0         = BLOCK_STEP [partmode][0];
  int step_v0         = BLOCK_STEP [partmode][1];

  int j4, i4;

  int list_offset = currMB->list_offset; 
  StorablePicture **list0 = currSlice->listX[LIST_0 + list_offset];
  StorablePicture **list1 = currSlice->listX[LIST_1 + list_offset];
  PicMotionParams **p_mv_info = &dec_picture->mv_info[currMB->block_y];

  if (currMB->mb_type == P8x8)
    update_direct_mv_info(currMB);   

  //=====  READ REFERENCE PICTURE INDICES =====
  currSE.type = SE_REFFRAME;
  dP = &(currSlice->partArr[partMap[SE_REFFRAME]]);

  //  For LIST_0, if multiple ref. pictures, read LIST_0 reference picture indices for the MB ***********
  prepareListforRefIdx (currMB, &currSE, dP, currSlice->num_ref_idx_l0_active_minus1 + 1, TRUE);
  readMBRefPictureIdx  (&currSE, dP, currMB, p_mv_info, LIST_0, step_v0, step_h0);

  //  For LIST_1, if multiple ref. pictures, read LIST_1 reference picture indices for the MB ***********
  prepareListforRefIdx (currMB, &currSE, dP, currSlice->num_ref_idx_l1_active_minus1 + 1, TRUE);
  readMBRefPictureIdx  (&currSE, dP, currMB, p_mv_info, LIST_1, step_v0, step_h0);

  //=====  READ MOTION VECTORS =====
  currSE.type = SE_MVD;
  dP = &(currSlice->partArr[partMap[SE_MVD]]);

  if (p_Vid->active_pps->entropy_coding_mode_flag == (Boolean) CAVLC || dP->bitstream->ei_flag) 
    currSE.mapping = linfo_se;
  else                                                  
    currSE.reading = currSlice->MbaffFrameFlag ? read_mvd_CABAC_mbaff : read_MVD_CABAC;

  // LIST_0 Motion vectors
  readMBMotionVectors (&currSE, dP, currMB, LIST_0, step_h0, step_v0);
  // LIST_1 Motion vectors
  readMBMotionVectors (&currSE, dP, currMB, LIST_1, step_h0, step_v0);

  // record reference picture Ids for deblocking decisions

  for(j4 = 0; j4 < 4; ++j4)
  {
    for(i4 = currMB->block_x; i4 < (currMB->block_x + 4); ++i4)
    {
      PicMotionParams *mv_info = &p_mv_info[j4][i4];
      short ref_idx = mv_info->ref_idx[LIST_0];

      mv_info->ref_pic[LIST_0] = (ref_idx >= 0) ? list0[ref_idx] : NULL;        
      ref_idx = mv_info->ref_idx[LIST_1];
      mv_info->ref_pic[LIST_1] = (ref_idx >= 0) ? list1[ref_idx] : NULL;
    }
  }
}


/*!
 ************************************************************************
 * \brief
 *    Read Intra 8x8 Prediction modes
 *
 ************************************************************************
 */
static void read_ipred_8x8_modes_mbaff(Macroblock *currMB)
{
  int b8, bi, bj, bx, by, dec;
  SyntaxElement currSE;
  DataPartition *dP;
  Slice *currSlice = currMB->p_Slice;
  const byte *partMap = assignSE2partition[currSlice->dp_mode];
  VideoParameters *p_Vid = currMB->p_Vid;

  int mostProbableIntraPredMode;
  int upIntraPredMode;
  int leftIntraPredMode;

  PixelPos left_block, top_block;

  int mb_size[2] = { MB_BLOCK_SIZE, MB_BLOCK_SIZE };

  currSE.type = SE_INTRAPREDMODE;

  dP = &(currSlice->partArr[partMap[SE_INTRAPREDMODE]]);

  if (!(p_Vid->active_pps->entropy_coding_mode_flag == (Boolean) CAVLC || dP->bitstream->ei_flag))
    currSE.reading = readIntraPredMode_CABAC;

  for(b8 = 0; b8 < 4; ++b8)  //loop 8x8 blocks
  {
    by = (b8 & 0x02);
    bj = currMB->block_y + by;

    bx = ((b8 & 0x01) << 1);
    bi = currMB->block_x + bx;
    //get from stream
    if (p_Vid->active_pps->entropy_coding_mode_flag == (Boolean) CAVLC || dP->bitstream->ei_flag)
      readSyntaxElement_Intra4x4PredictionMode(&currSE, dP->bitstream);
    else
    {
      currSE.context = (b8 << 2);
      dP->readSyntaxElement(currMB, &currSE, dP);
    }

    get4x4Neighbour(currMB, (bx << 2) - 1, (by << 2),     mb_size, &left_block);
    get4x4Neighbour(currMB, (bx << 2),     (by << 2) - 1, mb_size, &top_block );

    //get from array and decode

    if (p_Vid->active_pps->constrained_intra_pred_flag)
    {
      left_block.available = left_block.available ? currSlice->intra_block[left_block.mb_addr] : 0;
      top_block.available  = top_block.available  ? currSlice->intra_block[top_block.mb_addr]  : 0;
    }

    upIntraPredMode            = (top_block.available ) ? currSlice->ipredmode[top_block.pos_y ][top_block.pos_x ] : -1;
    leftIntraPredMode          = (left_block.available) ? currSlice->ipredmode[left_block.pos_y][left_block.pos_x] : -1;

    mostProbableIntraPredMode  = (upIntraPredMode < 0 || leftIntraPredMode < 0) ? DC_PRED : upIntraPredMode < leftIntraPredMode ? upIntraPredMode : leftIntraPredMode;

    dec = (currSE.value1 == -1) ? mostProbableIntraPredMode : currSE.value1 + (currSE.value1 >= mostProbableIntraPredMode);

    //set
    //loop 4x4s in the subblock for 8x8 prediction setting
    currSlice->ipredmode[bj    ][bi    ] = (byte) dec;
    currSlice->ipredmode[bj    ][bi + 1] = (byte) dec;
    currSlice->ipredmode[bj + 1][bi    ] = (byte) dec;
    currSlice->ipredmode[bj + 1][bi + 1] = (byte) dec;             
  }
}

/*!
 ************************************************************************
 * \brief
 *    Read Intra 8x8 Prediction modes
 *
 ************************************************************************
 */
static void read_ipred_8x8_modes(Macroblock *currMB)
{
  int b8, bi, bj, bx, by, dec;
  SyntaxElement currSE;
  DataPartition *dP;
  Slice *currSlice = currMB->p_Slice;
  const byte *partMap = assignSE2partition[currSlice->dp_mode];
  VideoParameters *p_Vid = currMB->p_Vid;

  int mostProbableIntraPredMode;
  int upIntraPredMode;
  int leftIntraPredMode;

  PixelPos left_mb, top_mb;
  PixelPos left_block, top_block;

  int mb_size[2] = { MB_BLOCK_SIZE, MB_BLOCK_SIZE };

  currSE.type = SE_INTRAPREDMODE;

  dP = &(currSlice->partArr[partMap[SE_INTRAPREDMODE]]);

  if (!(p_Vid->active_pps->entropy_coding_mode_flag == (Boolean) CAVLC || dP->bitstream->ei_flag))
    currSE.reading = readIntraPredMode_CABAC;

  get4x4Neighbour(currMB, -1,  0, mb_size, &left_mb);
  get4x4Neighbour(currMB,  0, -1, mb_size, &top_mb );

  for(b8 = 0; b8 < 4; ++b8)  //loop 8x8 blocks
  {


    by = (b8 & 0x02);
    bj = currMB->block_y + by;

    bx = ((b8 & 0x01) << 1);
    bi = currMB->block_x + bx;

    //get from stream
    if (p_Vid->active_pps->entropy_coding_mode_flag == (Boolean) CAVLC || dP->bitstream->ei_flag)
      readSyntaxElement_Intra4x4PredictionMode(&currSE, dP->bitstream);
    else
    {
      currSE.context = (b8 << 2);
      dP->readSyntaxElement(currMB, &currSE, dP);
    }

    get4x4Neighbour(currMB, (bx<<2) - 1, (by<<2),     mb_size, &left_block);
    get4x4Neighbour(currMB, (bx<<2),     (by<<2) - 1, mb_size, &top_block );
    
    //get from array and decode

    if (p_Vid->active_pps->constrained_intra_pred_flag)
    {
      left_block.available = left_block.available ? currSlice->intra_block[left_block.mb_addr] : 0;
      top_block.available  = top_block.available  ? currSlice->intra_block[top_block.mb_addr]  : 0;
    }

    upIntraPredMode            = (top_block.available ) ? currSlice->ipredmode[top_block.pos_y ][top_block.pos_x ] : -1;
    leftIntraPredMode          = (left_block.available) ? currSlice->ipredmode[left_block.pos_y][left_block.pos_x] : -1;

    mostProbableIntraPredMode  = (upIntraPredMode < 0 || leftIntraPredMode < 0) ? DC_PRED : upIntraPredMode < leftIntraPredMode ? upIntraPredMode : leftIntraPredMode;

    dec = (currSE.value1 == -1) ? mostProbableIntraPredMode : currSE.value1 + (currSE.value1 >= mostProbableIntraPredMode);

    //set
    //loop 4x4s in the subblock for 8x8 prediction setting
    currSlice->ipredmode[bj    ][bi    ] = (byte) dec;
    currSlice->ipredmode[bj    ][bi + 1] = (byte) dec;
    currSlice->ipredmode[bj + 1][bi    ] = (byte) dec;
    currSlice->ipredmode[bj + 1][bi + 1] = (byte) dec;             
  }
}

/*!
 ************************************************************************
 * \brief
 *    Read Intra 4x4 Prediction modes
 *
 ************************************************************************
 */
static void read_ipred_4x4_modes_mbaff(Macroblock *currMB)
{
  int b8,i,j,bi,bj,bx,by;
  SyntaxElement currSE;
  DataPartition *dP;
  Slice *currSlice = currMB->p_Slice;
  const byte *partMap = assignSE2partition[currSlice->dp_mode];
  VideoParameters *p_Vid = currMB->p_Vid;
  BlockPos *PicPos = p_Vid->PicPos;

  int ts, ls;
  int mostProbableIntraPredMode;
  int upIntraPredMode;
  int leftIntraPredMode;

  PixelPos left_block, top_block;

  int mb_size[2] = { MB_BLOCK_SIZE, MB_BLOCK_SIZE };

  currSE.type = SE_INTRAPREDMODE;

  dP = &(currSlice->partArr[partMap[SE_INTRAPREDMODE]]);

  if (!(p_Vid->active_pps->entropy_coding_mode_flag == (Boolean) CAVLC || dP->bitstream->ei_flag))
    currSE.reading = readIntraPredMode_CABAC;

  for(b8 = 0; b8 < 4; ++b8)  //loop 8x8 blocks
  {           
    for(j = 0; j < 2; j++)  //loop subblocks
    {
      by = (b8 & 0x02) + j;
      bj = currMB->block_y + by;

      for(i = 0; i < 2; i++)
      {
        bx = ((b8 & 1) << 1) + i;
        bi = currMB->block_x + bx;
        //get from stream
        if (p_Vid->active_pps->entropy_coding_mode_flag == (Boolean) CAVLC || dP->bitstream->ei_flag)
          readSyntaxElement_Intra4x4PredictionMode(&currSE, dP->bitstream);
        else
        {
          currSE.context=(b8<<2) + (j<<1) +i;
          dP->readSyntaxElement(currMB, &currSE, dP);
        }

        get4x4Neighbour(currMB, (bx<<2) - 1, (by<<2),     mb_size, &left_block);
        get4x4Neighbour(currMB, (bx<<2),     (by<<2) - 1, mb_size, &top_block );

        //get from array and decode

        if (p_Vid->active_pps->constrained_intra_pred_flag)
        {
          left_block.available = left_block.available ? currSlice->intra_block[left_block.mb_addr] : 0;
          top_block.available  = top_block.available  ? currSlice->intra_block[top_block.mb_addr]  : 0;
        }

        // !! KS: not sure if the following is still correct...
        ts = ls = 0;   // Check to see if the neighboring block is SI
        if (currSlice->slice_type == SI_SLICE)           // need support for MBINTLC1
        {
          if (left_block.available)
            if (currSlice->siblock [PicPos[left_block.mb_addr].y][PicPos[left_block.mb_addr].x])
              ls=1;

          if (top_block.available)
            if (currSlice->siblock [PicPos[top_block.mb_addr].y][PicPos[top_block.mb_addr].x])
              ts=1;
        }

        upIntraPredMode            = (top_block.available  &&(ts == 0)) ? currSlice->ipredmode[top_block.pos_y ][top_block.pos_x ] : -1;
        leftIntraPredMode          = (left_block.available &&(ls == 0)) ? currSlice->ipredmode[left_block.pos_y][left_block.pos_x] : -1;

        mostProbableIntraPredMode  = (upIntraPredMode < 0 || leftIntraPredMode < 0) ? DC_PRED : upIntraPredMode < leftIntraPredMode ? upIntraPredMode : leftIntraPredMode;

        currSlice->ipredmode[bj][bi] = (byte) ((currSE.value1 == -1) ? mostProbableIntraPredMode : currSE.value1 + (currSE.value1 >= mostProbableIntraPredMode));
      }
    }
  }
}


/*!
 ************************************************************************
 * \brief
 *    Read Intra 4x4 Prediction modes
 *
 ************************************************************************
 */
static void read_ipred_4x4_modes(Macroblock *currMB)
{
  int b8,i,j,bi,bj,bx,by;
  SyntaxElement currSE;
  DataPartition *dP;
  Slice *currSlice = currMB->p_Slice;
  const byte *partMap = assignSE2partition[currSlice->dp_mode];
  VideoParameters *p_Vid = currMB->p_Vid;
  BlockPos *PicPos = p_Vid->PicPos;

  int ts, ls;
  int mostProbableIntraPredMode;
  int upIntraPredMode;
  int leftIntraPredMode;

  PixelPos left_mb, top_mb;
  PixelPos left_block, top_block;

  int mb_size[2] = { MB_BLOCK_SIZE, MB_BLOCK_SIZE };

  currSE.type = SE_INTRAPREDMODE;

  dP = &(currSlice->partArr[partMap[SE_INTRAPREDMODE]]);

  if (!(p_Vid->active_pps->entropy_coding_mode_flag == (Boolean) CAVLC || dP->bitstream->ei_flag))
    currSE.reading = readIntraPredMode_CABAC;

  get4x4Neighbour(currMB, -1,  0, mb_size, &left_mb);
  get4x4Neighbour(currMB,  0, -1, mb_size, &top_mb );

  for(b8 = 0; b8 < 4; ++b8)  //loop 8x8 blocks
  {       
    for(j = 0; j < 2; j++)  //loop subblocks
    {
      by = (b8 & 0x02) + j;
      bj = currMB->block_y + by;

      for(i = 0; i < 2; i++)
      {
        bx = ((b8 & 1) << 1) + i;
        bi = currMB->block_x + bx;
        //get from stream
        if (p_Vid->active_pps->entropy_coding_mode_flag == (Boolean) CAVLC || dP->bitstream->ei_flag)
          readSyntaxElement_Intra4x4PredictionMode(&currSE, dP->bitstream);
        else
        {
          currSE.context=(b8<<2) + (j<<1) +i;
          dP->readSyntaxElement(currMB, &currSE, dP);
        }

        get4x4Neighbour(currMB, (bx<<2) - 1, (by<<2),     mb_size, &left_block);
        get4x4Neighbour(currMB, (bx<<2),     (by<<2) - 1, mb_size, &top_block );

        //get from array and decode

        if (p_Vid->active_pps->constrained_intra_pred_flag)
        {
          left_block.available = left_block.available ? currSlice->intra_block[left_block.mb_addr] : 0;
          top_block.available  = top_block.available  ? currSlice->intra_block[top_block.mb_addr]  : 0;
        }

        // !! KS: not sure if the following is still correct...
        ts = ls = 0;   // Check to see if the neighboring block is SI
        if (currSlice->slice_type == SI_SLICE)           // need support for MBINTLC1
        {
          if (left_block.available)
            if (currSlice->siblock [PicPos[left_block.mb_addr].y][PicPos[left_block.mb_addr].x])
              ls=1;

          if (top_block.available)
            if (currSlice->siblock [PicPos[top_block.mb_addr].y][PicPos[top_block.mb_addr].x])
              ts=1;
        }

        upIntraPredMode            = (top_block.available  &&(ts == 0)) ? currSlice->ipredmode[top_block.pos_y ][top_block.pos_x ] : -1;
        leftIntraPredMode          = (left_block.available &&(ls == 0)) ? currSlice->ipredmode[left_block.pos_y][left_block.pos_x] : -1;

        mostProbableIntraPredMode  = (upIntraPredMode < 0 || leftIntraPredMode < 0) ? DC_PRED : upIntraPredMode < leftIntraPredMode ? upIntraPredMode : leftIntraPredMode;

        currSlice->ipredmode[bj][bi] = (byte) ((currSE.value1 == -1) ? mostProbableIntraPredMode : currSE.value1 + (currSE.value1 >= mostProbableIntraPredMode));
      }
    }
  }
}


/*!
 ************************************************************************
 * \brief
 *    Read Intra Prediction modes
 *
 ************************************************************************
 */
static void read_ipred_modes(Macroblock *currMB)
{
    Slice *currSlice = currMB->p_Slice;
    StorablePicture *dec_picture = currSlice->dec_picture;

    if (currSlice->MbaffFrameFlag) {
        if (currMB->mb_type == I8MB)
            read_ipred_8x8_modes_mbaff(currMB);
        else if (currMB->mb_type == I4MB)
            read_ipred_4x4_modes_mbaff(currMB);
    } else {
        if (currMB->mb_type == I8MB)
            read_ipred_8x8_modes(currMB);
        else if (currMB->mb_type == I4MB)
            read_ipred_4x4_modes(currMB);
    }

    if (dec_picture->chroma_format_idc != YUV400 && dec_picture->chroma_format_idc != YUV444) {
        VideoParameters *p_Vid = currMB->p_Vid;
        const byte *partMap = assignSE2partition[currSlice->dp_mode];
        DataPartition *dP = &(currSlice->partArr[partMap[SE_INTRAPREDMODE]]);
        SyntaxElement currSE;
        currSE.type = SE_INTRAPREDMODE;
        if (!p_Vid->active_pps->entropy_coding_mode_flag || dP->bitstream->ei_flag)
            currSE.mapping = linfo_ue;
        else
            currSE.reading = readCIPredMode_CABAC;

        dP->readSyntaxElement(currMB, &currSE, dP);
        currMB->c_ipred_mode = (char) currSE.value1;

        if (currMB->c_ipred_mode < DC_PRED_8 || currMB->c_ipred_mode > PLANE_8)
            error("illegal chroma intra pred mode!\n", 600);
    }
}


static inline void reset_mv_info(PicMotionParams *mv_info, int slice_no)
{
  mv_info->ref_pic[LIST_0] = NULL;
  mv_info->ref_pic[LIST_1] = NULL;
  mv_info->mv[LIST_0] = zero_mv;
  mv_info->mv[LIST_1] = zero_mv;
  mv_info->ref_idx[LIST_0] = -1;
  mv_info->ref_idx[LIST_1] = -1;
  mv_info->slice_no = slice_no;
}

static inline void reset_mv_info_list(PicMotionParams *mv_info, int list, int slice_no)
{
  mv_info->ref_pic[list] = NULL;
  mv_info->mv[list] = zero_mv;
  mv_info->ref_idx[list] = -1;
  mv_info->slice_no = slice_no;
}

/*!
 ************************************************************************
 * \brief
 *    init macroblock for skip mode. Only L1 info needs to be reset
 ************************************************************************
 */
static void init_macroblock_basic(Macroblock *currMB)
{
  int j, i;
  PicMotionParams **mv_info = &currMB->p_Slice->dec_picture->mv_info[currMB->block_y]; //&p_Vid->dec_picture->mv_info[currMB->block_y];
  int slice_no =  currMB->p_Slice->current_slice_nr;
  // reset vectors and pred. modes
  for(j = 0; j < BLOCK_SIZE; ++j)
  {                        
    i = currMB->block_x;
    reset_mv_info_list(*mv_info + (i++), LIST_1, slice_no);
    reset_mv_info_list(*mv_info + (i++), LIST_1, slice_no);
    reset_mv_info_list(*mv_info + (i++), LIST_1, slice_no);
    reset_mv_info_list(*(mv_info++) + i, LIST_1, slice_no);
  }
}

/*!
 ************************************************************************
 * \brief
 *    init macroblock (direct)
 ************************************************************************
 */
static void init_macroblock_direct(Macroblock *currMB)
{
  int slice_no = currMB->p_Slice->current_slice_nr;
  PicMotionParams **mv_info = &currMB->p_Slice->dec_picture->mv_info[currMB->block_y]; 
  int i, j;

  set_read_comp_coeff_cabac(currMB);
  set_read_comp_coeff_cavlc(currMB);
  i = currMB->block_x;
  for(j = 0; j < BLOCK_SIZE; ++j)
  {                        
    (*mv_info+i)->slice_no = slice_no;
    (*mv_info+i+1)->slice_no = slice_no;
    (*mv_info+i+2)->slice_no = slice_no;
    (*(mv_info++)+i+3)->slice_no = slice_no;
  }
}


/*!
 ************************************************************************
 * \brief
 *    init macroblock
 ************************************************************************
 */
static void init_macroblock(Macroblock *currMB)
{
  int j, i;
  Slice *currSlice = currMB->p_Slice;
  PicMotionParams **mv_info = &currSlice->dec_picture->mv_info[currMB->block_y]; 
  int slice_no = currSlice->current_slice_nr;
  // reset vectors and pred. modes

  for(j = 0; j < BLOCK_SIZE; ++j)
  {                        
    i = currMB->block_x;
    reset_mv_info(*mv_info + (i++), slice_no);
    reset_mv_info(*mv_info + (i++), slice_no);
    reset_mv_info(*mv_info + (i++), slice_no);
    reset_mv_info(*(mv_info++) + i, slice_no);
  }

  set_read_comp_coeff_cabac(currMB);
  set_read_comp_coeff_cavlc(currMB);
}

static void concealIPCMcoeffs(Macroblock *currMB)
{
    Slice *currSlice = currMB->p_Slice;
    sps_t *sps = currSlice->active_sps;
    int i, j, k;

    int mb_cr_size_x = sps->chroma_format_idc == YUV400 ? 0 :
                       sps->chroma_format_idc == YUV444 ? 16 : 8;
    int mb_cr_size_y = sps->chroma_format_idc == YUV400 ? 0 :
                       sps->chroma_format_idc == YUV420 ? 8 : 16;

    for(i=0;i<MB_BLOCK_SIZE;++i) {
        for(j=0;j<MB_BLOCK_SIZE;++j)
        currSlice->cof[0][i][j] = (1 << (sps->BitDepthY - 1));
    }

    if (sps->chroma_format_idc != YUV400 && !sps->separate_colour_plane_flag) {
        for (k = 0; k < 2; ++k)
            for(i=0;i<mb_cr_size_y;++i)
                for(j=0;j<mb_cr_size_x;++j)
                    currSlice->cof[k][i][j] = (1 << (sps->BitDepthC - 1));
    }
}


void readIPCM_CABAC(Slice *currSlice, struct datapartition_dec *dP)
{
    sps_t *sps = currSlice->active_sps;
    StorablePicture *dec_picture = currSlice->dec_picture;
    Bitstream* currStream = dP->bitstream;
    DecodingEnvironment *dep = &dP->bitstream->de_cabac;
    byte *buf = currStream->streamBuffer;
    int BitstreamLengthInBits = (dP->bitstream->bitstream_length << 3) + 7;

    int val = 0;
    int bits_read = 0;
    int bitoffset, bitdepth;
    int uv, i, j;

    int mb_cr_size_x = sps->chroma_format_idc == YUV400 ? 0 :
                       sps->chroma_format_idc == YUV444 ? 16 : 8;
    int mb_cr_size_y = sps->chroma_format_idc == YUV400 ? 0 :
                       sps->chroma_format_idc == YUV420 ? 8 : 16;

    while (dep->DbitsLeft >= 8) {
        dep->Dvalue   >>= 8;
        dep->DbitsLeft -= 8;
        (*dep->Dcodestrm_len)--;
    }

    bitoffset = (*dep->Dcodestrm_len) << 3;

    // read luma values
    bitdepth = sps->BitDepthY;
    for (i = 0; i < MB_BLOCK_SIZE; i++) {
        for (j = 0; j < MB_BLOCK_SIZE; j++) {
            bits_read += GetBits(buf, bitoffset, &val, BitstreamLengthInBits, bitdepth);
            currSlice->cof[0][i][j] = val;

            bitoffset += bitdepth;
        }
    }

    // read chroma values
    bitdepth = sps->BitDepthC;
    if (dec_picture->chroma_format_idc != YUV400 && !sps->separate_colour_plane_flag) {
        for (uv = 1; uv < 3; ++uv) {
            for (i = 0; i < mb_cr_size_y; ++i) {
                for (j = 0; j < mb_cr_size_x; ++j) {
                    bits_read += GetBits(buf, bitoffset, &val, BitstreamLengthInBits, bitdepth);
                    currSlice->cof[uv][i][j] = val;

                    bitoffset += bitdepth;
                }
            }
        }
    }

    (*dep->Dcodestrm_len) += ( bits_read >> 3);
    if (bits_read & 7)
        ++(*dep->Dcodestrm_len);

    arideco_start_decoding(&currStream->de_cabac, currStream->streamBuffer, currStream->read_len, &currStream->read_len);
}

static void read_IPCM_coeffs_from_NAL(Slice *currSlice, struct datapartition_dec *dP)
{
    sps_t *sps = currSlice->active_sps;
    pps_t *pps = currSlice->active_pps;

    Bitstream *currStream = dP->bitstream;
    int i, j;

    int mb_cr_size_x = sps->chroma_format_idc == YUV400 ? 0 :
                       sps->chroma_format_idc == YUV444 ? 16 : 8;
    int mb_cr_size_y = sps->chroma_format_idc == YUV400 ? 0 :
                       sps->chroma_format_idc == YUV420 ? 8 : 16;

    //For CABAC, we don't need to read bits to let stream byte aligned
    //  because we have variable for integer bytes position
    if (pps->entropy_coding_mode_flag) {
        readIPCM_CABAC(currSlice, dP);
        return;
    }

    //read bits to let stream byte aligned
    if ((dP->bitstream->frame_bitoffset & 0x07) != 0)
        currStream->f(8 - (currStream->frame_bitoffset & 0x07));

    //read luma and chroma IPCM coefficients
    for (i = 0; i < MB_BLOCK_SIZE; i++) {
        for (j = 0; j < MB_BLOCK_SIZE; j++)
            currSlice->cof[0][i][j] = currStream->f(sps->BitDepthY);
    }

    if (sps->chroma_format_idc != YUV400 && !sps->separate_colour_plane_flag) {
        for (i = 0; i < mb_cr_size_y; i++) {
            for (j = 0; j < mb_cr_size_x; j++)
                currSlice->cof[1][i][j] = currStream->f(sps->BitDepthC);
        }
        for (i = 0; i < mb_cr_size_y; i++) {
            for (j = 0; j < mb_cr_size_x; j++)
                currSlice->cof[2][i][j] = currStream->f(sps->BitDepthC);
        }
    }
}

/*!
 ************************************************************************
 * \brief
 *    Sets mode for 8x8 block
 ************************************************************************
 */
static inline void SetB8Mode (Macroblock* currMB, int value, int i)
{
  Slice* currSlice = currMB->p_Slice;
  static const char p_v2b8 [ 5] = {4, 5, 6, 7, IBLOCK};
  static const char p_v2pd [ 5] = {0, 0, 0, 0, -1};
  static const char b_v2b8 [14] = {0, 4, 4, 4, 5, 6, 5, 6, 5, 6, 7, 7, 7, IBLOCK};
  static const char b_v2pd [14] = {2, 0, 1, 2, 0, 0, 1, 1, 2, 2, 0, 1, 2, -1};

  if (currSlice->slice_type==B_SLICE)
  {
    currMB->b8mode[i] = b_v2b8[value];
    currMB->b8pdir[i] = b_v2pd[value];
  }
  else
  {
    currMB->b8mode[i] = p_v2b8[value];
    currMB->b8pdir[i] = p_v2pd[value];
  }
}

static inline void reset_coeffs(Macroblock *currMB)
{
  VideoParameters *p_Vid = currMB->p_Vid;

  // CAVLC
  if (p_Vid->active_pps->entropy_coding_mode_flag == (Boolean) CAVLC)
    memset(p_Vid->nz_coeff[currMB->mbAddrX][0][0], 0, 3 * BLOCK_PIXELS * sizeof(byte));
}

static inline void field_flag_inference(Macroblock *currMB)
{
  VideoParameters *p_Vid = currMB->p_Vid;
  if (currMB->mbAvailA)
  {
    currMB->mb_field_decoding_flag = p_Vid->mb_data[currMB->mbAddrA].mb_field_decoding_flag;
  }
  else
  {
    // check top macroblock pair
    currMB->mb_field_decoding_flag = currMB->mbAvailB ? p_Vid->mb_data[currMB->mbAddrB].mb_field_decoding_flag : FALSE;
  }
}


static void skip_macroblock(Macroblock *currMB)
{
  MotionVector pred_mv;
  int zeroMotionAbove;
  int zeroMotionLeft;
  PixelPos mb[4];    // neighbor blocks
  int   i, j;
  int   a_mv_y = 0;
  int   a_ref_idx = 0;
  int   b_mv_y = 0;
  int   b_ref_idx = 0;
  int   img_block_y   = currMB->block_y;
  VideoParameters *p_Vid = currMB->p_Vid;
  Slice *currSlice = currMB->p_Slice;
  int   list_offset = LIST_0 + currMB->list_offset;
  StorablePicture *dec_picture = currSlice->dec_picture;
  MotionVector *a_mv = NULL;
  MotionVector *b_mv = NULL;

  get_neighbors(currMB, mb, 0, 0, MB_BLOCK_SIZE);
  if (currSlice->MbaffFrameFlag == 0)
  {
    if (mb[0].available)
    {
      a_mv      = &dec_picture->mv_info[mb[0].pos_y][mb[0].pos_x].mv[LIST_0];
      a_mv_y    = a_mv->mv_y;    
      a_ref_idx = dec_picture->mv_info[mb[0].pos_y][mb[0].pos_x].ref_idx[LIST_0];
    }

    if (mb[1].available)
    {
      b_mv      = &dec_picture->mv_info[mb[1].pos_y][mb[1].pos_x].mv[LIST_0];
      b_mv_y    = b_mv->mv_y;
      b_ref_idx = dec_picture->mv_info[mb[1].pos_y][mb[1].pos_x].ref_idx[LIST_0];
    }
  }
  else
  {
    if (mb[0].available)
    {
      a_mv      = &dec_picture->mv_info[mb[0].pos_y][mb[0].pos_x].mv[LIST_0];
      a_mv_y    = a_mv->mv_y;    
      a_ref_idx = dec_picture->mv_info[mb[0].pos_y][mb[0].pos_x].ref_idx[LIST_0];

      if (currMB->mb_field_decoding_flag && !p_Vid->mb_data[mb[0].mb_addr].mb_field_decoding_flag)
      {
        a_mv_y    /=2;
        a_ref_idx *=2;
      }
      if (!currMB->mb_field_decoding_flag && p_Vid->mb_data[mb[0].mb_addr].mb_field_decoding_flag)
      {
        a_mv_y    *=2;
        a_ref_idx >>=1;
      }
    }

    if (mb[1].available)
    {
      b_mv      = &dec_picture->mv_info[mb[1].pos_y][mb[1].pos_x].mv[LIST_0];
      b_mv_y    = b_mv->mv_y;
      b_ref_idx = dec_picture->mv_info[mb[1].pos_y][mb[1].pos_x].ref_idx[LIST_0];

      if (currMB->mb_field_decoding_flag && !p_Vid->mb_data[mb[1].mb_addr].mb_field_decoding_flag)
      {
        b_mv_y    /=2;
        b_ref_idx *=2;
      }
      if (!currMB->mb_field_decoding_flag && p_Vid->mb_data[mb[1].mb_addr].mb_field_decoding_flag)
      {
        b_mv_y    *=2;
        b_ref_idx >>=1;
      }
    }
  }

  zeroMotionLeft  = !mb[0].available ? 1 : a_ref_idx==0 && a_mv->mv_x == 0 && a_mv_y==0 ? 1 : 0;
  zeroMotionAbove = !mb[1].available ? 1 : b_ref_idx==0 && b_mv->mv_x == 0 && b_mv_y==0 ? 1 : 0;

  currMB->cbp = 0;
  reset_coeffs(currMB);

  if (zeroMotionAbove || zeroMotionLeft)
  {
    PicMotionParams **dec_mv_info = &dec_picture->mv_info[img_block_y];
    StorablePicture *cur_pic = currSlice->listX[list_offset][0];
    PicMotionParams *mv_info = NULL;
    
    for(j = 0; j < BLOCK_SIZE; ++j)
    {
      for(i = currMB->block_x; i < currMB->block_x + BLOCK_SIZE; ++i)
      {
        mv_info = &dec_mv_info[j][i];
        mv_info->ref_pic[LIST_0] = cur_pic;
        mv_info->mv     [LIST_0] = zero_mv;
        mv_info->ref_idx[LIST_0] = 0;
      }
    }
  }
  else
  {
    PicMotionParams **dec_mv_info = &dec_picture->mv_info[img_block_y];
    PicMotionParams *mv_info = NULL;
    StorablePicture *cur_pic = currSlice->listX[list_offset][0];
    GetMVPredictor (currMB, mb, &pred_mv, 0, dec_picture->mv_info, LIST_0, 0, 0, MB_BLOCK_SIZE, MB_BLOCK_SIZE);

    // Set first block line (position img_block_y)
    for(j = 0; j < BLOCK_SIZE; ++j)
    {
      for(i = currMB->block_x; i < currMB->block_x + BLOCK_SIZE; ++i)
      {
        mv_info = &dec_mv_info[j][i];
        mv_info->ref_pic[LIST_0] = cur_pic;
        mv_info->mv     [LIST_0] = pred_mv;
        mv_info->ref_idx[LIST_0] = 0;
      }
    }
  }
}


static void read_skip_macroblock(Macroblock *currMB)
{
    Slice *currSlice = currMB->p_Slice;
    int mb_nr = currMB->mbAddrX;
    bool isCabac = currSlice->p_Vid->active_pps->entropy_coding_mode_flag;

    if (currSlice->slice_type == B_SLICE) {
        //init NoMbPartLessThan8x8Flag
        currMB->NoMbPartLessThan8x8Flag = !currSlice->active_sps->direct_8x8_inference_flag ? FALSE : TRUE;
        currMB->luma_transform_size_8x8_flag = FALSE;
        if (currMB->p_Vid->active_pps->constrained_intra_pred_flag)
            currSlice->intra_block[mb_nr] = 0;

        //--- init macroblock data ---
        init_macroblock_direct(currMB);

        if (currSlice->cod_counter >= 0) {
            currMB->cbp = 0;
            if (isCabac) {
                currSlice->is_reset_coeff = TRUE;
                currSlice->cod_counter = -1;
            } else
                reset_coeffs(currMB);
        } else
            // read CBP and Coeffs  ***************************************************************
            currSlice->read_CBP_and_coeffs_from_NAL(currMB);
    } else {
        currMB->luma_transform_size_8x8_flag = FALSE;

        if (currMB->p_Vid->active_pps->constrained_intra_pred_flag) {
            int mb_nr = currMB->mbAddrX; 
            currMB->p_Slice->intra_block[mb_nr] = 0;
        }

        //--- init macroblock data ---
        init_macroblock_basic(currMB);
        skip_macroblock(currMB);
    }
}

static void read_intra_macroblock(Macroblock *currMB)
{
    Slice *currSlice = currMB->p_Slice;

    if (currMB->mb_type != I4MB)
        currMB->NoMbPartLessThan8x8Flag = TRUE;

    //============= Transform Size Flag for INTRA MBs =============
    //-------------------------------------------------------------
    //transform size flag for INTRA_4x4 and INTRA_8x8 modes
    if (currMB->mb_type == I4MB && currSlice->active_pps->transform_8x8_mode_flag) {
        bool isCabac = currSlice->p_Vid->active_pps->entropy_coding_mode_flag;
        const byte *partMap = assignSE2partition[currSlice->dp_mode];
        DataPartition *dP = &(currSlice->partArr[partMap[SE_HEADER]]);
        SyntaxElement currSE;
        currSE.type = SE_HEADER;
        if (isCabac)
            currSE.reading = readMB_transform_size_flag_CABAC;

        // read CAVLC transform_size_8x8_flag
        if (!isCabac || dP->bitstream->ei_flag) {
            currSE.value1 = dP->bitstream->f(1);
        } else
            dP->readSyntaxElement(currMB, &currSE, dP);

        currMB->luma_transform_size_8x8_flag = (Boolean) currSE.value1;

        if (currMB->luma_transform_size_8x8_flag) {
            currMB->mb_type = I8MB;
            memset(&currMB->b8mode, I8MB, 4 * sizeof(char));
            memset(&currMB->b8pdir, -1, 4 * sizeof(char));
        }
    } else
        currMB->luma_transform_size_8x8_flag = FALSE;

    init_macroblock(currMB);
    read_ipred_modes(currMB);
    currSlice->read_CBP_and_coeffs_from_NAL(currMB);
}

static void read_inter_macroblock(Macroblock *currMB)
{
    Slice *currSlice = currMB->p_Slice;
    //init NoMbPartLessThan8x8Flag
    currMB->NoMbPartLessThan8x8Flag = TRUE;
    currMB->luma_transform_size_8x8_flag = FALSE;

    if (currMB->mb_type == P8x8) {
        bool isCabac = currSlice->p_Vid->active_pps->entropy_coding_mode_flag;
        const byte *partMap = assignSE2partition[currSlice->dp_mode];
        DataPartition *dP = &(currSlice->partArr[partMap[SE_MBTYPE]]);
        SyntaxElement currSE;
        currSE.type = SE_MBTYPE;      
        if (!isCabac || dP->bitstream->ei_flag) 
            currSE.mapping = linfo_ue;
        else
            currSE.reading = currSlice->slice_type != B_SLICE ? readB8_typeInfo_CABAC_p_slice :
                                                                readB8_typeInfo_CABAC_b_slice;

        for (int i = 0; i < 4; ++i) {
            dP->readSyntaxElement (currMB, &currSE, dP);
            SetB8Mode (currMB, currSE.value1, i);

            //set NoMbPartLessThan8x8Flag for P8x8 mode
            currMB->NoMbPartLessThan8x8Flag &= 
                (currMB->b8mode[i] == 0 && currSlice->active_sps->direct_8x8_inference_flag) ||
                (currMB->b8mode[i] == 4);
        }
    }

    if (currMB->p_Vid->active_pps->constrained_intra_pred_flag) {
        int mb_nr = currMB->mbAddrX;
        currSlice->intra_block[mb_nr] = 0;
    }

    init_macroblock(currMB);
    read_motion_info_from_NAL(currMB);
    currSlice->read_CBP_and_coeffs_from_NAL(currMB);
}

static void read_i_pcm_macroblock(Macroblock *currMB, const byte *partMap)
{
    Slice *currSlice = currMB->p_Slice;
    currMB->NoMbPartLessThan8x8Flag = TRUE;
    currMB->luma_transform_size_8x8_flag = FALSE;

    //--- init macroblock data ---
    init_macroblock(currMB);

    //read pcm_alignment_zero_bit and pcm_byte[i]

    // here dP is assigned with the same dP as SE_MBTYPE, because IPCM syntax is in the
    // same category as MBTYPE
    if ( currSlice->dp_mode && currSlice->dpB_NotPresent )
        concealIPCMcoeffs(currMB);
    else {
        DataPartition *dP = &(currSlice->partArr[partMap[SE_LUM_DC_INTRA]]);
        read_IPCM_coeffs_from_NAL(currSlice, dP);
    }
}




static bool check_mb_skip_cavlc(Macroblock *currMB)
{
    VideoParameters *p_Vid = currMB->p_Vid;
    Slice *currSlice = currMB->p_Slice;
    int mb_nr = currMB->mbAddrX; 
    DataPartition *dP;
    SyntaxElement currSE;
    const byte *partMap = assignSE2partition[currSlice->dp_mode];

    if (currSlice->MbaffFrameFlag)
        currMB->mb_field_decoding_flag = ((mb_nr&0x01) == 0)? FALSE : p_Vid->mb_data[mb_nr-1].mb_field_decoding_flag;
    else
        currMB->mb_field_decoding_flag = FALSE;

    update_qp(currMB, currSlice->SliceQpY);

    //  read MB mode *****************************************************************
    dP = &(currSlice->partArr[partMap[SE_MBTYPE]]);
    currSE.type = SE_MBTYPE;
    currSE.mapping = linfo_ue;

    // VLC Non-Intra  
    if (currSlice->cod_counter == -1)
        currSlice->cod_counter = dP->bitstream->ue();

    if (currSlice->cod_counter == 0) {
        currSlice->cod_counter--;
        if (currSlice->MbaffFrameFlag) {
            int prevMbSkipped = 0;
            if (mb_nr & 0x01) {
                Macroblock *topMB = &p_Vid->mb_data[mb_nr-1];
                prevMbSkipped = topMB->mb_skip_flag;
            } else
                prevMbSkipped = 0;

            // read MB aff
            if ((((mb_nr&0x01)==0) || ((mb_nr&0x01) && prevMbSkipped))) {
                currMB->mb_field_decoding_flag = dP->bitstream->f(1);
            }
        }
        return 1;
    }

    currSlice->cod_counter--;
    currMB->ei_flag = 0;
    currMB->mb_skip_flag = 1;      
    currMB->mb_type      = 0;

    if (currSlice->MbaffFrameFlag && (mb_nr & 0x01) == 0) {
        // read field flag of bottom block
        if (currSlice->cod_counter == 0) {
            currMB->mb_field_decoding_flag = dP->bitstream->f(1);
            dP->bitstream->frame_bitoffset--;
        } else if (currSlice->cod_counter > 0) {
            // check left macroblock pair first
            if (mb_is_available(mb_nr - 2, currMB) && (mb_nr % (p_Vid->active_sps->PicWidthInMbs * 2)) != 0)
                currMB->mb_field_decoding_flag = p_Vid->mb_data[mb_nr - 2].mb_field_decoding_flag;
            // check top macroblock pair
            else if (mb_is_available(mb_nr - 2 * p_Vid->active_sps->PicWidthInMbs, currMB))
                currMB->mb_field_decoding_flag = p_Vid->mb_data[mb_nr - 2 * p_Vid->active_sps->PicWidthInMbs].mb_field_decoding_flag;
            else
                currMB->mb_field_decoding_flag = FALSE;
        }
    }

    return 0;
}

static bool check_mb_skip_cabac(Macroblock *currMB)
{
    Slice *currSlice = currMB->p_Slice;  
    VideoParameters *p_Vid = currMB->p_Vid;
    int mb_nr = currMB->mbAddrX;
    SyntaxElement currSE;
    DataPartition *dP;
    const byte *partMap = assignSE2partition[currSlice->dp_mode];

    if (currSlice->MbaffFrameFlag)
        currMB->mb_field_decoding_flag = (mb_nr & 0x01) == 0 ? FALSE : p_Vid->mb_data[mb_nr-1].mb_field_decoding_flag;
    else
        currMB->mb_field_decoding_flag = FALSE;

    update_qp(currMB, currSlice->SliceQpY);

    //  read MB mode *****************************************************************
    dP = &(currSlice->partArr[partMap[SE_MBTYPE]]);
    currSE.type = SE_MBTYPE;
    if (dP->bitstream->ei_flag)   
        currSE.mapping = linfo_ue;

    if (currSlice->MbaffFrameFlag) {
        // read MB skip_flag
        int prevMbSkipped = 0;
        if (mb_nr & 0x01) {
            Macroblock *topMB = &p_Vid->mb_data[mb_nr - 1];
            prevMbSkipped = topMB->mb_skip_flag;
        } else
            prevMbSkipped = 0;
        if ((mb_nr & 0x01) == 0 || prevMbSkipped)
            field_flag_inference(currMB);
    }

    CheckAvailabilityOfNeighborsCABAC(currMB);
    currSE.reading = read_skip_flag_CABAC;
    dP->readSyntaxElement(currMB, &currSE, dP);

    currMB->mb_type      = (short) currSE.value1;
    currMB->mb_skip_flag = (char) !currSE.value1;
    currMB->cbp          = currSE.value1;
    if (!dP->bitstream->ei_flag)
        currMB->ei_flag = 0;

    if (currSE.value1 == 0)
        currSlice->cod_counter = 0;

    if (currSlice->MbaffFrameFlag) {
        // read MB AFF
        int check_bottom, read_bottom, read_top;  
        check_bottom = read_bottom = read_top = 0;
        if ((mb_nr & 0x01) == 0) {
            check_bottom = currMB->mb_skip_flag;
            read_top = !check_bottom;
        } else {
            Macroblock *topMB = &p_Vid->mb_data[mb_nr - 1];
            read_bottom = topMB->mb_skip_flag && !currMB->mb_skip_flag;
        }

        if (read_bottom || read_top) {
            currSE.reading = readFieldModeInfo_CABAC;
            dP->readSyntaxElement(currMB, &currSE, dP);
            currMB->mb_field_decoding_flag = (Boolean) currSE.value1;
        }

        if (check_bottom)
            check_next_mb_and_get_field_mode_CABAC(currSlice, &currSE, dP);
        //update the list offset;
        currMB->list_offset = currMB->mb_field_decoding_flag ? (mb_nr & 0x01 ? 4 : 2) : 0;
        CheckAvailabilityOfNeighborsCABAC(currMB);    
    }

    return currMB->mb_type != 0;
}

static void read_one_macroblock_i_slice(Macroblock *currMB)
{
    Slice *currSlice = currMB->p_Slice;
    bool isCabac = currSlice->p_Vid->active_pps->entropy_coding_mode_flag;

    SyntaxElement currSE;
    int mb_nr = currMB->mbAddrX; 

    DataPartition *dP;
    const byte *partMap = assignSE2partition[currSlice->dp_mode];
    StorablePicture *dec_picture = currSlice->dec_picture; 
    PicMotionParamsOld *motion = &dec_picture->motion;

    currMB->mb_field_decoding_flag = ((mb_nr&0x01) == 0)? FALSE : currSlice->mb_data[mb_nr-1].mb_field_decoding_flag; 

    update_qp(currMB, currSlice->SliceQpY);

    //  read MB mode *****************************************************************
    dP = &(currSlice->partArr[partMap[SE_MBTYPE]]);
    currSE.type = SE_MBTYPE;
    if (!isCabac || dP->bitstream->ei_flag)   
        currSE.mapping = linfo_ue;

    // read MB aff
    if (currSlice->MbaffFrameFlag && (mb_nr & 0x01) == 0) {
        if (!isCabac || dP->bitstream->ei_flag) {
            currSE.value1 = dP->bitstream->f(1);
        } else {
            currSE.reading = readFieldModeInfo_CABAC;
            dP->readSyntaxElement(currMB, &currSE, dP);
        }
        currMB->mb_field_decoding_flag = (Boolean) currSE.value1;
    }

    if (isCabac)
        CheckAvailabilityOfNeighborsCABAC(currMB);

    //  read MB type
    //if (isCabac)
    //    currSE.reading = readMB_typeInfo_CABAC_i_slice;
    //dP->readSyntaxElement(currMB, &currSE, dP);
    currSE.value1 = getSE(currMB, SE_MBTYPE);
    currMB->mb_type = (short) currSE.value1;
    //if (!dP->bitstream->ei_flag)
    //    currMB->ei_flag = 0;

    motion->mb_field_decoding_flag[mb_nr] = (byte) currMB->mb_field_decoding_flag;

    currMB->block_y_aff = (currSlice->MbaffFrameFlag && currMB->mb_field_decoding_flag) ?
                          (mb_nr & 0x01) ? (currMB->block_y - 4) >> 1 :
                                            currMB->block_y >> 1 : currMB->block_y;

    currSlice->siblock[currMB->mb.y][currMB->mb.x] = 0;

    interpret_mb_mode(currMB);

    //init NoMbPartLessThan8x8Flag
    currMB->NoMbPartLessThan8x8Flag = TRUE;

    if (currMB->mb_type == IPCM)
        read_i_pcm_macroblock(currMB, partMap);
    else
        read_intra_macroblock(currMB);
}

/*!
 ************************************************************************
 * \brief
 *    Get the syntax elements from the NAL
 ************************************************************************
 */
static void read_one_macroblock_pb_slice(Macroblock *currMB)
{
    Slice *currSlice = currMB->p_Slice;  
    int mb_nr = currMB->mbAddrX;
    SyntaxElement currSE;
    DataPartition *dP;
    const byte *partMap = assignSE2partition[currSlice->dp_mode];
    bool isCabac = currSlice->p_Vid->active_pps->entropy_coding_mode_flag;

    bool mb_skip;
    if (isCabac)
        mb_skip = check_mb_skip_cabac(currMB);
    else
        mb_skip = check_mb_skip_cavlc(currMB);

    // read MB type
    if (mb_skip) {
        dP = &(currSlice->partArr[partMap[SE_MBTYPE]]);
        //currSE.type = SE_MBTYPE;
        //if (!isCabac || dP->bitstream->ei_flag)   
        //    currSE.mapping = linfo_ue;
        //else
        //    currSE.reading = currSlice->slice_type != B_SLICE ? readMB_typeInfo_CABAC_p_slice :
        //                                                        readMB_typeInfo_CABAC_b_slice;
        //dP->readSyntaxElement(currMB, &currSE, dP);
        currSE.value1 = getSE(currMB, SE_MBTYPE);
        if (!isCabac && currSlice->slice_type != B_SLICE)
            ++(currSE.value1);
        currMB->mb_type = (short) currSE.value1;
        //if (!dP->bitstream->ei_flag)
        //    currMB->ei_flag = 0;
        if (!isCabac)
            currMB->mb_skip_flag = 0;
    }

    StorablePicture *dec_picture = currSlice->dec_picture;
    PicMotionParamsOld *motion = &dec_picture->motion;
    if (!currSlice->MbaffFrameFlag) {
        if (!isCabac)
            //update the list offset;
            currMB->list_offset = 0;  

        motion->mb_field_decoding_flag[mb_nr] = FALSE;
        currMB->block_y_aff = currMB->block_y;
        currSlice->siblock[currMB->mb.y][currMB->mb.x] = 0;
        interpret_mb_mode(currMB);    
    } else {
        if (!isCabac)
            //update the list offset;
            currMB->list_offset = (currMB->mb_field_decoding_flag)? (mb_nr & 0x01 ? 4 : 2) : 0;

        motion->mb_field_decoding_flag[mb_nr] = (byte) currMB->mb_field_decoding_flag;
        currMB->block_y_aff = (currMB->mb_field_decoding_flag) ?
                              (mb_nr & 0x01) ? (currMB->block_y - 4) >> 1 :
                                                currMB->block_y >> 1 : currMB->block_y;
        currSlice->siblock[currMB->mb.y][currMB->mb.x] = 0;
        interpret_mb_mode(currMB);

        if (currMB->mb_field_decoding_flag) {
            currSlice->num_ref_idx_l0_active_minus1 = ((currSlice->num_ref_idx_l0_active_minus1 + 1) << 1) - 1;
            currSlice->num_ref_idx_l1_active_minus1 = ((currSlice->num_ref_idx_l1_active_minus1 + 1) << 1) - 1;
        }
    }

    if (currSlice->slice_type != B_SLICE)
        //init NoMbPartLessThan8x8Flag
        currMB->NoMbPartLessThan8x8Flag = TRUE;

    if (currMB->mb_type == IPCM) // I_PCM mode
        read_i_pcm_macroblock(currMB, partMap);
    else if (currMB->mb_type == PSKIP || currMB->mb_type == BSKIP_DIRECT)
        read_skip_macroblock(currMB);
    else if (currMB->is_intra_block) // all other intra modes
        read_intra_macroblock(currMB);
    else // all other remaining modes
        read_inter_macroblock(currMB);
}



void read_one_macroblock(Macroblock *currMB)
{
    Slice *currSlice = currMB->p_Slice;

    switch (currSlice->slice_type) {
    case I_SLICE:
    case SI_SLICE:
        read_one_macroblock_i_slice(currMB);
        return;
    case P_SLICE:
    case SP_SLICE:
    case B_SLICE:
        read_one_macroblock_pb_slice(currMB);
        return;
    }
}

void read_motion_info_from_NAL(Macroblock *currMB)
{
    Slice *currSlice = currMB->p_Slice;

    switch (currSlice->slice_type) {
    case P_SLICE: 
    case SP_SLICE:
        read_motion_info_from_NAL_p_slice(currMB);
        break;
    case B_SLICE:
        read_motion_info_from_NAL_b_slice(currMB);
        break;
    case I_SLICE: 
    case SI_SLICE: 
        break;
    default:
        printf("Unsupported slice type\n");
        break;
    }
}

void setup_read_macroblock(Slice *currSlice)
{

    if (!currSlice->p_Vid->active_pps->entropy_coding_mode_flag)
        set_read_CBP_and_coeffs_cavlc(currSlice);
    else
        set_read_CBP_and_coeffs_cabac(currSlice);
}
