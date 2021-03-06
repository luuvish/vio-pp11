#include "global.h"
#include "input_parameters.h"
#include "h264decoder.h"
#include "report.h"

#include "slice.h"
#include "interpret.h"
#include "bitstream_cabac.h"
#include "bitstream.h"
#include "sets.h"

#include "sei.h"
#include "output.h"
#include "memalloc.h"
#include "macroblock.h"
#include "neighbour.h"
#include "decoder.h"

using vio::h264::mb_t;

#include "erc_api.h"
#include "dpb.h"



static inline int is_BL_profile(unsigned int profile_idc) 
{
  return ( profile_idc == FREXT_CAVLC444 || profile_idc == BASELINE || profile_idc == MAIN || profile_idc == EXTENDED ||
           profile_idc == FREXT_HP || profile_idc == FREXT_Hi10P || profile_idc == FREXT_Hi422 || profile_idc == FREXT_Hi444);
}

static inline int is_EL_profile(unsigned int profile_idc) 
{
  return ( (profile_idc == MVC_HIGH) || (profile_idc == STEREO_HIGH)
           );
}

static inline int is_MVC_profile(unsigned int profile_idc)
{
  return ( (0)
#if (MVC_EXTENSION_ENABLE)
  || (profile_idc == MVC_HIGH) || (profile_idc == STEREO_HIGH)
#endif
  );
}


static inline float psnr(int max_sample_sq, int samples, float sse_distortion ) 
{
    return (float) (sse_distortion == 0.0 ? 0.0 : (10.0 * log10(max_sample_sq * (double) ((double) samples / sse_distortion))));
}

static inline void reset_mbs(mb_t *currMB)
{
    currMB->slice_nr = -1;
    currMB->ei_flag  =  1;
    currMB->dpl_flag =  0;
}

static void setup_buffers(VideoParameters* p_Vid, int layer_id)
{
    CodingParameters* cps = p_Vid->p_EncodePar[layer_id];

    if (p_Vid->last_dec_layer_id != layer_id) {
        if (p_Vid->active_sps->separate_colour_plane_flag) {
            for (int i = 0; i < 3; ++i)
                p_Vid->mb_data_JV[i] = cps->mb_data_JV[i];
            p_Vid->mb_data = NULL;
        } else
            p_Vid->mb_data = cps->mb_data;
        p_Vid->last_dec_layer_id = layer_id;
    }
}

#if MVC_EXTENSION_ENABLE
static void init_mvc_picture(slice_t* currSlice)
{
    VideoParameters* p_Vid = currSlice->p_Vid;
    dpb_t* p_Dpb = p_Vid->p_Dpb_layer[0];
    shr_t& shr = currSlice->header;

    storable_picture* p_pic = NULL;

    // find BL reconstructed picture
    if (!shr.field_pic_flag) {
        for (int i = 0; i < (int)p_Dpb->used_size; ++i) {
            pic_t* fs = p_Dpb->fs[i];
            if (fs->frame->slice.view_id == 0 && fs->frame->frame_poc == shr.PicOrderCnt) {
                p_pic = fs->frame;
                break;
            }
        }
    } else if (!shr.bottom_field_flag) {
        for (int i = 0; i < (int)p_Dpb->used_size; ++i) {
            pic_t* fs = p_Dpb->fs[i];
            if (fs->top_field->slice.view_id == 0 && fs->top_field->top_poc == shr.TopFieldOrderCnt) {
                p_pic = fs->top_field;
                break;
            }
        }
    } else {
        for (int i = 0; i < (int)p_Dpb->used_size; ++i) {
            pic_t* fs = p_Dpb->fs[i];
            if (fs->bottom_field->slice.view_id == 0 && fs->bottom_field->bottom_poc == shr.BottomFieldOrderCnt) {
                p_pic = fs->bottom_field;
                break;
            }
        }
    }

    if (p_pic)
        picture_in_dpb(currSlice, p_Vid, p_pic);
}
#endif


