#include <functional>

#include <limits.h>

#include "global.h"
#include "slice.h"
#include "image.h"
#include "dpb.h"
#include "ref_list.h"
#include "memalloc.h"
#include "output.h"

using vio::h264::mb_t;

#include "erc_api.h"


void update_pic_num(slice_t *currSlice)
{
    dpb_t *dpb = currSlice->p_Dpb;
    sps_t *sps = currSlice->active_sps;
    int i;

    if (!currSlice->field_pic_flag) {
        for (i = 0; i < dpb->ref_frames_in_buffer; i++) {
            frame_store *fs = dpb->fs_ref[i];
            if (fs->is_used == 3) {
                if (fs->frame->used_for_reference && !fs->frame->is_long_term) {
                    if (fs->FrameNum > currSlice->frame_num)
                        fs->FrameNumWrap = fs->FrameNum - sps->MaxFrameNum;
                    else
                        fs->FrameNumWrap = fs->FrameNum;
                    fs->frame->PicNum = fs->FrameNumWrap;
                }
            }
        }
        for (i = 0; i < dpb->ltref_frames_in_buffer; i++) {
            frame_store *fs = dpb->fs_ltref[i];
            if (fs->is_used == 3) {
                if (fs->frame->is_long_term)
                    fs->frame->LongTermPicNum = fs->frame->LongTermFrameIdx;
            }
        }
    } else {
        int add_top    = !currSlice->bottom_field_flag ? 1 : 0;
        int add_bottom = !currSlice->bottom_field_flag ? 0 : 1;

        for (i = 0; i < dpb->ref_frames_in_buffer; i++) {
            frame_store *fs = dpb->fs_ref[i];
            if (fs->is_reference) {
                if (fs->FrameNum > currSlice->frame_num)
                    fs->FrameNumWrap = fs->FrameNum - sps->MaxFrameNum;
                else
                    fs->FrameNumWrap = fs->FrameNum;
                if (fs->is_reference & 1)
                    fs->top_field->PicNum = 2 * fs->FrameNumWrap + add_top;
                if (fs->is_reference & 2)
                    fs->bottom_field->PicNum = 2 * fs->FrameNumWrap + add_bottom;
            }
        }
        for (i = 0; i < dpb->ltref_frames_in_buffer; i++) {
            frame_store *fs = dpb->fs_ltref[i];
            if (fs->is_long_term & 1)
                fs->top_field->LongTermPicNum = 2 * fs->top_field->LongTermFrameIdx + add_top;
            if (fs->is_long_term & 2)
                fs->bottom_field->LongTermPicNum = 2 * fs->bottom_field->LongTermFrameIdx + add_bottom;
        }
    }
}



static void gen_pic_list_from_frame_list(bool bottom_field_flag, frame_store **fs_list, int list_idx, storable_picture **list, char *list_size, int long_term)
{
    int top_idx = 0;
    int bot_idx = 0;

    auto is_ref = long_term ? std::mem_fn(&storable_picture::is_long_ref) : std::mem_fn(&storable_picture::is_short_ref);

    if (!bottom_field_flag) {
        while (top_idx < list_idx || bot_idx<list_idx) {
            for (; top_idx < list_idx; top_idx++) {
                if (fs_list[top_idx]->is_used & 1) {
                    if (is_ref(fs_list[top_idx]->top_field)) {
                        // short term ref pic
                        list[(short) *list_size] = fs_list[top_idx]->top_field;
                        (*list_size)++;
                        top_idx++;
                        break;
                    }
                }
            }
            for (; bot_idx < list_idx; bot_idx++) {
                if (fs_list[bot_idx]->is_used & 2) {
                    if (is_ref(fs_list[bot_idx]->bottom_field)) {
                        // short term ref pic
                        list[(short) *list_size] = fs_list[bot_idx]->bottom_field;
                        (*list_size)++;
                        bot_idx++;
                        break;
                    }
                }
            }
        }
    } else {
        while (top_idx < list_idx || bot_idx<list_idx) {
            for (; bot_idx < list_idx; bot_idx++) {
                if (fs_list[bot_idx]->is_used & 2) {
                    if (is_ref(fs_list[bot_idx]->bottom_field)) {
                        // short term ref pic
                        list[(short) *list_size] = fs_list[bot_idx]->bottom_field;
                        (*list_size)++;
                        bot_idx++;
                        break;
                    }
                }
            }
            for (; top_idx < list_idx; top_idx++) {
                if (fs_list[top_idx]->is_used & 1) {
                    if (is_ref(fs_list[top_idx]->top_field)) {
                        // short term ref pic
                        list[(short) *list_size] = fs_list[top_idx]->top_field;
                        (*list_size)++;
                        top_idx++;
                        break;
                    }
                }
            }
        }
    }
}

static void init_lists_i_slice(slice_t *currSlice)
{
#if (MVC_EXTENSION_ENABLE)
    currSlice->listinterviewidx0 = 0;
    currSlice->listinterviewidx1 = 0;
#endif
    currSlice->listXsize[0] = 0;
    currSlice->listXsize[1] = 0;
}

static void init_lists_p_slice(slice_t *currSlice)
{
    VideoParameters *p_Vid = currSlice->p_Vid;
    dpb_t *p_Dpb = currSlice->p_Dpb;

#if (MVC_EXTENSION_ENABLE)
    currSlice->listinterviewidx0 = 0;
    currSlice->listinterviewidx1 = 0;
#endif

    if (!currSlice->field_pic_flag) {
        int list0idx = 0;
        for (int i = 0; i < p_Dpb->ref_frames_in_buffer; i++) {
            if (p_Dpb->fs_ref[i]->is_used == 3) {
                if (p_Dpb->fs_ref[i]->frame->used_for_reference && !p_Dpb->fs_ref[i]->frame->is_long_term)
                    currSlice->listX[0][list0idx++] = p_Dpb->fs_ref[i]->frame;
            }
        }

        // order list 0 by PicNum
        std::qsort(currSlice->listX[0], list0idx, sizeof(storable_picture*), [](const void* a, const void* b) {
            int pic_num1 = (*(storable_picture**)a)->PicNum;
            int pic_num2 = (*(storable_picture**)b)->PicNum;
            //int pic_num1 = (*(reinterpret_cast<const storable_picture**>(a)))->PicNum;
            //int pic_num2 = (*(reinterpret_cast<const storable_picture**>(b)))->PicNum;
            return (pic_num1 < pic_num2) ? 1 : (pic_num1 > pic_num2) ? -1 : 0;
        });
        currSlice->listXsize[0] = (char) list0idx;

        // long term handling
        for (int i = 0; i < p_Dpb->ltref_frames_in_buffer; i++) {
            if (p_Dpb->fs_ltref[i]->is_used == 3) {
                if (p_Dpb->fs_ltref[i]->frame->is_long_term)
                    currSlice->listX[0][list0idx++] = p_Dpb->fs_ltref[i]->frame;
            }
        }

        std::qsort(&currSlice->listX[0][(short) currSlice->listXsize[0]], list0idx - currSlice->listXsize[0],
                   sizeof(storable_picture*), [](const void* a, const void* b) {
            int pic_num1 = (*(storable_picture**)a)->LongTermPicNum;
            int pic_num2 = (*(storable_picture**)b)->LongTermPicNum;
            return (pic_num1 < pic_num2) ? -1 : (pic_num1 > pic_num2) ? 1 : 0;
        });
        currSlice->listXsize[0] = (char) list0idx;
    } else {
        frame_store** fs_list0  = new frame_store*[p_Dpb->size];
        frame_store** fs_listlt = new frame_store*[p_Dpb->size];

        int list0idx = 0;
        for (int i = 0; i < p_Dpb->ref_frames_in_buffer; i++) {
            if (p_Dpb->fs_ref[i]->is_reference)
                fs_list0[list0idx++] = p_Dpb->fs_ref[i];
        }

        std::qsort(fs_list0, list0idx, sizeof(frame_store*), [](const void* a, const void* b) {
            int pic_num1 = (*(frame_store**)a)->FrameNumWrap;
            int pic_num2 = (*(frame_store**)b)->FrameNumWrap;
            return (pic_num1 < pic_num2) ? 1 : (pic_num1 > pic_num2) ? -1 : 0;
        });

        currSlice->listXsize[0] = 0;
        gen_pic_list_from_frame_list(currSlice->bottom_field_flag, fs_list0, list0idx, currSlice->listX[0], &currSlice->listXsize[0], 0);

        // long term handling
        int listltidx = 0;
        for (int i = 0; i < p_Dpb->ltref_frames_in_buffer; i++)
            fs_listlt[listltidx++]=p_Dpb->fs_ltref[i];

        std::qsort(fs_listlt, listltidx, sizeof(frame_store*), [](const void* a, const void* b) {
            int idx1 = (*(frame_store**)a)->LongTermFrameIdx;
            int idx2 = (*(frame_store**)b)->LongTermFrameIdx;
            return (idx1 < idx2) ? -1 : (idx1 > idx2) ? 1 : 0;
        });

        gen_pic_list_from_frame_list(currSlice->bottom_field_flag, fs_listlt, listltidx, currSlice->listX[0], &currSlice->listXsize[0], 1);

        delete []fs_list0;
        delete []fs_listlt;
    }

    // set max size
    currSlice->listXsize[0] = (char) min<int>(currSlice->listXsize[0], currSlice->num_ref_idx_l0_active_minus1 + 1);
    currSlice->listXsize[1] = 0;

    // set the unused list entries to NULL
    for (int i = currSlice->listXsize[0]; i < MAX_LIST_SIZE; i++)
        currSlice->listX[0][i] = p_Vid->no_reference_picture;
    for (int i = currSlice->listXsize[1]; i < MAX_LIST_SIZE; i++)
        currSlice->listX[1][i] = p_Vid->no_reference_picture;
}

