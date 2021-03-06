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

using namespace vio::h264;

#include "erc_api.h"


#if (MVC_EXTENSION_ENABLE)
static int GetBaseViewId(VideoParameters* p_Vid, sub_sps_t** subset_sps)
{
    sub_sps_t* curr_subset_sps;
    int i, iBaseViewId = 0; //-1;

    *subset_sps = NULL;
    curr_subset_sps = p_Vid->SubsetSeqParSet;
    for (i = 0; i < MAX_NUM_SPS; i++) {
        if (curr_subset_sps->sps_mvc.views.size() > 0 && curr_subset_sps->sps.Valid) {
            iBaseViewId = curr_subset_sps->sps_mvc.views[0].view_id;
            break;
        }
        curr_subset_sps++;
    }

    if (i < MAX_NUM_SPS)
        *subset_sps = curr_subset_sps;
    return iBaseViewId;
}

int GetVOIdx(VideoParameters *p_Vid, int iViewId)
{
    int iVOIdx = -1;
    if (p_Vid->active_subset_sps) {
        auto& piViewIdMap = p_Vid->active_subset_sps->sps_mvc.views;
        for (iVOIdx = p_Vid->active_subset_sps->sps_mvc.num_views_minus1; iVOIdx >= 0; iVOIdx--) {
            if (piViewIdMap[iVOIdx].view_id == iViewId)
                break;
        }
    } else {
        int i;
        sub_sps_t* curr_subset_sps = p_Vid->SubsetSeqParSet;
        for (i = 0; i < MAX_NUM_SPS; i++) {
            if (curr_subset_sps->sps_mvc.views.size() > 0 && curr_subset_sps->sps.Valid)
                break;
            curr_subset_sps++;
        }

        if (i < MAX_NUM_SPS) {
            p_Vid->active_subset_sps = curr_subset_sps;
            auto& piViewIdMap = p_Vid->active_subset_sps->sps_mvc.views;
            for (iVOIdx = p_Vid->active_subset_sps->sps_mvc.num_views_minus1; iVOIdx >= 0; iVOIdx--)
                if (piViewIdMap[iVOIdx].view_id == iViewId)
                    break;

            return iVOIdx;
        } else
            iVOIdx = 0;
    }

    return iVOIdx;
}
#endif


static void Error_tracking(VideoParameters *p_Vid, slice_t *currSlice)
{
    shr_t& shr = currSlice->header;

    if (shr.redundant_pic_cnt == 0)
        p_Vid->Is_primary_correct = p_Vid->Is_redundant_correct = 1;

    if (shr.redundant_pic_cnt == 0 && p_Vid->type != I_slice) {
        for (int i = 0; i < shr.num_ref_idx_l0_active_minus1 + 1 ; i++) {
            if (currSlice->ref_flag[i] == 0)  // any reference of primary slice is incorrect
                p_Vid->Is_primary_correct = 0; // primary slice is incorrect
        }
    } else if (shr.redundant_pic_cnt != 0 && p_Vid->type != I_slice) {
        int redundant_slice_ref_idx = shr.ref_pic_list_modifications[0].size() > 0 ?
                shr.ref_pic_list_modifications[0][0].abs_diff_pic_num_minus1 + 1 : 1;
        if (currSlice->ref_flag[redundant_slice_ref_idx] == 0)  // reference of redundant slice is incorrect
            p_Vid->Is_redundant_correct = 0;  // redundant slice is incorrect
    }
}