static void copy_dec_picture_JV(VideoParameters *p_Vid, storable_picture *dst, storable_picture *src)
{
    dst->sps = src->sps;
    dst->pps = src->pps;
    dst->slice_headers = src->slice_headers;

    dst->top_poc            = src->top_poc;
    dst->bottom_poc         = src->bottom_poc;
    dst->frame_poc          = src->frame_poc;

    dst->poc                = src->poc;
    dst->used_for_reference = src->used_for_reference;

    dst->PicNum             = src->PicNum;
    dst->frame_num          = src->frame_num;
    dst->recovery_frame     = src->recovery_frame;

    dst->slice.slice_type   = src->slice.slice_type;
    dst->slice.idr_flag     = src->slice.idr_flag;

    // store the necessary tone mapping sei into storable_picture structure
    dst->seiHasTone_mapping    = src->seiHasTone_mapping;
    dst->tone_mapping_model_id = src->tone_mapping_model_id;
    dst->tonemapped_bit_depth  = src->tonemapped_bit_depth;
    if (src->tone_mapping_lut) {
        int coded_data_bit_max = (1 << p_Vid->seiToneMapping->coded_data_bit_depth);
        dst->tone_mapping_lut = new px_t[coded_data_bit_max];
        memcpy(dst->tone_mapping_lut, src->tone_mapping_lut, sizeof(px_t) * coded_data_bit_max);
    }
}