static void init_lists_b_slice(slice_t *currSlice)
{
    VideoParameters *p_Vid = currSlice->p_Vid;
    dpb_t *p_Dpb = currSlice->p_Dpb;

#if (MVC_EXTENSION_ENABLE)
    currSlice->listinterviewidx0 = 0;
    currSlice->listinterviewidx1 = 0;
#endif

    // B-slice_t
    if (!currSlice->field_pic_flag) {
        int list0idx = 0;
        for (int i = 0; i < p_Dpb->ref_frames_in_buffer; i++) {
            if (p_Dpb->fs_ref[i]->is_used == 3) {
                if (p_Dpb->fs_ref[i]->frame->used_for_reference && !p_Dpb->fs_ref[i]->frame->is_long_term) {
                    if (currSlice->PicOrderCnt >= p_Dpb->fs_ref[i]->frame->poc) //!KS use >= for error concealment
                        currSlice->listX[0][list0idx++] = p_Dpb->fs_ref[i]->frame;
                }
            }
        }
        std::qsort(currSlice->listX[0], list0idx, sizeof(storable_picture*), [](const void* a, const void* b) {
            int poc1 = (*(storable_picture**)a)->poc;
            int poc2 = (*(storable_picture**)b)->poc;
            return (poc1 < poc2) ? 1 : (poc1 > poc2) ? -1 : 0;
        });

        //get the backward reference picture (POC>current POC) in list0;
        int list0idx_1 = list0idx;
        for (int i = 0; i < p_Dpb->ref_frames_in_buffer; i++) {
            if (p_Dpb->fs_ref[i]->is_used == 3) {
                if (p_Dpb->fs_ref[i]->frame->used_for_reference && !p_Dpb->fs_ref[i]->frame->is_long_term) {
                    if (currSlice->PicOrderCnt < p_Dpb->fs_ref[i]->frame->poc)
                        currSlice->listX[0][list0idx++] = p_Dpb->fs_ref[i]->frame;
                }
            }
        }
        std::qsort(&currSlice->listX[0][list0idx_1], list0idx-list0idx_1, sizeof(storable_picture*), [](const void* a, const void* b) {
            int poc1 = (*(storable_picture**)a)->poc;
            int poc2 = (*(storable_picture**)b)->poc;
            return (poc1 < poc2) ? -1 : (poc1 > poc2) ? 1 : 0;
        });

        for (int j = 0; j < list0idx_1; j++)
            currSlice->listX[1][list0idx - list0idx_1 + j] = currSlice->listX[0][j];
        for (int j = list0idx_1; j < list0idx; j++)
            currSlice->listX[1][j - list0idx_1] = currSlice->listX[0][j];

        currSlice->listXsize[0] = currSlice->listXsize[1] = list0idx;

        // long term handling
        for (int i = 0; i < p_Dpb->ltref_frames_in_buffer; i++) {
            if (p_Dpb->fs_ltref[i]->is_used == 3) {
                if (p_Dpb->fs_ltref[i]->frame->is_long_term) {
                    currSlice->listX[0][list0idx]   = p_Dpb->fs_ltref[i]->frame;
                    currSlice->listX[1][list0idx++] = p_Dpb->fs_ltref[i]->frame;
                }
            }
        }
        std::qsort(&currSlice->listX[0][(int)currSlice->listXsize[0]], list0idx - currSlice->listXsize[0],
                   sizeof(storable_picture*), [](const void* a, const void* b) {
            int pic_num1 = (*(storable_picture**)a)->LongTermPicNum;
            int pic_num2 = (*(storable_picture**)b)->LongTermPicNum;
            return (pic_num1 < pic_num2) ? -1 : (pic_num1 > pic_num2) ? 1 : 0;
        });
        std::qsort(&currSlice->listX[1][(int)currSlice->listXsize[0]], list0idx - currSlice->listXsize[0],
                   sizeof(storable_picture*), [](const void* a, const void* b) {
            int pic_num1 = (*(storable_picture**)a)->LongTermPicNum;
            int pic_num2 = (*(storable_picture**)b)->LongTermPicNum;
            return (pic_num1 < pic_num2) ? -1 : (pic_num1 > pic_num2) ? 1 : 0;
        });
        currSlice->listXsize[0] = currSlice->listXsize[1] = (char) list0idx;
    } else {
        frame_store** fs_list0  = new frame_store*[p_Dpb->size];
        frame_store** fs_list1  = new frame_store*[p_Dpb->size];
        frame_store** fs_listlt = new frame_store*[p_Dpb->size];

        currSlice->listXsize[0] = 0;
        currSlice->listXsize[1] = 1;

        int list0idx = 0;
        for (int i = 0; i < p_Dpb->ref_frames_in_buffer; i++) {
            if (p_Dpb->fs_ref[i]->is_used) {
                if (currSlice->PicOrderCnt >= p_Dpb->fs_ref[i]->poc)
                    fs_list0[list0idx++] = p_Dpb->fs_ref[i];
            }
        }
        std::qsort(fs_list0, list0idx, sizeof(frame_store*), [](const void* a, const void* b) {
            int poc1 = (*(frame_store**)a)->poc;
            int poc2 = (*(frame_store**)b)->poc;
            return (poc1 < poc2) ? 1 : (poc1 > poc2) ? -1 : 0;
        });

        int list0idx_1 = list0idx;
        for (int i = 0; i < p_Dpb->ref_frames_in_buffer; i++) {
            if (p_Dpb->fs_ref[i]->is_used) {
                if (currSlice->PicOrderCnt < p_Dpb->fs_ref[i]->poc)
                    fs_list0[list0idx++] = p_Dpb->fs_ref[i];
            }
        }
        std::qsort(&fs_list0[list0idx_1], list0idx-list0idx_1, sizeof(frame_store*), [](const void* a, const void* b) {
            int poc1 = (*(frame_store**)a)->poc;
            int poc2 = (*(frame_store**)b)->poc;
            return (poc1 < poc2) ? -1 : (poc1 > poc2) ? 1 : 0;
        });

        for (int j = 0; j < list0idx_1; j++)
            fs_list1[list0idx - list0idx_1 + j] = fs_list0[j];
        for (int j = list0idx_1; j < list0idx; j++)
            fs_list1[j - list0idx_1] = fs_list0[j];

        currSlice->listXsize[0] = 0;
        currSlice->listXsize[1] = 0;
        gen_pic_list_from_frame_list(currSlice->bottom_field_flag, fs_list0, list0idx, currSlice->listX[0], &currSlice->listXsize[0], 0);
        gen_pic_list_from_frame_list(currSlice->bottom_field_flag, fs_list1, list0idx, currSlice->listX[1], &currSlice->listXsize[1], 0);

        // long term handling
        int listltidx = 0;
        for (int i = 0; i < p_Dpb->ltref_frames_in_buffer; i++)
            fs_listlt[listltidx++] = p_Dpb->fs_ltref[i];

        std::qsort(fs_listlt, listltidx, sizeof(frame_store*), [](const void* a, const void* b) {
            int idx1 = (*(frame_store**)a)->LongTermFrameIdx;
            int idx2 = (*(frame_store**)b)->LongTermFrameIdx;
            return (idx1 < idx2) ? -1 : (idx1 > idx2) ? 1 : 0;
        });

        gen_pic_list_from_frame_list(currSlice->bottom_field_flag, fs_listlt, listltidx, currSlice->listX[0], &currSlice->listXsize[0], 1);
        gen_pic_list_from_frame_list(currSlice->bottom_field_flag, fs_listlt, listltidx, currSlice->listX[1], &currSlice->listXsize[1], 1);

        delete []fs_list0;
        delete []fs_list1;
        delete []fs_listlt;
    }

    if ((currSlice->listXsize[0] == currSlice->listXsize[1]) && (currSlice->listXsize[0] > 1)) {
        // check if lists are identical, if yes swap first two elements of currSlice->listX[1]
        int diff = 0;
        for (int j = 0; j < currSlice->listXsize[0]; j++) {
            if (currSlice->listX[0][j] != currSlice->listX[1][j]) {
                diff = 1;
                break;
            }
        }
        if (!diff) {
            storable_picture *tmp_s = currSlice->listX[1][0];
            currSlice->listX[1][0] = currSlice->listX[1][1];
            currSlice->listX[1][1] = tmp_s;
        }
    }

    // set max size
    currSlice->listXsize[0] = min<int>(currSlice->listXsize[0], currSlice->num_ref_idx_l0_active_minus1 + 1);
    currSlice->listXsize[1] = min<int>(currSlice->listXsize[1], currSlice->num_ref_idx_l1_active_minus1 + 1);

    // set the unused list entries to NULL
    for (int i = currSlice->listXsize[0]; i < MAX_LIST_SIZE; i++)
        currSlice->listX[0][i] = p_Vid->no_reference_picture;
    for (int i = currSlice->listXsize[1]; i < MAX_LIST_SIZE; i++)
        currSlice->listX[1][i] = p_Vid->no_reference_picture;
}