static int parse_idr(slice_t *currSlice)
{
    InputParameters *p_Inp = currSlice->p_Vid->p_Inp;
    VideoParameters *p_Vid = currSlice->p_Vid;
    nal_unit_t& nal = *p_Vid->nalu; 
    int current_header = 0;

    if (p_Vid->recovery_point || nal.nal_unit_type == nal_unit_t::NALU_TYPE_IDR) {
        if (!p_Vid->recovery_point_found) {
            if (nal.nal_unit_type != nal_unit_t::NALU_TYPE_IDR) {
                printf("Warning: Decoding does not start with an IDR picture.\n");
                p_Vid->non_conforming_stream = 1;
            } else
                p_Vid->non_conforming_stream = 0;
        }
        p_Vid->recovery_point_found = true;
    }

    if (!p_Vid->recovery_point_found)
        return current_header;

    currSlice->mvc_extension_flag = 0;
    currSlice->svc_extension_flag = 0;
#if (MVC_EXTENSION_ENABLE)
    if (p_Inp->DecodeAllLayers == 1 && nal.nal_unit_type == 1) {
        currSlice->mvc_extension_flag = nal.mvc_extension_flag;
        currSlice->svc_extension_flag = nal.svc_extension_flag;
        currSlice->non_idr_flag       = nal.non_idr_flag;
        currSlice->priority_id        = nal.priority_id;
        currSlice->view_id            = nal.view_id;
        currSlice->temporal_id        = nal.temporal_id;
        currSlice->anchor_pic_flag    = nal.anchor_pic_flag;
        currSlice->inter_view_flag    = nal.inter_view_flag;
        currSlice->reserved_one_bit   = nal.reserved_one_bit;
    }
#endif

    currSlice->IdrPicFlag    = nal.nal_unit_type == nal_unit_t::NALU_TYPE_IDR;
    currSlice->nal_ref_idc   = nal.nal_ref_idc;
    currSlice->nal_unit_type = nal.nal_unit_type;

#if (MVC_EXTENSION_ENABLE)
    if (currSlice->mvc_extension_flag) {
        currSlice->view_id         = nal.view_id;
        currSlice->anchor_pic_flag = nal.anchor_pic_flag;
        currSlice->inter_view_flag = nal.inter_view_flag;
    } else if (!currSlice->svc_extension_flag) { //SVC and the normal AVC;
        if (p_Vid->active_subset_sps == NULL) {
            currSlice->view_id         = GetBaseViewId(p_Vid, &p_Vid->active_subset_sps);
            currSlice->anchor_pic_flag = nal.nal_unit_type == nal_unit_t::NALU_TYPE_IDR;
            currSlice->inter_view_flag = 1;
        } else {
            assert(p_Vid->active_subset_sps->sps_mvc.num_views_minus1 >= 0);
            currSlice->view_id         = p_Vid->active_subset_sps->sps_mvc.views[0].view_id;
            currSlice->anchor_pic_flag = nal.nal_unit_type == nal_unit_t::NALU_TYPE_IDR;
            currSlice->inter_view_flag = 1;
        }
    }
    currSlice->layer_id = currSlice->view_id = GetVOIdx(p_Vid, currSlice->view_id);
#endif

    // Some syntax of the slice_t Header depends on the parameter set, which depends on
    // the parameter set ID of the SLice header.  Hence, read the pic_parameter_set_id
    // of the slice header first, then setup the active parameter sets, and then read
    // the rest of the slice header
    currSlice->parser.dp_mode = PAR_DP_1;
    currSlice->parser.partArr[0] = nal;
    currSlice->parser.partArr[0].slice_header(*currSlice);

    UseParameterSet(currSlice);
    /* Tian Dong: frame_num gap processing, if found */
    if (currSlice->IdrPicFlag) {
        p_Vid->PrevRefFrameNum = currSlice->header.frame_num;
        // picture error concealment
        p_Vid->last_ref_pic_poc = 0;
    }
    p_Vid->type = currSlice->header.slice_type;
    p_Vid->structure = currSlice->header.structure;
    p_Vid->no_output_of_prior_pics_flag = currSlice->header.no_output_of_prior_pics_flag;

#if (MVC_EXTENSION_ENABLE)
    if (currSlice->view_id >= 0)
        currSlice->p_Dpb = p_Vid->p_Dpb_layer[currSlice->view_id];
#endif

    currSlice->decoder.assign_quant_params(*currSlice);

    shr_t& shr = currSlice->header;

    // if primary slice is replaced with redundant slice, set the correct image type
    if (shr.redundant_pic_cnt && p_Vid->Is_primary_correct == 0 && p_Vid->Is_redundant_correct)
        p_Vid->dec_picture->slice.slice_type = p_Vid->type;

    if (!p_Vid->dec_picture || *(p_Vid->dec_picture->slice_headers[0]) != *currSlice) {
        if (p_Vid->iSliceNumOfCurrPic == 0)
            init_picture(currSlice);
        current_header = SOP;
    } else {
        current_header = SOS;
        p_Vid->dec_picture->slice_headers.push_back(currSlice);
    }

    p_Vid->recovery_point = false;
    return current_header;
}