void init_picture(slice_t* currSlice)
{
    VideoParameters *p_Vid = currSlice->p_Vid;
    InputParameters *p_Inp = currSlice->p_Vid->p_Inp;
    dpb_t* p_Dpb = currSlice->p_Dpb;
    sps_t& sps = *p_Vid->active_sps;
    shr_t& shr = currSlice->header;

    if (p_Vid->dec_picture)
        // this may only happen on slice loss
        exit_picture(p_Vid);
    p_Vid->dpb_layer_id = currSlice->layer_id;
    //set buffers;
    setup_buffers(p_Vid, currSlice->layer_id);

    if (p_Vid->recovery_point)
        p_Vid->recovery_frame_num = (shr.frame_num + p_Vid->recovery_frame_cnt) % sps.MaxFrameNum;

    if (currSlice->IdrPicFlag)
        p_Vid->recovery_frame_num = shr.frame_num;

    if (!p_Vid->recovery_point &&
        shr.frame_num != p_Vid->PrevRefFrameNum &&
        shr.frame_num != (p_Vid->PrevRefFrameNum + 1) % sps.MaxFrameNum) {
        if (sps.gaps_in_frame_num_value_allowed_flag == 0) {
#if (DISABLE_ERC == 0)
            // picture error concealment
            if (p_Inp->conceal_mode != 0) {
                if (shr.frame_num < (p_Vid->PrevRefFrameNum + 1) % sps.MaxFrameNum) {
                    /* Conceal lost IDR frames and any frames immediately
                       following the IDR. Use frame copy for these since
                       lists cannot be formed correctly for motion copy*/
                    p_Vid->conceal_mode = 1;
                    p_Vid->IDR_concealment_flag = 1;
                    p_Dpb->conceal_lost_frames(currSlice);
                    //reset to original concealment mode for future drops
                    p_Vid->conceal_mode = p_Inp->conceal_mode;
                } else {
                    //reset to original concealment mode for future drops
                    p_Vid->conceal_mode = p_Inp->conceal_mode;

                    p_Vid->IDR_concealment_flag = 0;
                    p_Dpb->conceal_lost_frames(currSlice);
                }
            } else
#endif
                /* Advanced Error Concealment would be called here to combat unintentional loss of pictures. */
                error(100, "An unintentional loss of pictures occurs! Exit\n");
        }
        if (p_Vid->conceal_mode == 0)
            fill_frame_num_gap(p_Vid, currSlice);
    }

    if (currSlice->nal_ref_idc)
        p_Vid->PrevRefFrameNum = shr.frame_num;

    //calculate POC
    currSlice->decode_poc();

    if (p_Vid->recovery_frame_num == (int) shr.frame_num && p_Vid->recovery_poc == 0x7fffffff)
        p_Vid->recovery_poc = shr.PicOrderCnt;

    if (currSlice->nal_ref_idc)
        p_Vid->last_ref_pic_poc = shr.PicOrderCnt;

    if (!shr.field_pic_flag || !shr.bottom_field_flag)
        p_Vid->snr->start_time = std::chrono::system_clock::now();

    storable_picture* dec_picture = p_Vid->dec_picture = new storable_picture(p_Vid, shr.structure,
        sps.PicWidthInMbs * 16, sps.FrameHeightInMbs * 16,
        sps.PicWidthInMbs * sps.MbWidthC, sps.FrameHeightInMbs * sps.MbHeightC, 1);
    dec_picture->sps = currSlice->active_sps;
    dec_picture->pps = currSlice->active_pps;
    dec_picture->slice_headers.push_back(currSlice);

    dec_picture->top_poc     = shr.TopFieldOrderCnt;
    dec_picture->bottom_poc  = shr.BottomFieldOrderCnt;
    dec_picture->frame_poc   = shr.PicOrderCnt;
    dec_picture->slice.iCodingType = !shr.field_pic_flag ?
                                     (shr.MbaffFrameFlag ? FRAME_MB_PAIR_CODING : FRAME_CODING) : FIELD_CODING;
    dec_picture->slice.layer_id    = currSlice->layer_id;
#if (MVC_EXTENSION_ENABLE)
    dec_picture->slice.view_id         = currSlice->view_id;
    dec_picture->slice.inter_view_flag = currSlice->inter_view_flag;
    dec_picture->slice.anchor_pic_flag = currSlice->anchor_pic_flag;
    if (dec_picture->slice.view_id == 1) {
        if ((p_Vid->profile_idc == MVC_HIGH) || (p_Vid->profile_idc == STEREO_HIGH))
            init_mvc_picture(currSlice);
    }
#endif

    // reset all variables of the error concealment instance before decoding of every frame.
    // here the third parameter should, if perfectly, be equal to the number of slices per frame.
    // using little value is ok, the code will allocate more memory if the slice number is larger
#if (DISABLE_ERC == 0)
    p_Vid->erc_errorVar->reset(shr.PicSizeInMbs, shr.PicSizeInMbs);
#endif

    if (!shr.field_pic_flag)
        dec_picture->poc = shr.PicOrderCnt;
    else if (!shr.bottom_field_flag) {
        dec_picture->poc = shr.TopFieldOrderCnt;
        p_Vid->number *= 2;
    } else {
        dec_picture->poc = shr.BottomFieldOrderCnt;
        p_Vid->number = p_Vid->number * 2 + 1;
    }

    if (p_Vid->type > SI_slice)
        p_Vid->type = P_slice;  // concealed element

    // TO set mb_t Map (mark all MBs as 'have to be concealed')
    if (sps.separate_colour_plane_flag) {
        for (int nplane = 0; nplane < 3; ++nplane) {
            mb_t* currMB = p_Vid->mb_data_JV[nplane];
            for (int i = 0; i < shr.PicSizeInMbs; ++i)
                reset_mbs(currMB++);
        }
    } else {
        mb_t* currMB = p_Vid->mb_data;
        for (int i = 0; i < shr.PicSizeInMbs; ++i)
            reset_mbs(currMB++);
    }

    dec_picture->used_for_reference = currSlice->nal_ref_idc != 0;

    dec_picture->slice.idr_flag     = currSlice->IdrPicFlag;
    dec_picture->slice.slice_type   = p_Vid->type;

    dec_picture->PicNum             = shr.frame_num;
    dec_picture->frame_num          = shr.frame_num;
    dec_picture->recovery_frame     = (unsigned int) ((int) shr.frame_num == p_Vid->recovery_frame_num);

    // store the necessary tone mapping sei into storable_picture structure
    if (p_Vid->seiToneMapping->seiHasTone_mapping) {
        int coded_data_bit_max = (1 << p_Vid->seiToneMapping->coded_data_bit_depth);
        dec_picture->seiHasTone_mapping    = 1;
        dec_picture->tone_mapping_model_id = p_Vid->seiToneMapping->model_id;
        dec_picture->tonemapped_bit_depth  = p_Vid->seiToneMapping->sei_bit_depth;
        dec_picture->tone_mapping_lut      = new px_t[coded_data_bit_max];
        memcpy(dec_picture->tone_mapping_lut, p_Vid->seiToneMapping->lut, sizeof(px_t) * coded_data_bit_max);
        update_tone_mapping_sei(p_Vid->seiToneMapping);
    } else
        dec_picture->seiHasTone_mapping = 0;

    if (sps.separate_colour_plane_flag) {
        p_Vid->dec_picture_JV[0] = p_Vid->dec_picture;
        p_Vid->dec_picture_JV[1] = new storable_picture(
            p_Vid, (PictureStructure) shr.structure,
            sps.PicWidthInMbs * 16, sps.FrameHeightInMbs * 16,
            sps.PicWidthInMbs * sps.MbWidthC, sps.FrameHeightInMbs * sps.MbHeightC, 1);
        p_Vid->dec_picture_JV[1]->sps = currSlice->active_sps;
        p_Vid->dec_picture_JV[1]->pps = currSlice->active_pps;
        p_Vid->dec_picture_JV[1]->slice_headers.push_back(currSlice);
        copy_dec_picture_JV( p_Vid, p_Vid->dec_picture_JV[1], p_Vid->dec_picture_JV[0] );
        p_Vid->dec_picture_JV[2] = new storable_picture(
            p_Vid, (PictureStructure) shr.structure,
            sps.PicWidthInMbs * 16, sps.FrameHeightInMbs * 16,
            sps.PicWidthInMbs * sps.MbWidthC, sps.FrameHeightInMbs * sps.MbHeightC, 1);
        copy_dec_picture_JV( p_Vid, p_Vid->dec_picture_JV[2], p_Vid->dec_picture_JV[0] );
        p_Vid->dec_picture_JV[2]->sps = currSlice->active_sps;
        p_Vid->dec_picture_JV[2]->pps = currSlice->active_pps;
        p_Vid->dec_picture_JV[2]->slice_headers.push_back(currSlice);
    }
}