#if (MVC_EXTENSION_ENABLE)

static int is_view_id_in_ref_view_list(int view_id, int *ref_view_id, int num_ref_views)
{
    int i;
    for (i = 0; i < num_ref_views; i++) {
        if (view_id == ref_view_id[i])
            break;
    }

    return num_ref_views && i < num_ref_views;
}

static void append_interview_list(
    dpb_t *p_Dpb, bool field_pic_flag, bool bottom_field_flag,
    int list_idx, frame_store **list,
    int *listXsize, int currPOC, int curr_view_id, int anchor_pic_flag)
{
    VideoParameters *p_Vid = p_Dpb->p_Vid;
    int iVOIdx = curr_view_id;
    int pic_avail;
    int poc = 0;
    int fld_idx;
    int num_ref_views, *ref_view_id;
    frame_store *fs = p_Dpb->fs_ilref[0];

    if (iVOIdx < 0)
        printf("Error: iVOIdx: %d is not less than 0\n", iVOIdx);

    if (anchor_pic_flag) {
        num_ref_views = list_idx? p_Vid->active_subset_sps->num_anchor_refs_l1[iVOIdx] : p_Vid->active_subset_sps->num_anchor_refs_l0[iVOIdx];
        ref_view_id   = list_idx? p_Vid->active_subset_sps->anchor_ref_l1[iVOIdx]:p_Vid->active_subset_sps->anchor_ref_l0[iVOIdx];
    } else {
        num_ref_views = list_idx? p_Vid->active_subset_sps->num_non_anchor_refs_l1[iVOIdx] : p_Vid->active_subset_sps->num_non_anchor_refs_l0[iVOIdx];
        ref_view_id   = list_idx? p_Vid->active_subset_sps->non_anchor_ref_l1[iVOIdx]:p_Vid->active_subset_sps->non_anchor_ref_l0[iVOIdx];
    }

    fld_idx = bottom_field_flag ? 1 : 0;

    if (!field_pic_flag) {
        pic_avail = (fs->is_used == 3);
        if (pic_avail)
            poc = fs->frame->poc;
    } else if (!bottom_field_flag) {
        pic_avail = fs->is_used & 1;
        if (pic_avail)
            poc = fs->top_field->poc;
    } else {
        pic_avail = fs->is_used & 2;
        if (pic_avail)
            poc = fs->bottom_field->poc;
    }

    if (pic_avail && fs->inter_view_flag[fld_idx]) {
        if (poc == currPOC) {
            if (is_view_id_in_ref_view_list(fs->view_id, ref_view_id, num_ref_views)) {
                //add one inter-view reference;
                list[*listXsize] = fs; 
                //next;
                (*listXsize)++;
            }
        }
    }
}

static void gen_pic_list_from_frame_interview_list(bool bottom_field_flag, frame_store **fs_list, int list_idx, storable_picture **list, char *list_size)
{
    if (!bottom_field_flag) {
        for (int i = 0; i < list_idx; i++) {
            list[(int)(*list_size)] = fs_list[i]->top_field;
            (*list_size)++;
        }
    } else {
        for (int i = 0; i < list_idx; i++) {
            list[(int)(*list_size)] = fs_list[i]->bottom_field;
            (*list_size)++;
        }
    }
}

static void init_lists_i_slice_mvc(slice_t *currSlice)
{
    currSlice->listinterviewidx0 = 0;
    currSlice->listinterviewidx1 = 0;

    currSlice->listXsize[0] = 0;
    currSlice->listXsize[1] = 0;
}