static int parse_dpa(slice_t *currSlice)
{
    VideoParameters* p_Vid = currSlice->p_Vid;
    nal_unit_t& nal = *p_Vid->nalu;
    int current_header = 0;

    int slice_id_a, slice_id_b, slice_id_c;

    if (!p_Vid->recovery_point_found)
        return current_header;

    // read DP_A
    currSlice->dpB_NotPresent = 1;
    currSlice->dpC_NotPresent = 1;

    currSlice->mvc_extension_flag = 0;
    currSlice->svc_extension_flag = 0;
    currSlice->IdrPicFlag    = 0;
    currSlice->nal_ref_idc   = nal.nal_ref_idc;
    currSlice->nal_unit_type = nal.nal_unit_type;
#if MVC_EXTENSION_ENABLE
    currSlice->p_Dpb = p_Vid->p_Dpb_layer[0];
#endif
#if MVC_EXTENSION_ENABLE
    currSlice->view_id = GetBaseViewId(p_Vid, &p_Vid->active_subset_sps);
    currSlice->layer_id = currSlice->view_id = GetVOIdx(p_Vid, currSlice->view_id);
    currSlice->anchor_pic_flag = currSlice->IdrPicFlag;
    currSlice->inter_view_flag = 1;
#endif

    currSlice->parser.dp_mode = PAR_DP_3;
    currSlice->parser.partArr[0] = nal;
    currSlice->parser.partArr[0].slice_header(*currSlice);

    UseParameterSet(currSlice);
    /* Tian Dong: frame_num gap processing, if found */
    if (currSlice->IdrPicFlag) {
        p_Vid->PrevRefFrameNum = currSlice->header.frame_num;
        // picture error concealment
        p_Vid->last_ref_pic_poc = 0;
    }
    p_Vid->type = currSlice->header.slice_type;
    p_Vid->structure = currSlice->header.structure;
    p_Vid->no_output_of_prior_pics_flag = currSlice->header.no_output_of_prior_pics_flag;

#if MVC_EXTENSION_ENABLE
    currSlice->p_Dpb = p_Vid->p_Dpb_layer[currSlice->view_id];
#endif

    currSlice->decoder.assign_quant_params(*currSlice);

    if (!p_Vid->dec_picture || *(p_Vid->dec_picture->slice_headers[0]) != *currSlice) {
        if (p_Vid->iSliceNumOfCurrPic == 0)
            init_picture(currSlice);
        current_header = SOP;
    } else {
        current_header = SOS;
        p_Vid->dec_picture->slice_headers.push_back(currSlice);
    }

    // Now I need to read the slice ID, which depends on the value of
    // redundant_pic_cnt_present_flag

    slice_id_a = currSlice->parser.partArr[0].ue("NALU: DP_A slice_id");

    if (p_Vid->active_pps->entropy_coding_mode_flag)
        error(500, "received data partition with CABAC, this is not allowed");

    // continue with reading next DP
    p_Vid->bitstream >> nal;
    if (0 == nal.num_bytes_in_rbsp)
        return current_header;

    if (nal_unit_t::NALU_TYPE_DPB == nal.nal_unit_type) {
        // we got a DPB
        InterpreterRbsp& dp1 = currSlice->parser.partArr[1];
        dp1 = nal;

        slice_id_b = dp1.ue("NALU: DP_B slice_id");

        currSlice->dpB_NotPresent = 0; 

        if (slice_id_b != slice_id_a || nal.lost_packets) {
            printf ("Waning: got a data partition B which does not match DP_A (DP loss!)\n");
            currSlice->dpB_NotPresent = 1;
            currSlice->dpC_NotPresent = 1;
        } else {
            if (p_Vid->active_pps->redundant_pic_cnt_present_flag)
                dp1.ue("NALU: DP_B redundant_pic_cnt");

            // we're finished with DP_B, so let's continue with next DP
            p_Vid->bitstream >> nal;
            if (0 == nal.num_bytes_in_rbsp)
                return current_header;
        }
    } else
        currSlice->dpB_NotPresent = 1;

    // check if we got DP_C
    if (nal_unit_t::NALU_TYPE_DPC == nal.nal_unit_type) {
        InterpreterRbsp& dp2 = currSlice->parser.partArr[2];
        dp2 = nal;

        currSlice->dpC_NotPresent = 0;

        slice_id_c = dp2.ue("NALU: DP_C slice_id");
        if (slice_id_c != slice_id_a || nal.lost_packets) {
            printf ("Warning: got a data partition C which does not match DP_A(DP loss!)\n");
            currSlice->dpC_NotPresent =1;
        }

        if (p_Vid->active_pps->redundant_pic_cnt_present_flag)
            dp2.ue("NALU:SLICE_C redudand_pic_cnt");
    } else
        currSlice->dpC_NotPresent = 1;

    // check if we read anything else than the expected partitions
    if (nal.nal_unit_type != nal_unit_t::NALU_TYPE_DPB && nal.nal_unit_type != nal_unit_t::NALU_TYPE_DPC) {
        // we have a NALI that we can't process here, so restart processing
        return 100;
        // yes, "goto" should not be used, but it's really the best way here before we restructure the decoding loop
        // (which should be taken care of anyway)
    }

    return current_header;
}