static int init_global_buffers(VideoParameters *p_Vid, int layer_id)
{
    int memory_size=0;
    int i;
    CodingParameters *cps = p_Vid->p_EncodePar[layer_id];
    sps_t *sps = p_Vid->active_sps;
    int FrameSizeInMbs = sps->PicWidthInMbs * sps->FrameHeightInMbs;

    if (p_Vid->global_init_done[layer_id])
        free_layer_buffers(p_Vid, layer_id);

    // allocate memory in structure p_Vid
    if (sps->separate_colour_plane_flag) {
        for (i = 0; i < 3; i++)
            cps->mb_data_JV[i] = new mb_t[FrameSizeInMbs];
        cps->mb_data = NULL;
    } else {
        cps->mb_data = new mb_t[FrameSizeInMbs];
    }

    p_Vid->global_init_done[layer_id] = 1;

    return memory_size;
}

static void setup_layer_info(VideoParameters *p_Vid, sps_t *sps, LayerParameters *p_Lps)
{
    int layer_id = p_Lps->layer_id;
    p_Lps->p_Vid = p_Vid;
    p_Lps->p_Cps = p_Vid->p_EncodePar[layer_id];
    p_Lps->p_SPS = sps;
    p_Lps->p_Dpb = p_Vid->p_Dpb_layer[layer_id];
}

