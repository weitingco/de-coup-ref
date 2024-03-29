/*
 * Copyright (c) 2016, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#include <limits.h>

#include "av1/encoder/encoder.h"
#include "av1/encoder/speed_features.h"
#include "av1/encoder/rdopt.h"

#include "aom_dsp/aom_dsp_common.h"

#define MAX_MESH_SPEED 5  // Max speed setting for mesh motion method
// Max speed setting for tx domain evaluation
#define MAX_TX_DOMAIN_EVAL_SPEED 5
static MESH_PATTERN
    good_quality_mesh_patterns[MAX_MESH_SPEED + 1][MAX_MESH_STEP] = {
      { { 64, 8 }, { 28, 4 }, { 15, 1 }, { 7, 1 } },
      { { 64, 8 }, { 28, 4 }, { 15, 1 }, { 7, 1 } },
      { { 64, 8 }, { 14, 2 }, { 7, 1 }, { 7, 1 } },
      { { 64, 16 }, { 24, 8 }, { 12, 4 }, { 7, 1 } },
      { { 64, 16 }, { 24, 8 }, { 12, 4 }, { 7, 1 } },
      { { 64, 16 }, { 24, 8 }, { 12, 4 }, { 7, 1 } },
    };
static unsigned char good_quality_max_mesh_pct[MAX_MESH_SPEED + 1] = { 50, 50,
                                                                       25, 15,
                                                                       5,  1 };

// TODO(huisu@google.com): These settings are pretty relaxed, tune them for
// each speed setting
static MESH_PATTERN intrabc_mesh_patterns[MAX_MESH_SPEED + 1][MAX_MESH_STEP] = {
  { { 256, 1 }, { 256, 1 }, { 0, 0 }, { 0, 0 } },
  { { 256, 1 }, { 256, 1 }, { 0, 0 }, { 0, 0 } },
  { { 64, 1 }, { 64, 1 }, { 0, 0 }, { 0, 0 } },
  { { 64, 1 }, { 64, 1 }, { 0, 0 }, { 0, 0 } },
  { { 64, 4 }, { 16, 1 }, { 0, 0 }, { 0, 0 } },
  { { 64, 4 }, { 16, 1 }, { 0, 0 }, { 0, 0 } },
};
static uint8_t intrabc_max_mesh_pct[MAX_MESH_SPEED + 1] = { 100, 100, 100,
                                                            25,  25,  10 };

// Threshold values to be used for pruning the txfm_domain_distortion
// based on block MSE
// TODO(any): Experiment the threshold logic based on variance metric
static unsigned int tx_domain_dist_thresholds[MAX_TX_DOMAIN_EVAL_SPEED + 1] = {
  UINT_MAX, 22026, 22026, 22026, 22026, 0
};
// Threshold values to be used for disabling coeff RD-optimization
// based on block MSE
// TODO(any): Experiment the threshold logic based on variance metric
// Index 0 corresponds to the modes where winner mode processing is not
// applicable (Eg : IntraBc). Index 1 corresponds to the mode evaluation and is
// applicable when enable_winner_mode_for_coeff_opt speed feature is ON
static unsigned int coeff_opt_dist_thresholds[5][2] = { { UINT_MAX, UINT_MAX },
                                                        { 442413, 36314 },
                                                        { 162754, 36314 },
                                                        { 22026, 22026 },
                                                        { 22026, 22026 } };
// scaling values to be used for gating wedge/compound segment based on best
// approximate rd
static int comp_type_rd_threshold_mul[3] = { 1, 11, 12 };
static int comp_type_rd_threshold_div[3] = { 3, 16, 16 };

// Intra only frames, golden frames (except alt ref overlays) and
// alt ref frames tend to be coded at a higher than ambient quality
static int frame_is_boosted(const AV1_COMP *cpi) {
  return frame_is_kf_gf_arf(cpi);
}

// Sets a partition size down to which the auto partition code will always
// search (can go lower), based on the image dimensions. The logic here
// is that the extent to which ringing artefacts are offensive, depends
// partly on the screen area that over which they propogate. Propogation is
// limited by transform block size but the screen area take up by a given block
// size will be larger for a small image format stretched to full screen.
static BLOCK_SIZE set_partition_min_limit(const AV1_COMMON *const cm) {
  unsigned int screen_area = (cm->width * cm->height);

  // Select block size based on image format size.
  if (screen_area < 1280 * 720) {
    // Formats smaller in area than 720P
    return BLOCK_4X4;
  } else if (screen_area < 1920 * 1080) {
    // Format >= 720P and < 1080P
    return BLOCK_8X8;
  } else {
    // Formats 1080P and up
    return BLOCK_16X16;
  }
}

static void set_good_speed_feature_framesize_dependent(
    const AV1_COMP *const cpi, SPEED_FEATURES *const sf, int speed) {
  const AV1_COMMON *const cm = &cpi->common;
  const int is_720p_or_larger = AOMMIN(cm->width, cm->height) >= 720;
  const int is_480p_or_larger = AOMMIN(cm->width, cm->height) >= 480;

  if (is_480p_or_larger) {
    sf->use_square_partition_only_threshold = BLOCK_128X128;
    if (is_720p_or_larger)
      sf->auto_max_partition_based_on_simple_motion = ADAPT_PRED;
    else
      sf->auto_max_partition_based_on_simple_motion = RELAXED_PRED;
  } else {
    sf->use_square_partition_only_threshold = BLOCK_64X64;
    sf->auto_max_partition_based_on_simple_motion = DIRECT_PRED;
  }

  // TODO(huisu@google.com): train models for 720P and above.
  if (!is_720p_or_larger) {
    sf->ml_partition_search_breakout_thresh[0] = 200;  // BLOCK_8X8
    sf->ml_partition_search_breakout_thresh[1] = 250;  // BLOCK_16X16
    sf->ml_partition_search_breakout_thresh[2] = 300;  // BLOCK_32X32
    sf->ml_partition_search_breakout_thresh[3] = 500;  // BLOCK_64X64
    sf->ml_partition_search_breakout_thresh[4] = -1;   // BLOCK_128X128
    sf->ml_early_term_after_part_split_level = 1;
  }

  if (speed >= 1) {
    sf->simple_motion_search_split = 2;
    if (is_720p_or_larger) {
      sf->use_square_partition_only_threshold = BLOCK_128X128;
    } else if (is_480p_or_larger) {
      sf->use_square_partition_only_threshold = BLOCK_64X64;
    } else {
      sf->use_square_partition_only_threshold = BLOCK_32X32;

      sf->simple_motion_search_split = 1;
    }

    if (!is_720p_or_larger) {
      sf->ml_partition_search_breakout_thresh[0] = 200;  // BLOCK_8X8
      sf->ml_partition_search_breakout_thresh[1] = 250;  // BLOCK_16X16
      sf->ml_partition_search_breakout_thresh[2] = 300;  // BLOCK_32X32
      sf->ml_partition_search_breakout_thresh[3] = 300;  // BLOCK_64X64
      sf->ml_partition_search_breakout_thresh[4] = -1;   // BLOCK_128X128
    }
    sf->ml_early_term_after_part_split_level = 2;
  }

  if (speed >= 2) {
    if (is_720p_or_larger) {
      sf->use_square_partition_only_threshold = BLOCK_64X64;
    } else if (is_480p_or_larger) {
      sf->use_square_partition_only_threshold = BLOCK_32X32;
    } else {
      sf->use_square_partition_only_threshold = BLOCK_32X32;
    }

    if (is_720p_or_larger) {
      sf->partition_search_breakout_dist_thr = (1 << 24);
      sf->partition_search_breakout_rate_thr = 120;
    } else {
      sf->partition_search_breakout_dist_thr = (1 << 22);
      sf->partition_search_breakout_rate_thr = 100;
    }
    sf->rd_auto_partition_min_limit = set_partition_min_limit(cm);
  }

  if (speed >= 3) {
    sf->ml_early_term_after_part_split_level = 0;
    if (is_720p_or_larger) {
      sf->partition_search_breakout_dist_thr = (1 << 25);
      sf->partition_search_breakout_rate_thr = 200;
    } else {
      sf->max_intra_bsize = BLOCK_32X32;
      sf->partition_search_breakout_dist_thr = (1 << 23);
      sf->partition_search_breakout_rate_thr = 120;
    }

    // TODO(Venkat): Clean-up frame type dependency for
    // simple_motion_search_split in partition search function and set the
    // speed feature accordingly
    // TODO(any): The models and thresholds used by simple_motion_split is
    // trained and tuned on speed 1 and 2. We might get better performance if we
    // readjust them for speed 3 and 4.
    sf->simple_motion_search_split = cm->allow_screen_content_tools ? 1 : 2;
  }

  if (speed >= 4) {
    if (is_720p_or_larger) {
      sf->partition_search_breakout_dist_thr = (1 << 26);
    } else {
      sf->partition_search_breakout_dist_thr = (1 << 24);
    }
  }
}

static void set_good_speed_features_framesize_independent(
    const AV1_COMP *const cpi, SPEED_FEATURES *const sf, int speed) {
  const AV1_COMMON *const cm = &cpi->common;
  const int boosted = frame_is_boosted(cpi);
  const int is_boosted_arf2_bwd_type =
      boosted || cpi->refresh_bwd_ref_frame || cpi->refresh_alt2_ref_frame;

  // Speed 0 for all speed features that give neutral coding performance change.
  sf->reduce_inter_modes = 1;
  sf->prune_ext_partition_types_search_level = 1;
  sf->ml_prune_rect_partition = 1;
  sf->ml_prune_ab_partition = 1;
  sf->ml_prune_4_partition = 1;
  sf->simple_motion_search_prune_rect = 1;
  sf->adaptive_txb_search_level = 1;
  sf->use_dist_wtd_comp_flag = DIST_WTD_COMP_SKIP_MV_SEARCH;
  sf->model_based_prune_tx_search_level = 1;
  sf->model_based_post_interp_filter_breakout = 1;

  // TODO(debargha): Test, tweak and turn on either 1 or 2
  sf->inter_mode_rd_model_estimation = 1;
  sf->inter_mode_rd_model_estimation_adaptive = 0;
  sf->prune_compound_using_single_ref = 1;

  sf->prune_mode_search_simple_translation = 1;
  sf->two_loop_comp_search = 0;
  sf->prune_ref_frame_for_rect_partitions =
      (boosted || (cm->allow_screen_content_tools))
          ? 0
          : (is_boosted_arf2_bwd_type ? 1 : 2);
  sf->less_rectangular_check_level = 1;
  sf->gm_search_type = GM_REDUCED_REF_SEARCH_SKIP_L2_L3;
  sf->gm_disable_recode = 1;
  sf->use_fast_interpolation_filter_search = 1;
  sf->intra_tx_size_search_init_depth_sqr = 1;
  sf->intra_angle_estimation = 1;
  sf->tx_type_search.use_reduced_intra_txset = 1;
  sf->selective_ref_frame = 1;
  sf->prune_wedge_pred_diff_based = 1;
  sf->disable_wedge_search_var_thresh = 0;
  sf->disable_wedge_search_edge_thresh = 0;
  sf->prune_motion_mode_level = 1;
  sf->cb_pred_filter_search = 0;
  sf->use_nonrd_pick_mode = 0;
  sf->use_real_time_ref_set = 0;

  if (speed >= 1) {
    sf->selective_ref_frame = 2;

    sf->intra_tx_size_search_init_depth_rect = 1;

    sf->skip_repeat_interpolation_filter_search = 1;
    sf->tx_type_search.skip_tx_search = 1;
    sf->tx_type_search.ml_tx_split_thresh = 40;
    sf->adaptive_txb_search_level = 2;
    sf->use_intra_txb_hash = 1;
    sf->dual_sgr_penalty_level = 1;
    sf->use_accurate_subpel_search = USE_4_TAPS;
    sf->reuse_inter_intra_mode = 1;
    sf->prune_comp_search_by_single_result = 1;
    sf->skip_repeated_newmv = 1;
    sf->obmc_full_pixel_search_level = 1;
    sf->simple_motion_search_split = 1;
    sf->simple_motion_search_early_term_none = 1;

    sf->disable_wedge_search_var_thresh = 0;
    sf->disable_wedge_search_edge_thresh = 0;
    sf->disable_interinter_wedge_newmv_search = boosted ? 0 : 1;
    sf->prune_comp_type_by_comp_avg = 1;
    sf->prune_motion_mode_level = 2;
    sf->gm_search_type = GM_REDUCED_REF_SEARCH_SKIP_L2_L3_ARF2;
    sf->cb_pred_filter_search = 1;
    sf->use_transform_domain_distortion = boosted ? 1 : 2;
    sf->perform_coeff_opt = boosted ? 1 : 2;
    sf->prune_ref_frame_for_rect_partitions =
        (frame_is_intra_only(&cpi->common) || (cm->allow_screen_content_tools))
            ? 0
            : (boosted ? 1 : 2);
    sf->intra_cnn_split = (speed == 1);
  }

  if (speed >= 2) {
    sf->gm_erroradv_type = GM_ERRORADV_TR_2;

    sf->enable_sgr_ep_pruning = 1;
    sf->selective_ref_frame = 3;
    sf->inter_tx_size_search_init_depth_rect = 1;
    sf->inter_tx_size_search_init_depth_sqr = 1;

    sf->cdef_pick_method = CDEF_FAST_SEARCH;

    sf->adaptive_rd_thresh = 1;
    sf->mv.auto_mv_step_size = 1;
    sf->mv.subpel_iters_per_step = 1;
    sf->disable_filter_search_var_thresh = 100;
    sf->comp_inter_joint_search_thresh = BLOCK_SIZES_ALL;

    sf->allow_partition_search_skip = 1;
    sf->disable_wedge_search_var_thresh = 100;
    sf->disable_wedge_search_edge_thresh = 0;
    sf->disable_interinter_wedge_newmv_search = 1;
    sf->fast_wedge_sign_estimate = 1;
    sf->disable_dual_filter = 1;
    sf->use_dist_wtd_comp_flag = DIST_WTD_COMP_DISABLED;
    sf->prune_comp_type_by_comp_avg = 2;
    // TODO(Sachin): Enable/Enhance this speed feature for speed 2 & 3
    sf->cb_pred_filter_search = 0;
    sf->adaptive_interp_filter_search = 1;
    sf->perform_coeff_opt = is_boosted_arf2_bwd_type ? 2 : 3;
    sf->model_based_prune_tx_search_level = 0;
  }

  if (speed >= 3) {
    sf->tx_size_search_method = boosted ? USE_FULL_RD : USE_LARGESTALL;
    sf->less_rectangular_check_level = 2;
    // adaptive_motion_search breaks encoder multi-thread tests.
    // The values in x->pred_mv[] differ for single and multi-thread cases.
    // See aomedia:1778.
    // sf->adaptive_motion_search = 1;
    sf->recode_loop = ALLOW_RECODE_KFARFGF;
    sf->use_accurate_subpel_search = USE_2_TAPS;
    if (cpi->oxcf.enable_smooth_interintra)
      sf->disable_smooth_interintra = boosted ? 0 : 1;
    sf->tx_type_search.prune_mode = PRUNE_2D_FAST;
    sf->gm_search_type = GM_DISABLE_SEARCH;
    sf->prune_comp_search_by_single_result = 2;
    sf->prune_motion_mode_level = boosted ? 2 : 3;
    sf->prune_warp_using_wmtype = 1;
    // TODO(yunqing): evaluate this speed feature for speed 1 & 2, and combine
    // it with cpi->sf.disable_wedge_search_var_thresh.
    sf->disable_wedge_interintra_search = 1;
    // TODO(any): Experiment with the early exit mechanism for speeds 0, 1 and 2
    // and clean-up the speed feature
    sf->perform_best_rd_based_gating_for_chroma = 1;
    sf->prune_comp_type_by_model_rd = boosted ? 0 : 1;
    sf->disable_smooth_intra =
        !frame_is_intra_only(&cpi->common) || (cpi->rc.frames_to_key != 1);
    // TODO(any): Experiment on the dependency of this speed feature with
    // use_intra_txb_hash, use_inter_txb_hash and use_mb_rd_hash speed features
    sf->enable_winner_mode_for_coeff_opt = 1;
  }

  if (speed >= 4) {
    sf->selective_ref_frame = 4;
    sf->use_intra_txb_hash = 0;
    sf->tx_type_search.fast_intra_tx_type_search = 1;
    sf->disable_loop_restoration_chroma =
        (boosted || cm->allow_screen_content_tools) ? 0 : 1;
    sf->reduce_wiener_window_size = !boosted;
    sf->mv.subpel_search_method = SUBPEL_TREE_PRUNED;
    sf->cb_pred_filter_search = 1;
    sf->adaptive_mode_search = 1;
    sf->alt_ref_search_fp = 1;
    sf->skip_sharp_interp_filter_search = 1;
    sf->perform_coeff_opt = is_boosted_arf2_bwd_type ? 2 : 4;
    sf->adaptive_txb_search_level = boosted ? 2 : 3;
  }

  if (speed >= 5) {
    sf->recode_loop = ALLOW_RECODE_KFMAXBW;
    sf->intra_y_mode_mask[TX_64X64] = INTRA_DC_H_V;
    sf->intra_uv_mode_mask[TX_64X64] = UV_INTRA_DC_H_V_CFL;
    sf->intra_y_mode_mask[TX_32X32] = INTRA_DC_H_V;
    sf->intra_uv_mode_mask[TX_32X32] = UV_INTRA_DC_H_V_CFL;
    sf->intra_y_mode_mask[TX_16X16] = INTRA_DC_H_V;
    sf->intra_uv_mode_mask[TX_16X16] = UV_INTRA_DC_H_V_CFL;

    sf->mv.subpel_search_method = SUBPEL_TREE_PRUNED_MORE;

    // TODO(any): The following features have no impact on quality and speed,
    // and are disabled.
    // sf->disable_filter_search_var_thresh = 200;
    // sf->use_fast_coef_costing = 1;
    // sf->partition_search_breakout_rate_thr = 300;

    // TODO(any): The following features give really bad quality/speed trade
    // off. Needs to be re-worked.
    // sf->tx_size_search_method = USE_LARGESTALL;
    // sf->mv.search_method = BIGDIA;
    // sf->adaptive_rd_thresh = 4;
    // sf->mode_search_skip_flags =
    //     (cm->current_frame.frame_type == KEY_FRAME)
    //     ? 0
    //     : FLAG_SKIP_INTRA_DIRMISMATCH | FLAG_SKIP_INTRA_BESTINTER |
    //     FLAG_SKIP_COMP_BESTINTRA | FLAG_SKIP_INTRA_LOWVAR |
    //     FLAG_EARLY_TERMINATE;
    // sf->use_transform_domain_distortion = 2;
  }

  if (speed >= 6) {
    int i;
    sf->optimize_coefficients = NO_TRELLIS_OPT;
    sf->mv.search_method = HEX;
    sf->disable_filter_search_var_thresh = 500;
    for (i = 0; i < TX_SIZES; ++i) {
      sf->intra_y_mode_mask[i] = INTRA_DC;
      sf->intra_uv_mode_mask[i] = UV_INTRA_DC_CFL;
    }
    sf->partition_search_breakout_rate_thr = 500;
    sf->mv.reduce_first_step_size = 1;
    sf->simple_model_rd_from_var = 1;
  }
  if (speed >= 7) {
    sf->default_max_partition_size = BLOCK_32X32;
    sf->default_min_partition_size = BLOCK_8X8;
    sf->intra_y_mode_mask[TX_64X64] = INTRA_DC;
    sf->intra_y_mode_mask[TX_32X32] = INTRA_DC;
    sf->frame_parameter_update = 0;
    sf->mv.search_method = FAST_HEX;
    sf->partition_search_type = REFERENCE_PARTITION;
    sf->mode_search_skip_flags |= FLAG_SKIP_INTRA_DIRMISMATCH;
    // TODO(any): evaluate adaptive_mode_search=1 for speed 7 & 8
    sf->adaptive_mode_search = 2;
  }
  if (speed >= 8) {
    sf->mv.search_method = FAST_DIAMOND;
    sf->mv.subpel_force_stop = HALF_PEL;
    sf->lpf_pick = LPF_PICK_FROM_Q;
  }
}

// TODO(kyslov): now this is very similar to
// set_good_speed_features_framesize_independent
//               except it sets non-rd flag on speed8. This function will likely
//               be modified in the future with RT-specific speed features
static void set_rt_speed_features_framesize_independent(AV1_COMP *cpi,
                                                        SPEED_FEATURES *sf,
                                                        int speed) {
  AV1_COMMON *const cm = &cpi->common;
  const int boosted = frame_is_boosted(cpi);

  // Speed 0 for all speed features that give neutral coding performance change.
  sf->reduce_inter_modes = 1;
  sf->prune_ext_partition_types_search_level = 1;
  sf->ml_prune_rect_partition = 1;
  sf->ml_prune_ab_partition = 1;
  sf->ml_prune_4_partition = 1;
  sf->adaptive_txb_search_level = 1;
  sf->use_dist_wtd_comp_flag = DIST_WTD_COMP_SKIP_MV_SEARCH;
  sf->model_based_prune_tx_search_level = 1;
  sf->model_based_post_interp_filter_breakout = 1;

  // TODO(debargha): Test, tweak and turn on either 1 or 2
  sf->inter_mode_rd_model_estimation = 0;
  sf->inter_mode_rd_model_estimation_adaptive = 0;
  sf->prune_compound_using_single_ref = 0;
  sf->prune_mode_search_simple_translation = 1;
  sf->two_loop_comp_search = 0;

  sf->prune_ref_frame_for_rect_partitions = !boosted;
  sf->less_rectangular_check_level = 1;
  sf->gm_search_type = GM_REDUCED_REF_SEARCH_SKIP_L2_L3;
  sf->gm_disable_recode = 1;
  sf->use_fast_interpolation_filter_search = 1;
  sf->intra_tx_size_search_init_depth_sqr = 1;
  sf->intra_angle_estimation = 1;
  sf->tx_type_search.use_reduced_intra_txset = 1;
  sf->selective_ref_frame = 1;
  sf->prune_wedge_pred_diff_based = 1;
  sf->disable_wedge_search_var_thresh = 0;
  sf->disable_wedge_search_edge_thresh = 0;
  sf->prune_motion_mode_level = 1;
  sf->cb_pred_filter_search = 0;
  sf->use_nonrd_pick_mode = 0;
  sf->use_real_time_ref_set = 0;
  sf->use_fast_nonrd_pick_mode = 0;
  sf->reuse_inter_pred_nonrd = 0;
  sf->estimate_motion_for_var_based_partition = 1;
  sf->use_comp_ref_nonrd = 1;
  sf->check_intra_pred_nonrd = 1;

  if (speed >= 1) {
    sf->gm_erroradv_type = GM_ERRORADV_TR_1;
    sf->selective_ref_frame = 2;

    sf->intra_tx_size_search_init_depth_rect = 1;
    sf->tx_size_search_lgr_block = 1;
    sf->prune_ext_partition_types_search_level = 2;
    sf->skip_repeat_interpolation_filter_search = 1;
    sf->tx_type_search.skip_tx_search = 1;
    sf->tx_type_search.ml_tx_split_thresh = 40;
    sf->adaptive_txb_search_level = 2;
    sf->use_intra_txb_hash = 1;
    sf->optimize_b_precheck = 1;
    sf->dual_sgr_penalty_level = 1;
    sf->use_accurate_subpel_search = USE_4_TAPS;
    sf->reuse_inter_intra_mode = 1;
    sf->prune_comp_search_by_single_result = 1;
    sf->skip_repeated_newmv = 1;
    sf->obmc_full_pixel_search_level = 1;
    // TODO(jianj): Following speed feature will be further explored to
    // identify the appropriate tradeoff between encoder performance and its
    // speed.
    sf->prune_single_motion_modes_by_simple_trans = 1;

    sf->simple_motion_search_prune_rect = 1;

    sf->disable_wedge_search_var_thresh = 0;
    sf->disable_wedge_search_edge_thresh = 0;
    sf->prune_comp_type_by_comp_avg = 1;
    sf->prune_motion_mode_level = 2;
    sf->gm_search_type = GM_REDUCED_REF_SEARCH_SKIP_L2_L3_ARF2;
    sf->cb_pred_filter_search = 1;
    sf->use_transform_domain_distortion = boosted ? 0 : 1;
  }

  if (speed >= 2) {
    sf->gm_erroradv_type = GM_ERRORADV_TR_2;

    sf->selective_ref_frame = 3;
    sf->inter_tx_size_search_init_depth_rect = 1;
    sf->inter_tx_size_search_init_depth_sqr = 1;
    sf->cdef_pick_method = CDEF_FAST_SEARCH;

    sf->adaptive_rd_thresh = 1;
    sf->mv.auto_mv_step_size = 1;
    sf->mv.subpel_iters_per_step = 1;
    sf->disable_filter_search_var_thresh = 100;
    sf->comp_inter_joint_search_thresh = BLOCK_SIZES_ALL;

    sf->partition_search_breakout_rate_thr = 80;
    sf->allow_partition_search_skip = 1;
    sf->disable_wedge_search_var_thresh = 100;
    sf->disable_wedge_search_edge_thresh = 0;
    sf->fast_wedge_sign_estimate = 1;
    sf->disable_dual_filter = 1;
    sf->use_dist_wtd_comp_flag = DIST_WTD_COMP_DISABLED;
    sf->prune_comp_type_by_comp_avg = 2;
    sf->cb_pred_filter_search = 0;
    sf->adaptive_interp_filter_search = 1;
    sf->model_based_prune_tx_search_level = 0;
  }

  if (speed >= 3) {
    sf->selective_ref_frame = 4;
    sf->tx_size_search_method = boosted ? USE_FULL_RD : USE_LARGESTALL;
    sf->less_rectangular_check_level = 2;
    // adaptive_motion_search breaks encoder multi-thread tests.
    // The values in x->pred_mv[] differ for single and multi-thread cases.
    // See aomedia:1778.
    // sf->adaptive_motion_search = 1;
    sf->recode_loop = ALLOW_RECODE_KFARFGF;
    sf->use_transform_domain_distortion = 1;
    sf->use_accurate_subpel_search = USE_2_TAPS;
    sf->adaptive_rd_thresh = 2;
    sf->tx_type_search.prune_mode = PRUNE_2D_FAST;
    sf->gm_search_type = GM_DISABLE_SEARCH;
    sf->prune_comp_search_by_single_result = 2;
    sf->prune_motion_mode_level = boosted ? 2 : 3;
    sf->prune_warp_using_wmtype = 1;
    // TODO(yunqing): evaluate this speed feature for speed 1 & 2, and combine
    // it with cpi->sf.disable_wedge_search_var_thresh.
    sf->disable_wedge_interintra_search = 1;
  }

  if (speed >= 4) {
    sf->use_intra_txb_hash = 0;
    sf->use_mb_rd_hash = 0;
    sf->tx_type_search.fast_intra_tx_type_search = 1;
    sf->tx_type_search.fast_inter_tx_type_search = 1;
    sf->tx_size_search_method =
        frame_is_intra_only(cm) ? USE_FULL_RD : USE_LARGESTALL;
    sf->mv.subpel_search_method = SUBPEL_TREE_PRUNED;
    sf->adaptive_mode_search = 1;
    sf->alt_ref_search_fp = 1;
    sf->skip_sharp_interp_filter_search = 1;
  }

  if (speed >= 5) {
    sf->recode_loop = ALLOW_RECODE_KFMAXBW;
    sf->intra_y_mode_mask[TX_64X64] = INTRA_DC_H_V;
    sf->intra_uv_mode_mask[TX_64X64] = UV_INTRA_DC_H_V_CFL;
    sf->intra_y_mode_mask[TX_32X32] = INTRA_DC_H_V;
    sf->intra_uv_mode_mask[TX_32X32] = UV_INTRA_DC_H_V_CFL;
    sf->intra_y_mode_mask[TX_16X16] = INTRA_DC_H_V;
    sf->intra_uv_mode_mask[TX_16X16] = UV_INTRA_DC_H_V_CFL;
    sf->tx_size_search_method = USE_LARGESTALL;
    sf->mv.search_method = BIGDIA;
    sf->mv.subpel_search_method = SUBPEL_TREE_PRUNED_MORE;
    sf->adaptive_rd_thresh = 4;
    sf->mode_search_skip_flags =
        (cm->current_frame.frame_type == KEY_FRAME)
            ? 0
            : FLAG_SKIP_INTRA_DIRMISMATCH | FLAG_SKIP_INTRA_BESTINTER |
                  FLAG_SKIP_COMP_BESTINTRA | FLAG_SKIP_INTRA_LOWVAR |
                  FLAG_EARLY_TERMINATE;
    sf->disable_filter_search_var_thresh = 200;
    sf->use_fast_coef_costing = 1;
    sf->partition_search_breakout_rate_thr = 300;
    sf->use_transform_domain_distortion = 2;
  }

  if (speed >= 6) {
    sf->optimize_coefficients = NO_TRELLIS_OPT;
    sf->mv.search_method = HEX;
    sf->disable_filter_search_var_thresh = 500;
    for (int i = 0; i < TX_SIZES; ++i) {
      sf->intra_y_mode_mask[i] = INTRA_DC;
      sf->intra_uv_mode_mask[i] = UV_INTRA_DC_CFL;
    }
    sf->partition_search_breakout_rate_thr = 500;
    sf->mv.reduce_first_step_size = 1;
    sf->simple_model_rd_from_var = 1;
  }
  if (speed >= 7) {
    sf->lpf_pick = LPF_PICK_FROM_Q;
    sf->mv.subpel_force_stop = QUARTER_PEL;
    sf->default_max_partition_size = BLOCK_128X128;
    sf->default_min_partition_size = BLOCK_8X8;
    sf->frame_parameter_update = 0;
    sf->mv.search_method = FAST_DIAMOND;
    sf->partition_search_type = VAR_BASED_PARTITION;
    sf->mode_search_skip_flags |= FLAG_SKIP_INTRA_DIRMISMATCH;
    sf->use_real_time_ref_set = 1;
    // Can't use LARGEST TX mode with pre-calculated partition
    // and disabled TX64
    if (!cpi->oxcf.enable_tx64) sf->tx_size_search_method = USE_FAST_RD;
    sf->use_nonrd_pick_mode = 1;
    sf->use_comp_ref_nonrd = 0;
    sf->inter_mode_rd_model_estimation = 2;
    sf->cdef_pick_method = CDEF_PICK_FROM_Q;
    sf->max_intra_bsize = BLOCK_16X16;
  }
  if (speed >= 8) {
    sf->use_fast_nonrd_pick_mode = 1;
    sf->mv.subpel_search_method = SUBPEL_TREE_PRUNED_MORE;
    sf->tx_size_search_method = USE_FAST_RD;
    sf->estimate_motion_for_var_based_partition = 0;
// TODO(kyslov) Enable when better model is available
// It gives +5% speedup and 11% overall BDRate degradation
// So, can not enable now until better CurvFit is there
#if 0
    sf->use_modeled_non_rd_cost = 1;
#endif
  }
}

void av1_set_speed_features_framesize_dependent(AV1_COMP *cpi, int speed) {
  SPEED_FEATURES *const sf = &cpi->sf;
  const AV1EncoderConfig *const oxcf = &cpi->oxcf;

  if (oxcf->mode == GOOD) {
    set_good_speed_feature_framesize_dependent(cpi, sf, speed);
  }

  // This is only used in motion vector unit test.
  if (cpi->oxcf.motion_vector_unit_test == 1)
    cpi->find_fractional_mv_step = av1_return_max_sub_pixel_mv;
  else if (cpi->oxcf.motion_vector_unit_test == 2)
    cpi->find_fractional_mv_step = av1_return_min_sub_pixel_mv;
}

void av1_set_speed_features_framesize_independent(AV1_COMP *cpi, int speed) {
  AV1_COMMON *const cm = &cpi->common;
  SPEED_FEATURES *const sf = &cpi->sf;
  MACROBLOCK *const x = &cpi->td.mb;
  const AV1EncoderConfig *const oxcf = &cpi->oxcf;
  int i;

  // best quality defaults
  sf->frame_parameter_update = 1;
  sf->mv.search_method = NSTEP;
  sf->recode_loop = ALLOW_RECODE;
  sf->mv.subpel_search_method = SUBPEL_TREE;
  sf->mv.subpel_iters_per_step = 2;
  sf->mv.subpel_force_stop = EIGHTH_PEL;
  if (cpi->oxcf.disable_trellis_quant == 3) {
    sf->optimize_coefficients = !is_lossless_requested(&cpi->oxcf)
                                    ? NO_ESTIMATE_YRD_TRELLIS_OPT
                                    : NO_TRELLIS_OPT;
  } else if (cpi->oxcf.disable_trellis_quant == 2) {
    sf->optimize_coefficients = !is_lossless_requested(&cpi->oxcf)
                                    ? FINAL_PASS_TRELLIS_OPT
                                    : NO_TRELLIS_OPT;
  } else if (cpi->oxcf.disable_trellis_quant == 0) {
    if (is_lossless_requested(&cpi->oxcf))
      sf->optimize_coefficients = NO_TRELLIS_OPT;
    else
      sf->optimize_coefficients = FULL_TRELLIS_OPT;
  } else if (cpi->oxcf.disable_trellis_quant == 1) {
    sf->optimize_coefficients = NO_TRELLIS_OPT;
  } else {
    assert(0 && "Invalid disable_trellis_quant value");
  }
  sf->gm_erroradv_type = GM_ERRORADV_TR_0;
  sf->mv.reduce_first_step_size = 0;
  sf->mv.auto_mv_step_size = 0;
  sf->comp_inter_joint_search_thresh = BLOCK_4X4;
  sf->adaptive_rd_thresh = 0;
  // TODO(sarahparker) Pair this with a speed setting once experiments are done
  sf->trellis_eob_fast = 0;
  sf->tx_size_search_method = cpi->oxcf.tx_size_search_method;
  sf->inter_tx_size_search_init_depth_sqr = 0;
  sf->inter_tx_size_search_init_depth_rect = 0;
  sf->intra_tx_size_search_init_depth_rect = 0;
  sf->intra_tx_size_search_init_depth_sqr = 0;
  sf->tx_size_search_lgr_block = 0;
  sf->model_based_prune_tx_search_level = 0;
  sf->model_based_post_interp_filter_breakout = 0;
  sf->reduce_inter_modes = 0;
  sf->selective_ref_gm = 1;
  sf->adaptive_motion_search = 0;
  sf->adaptive_mode_search = 0;
  sf->alt_ref_search_fp = 0;
  sf->partition_search_type = SEARCH_PARTITION;
  sf->tx_type_search.prune_mode = PRUNE_2D_ACCURATE;
  sf->tx_type_search.ml_tx_split_thresh = 30;
  sf->tx_type_search.use_skip_flag_prediction = 1;
  sf->tx_type_search.use_reduced_intra_txset = 0;
  sf->tx_type_search.fast_intra_tx_type_search = 0;
  sf->tx_type_search.fast_inter_tx_type_search = 0;
  sf->tx_type_search.skip_tx_search = 0;
  sf->selective_ref_frame = 0;
  sf->less_rectangular_check_level = 0;
  sf->use_square_partition_only_threshold = BLOCK_128X128;
  sf->prune_ref_frame_for_rect_partitions = 0;
  sf->auto_max_partition_based_on_simple_motion = NOT_IN_USE;
  sf->auto_min_partition_based_on_simple_motion = 0;
  sf->rd_auto_partition_min_limit = BLOCK_4X4;
  sf->default_max_partition_size = BLOCK_LARGEST;
  sf->default_min_partition_size = BLOCK_4X4;
  sf->adjust_partitioning_from_last_frame = 0;
  sf->mode_search_skip_flags = 0;
  sf->disable_filter_search_var_thresh = 0;
  sf->allow_partition_search_skip = 0;
  sf->use_accurate_subpel_search = USE_8_TAPS;
  sf->disable_wedge_search_edge_thresh = 0;
  sf->disable_wedge_search_var_thresh = 0;
  sf->disable_loop_restoration_chroma = 0;
  sf->enable_sgr_ep_pruning = 0;
  sf->reduce_wiener_window_size = 0;
  sf->fast_wedge_sign_estimate = 0;
  sf->prune_wedge_pred_diff_based = 0;
  sf->drop_ref = 0;
  sf->skip_intra_in_interframe = 1;
  sf->txb_split_cap = 1;
  sf->adaptive_txb_search_level = 0;
  sf->use_intra_txb_hash = 0;
  sf->use_inter_txb_hash = 1;
  sf->use_mb_rd_hash = 1;
  sf->optimize_b_precheck = 0;
  sf->two_loop_comp_search = 1;
  sf->use_dist_wtd_comp_flag = DIST_WTD_COMP_ENABLED;
  sf->reuse_inter_intra_mode = 0;
  sf->intra_angle_estimation = 0;
  sf->skip_obmc_in_uniform_mv_field = 0;
  sf->skip_wm_in_uniform_mv_field = 0;
  sf->adaptive_interp_filter_search = 0;

  for (i = 0; i < TX_SIZES; i++) {
    sf->intra_y_mode_mask[i] = INTRA_ALL;
    sf->intra_uv_mode_mask[i] = UV_INTRA_ALL;
  }
  sf->lpf_pick = LPF_PICK_FROM_FULL_IMAGE;
  sf->cdef_pick_method = CDEF_FULL_SEARCH;
  sf->use_fast_coef_costing = 0;
  sf->max_intra_bsize = BLOCK_LARGEST;
  // This setting only takes effect when partition_search_type is set
  // to FIXED_PARTITION.
  sf->always_this_block_size = BLOCK_16X16;
  // Recode loop tolerance %.
  sf->recode_tolerance = 25;
  sf->partition_search_breakout_dist_thr = 0;
  sf->partition_search_breakout_rate_thr = 0;
  sf->simple_model_rd_from_var = 0;
  sf->prune_ext_partition_types_search_level = 0;
  sf->ml_prune_rect_partition = 0;
  sf->ml_prune_ab_partition = 0;
  sf->ml_prune_4_partition = 0;
  sf->ml_early_term_after_part_split_level = 0;
  for (i = 0; i < PARTITION_BLOCK_SIZES; ++i) {
    sf->ml_partition_search_breakout_thresh[i] = -1;  // -1 means not enabled.
  }
  sf->simple_motion_search_split = 0;
  sf->simple_motion_search_prune_rect = 0;
  sf->simple_motion_search_early_term_none = 0;
  sf->intra_cnn_split = 0;

  // Set this at the appropriate speed levels
  sf->use_transform_domain_distortion = 0;
  sf->gm_search_type = GM_FULL_SEARCH;
  sf->gm_disable_recode = 0;
  sf->use_fast_interpolation_filter_search = 0;
  sf->disable_dual_filter = 0;
  sf->skip_repeat_interpolation_filter_search = 0;
  sf->use_hash_based_trellis = 0;
  sf->prune_comp_search_by_single_result = 0;
  sf->skip_repeated_newmv = 0;
  // TODO(any) Cleanup this speed feature
  sf->prune_single_motion_modes_by_simple_trans = 0;

  // Set decoder side speed feature to use less dual sgr modes
  sf->dual_sgr_penalty_level = 0;

  sf->inter_mode_rd_model_estimation = 0;
  sf->inter_mode_rd_model_estimation_adaptive = 0;
  sf->prune_compound_using_single_ref = 0;

  sf->prune_mode_search_simple_translation = 0;
  sf->obmc_full_pixel_search_level = 0;
  sf->skip_sharp_interp_filter_search = 0;
  sf->prune_comp_type_by_comp_avg = 0;
  sf->disable_interinter_wedge_newmv_search = 0;
  sf->disable_smooth_interintra = 0;
  sf->prune_motion_mode_level = 0;
  sf->prune_warp_using_wmtype = 0;
  sf->disable_wedge_interintra_search = 0;
  sf->perform_coeff_opt = 0;
  sf->enable_winner_mode_for_coeff_opt = 0;
  sf->prune_comp_type_by_model_rd = 0;
  sf->disable_smooth_intra = 0;
  sf->perform_best_rd_based_gating_for_chroma = 0;

  if (oxcf->mode == GOOD)
    set_good_speed_features_framesize_independent(cpi, sf, speed);
  else if (oxcf->mode == REALTIME)
    set_rt_speed_features_framesize_independent(cpi, sf, speed);

  if (!cpi->seq_params_locked) {
    cpi->common.seq_params.enable_dual_filter &= !sf->disable_dual_filter;
  }

  // sf->partition_search_breakout_dist_thr is set assuming max 64x64
  // blocks. Normalise this if the blocks are bigger.
  if (MAX_SB_SIZE_LOG2 > 6) {
    sf->partition_search_breakout_dist_thr <<= 2 * (MAX_SB_SIZE_LOG2 - 6);
  }

  cpi->diamond_search_sad = av1_diamond_search_sad;

  sf->allow_exhaustive_searches = 1;

  const int mesh_speed = AOMMIN(speed, MAX_MESH_SPEED);
  if (cpi->twopass.fr_content_type == FC_GRAPHICS_ANIMATION)
    sf->exhaustive_searches_thresh = (1 << 24);
  else
    sf->exhaustive_searches_thresh = (1 << 25);
  sf->max_exaustive_pct = good_quality_max_mesh_pct[mesh_speed];
  if (mesh_speed > 0)
    sf->exhaustive_searches_thresh = sf->exhaustive_searches_thresh << 1;

  for (i = 0; i < MAX_MESH_STEP; ++i) {
    sf->mesh_patterns[i].range =
        good_quality_mesh_patterns[mesh_speed][i].range;
    sf->mesh_patterns[i].interval =
        good_quality_mesh_patterns[mesh_speed][i].interval;
  }
  if (frame_is_intra_only(cm) && cm->allow_screen_content_tools &&
      (cpi->twopass.fr_content_type == FC_GRAPHICS_ANIMATION ||
       cpi->oxcf.content == AOM_CONTENT_SCREEN)) {
    for (i = 0; i < MAX_MESH_STEP; ++i) {
      sf->mesh_patterns[i].range = intrabc_mesh_patterns[mesh_speed][i].range;
      sf->mesh_patterns[i].interval =
          intrabc_mesh_patterns[mesh_speed][i].interval;
    }
    sf->max_exaustive_pct = intrabc_max_mesh_pct[mesh_speed];
  }

  // Slow quant, dct and trellis not worthwhile for first pass
  // so make sure they are always turned off.
  if (oxcf->pass == 1) sf->optimize_coefficients = NO_TRELLIS_OPT;

  // No recode or trellis for 1 pass.
  if (oxcf->pass == 0) {
    sf->recode_loop = DISALLOW_RECODE;
    sf->optimize_coefficients = NO_TRELLIS_OPT;
  }
  // FIXME: trellis not very efficient for quantization matrices
  if (oxcf->using_qm) sf->optimize_coefficients = NO_TRELLIS_OPT;

  if (sf->mv.subpel_search_method == SUBPEL_TREE) {
    cpi->find_fractional_mv_step = av1_find_best_sub_pixel_tree;
  } else if (sf->mv.subpel_search_method == SUBPEL_TREE_PRUNED) {
    cpi->find_fractional_mv_step = av1_find_best_sub_pixel_tree_pruned;
  } else if (sf->mv.subpel_search_method == SUBPEL_TREE_PRUNED_MORE) {
    cpi->find_fractional_mv_step = av1_find_best_sub_pixel_tree_pruned_more;
  } else if (sf->mv.subpel_search_method == SUBPEL_TREE_PRUNED_EVENMORE) {
    cpi->find_fractional_mv_step = av1_find_best_sub_pixel_tree_pruned_evenmore;
  }

  x->min_partition_size = sf->default_min_partition_size;
  x->max_partition_size = sf->default_max_partition_size;

  // This is only used in motion vector unit test.
  if (cpi->oxcf.motion_vector_unit_test == 1)
    cpi->find_fractional_mv_step = av1_return_max_sub_pixel_mv;
  else if (cpi->oxcf.motion_vector_unit_test == 2)
    cpi->find_fractional_mv_step = av1_return_min_sub_pixel_mv;
  cpi->max_comp_type_rd_threshold_mul =
      comp_type_rd_threshold_mul[sf->prune_comp_type_by_comp_avg];
  cpi->max_comp_type_rd_threshold_div =
      comp_type_rd_threshold_div[sf->prune_comp_type_by_comp_avg];
  const int tx_domain_speed = AOMMIN(speed, MAX_TX_DOMAIN_EVAL_SPEED);
  cpi->tx_domain_dist_threshold = tx_domain_dist_thresholds[tx_domain_speed];

  // assert ensures that coeff_opt_dist_thresholds is accessed correctly
  assert(cpi->sf.perform_coeff_opt >= 0 && cpi->sf.perform_coeff_opt < 5);
  memcpy(cpi->coeff_opt_dist_threshold,
         coeff_opt_dist_thresholds[cpi->sf.perform_coeff_opt],
         sizeof(cpi->coeff_opt_dist_threshold));

#if CONFIG_DIST_8X8
  if (sf->use_transform_domain_distortion > 0) cpi->oxcf.using_dist_8x8 = 0;

  if (cpi->oxcf.using_dist_8x8) x->min_partition_size = BLOCK_8X8;
#endif  // CONFIG_DIST_8X8
  if (cpi->oxcf.row_mt == 1 && (cpi->oxcf.max_threads > 1)) {
    if (sf->inter_mode_rd_model_estimation == 1) {
      // Revert to type 2
      sf->inter_mode_rd_model_estimation = 2;
      sf->inter_mode_rd_model_estimation_adaptive = 0;
    }
  }
}
