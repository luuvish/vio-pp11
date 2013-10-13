#include "global.h"
#include "input_parameters.h"
#include "slice.h"
#include "dpb.h"
#include "memalloc.h"
#include "erc_do.h"
#include "macroblock.h"


//! look up tables for FRExt_chroma support
static const uint8_t subblk_offset_x[3][8][4] = {
  { {  0,  4,  0,  4 },
    {  0,  4,  0,  4 },
    {  0,  0,  0,  0 },
    {  0,  0,  0,  0 },
    {  0,  0,  0,  0 },
    {  0,  0,  0,  0 },
    {  0,  0,  0,  0 },
    {  0,  0,  0,  0 } },
  { {  0,  4,  0,  4 },
    {  0,  4,  0,  4 },
    {  0,  4,  0,  4 },
    {  0,  4,  0,  4 },
    {  0,  0,  0,  0 },
    {  0,  0,  0,  0 },
    {  0,  0,  0,  0 },
    {  0,  0,  0,  0 } },
  { {  0,  4,  0,  4 },
    {  8, 12,  8, 12 },
    {  0,  4,  0,  4 },
    {  8, 12,  8, 12 },
    {  0,  4,  0,  4 },
    {  8, 12,  8, 12 },
    {  0,  4,  0,  4 },
    {  8, 12,  8, 12 } }
};

static const uint8_t subblk_offset_y[3][8][4] = {
  { {  0,  0,  4,  4 },
    {  0,  0,  4,  4 },
    {  0,  0,  0,  0 },
    {  0,  0,  0,  0 },
    {  0,  0,  0,  0 },
    {  0,  0,  0,  0 },
    {  0,  0,  0,  0 },
    {  0,  0,  0,  0 } },
  { {  0,  0,  4,  4 },
    {  8,  8, 12, 12 },
    {  0,  0,  4,  4 },
    {  8,  8, 12, 12 },
    {  0,  0,  0,  0 },
    {  0,  0,  0,  0 },
    {  0,  0,  0,  0 },
    {  0,  0,  0,  0 } },
  { {  0,  0,  4,  4 },
    {  0,  0,  4,  4 },
    {  8,  8, 12, 12 },
    {  8,  8, 12, 12 },
    {  0,  0,  4,  4 },
    {  0,  0,  4,  4 },
    {  8,  8, 12, 12 },
    {  8,  8, 12, 12 } }
};


// static function declarations
static int concealByCopy(frame *recfr, int currMBNum, objectBuffer_t *object_list, int picSizeX);
static int concealByTrial(frame *recfr, imgpel *predMB,
                          int currMBNum, objectBuffer_t *object_list, int predBlocks[],
                          int picSizeX, int picSizeY, char *yCondition);
static int edgeDistortion (int predBlocks[], int currYBlockNum, imgpel *predMB,
                           imgpel *recY, int picSizeX, int regionSize);
static void copyBetweenFrames (frame *recfr, int currYBlockNum, int picSizeX, int regionSize);
static void buildPredRegionYUV(VideoParameters *p_Vid, int *mv, int x, int y, imgpel *predMB);

// picture error concealment
static void buildPredblockRegionYUV(VideoParameters *p_Vid, int *mv,
                                    int x, int y, imgpel *predMB, int list, int mb);
static void CopyImgData(imgpel **inputY, imgpel ***inputUV, imgpel **outputY, imgpel ***outputUV, 
                        int img_width, int img_height, int img_width_cr, int img_height_cr);

static void copyPredMB (int currYBlockNum, imgpel *predMB, frame *recfr,
                        int picSizeX, int regionSize);
static void add_node   ( VideoParameters *p_Vid, struct concealment_node *ptr );
static void delete_node( VideoParameters *p_Vid, struct concealment_node *ptr );

static const int uv_div[2][4] = {{0, 1, 1, 0}, {0, 1, 0, 0}}; //[x/y][yuv_format]

/*!
 ************************************************************************
 * \brief
 *      The main function for Inter (P) frame concealment.
 * \return
 *      0, if the concealment was not successful and simple concealment should be used
 *      1, otherwise (even if none of the blocks were concealed)
 * \param recfr
 *      Reconstructed frame buffer
 * \param object_list
 *      Motion info for all MBs in the frame
 * \param picSizeX
 *      Width of the frame in pixels
 * \param picSizeY
 *      Height of the frame in pixels
 * \param errorVar
 *      Variables for error concealment
 * \param chroma_format_idc
 *      Chroma format IDC
 ************************************************************************
 */
int ercConcealInterFrame(frame *recfr, objectBuffer_t *object_list,
                         int picSizeX, int picSizeY, ercVariables_t *errorVar, int chroma_format_idc )
{
  VideoParameters *p_Vid = recfr->p_Vid;
  sps_t *sps = p_Vid->active_sps;
  int lastColumn = 0, lastRow = 0, predBlocks[8];
  int lastCorruptedRow = -1, firstCorruptedRow = -1;
  int currRow = 0, row, column, columnInd, areaHeight = 0, i = 0;
  imgpel *predMB;

  /* if concealment is on */
  if ( errorVar && errorVar->concealment )
  {
    /* if there are segments to be concealed */
    if ( errorVar->nOfCorruptedSegments )
    {
      if (chroma_format_idc != YUV400)
        predMB = new imgpel[256 + sps->MbWidthC * sps->MbHeightC * 2];
      else
        predMB = new imgpel[256];

      lastRow = (int) (picSizeY>>4);
      lastColumn = (int) (picSizeX>>4);

      for ( columnInd = 0; columnInd < lastColumn; columnInd ++)
      {

        column = ((columnInd%2) ? (lastColumn - columnInd/2 -1) : (columnInd/2));

        for ( row = 0; row < lastRow; row++)
        {

          if ( errorVar->yCondition[MBxy2YBlock(column, row, 0, picSizeX)] <= ERC_BLOCK_CORRUPTED )
          {                           // ERC_BLOCK_CORRUPTED (1) or ERC_BLOCK_EMPTY (0)
            firstCorruptedRow = row;
            /* find the last row which has corrupted blocks (in same continuous area) */
            for ( lastCorruptedRow = row+1; lastCorruptedRow < lastRow; lastCorruptedRow++)
            {
              /* check blocks in the current column */
              if (errorVar->yCondition[MBxy2YBlock(column, lastCorruptedRow, 0, picSizeX)] > ERC_BLOCK_CORRUPTED)
              {
                /* current one is already OK, so the last was the previous one */
                lastCorruptedRow --;
                break;
              }
            }
            if ( lastCorruptedRow >= lastRow )
            {
              /* correct only from above */
              lastCorruptedRow = lastRow-1;
              for ( currRow = firstCorruptedRow; currRow < lastRow; currRow++ )
              {

                ercCollect8PredBlocks (predBlocks, (currRow<<1), (column<<1),
                  errorVar->yCondition, (lastRow<<1), (lastColumn<<1), 2, 0);

                if(p_Vid->erc_mvperMB >= MVPERMB_THR)
                  concealByTrial(recfr, predMB,
                    currRow*lastColumn+column, object_list, predBlocks,
                    picSizeX, picSizeY,
                    errorVar->yCondition);
                else
                  concealByCopy(recfr, currRow*lastColumn+column,
                    object_list, picSizeX);

                ercMarkCurrMBConcealed (currRow*lastColumn+column, -1, picSizeX, errorVar);
              }
              row = lastRow;
            }
            else if ( firstCorruptedRow == 0 )
            {
              /* correct only from below */
              for ( currRow = lastCorruptedRow; currRow >= 0; currRow-- )
              {

                ercCollect8PredBlocks (predBlocks, (currRow<<1), (column<<1),
                  errorVar->yCondition, (lastRow<<1), (lastColumn<<1), 2, 0);

                if(p_Vid->erc_mvperMB >= MVPERMB_THR)
                  concealByTrial(recfr, predMB,
                    currRow*lastColumn+column, object_list, predBlocks,
                    picSizeX, picSizeY,
                    errorVar->yCondition);
                else
                  concealByCopy(recfr, currRow*lastColumn+column,
                    object_list, picSizeX);

                ercMarkCurrMBConcealed (currRow*lastColumn+column, -1, picSizeX, errorVar);
              }

              row = lastCorruptedRow+1;
            }
            else
            {
              /* correct bi-directionally */

              row = lastCorruptedRow+1;

              areaHeight = lastCorruptedRow-firstCorruptedRow+1;

              /*
              *  Conceal the corrupted area switching between the up and the bottom rows
              */
              for ( i = 0; i < areaHeight; i++)
              {
                if ( i % 2 )
                {
                  currRow = lastCorruptedRow;
                  lastCorruptedRow --;
                }
                else
                {
                  currRow = firstCorruptedRow;
                  firstCorruptedRow ++;
                }

                ercCollect8PredBlocks (predBlocks, (currRow<<1), (column<<1),
                  errorVar->yCondition, (lastRow<<1), (lastColumn<<1), 2, 0);

                if(p_Vid->erc_mvperMB >= MVPERMB_THR)
                  concealByTrial(recfr, predMB,
                    currRow*lastColumn+column, object_list, predBlocks,
                    picSizeX, picSizeY,
                    errorVar->yCondition);
                else
                  concealByCopy(recfr, currRow*lastColumn+column,
                    object_list, picSizeX);

                ercMarkCurrMBConcealed (currRow*lastColumn+column, -1, picSizeX, errorVar);

              }
            }
            lastCorruptedRow = -1;
            firstCorruptedRow = -1;
          }
        }
      }

      delete []predMB;
    }
    return 1;
  }
  else
    return 0;
}