static void init_lists_p_slice_mvc(slice_t *currSlice)
{
    VideoParameters *p_Vid = currSlice->p_Vid;
    dpb_t *p_Dpb = currSlice->p_Dpb;

    unsigned int i;

    int list0idx = 0;
    int listltidx = 0;

    int currPOC = currSlice->PicOrderCnt;
    int anchor_pic_flag = currSlice->anchor_pic_flag;

    currSlice->listinterviewidx0 = 0;
    currSlice->listinterviewidx1 = 0;

    if (!currSlice->field_pic_flag) {
        for (i=0; i<p_Dpb->ref_frames_in_buffer; i++) {
            if (p_Dpb->fs_ref[i]->is_used == 3) {
                if ((p_Dpb->fs_ref[i]->frame->used_for_reference)&&(!p_Dpb->fs_ref[i]->frame->is_long_term))
                    currSlice->listX[0][list0idx++] = p_Dpb->fs_ref[i]->frame;
            }
        }
        // order list 0 by PicNum
        std::qsort(currSlice->listX[0], list0idx, sizeof(storable_picture*), [](const void *arg1, const void *arg2) {
            int pic_num1 = (*(storable_picture**)arg1)->PicNum;
            int pic_num2 = (*(storable_picture**)arg2)->PicNum;
            return (pic_num1 < pic_num2) ? 1 : (pic_num1 > pic_num2) ? -1 : 0;
        });

        currSlice->listXsize[0] = (char) list0idx;

        // long term handling
        for (i = 0; i < p_Dpb->ltref_frames_in_buffer; i++) {
            if (p_Dpb->fs_ltref[i]->is_used == 3) {
                if (p_Dpb->fs_ltref[i]->frame->is_long_term)
                    currSlice->listX[0][list0idx++]=p_Dpb->fs_ltref[i]->frame;
            }
        }
        std::qsort(&currSlice->listX[0][(short) currSlice->listXsize[0]], list0idx - currSlice->listXsize[0],
                   sizeof(storable_picture*), [](const void *arg1, const void *arg2) {
            int long_term_pic_num1 = (*(storable_picture**)arg1)->LongTermPicNum;
            int long_term_pic_num2 = (*(storable_picture**)arg2)->LongTermPicNum;
            return (long_term_pic_num1 < long_term_pic_num2) ? -1 : (long_term_pic_num1 > long_term_pic_num2) ? 1 : 0;
        });

        currSlice->listXsize[0] = (char) list0idx;
    } else {
        frame_store** fs_list0  = new frame_store*[p_Dpb->size];
        frame_store** fs_listlt = new frame_store*[p_Dpb->size];

        for (i = 0; i < p_Dpb->ref_frames_in_buffer; i++) {
            if (p_Dpb->fs_ref[i]->is_reference)
                fs_list0[list0idx++] = p_Dpb->fs_ref[i];
        }

        std::qsort(fs_list0, list0idx, sizeof(frame_store*), [](const void *arg1, const void *arg2) {
            int frame_num_wrap1 = (*(frame_store**)arg1)->FrameNumWrap;
            int frame_num_wrap2 = (*(frame_store**)arg2)->FrameNumWrap;
            return (frame_num_wrap1 < frame_num_wrap2) ? 1 : (frame_num_wrap1 > frame_num_wrap2) ? -1 : 0;
        });

        currSlice->listXsize[0] = 0;
        gen_pic_list_from_frame_list(currSlice->bottom_field_flag, fs_list0, list0idx, currSlice->listX[0], &currSlice->listXsize[0], 0);

        // long term handling
        for (i = 0; i < p_Dpb->ltref_frames_in_buffer; i++)
            fs_listlt[listltidx++]=p_Dpb->fs_ltref[i];

        std::qsort(fs_listlt, listltidx, sizeof(frame_store*), [](const void *arg1, const void *arg2) {
            int long_term_frame_idx1 = (*(frame_store**)arg1)->LongTermFrameIdx;
            int long_term_frame_idx2 = (*(frame_store**)arg2)->LongTermFrameIdx;
            return (long_term_frame_idx1 < long_term_frame_idx2) ? -1 : (long_term_frame_idx1 > long_term_frame_idx2) ? 1 : 0;
        });

        gen_pic_list_from_frame_list(currSlice->bottom_field_flag, fs_listlt, listltidx, currSlice->listX[0], &currSlice->listXsize[0], 1);

        delete []fs_list0;
        delete []fs_listlt;
    }

    currSlice->listXsize[1] = 0;    

    if (currSlice->svc_extension_flag == 0) {
        int curr_view_id = currSlice->layer_id;
        currSlice->fs_listinterview0 = new frame_store*[p_Dpb->size];
        list0idx = currSlice->listXsize[0];
        if (!currSlice->field_pic_flag) {
            append_interview_list(p_Vid->p_Dpb_layer[1], false, false, 0, currSlice->fs_listinterview0, &currSlice->listinterviewidx0, currPOC, curr_view_id, anchor_pic_flag);
            for (i = 0; i < (unsigned int)currSlice->listinterviewidx0; i++)
                currSlice->listX[0][list0idx++]=currSlice->fs_listinterview0[i]->frame;
            currSlice->listXsize[0] = (char) list0idx;
        } else {
            append_interview_list(p_Vid->p_Dpb_layer[1], currSlice->field_pic_flag, currSlice->bottom_field_flag, 0, currSlice->fs_listinterview0, &currSlice->listinterviewidx0, currPOC, curr_view_id, anchor_pic_flag);
            gen_pic_list_from_frame_interview_list(currSlice->bottom_field_flag, currSlice->fs_listinterview0, currSlice->listinterviewidx0, currSlice->listX[0], &currSlice->listXsize[0]);
        }
    }

    // set max size
    currSlice->listXsize[0] = (char) min<int>(currSlice->listXsize[0], currSlice->num_ref_idx_l0_active_minus1 + 1);
    currSlice->listXsize[1] = (char) min<int>(currSlice->listXsize[1], currSlice->num_ref_idx_l1_active_minus1 + 1);

    // set the unused list entries to NULL
    for (i = currSlice->listXsize[0]; i < MAX_LIST_SIZE; i++)
        currSlice->listX[0][i] = p_Vid->no_reference_picture;
    for (i = currSlice->listXsize[1]; i < MAX_LIST_SIZE; i++)
        currSlice->listX[1][i] = p_Vid->no_reference_picture;
}