static int read_new_slice(slice_t *currSlice)
{
    VideoParameters *p_Vid = currSlice->p_Vid;
    InputParameters *p_Inp = currSlice->p_Vid->p_Inp;

    nal_unit_t& nal = *p_Vid->nalu; 
    int current_header = 0;

    for (;;) {
        p_Vid->bitstream >> nal;
        if (0 == nal.num_bytes_in_rbsp)
            return EOS;

process_nalu:
        switch (nal.nal_unit_type) {
        case nal_unit_t::NALU_TYPE_SLICE:
        case nal_unit_t::NALU_TYPE_IDR:
            current_header = parse_idr(currSlice);
            if (current_header != 0)
                return current_header;
            break;

        case nal_unit_t::NALU_TYPE_DPA:
            current_header = parse_dpa(currSlice);
            if (current_header == 100)
                goto process_nalu;
            if (current_header != 0)
                return current_header;
            break;

        case nal_unit_t::NALU_TYPE_DPB:
            if (!p_Inp->silent)
                printf ("found data partition B without matching DP A, discarding\n");
            break;

        case nal_unit_t::NALU_TYPE_DPC:
            if (!p_Inp->silent)
                printf ("found data partition C without matching DP A, discarding\n");
            break;

        case nal_unit_t::NALU_TYPE_SEI:
            {
                InterpreterRbsp* dp = new InterpreterRbsp { nal };
                dp->p_Vid = p_Vid;
                dp->slice = currSlice;

                dp->sei_rbsp();

                delete dp;
            }
            break;

        case nal_unit_t::NALU_TYPE_PPS:
            {
                InterpreterRbsp* dp = new InterpreterRbsp { nal };
                pps_t* pps = new pps_t;

                dp->pic_parameter_set_rbsp(p_Vid, *pps);

                if (p_Vid->active_pps) {
                    if (pps->pic_parameter_set_id == p_Vid->active_pps->pic_parameter_set_id) {
                        if (!(*pps == *(p_Vid->active_pps))) {
                            memcpy(p_Vid->pNextPPS, p_Vid->active_pps, sizeof (pps_t));
                            if (p_Vid->dec_picture)
                                exit_picture(p_Vid);
                            p_Vid->active_pps = nullptr;
                        }
                    }
                }
                p_Vid->PicParSet[pps->pic_parameter_set_id] = *pps;

                delete dp;
                delete pps;
            }
            break;

        case nal_unit_t::NALU_TYPE_SPS:
            {  
                InterpreterRbsp* dp = new InterpreterRbsp { nal };
                sps_t* sps = new sps_t;

                dp->seq_parameter_set_rbsp(*sps);

                if (sps->Valid) {
                    if (p_Vid->active_sps) {
                        if (sps->seq_parameter_set_id == p_Vid->active_sps->seq_parameter_set_id) {
                            if (!(*sps == *(p_Vid->active_sps))) {
                                if (p_Vid->dec_picture)
                                    exit_picture(p_Vid);
                                p_Vid->active_sps = NULL;
                            }
                        }
                    }
                    p_Vid->SeqParSet[sps->seq_parameter_set_id] = *sps;
                    if (p_Vid->profile_idc < (int) sps->profile_idc)
                        p_Vid->profile_idc = sps->profile_idc;
                }

                delete sps;
                delete dp;
            }
            break;

        case nal_unit_t::NALU_TYPE_AUD:
            break;

        case nal_unit_t::NALU_TYPE_EOSEQ:
            break;

        case nal_unit_t::NALU_TYPE_EOSTREAM:
            break;

        case nal_unit_t::NALU_TYPE_FILL:
            break;

        case nal_unit_t::NALU_TYPE_SPS_EXT:
            {
                InterpreterRbsp* dp = new InterpreterRbsp { nal };
                sps_ext_t* sps_ext = new sps_ext_t;
                dp->seq_parameter_set_extension_rbsp(*sps_ext);
                delete sps_ext;
                delete dp;
            }
            break;

#if (MVC_EXTENSION_ENABLE)
        case nal_unit_t::NALU_TYPE_VDRD:
            break;

        case nal_unit_t::NALU_TYPE_PREFIX:
            {
                InterpreterRbsp* dp = new InterpreterRbsp { nal };
                dp->prefix_nal_unit_rbsp();
                delete dp;
            }
            break;

        case nal_unit_t::NALU_TYPE_SUB_SPS:
            if (p_Inp->DecodeAllLayers == 1) {
                InterpreterRbsp* dp = new InterpreterRbsp { nal };
                sub_sps_t* sub_sps = new sub_sps_t;

                dp->subset_seq_parameter_set_rbsp(*sub_sps);

                if (sub_sps->Valid) {
                    sub_sps_t& sub_new = p_Vid->SubsetSeqParSet[sub_sps->sps.seq_parameter_set_id];
                    if (sub_new.Valid) {
                        if (memcmp(&sub_new.sps, &sub_sps->sps, sizeof(sps_t) - sizeof(int)))
                            assert(0);
                    }

                    sub_new = *sub_sps;

                    if (sub_new.sps_mvc.num_views_minus1 > 1) {
                        printf("Warning: num_views:%d is greater than 2, only decode baselayer!\n",
                                sub_new.sps_mvc.num_views_minus1 + 1);
                        sub_new.Valid = 0;
                        p_Vid->p_Inp->DecodeAllLayers = 0;
                    } else if (sub_new.sps_mvc.num_views_minus1 == 1 &&
                        (sub_new.sps_mvc.views[0].view_id != 0 || sub_new.sps_mvc.views[1].view_id != 1))
                        p_Vid->OpenOutputFiles(sub_new.sps_mvc.views[0].view_id, sub_new.sps_mvc.views[1].view_id);

                    if (sub_new.Valid)
                        p_Vid->profile_idc = sub_new.sps.profile_idc;
                }

                delete sub_sps;
                delete dp;
            } else if (!p_Inp->silent)
                printf ("Found Subsequence SPS NALU. Ignoring.\n");
            break;

        case nal_unit_t::NALU_TYPE_SLC_EXT:
            if (p_Inp->DecodeAllLayers == 0 && !p_Inp->silent)
                printf ("Found SVC extension NALU (%d). Ignoring.\n",
                        (int) nal.nal_unit_type);
            break;
#endif

        default:
            if (!p_Inp->silent)
                printf ("Found NALU type %d, len %d undefined, ignore NALU, moving on\n",
                        (int) nal.nal_unit_type, (int) nal.num_bytes_in_rbsp);
            break;
        }
    }
}