/*!
 ************************************************************************
 * \brief
 *      It conceals a given MB by simply copying the pixel area from the reference image
 *      that is at the same location as the macroblock in the current image. This correcponds
 *      to COPY MBs.
 * \return
 *      Always zero (0).
 * \param recfr
 *      Reconstructed frame buffer
 * \param currMBNum
 *      current MB index
 * \param object_list
 *      Motion info for all MBs in the frame
 * \param picSizeX
 *      Width of the frame in pixels
 ************************************************************************
 */
static int concealByCopy(frame *recfr, int currMBNum,
                         objectBuffer_t *object_list, int picSizeX)
{
  objectBuffer_t *currRegion;

  currRegion = object_list+(currMBNum<<2);
  currRegion->regionMode = REGMODE_INTER_COPY;

  currRegion->xMin = (xPosMB(currMBNum,picSizeX)<<4);
  currRegion->yMin = (yPosMB(currMBNum,picSizeX)<<4);

  copyBetweenFrames (recfr, MBNum2YBlock(currMBNum,0,picSizeX), picSizeX, 16);

  return 0;
}

/*!
 ************************************************************************
 * \brief
 *      Copies the co-located pixel values from the reference to the current frame.
 *      Used by concealByCopy
 * \param recfr
 *      Reconstructed frame buffer
 * \param currYBlockNum
 *      index of the block (8x8) in the Y plane
 * \param picSizeX
 *      Width of the frame in pixels
 * \param regionSize
 *      can be 16 or 8 to tell the dimension of the region to copy
 ************************************************************************
 */
static void copyBetweenFrames (frame *recfr, int currYBlockNum, int picSizeX, int regionSize)
{
  VideoParameters *p_Vid = recfr->p_Vid;
  sps_t* sps = p_Vid->active_sps;
  int j, k, location, xmin, ymin;
  storable_picture* refPic = p_Vid->ppSliceList[0]->RefPicList[0][0];

  /* set the position of the region to be copied */
  xmin = (xPosYBlock(currYBlockNum,picSizeX)<<3);
  ymin = (yPosYBlock(currYBlockNum,picSizeX)<<3);

  for (j = ymin; j < ymin + regionSize; j++)
    for (k = xmin; k < xmin + regionSize; k++)
    {
      location = j * picSizeX + k;
      recfr->yptr[location] = refPic->imgY[j][k];
    }

    for (j = ymin >> uv_div[1][sps->chroma_format_idc]; j < (ymin + regionSize) >> uv_div[1][sps->chroma_format_idc]; j++)
      for (k = xmin >> uv_div[0][sps->chroma_format_idc]; k < (xmin + regionSize) >> uv_div[0][sps->chroma_format_idc]; k++)
      {
        location = ((j * picSizeX) >> uv_div[0][sps->chroma_format_idc]) + k;

        recfr->uptr[location] = refPic->imgUV[0][j][k];
        recfr->vptr[location] = refPic->imgUV[1][j][k];
      }
}

/*!
 ************************************************************************
 * \brief
 *      It conceals a given MB by using the motion vectors of one reliable neighbor. That MV of a
 *      neighbor is selected wich gives the lowest pixel difference at the edges of the MB
 *      (see function edgeDistortion). This corresponds to a spatial smoothness criteria.
 * \return
 *      Always zero (0).
 * \param recfr
 *      Reconstructed frame buffer
 * \param predMB
 *      memory area for storing temporary pixel values for a macroblock
 *      the Y,U,V planes are concatenated y = predMB, u = predMB+256, v = predMB+320
 * \param currMBNum
 *      current MB index
 * \param object_list
 *      array of region structures storing region mode and mv for each region
 * \param predBlocks
 *      status array of the neighboring blocks (if they are OK, concealed or lost)
 * \param picSizeX
 *      Width of the frame in pixels
 * \param picSizeY
 *      Height of the frame in pixels
 * \param yCondition
 *      array for conditions of Y blocks from ercVariables_t
 ************************************************************************
 */