static void init_lists_b_slice_mvc(slice_t *currSlice)
{
    VideoParameters *p_Vid = currSlice->p_Vid;
    dpb_t *p_Dpb = currSlice->p_Dpb;

    unsigned int i;
    int j;

    int list0idx = 0;
    int list0idx_1 = 0;
    int listltidx = 0;

    int currPOC = currSlice->PicOrderCnt;
    int anchor_pic_flag = currSlice->anchor_pic_flag;

    currSlice->listinterviewidx0 = 0;
    currSlice->listinterviewidx1 = 0;

    // B-slice_t
    if (!currSlice->field_pic_flag) {
        for (i = 0; i < p_Dpb->ref_frames_in_buffer; i++) {
            if (p_Dpb->fs_ref[i]->is_used == 3) {
                if (p_Dpb->fs_ref[i]->frame->used_for_reference && !p_Dpb->fs_ref[i]->frame->is_long_term) {
                    if (currSlice->PicOrderCnt >= p_Dpb->fs_ref[i]->frame->poc) //!KS use >= for error concealment
                        currSlice->listX[0][list0idx++] = p_Dpb->fs_ref[i]->frame;
                }
            }
        }
        std::qsort(currSlice->listX[0], list0idx, sizeof(storable_picture*), [](const void *arg1, const void *arg2) {
            int poc1 = (*(storable_picture**)arg1)->poc;
            int poc2 = (*(storable_picture**)arg2)->poc;
            return (poc1 < poc2) ? 1 : (poc1 > poc2) ? -1 : 0;
        });

        list0idx_1 = list0idx;
        for (i = 0; i < p_Dpb->ref_frames_in_buffer; i++) {
            if (p_Dpb->fs_ref[i]->is_used == 3) {
                if (p_Dpb->fs_ref[i]->frame->used_for_reference && !p_Dpb->fs_ref[i]->frame->is_long_term) {
                    if (currSlice->PicOrderCnt < p_Dpb->fs_ref[i]->frame->poc)
                        currSlice->listX[0][list0idx++] = p_Dpb->fs_ref[i]->frame;
                }
            }
        }
        std::qsort(&currSlice->listX[0][list0idx_1], list0idx-list0idx_1,
                   sizeof(storable_picture*), [](const void *arg1, const void *arg2) {
            int poc1 = (*(storable_picture**)arg1)->poc;
            int poc2 = (*(storable_picture**)arg2)->poc;
            return (poc1 < poc2) ? -1 : (poc1 > poc2) ? 1 : 0;
        });

        for (j = 0; j < list0idx_1; j++)
            currSlice->listX[1][list0idx - list0idx_1 + j] = currSlice->listX[0][j];
        for (j = list0idx_1; j < list0idx; j++)
            currSlice->listX[1][j - list0idx_1] = currSlice->listX[0][j];

        currSlice->listXsize[0] = currSlice->listXsize[1] = (char) list0idx;

        // long term handling
        for (i = 0; i < p_Dpb->ltref_frames_in_buffer; i++) {
            if (p_Dpb->fs_ltref[i]->is_used == 3) {
                if (p_Dpb->fs_ltref[i]->frame->is_long_term) {
                    currSlice->listX[0][list0idx]   = p_Dpb->fs_ltref[i]->frame;
                    currSlice->listX[1][list0idx++] = p_Dpb->fs_ltref[i]->frame;
                }
            }
        }
        std::qsort(&currSlice->listX[0][(short) currSlice->listXsize[0]], list0idx - currSlice->listXsize[0],
                   sizeof(storable_picture*), [](const void *arg1, const void *arg2) {
            int long_term_pic_num1 = (*(storable_picture**)arg1)->LongTermPicNum;
            int long_term_pic_num2 = (*(storable_picture**)arg2)->LongTermPicNum;
            return (long_term_pic_num1 < long_term_pic_num2) ? -1 : (long_term_pic_num1 > long_term_pic_num2) ? 1 : 0;
        });
        std::qsort(&currSlice->listX[1][(short) currSlice->listXsize[0]], list0idx - currSlice->listXsize[0],
                   sizeof(storable_picture*), [](const void *arg1, const void *arg2) {
            int long_term_pic_num1 = (*(storable_picture**)arg1)->LongTermPicNum;
            int long_term_pic_num2 = (*(storable_picture**)arg2)->LongTermPicNum;
            return (long_term_pic_num1 < long_term_pic_num2) ? -1 : (long_term_pic_num1 > long_term_pic_num2) ? 1 : 0;
        });
        currSlice->listXsize[0] = currSlice->listXsize[1] = (char) list0idx;
    } else {
        frame_store **fs_list0  = new frame_store*[p_Dpb->size];
        frame_store **fs_list1  = new frame_store*[p_Dpb->size];
        frame_store **fs_listlt = new frame_store*[p_Dpb->size];

        currSlice->listXsize[0] = 0;
        currSlice->listXsize[1] = 1;

        for (i = 0; i < p_Dpb->ref_frames_in_buffer; i++) {
            if (p_Dpb->fs_ref[i]->is_used) {
                if (currSlice->PicOrderCnt >= p_Dpb->fs_ref[i]->poc)
                    fs_list0[list0idx++] = p_Dpb->fs_ref[i];
            }
        }
        std::qsort(fs_list0, list0idx, sizeof(frame_store*), [](const void *arg1, const void *arg2) {
            int poc1 = (*(frame_store**)arg1)->poc;
            int poc2 = (*(frame_store**)arg2)->poc;
            return (poc1 < poc2) ? 1 : (poc1 > poc2) ? -1 : 0;
        });

        list0idx_1 = list0idx;
        for (i = 0; i < p_Dpb->ref_frames_in_buffer; i++) {
            if (p_Dpb->fs_ref[i]->is_used) {
                if (currSlice->PicOrderCnt < p_Dpb->fs_ref[i]->poc)
                    fs_list0[list0idx++] = p_Dpb->fs_ref[i];
            }
        }
        std::qsort(&fs_list0[list0idx_1], list0idx-list0idx_1, sizeof(frame_store*), [](const void *arg1, const void *arg2) {
            int poc1 = (*(frame_store**)arg1)->poc;
            int poc2 = (*(frame_store**)arg2)->poc;
            return (poc1 < poc2) ? -1 : (poc1 > poc2) ? 1 : 0;
        });

        for (j = 0; j < list0idx_1; j++)
            fs_list1[list0idx - list0idx_1 + j] = fs_list0[j];
        for (j = list0idx_1; j < list0idx; j++)
            fs_list1[j - list0idx_1] = fs_list0[j];

        currSlice->listXsize[0] = 0;
        currSlice->listXsize[1] = 0;
        gen_pic_list_from_frame_list(currSlice->bottom_field_flag, fs_list0, list0idx, currSlice->listX[0], &currSlice->listXsize[0], 0);
        gen_pic_list_from_frame_list(currSlice->bottom_field_flag, fs_list1, list0idx, currSlice->listX[1], &currSlice->listXsize[1], 0);

        // long term handling
        for (i = 0; i < p_Dpb->ltref_frames_in_buffer; i++)
            fs_listlt[listltidx++] = p_Dpb->fs_ltref[i];

        std::qsort(fs_listlt, listltidx, sizeof(frame_store*), [](const void *arg1, const void *arg2) {
            int long_term_frame_idx1 = (*(frame_store**)arg1)->LongTermFrameIdx;
            int long_term_frame_idx2 = (*(frame_store**)arg2)->LongTermFrameIdx;
            return (long_term_frame_idx1 < long_term_frame_idx2) ? -1 : (long_term_frame_idx1 > long_term_frame_idx2) ? 1 : 0;
        });

        gen_pic_list_from_frame_list(currSlice->bottom_field_flag, fs_listlt, listltidx, currSlice->listX[0], &currSlice->listXsize[0], 1);
        gen_pic_list_from_frame_list(currSlice->bottom_field_flag, fs_listlt, listltidx, currSlice->listX[1], &currSlice->listXsize[1], 1);

        delete []fs_list0;
        delete []fs_list1;
        delete []fs_listlt;
    }

    if (currSlice->listXsize[0] == currSlice->listXsize[1] && currSlice->listXsize[0] > 1) {
        // check if lists are identical, if yes swap first two elements of currSlice->listX[1]
        int diff = 0;
        for (j = 0; j < currSlice->listXsize[0]; j++) {
            if (currSlice->listX[0][j] != currSlice->listX[1][j]) {
                diff = 1;
                break;
            }
        }
        if (!diff) {
            storable_picture *tmp_s = currSlice->listX[1][0];
            currSlice->listX[1][0]=currSlice->listX[1][1];
            currSlice->listX[1][1]=tmp_s;
        }
    }

    if (currSlice->svc_extension_flag == 0) {
        int curr_view_id = currSlice->view_id;
        // B-slice_t
        currSlice->fs_listinterview0 = new frame_store*[p_Dpb->size];
        currSlice->fs_listinterview1 = new frame_store*[p_Dpb->size];
        list0idx = currSlice->listXsize[0];

        if (!currSlice->field_pic_flag) {
            append_interview_list(
                p_Vid->p_Dpb_layer[1], false, false, 0,
                currSlice->fs_listinterview0, &currSlice->listinterviewidx0,
                currPOC, curr_view_id, anchor_pic_flag);
            append_interview_list(
                p_Vid->p_Dpb_layer[1], false, false, 1,
                currSlice->fs_listinterview1, &currSlice->listinterviewidx1,
                currPOC, curr_view_id, anchor_pic_flag);
            for (i = 0; i < (unsigned int)currSlice->listinterviewidx0; i++)
                currSlice->listX[0][list0idx++]=currSlice->fs_listinterview0[i]->frame;
            currSlice->listXsize[0] = (char) list0idx;
            list0idx = currSlice->listXsize[1];
            for (i = 0; i < (unsigned int)currSlice->listinterviewidx1; i++)
                currSlice->listX[1][list0idx++] = currSlice->fs_listinterview1[i]->frame;
            currSlice->listXsize[1] = (char) list0idx;
        } else {
            append_interview_list(
                p_Vid->p_Dpb_layer[1],
                currSlice->field_pic_flag, currSlice->bottom_field_flag, 0,
                currSlice->fs_listinterview0, &currSlice->listinterviewidx0,
                currPOC, curr_view_id, anchor_pic_flag);
            gen_pic_list_from_frame_interview_list(
                currSlice->bottom_field_flag, currSlice->fs_listinterview0, currSlice->listinterviewidx0,
                currSlice->listX[0], &currSlice->listXsize[0]);
            append_interview_list(
                p_Vid->p_Dpb_layer[1],
                currSlice->field_pic_flag, currSlice->bottom_field_flag, 1,
                currSlice->fs_listinterview1, &currSlice->listinterviewidx1,
                currPOC, curr_view_id, anchor_pic_flag);
            gen_pic_list_from_frame_interview_list(
                currSlice->bottom_field_flag, currSlice->fs_listinterview1, currSlice->listinterviewidx1,
                currSlice->listX[1], &currSlice->listXsize[1]);
        }    
    }

    // set max size
    currSlice->listXsize[0] = (char) min<int>(currSlice->listXsize[0], currSlice->num_ref_idx_l0_active_minus1 + 1);
    currSlice->listXsize[1] = (char) min<int>(currSlice->listXsize[1], currSlice->num_ref_idx_l1_active_minus1 + 1);

    // set the unused list entries to NULL
    for (i = currSlice->listXsize[0]; i < MAX_LIST_SIZE; i++)
        currSlice->listX[0][i] = p_Vid->no_reference_picture;
    for (i = currSlice->listXsize[1]; i < MAX_LIST_SIZE; i++)
        currSlice->listX[1][i] = p_Vid->no_reference_picture;
}