int DecoderParams::decode_slice_headers()
{
    VideoParameters *p_Vid = this->p_Vid;
    int current_header = 0;
    slice_t *currSlice;
    auto& ppSliceList = p_Vid->ppSliceList;
    //read one picture first;
    p_Vid->iSliceNumOfCurrPic = 0;
    p_Vid->num_dec_mb = 0;

    if (p_Vid->newframe) {
        if (p_Vid->pNextPPS->Valid) {
            p_Vid->PicParSet[p_Vid->pNextPPS->pic_parameter_set_id] = *(p_Vid->pNextPPS);
            p_Vid->pNextPPS->Valid = 0;
        }

        //get the first slice from currentslice;
        assert(ppSliceList[p_Vid->iSliceNumOfCurrPic]);
        currSlice = p_Vid->pNextSlice;
        p_Vid->pNextSlice = ppSliceList[p_Vid->iSliceNumOfCurrPic];
        ppSliceList[p_Vid->iSliceNumOfCurrPic] = currSlice;
        assert(currSlice->current_slice_nr == 0);

        UseParameterSet(currSlice);

        init_picture(currSlice);

        p_Vid->iSliceNumOfCurrPic++;
        current_header = SOS;
        //p_Vid->newframe = 0;
    }

    while (current_header != SOP && current_header != EOS) {
        //no pending slices;
        while (p_Vid->iSliceNumOfCurrPic >= ppSliceList.size())
            ppSliceList.push_back(new slice_t);
        currSlice = ppSliceList[p_Vid->iSliceNumOfCurrPic];
        currSlice->p_Vid = p_Vid;
        currSlice->p_Dpb = p_Vid->p_Dpb_layer[0]; //set default value;

        current_header = read_new_slice(currSlice);

        shr_t& shr = currSlice->header;

        // error tracking of primary and redundant slices.
        Error_tracking(p_Vid, currSlice);
        // If primary and redundant are received and primary is correct, discard the redundant
        // else, primary slice will be replaced with redundant slice.
        if (shr.frame_num == p_Vid->previous_frame_num &&
            shr.redundant_pic_cnt != 0 &&
            p_Vid->Is_primary_correct != 0 && current_header != EOS)
            continue;

        if ((current_header == SOS) || (current_header == SOP && p_Vid->iSliceNumOfCurrPic == 0)) {
            currSlice->current_slice_nr = (short) p_Vid->iSliceNumOfCurrPic;
            if (p_Vid->iSliceNumOfCurrPic > 0) {
                shr.PicOrderCnt         = ppSliceList[0]->header.PicOrderCnt;
                shr.TopFieldOrderCnt    = ppSliceList[0]->header.TopFieldOrderCnt;
                shr.BottomFieldOrderCnt = ppSliceList[0]->header.BottomFieldOrderCnt;  
            }
            p_Vid->iSliceNumOfCurrPic++;
            while (p_Vid->iSliceNumOfCurrPic >= ppSliceList.size())
                ppSliceList.push_back(new slice_t);
            current_header = SOS;       
        }
        if (current_header == SOP && p_Vid->iSliceNumOfCurrPic > 0) {
            p_Vid->newframe = 1;
            currSlice->current_slice_nr = 0;
            //keep it in currentslice;
            ppSliceList[p_Vid->iSliceNumOfCurrPic] = p_Vid->pNextSlice;
            p_Vid->pNextSlice = currSlice; 
        }
    }
    
    return current_header;
}