static int concealByTrial(frame *recfr, imgpel *predMB,
                          int currMBNum, objectBuffer_t *object_list, int predBlocks[],
                          int picSizeX, int picSizeY, char *yCondition)
{
  VideoParameters *p_Vid = recfr->p_Vid;

  int predMBNum = 0, numMBPerLine,
      compSplit1 = 0, compSplit2 = 0, compLeft = 1, comp = 0, compPred, order = 1,
      fInterNeighborExists, numIntraNeighbours,
      fZeroMotionChecked, predSplitted = 0,
      threshold = ERC_BLOCK_OK,
      minDist, currDist, i, k;
  int regionSize;
  objectBuffer_t *currRegion;
  int mvBest[3] = {0, 0, 0}, mvPred[3] = {0, 0, 0}, *mvptr;

  numMBPerLine = (int) (picSizeX>>4);

  comp = 0;
  regionSize = 16;

  do
  { /* 4 blocks loop */

    currRegion = object_list+(currMBNum<<2)+comp;

    /* set the position of the region to be concealed */

    currRegion->xMin = (xPosYBlock(MBNum2YBlock(currMBNum,comp,picSizeX),picSizeX)<<3);
    currRegion->yMin = (yPosYBlock(MBNum2YBlock(currMBNum,comp,picSizeX),picSizeX)<<3);

    do
    { /* reliability loop */

      minDist = 0;
      fInterNeighborExists = 0;
      numIntraNeighbours = 0;
      fZeroMotionChecked = 0;

      /* loop the 4 neighbours */
      for (i = 4; i < 8; i++)
      {

        /* if reliable, try it */
        if (predBlocks[i] >= threshold)
        {
          switch (i)
          {
          case 4:
            predMBNum = currMBNum-numMBPerLine;
            compSplit1 = 2;
            compSplit2 = 3;
            break;

          case 5:
            predMBNum = currMBNum-1;
            compSplit1 = 1;
            compSplit2 = 3;
            break;

          case 6:
            predMBNum = currMBNum+numMBPerLine;
            compSplit1 = 0;
            compSplit2 = 1;
            break;

          case 7:
            predMBNum = currMBNum+1;
            compSplit1 = 0;
            compSplit2 = 2;
            break;
          }

          /* try the concealment with the Motion Info of the current neighbour
          only try if the neighbour is not Intra */
          if (isBlock(object_list,predMBNum,compSplit1,INTRA) ||
            isBlock(object_list,predMBNum,compSplit2,INTRA))
          {
            numIntraNeighbours++;
          }
          else
          {
            /* if neighbour MB is splitted, try both neighbour blocks */
            for (predSplitted = isSplitted(object_list, predMBNum),
              compPred = compSplit1;
              predSplitted >= 0;
              compPred = compSplit2,
              predSplitted -= ((compSplit1 == compSplit2) ? 2 : 1))
            {

              /* if Zero Motion Block, do the copying. This option is tried only once */
              if (isBlock(object_list, predMBNum, compPred, INTER_COPY))
              {

                if (fZeroMotionChecked)
                {
                  continue;
                }
                else
                {
                  fZeroMotionChecked = 1;

                  mvPred[0] = mvPred[1] = 0;
                  mvPred[2] = 0;

                  buildPredRegionYUV(p_Vid->erc_img, mvPred, currRegion->xMin, currRegion->yMin, predMB);
                }
              }
              /* build motion using the neighbour's Motion Parameters */
              else if (isBlock(object_list,predMBNum,compPred,INTRA))
              {
                continue;
              }
              else
              {
                mvptr = getParam(object_list, predMBNum, compPred, mv);

                mvPred[0] = mvptr[0];
                mvPred[1] = mvptr[1];
                mvPred[2] = mvptr[2];

                buildPredRegionYUV(p_Vid->erc_img, mvPred, currRegion->xMin, currRegion->yMin, predMB);
              }

              /* measure absolute boundary pixel difference */
              currDist = edgeDistortion(predBlocks,
                MBNum2YBlock(currMBNum,comp,picSizeX),
                predMB, recfr->yptr, picSizeX, regionSize);

              /* if so far best -> store the pixels as the best concealment */
              if (currDist < minDist || !fInterNeighborExists)
              {

                minDist = currDist;

                for (k=0;k<3;k++)
                  mvBest[k] = mvPred[k];

                currRegion->regionMode =
                  (isBlock(object_list, predMBNum, compPred, INTER_COPY)) ?
                  ((regionSize == 16) ? REGMODE_INTER_COPY : REGMODE_INTER_COPY_8x8) :
                  ((regionSize == 16) ? REGMODE_INTER_PRED : REGMODE_INTER_PRED_8x8);

                copyPredMB(MBNum2YBlock(currMBNum,comp,picSizeX), predMB, recfr,
                  picSizeX, regionSize);
              }

              fInterNeighborExists = 1;
            }
          }
        }
    }

    threshold--;

    } while ((threshold >= ERC_BLOCK_CONCEALED) && (fInterNeighborExists == 0));

    /* always try zero motion */
    if (!fZeroMotionChecked)
    {
      mvPred[0] = mvPred[1] = 0;
      mvPred[2] = 0;

      buildPredRegionYUV(p_Vid->erc_img, mvPred, currRegion->xMin, currRegion->yMin, predMB);

      currDist = edgeDistortion(predBlocks,
        MBNum2YBlock(currMBNum,comp,picSizeX),
        predMB, recfr->yptr, picSizeX, regionSize);

      if (currDist < minDist || !fInterNeighborExists)
      {

        minDist = currDist;
        for (k=0;k<3;k++)
          mvBest[k] = mvPred[k];

        currRegion->regionMode =
          ((regionSize == 16) ? REGMODE_INTER_COPY : REGMODE_INTER_COPY_8x8);

        copyPredMB(MBNum2YBlock(currMBNum,comp,picSizeX), predMB, recfr,
          picSizeX, regionSize);
      }
    }

    for (i=0; i<3; i++)
      currRegion->mv[i] = mvBest[i];

    yCondition[MBNum2YBlock(currMBNum,comp,picSizeX)] = ERC_BLOCK_CONCEALED;
    comp = (comp+order+4)%4;
    compLeft--;

    } while (compLeft);

    return 0;
}

/*!
************************************************************************
* \brief
*      Builds the motion prediction pixels from the given location (in 1/4 pixel units)
*      of the reference frame. It not only copies the pixel values but builds the interpolation
*      when the pixel positions to be copied from is not full pixel (any 1/4 pixel position).
*      It copies the resulting pixel vlaues into predMB.
* \param p_Vid
*      The pointer of video_par structure of current frame
* \param mv
*      The pointer of the predicted MV of the current (being concealed) MB
* \param x
*      The x-coordinate of the above-left corner pixel of the current MB
* \param y
*      The y-coordinate of the above-left corner pixel of the current MB
* \param predMB
*      memory area for storing temporary pixel values for a macroblock
*      the Y,U,V planes are concatenated y = predMB, u = predMB+256, v = predMB+320
************************************************************************
*/
static void buildPredRegionYUV(VideoParameters *p_Vid, int *mv, int x, int y, imgpel *predMB)
{
  int i=0, j=0, ii=0, jj=0,i1=0,j1=0,j4=0,i4=0;
  int uv;
  int vec1_x=0,vec1_y=0;
  int ioff,joff;
  imgpel *pMB = predMB;
  slice_t *currSlice;// = p_Vid->currentSlice;
  storable_picture *dec_picture = p_Vid->dec_picture;
  int ii0,jj0,ii1,jj1,if1,jf1,if0,jf0;
  int mv_mul;

  //FRExt
  int f1_x, f1_y, f2_x, f2_y, f3, f4;
  int b8, b4;

  sps_t *sps = p_Vid->active_sps;
  int yuv = sps->chroma_format_idc - 1;
  int ref_frame = max (mv[2], 0); // !!KS: quick fix, we sometimes seem to get negative ref_pic here, so restrict to zero and above
  int mb_nr = y/16*(sps->PicWidthInMbs)+x/16;
  
  mb_t *currMB = &p_Vid->mb_data[mb_nr];   // intialization code deleted, see below, StW  
  currSlice = currMB->p_Slice;

    // This should be allocated only once. 
    imgpel tmp_block[16][16];

  /* Update coordinates of the current concealed macroblock */
  currMB->mb.x = (short) (x/16);
  currMB->mb.y = (short) (y/16);

  mv_mul=4;

    int num_uv_blocks;
    if (sps->chroma_format_idc != YUV400)
        num_uv_blocks = (((1 << sps->chroma_format_idc) & (~0x1)) >> 1);
    else
        num_uv_blocks = 0;

  // luma *******************************************************

  for(j=0;j<16/4;j++)
  {
    joff=j*4;
    j4=currMB->mb.y*4+j;
    for(i=0;i<16/4;i++)
    {
      ioff=i*4;
      i4=currMB->mb.x*4+i;

      vec1_x = i4*4*mv_mul + mv[0];
      vec1_y = j4*4*mv_mul + mv[1];

      currSlice->decoder.get_block_luma(currSlice->RefPicList[0][ref_frame], vec1_x, vec1_y, 4, 4,
        tmp_block,
        dec_picture->iLumaStride,dec_picture->size_x - 1,
        (currMB->mb_field_decoding_flag) ? (dec_picture->size_y >> 1) - 1 : dec_picture->size_y - 1, PLANE_Y, currMB);

      for(ii=0;ii<4;ii++)
        for(jj=0;jj<16/4;jj++)
          currSlice->mb_pred[PLANE_Y][jj+joff][ii+ioff]=tmp_block[jj][ii];
    }
  }


  for (j = 0; j < 16; j++)
  {
    for (i = 0; i < 16; i++)
    {
      pMB[j*16+i] = currSlice->mb_pred[PLANE_Y][j][i];
    }
  }
  pMB += 256;

  if (sps->chroma_format_idc != YUV400)
  {
    // chroma *******************************************************
    f1_x = 64/sps->MbWidthC;
    f2_x=f1_x-1;

    f1_y = 64/sps->MbHeightC;
    f2_y=f1_y-1;

    f3=f1_x*f1_y;
    f4=f3>>1;

    for(uv=0;uv<2;uv++)
    {
      for (b8=0;b8<num_uv_blocks;b8++)
      {
        for(b4=0;b4<4;b4++)
        {
          joff = subblk_offset_y[yuv][b8][b4];
          j4   = currMB->mb.y * sps->MbHeightC + joff;
          ioff = subblk_offset_x[yuv][b8][b4];
          i4   = currMB->mb.x * sps->MbWidthC + ioff;

          for(jj=0;jj<4;jj++)
          {
            for(ii=0;ii<4;ii++)
            {
              i1=(i4+ii)*f1_x + mv[0];
              j1=(j4+jj)*f1_y + mv[1];

              ii0=clip3 (0, dec_picture->size_x_cr-1, i1/f1_x);
              jj0=clip3 (0, dec_picture->size_y_cr-1, j1/f1_y);
              ii1=clip3 (0, dec_picture->size_x_cr-1, ((i1+f2_x)/f1_x));
              jj1=clip3 (0, dec_picture->size_y_cr-1, ((j1+f2_y)/f1_y));

              if1=(i1 & f2_x);
              jf1=(j1 & f2_y);
              if0=f1_x-if1;
              jf0=f1_y-jf1;

              currSlice->mb_pred[uv + 1][jj+joff][ii+ioff] = (imgpel) 
                ((if0*jf0*currSlice->RefPicList[0][ref_frame]->imgUV[uv][jj0][ii0]+
                  if1*jf0*currSlice->RefPicList[0][ref_frame]->imgUV[uv][jj0][ii1]+
                  if0*jf1*currSlice->RefPicList[0][ref_frame]->imgUV[uv][jj1][ii0]+
                  if1*jf1*currSlice->RefPicList[0][ref_frame]->imgUV[uv][jj1][ii1]+f4)/f3);
            }
          }
        }
      }

      for (j = 0; j < 8; j++)
      {
        for (i = 0; i < 8; i++)
        {
          pMB[j*8+i] = currSlice->mb_pred[uv + 1][j][i];
        }
      }
      pMB += 64;

    }
  }
}
/*!
 ************************************************************************
 * \brief
 *      Copies pixel values between a YUV frame and the temporary pixel value storage place. This is
 *      used to save some pixel values temporarily before overwriting it, or to copy back to a given
 *      location in a frame the saved pixel values.
 * \param currYBlockNum
 *      index of the block (8x8) in the Y plane
 * \param predMB
 *      memory area where the temporary pixel values are stored
 *      the Y,U,V planes are concatenated y = predMB, u = predMB+256, v = predMB+320
 * \param recfr
 *      pointer to a YUV frame
 * \param picSizeX
 *      picture width in pixels
 * \param regionSize
 *      can be 16 or 8 to tell the dimension of the region to copy
 ************************************************************************
 */