void activate_sps(VideoParameters *p_Vid, sps_t *sps)
{
    if (p_Vid->active_sps != sps) {
        int prev_profile_idc = p_Vid->active_sps ? p_Vid->active_sps->profile_idc : 0;

        if (p_Vid->dec_picture)
            exit_picture(p_Vid);
        p_Vid->active_sps = sps;

        if (p_Vid->dpb_layer_id == 0 && is_BL_profile(sps->profile_idc) && !p_Vid->p_Dpb_layer[0]->init_done) {
            setup_layer_info(p_Vid, sps, p_Vid->p_LayerPar[0]);
        } else if (p_Vid->dpb_layer_id == 1 && is_EL_profile(sps->profile_idc) && !p_Vid->p_Dpb_layer[1]->init_done) {
            setup_layer_info(p_Vid, sps, p_Vid->p_LayerPar[1]);
        }

#if (MVC_EXTENSION_ENABLE)
        if (prev_profile_idc != sps->profile_idc) {
            if (is_BL_profile(sps->profile_idc) && !p_Vid->p_Dpb_layer[0]->init_done) {
                init_global_buffers(p_Vid, 0);

                if (!p_Vid->no_output_of_prior_pics_flag) {
                    p_Vid->p_Dpb_layer[0]->flush();
                    p_Vid->p_Dpb_layer[1]->flush();
                }
                p_Vid->p_Dpb_layer[0]->init(p_Vid, 1);

            } else if ((is_MVC_profile(prev_profile_idc) ||
                        is_MVC_profile(sps->profile_idc)) &&
                       !p_Vid->p_Dpb_layer[1]->init_done) {

                assert(p_Vid->p_Dpb_layer[0]->init_done);
                if (p_Vid->p_Dpb_layer[0]->init_done) {
                    p_Vid->p_Dpb_layer[0]->free();
                    p_Vid->p_Dpb_layer[0]->init(p_Vid, 1);
                }

                init_global_buffers(p_Vid, 1);
                // for now lets re_init both buffers. Later, we should only re_init appropriate one
                // Note that we seem to be doing this for every frame which seems not good.
                p_Vid->p_Dpb_layer[1]->init(p_Vid, 2);
            }
        }
#endif

#if (DISABLE_ERC == 0)
        if (p_Vid->erc_errorVar)
            delete p_Vid->erc_errorVar;
        p_Vid->erc_errorVar = new ercVariables_t(sps->PicWidthInMbs * 16, sps->FrameHeightInMbs * 16, 1);
        //ercInit(p_Vid, sps->PicWidthInMbs * 16, sps->FrameHeightInMbs * 16, 1);
        if (p_Vid->dec_picture) {
            slice_t& slice = *p_Vid->dec_picture->slice_headers[0];
            shr_t& shr = slice.header;
            p_Vid->erc_errorVar->reset(shr.PicSizeInMbs, shr.PicSizeInMbs);
        }
#endif
    }
}

void activate_pps(VideoParameters *p_Vid, pps_t *pps)
{  
    if (p_Vid->active_pps != pps) {
        if (p_Vid->dec_picture)
            exit_picture(p_Vid);

        p_Vid->active_pps = pps;
    }
}



void UseParameterSet(slice_t *currSlice)
{
    VideoParameters *p_Vid = currSlice->p_Vid;
    int PicParsetId = currSlice->header.pic_parameter_set_id;  
    pps_t *pps = &p_Vid->PicParSet[PicParsetId];
    sps_t *sps = &p_Vid->SeqParSet[pps->seq_parameter_set_id];

    if (!pps->Valid)
        printf ("Trying to use an invalid (uninitialized) Picture Parameter Set with ID %d, expect the unexpected...\n", PicParsetId);
#if (MVC_EXTENSION_ENABLE)
    if (!currSlice->mvc_extension_flag && !currSlice->svc_extension_flag) {
        if (!sps->Valid)
            printf ("PicParset %d references an invalid (uninitialized) Sequence Parameter Set with ID %d, expect the unexpected...\n", 
        PicParsetId, (int) pps->seq_parameter_set_id);
    } else {
        // Set SPS to the subset SPS parameters
        p_Vid->active_subset_sps = p_Vid->SubsetSeqParSet + pps->seq_parameter_set_id;
        sps = &p_Vid->active_subset_sps->sps;
        if (!p_Vid->active_subset_sps->Valid)
            printf ("PicParset %d references an invalid (uninitialized) Subset Sequence Parameter Set with ID %d, expect the unexpected...\n", 
                    PicParsetId, (int) pps->seq_parameter_set_id);
    }
#endif

    p_Vid->dpb_layer_id = currSlice->layer_id;
    activate_sps(p_Vid, sps);
    activate_pps(p_Vid, pps);

    p_Vid->type = currSlice->header.slice_type;
}