namespace vio { namespace h264 {

bool operator==(const sps_t& l, const sps_t& r)
{
    if (!l.Valid || !r.Valid)
        return false;

    bool equal = true;

    equal &= (l.profile_idc               == r.profile_idc);
    equal &= (l.constraint_set0_flag      == r.constraint_set0_flag);
    equal &= (l.constraint_set1_flag      == r.constraint_set1_flag);
    equal &= (l.constraint_set2_flag      == r.constraint_set2_flag);
    equal &= (l.level_idc                 == r.level_idc);
    equal &= (l.seq_parameter_set_id      == r.seq_parameter_set_id);
    equal &= (l.log2_max_frame_num_minus4 == r.log2_max_frame_num_minus4);
    equal &= (l.pic_order_cnt_type        == r.pic_order_cnt_type);
    if (!equal)
        return false;

    if (l.pic_order_cnt_type == 0)
        equal &= (l.log2_max_pic_order_cnt_lsb_minus4 == r.log2_max_pic_order_cnt_lsb_minus4);
    else if (l.pic_order_cnt_type == 1) {
        equal &= (l.delta_pic_order_always_zero_flag == r.delta_pic_order_always_zero_flag);
        equal &= (l.offset_for_non_ref_pic == r.offset_for_non_ref_pic);
        equal &= (l.offset_for_top_to_bottom_field == r.offset_for_top_to_bottom_field);
        equal &= (l.num_ref_frames_in_pic_order_cnt_cycle == r.num_ref_frames_in_pic_order_cnt_cycle);
        if (!equal)
            return false;

        for (int i = 0; i < l.num_ref_frames_in_pic_order_cnt_cycle; ++i)
            equal &= (l.offset_for_ref_frame[i] == r.offset_for_ref_frame[i]);
    }

    equal &= (l.max_num_ref_frames                   == r.max_num_ref_frames);
    equal &= (l.gaps_in_frame_num_value_allowed_flag == r.gaps_in_frame_num_value_allowed_flag);
    equal &= (l.pic_width_in_mbs_minus1              == r.pic_width_in_mbs_minus1);
    equal &= (l.pic_height_in_map_units_minus1       == r.pic_height_in_map_units_minus1);
    equal &= (l.frame_mbs_only_flag                  == r.frame_mbs_only_flag);
    if (!equal)
        return false;
  
    if (!l.frame_mbs_only_flag)
        equal &= (l.mb_adaptive_frame_field_flag == r.mb_adaptive_frame_field_flag);

    equal &= (l.direct_8x8_inference_flag == r.direct_8x8_inference_flag);
    equal &= (l.frame_cropping_flag       == r.frame_cropping_flag);
    if (!equal)
        return false;

    if (l.frame_cropping_flag) {
        equal &= (l.frame_crop_left_offset   == r.frame_crop_left_offset);
        equal &= (l.frame_crop_right_offset  == r.frame_crop_right_offset);
        equal &= (l.frame_crop_top_offset    == r.frame_crop_top_offset);
        equal &= (l.frame_crop_bottom_offset == r.frame_crop_bottom_offset);
    }
    equal &= (l.vui_parameters_present_flag == r.vui_parameters_present_flag);

    return equal;
}

bool operator==(const pps_t& l, const pps_t& r)
{
    if (!l.Valid || !r.Valid)
        return false;

    bool equal = true;

    equal &= (l.pic_parameter_set_id     == r.pic_parameter_set_id);
    equal &= (l.seq_parameter_set_id     == r.seq_parameter_set_id);
    equal &= (l.entropy_coding_mode_flag == r.entropy_coding_mode_flag);
    equal &= (l.bottom_field_pic_order_in_frame_present_flag == r.bottom_field_pic_order_in_frame_present_flag);
    equal &= (l.num_slice_groups_minus1  == r.num_slice_groups_minus1);

    if (!equal)
        return false;

    if (l.num_slice_groups_minus1 > 0) {
        equal &= (l.slice_group_map_type == r.slice_group_map_type);
        if (!equal)
            return false;

        if (l.slice_group_map_type == 0) {
            for (int i = 0; i <= l.num_slice_groups_minus1; ++i)
                equal &= (l.slice_groups[i].run_length_minus1 == r.slice_groups[i].run_length_minus1);
        } else if (l.slice_group_map_type == 2) {
            for (int i = 0; i < l.num_slice_groups_minus1; ++i) {
                equal &= (l.slice_groups[i].top_left     == r.slice_groups[i].top_left);
                equal &= (l.slice_groups[i].bottom_right == r.slice_groups[i].bottom_right);
            }
        } else if (l.slice_group_map_type == 3 || l.slice_group_map_type == 4 || l.slice_group_map_type == 5) {
            equal &= (l.slice_group_change_direction_flag == r.slice_group_change_direction_flag);
            equal &= (l.slice_group_change_rate_minus1 == r.slice_group_change_rate_minus1);
        } else if (l.slice_group_map_type == 6) {
            equal &= (l.pic_size_in_map_units_minus1 == r.pic_size_in_map_units_minus1);
            if (!equal)
                return false;

            for (int i = 0; i <= l.pic_size_in_map_units_minus1; ++i)
                equal &= (l.slice_group_id[i] == r.slice_group_id[i]);
        }
    }

    equal &= (l.num_ref_idx_l0_default_active_minus1 == r.num_ref_idx_l0_default_active_minus1);
    equal &= (l.num_ref_idx_l1_default_active_minus1 == r.num_ref_idx_l1_default_active_minus1);
    equal &= (l.weighted_pred_flag     == r.weighted_pred_flag);
    equal &= (l.weighted_bipred_idc    == r.weighted_bipred_idc);
    equal &= (l.pic_init_qp_minus26    == r.pic_init_qp_minus26);
    equal &= (l.pic_init_qs_minus26    == r.pic_init_qs_minus26);
    equal &= (l.chroma_qp_index_offset == r.chroma_qp_index_offset);
    equal &= (l.deblocking_filter_control_present_flag == r.deblocking_filter_control_present_flag);
    equal &= (l.constrained_intra_pred_flag == r.constrained_intra_pred_flag);
    equal &= (l.redundant_pic_cnt_present_flag == r.redundant_pic_cnt_present_flag);
    if (!equal)
        return false;

    //Fidelity Range Extensions Stuff
    //It is initialized to zero, so should be ok to check all the time.
    equal &= (l.transform_8x8_mode_flag == r.transform_8x8_mode_flag);
    equal &= (l.pic_scaling_matrix_present_flag == r.pic_scaling_matrix_present_flag);
    if (l.pic_scaling_matrix_present_flag) {
        for (int i = 0; i < 6 + (l.transform_8x8_mode_flag << 1); ++i) {
            equal &= (l.pic_scaling_list_present_flag[i] == r.pic_scaling_list_present_flag[i]);
            if (l.pic_scaling_list_present_flag[i]) {
                if (i < 6) {
                    for (int j = 0; j < 16; ++j)
                        equal &= (l.ScalingList4x4[i][j] == r.ScalingList4x4[i][j]);
                } else {
                    for (int j = 0; j < 64; ++j)
                        equal &= (l.ScalingList8x8[i - 6][j] == r.ScalingList8x8[i - 6][j]);
                }
            }
        }
    }
    equal &= (l.second_chroma_qp_index_offset == r.second_chroma_qp_index_offset);
    return equal;
}

} }