static void copyPredMB (int currYBlockNum, imgpel *predMB, frame *recfr,
                        int picSizeX, int regionSize)
{
  VideoParameters *p_Vid = recfr->p_Vid;
  sps_t *sps = p_Vid->active_sps;
  storable_picture *dec_picture = p_Vid->dec_picture;
  int j, k, xmin, ymin, xmax, ymax;
  int locationTmp;
  int uv_x = uv_div[0][sps->chroma_format_idc];
  int uv_y = uv_div[1][sps->chroma_format_idc];

  xmin = (xPosYBlock(currYBlockNum,picSizeX)<<3);
  ymin = (yPosYBlock(currYBlockNum,picSizeX)<<3);
  xmax = xmin + regionSize -1;
  ymax = ymin + regionSize -1;

  for (j = ymin; j <= ymax; j++)
  {
    for (k = xmin; k <= xmax; k++)
    {
      locationTmp = (j-ymin) * 16 + (k-xmin);
      dec_picture->imgY[j][k] = predMB[locationTmp];
    }
  }

  if (sps->chroma_format_idc != YUV400)
  {
    for (j = (ymin>>uv_y); j <= (ymax>>uv_y); j++)
    {
      for (k = (xmin>>uv_x); k <= (xmax>>uv_x); k++)
      {
        locationTmp = (j-(ymin>>uv_y)) * sps->MbWidthC + (k-(xmin>>1)) + 256;
        dec_picture->imgUV[0][j][k] = predMB[locationTmp];

        locationTmp += 64;

        dec_picture->imgUV[1][j][k] = predMB[locationTmp];
      }
    }
  }
}

/*!
 ************************************************************************
 * \brief
 *      Calculates a weighted pixel difference between edge Y pixels of the macroblock stored in predMB
 *      and the pixels in the given Y plane of a frame (recY) that would become neighbor pixels if
 *      predMB was placed at currYBlockNum block position into the frame. This "edge distortion" value
 *      is used to determine how well the given macroblock in predMB would fit into the frame when
 *      considering spatial smoothness. If there are correctly received neighbor blocks (status stored
 *      in predBlocks) only they are used in calculating the edge distorion; otherwise also the already
 *      concealed neighbor blocks can also be used.
 * \return
 *      The calculated weighted pixel difference at the edges of the MB.
 * \param predBlocks
 *      status array of the neighboring blocks (if they are OK, concealed or lost)
 * \param currYBlockNum
 *      index of the block (8x8) in the Y plane
 * \param predMB
 *      memory area where the temporary pixel values are stored
 *      the Y,U,V planes are concatenated y = predMB, u = predMB+256, v = predMB+320
 * \param recY
 *      pointer to a Y plane of a YUV frame
 * \param picSizeX
 *      picture width in pixels
 * \param regionSize
 *      can be 16 or 8 to tell the dimension of the region to copy
 ************************************************************************
 */
static int edgeDistortion (int predBlocks[], int currYBlockNum, imgpel *predMB,
                           imgpel *recY, int picSizeX, int regionSize)
{
  int i, j, distortion, numOfPredBlocks, threshold = ERC_BLOCK_OK;
  imgpel *currBlock = NULL, *neighbor = NULL;
  int currBlockOffset = 0;

  currBlock = recY + (yPosYBlock(currYBlockNum,picSizeX)<<3)*picSizeX + (xPosYBlock(currYBlockNum,picSizeX)<<3);

  do
  {

    distortion = 0; numOfPredBlocks = 0;

    // loop the 4 neighbors
    for (j = 4; j < 8; j++)
    {
      /* if reliable, count boundary pixel difference */
      if (predBlocks[j] >= threshold)
      {

        switch (j)
        {
        case 4:
          neighbor = currBlock - picSizeX;
          for ( i = 0; i < regionSize; i++ )
          {
            distortion += abs((int)(predMB[i] - neighbor[i]));
          }
          break;
        case 5:
          neighbor = currBlock - 1;
          for ( i = 0; i < regionSize; i++ )
          {
            distortion += abs((int)(predMB[i*16] - neighbor[i*picSizeX]));
          }
          break;
        case 6:
          neighbor = currBlock + regionSize*picSizeX;
          currBlockOffset = (regionSize-1)*16;
          for ( i = 0; i < regionSize; i++ )
          {
            distortion += abs((int)(predMB[i+currBlockOffset] - neighbor[i]));
          }
          break;
        case 7:
          neighbor = currBlock + regionSize;
          currBlockOffset = regionSize-1;
          for ( i = 0; i < regionSize; i++ )
          {
            distortion += abs((int)(predMB[i*16+currBlockOffset] - neighbor[i*picSizeX]));
          }
          break;
        }

        numOfPredBlocks++;
      }
    }

    threshold--;
    if (threshold < ERC_BLOCK_CONCEALED)
      break;
  } while (numOfPredBlocks == 0);

  if(numOfPredBlocks == 0)
  {
    return 0;
    // assert (numOfPredBlocks != 0); !!!KS hmm, trying to continue...
  }
  return (distortion/numOfPredBlocks);
}