namespace vio  {
namespace h264 {


void macroblock_t::init(slice_t& slice)
{
    sps_t& sps = *slice.active_sps;
    shr_t& shr = slice.header;
    mb_t& mb = *this;

    mb.p_Slice = &slice;
    mb.mbAddrX = slice.parser.current_mb_nr;
    // Save the slice number of this macroblock. When the macroblock below
    // is coded it will use this to decide if prediction for above is possible
    mb.slice_nr = (short) slice.current_slice_nr;
    mb.ei_flag  = 1;
    mb.dpl_flag = 0;

    /* Update coordinates of the current macroblock */
    if (shr.MbaffFrameFlag) {
        mb.mb.x = (mb.mbAddrX / 2) % sps.PicWidthInMbs;
        mb.mb.y = (mb.mbAddrX / 2) / sps.PicWidthInMbs * 2 + (mb.mbAddrX % 2);
    } else {
        mb.mb.x = mb.mbAddrX % sps.PicWidthInMbs;
        mb.mb.y = mb.mbAddrX / sps.PicWidthInMbs;
    }

    mb.is_intra_block          = 0;
    mb.mb_skip_flag            = 0;
    mb.mb_type                 = 0;
    mb.mb_qp_delta             = 0;
    mb.intra_chroma_pred_mode  = 0;
    mb.CodedBlockPatternLuma   = 0;
    mb.CodedBlockPatternChroma = 0;

    // Reset syntax element entries in MB struct
    if (shr.slice_type != I_slice) {
        memset(mb.mvd_l0, 0, sizeof(mb.mvd_l0));
        if (shr.slice_type == B_slice)
            memset(mb.mvd_l1, 0, sizeof(mb.mvd_l1));
    }

    memset(mb.cbp_blks, 0, sizeof(mb.cbp_blks));
    memset(mb.cbp_bits, 0, sizeof(mb.cbp_bits));

    if (!slice.parser.is_reset_coeff) {
        memset(slice.decoder.transform->cof[0][0], 0, 16 * 16 * sizeof(int));
        if (!slice.parser.is_reset_coeff_cr) {
            memset(slice.decoder.transform->cof[1][0], 0, 2 * 16 * 16 * sizeof(int));
            slice.parser.is_reset_coeff_cr = 1;
        }
        slice.parser.is_reset_coeff = 1;
    }

    mb.mb_field_decoding_flag = 0;
    if (shr.MbaffFrameFlag) {
        bool prevMbSkipped = (mb.mbAddrX % 2 == 1) ?
            slice.neighbour.mb_data[mb.mbAddrX - 1].mb_skip_flag : 0;
        if (mb.mbAddrX % 2 == 0 || prevMbSkipped) {
            int topMbAddr = mb.mbAddrX & ~1;

            mb_t* mbA = slice.neighbour.get_mb(&slice, false, topMbAddr, {-1, 0});
            mb_t* mbB = slice.neighbour.get_mb(&slice, false, topMbAddr, {0, -1});
            mbA = mbA && mbA->slice_nr == mb.slice_nr ? mbA : nullptr;
            mbB = mbB && mbB->slice_nr == mb.slice_nr ? mbB : nullptr;

            if (mbA)
                mb.mb_field_decoding_flag = mbA->mb_field_decoding_flag;
            else if (mbB)
                mb.mb_field_decoding_flag = mbB->mb_field_decoding_flag;
        } else
            mb.mb_field_decoding_flag = slice.neighbour.mb_data[mb.mbAddrX - 1].mb_field_decoding_flag;
    }
}

bool macroblock_t::close(slice_t& slice)
{
    pps_t& pps = *slice.active_pps;
    shr_t& shr = slice.header;

    bool eos_bit = (!shr.MbaffFrameFlag || this->mbAddrX % 2);
    bool startcode_follows;

    //! The if() statement below resembles the original code, which tested
    //! mbAddrX == p_Vid->PicSizeInMbs.  Both is, of course, nonsense
    //! In an error prone environment, one can only be sure to have a new
    //! picture by checking the tr of the next slice header!

    if (this->mbAddrX == shr.PicSizeInMbs - 1)
        return true;

    slice.parser.current_mb_nr = slice.p_Vid->ppSliceList[0]->NextMbAddress(slice.parser.current_mb_nr);

    if (pps.entropy_coding_mode_flag)
        startcode_follows = eos_bit && slice.parser.cabac[0].decode_terminate();
    else
        startcode_follows = !slice.parser.partArr[0].more_rbsp_data();

    if (slice.parser.current_mb_nr == -1) { // End of slice_t group, MUST be end of slice
        assert(startcode_follows);
        return true;
    }

    if (!startcode_follows)
        return false;

    if (shr.slice_type == I_slice || shr.slice_type == SI_slice)
        return true;
    if (pps.entropy_coding_mode_flag)
        return true;
    if (slice.parser.mb_skip_run <= 0)
        return true;

    return false;
}

    
}
}