bool slice_t::operator!=(const slice_t& slice)
{
    const sps_t& sps = *slice.active_sps;
    const pps_t& pps = *slice.active_pps;
    const shr_t& shr = slice.header;

    bool result = false;

    result |= this->header.pic_parameter_set_id != shr.pic_parameter_set_id;
    result |= this->header.frame_num            != shr.frame_num;
    result |= this->header.field_pic_flag       != shr.field_pic_flag;

    if (shr.field_pic_flag && this->header.field_pic_flag)
        result |= this->header.bottom_field_flag != shr.bottom_field_flag;

    result |= this->nal_ref_idc != slice.nal_ref_idc && (this->nal_ref_idc == 0 || slice.nal_ref_idc == 0);
    result |= this->IdrPicFlag  != slice.IdrPicFlag;

    if (slice.IdrPicFlag && this->IdrPicFlag)
        result |= this->header.idr_pic_id != shr.idr_pic_id;

    if (sps.pic_order_cnt_type == 0) {
        result |= this->header.pic_order_cnt_lsb != shr.pic_order_cnt_lsb;
        if (pps.bottom_field_pic_order_in_frame_present_flag && !shr.field_pic_flag)
            result |= this->header.delta_pic_order_cnt_bottom != shr.delta_pic_order_cnt_bottom;
    }
    if (sps.pic_order_cnt_type == 1) {
        if (!sps.delta_pic_order_always_zero_flag) {
            result |= this->header.delta_pic_order_cnt[0] != shr.delta_pic_order_cnt[0];
            if (pps.bottom_field_pic_order_in_frame_present_flag && !shr.field_pic_flag)
                result |= this->header.delta_pic_order_cnt[1] != shr.delta_pic_order_cnt[1];
        }
    }

#if (MVC_EXTENSION_ENABLE)
    result |= this->view_id         != slice.view_id;
    result |= this->inter_view_flag != slice.inter_view_flag;
    result |= this->anchor_pic_flag != slice.anchor_pic_flag;
#endif
    result |= this->layer_id        != slice.layer_id;

    return result;
}