// picture error concealment below

/*!
************************************************************************
* \brief
* The motion prediction pixels are calculated from the given location (in
* 1/4 pixel units) of the referenced frame. It copies the sub block from the
* corresponding reference to the frame to be concealed.
*
*************************************************************************
*/
static void buildPredblockRegionYUV(VideoParameters *p_Vid, int *mv,
                                    int x, int y, imgpel *predMB, int list, int current_mb_nr)
{
  int i=0,j=0,ii=0,jj=0,i1=0,j1=0,j4=0,i4=0;
  int uv;
  int vec1_x=0,vec1_y=0;
  int ioff,joff;

  storable_picture *dec_picture = p_Vid->dec_picture;
  imgpel *pMB = predMB;

  int ii0,jj0,ii1,jj1,if1,jf1,if0,jf0;
  int mv_mul;

  //FRExt
  int f1_x, f1_y, f2_x, f2_y, f3, f4;

  int ref_frame = mv[2];
  int mb_nr = current_mb_nr;
  
  mb_t *currMB = &p_Vid->mb_data[mb_nr];   // intialization code deleted, see below, StW  
  slice_t *currSlice = currMB->p_Slice;
    sps_t *sps = currSlice->active_sps;
  int yuv = sps->chroma_format_idc - 1;

    imgpel tmp_block[16][16];

  /* Update coordinates of the current concealed macroblock */

  currMB->mb.x = (short) (x/4);
  currMB->mb.y = (short) (y/4);

  mv_mul=4;

  // luma *******************************************************

  vec1_x = x*mv_mul + mv[0];
  vec1_y = y*mv_mul + mv[1];
  currSlice->decoder.get_block_luma(currSlice->RefPicList[list][ref_frame],  vec1_x, vec1_y, 4, 4, tmp_block,
    dec_picture->iLumaStride,dec_picture->size_x - 1,
    (currMB->mb_field_decoding_flag) ? (dec_picture->size_y >> 1) - 1 : dec_picture->size_y - 1, PLANE_Y, currMB);

  for(jj=0;jj<16/4;jj++)
    for(ii=0;ii<4;ii++)
      currSlice->mb_pred[PLANE_Y][jj][ii]=tmp_block[jj][ii];


  for (j = 0; j < 4; j++)
  {
    for (i = 0; i < 4; i++)
    {
      pMB[j*4+i] = currSlice->mb_pred[PLANE_Y][j][i];
    }
  }
  pMB += 16;

  if (sps->chroma_format_idc != YUV400)
  {
    // chroma *******************************************************
    f1_x = 64/sps->MbWidthC;
    f2_x=f1_x-1;

    f1_y = 64/sps->MbHeightC;
    f2_y=f1_y-1;

    f3=f1_x*f1_y;
    f4=f3>>1;

    for(uv=0;uv<2;uv++)
    {
      joff = subblk_offset_y[yuv][0][0];
      j4   = currMB->mb.y * sps->MbHeightC/4 + joff;
      ioff = subblk_offset_x[yuv][0][0];
      i4   = currMB->mb.x * sps->MbWidthC/4 + ioff;

      for(jj=0;jj<2;jj++)
      {
        for(ii=0;ii<2;ii++)
        {
          i1=(i4+ii)*f1_x + mv[0];
          j1=(j4+jj)*f1_y + mv[1];

          ii0=clip3 (0, dec_picture->size_x_cr-1, i1/f1_x);
          jj0=clip3 (0, dec_picture->size_y_cr-1, j1/f1_y);
          ii1=clip3 (0, dec_picture->size_x_cr-1, ((i1+f2_x)/f1_x));
          jj1=clip3 (0, dec_picture->size_y_cr-1, ((j1+f2_y)/f1_y));

          if1=(i1 & f2_x);
          jf1=(j1 & f2_y);
          if0=f1_x-if1;
          jf0=f1_y-jf1;

          currSlice->mb_pred[uv + 1][jj][ii]=(imgpel) ((if0*jf0*currSlice->RefPicList[list][ref_frame]->imgUV[uv][jj0][ii0]+
                                                        if1*jf0*currSlice->RefPicList[list][ref_frame]->imgUV[uv][jj0][ii1]+
                                                        if0*jf1*currSlice->RefPicList[list][ref_frame]->imgUV[uv][jj1][ii0]+
                                                        if1*jf1*currSlice->RefPicList[list][ref_frame]->imgUV[uv][jj1][ii1]+f4)/f3);
        }
      }

      for (j = 0; j < 2; j++)
      {
        for (i = 0; i < 2; i++)
        {
          pMB[j*2+i] = currSlice->mb_pred[uv + 1][j][i];
        }
      }
      pMB += 4;

    }
  }
}

/*!
************************************************************************
* \brief
*    Copy image data from one array to another array
************************************************************************
*/

static void CopyImgData(imgpel **inputY, imgpel ***inputUV, imgpel **outputY, imgpel ***outputUV, 
                        int img_width, int img_height, int img_width_cr, int img_height_cr)
{
  int x, y;

  for (y=0; y<img_height; y++)
    for (x=0; x<img_width; x++)
      outputY[y][x] = inputY[y][x];

  for (y=0; y<img_height_cr; y++)
    for (x=0; x<img_width_cr; x++)
    {
      outputUV[0][y][x] = inputUV[0][y][x];
      outputUV[1][y][x] = inputUV[1][y][x];
    }
}

/*!
************************************************************************
* \brief
*    Copies the last reference frame for concealing reference frame loss.
************************************************************************
*/

static storable_picture* get_last_ref_pic_from_dpb(dpb_t *p_Dpb)
{
  int used_size = p_Dpb->used_size - 1;
  int i;

  for(i = used_size; i >= 0; i--)
  {
    if (p_Dpb->fs[i]->is_used==3)
    {
      if (((p_Dpb->fs[i]->frame->used_for_reference) &&
        (!p_Dpb->fs[i]->frame->is_long_term)) /*||  ((p_Dpb->fs[i]->frame->used_for_reference==0)
                                           && (p_Dpb->fs[i]->frame->slice_type == P_slice))*/ )
      {
        return p_Dpb->fs[i]->frame;
      }
    }
  }

  return NULL;
}

/*!
************************************************************************
* \brief
* Conceals the lost reference or non reference frame by either frame copy
* or motion vector copy concealment.
*
************************************************************************
*/