#endif


void init_lists(slice_t *currSlice)
{
#if (MVC_EXTENSION_ENABLE)
    if (currSlice->view_id) {
        switch (currSlice->slice_type) {
        case P_slice: 
        case SP_slice:
            init_lists_p_slice_mvc(currSlice);
            return;
        case B_slice:
            init_lists_b_slice_mvc(currSlice);
            return;
        case I_slice: 
        case SI_slice: 
            init_lists_i_slice_mvc(currSlice);
            return;
        default:
            printf("Unsupported slice type\n");
            break;
        }
    } else
#endif
    {
        switch (currSlice->slice_type) {
        case P_slice:
        case SP_slice:
            init_lists_p_slice(currSlice);
            return;
        case B_slice:
            init_lists_b_slice(currSlice);
            return;
        case I_slice:
        case SI_slice:
            init_lists_i_slice(currSlice);
            return;
        default:
            printf("Unsupported slice type\n");
            break;
        }
    }
}


static storable_picture *get_short_term_pic(slice_t *currSlice, dpb_t *p_Dpb, int picNum)
{
    unsigned i;

    for (i = 0; i < p_Dpb->ref_frames_in_buffer; i++) {
        if (!currSlice->field_pic_flag) {
            if (p_Dpb->fs_ref[i]->is_reference == 3)
                if (!p_Dpb->fs_ref[i]->frame->is_long_term && p_Dpb->fs_ref[i]->frame->PicNum == picNum)
                    return p_Dpb->fs_ref[i]->frame;
        } else {
            if (p_Dpb->fs_ref[i]->is_reference & 1)
                if (!p_Dpb->fs_ref[i]->top_field->is_long_term && p_Dpb->fs_ref[i]->top_field->PicNum == picNum)
                    return p_Dpb->fs_ref[i]->top_field;
            if (p_Dpb->fs_ref[i]->is_reference & 2)
                if (!p_Dpb->fs_ref[i]->bottom_field->is_long_term && p_Dpb->fs_ref[i]->bottom_field->PicNum == picNum)
                    return p_Dpb->fs_ref[i]->bottom_field;
        }
    }

    return currSlice->p_Vid->no_reference_picture;
}

static storable_picture *get_long_term_pic(slice_t *currSlice, dpb_t *p_Dpb, int LongtermPicNum)
{
    uint32_t i;

    for (i = 0; i < p_Dpb->ltref_frames_in_buffer; i++) {
        if (!currSlice->field_pic_flag) {
            if (p_Dpb->fs_ltref[i]->is_reference == 3)
                if (p_Dpb->fs_ltref[i]->frame->is_long_term && p_Dpb->fs_ltref[i]->frame->LongTermPicNum == LongtermPicNum)
                    return p_Dpb->fs_ltref[i]->frame;
        } else {
            if (p_Dpb->fs_ltref[i]->is_reference & 1)
                if (p_Dpb->fs_ltref[i]->top_field->is_long_term && p_Dpb->fs_ltref[i]->top_field->LongTermPicNum == LongtermPicNum)
                    return p_Dpb->fs_ltref[i]->top_field;
            if (p_Dpb->fs_ltref[i]->is_reference & 2)
                if (p_Dpb->fs_ltref[i]->bottom_field->is_long_term && p_Dpb->fs_ltref[i]->bottom_field->LongTermPicNum == LongtermPicNum)
                    return p_Dpb->fs_ltref[i]->bottom_field;
        }
    }

    return NULL;
}

#if (MVC_EXTENSION_ENABLE)
static void reorder_short_term(slice_t *currSlice, int cur_list, int num_ref_idx_lX_active_minus1, int picNumLX, int *refIdxLX, int currViewID)
{
    storable_picture **RefPicListX = currSlice->listX[cur_list]; 
    int cIdx, nIdx;

    for (cIdx = num_ref_idx_lX_active_minus1 + 1; cIdx > *refIdxLX; cIdx--)
        RefPicListX[cIdx] = RefPicListX[cIdx - 1];
    RefPicListX[(*refIdxLX)++] = get_short_term_pic(currSlice, currSlice->p_Dpb, picNumLX);
    nIdx = *refIdxLX;
    for (cIdx = *refIdxLX; cIdx <= num_ref_idx_lX_active_minus1 + 1; cIdx++) {
        if (RefPicListX[cIdx]) {
            if (RefPicListX[cIdx]->is_long_term ||
                RefPicListX[cIdx]->PicNum != picNumLX ||
                (currViewID != -1 && RefPicListX[cIdx]->slice.layer_id != currViewID))
                RefPicListX[nIdx++] = RefPicListX[cIdx];
        }
    }
}

static void reorder_long_term(slice_t *currSlice, storable_picture **RefPicListX, int num_ref_idx_lX_active_minus1, int LongTermPicNum, int *refIdxLX, int currViewID)
{
    int cIdx, nIdx;

    for (cIdx = num_ref_idx_lX_active_minus1 + 1; cIdx > *refIdxLX; cIdx--)
        RefPicListX[cIdx] = RefPicListX[cIdx - 1];
    RefPicListX[(*refIdxLX)++] = get_long_term_pic(currSlice, currSlice->p_Dpb, LongTermPicNum);
    nIdx = *refIdxLX;
    for (cIdx = *refIdxLX; cIdx <= num_ref_idx_lX_active_minus1 + 1; cIdx++) {
        if (RefPicListX[cIdx]) {
            if (!RefPicListX[cIdx]->is_long_term ||
                 RefPicListX[cIdx]->LongTermPicNum != LongTermPicNum ||
                (currViewID != -1 && RefPicListX[cIdx]->slice.layer_id != currViewID))
                RefPicListX[nIdx++] = RefPicListX[cIdx];
        }
    }
}
#endif

static void reorder_ref_pic_list(slice_t *currSlice, int cur_list)
{
    uint8_t  *modification_of_pic_nums_idc = currSlice->modification_of_pic_nums_idc[cur_list];
    uint32_t *abs_diff_pic_num_minus1      = currSlice->abs_diff_pic_num_minus1     [cur_list];
    uint32_t *long_term_pic_num            = currSlice->long_term_pic_num           [cur_list];

    int num_ref_idx_lX_active_minus1 = 
        cur_list == 0 ? currSlice->num_ref_idx_l0_active_minus1
                      : currSlice->num_ref_idx_l1_active_minus1;

    int picNumLXNoWrap, picNumLXPred, picNumLX;
    int refIdxLX = 0;
    int i;

    picNumLXPred = currSlice->CurrPicNum;

    for (i = 0; modification_of_pic_nums_idc[i] != 3; i++) {
        if (modification_of_pic_nums_idc[i] > 3)
            error ("Invalid modification_of_pic_nums_idc command", 500);

        if (modification_of_pic_nums_idc[i] < 2) {
            if (modification_of_pic_nums_idc[i] == 0) {
                if (picNumLXPred < abs_diff_pic_num_minus1[i] + 1)
                    picNumLXNoWrap = picNumLXPred - (abs_diff_pic_num_minus1[i] + 1) + currSlice->MaxPicNum;
                else
                    picNumLXNoWrap = picNumLXPred - (abs_diff_pic_num_minus1[i] + 1);
            } else {
                if (picNumLXPred + (abs_diff_pic_num_minus1[i] + 1) >= currSlice->MaxPicNum)
                    picNumLXNoWrap = picNumLXPred + (abs_diff_pic_num_minus1[i] + 1) - currSlice->MaxPicNum;
                else
                    picNumLXNoWrap = picNumLXPred + (abs_diff_pic_num_minus1[i] + 1);
            }
            picNumLXPred = picNumLXNoWrap;

            if (picNumLXNoWrap > currSlice->CurrPicNum)
                picNumLX = picNumLXNoWrap - currSlice->MaxPicNum;
            else
                picNumLX = picNumLXNoWrap;

#if (MVC_EXTENSION_ENABLE)
            reorder_short_term(currSlice, cur_list, num_ref_idx_lX_active_minus1, picNumLX, &refIdxLX, -1);
#endif
        } else {
#if (MVC_EXTENSION_ENABLE)
            reorder_long_term(currSlice, currSlice->listX[cur_list], num_ref_idx_lX_active_minus1, long_term_pic_num[i], &refIdxLX, -1);
#endif
        }
    }
    // that's a definition
    currSlice->listXsize[cur_list] = num_ref_idx_lX_active_minus1 + 1;
}

