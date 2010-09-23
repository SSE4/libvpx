/*
 *  Copyright (c) 2010 The VP8 project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license and patent
 *  grant that can be found in the LICENSE file in the root of the source
 *  tree. All contributing project authors may be found in the AUTHORS
 *  file in the root of the source tree.
 */


#include "vpx_ports/config.h"
#include "encodemb.h"
#include "encodemv.h"
#include "common.h"
#include "onyx_int.h"
#include "extend.h"
#include "entropymode.h"
#include "quant_common.h"
#include "segmentation_common.h"
#include "setupintrarecon.h"
#include "encodeintra.h"
#include "reconinter.h"
#include "rdopt.h"
#include "pickinter.h"
#include "findnearmv.h"
#include "reconintra.h"
#include <stdio.h>
#include <limits.h>
#include "subpixel.h"
#include "vpx_ports/vpx_timer.h"


#if CONFIG_RUNTIME_CPU_DETECT
#define RTCD(x)     &cpi->common.rtcd.x
#define IF_RTCD(x)  (x)
#else
#define RTCD(x)     NULL
#define IF_RTCD(x)  NULL
#endif

#if CONFIG_SEGMENTATION
#define SEEK_SEGID 12
#define SEEK_SAMEID 4
#define SEEK_DIFFID 7
#endif

extern void vp8_stuff_mb(VP8_COMP *cpi, MACROBLOCKD *x, TOKENEXTRA **t) ;

extern void vp8cx_initialize_me_consts(VP8_COMP *cpi, int QIndex);
extern void vp8_auto_select_speed(VP8_COMP *cpi);
extern void vp8cx_init_mbrthread_data(VP8_COMP *cpi,
                                      MACROBLOCK *x,
                                      MB_ROW_COMP *mbr_ei,
                                      int mb_row,
                                      int count);
void vp8_build_block_offsets(MACROBLOCK *x);
void vp8_setup_block_ptrs(MACROBLOCK *x);
int vp8cx_encode_inter_macroblock(VP8_COMP *cpi, MACROBLOCK *x, TOKENEXTRA **t, int recon_yoffset, int recon_uvoffset);
int vp8cx_encode_intra_macro_block(VP8_COMP *cpi, MACROBLOCK *x, TOKENEXTRA **t);