static void copy_to_conceal(storable_picture *src, storable_picture *dst, VideoParameters *p_Vid)
{
    int i = 0;
    int mv[3];
    int multiplier;
    imgpel *predMB, *storeYUV;
    int j, y, x, mb_height, mb_width, ii = 0, jj = 0;
    int uv;
    int mm, nn;
    int scale = 1;
    storable_picture *dec_picture = p_Vid->dec_picture;
    sps_t *sps = p_Vid->active_sps;

    int current_mb_nr = 0;

    dst->PicSizeInMbs  = src->PicSizeInMbs;

    dst->slice.slice_type = src->slice.slice_type = p_Vid->conceal_slice_type;

    dst->slice.idr_flag = 0; //since we do not want to clears the ref list

    dst->slice.no_output_of_prior_pics_flag    = src->slice.no_output_of_prior_pics_flag;
    dst->slice.long_term_reference_flag        = src->slice.long_term_reference_flag;
    dst->slice.adaptive_ref_pic_buffering_flag = src->slice.adaptive_ref_pic_buffering_flag = 0;

    dec_picture = src;

    // Conceals the missing frame by frame copy concealment
    if (p_Vid->conceal_mode == 1) {
        // We need these initializations for using deblocking filter for frame copy
        // concealment as well.
        dst->PicWidthInMbs = src->PicWidthInMbs;
        dst->PicSizeInMbs = src->PicSizeInMbs;

        CopyImgData(src->imgY, src->imgUV, dst->imgY, dst->imgUV,
                    sps->PicWidthInMbs * 16, sps->FrameHeightInMbs * 16,
                    sps->PicWidthInMbs * sps->MbWidthC, sps->FrameHeightInMbs * sps->MbHeightC);
    }

    // Conceals the missing frame by motion vector copy concealment
    if (p_Vid->conceal_mode == 2) {
        if (sps->chroma_format_idc != YUV400)
            storeYUV = (imgpel *) malloc ((16 + sps->MbWidthC * sps->MbHeightC * 2 / 16) * sizeof (imgpel));
        else
            storeYUV = (imgpel *) malloc (16  * sizeof (imgpel));

        p_Vid->erc_img = p_Vid;

        dst->PicWidthInMbs = src->PicWidthInMbs;
        dst->PicSizeInMbs = src->PicSizeInMbs;
        mb_width = sps->PicWidthInMbs;
        mb_height = sps->FrameHeightInMbs;
        scale = (p_Vid->conceal_slice_type == B_slice) ? 2 : 1;

        if (p_Vid->conceal_slice_type == B_slice) {
            init_lists_for_non_reference_loss(
                p_Vid->p_Dpb_layer[0],
                dst->slice.slice_type, p_Vid->ppSliceList[0]->field_pic_flag);
        } else
            p_Vid->ppSliceList[0]->init_lists();

        multiplier = 4;

        for (i = 0; i < mb_height * 4; i++) {
            mm = i * 4;
            for (j = 0; j < mb_width * 4; j++) {
                nn = j * 4;

                mv[0] = src->mv_info[i][j].mv[LIST_0].mv_x / scale;
                mv[1] = src->mv_info[i][j].mv[LIST_0].mv_y / scale;
                mv[2] = src->mv_info[i][j].ref_idx[LIST_0];

                if (mv[2] < 0)
                    mv[2] = 0;

                dst->mv_info[i][j].mv[LIST_0].mv_x = (short) mv[0];
                dst->mv_info[i][j].mv[LIST_0].mv_y = (short) mv[1];
                dst->mv_info[i][j].ref_idx[LIST_0] = (char) mv[2];

                x = j * multiplier;
                y = i * multiplier;

                if (mm % 16 == 0 && nn % 16 == 0)
                    current_mb_nr++;

                buildPredblockRegionYUV(p_Vid->erc_img, mv, x, y, storeYUV, LIST_0, current_mb_nr);

                predMB = storeYUV;

                for (ii = 0; ii < multiplier; ii++) {
                    for (jj = 0; jj < multiplier; jj++)
                        dst->imgY[i * multiplier + ii][j * multiplier + jj] = predMB[ii * multiplier + jj];
                }

                predMB = predMB + multiplier * multiplier;

                if (sps->chroma_format_idc != YUV400) {
                    for (uv = 0; uv < 2; uv++) {
                        for (ii = 0; ii < multiplier / 2; ii++) {
                            for (jj = 0; jj < multiplier / 2; jj++)
                                dst->imgUV[uv][i*multiplier/2 +ii][j*multiplier/2 +jj] = predMB[ii*(multiplier/2)+jj];
                        }
                        predMB = predMB + (multiplier*multiplier/4);
                    }
                }
            }
        }
        free(storeYUV);
    }
}

/*!
************************************************************************
* \brief
* Uses the previous reference pic for concealment of reference frames
*
************************************************************************
*/

static void
copy_prev_pic_to_concealed_pic(storable_picture *picture, dpb_t *p_Dpb)
{
  VideoParameters *p_Vid = p_Dpb->p_Vid;
  /* get the last ref pic in dpb */
  storable_picture *ref_pic = get_last_ref_pic_from_dpb(p_Dpb);

  assert(ref_pic != NULL);

  /* copy all the struc from this to current concealment pic */
  p_Vid->conceal_slice_type = P_slice;
  copy_to_conceal(ref_pic, picture, p_Vid);
}


/*!
************************************************************************
* \brief
* This function conceals a missing reference frame. The routine is called
* based on the difference in frame number. It conceals an IDR frame loss
* based on the sudden decrease in frame number.
*
************************************************************************
*/

void conceal_lost_frames(dpb_t *p_Dpb, slice_t *pSlice)
{
  VideoParameters *p_Vid = p_Dpb->p_Vid;
  sps_t *sps = p_Vid->active_sps;
  int CurrFrameNum;
  int UnusedShortTermFrameNum;
  storable_picture *picture = NULL;
  int tmp1 = pSlice->delta_pic_order_cnt[0];
  int tmp2 = pSlice->delta_pic_order_cnt[1];
  int i;

  pSlice->delta_pic_order_cnt[0] = pSlice->delta_pic_order_cnt[1] = 0;

  // printf("A gap in frame number is found, try to fill it.\n");

  if(p_Vid->IDR_concealment_flag == 1)
  {
    // Conceals an IDR frame loss. Uses the reference frame in the previous
    // GOP for concealment.
    UnusedShortTermFrameNum = 0;
    p_Vid->last_ref_pic_poc = -p_Vid->p_Inp->poc_gap;
    p_Vid->earlier_missing_poc = 0;
  }
  else
    UnusedShortTermFrameNum = (p_Vid->pre_frame_num + 1) % p_Vid->active_sps->MaxFrameNum;

  CurrFrameNum = pSlice->frame_num;

  while (CurrFrameNum != UnusedShortTermFrameNum)
  {
    picture = new storable_picture(p_Vid, FRAME,
        sps->PicWidthInMbs * 16, sps->FrameHeightInMbs * 16,
        sps->PicWidthInMbs * sps->MbWidthC, sps->FrameHeightInMbs * sps->MbHeightC, 1);

    picture->slice.coded_frame = 1;
    picture->PicNum = UnusedShortTermFrameNum;
    picture->frame_num = UnusedShortTermFrameNum;
    picture->non_existing = 0;
    picture->is_output = 0;
    picture->used_for_reference = 1;
    picture->concealed_pic = 1;

    picture->slice.adaptive_ref_pic_buffering_flag = 0;

    pSlice->frame_num = UnusedShortTermFrameNum;

    picture->top_poc=p_Vid->last_ref_pic_poc + p_Vid->p_Inp->ref_poc_gap;
    picture->bottom_poc=picture->top_poc;
    picture->frame_poc=picture->top_poc;
    picture->poc=picture->top_poc;
    p_Vid->last_ref_pic_poc = picture->poc;

    copy_prev_pic_to_concealed_pic(picture, p_Dpb);

    //if (UnusedShortTermFrameNum == 0)
    if(p_Vid->IDR_concealment_flag == 1)
    {
      picture->slice.slice_type = I_slice;
      picture->slice.idr_flag = 1;
      p_Dpb->flush();
      picture->top_poc= 0;
      picture->bottom_poc=picture->top_poc;
      picture->frame_poc=picture->top_poc;
      picture->poc=picture->top_poc;
      p_Vid->last_ref_pic_poc = picture->poc;
    }

    p_Vid->p_Dpb_layer[0]->store_picture(picture);

    picture=NULL;

    p_Vid->pre_frame_num = UnusedShortTermFrameNum;
    UnusedShortTermFrameNum = (UnusedShortTermFrameNum + 1) % p_Vid->active_sps->MaxFrameNum;

    // update reference flags and set current flag.
    for(i=16;i>0;i--)
    {
      pSlice->ref_flag[i] = pSlice->ref_flag[i-1];
    }
    pSlice->ref_flag[0] = 0;
  }
  pSlice->delta_pic_order_cnt[0] = tmp1;
  pSlice->delta_pic_order_cnt[1] = tmp2;
  pSlice->frame_num = CurrFrameNum;
}

/*!
************************************************************************
* \brief
* Updates the reference list for motion vector copy concealment for non-
* reference frame loss.
*
************************************************************************
*/