static void reorder_lists(slice_t *currSlice)
{
    VideoParameters *p_Vid = currSlice->p_Vid;

    if (currSlice->slice_type != I_slice && currSlice->slice_type != SI_slice) {
        if (currSlice->ref_pic_list_modification_flag_l0)
            reorder_ref_pic_list(currSlice, LIST_0);
        if (p_Vid->no_reference_picture == currSlice->listX[0][currSlice->num_ref_idx_l0_active_minus1]) {
            if (p_Vid->non_conforming_stream)
                printf("RefPicList0[ %d ] is equal to 'no reference picture'\n", currSlice->num_ref_idx_l0_active_minus1);
            else
                error("RefPicList0[ num_ref_idx_l0_active_minus1 ] is equal to 'no reference picture', invalid bitstream",500);
        }
        // that's a definition
        currSlice->listXsize[0] = (char) currSlice->num_ref_idx_l0_active_minus1 + 1;
    }

    if (currSlice->slice_type == B_slice) {
        if (currSlice->ref_pic_list_modification_flag_l1)
            reorder_ref_pic_list(currSlice, LIST_1);
        if (p_Vid->no_reference_picture == currSlice->listX[1][currSlice->num_ref_idx_l1_active_minus1]) {
            if (p_Vid->non_conforming_stream)
                printf("RefPicList1[ %d ] is equal to 'no reference picture'\n", currSlice->num_ref_idx_l1_active_minus1);
            else
                error("RefPicList1[ num_ref_idx_l1_active_minus1 ] is equal to 'no reference picture', invalid bitstream",500);
        }
        // that's a definition
        currSlice->listXsize[1] = (char) currSlice->num_ref_idx_l1_active_minus1 + 1;
    }
}


#if (MVC_EXTENSION_ENABLE)

static int GetViewIdx(VideoParameters *p_Vid, int iVOIdx)
{
    int iViewIdx = -1;
    int *piViewIdMap;

    if (p_Vid->active_subset_sps) {
        assert(p_Vid->active_subset_sps->num_views_minus1 >= iVOIdx && iVOIdx >= 0);
        piViewIdMap = p_Vid->active_subset_sps->view_id;
        iViewIdx = piViewIdMap[iVOIdx];    
    }

    return iViewIdx;
}

static int get_maxViewIdx(VideoParameters *p_Vid, int view_id, int anchor_pic_flag, int listidx)
{
    int maxViewIdx = 0;
    int VOIdx = view_id; 

    if (VOIdx >= 0) {
        if (anchor_pic_flag)
            maxViewIdx = listidx? p_Vid->active_subset_sps->num_anchor_refs_l1[VOIdx] : p_Vid->active_subset_sps->num_anchor_refs_l0[VOIdx];
        else
            maxViewIdx = listidx? p_Vid->active_subset_sps->num_non_anchor_refs_l1[VOIdx] : p_Vid->active_subset_sps->num_non_anchor_refs_l0[VOIdx];
    }

    return maxViewIdx;
}

static storable_picture* get_inter_view_pic(VideoParameters *p_Vid, slice_t *currSlice, int targetViewID, int currPOC, int listidx)
{
    unsigned int listinterview_size;
    frame_store **fs_listinterview;

    if (listidx == 0) {
        fs_listinterview   = currSlice->fs_listinterview0;
        listinterview_size = currSlice->listinterviewidx0; 
    } else {
        fs_listinterview   = currSlice->fs_listinterview1;
        listinterview_size = currSlice->listinterviewidx1; 
    }

    for (int i = 0; i < listinterview_size; i++) {
        if (fs_listinterview[i]->layer_id == GetVOIdx(p_Vid, targetViewID)) {
            if (!currSlice->field_pic_flag && fs_listinterview[i]->frame->poc == currPOC)
                return fs_listinterview[i]->frame;
            else if (currSlice->field_pic_flag && !currSlice->bottom_field_flag && fs_listinterview[i]->top_field->poc == currPOC)
                return fs_listinterview[i]->top_field;
            else if (currSlice->field_pic_flag && currSlice->bottom_field_flag && fs_listinterview[i]->bottom_field->poc == currPOC)
                return fs_listinterview[i]->bottom_field;
        }
    }

    return NULL;
}

static void reorder_interview(VideoParameters *p_Vid, slice_t *currSlice, storable_picture **RefPicListX, int num_ref_idx_lX_active_minus1, int *refIdxLX, int targetViewID, int currPOC, int listidx)
{
    storable_picture* picLX = get_inter_view_pic(p_Vid, currSlice, targetViewID, currPOC, listidx);

    if (picLX) {
        for (int cIdx = num_ref_idx_lX_active_minus1 + 1; cIdx > *refIdxLX; cIdx--)
            RefPicListX[cIdx] = RefPicListX[cIdx - 1];
        RefPicListX[(*refIdxLX)++] = picLX;
        int nIdx = *refIdxLX;

        for (int cIdx = *refIdxLX; cIdx <= num_ref_idx_lX_active_minus1 + 1; cIdx++) {
            if ((GetViewIdx(p_Vid, RefPicListX[cIdx]->slice.view_id) != targetViewID) || (RefPicListX[cIdx]->poc != currPOC))
                RefPicListX[nIdx++] = RefPicListX[cIdx];
        }
    }
}