#ifdef MODE_STATS
unsigned int inter_y_modes[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
unsigned int inter_uv_modes[4] = {0, 0, 0, 0};
unsigned int inter_b_modes[15]  = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
unsigned int y_modes[5]   = {0, 0, 0, 0, 0};
unsigned int uv_modes[4]  = {0, 0, 0, 0};
unsigned int b_modes[14]  = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
#endif

// The first four entries are dummy values
static const int qrounding_factors[129] =
{
    56, 56, 56, 56, 56, 56, 56, 56,
    48, 48, 48, 48, 48, 48, 48, 48,
    48, 48, 48, 48, 48, 48, 48, 48,
    48, 48, 48, 48, 48, 48, 48, 48,
    48, 48, 48, 48, 48, 48, 48, 48,
    48, 48, 48, 48, 48, 48, 48, 48,
    48, 48, 48, 48, 48, 48, 48, 48,
    48, 48, 48, 48, 48, 48, 48, 48,
    48, 48, 48, 48, 48, 48, 48, 48,
    48, 48, 48, 48, 48, 48, 48, 48,
    48, 48, 48, 48, 48, 48, 48, 48,
    48, 48, 48, 48, 48, 48, 48, 48,
    48, 48, 48, 48, 48, 48, 48, 48,
    48, 48, 48, 48, 48, 48, 48, 48,
    48, 48, 48, 48, 48, 48, 48, 48,
    48, 48, 48, 48, 48, 48, 48, 48,
    48,
};

static const int qzbin_factors[129] =
{
    64, 64, 64, 64, 80, 80, 80, 80,
    80, 80, 80, 80, 80, 80, 80, 80,
    80, 80, 80, 80, 80, 80, 80, 80,
    80, 80, 80, 80, 80, 80, 80, 80,
    80, 80, 80, 80, 80, 80, 80, 80,
    80, 80, 80, 80, 80, 80, 80, 80,
    80, 80, 80, 80, 80, 80, 80, 80,
    80, 80, 80, 80, 80, 80, 80, 80,
    80, 80, 80, 80, 80, 80, 80, 80,
    80, 80, 80, 80, 80, 80, 80, 80,
    80, 80, 80, 80, 80, 80, 80, 80,
    80, 80, 80, 80, 80, 80, 80, 80,
    80, 80, 80, 80, 80, 80, 80, 80,
    80, 80, 80, 80, 80, 80, 80, 80,
    80, 80, 80, 80, 80, 80, 80, 80,
    80, 80, 80, 80, 80, 80, 80, 80,
    80,
};

void vp8cx_init_quantizer(VP8_COMP *cpi)
{
    int r, c;
    int i;
    int quant_val;
    int Q;

    int zbin_boost[16] = {0, 0, 8, 10, 12, 14, 16, 20, 24, 28, 32, 36, 40, 44, 44, 44};

    for (Q = 0; Q < QINDEX_RANGE; Q++)
    {
        // dc values
        quant_val = vp8_dc_quant(Q, cpi->common.y1dc_delta_q);
        cpi->Y1quant[Q][0][0] = (1 << 16) / quant_val;
        cpi->Y1zbin[Q][0][0] = ((qzbin_factors[Q] * quant_val) + 64) >> 7;
        cpi->Y1round[Q][0][0] = (qrounding_factors[Q] * quant_val) >> 7;
        cpi->common.Y1dequant[Q][0][0] = quant_val;
        cpi->zrun_zbin_boost_y1[Q][0] = (quant_val * zbin_boost[0]) >> 7;

        quant_val = vp8_dc2quant(Q, cpi->common.y2dc_delta_q);
        cpi->Y2quant[Q][0][0] = (1 << 16) / quant_val;
        cpi->Y2zbin[Q][0][0] = ((qzbin_factors[Q] * quant_val) + 64) >> 7;
        cpi->Y2round[Q][0][0] = (qrounding_factors[Q] * quant_val) >> 7;
        cpi->common.Y2dequant[Q][0][0] = quant_val;
        cpi->zrun_zbin_boost_y2[Q][0] = (quant_val * zbin_boost[0]) >> 7;

        quant_val = vp8_dc_uv_quant(Q, cpi->common.uvdc_delta_q);
        cpi->UVquant[Q][0][0] = (1 << 16) / quant_val;
        cpi->UVzbin[Q][0][0] = ((qzbin_factors[Q] * quant_val) + 64) >> 7;;
        cpi->UVround[Q][0][0] = (qrounding_factors[Q] * quant_val) >> 7;
        cpi->common.UVdequant[Q][0][0] = quant_val;
        cpi->zrun_zbin_boost_uv[Q][0] = (quant_val * zbin_boost[0]) >> 7;

        // all the ac values = ;
        for (i = 1; i < 16; i++)
        {
            int rc = vp8_default_zig_zag1d[i];
            r = (rc >> 2);
            c = (rc & 3);

            quant_val = vp8_ac_yquant(Q);
            cpi->Y1quant[Q][r][c] = (1 << 16) / quant_val;
            cpi->Y1zbin[Q][r][c] = ((qzbin_factors[Q] * quant_val) + 64) >> 7;
            cpi->Y1round[Q][r][c] = (qrounding_factors[Q] * quant_val) >> 7;
            cpi->common.Y1dequant[Q][r][c] = quant_val;
            cpi->zrun_zbin_boost_y1[Q][i] = (quant_val * zbin_boost[i]) >> 7;

            quant_val = vp8_ac2quant(Q, cpi->common.y2ac_delta_q);
            cpi->Y2quant[Q][r][c] = (1 << 16) / quant_val;
            cpi->Y2zbin[Q][r][c] = ((qzbin_factors[Q] * quant_val) + 64) >> 7;
            cpi->Y2round[Q][r][c] = (qrounding_factors[Q] * quant_val) >> 7;
            cpi->common.Y2dequant[Q][r][c] = quant_val;
            cpi->zrun_zbin_boost_y2[Q][i] = (quant_val * zbin_boost[i]) >> 7;

            quant_val = vp8_ac_uv_quant(Q, cpi->common.uvac_delta_q);
            cpi->UVquant[Q][r][c] = (1 << 16) / quant_val;
            cpi->UVzbin[Q][r][c] = ((qzbin_factors[Q] * quant_val) + 64) >> 7;
            cpi->UVround[Q][r][c] = (qrounding_factors[Q] * quant_val) >> 7;
            cpi->common.UVdequant[Q][r][c] = quant_val;
            cpi->zrun_zbin_boost_uv[Q][i] = (quant_val * zbin_boost[i]) >> 7;
        }
    }
}

void vp8cx_mb_init_quantizer(VP8_COMP *cpi, MACROBLOCK *x)
{
    int i;
    int QIndex;
    MACROBLOCKD *xd = &x->e_mbd;
    MB_MODE_INFO *mbmi = &xd->mbmi;
    int zbin_extra;

    // Select the baseline MB Q index.
    if (xd->segmentation_enabled)
    {
        // Abs Value
        if (xd->mb_segement_abs_delta == SEGMENT_ABSDATA)
            QIndex = xd->segment_feature_data[MB_LVL_ALT_Q][mbmi->segment_id];

        // Delta Value
        else
        {
            QIndex = cpi->common.base_qindex + xd->segment_feature_data[MB_LVL_ALT_Q][mbmi->segment_id];
            QIndex = (QIndex >= 0) ? ((QIndex <= MAXQ) ? QIndex : MAXQ) : 0;    // Clamp to valid range
        }
    }
    else
        QIndex = cpi->common.base_qindex;

    // Y
    zbin_extra = (cpi->common.Y1dequant[QIndex][0][1] * (cpi->zbin_over_quant + cpi->zbin_mode_boost)) >> 7;

    for (i = 0; i < 16; i++)
    {
        x->block[i].quant = cpi->Y1quant[QIndex];
        x->block[i].zbin = cpi->Y1zbin[QIndex];
        x->block[i].round = cpi->Y1round[QIndex];
        x->e_mbd.block[i].dequant = cpi->common.Y1dequant[QIndex];
        x->block[i].zrun_zbin_boost = cpi->zrun_zbin_boost_y1[QIndex];
        x->block[i].zbin_extra = (short)zbin_extra;
    }

    // UV
    zbin_extra = (cpi->common.UVdequant[QIndex][0][1] * (cpi->zbin_over_quant + cpi->zbin_mode_boost)) >> 7;

    for (i = 16; i < 24; i++)
    {
        x->block[i].quant = cpi->UVquant[QIndex];
        x->block[i].zbin = cpi->UVzbin[QIndex];
        x->block[i].round = cpi->UVround[QIndex];
        x->e_mbd.block[i].dequant = cpi->common.UVdequant[QIndex];
        x->block[i].zrun_zbin_boost = cpi->zrun_zbin_boost_uv[QIndex];
        x->block[i].zbin_extra = (short)zbin_extra;
    }

    // Y2
    zbin_extra = (cpi->common.Y2dequant[QIndex][0][1] * ((cpi->zbin_over_quant / 2) + cpi->zbin_mode_boost)) >> 7;
    x->block[24].quant = cpi->Y2quant[QIndex];
    x->block[24].zbin = cpi->Y2zbin[QIndex];
    x->block[24].round = cpi->Y2round[QIndex];
    x->e_mbd.block[24].dequant = cpi->common.Y2dequant[QIndex];
    x->block[24].zrun_zbin_boost = cpi->zrun_zbin_boost_y2[QIndex];
    x->block[24].zbin_extra = (short)zbin_extra;
}

void vp8cx_frame_init_quantizer(VP8_COMP *cpi)
{
    // vp8cx_init_quantizer() is first called in vp8_create_compressor(). A check is added here so that vp8cx_init_quantizer() is only called
    // when these values are not all zero.
    if (cpi->common.y1dc_delta_q | cpi->common.y2dc_delta_q | cpi->common.uvdc_delta_q | cpi->common.y2ac_delta_q | cpi->common.uvac_delta_q)
    {
        vp8cx_init_quantizer(cpi);
    }

    // MB level quantizer setup
    vp8cx_mb_init_quantizer(cpi, &cpi->mb);
}



static
void encode_mb_row(VP8_COMP *cpi,
                   VP8_COMMON *cm,
                   int mb_row,
                   MACROBLOCK  *x,
                   MACROBLOCKD *xd,
                   TOKENEXTRA **tp,
                   int *segment_counts,
                   int *totalrate)
{
    int i;
    int recon_yoffset, recon_uvoffset;
    int mb_col;
    int recon_y_stride = cm->last_frame.y_stride;
    int recon_uv_stride = cm->last_frame.uv_stride;
    int seg_map_index = (mb_row * cpi->common.mb_cols);
#if CONFIG_SEGMENTATION
    int left_id, above_id;
    int sum;
#endif
    // reset above block coeffs
    xd->above_context[Y1CONTEXT] = cm->above_context[Y1CONTEXT];
    xd->above_context[UCONTEXT ] = cm->above_context[UCONTEXT ];
    xd->above_context[VCONTEXT ] = cm->above_context[VCONTEXT ];
    xd->above_context[Y2CONTEXT] = cm->above_context[Y2CONTEXT];

    xd->up_available = (mb_row != 0);
    recon_yoffset = (mb_row * recon_y_stride * 16);
    recon_uvoffset = (mb_row * recon_uv_stride * 8);

    cpi->tplist[mb_row].start = *tp;
    //printf("Main mb_row = %d\n", mb_row);

    // for each macroblock col in image
    for (mb_col = 0; mb_col < cm->mb_cols; mb_col++)
    {
        // Distance of Mb to the various image edges.
        // These specified to 8th pel as they are always compared to values that are in 1/8th pel units
        xd->mb_to_left_edge = -((mb_col * 16) << 3);
        xd->mb_to_right_edge = ((cm->mb_cols - 1 - mb_col) * 16) << 3;
        xd->mb_to_top_edge = -((mb_row * 16) << 3);
        xd->mb_to_bottom_edge = ((cm->mb_rows - 1 - mb_row) * 16) << 3;

        // Set up limit values for motion vectors used to prevent them extending outside the UMV borders
        x->mv_col_min = -((mb_col * 16) + (VP8BORDERINPIXELS - 16));
        x->mv_col_max = ((cm->mb_cols - 1 - mb_col) * 16) + (VP8BORDERINPIXELS - 16);
        x->mv_row_min = -((mb_row * 16) + (VP8BORDERINPIXELS - 16));
        x->mv_row_max = ((cm->mb_rows - 1 - mb_row) * 16) + (VP8BORDERINPIXELS - 16);

        xd->dst.y_buffer = cm->new_frame.y_buffer + recon_yoffset;
        xd->dst.u_buffer = cm->new_frame.u_buffer + recon_uvoffset;
        xd->dst.v_buffer = cm->new_frame.v_buffer + recon_uvoffset;
        xd->left_available = (mb_col != 0);

        // Is segmentation enabled
        // MB level adjutment to quantizer
        if (xd->segmentation_enabled)
        {
            // Code to set segment id in xd->mbmi.segment_id for current MB (with range checking)
            if (cpi->segmentation_map[seg_map_index+mb_col] <= 3)
                xd->mbmi.segment_id = cpi->segmentation_map[seg_map_index+mb_col];
            else
                xd->mbmi.segment_id = 0;

            vp8cx_mb_init_quantizer(cpi, x);

        }
        else
            xd->mbmi.segment_id = 0;         // Set to Segment 0 by default

        x->active_ptr = cpi->active_map + seg_map_index + mb_col;

        if (cm->frame_type == KEY_FRAME)
        {
            *totalrate += vp8cx_encode_intra_macro_block(cpi, x, tp);
#ifdef MODE_STATS
            y_modes[xd->mbmi.mode] ++;
#endif
        }
        else
        {
            *totalrate += vp8cx_encode_inter_macroblock(cpi, x, tp, recon_yoffset, recon_uvoffset);

#ifdef MODE_STATS
            inter_y_modes[xd->mbmi.mode] ++;

            if (xd->mbmi.mode == SPLITMV)
            {
                int b;

                for (b = 0; b < xd->mbmi.partition_count; b++)
                {
                    inter_b_modes[xd->mbmi.partition_bmi[b].mode] ++;
                }
            }

#endif

            // Count of last ref frame 0,0 useage
            if ((xd->mbmi.mode == ZEROMV) && (xd->mbmi.ref_frame == LAST_FRAME))
                cpi->inter_zz_count ++;

            // Special case code for cyclic refresh
            // If cyclic update enabled then copy xd->mbmi.segment_id; (which may have been updated based on mode
            // during vp8cx_encode_inter_macroblock()) back into the global sgmentation map
            if (cpi->cyclic_refresh_mode_enabled && xd->segmentation_enabled)
            {
                cpi->segmentation_map[seg_map_index+mb_col] = xd->mbmi.segment_id;

                // If the block has been refreshed mark it as clean (the magnitude of the -ve influences how long it will be before we consider another refresh):
                // Else if it was coded (last frame 0,0) and has not already been refreshed then mark it as a candidate for cleanup next time (marked 0)
                // else mark it as dirty (1).
                if (xd->mbmi.segment_id)
                    cpi->cyclic_refresh_map[seg_map_index+mb_col] = -1;
                else if ((xd->mbmi.mode == ZEROMV) && (xd->mbmi.ref_frame == LAST_FRAME))
                {
                    if (cpi->cyclic_refresh_map[seg_map_index+mb_col] == 1)
                        cpi->cyclic_refresh_map[seg_map_index+mb_col] = 0;
                }
                else
                    cpi->cyclic_refresh_map[seg_map_index+mb_col] = 1;

            }
        }

        cpi->tplist[mb_row].stop = *tp;

        xd->gf_active_ptr++;      // Increment pointer into gf useage flags structure for next mb

        if ((xd->mbmi.mode == ZEROMV) && (xd->mbmi.ref_frame == LAST_FRAME))
            xd->mbmi.segment_id = 0;
        else
            xd->mbmi.segment_id = 1;

        // store macroblock mode info into context array
        vpx_memcpy(&xd->mode_info_context->mbmi, &xd->mbmi, sizeof(xd->mbmi));

        for (i = 0; i < 16; i++)
            vpx_memcpy(&xd->mode_info_context->bmi[i], &xd->block[i].bmi, sizeof(xd->block[i].bmi));

        // adjust to the next column of macroblocks
        x->src.y_buffer += 16;
        x->src.u_buffer += 8;
        x->src.v_buffer += 8;

        recon_yoffset += 16;
        recon_uvoffset += 8;

#if CONFIG_SEGMENTATION
       //cpi->segmentation_map[mb_row * cm->mb_cols + mb_col] =  xd->mbmi.segment_id;
        if (cm->frame_type == KEY_FRAME)
        {
            segment_counts[xd->mode_info_context->mbmi.segment_id] ++;
        }
        else
        {
            sum = 0;
            if (mb_col != 0)
                sum += (xd->mode_info_context-1)->mbmi.segment_flag;
            if (mb_row != 0)
                sum += (xd->mode_info_context-cm->mb_cols)->mbmi.segment_flag;

            if (xd->mbmi.segment_id == cpi->segmentation_map[(mb_row*cm->mb_cols) + mb_col])
                xd->mode_info_context->mbmi.segment_flag = 0;
            else
                xd->mode_info_context->mbmi.segment_flag = 1;

            if (xd->mode_info_context->mbmi.segment_flag == 0)
            {
                segment_counts[SEEK_SAMEID + sum]++;
                segment_counts[10]++;
            }
            else
            {
                segment_counts[SEEK_DIFFID + sum]++;
                segment_counts[11]++;
                //calculate individual segment ids
                segment_counts[xd->mode_info_context->mbmi.segment_id] ++;
            }
        }
        segment_counts[SEEK_SEGID + xd->mbmi.segment_id] ++;
#else
        segment_counts[xd->mode_info_context->mbmi.segment_id] ++;
#endif
        // skip to next mb
        xd->mode_info_context++;

        xd->above_context[Y1CONTEXT] += 4;
        xd->above_context[UCONTEXT ] += 2;
        xd->above_context[VCONTEXT ] += 2;
        xd->above_context[Y2CONTEXT] ++;
        cpi->current_mb_col_main = mb_col;
    }

    //extend the recon for intra prediction
    vp8_extend_mb_row(
        &cm->new_frame,
        xd->dst.y_buffer + 16,
        xd->dst.u_buffer + 8,
        xd->dst.v_buffer + 8);

    // this is to account for the border
    xd->mode_info_context++;
}

void vp8_encode_frame(VP8_COMP *cpi)
{
    int mb_row;
    MACROBLOCK *const x = & cpi->mb;
    VP8_COMMON *const cm = & cpi->common;
    MACROBLOCKD *const xd = & x->e_mbd;

    int i;
    TOKENEXTRA *tp = cpi->tok;
#if CONFIG_SEGMENTATION
    int segment_counts[MAX_MB_SEGMENTS + SEEK_SEGID];
    int prob[3];
    int new_cost, original_cost;
#else
    int segment_counts[MAX_MB_SEGMENTS];
#endif
    int totalrate;

    if (cm->frame_type != KEY_FRAME)
    {
        if (cm->mcomp_filter_type == SIXTAP)
        {
            xd->subpixel_predict     = SUBPIX_INVOKE(&cpi->common.rtcd.subpix, sixtap4x4);
            xd->subpixel_predict8x4      = SUBPIX_INVOKE(&cpi->common.rtcd.subpix, sixtap8x4);
            xd->subpixel_predict8x8      = SUBPIX_INVOKE(&cpi->common.rtcd.subpix, sixtap8x8);
            xd->subpixel_predict16x16    = SUBPIX_INVOKE(&cpi->common.rtcd.subpix, sixtap16x16);
        }
        else
        {
            xd->subpixel_predict     = SUBPIX_INVOKE(&cpi->common.rtcd.subpix, bilinear4x4);
            xd->subpixel_predict8x4      = SUBPIX_INVOKE(&cpi->common.rtcd.subpix, bilinear8x4);
            xd->subpixel_predict8x8      = SUBPIX_INVOKE(&cpi->common.rtcd.subpix, bilinear8x8);
            xd->subpixel_predict16x16    = SUBPIX_INVOKE(&cpi->common.rtcd.subpix, bilinear16x16);
        }
    }

    //else  // Key Frame
    //{
    // For key frames make sure the intra ref frame probability value
    // is set to "all intra"
    //cpi->prob_intra_coded = 255;
    //}

    xd->gf_active_ptr = (signed char *)cm->gf_active_flags;     // Point to base of GF active flags data structure

    x->vector_range = 32;

    // Count of MBs using the alternate Q if any
    cpi->alt_qcount = 0;

    // Reset frame count of inter 0,0 motion vector useage.
    cpi->inter_zz_count = 0;

    vpx_memset(segment_counts, 0, sizeof(segment_counts));

    cpi->prediction_error = 0;
    cpi->intra_error = 0;
    cpi->skip_true_count = 0;
    cpi->skip_false_count = 0;

#if 0
    // Experimental code
    cpi->frame_distortion = 0;
    cpi->last_mb_distortion = 0;
#endif

    totalrate = 0;

    xd->mode_info = cm->mi - 1;

    xd->mode_info_context = cm->mi;
    xd->mode_info_stride = cm->mode_info_stride;

    xd->frame_type = cm->frame_type;

    xd->frames_since_golden = cm->frames_since_golden;
    xd->frames_till_alt_ref_frame = cm->frames_till_alt_ref_frame;
    vp8_zero(cpi->MVcount);
    // vp8_zero( Contexts)
    vp8_zero(cpi->coef_counts);

    // reset intra mode contexts
    if (cm->frame_type == KEY_FRAME)
        vp8_init_mbmode_probs(cm);


    vp8cx_frame_init_quantizer(cpi);

    if (cpi->compressor_speed == 2)
    {
        if (cpi->oxcf.cpu_used < 0)
            cpi->Speed = -(cpi->oxcf.cpu_used);
        else
            vp8_auto_select_speed(cpi);
    }

    vp8_initialize_rd_consts(cpi, vp8_dc_quant(cm->base_qindex, cm->y1dc_delta_q));
    //vp8_initialize_rd_consts( cpi, vp8_dc_quant(cpi->avg_frame_qindex, cm->y1dc_delta_q) );
    vp8cx_initialize_me_consts(cpi, cm->base_qindex);
    //vp8cx_initialize_me_consts( cpi, cpi->avg_frame_qindex);

    // Copy data over into macro block data sturctures.

    x->src = * cpi->Source;
    xd->pre = cm->last_frame;
    xd->dst = cm->new_frame;

    // set up frame new frame for intra coded blocks

    vp8_setup_intra_recon(&cm->new_frame);

    vp8_build_block_offsets(x);

    vp8_setup_block_dptrs(&x->e_mbd);

    vp8_setup_block_ptrs(x);

    x->rddiv = cpi->RDDIV;
    x->rdmult = cpi->RDMULT;

#if 0
    // Experimental rd code
    // 2 Pass - Possibly set Rdmult based on last frame distortion + this frame target bits or other metrics
    // such as cpi->rate_correction_factor that indicate relative complexity.
    /*if ( cpi->pass == 2 && (cpi->last_frame_distortion > 0) && (cpi->target_bits_per_mb > 0) )
    {
        //x->rdmult = ((cpi->last_frame_distortion * 256)/cpi->common.MBs)/ cpi->target_bits_per_mb;
        x->rdmult = (int)(cpi->RDMULT * cpi->rate_correction_factor);
    }
    else
        x->rdmult = cpi->RDMULT; */
    //x->rdmult = (int)(cpi->RDMULT * pow( (cpi->rate_correction_factor * 2.0), 0.75 ));
#endif

    xd->mbmi.mode = DC_PRED;
    xd->mbmi.uv_mode = DC_PRED;

    xd->left_context = cm->left_context;

    vp8_zero(cpi->count_mb_ref_frame_usage)
    vp8_zero(cpi->ymode_count)
    vp8_zero(cpi->uv_mode_count)

    x->mvc = cm->fc.mvc;

    // vp8_zero( entropy_stats)
    {
        ENTROPY_CONTEXT **p = cm->above_context;
        const size_t L = cm->mb_cols;

        vp8_zero_array(p [Y1CONTEXT], L * 4)
        vp8_zero_array(p [ UCONTEXT], L * 2)
        vp8_zero_array(p [ VCONTEXT], L * 2)
        vp8_zero_array(p [Y2CONTEXT], L)
    }


    {
        struct vpx_usec_timer  emr_timer;
        vpx_usec_timer_start(&emr_timer);

        if (!cpi->b_multi_threaded)
        {
            // for each macroblock row in image
            for (mb_row = 0; mb_row < cm->mb_rows; mb_row++)
            {

                vp8_zero(cm->left_context)

                encode_mb_row(cpi, cm, mb_row, x, xd, &tp, segment_counts, &totalrate);

                // adjust to the next row of mbs
                x->src.y_buffer += 16 * x->src.y_stride - 16 * cm->mb_cols;
                x->src.u_buffer += 8 * x->src.uv_stride - 8 * cm->mb_cols;
                x->src.v_buffer += 8 * x->src.uv_stride - 8 * cm->mb_cols;
            }

            cpi->tok_count = tp - cpi->tok;

        }
        else
        {
#if CONFIG_MULTITHREAD
            vp8cx_init_mbrthread_data(cpi, x, cpi->mb_row_ei, 1,  cpi->encoding_thread_count);

            for (mb_row = 0; mb_row < cm->mb_rows; mb_row += (cpi->encoding_thread_count + 1))
            {
                int i;
                cpi->current_mb_col_main = -1;

                for (i = 0; i < cpi->encoding_thread_count; i++)
                {
                    if ((mb_row + i + 1) >= cm->mb_rows)
                        break;

                    cpi->mb_row_ei[i].mb_row = mb_row + i + 1;
                    cpi->mb_row_ei[i].tp  = cpi->tok + (mb_row + i + 1) * (cm->mb_cols * 16 * 24);
                    cpi->mb_row_ei[i].current_mb_col = -1;
                    //SetEvent(cpi->h_event_mbrencoding[i]);
                    sem_post(&cpi->h_event_mbrencoding[i]);
                }

                vp8_zero(cm->left_context)

                tp = cpi->tok + mb_row * (cm->mb_cols * 16 * 24);

                encode_mb_row(cpi, cm, mb_row, x, xd, &tp, segment_counts, &totalrate);

                // adjust to the next row of mbs
                x->src.y_buffer += 16 * x->src.y_stride * (cpi->encoding_thread_count + 1) - 16 * cm->mb_cols;
                x->src.u_buffer +=  8 * x->src.uv_stride * (cpi->encoding_thread_count + 1) - 8 * cm->mb_cols;
                x->src.v_buffer +=  8 * x->src.uv_stride * (cpi->encoding_thread_count + 1) - 8 * cm->mb_cols;

                xd->mode_info_context += xd->mode_info_stride * cpi->encoding_thread_count;

                if (mb_row < cm->mb_rows - 1)
                    //WaitForSingleObject(cpi->h_event_main, INFINITE);
                    sem_wait(&cpi->h_event_main);
            }

            /*
            for( ;mb_row<cm->mb_rows; mb_row ++)
            {
            vp8_zero( cm->left_context)

            tp = cpi->tok + mb_row * (cm->mb_cols * 16 * 24);

            encode_mb_row(cpi, cm, mb_row, x, xd, &tp, segment_counts, &totalrate);
            // adjust to the next row of mbs
            x->src.y_buffer += 16 * x->src.y_stride - 16 * cm->mb_cols;
            x->src.u_buffer +=  8 * x->src.uv_stride - 8 * cm->mb_cols;
            x->src.v_buffer +=  8 * x->src.uv_stride - 8 * cm->mb_cols;

            }
            */
            cpi->tok_count = 0;

            for (mb_row = 0; mb_row < cm->mb_rows; mb_row ++)
            {
                cpi->tok_count += cpi->tplist[mb_row].stop - cpi->tplist[mb_row].start;
            }

            if (xd->segmentation_enabled)
            {

                int i, j;

                if (xd->segmentation_enabled)
                {

                    for (i = 0; i < cpi->encoding_thread_count; i++)
                    {
                        for (j = 0; j < 4; j++)
                            segment_counts[j] += cpi->mb_row_ei[i].segment_counts[j];
                    }
                }

            }

            for (i = 0; i < cpi->encoding_thread_count; i++)
            {
                totalrate += cpi->mb_row_ei[i].totalrate;
            }

#endif

        }

        vpx_usec_timer_mark(&emr_timer);
        cpi->time_encode_mb_row += vpx_usec_timer_elapsed(&emr_timer);

    }

    // Work out the segment probabilites if segmentation is enabled
    if (xd->segmentation_enabled)
    {
        int tot_count;
        int i,j;
        int count1,count2,count3,count4;

        // Set to defaults
        vpx_memset(xd->mb_segment_tree_probs, 255 , sizeof(xd->mb_segment_tree_probs));
#if CONFIG_SEGMENTATION

        tot_count = segment_counts[12] + segment_counts[13] + segment_counts[14] + segment_counts[15];
        count1 = segment_counts[12] + segment_counts[13];
        count2 = segment_counts[14] + segment_counts[15];

        if (tot_count)
            prob[0] = (count1 * 255) / tot_count;

        if (count1 > 0)
            prob[1] = (segment_counts[12] * 255) /count1;

        if (count2 > 0)
            prob[2] = (segment_counts[14] * 255) /count2;

        if (cm->frame_type != KEY_FRAME)
        {
            tot_count = segment_counts[4] + segment_counts[7];
            if (tot_count)
                xd->mb_segment_tree_probs[3] = (segment_counts[4] * 255)/tot_count;

            tot_count = segment_counts[5] + segment_counts[8];
            if (tot_count)
                xd->mb_segment_tree_probs[4] = (segment_counts[5] * 255)/tot_count;

            tot_count = segment_counts[6] + segment_counts[9];
            if (tot_count)
                xd->mb_segment_tree_probs[5] = (segment_counts[6] * 255)/tot_count;
        }

        tot_count = segment_counts[0] + segment_counts[1] + segment_counts[2] + segment_counts[3];
        count3 = segment_counts[0] + segment_counts[1];
        count4 = segment_counts[2] + segment_counts[3];

        if (tot_count)
            xd->mb_segment_tree_probs[0] = (count3 * 255) / tot_count;

        if (count3 > 0)
            xd->mb_segment_tree_probs[1] = (segment_counts[0] * 255) /count3;

        if (count4 > 0)
            xd->mb_segment_tree_probs[2] = (segment_counts[2] * 255) /count4;

        for (i = 0; i < MB_FEATURE_TREE_PROBS+3; i++)
        {
            if (xd->mb_segment_tree_probs[i] == 0)
                xd->mb_segment_tree_probs[i] = 1;
        }

        original_cost = count1 * vp8_cost_zero(prob[0]) + count2 * vp8_cost_one(prob[0]);

        if (count1 > 0)
            original_cost += segment_counts[12] * vp8_cost_zero(prob[1]) + segment_counts[13] * vp8_cost_one(prob[1]);

        if (count2 > 0)
            original_cost += segment_counts[14] * vp8_cost_zero(prob[2]) + segment_counts[15] * vp8_cost_one(prob[2]) ;

        new_cost = 0;

        if (cm->frame_type != KEY_FRAME)
        {
            new_cost = segment_counts[4] * vp8_cost_zero(xd->mb_segment_tree_probs[3]) + segment_counts[7] *  vp8_cost_one(xd->mb_segment_tree_probs[3]);

            new_cost += segment_counts[5] * vp8_cost_zero(xd->mb_segment_tree_probs[4]) + segment_counts[8] * vp8_cost_one(xd->mb_segment_tree_probs[4]);

            new_cost += segment_counts[6] * vp8_cost_zero(xd->mb_segment_tree_probs[5]) + segment_counts[9] * vp8_cost_one (xd->mb_segment_tree_probs[5]);
        }

        if (tot_count > 0)
            new_cost += count3 * vp8_cost_zero(xd->mb_segment_tree_probs[0]) + count4 * vp8_cost_one(xd->mb_segment_tree_probs[0]);

        if (count3 > 0)
            new_cost += segment_counts[0] * vp8_cost_zero(xd->mb_segment_tree_probs[1]) + segment_counts[1] * vp8_cost_one(xd->mb_segment_tree_probs[1]);

        if (count4 > 0)
            new_cost += segment_counts[2] * vp8_cost_zero(xd->mb_segment_tree_probs[2]) + segment_counts[3] * vp8_cost_one(xd->mb_segment_tree_probs[2]) ;

        if (new_cost < original_cost)
            xd->temporal_update = 1;
        else
        {
            xd->temporal_update = 0;
            xd->mb_segment_tree_probs[0] = prob[0];
            xd->mb_segment_tree_probs[1] = prob[1];
            xd->mb_segment_tree_probs[2] = prob[2];
        }
#else
        tot_count = segment_counts[0] + segment_counts[1] + segment_counts[2] + segment_counts[3];
        count1 = segment_counts[0] + segment_counts[1];
        count2 = segment_counts[2] + segment_counts[3];

        if (tot_count)
            xd->mb_segment_tree_probs[0] = (count1 * 255) / tot_count;

        if (count1 > 0)
            xd->mb_segment_tree_probs[1] = (segment_counts[0] * 255) /count1;

        if (count2 > 0)
            xd->mb_segment_tree_probs[2] = (segment_counts[2] * 255) /count2;

#endif
        // Zero probabilities not allowed
#if CONFIG_SEGMENTATION
            for (i = 0; i < MB_FEATURE_TREE_PROBS+3; i++)
#else
            for (i = 0; i < MB_FEATURE_TREE_PROBS; i++)
#endif
            {
                if (xd->mb_segment_tree_probs[i] == 0)
                    xd->mb_segment_tree_probs[i] = 1;
            }
    }

    // 256 rate units to the bit
    cpi->projected_frame_size = totalrate >> 8;   // projected_frame_size in units of BYTES

    // Make a note of the percentage MBs coded Intra.
    if (cm->frame_type == KEY_FRAME)
    {
        cpi->this_frame_percent_intra = 100;
    }
    else
    {
        int tot_modes;

        tot_modes = cpi->count_mb_ref_frame_usage[INTRA_FRAME]
                    + cpi->count_mb_ref_frame_usage[LAST_FRAME]
                    + cpi->count_mb_ref_frame_usage[GOLDEN_FRAME]
                    + cpi->count_mb_ref_frame_usage[ALTREF_FRAME];

        if (tot_modes)
            cpi->this_frame_percent_intra = cpi->count_mb_ref_frame_usage[INTRA_FRAME] * 100 / tot_modes;

    }

#if 0
    {
        int cnt = 0;
        int flag[2] = {0, 0};

        for (cnt = 0; cnt < MVPcount; cnt++)
        {
            if (cm->fc.pre_mvc[0][cnt] != cm->fc.mvc[0][cnt])
            {
                flag[0] = 1;
                vpx_memcpy(cm->fc.pre_mvc[0], cm->fc.mvc[0], MVPcount);
                break;
            }
        }

        for (cnt = 0; cnt < MVPcount; cnt++)
        {
            if (cm->fc.pre_mvc[1][cnt] != cm->fc.mvc[1][cnt])
            {
                flag[1] = 1;
                vpx_memcpy(cm->fc.pre_mvc[1], cm->fc.mvc[1], MVPcount);
                break;
            }
        }

        if (flag[0] || flag[1])
            vp8_build_component_cost_table(cpi->mb.mvcost, cpi->mb.mvsadcost, (const MV_CONTEXT *) cm->fc.mvc, flag);
    }
#endif

    // Adjust the projected reference frame useage probability numbers to reflect
    // what we have just seen. This may be usefull when we make multiple itterations
    // of the recode loop rather than continuing to use values from the previous frame.
    if ((cm->frame_type != KEY_FRAME) && !cm->refresh_alt_ref_frame && !cm->refresh_golden_frame)
    {
        const int *const rfct = cpi->count_mb_ref_frame_usage;
        const int rf_intra = rfct[INTRA_FRAME];
        const int rf_inter = rfct[LAST_FRAME] + rfct[GOLDEN_FRAME] + rfct[ALTREF_FRAME];

        if ((rf_intra + rf_inter) > 0)
        {
            cpi->prob_intra_coded = (rf_intra * 255) / (rf_intra + rf_inter);

            if (cpi->prob_intra_coded < 1)
                cpi->prob_intra_coded = 1;

            if ((cm->frames_since_golden > 0) || cpi->source_alt_ref_active)
            {
                cpi->prob_last_coded = rf_inter ? (rfct[LAST_FRAME] * 255) / rf_inter : 128;

                if (cpi->prob_last_coded < 1)
                    cpi->prob_last_coded = 1;

                cpi->prob_gf_coded = (rfct[GOLDEN_FRAME] + rfct[ALTREF_FRAME])
                                     ? (rfct[GOLDEN_FRAME] * 255) / (rfct[GOLDEN_FRAME] + rfct[ALTREF_FRAME]) : 128;

                if (cpi->prob_gf_coded < 1)
                    cpi->prob_gf_coded = 1;
            }
        }
    }

#if 0
    // Keep record of the total distortion this time around for future use
    cpi->last_frame_distortion = cpi->frame_distortion;
#endif

}
void vp8_setup_block_ptrs(MACROBLOCK *x)
{
    int r, c;
    int i;

    for (r = 0; r < 4; r++)
    {
        for (c = 0; c < 4; c++)
        {
            x->block[r*4+c].src_diff = x->src_diff + r * 4 * 16 + c * 4;
        }
    }

    for (r = 0; r < 2; r++)
    {
        for (c = 0; c < 2; c++)
        {
            x->block[16 + r*2+c].src_diff = x->src_diff + 256 + r * 4 * 8 + c * 4;
        }
    }


    for (r = 0; r < 2; r++)
    {
        for (c = 0; c < 2; c++)
        {
            x->block[20 + r*2+c].src_diff = x->src_diff + 320 + r * 4 * 8 + c * 4;
        }
    }

    x->block[24].src_diff = x->src_diff + 384;


    for (i = 0; i < 25; i++)
    {
        x->block[i].coeff = x->coeff + i * 16;
    }
}

void vp8_build_block_offsets(MACROBLOCK *x)
{
    int block = 0;
    int br, bc;

    vp8_build_block_doffsets(&x->e_mbd);

    // y blocks
    for (br = 0; br < 4; br++)
    {
        for (bc = 0; bc < 4; bc++)
        {
            BLOCK *this_block = &x->block[block];
            this_block->base_src = &x->src.y_buffer;
            this_block->src_stride = x->src.y_stride;
            this_block->src = 4 * br * this_block->src_stride + 4 * bc;
            ++block;
        }
    }

    // u blocks
    for (br = 0; br < 2; br++)
    {
        for (bc = 0; bc < 2; bc++)
        {
            BLOCK *this_block = &x->block[block];
            this_block->base_src = &x->src.u_buffer;
            this_block->src_stride = x->src.uv_stride;
            this_block->src = 4 * br * this_block->src_stride + 4 * bc;
            ++block;
        }
    }

    // v blocks
    for (br = 0; br < 2; br++)
    {
        for (bc = 0; bc < 2; bc++)
        {
            BLOCK *this_block = &x->block[block];
            this_block->base_src = &x->src.v_buffer;
            this_block->src_stride = x->src.uv_stride;
            this_block->src = 4 * br * this_block->src_stride + 4 * bc;
            ++block;
        }
    }
}

static void sum_intra_stats(VP8_COMP *cpi, MACROBLOCK *x)
{
    const MACROBLOCKD *xd = & x->e_mbd;
    const MB_PREDICTION_MODE m = xd->mbmi.mode;
    const MB_PREDICTION_MODE uvm = xd->mbmi.uv_mode;

#ifdef MODE_STATS
    const int is_key = cpi->common.frame_type == KEY_FRAME;

    ++ (is_key ? uv_modes : inter_uv_modes)[uvm];

    if (m == B_PRED)
    {
        unsigned int *const bct = is_key ? b_modes : inter_b_modes;

        int b = 0;

        do
        {
            ++ bct[xd->block[b].bmi.mode];
        }
        while (++b < 16);
    }

#endif

    ++cpi->ymode_count[m];
    ++cpi->uv_mode_count[uvm];

}
int vp8cx_encode_intra_macro_block(VP8_COMP *cpi, MACROBLOCK *x, TOKENEXTRA **t)
{
    int Error4x4, Error16x16, error_uv;
    B_PREDICTION_MODE intra_bmodes[16];
    int rate4x4, rate16x16, rateuv;
    int dist4x4, dist16x16, distuv;
    int rate = 0;
    int rate4x4_tokenonly = 0;
    int rate16x16_tokenonly = 0;
    int rateuv_tokenonly = 0;
    int i;

    x->e_mbd.mbmi.ref_frame = INTRA_FRAME;

#if !(CONFIG_REALTIME_ONLY)

    if (cpi->sf.RD || cpi->compressor_speed != 2)
    {
        Error4x4 = vp8_rd_pick_intra4x4mby_modes(cpi, x, &rate4x4, &rate4x4_tokenonly, &dist4x4);

        //save the b modes for possible later use
        for (i = 0; i < 16; i++)
            intra_bmodes[i] = x->e_mbd.block[i].bmi.mode;

        Error16x16 = vp8_rd_pick_intra16x16mby_mode(cpi, x, &rate16x16, &rate16x16_tokenonly, &dist16x16);

        error_uv = vp8_rd_pick_intra_mbuv_mode(cpi, x, &rateuv, &rateuv_tokenonly, &distuv);

        x->e_mbd.mbmi.mb_skip_coeff = (cpi->common.mb_no_coeff_skip) ? 1 : 0;

        vp8_encode_intra16x16mbuv(IF_RTCD(&cpi->rtcd), x);
        rate += rateuv;

        if (Error4x4 < Error16x16)
        {
            rate += rate4x4;
            x->e_mbd.mbmi.mode = B_PRED;

            // get back the intra block modes
            for (i = 0; i < 16; i++)
                x->e_mbd.block[i].bmi.mode = intra_bmodes[i];

            vp8_encode_intra4x4mby(IF_RTCD(&cpi->rtcd), x);
            cpi->prediction_error += Error4x4 ;
#if 0
            // Experimental RD code
            cpi->frame_distortion += dist4x4;
#endif
        }
        else
        {
            vp8_encode_intra16x16mby(IF_RTCD(&cpi->rtcd), x);
            rate += rate16x16;

#if 0
            // Experimental RD code
            cpi->prediction_error += Error16x16;
            cpi->frame_distortion += dist16x16;
#endif
        }

        sum_intra_stats(cpi, x);

        vp8_tokenize_mb(cpi, &x->e_mbd, t);
    }
    else
#endif
    {

        int rate2, distortion2;
        MB_PREDICTION_MODE mode, best_mode = DC_PRED;
        int this_rd;
        Error16x16 = INT_MAX;

        for (mode = DC_PRED; mode <= TM_PRED; mode ++)
        {
            x->e_mbd.mbmi.mode = mode;
            vp8_build_intra_predictors_mby_ptr(&x->e_mbd);
            distortion2 = VARIANCE_INVOKE(&cpi->rtcd.variance, get16x16prederror)(x->src.y_buffer, x->src.y_stride, x->e_mbd.predictor, 16, 0x7fffffff);
            rate2  = x->mbmode_cost[x->e_mbd.frame_type][mode];
            this_rd = RD_ESTIMATE(x->rdmult, x->rddiv, rate2, distortion2);

            if (Error16x16 > this_rd)
            {
                Error16x16 = this_rd;
                best_mode = mode;
            }
        }

        vp8_pick_intra4x4mby_modes(IF_RTCD(&cpi->rtcd), x, &rate2, &distortion2);

        if (distortion2 == INT_MAX)
            Error4x4 = INT_MAX;
        else
            Error4x4 = RD_ESTIMATE(x->rdmult, x->rddiv, rate2, distortion2);

        x->e_mbd.mbmi.mb_skip_coeff = (cpi->common.mb_no_coeff_skip) ? 1 : 0;

        if (Error4x4 < Error16x16)
        {
            x->e_mbd.mbmi.mode = B_PRED;
            vp8_encode_intra4x4mby(IF_RTCD(&cpi->rtcd), x);
            cpi->prediction_error += Error4x4;
        }
        else
        {
            x->e_mbd.mbmi.mode = best_mode;
            vp8_encode_intra16x16mby(IF_RTCD(&cpi->rtcd), x);
            cpi->prediction_error += Error16x16;
        }

        vp8_pick_intra_mbuv_mode(x);
        vp8_encode_intra16x16mbuv(IF_RTCD(&cpi->rtcd), x);
        sum_intra_stats(cpi, x);
        vp8_tokenize_mb(cpi, &x->e_mbd, t);
    }

    return rate;
}
#ifdef SPEEDSTATS
extern int cnt_pm;
#endif

extern void vp8_fix_contexts(VP8_COMP *cpi, MACROBLOCKD *x);

int vp8cx_encode_inter_macroblock
(
    VP8_COMP *cpi, MACROBLOCK *x, TOKENEXTRA **t,
    int recon_yoffset, int recon_uvoffset
)
{
    MACROBLOCKD *const xd = &x->e_mbd;
    int inter_error;
    int intra_error = 0;
    int rate;
    int distortion;

    x->skip = 0;

    if (xd->segmentation_enabled)
        x->encode_breakout = cpi->segment_encode_breakout[xd->mbmi.segment_id];
    else
        x->encode_breakout = cpi->oxcf.encode_breakout;

#if !(CONFIG_REALTIME_ONLY)

    if (cpi->sf.RD)
    {
        inter_error = vp8_rd_pick_inter_mode(cpi, x, recon_yoffset, recon_uvoffset, &rate, &distortion, &intra_error);
    }
    else
#endif
        inter_error = vp8_pick_inter_mode(cpi, x, recon_yoffset, recon_uvoffset, &rate, &distortion, &intra_error);


    cpi->prediction_error += inter_error;
    cpi->intra_error += intra_error;

#if 0
    // Experimental RD code
    cpi->frame_distortion += distortion;
    cpi->last_mb_distortion = distortion;
#endif

    // MB level adjutment to quantizer setup
    if (xd->segmentation_enabled || cpi->zbin_mode_boost_enabled)
    {
        // If cyclic update enabled
        if (cpi->cyclic_refresh_mode_enabled)
        {
            // Clear segment_id back to 0 if not coded (last frame 0,0)
            if ((xd->mbmi.segment_id == 1) &&
                ((xd->mbmi.ref_frame != LAST_FRAME) || (xd->mbmi.mode != ZEROMV)))
            {
                xd->mbmi.segment_id = 0;
            }
        }

        // Experimental code. Special case for gf and arf zeromv modes. Increase zbin size to supress noise
        if (cpi->zbin_mode_boost_enabled)
        {
            if ((xd->mbmi.mode == ZEROMV) && (xd->mbmi.ref_frame != LAST_FRAME))
                cpi->zbin_mode_boost = GF_ZEROMV_ZBIN_BOOST;
            else
                cpi->zbin_mode_boost = 0;
        }

        vp8cx_mb_init_quantizer(cpi,  x);
    }

    cpi->count_mb_ref_frame_usage[xd->mbmi.ref_frame] ++;

    if (xd->mbmi.ref_frame == INTRA_FRAME)
    {
        x->e_mbd.mbmi.mb_skip_coeff = (cpi->common.mb_no_coeff_skip) ? 1 : 0;

        vp8_encode_intra16x16mbuv(IF_RTCD(&cpi->rtcd), x);

        if (xd->mbmi.mode == B_PRED)
        {
            vp8_encode_intra4x4mby(IF_RTCD(&cpi->rtcd), x);
        }
        else
        {
            vp8_encode_intra16x16mby(IF_RTCD(&cpi->rtcd), x);
        }

        sum_intra_stats(cpi, x);
    }
    else
    {
        MV best_ref_mv;
        MV nearest, nearby;
        int mdcounts[4];

        vp8_find_near_mvs(xd, xd->mode_info_context,
                          &nearest, &nearby, &best_ref_mv, mdcounts, xd->mbmi.ref_frame, cpi->common.ref_frame_sign_bias);

        vp8_build_uvmvs(xd, cpi->common.full_pixel);

        // store motion vectors in our motion vector list
        if (xd->mbmi.ref_frame == LAST_FRAME)
        {
            // Set up pointers for this macro block into the previous frame recon buffer
            xd->pre.y_buffer = cpi->common.last_frame.y_buffer + recon_yoffset;
            xd->pre.u_buffer = cpi->common.last_frame.u_buffer + recon_uvoffset;
            xd->pre.v_buffer = cpi->common.last_frame.v_buffer + recon_uvoffset;
        }
        else if (xd->mbmi.ref_frame == GOLDEN_FRAME)
        {
            // Set up pointers for this macro block into the golden frame recon buffer
            xd->pre.y_buffer = cpi->common.golden_frame.y_buffer + recon_yoffset;
            xd->pre.u_buffer = cpi->common.golden_frame.u_buffer + recon_uvoffset;
            xd->pre.v_buffer = cpi->common.golden_frame.v_buffer + recon_uvoffset;
        }
        else
        {
            // Set up pointers for this macro block into the alternate reference frame recon buffer
            xd->pre.y_buffer = cpi->common.alt_ref_frame.y_buffer + recon_yoffset;
            xd->pre.u_buffer = cpi->common.alt_ref_frame.u_buffer + recon_uvoffset;
            xd->pre.v_buffer = cpi->common.alt_ref_frame.v_buffer + recon_uvoffset;
        }

        if (xd->mbmi.mode == SPLITMV)
        {
            int i;

            for (i = 0; i < 16; i++)
            {
                if (xd->block[i].bmi.mode == NEW4X4)
                {
                    cpi->MVcount[0][mv_max+((xd->block[i].bmi.mv.as_mv.row - best_ref_mv.row) >> 1)]++;
                    cpi->MVcount[1][mv_max+((xd->block[i].bmi.mv.as_mv.col - best_ref_mv.col) >> 1)]++;
                }
            }
        }
        else if (xd->mbmi.mode == NEWMV)
        {
            cpi->MVcount[0][mv_max+((xd->block[0].bmi.mv.as_mv.row - best_ref_mv.row) >> 1)]++;
            cpi->MVcount[1][mv_max+((xd->block[0].bmi.mv.as_mv.col - best_ref_mv.col) >> 1)]++;
        }

        if (!x->skip && !x->e_mbd.mbmi.force_no_skip)
        {
            vp8_encode_inter16x16(IF_RTCD(&cpi->rtcd), x);

            // Clear mb_skip_coeff if mb_no_coeff_skip is not set
            if (!cpi->common.mb_no_coeff_skip)
                xd->mbmi.mb_skip_coeff = 0;

        }
        else
            vp8_stuff_inter16x16(x);
    }

    if (!x->skip)
        vp8_tokenize_mb(cpi, xd, t);
    else
    {
        if (cpi->common.mb_no_coeff_skip)
        {
            if (xd->mbmi.mode != B_PRED && xd->mbmi.mode != SPLITMV)
                xd->mbmi.dc_diff = 0;
            else
                xd->mbmi.dc_diff = 1;

            xd->mbmi.mb_skip_coeff = 1;
            cpi->skip_true_count ++;
            vp8_fix_contexts(cpi, xd);
        }
        else
        {
            vp8_stuff_mb(cpi, xd, t);
            xd->mbmi.mb_skip_coeff = 0;
            cpi->skip_false_count ++;
        }
    }

    return rate;
}