void update_ref_list_for_concealment(dpb_t *p_Dpb)
{
  VideoParameters *p_Vid = p_Dpb->p_Vid;
  unsigned i, j= 0;

  for (i = 0; i < p_Dpb->used_size; i++)
  {
    if (p_Dpb->fs[i]->concealment_reference)
    {
      p_Dpb->fs_ref[j++] = p_Dpb->fs[i];
    }
  }

  p_Dpb->ref_frames_in_buffer = p_Vid->active_pps->num_ref_idx_l0_default_active_minus1;
}

/*!
************************************************************************
* \brief
*    Initialize the list based on the B frame or non reference 'p' frame
*    to be concealed. The function initialize currSlice->listX[0] and list 1 depending
*    on current picture type
*
************************************************************************
*/
void init_lists_for_non_reference_loss(dpb_t *p_Dpb, int currSliceType, bool field_pic_flag)
{
    VideoParameters *p_Vid = p_Dpb->p_Vid;
    sps_t *active_sps = p_Vid->active_sps;

    unsigned i;
    int j;
    int diff;

    int list0idx = 0;
    int list0idx_1 = 0;

    storable_picture *tmp_s;

    if (!field_pic_flag) {
        for (i = 0; i < p_Dpb->ref_frames_in_buffer; i++) {
            if (p_Dpb->fs[i]->concealment_reference == 1) {
                if (p_Dpb->fs[i]->FrameNum > p_Vid->frame_to_conceal)
                    p_Dpb->fs_ref[i]->FrameNumWrap = p_Dpb->fs[i]->FrameNum - active_sps->MaxFrameNum;
                else
                    p_Dpb->fs_ref[i]->FrameNumWrap = p_Dpb->fs[i]->FrameNum;
                p_Dpb->fs_ref[i]->frame->PicNum = p_Dpb->fs_ref[i]->FrameNumWrap;
            }
        }
    }

    if (currSliceType == P_slice) {
    // Calculate FrameNumWrap and PicNum
        if (!field_pic_flag) {
            for (i = 0; i < p_Dpb->used_size; i++) {
                if (p_Dpb->fs[i]->concealment_reference == 1)
                    p_Vid->ppSliceList[0]->RefPicList[0][list0idx++] = p_Dpb->fs[i]->frame;
            }
            // order list 0 by PicNum
            std::qsort(p_Vid->ppSliceList[0]->RefPicList[0], list0idx, sizeof(storable_picture*), [](const void *arg1, const void *arg2) {
                int pic_num1 = (*(storable_picture**)arg1)->PicNum;
                int pic_num2 = (*(storable_picture**)arg2)->PicNum;
                return (pic_num1 < pic_num2) ? 1 : (pic_num1 > pic_num2) ? -1 : 0;
            });

            p_Vid->ppSliceList[0]->RefPicSize[0] = (char) list0idx;
        }
    }

    if (currSliceType == B_slice) {
        if (!field_pic_flag) {
            for (i = 0; i < p_Dpb->used_size; i++) {
                if (p_Dpb->fs[i]->concealment_reference == 1) {
                    if (p_Vid->earlier_missing_poc > p_Dpb->fs[i]->frame->poc)
                        p_Vid->ppSliceList[0]->RefPicList[0][list0idx++] = p_Dpb->fs[i]->frame;
                }
            }

            std::qsort(p_Vid->ppSliceList[0]->RefPicList[0], list0idx, sizeof(storable_picture*), [](const void *arg1, const void *arg2) {
                int poc1 = (*(storable_picture**)arg1)->poc;
                int poc2 = (*(storable_picture**)arg2)->poc;
                return (poc1 < poc2) ? 1 : (poc1 > poc2) ? -1 : 0;
            });

            list0idx_1 = list0idx;

            for (i = 0; i < p_Dpb->used_size; i++) {
                if (p_Dpb->fs[i]->concealment_reference == 1) {
                    if (p_Vid->earlier_missing_poc < p_Dpb->fs[i]->frame->poc)
                        p_Vid->ppSliceList[0]->RefPicList[0][list0idx++] = p_Dpb->fs[i]->frame;
                }
            }

            std::qsort(&p_Vid->ppSliceList[0]->RefPicList[0][list0idx_1], list0idx-list0idx_1,
                       sizeof(storable_picture*), [](const void *arg1, const void *arg2) {
                int poc1 = (*(storable_picture**)arg1)->poc;
                int poc2 = (*(storable_picture**)arg2)->poc;
                return (poc1 < poc2) ? -1 : (poc1 > poc2) ? 1 : 0;
            });

            for (j = 0; j < list0idx_1; j++)
                p_Vid->ppSliceList[0]->RefPicList[1][list0idx-list0idx_1+j]=p_Vid->ppSliceList[0]->RefPicList[0][j];
            for (j = list0idx_1; j < list0idx; j++)
                p_Vid->ppSliceList[0]->RefPicList[1][j-list0idx_1]=p_Vid->ppSliceList[0]->RefPicList[0][j];

            p_Vid->ppSliceList[0]->RefPicSize[0] = p_Vid->ppSliceList[0]->RefPicSize[1] = (char) list0idx;

            std::qsort(&p_Vid->ppSliceList[0]->RefPicList[0][(short) p_Vid->ppSliceList[0]->RefPicSize[0]], list0idx-p_Vid->ppSliceList[0]->RefPicSize[0],
                       sizeof(storable_picture*), [](const void *arg1, const void *arg2) {
                int long_term_pic_num1 = (*(storable_picture**)arg1)->LongTermPicNum;
                int long_term_pic_num2 = (*(storable_picture**)arg2)->LongTermPicNum;
                return (long_term_pic_num1 < long_term_pic_num2) ? -1 : (long_term_pic_num1 > long_term_pic_num2) ? 1 : 0;
            });
            std::qsort(&p_Vid->ppSliceList[0]->RefPicList[1][(short) p_Vid->ppSliceList[0]->RefPicSize[0]], list0idx-p_Vid->ppSliceList[0]->RefPicSize[0],
                       sizeof(storable_picture*), [](const void *arg1, const void *arg2) {
                int long_term_pic_num1 = (*(storable_picture**)arg1)->LongTermPicNum;
                int long_term_pic_num2 = (*(storable_picture**)arg2)->LongTermPicNum;
                return (long_term_pic_num1 < long_term_pic_num2) ? -1 : (long_term_pic_num1 > long_term_pic_num2) ? 1 : 0;
            });
            p_Vid->ppSliceList[0]->RefPicSize[0] = p_Vid->ppSliceList[0]->RefPicSize[1] = (char) list0idx;
        }
    }

    if ((p_Vid->ppSliceList[0]->RefPicSize[0] == p_Vid->ppSliceList[0]->RefPicSize[1]) &&
        (p_Vid->ppSliceList[0]->RefPicSize[0] > 1)) {
        // check if lists are identical, if yes swap first two elements of listX[1]
        diff = 0;
        for (j = 0; j < p_Vid->ppSliceList[0]->RefPicSize[0]; j++) {
            if (p_Vid->ppSliceList[0]->RefPicList[0][j]!=p_Vid->ppSliceList[0]->RefPicList[1][j])
                diff = 1;
        }
        if (!diff) {
            tmp_s = p_Vid->ppSliceList[0]->RefPicList[1][0];
            p_Vid->ppSliceList[0]->RefPicList[1][0]=p_Vid->ppSliceList[0]->RefPicList[1][1];
            p_Vid->ppSliceList[0]->RefPicList[1][1]=tmp_s;
        }
    }

    // set max size
    p_Vid->ppSliceList[0]->RefPicSize[0] = (char) min<int>(p_Vid->ppSliceList[0]->RefPicSize[0], (int)active_sps->max_num_ref_frames);
    p_Vid->ppSliceList[0]->RefPicSize[1] = 0;

    // set the unused list entries to NULL
    for (i = p_Vid->ppSliceList[0]->RefPicSize[0]; i < MAX_LIST_SIZE; i++)
        p_Vid->ppSliceList[0]->RefPicList[0][i] = NULL;
    for (i = p_Vid->ppSliceList[0]->RefPicSize[1]; i < MAX_LIST_SIZE; i++)
        p_Vid->ppSliceList[0]->RefPicList[1][i] = NULL;
}