static void reorder_ref_pic_list_mvc(slice_t *currSlice, int cur_list, int **anchor_ref, int **non_anchor_ref, int view_id, int anchor_pic_flag, int currPOC, int listidx)
{
    VideoParameters *p_Vid = currSlice->p_Vid;
    uint8_t  *modification_of_pic_nums_idc = currSlice->modification_of_pic_nums_idc[cur_list];
    uint32_t *abs_diff_pic_num_minus1      = currSlice->abs_diff_pic_num_minus1[cur_list];
    uint32_t *long_term_pic_num            = currSlice->long_term_pic_num[cur_list];
    int num_ref_idx_lX_active_minus1 = cur_list == 0 ?
        currSlice->num_ref_idx_l0_active_minus1 : currSlice->num_ref_idx_l1_active_minus1;
    uint32_t *abs_diff_view_idx_minus1 = currSlice->abs_diff_view_idx_minus1[cur_list];

    int maxPicNum, currPicNum, picNumLXNoWrap, picNumLXPred, picNumLX;
    int picViewIdxLX, targetViewID;
    int refIdxLX   = 0;
    int maxViewIdx = 0;
    int curr_VOIdx = -1;
    int picViewIdxLXPred = -1;

    if (!currSlice->field_pic_flag) {
        maxPicNum  = p_Vid->active_sps->MaxFrameNum;
        currPicNum = currSlice->frame_num;
    } else {
        maxPicNum  = 2 * p_Vid->active_sps->MaxFrameNum;
        currPicNum = 2 * currSlice->frame_num + 1;
    }

    if (currSlice->svc_extension_flag == 0) {
        curr_VOIdx = view_id;
        maxViewIdx = get_maxViewIdx(p_Vid, view_id, anchor_pic_flag, 0);
        picViewIdxLXPred=-1;
    }

    picNumLXPred = currPicNum;

    for (int i = 0; modification_of_pic_nums_idc[i] != 3; i++) {
        if (modification_of_pic_nums_idc[i] > 5)
            error("Invalid modification_of_pic_nums_idc command", 500);

        if (modification_of_pic_nums_idc[i] < 2) {
            if (modification_of_pic_nums_idc[i] == 0) {
                if (picNumLXPred < abs_diff_pic_num_minus1[i] + 1)
                    picNumLXNoWrap = picNumLXPred - (abs_diff_pic_num_minus1[i] + 1) + maxPicNum;
                else
                    picNumLXNoWrap = picNumLXPred - (abs_diff_pic_num_minus1[i] + 1);
            } else {
                if (picNumLXPred + abs_diff_pic_num_minus1[i] + 1 >= maxPicNum)
                    picNumLXNoWrap = picNumLXPred + (abs_diff_pic_num_minus1[i] + 1) - maxPicNum;
                else
                    picNumLXNoWrap = picNumLXPred + (abs_diff_pic_num_minus1[i] + 1);
            }
            picNumLXPred = picNumLXNoWrap;

            if (picNumLXNoWrap > currPicNum)
                picNumLX = picNumLXNoWrap - maxPicNum;
            else
                picNumLX = picNumLXNoWrap;

            reorder_short_term(currSlice, cur_list, num_ref_idx_lX_active_minus1, picNumLX, &refIdxLX, view_id);
        } else if (modification_of_pic_nums_idc[i] == 2)
            reorder_long_term(currSlice, currSlice->listX[cur_list], num_ref_idx_lX_active_minus1, long_term_pic_num[i], &refIdxLX, view_id);
        else {
            if (modification_of_pic_nums_idc[i] == 4) {
                picViewIdxLX = picViewIdxLXPred - (abs_diff_view_idx_minus1[i] + 1);
                if (picViewIdxLX < 0)
                    picViewIdxLX += maxViewIdx;
            } else {
                picViewIdxLX = picViewIdxLXPred + (abs_diff_view_idx_minus1[i] + 1);
                if (picViewIdxLX >= maxViewIdx)
                    picViewIdxLX -= maxViewIdx;
            }
            picViewIdxLXPred = picViewIdxLX;

            if (anchor_pic_flag)
                targetViewID = anchor_ref[curr_VOIdx][picViewIdxLX];
            else
                targetViewID = non_anchor_ref[curr_VOIdx][picViewIdxLX];

            reorder_interview(p_Vid, currSlice, currSlice->listX[cur_list], num_ref_idx_lX_active_minus1, &refIdxLX, targetViewID, currPOC, listidx);
        }
    }
    // that's a definition
    currSlice->listXsize[cur_list] = (char) (num_ref_idx_lX_active_minus1 + 1);
}

static void reorder_lists_mvc(slice_t* currSlice, int currPOC)
{
    VideoParameters* p_Vid = currSlice->p_Vid;

    if (currSlice->slice_type != I_slice && currSlice->slice_type != SI_slice) {
        if (currSlice->ref_pic_list_modification_flag_l0) {
            reorder_ref_pic_list_mvc(currSlice, LIST_0,
                p_Vid->active_subset_sps->anchor_ref_l0,
                p_Vid->active_subset_sps->non_anchor_ref_l0,
                currSlice->view_id, currSlice->anchor_pic_flag, currPOC, 0);
        }
        if (p_Vid->no_reference_picture == currSlice->listX[0][currSlice->num_ref_idx_l0_active_minus1]) {
            if (p_Vid->non_conforming_stream)
                printf("RefPicList0[ %d ] is equal to 'no reference picture'\n", currSlice->num_ref_idx_l0_active_minus1);
            else
                error("RefPicList0[ num_ref_idx_l0_active_minus1 ] in MVC layer is equal to 'no reference picture', invalid bitstream", 500);
        }
        // that's a definition
        currSlice->listXsize[0] = (char)currSlice->num_ref_idx_l0_active_minus1 + 1;
    }
    if (currSlice->slice_type == B_slice) {
        if (currSlice->ref_pic_list_modification_flag_l1) {
            reorder_ref_pic_list_mvc(currSlice, LIST_1,
                p_Vid->active_subset_sps->anchor_ref_l1,
                p_Vid->active_subset_sps->non_anchor_ref_l1,
                currSlice->view_id, currSlice->anchor_pic_flag, currPOC, 1);
        }
        if (p_Vid->no_reference_picture == currSlice->listX[1][currSlice->num_ref_idx_l1_active_minus1]) {
            if (p_Vid->non_conforming_stream)
                printf("RefPicList1[ %d ] is equal to 'no reference picture'\n", currSlice->num_ref_idx_l1_active_minus1);
            else
                error("RefPicList1[ num_ref_idx_l1_active_minus1 ] is equal to 'no reference picture', invalid bitstream", 500);
        }
        // that's a definition
        currSlice->listXsize[1] = (char)currSlice->num_ref_idx_l1_active_minus1 + 1;
    }
}

#endif

/*!
 ************************************************************************
 * \brief
 *    Initialize listX[2..5] from lists 0 and 1
 *    listX[2]: list0 for current_field==top
 *    listX[3]: list1 for current_field==top
 *    listX[4]: list0 for current_field==bottom
 *    listX[5]: list1 for current_field==bottom
 *
 ************************************************************************
 */
static void init_mbaff_lists(slice_t* currSlice)
{
    VideoParameters* p_Vid = currSlice->p_Vid;

    for (int i = 2; i < 6; i++) {
        for (int j = 0; j < MAX_LIST_SIZE; j++)
            currSlice->listX[i][j] = p_Vid->no_reference_picture;
        currSlice->listXsize[i] = 0;
    }

    for (int i = 0; i < currSlice->listXsize[0]; i++) {
        currSlice->listX[2][2 * i    ] = currSlice->listX[0][i]->top_field;
        currSlice->listX[2][2 * i + 1] = currSlice->listX[0][i]->bottom_field;
        currSlice->listX[4][2 * i    ] = currSlice->listX[0][i]->bottom_field;
        currSlice->listX[4][2 * i + 1] = currSlice->listX[0][i]->top_field;
    }
    currSlice->listXsize[2] = currSlice->listXsize[4] = currSlice->listXsize[0] * 2;

    for (int i = 0; i < currSlice->listXsize[1]; i++) {
        currSlice->listX[3][2 * i    ] = currSlice->listX[1][i]->top_field;
        currSlice->listX[3][2 * i + 1] = currSlice->listX[1][i]->bottom_field;
        currSlice->listX[5][2 * i    ] = currSlice->listX[1][i]->bottom_field;
        currSlice->listX[5][2 * i + 1] = currSlice->listX[1][i]->top_field;
    }
    currSlice->listXsize[3] = currSlice->listXsize[5] = currSlice->listXsize[1] * 2;
}


void init_ref_lists(slice_t* slice)
{
    VideoParameters* p_Vid = slice->p_Vid;

    init_lists(slice);

#if (MVC_EXTENSION_ENABLE)
    if (slice->svc_extension_flag == 0 || slice->svc_extension_flag == 1)
        reorder_lists_mvc(slice, slice->PicOrderCnt);
    else
        reorder_lists(slice);

    if (slice->fs_listinterview0) {
        delete []slice->fs_listinterview0;
        slice->fs_listinterview0 = nullptr;
    }
    if (slice->fs_listinterview1) {
        delete []slice->fs_listinterview1;
        slice->fs_listinterview1 = nullptr;
    }
#endif

    if (!slice->field_pic_flag)
        init_mbaff_lists(slice);

    // update reference flags and set current p_Vid->ref_flag
    if (!(slice->redundant_pic_cnt != 0 && p_Vid->previous_frame_num == slice->frame_num)) {
        for (int i = 16; i > 0; i--)
            slice->ref_flag[i] = slice->ref_flag[i-1];
    }
    slice->ref_flag[0] = slice->redundant_pic_cnt == 0 ? p_Vid->Is_primary_correct : p_Vid->Is_redundant_correct;
}