slice_t::slice_t()
{
    get_mem3Dpel(&this->mb_pred, 3, 16, 16);

#if (MVC_EXTENSION_ENABLE)
    this->view_id         = -1;
    this->inter_view_flag = 0;
    this->anchor_pic_flag = 0;
#endif
    // reference flag initialization
    for (int i = 0; i < 17; i++)
        this->ref_flag[i] = 1;
    for (int j = 0; j < 2; j++) {
        for (int i = 0; i < MAX_LIST_SIZE; i++)
            this->RefPicList[j][i] = NULL;
        this->RefPicSize[j] = 0;
    }
}

slice_t::~slice_t()
{
    free_mem3Dpel(this->mb_pred);
}

void slice_t::init()
{
    VideoParameters *p_Vid = this->p_Vid;
    p_Vid->active_sps = this->active_sps;
    p_Vid->active_pps = this->active_pps;
    shr_t& shr = this->header;

    this->parser.current_mb_nr = shr.first_mb_in_slice * (1 + shr.MbaffFrameFlag);
    if (this->active_pps->entropy_coding_mode_flag)
        this->parser.cabac[0].init(&this->parser.partArr[0]);

    this->num_dec_mb = 0;

    if (this->active_sps->separate_colour_plane_flag) {
        p_Vid->mb_data     = p_Vid->mb_data_JV    [shr.colour_plane_id];
        p_Vid->dec_picture = p_Vid->dec_picture_JV[shr.colour_plane_id];
    }
    this->neighbour.mb_data = p_Vid->mb_data;
    this->dec_picture = p_Vid->dec_picture;

    this->init_ref_lists();

    this->parser.init(*this);
    this->decoder.init(*this);
    //this->decoder.assign_quant_params(*this);

    if (shr.slice_type != I_slice && shr.slice_type != SI_slice) {
        if (!this->active_sps->separate_colour_plane_flag || shr.colour_plane_id == 0) {
            storable_picture* vidref = p_Vid->no_reference_picture;
            int noref = (shr.PicOrderCnt < p_Vid->recovery_poc);
            for (int j = 0; j < 2; ++j) {
                for (int i = 0; i < MAX_LIST_SIZE; ++i) {
                    storable_picture* curr_ref = this->RefPicList[j][i];
                    if (curr_ref)
                        curr_ref->no_ref = noref && (curr_ref == vidref);
                }
            }
        }
    }
}

void slice_t::decode()
{
    shr_t& shr = this->header;

    bool end_of_slice = 0;

    while (!end_of_slice) { // loop over macroblocks
        mb_t& mb = this->neighbour.mb_data[this->parser.current_mb_nr]; 
        mb.init(*this);
        this->parser.parse(mb);
        this->decoder.decode(mb);

        if (shr.MbaffFrameFlag && mb.mb_field_decoding_flag) {
            shr.num_ref_idx_l0_active_minus1 = ((shr.num_ref_idx_l0_active_minus1 + 1) >> 1) - 1;
            shr.num_ref_idx_l1_active_minus1 = ((shr.num_ref_idx_l1_active_minus1 + 1) >> 1) - 1;
        }

#if (DISABLE_ERC == 0)
        this->p_Vid->erc_errorVar->ercWriteMBMODEandMV(mb, shr.slice_type, this->p_Vid->dec_picture);
#endif

        end_of_slice = mb.close(*this);

        ++this->num_dec_mb;
    }
}