/*!
************************************************************************
* \brief
* Get from the dpb the picture corresponding to a POC.  The POC varies
* depending on whether it is a frame copy or motion vector copy concealment.
* The frame corresponding to the POC is returned.
*
************************************************************************
*/

storable_picture *get_pic_from_dpb(dpb_t *p_Dpb, int missingpoc, unsigned int *pos)
{
  VideoParameters *p_Vid = p_Dpb->p_Vid;
  int used_size = p_Dpb->used_size - 1;
  int i, concealfrom = 0;

  if(p_Vid->conceal_mode == 1)
    concealfrom = missingpoc - p_Vid->p_Inp->poc_gap;
  else if (p_Vid->conceal_mode == 2)
    concealfrom = missingpoc + p_Vid->p_Inp->poc_gap;

  for(i = used_size; i >= 0; i--)
  {
    if(p_Dpb->fs[i]->poc == concealfrom)
    {
      *pos = i;
      return p_Dpb->fs[i]->frame;
    }
  }

  return NULL;
}

/*!
************************************************************************
* \brief
* Function to sort the POC and find the lowest number in the POC list
* Compare the integers
*
************************************************************************
*/

int comp(const void *i, const void *j)
{
  return *(int *)i - *(int *)j;
}

/*!
************************************************************************
* \brief
* Initialises a node, allocates memory for the node, and returns
* a pointer to the new node.
*
************************************************************************
*/

struct concealment_node * init_node( storable_picture* picture, int missingpoc )
{
  struct concealment_node *ptr;

  ptr = (struct concealment_node *) calloc( 1, sizeof(struct concealment_node ) );

  if( ptr == NULL )
    return (struct concealment_node *) NULL;
  else {
    ptr->picture = picture;
    ptr->missingpocs = missingpoc;
    ptr->next = NULL;
    return ptr;
  }
}

/*!
************************************************************************
* \brief
* Adds a node to the end of the list.
*
************************************************************************
*/


static void add_node( VideoParameters *p_Vid, struct concealment_node *concealment_new )
{
  if( p_Vid->concealment_head == NULL )
  {
    p_Vid->concealment_end = p_Vid->concealment_head = concealment_new;
    return;
  }
  p_Vid->concealment_end->next = concealment_new;
  p_Vid->concealment_end = concealment_new;
}


/*!
************************************************************************
* \brief
* Deletes the specified node pointed to by 'ptr' from the list
*
************************************************************************
*/


static void delete_node( VideoParameters *p_Vid, struct concealment_node *ptr )
{
  // We only need to delete the first node in the linked list
  if( ptr == p_Vid->concealment_head ) 
  {
    p_Vid->concealment_head = p_Vid->concealment_head->next;
    if( p_Vid->concealment_end == ptr )
      p_Vid->concealment_end = p_Vid->concealment_end->next;
    free(ptr);
  }
}

/*!
************************************************************************
* \brief
* Deletes all nodes from the place specified by ptr
*
************************************************************************
*/

void delete_list( VideoParameters *p_Vid, struct concealment_node *ptr )
{
  struct concealment_node *temp;

  if( p_Vid->concealment_head == NULL ) return;

  if( ptr == p_Vid->concealment_head ) 
  {
    p_Vid->concealment_head = NULL;
    p_Vid->concealment_end = NULL;
  }
  else
  {
    temp = p_Vid->concealment_head;

    while( temp->next != ptr )
      temp = temp->next;
    p_Vid->concealment_end = temp;
  }

  while( ptr != NULL ) 
  {
    temp = ptr->next;
    free( ptr );
    ptr = temp;
  }
}



void decoded_picture_buffer_t::conceal_non_ref_pics(int diff)
{
    VideoParameters *p_Vid = this->p_Vid;
    sps_t *sps = p_Vid->active_sps;
    int missingpoc = 0;
    unsigned int i, pos = 0;
    storable_picture* conceal_from_picture = NULL;
    storable_picture* conceal_to_picture = NULL;
    concealment_node* concealment_ptr = NULL;
    int temp_used_size = this->used_size;

    if (this->used_size == 0)
        return;

    qsort(p_Vid->pocs_in_dpb, this->size, sizeof(int), comp);

    for (i = 0; i < this->size-diff; i++) {
        this->used_size = this->size;
        if (p_Vid->pocs_in_dpb[i+1] - p_Vid->pocs_in_dpb[i] > p_Vid->p_Inp->poc_gap) {
            conceal_to_picture = new storable_picture(p_Vid, FRAME,
                sps->PicWidthInMbs * 16, sps->FrameHeightInMbs * 16,
                sps->PicWidthInMbs * sps->MbWidthC, sps->FrameHeightInMbs * sps->MbHeightC, 1);

            missingpoc = p_Vid->pocs_in_dpb[i] + p_Vid->p_Inp->poc_gap;

            if (missingpoc > p_Vid->earlier_missing_poc) {
                p_Vid->earlier_missing_poc  = missingpoc;
                conceal_to_picture->top_poc = missingpoc;
                conceal_to_picture->bottom_poc = missingpoc;
                conceal_to_picture->frame_poc = missingpoc;
                conceal_to_picture->poc = missingpoc;
                conceal_from_picture = get_pic_from_dpb(this, missingpoc, &pos);

                assert(conceal_from_picture != NULL);

                this->used_size = pos + 1;

                p_Vid->frame_to_conceal = conceal_from_picture->frame_num + 1;

                update_ref_list_for_concealment(this);
                p_Vid->conceal_slice_type = B_slice;
                copy_to_conceal(conceal_from_picture, conceal_to_picture, p_Vid);
                concealment_ptr = init_node( conceal_to_picture, missingpoc );
                add_node(p_Vid, concealment_ptr);
            }
        }
    }

    this->used_size = temp_used_size;
}


void decoded_picture_buffer_t::sliding_window_poc_management(storable_picture* p)
{
    if (this->used_size == this->size) {
        VideoParameters* p_Vid = this->p_Vid;
        for (int i = 0; i < this->size - 1; i++)
            p_Vid->pocs_in_dpb[i] = p_Vid->pocs_in_dpb[i+1];
    }
}

void decoded_picture_buffer_t::write_lost_non_ref_pic(int poc, int p_out)
{
    VideoParameters* p_Vid = this->p_Vid;
    frame_store concealment_fs;
    if (poc > 0) {
        if (poc - this->last_output_poc > p_Vid->p_Inp->poc_gap) {
            concealment_fs.frame = p_Vid->concealment_head->picture;
            concealment_fs.is_output = 0;
            concealment_fs.is_reference = 0;
            concealment_fs.is_used = 3;

            write_stored_frame(p_Vid, &concealment_fs, p_out);
            delete_node(p_Vid, p_Vid->concealment_head);
        }
    }
}

void decoded_picture_buffer_t::write_lost_ref_after_idr(int pos)
{
    VideoParameters *p_Vid = this->p_Vid;
    sps_t *sps = p_Vid->active_sps;

    int temp = 1;

    if (!p_Vid->last_out_fs->frame) {
        p_Vid->last_out_fs->frame = new storable_picture(p_Vid, FRAME,
            sps->PicWidthInMbs * 16, sps->FrameHeightInMbs * 16,
            sps->PicWidthInMbs * sps->MbWidthC, sps->FrameHeightInMbs * sps->MbHeightC, 1);
        p_Vid->last_out_fs->is_used = 3;
    }

    if (p_Vid->conceal_mode == 2) {
        temp = 2;
        p_Vid->conceal_mode = 1;
    }
    copy_to_conceal(this->fs[pos]->frame, p_Vid->last_out_fs->frame, p_Vid);

    p_Vid->conceal_mode = temp;
}
