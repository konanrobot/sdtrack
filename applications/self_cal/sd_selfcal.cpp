// Copyright (c) George Washington University, all rights reserved.  See the
// accompanying LICENSE file for more information.
#undef NDEBUG
#include <assert.h>
#include <Eigen/Eigen>
#include <glog/logging.h>
#include "GetPot"
#include <unistd.h>
#include <iomanip>

#include "etc_common.h"
#include <HAL/Camera/CameraDevice.h>
#include <sdtrack/TicToc.h>
#include <HAL/IMU/IMUDevice.h>
#include <HAL/Messages/Matrix.h>
#include <SceneGraph/SceneGraph.h>
#include <pangolin/pangolin.h>
#include <ba/BundleAdjuster.h>
#include <ba/InterpolationBuffer.h>
#include <sdtrack/utils.h>
#include "math_types.h"
#include "gui_common.h"
#include "CVars/CVar.h"
#include <thread>
#include "selfcal-cvars.h"
#include "chi2inv.h"

#ifdef CHECK_NANS
#include <xmmintrin.h>
#endif

#include <sdtrack/semi_dense_tracker.h>

#include "online_calibrator.h"

#define POSES_TO_INIT 30


uint32_t keyframe_tracks = UINT_MAX;
double start_time = 0;
uint32_t frame_count = 0;
Sophus::SE3d last_t_ba, prev_delta_t_ba, prev_t_ba;
// Self calibration params
uint32_t num_change_detected = 0;
uint32_t num_change_needed = 3;
bool unknown_cam_calibration = true;
bool unknown_imu_calibration = false;

bool compare_self_cal_with_batch = false;
bool do_self_cal = true;
bool do_imu_self_cal = false;
uint32_t num_self_cal_segments = 5;
uint32_t self_cal_segment_length = 10;

const int window_width = 640 * 1.5;
const int window_height = 480 * 1.5;
std::string g_usage = "SD SELFCAL. Example usage:\n"
    "-cam file:[loop=1]///Path/To/Dataset/[left,right]*pgm -cmod cameras.xml";
bool is_keyframe = true, is_prev_keyframe = true;
bool optimize_landmarks = true;
bool optimize_pose = true;
bool follow_camera = false;
bool is_running = false;
bool is_stepping = false;
bool is_manual_mode = false;
bool do_bundle_adjustment = true;
bool do_start_new_landmarks = true;
bool use_system_time = false;
int image_width;
int image_height;
calibu::Rig<Scalar> rig;
calibu::Rig<Scalar> selfcal_rig;
calibu::Rig<Scalar> aac_rig;
hal::Camera camera_device;
bool has_imu = false;
hal::IMU imu_device;
sdtrack::SemiDenseTracker tracker;

sdtrack::OnlineCalibrator online_calib;
std::vector<pangolin::DataLog> plot_logs;
std::vector<pangolin::Plotter*> plot_views;
std::vector<pangolin::Plotter*> analysis_views;
std::vector<pangolin::DataLog> analysis_logs;

TrackerGuiVars gui_vars;
pangolin::View* params_plot_view;
pangolin::View* imu_plot_view;
pangolin::View* analysis_plot_view;
std::shared_ptr<GetPot> cl;

// TrackCenterMap current_track_centers;
std::list<std::shared_ptr<sdtrack::DenseTrack>>* current_tracks = nullptr;
int last_optimization_level = 0;
// std::shared_ptr<sdtrack::DenseTrack> selected_track = nullptr;
std::shared_ptr<hal::Image> camera_img;
std::vector<std::vector<std::shared_ptr<SceneGraph::ImageView>>> patches;
std::vector<std::shared_ptr<sdtrack::TrackerPose>> poses;
std::vector<std::unique_ptr<SceneGraph::GLAxis>> axes;
std::shared_ptr<SceneGraph::GLPrimitives<>> line_strip;

// Inertial stuff
ba::BundleAdjuster<double, 1, 6, 0> bundle_adjuster;
ba::BundleAdjuster<double, 1, 15, 0> vi_bundle_adjuster;
ba::BundleAdjuster<double, 1, 15, 0> aac_bundle_adjuster;
ba::InterpolationBufferT<ba::ImuMeasurementT<Scalar>, Scalar> imu_buffer;
std::vector<uint32_t> ba_imu_residual_ids, aac_imu_residual_ids;
int orig_num_aac_poses = num_aac_poses;
double prev_cond_error;
int imu_cond_start_pose_id = -1;
int imu_cond_residual_id = -1;
std::shared_ptr<std::thread> aac_thread;
std::mutex aac_mutex;

sdtrack::CalibrationWindow pq_window;
sdtrack::CalibrationWindow candidate_window;
double last_window_kl_divergence = 0;
double last_added_window_kl_divergence = 0;
uint32_t unknown_cam_calibration_start_pose = 0;
uint32_t unknown_imu_calibration_start_pose = 0;
double total_last_frame_proj_norm = 0;

// State variables
std::vector<cv::KeyPoint> keypoints;
Sophus::SE3d guess;

void ImuCallback(const hal::ImuMsg& ref) {
  const double timestamp = use_system_time ? ref.system_time() :
                                             ref.device_time();
  Eigen::VectorXd a, w;
  hal::ReadVector(ref.accel(), &a);
  hal::ReadVector(ref.gyro(), &w);
  imu_buffer.AddElement(ba::ImuMeasurementT<Scalar>(w, a, timestamp));
  // std::cerr << "Added accel: " << a.transpose() << " and gyro " <<
  //              w.transpose() << " at time " << timestamp << std::endl;
}

template <typename BaType>
void DoBundleAdjustment(BaType& ba, bool use_imu,
                        bool do_adaptive_conditioning,
                        uint32_t& num_active_poses, uint32_t id,
                        std::vector<uint32_t>& imu_residual_ids,
                        calibu::Rig<Scalar>& ba_rig)
{
  std::vector<uint32_t> last_frame_proj_residual_ids;
  if (reset_outliers) {
    for (std::shared_ptr<sdtrack::TrackerPose> pose : poses) {
      for (std::shared_ptr<sdtrack::DenseTrack> track: pose->tracks) {
        track->is_outlier = false;
      }
    }
    reset_outliers = false;
  }

  imu_residual_ids.clear();
  ba::Options<double> options;
  options.gyro_sigma = gyro_sigma;
  options.accel_sigma = accel_sigma;
  options.accel_bias_sigma = accel_bias_sigma;
  options.gyro_bias_sigma = gyro_bias_sigma;
  options.use_dogleg = use_dogleg;
  options.use_sparse_solver = true;
  options.param_change_threshold = 1e-10;
  options.error_change_threshold = 1e-3;
  options.use_robust_norm_for_proj_residuals = use_robust_norm_for_proj;
  options.projection_outlier_threshold = outlier_threshold;
  options.use_per_pose_cam_params = true;
  options.regularize_biases_in_batch = poses.size() < POSES_TO_INIT ||
      regularize_biases_in_batch;


  uint32_t num_outliers = 0;
  Sophus::SE3d t_ba;
  uint32_t start_active_pose, start_pose_id;

  uint32_t end_pose_id;
  {
    std::lock_guard<std::mutex> lock(aac_mutex);
    end_pose_id = poses.size() - 1;

    GetBaPoseRange(poses, num_active_poses, start_pose_id, start_active_pose);

    if (start_pose_id == end_pose_id) {
      return;
    }

    // Add an extra pose to conditon the IMU
    if (use_imu && use_imu_measurements && start_active_pose == start_pose_id &&
        start_pose_id != 0) {
      start_pose_id--;
      std::cerr << "expanding sp from " << start_pose_id - 1 << " to " << start_pose_id << std::endl;
    }
  }

  bool all_poses_active = start_active_pose == start_pose_id;

  // Do a bundle adjustment on the current set
  if (current_tracks && end_pose_id) {
    {
      std::lock_guard<std::mutex> lock(aac_mutex);
      if (use_imu) {
        ba.SetGravity(gravity_vector);
      }

      ba.Init(options, end_pose_id + 1, current_tracks->size() *
              (end_pose_id + 1));
      for (uint32_t cam_id = 0; cam_id < ba_rig.cameras_.size(); ++cam_id) {
        ba.AddCamera(ba_rig.cameras_[cam_id]);
      }

      // First add all the poses and landmarks to ba.
      for (uint32_t ii = start_pose_id ; ii <= end_pose_id ; ++ii) {
        std::shared_ptr<sdtrack::TrackerPose> pose = poses[ii];
        pose->opt_id[id] = ba.AddPose(
              pose->t_wp, pose->cam_params, pose->v_w, pose->b,
              ii >= start_active_pose , pose->time);

        if (ii == start_active_pose && use_imu && all_poses_active) {
          ba.RegularizePose(pose->opt_id[id], true, true, false, false);
        }

        if (use_imu && ii >= start_active_pose && ii > 0) {
          std::vector<ba::ImuMeasurementT<Scalar>> meas =
              imu_buffer.GetRange(poses[ii - 1]->time, pose->time);

          /*std::cerr << "Adding imu residual between poses " << ii - 1 << std::setprecision(15) <<
                       " with time " << poses[ii - 1]->time << " active: " <<
                       ba.GetPose(poses[ii - 1]->opt_id[id]).is_active <<
                       " and " << ii << " with time " << pose->time <<
                       " active: " <<
                       ba.GetPose(poses[ii]->opt_id[id]).is_active <<
                       " with " << meas.size() << " measurements" << std::endl;*/

          imu_residual_ids.push_back(
                ba.AddImuResidual(poses[ii - 1]->opt_id[id],
                pose->opt_id[id], meas));
          if (do_adaptive_conditioning) {
            if (imu_cond_start_pose_id == -1 &&
                !ba.GetPose(poses[ii - 1]->opt_id[id]).is_active &&
                ba.GetPose(pose->opt_id[id]).is_active) {
              // std::cerr << "Setting cond pose id to " << ii - 1 << std::endl;
              imu_cond_start_pose_id = ii - 1;
              imu_cond_residual_id = imu_residual_ids.back();
              // std::cerr << "Setting cond residual id to " <<
              //              imu_cond_residual_id << std::endl;
            } else if ((uint32_t)imu_cond_start_pose_id == ii - 1) {
              imu_cond_residual_id = imu_residual_ids.back();
              // std::cerr << "Setting cond residual id to " <<
              //              imu_cond_residual_id << std::endl;
            }
          }
        }

        for (std::shared_ptr<sdtrack::DenseTrack> track: pose->tracks) {
          const bool constrains_active =
              track->keypoints.size() + ii > start_active_pose;
          if (track->num_good_tracked_frames <= 1 || track->is_outlier ||
              !constrains_active) {
            track->external_id[id] = UINT_MAX;
            continue;
          }
          Eigen::Vector4d ray;
          ray.head<3>() = track->ref_keypoint.ray;
          ray[3] = track->ref_keypoint.rho;
          ray = sdtrack::MultHomogeneous(pose->t_wp  * ba_rig.cameras_[0]->Pose(), ray);
          bool active = track->id != tracker.longest_track_id() ||
              !all_poses_active || use_imu;
          if (!active) {
            std::cerr << "Landmark " << track->id << " inactive. " << std::endl;
          }
          track->external_id[id] =
              ba.AddLandmark(ray, pose->opt_id[id], 0, active);
        }
      }

      // Now add all reprojections to ba)
      for (uint32_t ii = start_pose_id ; ii <= end_pose_id ; ++ii) {
        std::shared_ptr<sdtrack::TrackerPose> pose = poses[ii];
        uint32_t total_proj_res = 0;
        for (std::shared_ptr<sdtrack::DenseTrack> track : pose->tracks) {
          if (track->external_id[id] == UINT_MAX) {
            continue;
          }
          for (uint32_t cam_id = 0; cam_id < ba_rig.cameras_.size(); ++cam_id) {
            for (size_t jj = 0; jj < track->keypoints.size() ; ++jj) {
              if (track->keypoints[jj][cam_id].tracked) {
                const Eigen::Vector2d& z = track->keypoints[jj][cam_id].kp;
                if (ba.GetNumPoses() > (pose->opt_id[id] + jj)) {
                  const uint32_t res_id =
                      ba.AddProjectionResidual(
                        z, pose->opt_id[id] + jj,
                        track->external_id[id], cam_id, 2.0);

                  // Store reprojection constraint ids for the last frame.
                  if ((ii + jj) == end_pose_id) {
                    last_frame_proj_residual_ids.push_back(res_id);
                  }
                  total_proj_res++;
                }
              }
            }
          }
        }

        if (!do_adaptive_conditioning) {
          // std::cerr << "Total proj res for pose: " << ii << ": " <<
          //             total_proj_res << std::endl;
        }
      }
    }

    // Optimize the poses
    ba.Solve(num_ba_iterations);

    {
      std::lock_guard<std::mutex> lock(aac_mutex);
      total_last_frame_proj_norm = 0;

      ////ZZZZZZZZZZ THIS IS NOT THREAD SAFE
      // Calculate the average reprojection error.
      for (uint32_t id : last_frame_proj_residual_ids) {
        const auto& res = ba.GetProjectionResidual(id);
        total_last_frame_proj_norm += res.z.norm();
      }
      total_last_frame_proj_norm /= last_frame_proj_residual_ids.size();

      uint32_t last_pose_id =
          is_keyframe ? poses.size() - 1 : poses.size() - 2;
      std::shared_ptr<sdtrack::TrackerPose> last_pose = poses[last_pose_id];

      if (last_pose_id <= end_pose_id) {
        // Get the pose of the last pose. This is used to calculate the relative
        // transform from the pose to the current pose.
        last_pose->t_wp = ba.GetPose(last_pose->opt_id[id]).t_wp;
      }

      // Read out the pose and landmark values.
      for (uint32_t ii = start_pose_id ; ii <= end_pose_id ; ++ii) {
        std::shared_ptr<sdtrack::TrackerPose> pose = poses[ii];
        const ba::PoseT<double>& ba_pose =
            ba.GetPose(pose->opt_id[id]);

        pose->t_wp = ba_pose.t_wp;
        if (use_imu) {
          pose->v_w = ba_pose.v_w;
          pose->b = ba_pose.b;
        }

        // Here the last pose is actually t_wb and the current pose t_wa.
        last_t_ba = t_ba;
        t_ba = last_pose->t_wp.inverse() * pose->t_wp;
        for (std::shared_ptr<sdtrack::DenseTrack> track: pose->tracks) {
          if (track->external_id[id] == UINT_MAX) {
            continue;
          }
          track->t_ba = t_ba;

          // Get the landmark location in the world frame.
          const Eigen::Vector4d& x_w =
              ba.GetLandmark(track->external_id[id]);
          double ratio =
              ba.LandmarkOutlierRatio(track->external_id[id]);


          if (do_outlier_rejection && !unknown_cam_calibration &&
              poses.size() > POSES_TO_INIT) {
            if (ratio > 0.3 && track->tracked == false &&
                (end_pose_id >= min_poses_for_imu - 1 || !use_imu)) {
//            if (ratio > 0.3 &&
//                ((track->keypoints.size() == num_ba_poses - 1) ||
//                 track->tracked == false)) {
              num_outliers++;
              track->is_outlier = true;
            } else {
              track->is_outlier = false;
            }
          }

          Eigen::Vector4d prev_ray;
          prev_ray.head<3>() = track->ref_keypoint.ray;
          prev_ray[3] = track->ref_keypoint.rho;
          // Make the ray relative to the pose.
          Eigen::Vector4d x_r =
              sdtrack::MultHomogeneous(
                (pose->t_wp * ba_rig.cameras_[0]->Pose()).inverse(), x_w);
          // Normalize the xyz component of the ray to compare to the original
          // ray.
          x_r /= x_r.head<3>().norm();
          track->ref_keypoint.rho = x_r[3];
        }
      }

      if (follow_camera) {
        FollowCamera(gui_vars, poses.back()->t_wp);
      }
    }
  }
  if (!do_adaptive_conditioning) {
    std::cerr << "Rejected " << num_outliers << " outliers." << std::endl;
  }

  const ba::SolutionSummary<Scalar>& summary = ba.GetSolutionSummary();
  // std::cerr << "Rejected " << num_outliers << " outliers." << std::endl;

  if (use_imu && imu_cond_start_pose_id != -1 && do_adaptive_conditioning) {
    const uint32_t cond_dims =
        summary.num_cond_inertial_residuals * BaType::kPoseDim +
        summary.num_cond_proj_residuals * 2;
    const Scalar cond_error = summary.cond_inertial_error +
        summary.cond_proj_error;

    const double cond_inertial_error =
        ba.GetImuResidual(
          imu_cond_residual_id).mahalanobis_distance;

    if (prev_cond_error == -1) {
      prev_cond_error = DBL_MAX;
    }

    const Scalar cond_v_chi2_dist =
        chi2inv(adaptive_threshold, summary.num_cond_proj_residuals * 2);
    const Scalar cond_i_chi2_dist =
        chi2inv(adaptive_threshold, BaType::kPoseDim);

    if (num_active_poses > end_pose_id) {
      num_active_poses = orig_num_aac_poses;
      // std::cerr << "Reached batch solution. resetting number of poses to " <<
      //              num_ba_poses << std::endl;
    }

    if (cond_error == 0 || cond_dims == 0) {
      // status = OptStatus_NoChange;
    } else {
      const double cond_total_error =
          (cond_inertial_error + summary.cond_proj_error);
      const double inertial_ratio = cond_inertial_error / cond_i_chi2_dist;
      const double visual_ratio = summary.cond_proj_error / cond_v_chi2_dist;
      if ((inertial_ratio > 1.0 || visual_ratio > 1.0) &&
          (cond_total_error <= prev_cond_error) &&
          (((prev_cond_error - cond_total_error) / prev_cond_error) > 0.00001)) {
        num_active_poses += 30;
      } else {
        num_active_poses = orig_num_aac_poses;

      }
      prev_cond_error = cond_total_error;
    }
  }
}

void UpdateCurrentPose()
{
  std::shared_ptr<sdtrack::TrackerPose> current_pose = poses.back();
  if (poses.size() > 1) {
    current_pose->t_wp = poses[poses.size() - 2]->t_wp * tracker.t_ba().inverse();
  }

  // Also use the current tracks to update the index of the earliest covisible
  // pose.
  size_t max_track_length = 0;
  for (std::shared_ptr<sdtrack::DenseTrack>& track : tracker.GetCurrentTracks()) {
    max_track_length = std::max(track->keypoints.size(), max_track_length);
  }
  current_pose->longest_track = max_track_length;
  std::cerr << "Setting longest track for pose " << poses.size() << " to " <<
               current_pose->longest_track << std::endl;
}

void DoAAC()
{
  while (true) {
    if (has_imu && use_imu_measurements &&
        poses.size() > 10 && do_async_ba) {

      orig_num_aac_poses = num_aac_poses;
      while (true) {
        if (poses.size() > min_poses_for_imu &&
            use_imu_measurements && has_imu) {
          {
            std::lock_guard<std::mutex> lock(aac_mutex);
            aac_rig.cameras_[0]->SetParams(rig.cameras_[0]->GetParams());
            aac_rig.cameras_[0] = rig.cameras_[0];
          }
          DoBundleAdjustment(aac_bundle_adjuster, true, do_adaptive,
                             num_aac_poses, 1, aac_imu_residual_ids,
                             aac_rig);
        }

        if ((int)num_aac_poses == orig_num_aac_poses || !do_adaptive) {
          break;
        }

        usleep(10);
      }

      imu_cond_start_pose_id = -1;
      prev_cond_error = -1;
    }
    usleep(1000);
  }
}

void BaAndStartNewLandmarks()
{
  bool imu_selfcal_active = has_imu && use_imu_measurements && do_imu_self_cal;
  if (!is_keyframe) {
    return;
  }

  sdtrack::CalibrationWindow current_window;

  uint32_t keyframe_id = poses.size();
  // If we have no idea about the calibration, do batch mode.
  double batch_time = sdtrack::Tic();
  double ba_time = 0, analyze_time = 0, queue_time = 0, snl_time = 0;
  const uint32_t batch_start = unknown_cam_calibration_start_pose;
  const uint32_t batch_end = poses.size();
  if (do_self_cal && (unknown_cam_calibration || unknown_imu_calibration)
      && ((batch_end - batch_start) > self_cal_segment_length)) {
    bool window_analyzed = false;
    if (imu_selfcal_active && poses.size() > min_poses_for_imu &&
        unknown_imu_calibration) {
      online_calib.AnalyzeCalibrationWindow<true>(
            poses, current_tracks, batch_start,
            batch_end, pq_window, 50, true);
      window_analyzed = true;
    } else if(unknown_cam_calibration) {
      online_calib.AnalyzeCalibrationWindow<false>(
            poses, current_tracks, batch_start,
            batch_end, pq_window, 50, true);
      window_analyzed = true;
    }

    double score = window_analyzed ? online_calib.GetWindowScore(pq_window) : 0;

    if (pq_window.covariance.fullPivLu().rank() ==
        selfcal_rig.cameras_[0]->GetParams().rows() /*&& score < 1e7*/) {
      std::cerr << "Setting new batch params: " << std::endl;
      const Eigen::VectorXd new_params = selfcal_rig.cameras_[0]->GetParams();
      rig.cameras_[0]->SetParams(new_params);
      rig.cameras_[0] = selfcal_rig.cameras_[0];
      {
        std::unique_lock<std::mutex>(aac_mutex);
        for (uint32_t ii = unknown_cam_calibration_start_pose ;
             ii < poses.size() ; ++ii) {
          poses[ii]->cam_params = new_params;
          for (std::shared_ptr<sdtrack::DenseTrack> track: poses[ii]->tracks) {
            if (track->external_id[0] == UINT_MAX) {
              continue;
            }
            // We also have to backproject this track again.
            // tracker.BackProjectTrack(track);
            track->ref_keypoint.ray =
                rig.cameras_[0]->Unproject(
                  track->ref_keypoint.center_px).normalized();
            track->needs_backprojection = true;
          }
        }
      }
    }

    if (pq_window.mean.rows() != 0) {
      current_window = pq_window;
    }

    // Write this to the batch file.
    std::ofstream("batch.txt", std::ios_base::app) << keyframe_id << ", " <<
      pq_window.covariance.diagonal().transpose().format(
      sdtrack::kLongCsvFmt) << ", " << score << ", " <<
      pq_window.mean.transpose().format(sdtrack::kLongCsvFmt) << std::endl;

    std::cerr << "Batch means are: " << pq_window.mean.transpose() <<
                 std::endl;
    std::cerr << "Batch sigmas are:\n" <<
                 pq_window.covariance << std::endl;
    std::cerr << "Batch score: " << score << std::endl;

    // If the determinant is smaller than a heuristic, switch to self_cal.
    if ((score < 1e7 && score != 0 && !std::isnan(score) && !std::isinf(score)) ||
        ((batch_end - batch_start) > self_cal_segment_length * 2)) {
      std::cerr << "Determinant small enough, switching to self-cal" <<
                   std::endl;
      unknown_cam_calibration = false;
      unknown_imu_calibration = false;
    }
  }
  batch_time = sdtrack::Toc(batch_time);

  if (do_bundle_adjustment) {
    ba_time = sdtrack::Tic();
    uint32_t ba_size = std::max(num_ba_poses, unknown_cam_calibration ?
                                  batch_end - batch_start : num_ba_poses);
    if (has_imu && use_imu_measurements &&
        poses.size() > min_poses_for_imu) {
      std::cerr << "doing VI BA." << std::endl;
      DoBundleAdjustment(vi_bundle_adjuster, true, false, ba_size, 0,
                         ba_imu_residual_ids, rig);
    } else {
      std::cerr << "doing visual BA." << std::endl;
      DoBundleAdjustment(bundle_adjuster, false, false, ba_size, 0,
                         ba_imu_residual_ids, rig);
    }
    ba_time = sdtrack::Toc(ba_time);

    if (do_self_cal && (batch_end - batch_start) >= self_cal_segment_length) {
      analyze_time = sdtrack::Tic();
      uint32_t start_pose =
          std::max(0, (int)poses.size() - (int)self_cal_segment_length);
      uint32_t end_pose = poses.size();
      if (imu_selfcal_active) {
        online_calib.AnalyzeCalibrationWindow<true>(
              poses, current_tracks, start_pose, end_pose,
              candidate_window, 50);
      } else {
        online_calib.AnalyzeCalibrationWindow<false>(
              poses, current_tracks, start_pose, end_pose,
              candidate_window, 50);
      }

      online_calib.AnalyzeCalibrationWindow(candidate_window);
      last_window_kl_divergence =
          online_calib.ComputeYao1965(
            pq_window, candidate_window);

      if (candidate_window.mean.rows() != 0) {
        current_window = candidate_window;
      }

      std::cerr << "KL divergence for last window: " <<
                   last_window_kl_divergence << " num change: " <<
                   num_change_detected << std::endl;
      if (isnan(last_window_kl_divergence) ||
          isinf(last_window_kl_divergence)) {
        last_window_kl_divergence = 0;
      }

      if (last_window_kl_divergence < 0.2 && last_window_kl_divergence != 0 &&
          (online_calib.NumWindows() == online_calib.queue_length()) &&
          !unknown_cam_calibration) {
        num_change_detected++;

        if (num_change_detected > num_change_needed) {
          std::cerr << "PARAM CHANGE DETECTED" << std::endl;
          unknown_cam_calibration = true;
          unknown_cam_calibration_start_pose = poses.size() - num_change_needed;
          std::cerr << "Unknown calibration = true with start pose " <<
                       unknown_cam_calibration_start_pose << std::endl;
          online_calib.ClearQueue();
        }
      } else {
        num_change_detected = 0;
      }
      analyze_time = sdtrack::Toc(analyze_time);

      // If the priority queue was modified, calculate the new results for it.
      if (online_calib.needs_update() && !unknown_cam_calibration) {
        queue_time = sdtrack::Toc(ba_time);
        last_added_window_kl_divergence = last_window_kl_divergence;
        const bool apply_results =
            !(unknown_cam_calibration || unknown_imu_calibration);
        if (imu_selfcal_active) {
          online_calib.AnalyzePriorityQueue<true>(
                poses, current_tracks, pq_window, 50, apply_results);
        } else {
          online_calib.AnalyzePriorityQueue<false>(
                poses, current_tracks, pq_window, 50, apply_results);
        }
        if (apply_results) {
          std::unique_lock<std::mutex>(aac_mutex);
          const Eigen::VectorXd new_params =
              selfcal_rig.cameras_[0]->GetParams();
          rig.cameras_[0]->SetParams(new_params);
          for (size_t ii = unknown_cam_calibration_start_pose;
               ii < poses.size(); ++ii) {
            for (std::shared_ptr<sdtrack::DenseTrack> track : poses[ii]->tracks) {
              poses[ii]->cam_params = new_params;
              track->ref_keypoint.ray =
                  rig.cameras_[0]->Unproject(
                    track->ref_keypoint.center_px).normalized();
              // tracker.BackProjectTrack(track);
              track->needs_backprojection = true;
            }
          }
        }
        std::cerr << "Analyzed priority queue with mean " <<
                     pq_window.mean.transpose() << " and cov\n " <<
                     pq_window.covariance << std::endl;
        online_calib.SetPriorityQueueDistribution(pq_window.covariance,
                                                  pq_window.mean);


        const double score =
            online_calib.GetWindowScore(pq_window);

        // Write this to the pq file.
        std::ofstream("pq.txt", std::ios_base::app) << keyframe_id << ", " <<
          pq_window.covariance.diagonal().transpose().format(
            sdtrack::kLongCsvFmt) << ", " << score << ", " <<
          pq_window.mean.transpose().format(sdtrack::kLongCsvFmt) << ", " <<
          last_window_kl_divergence << std::endl;

        if (compare_self_cal_with_batch && !unknown_cam_calibration) {
          // Also analyze the full batch solution.
          sdtrack::CalibrationWindow batch_window;
          if (imu_selfcal_active) {
            online_calib.AnalyzeCalibrationWindow<true>(
                  poses, current_tracks, 0, poses.size(), batch_window, 50);
          } else {
            online_calib.AnalyzeCalibrationWindow<false>(
                  poses, current_tracks, 0, poses.size(), batch_window, 50);
          }

          const double batch_score =
              online_calib.GetWindowScore(batch_window);

          // Write this to the batch file.
          std::ofstream("batch.txt", std::ios_base::app) << keyframe_id << ", " <<
            batch_window.covariance.diagonal().transpose().format(
            sdtrack::kLongCsvFmt) << ", " << batch_score << ", " <<
            batch_window.mean.transpose().format(sdtrack::kLongCsvFmt) <<
            std::endl;

          std::cerr << "Batch means are: " << batch_window.mean.transpose() <<
                       std::endl;
          std::cerr << "Batch sigmas are:\n" <<
                       batch_window.covariance << std::endl;
          std::cerr << "Batch score: " << batch_score << std::endl;
        }

        queue_time = sdtrack::Toc(queue_time);
      }
    }

    if (do_self_cal && current_window.mean.rows() != 0) {
      std::ofstream("sigmas.txt", std::ios_base::app) << keyframe_id << ", " <<
        current_window.covariance.diagonal(). transpose().format(
          sdtrack::kLongCsvFmt) << ", " <<
        current_window.mean.transpose().format(sdtrack::kLongCsvFmt) <<
        ", " << last_window_kl_divergence << ", " << current_window.score <<
        std::endl;
    }
  }

  if (do_start_new_landmarks) {
    snl_time = sdtrack::Tic();
    tracker.StartNewLandmarks();
    snl_time = sdtrack::Toc(snl_time);
  }

  std::cerr << "Timings batch: " << batch_time << " ba: " << ba_time <<
               " analyze: " << analyze_time << " queue: " << queue_time <<
               " snl: " << snl_time << std::endl;

  std::ofstream("timings.txt", std::ios_base::app) << keyframe_id << ", " <<
    batch_time << ", " << ba_time << ", " << analyze_time << ", " <<
    queue_time << ", " << snl_time << std::endl;

  std::shared_ptr<sdtrack::TrackerPose> new_pose = poses.back();
  // Update the tracks on this new pose.
  new_pose->tracks = tracker.GetNewTracks();

  if (!do_bundle_adjustment) {
    tracker.TransformTrackTabs(tracker.t_ba());
  }
}

void ProcessImage(std::vector<cv::Mat>& images, double timestamp)
{
  bundle_adjuster.debug_level_threshold = ba_debug_level;
  vi_bundle_adjuster.debug_level_threshold = vi_ba_debug_level;
  aac_bundle_adjuster.debug_level_threshold = aac_ba_debug_level;
  online_calib.SetBaDebugLevel(selfcal_ba_debug_level);

#ifdef CHECK_NANS
  _MM_SET_EXCEPTION_MASK(_MM_GET_EXCEPTION_MASK() &
                         ~(_MM_MASK_INVALID | _MM_MASK_OVERFLOW |
                           _MM_MASK_DIV_ZERO));
#endif

  if (frame_count == 0) {
    start_time = sdtrack::Tic();
  }
  frame_count++;
//  if (poses.size() > 100) {
//    std::cerr << "last_pose\n " << poses.back()->t_wp.matrix().format(
//                   sdtrack::kLongFmt) << std::endl;
//    exit(EXIT_SUCCESS);
//  }


  // If this is a keyframe, set it as one on the tracker.
  prev_delta_t_ba = tracker.t_ba() * prev_t_ba.inverse();

  if (is_prev_keyframe) {
    prev_t_ba = Sophus::SE3d();
  } else {
    prev_t_ba = tracker.t_ba();
  }

  // Add a pose to the poses array
  if (is_prev_keyframe) {
    std::shared_ptr<sdtrack::TrackerPose> new_pose(new sdtrack::TrackerPose);
    if (poses.size() > 0) {
      new_pose->t_wp = poses.back()->t_wp * last_t_ba.inverse();
      if (use_imu_measurements && has_imu) {
        new_pose->v_w = poses.back()->v_w;
        new_pose->b = poses.back()->b;
      }
    } else {
      if (has_imu && use_imu_measurements && imu_buffer.elements.size() > 0) {
        Eigen::Vector3t down = -imu_buffer.elements.front().a.normalized();
        std::cerr << "Down vector based on first imu meas: " <<
                     down.transpose() << std::endl;

        // compute path transformation
        Eigen::Vector3t forward(1.0, 0.0, 0.0);
        Eigen::Vector3t right = down.cross(forward);
        right.normalize();
        forward = right.cross(down);
        forward.normalize();

        Eigen::Matrix4t base = Eigen::Matrix4t::Identity();
        base.block<1, 3>(0, 0) = forward;
        base.block<1, 3>(1, 0) = right;
        base.block<1, 3>(2, 0) = down;
        new_pose->t_wp = Sophus::SE3t(base);
      }
      // Set the initial velocity and bias. The initial pose is initialized to
      // align the gravity plane
      new_pose->v_w.setZero();
      new_pose->b.setZero();
    }

    {
      std::unique_lock<std::mutex>(aac_mutex);
      new_pose->cam_params = rig.cameras_[0]->GetParams();
      poses.push_back(new_pose);
    }
    axes.push_back(std::unique_ptr<SceneGraph::GLAxis>(
                      new SceneGraph::GLAxis(0.2)));
    gui_vars.scene_graph.AddChild(axes.back().get());
  }

  // Set the timestamp of the latest pose to this image's timestamp.
  poses.back()->time = timestamp + imu_time_offset;

  if (((double)tracker.num_successful_tracks() / (double)num_features) > 0.3) {
    guess = prev_delta_t_ba * prev_t_ba;
  } else {
    guess = Sophus::SE3d();
  }

  if(guess.translation() == Eigen::Vector3d(0,0,0) &&
     poses.size() > 1) {
    guess.translation() = Eigen::Vector3d(0, 0, 0.001);
  }

  if (use_imu_measurements &&
      use_imu_for_guess && poses.size() >= min_poses_for_imu) {
    std::shared_ptr<sdtrack::TrackerPose> pose1 = poses[poses.size() - 2];
    std::shared_ptr<sdtrack::TrackerPose> pose2 = poses.back();
    std::vector<ba::ImuPoseT<Scalar>> imu_poses;
    ba::PoseT<Scalar> start_pose;
    start_pose.t_wp = pose1->t_wp;
    start_pose.b = pose1->b;
    start_pose.v_w = pose1->v_w;
    start_pose.time = pose1->time;
    // Integrate the measurements since the last frame.
    std::vector<ba::ImuMeasurementT<Scalar> > meas =
        imu_buffer.GetRange(pose1->time, pose2->time);
    decltype(vi_bundle_adjuster)::ImuResidual::IntegrateResidual(
          start_pose, meas, start_pose.b.head<3>(), start_pose.b.tail<3>(),
          vi_bundle_adjuster.GetImuCalibration().g_vec, imu_poses);

    if (imu_poses.size() > 1) {
      ba::ImuPoseT<Scalar>& last_pose = imu_poses.back();
      guess = last_pose.t_wp.inverse() *
          imu_poses.front().t_wp;
      pose2->t_wp = last_pose.t_wp;
      pose2->v_w = last_pose.v_w;
      poses.back()->t_wp = pose2->t_wp;
      poses.back()->v_w = pose2->v_w;
      poses.back()->b = pose2->b;
    }
  }

  std::cerr << "Guess:\n " << guess.matrix() << std::endl;

  bool tracking_failed = false;
  {
    std::lock_guard<std::mutex> lock(aac_mutex);

    tracker.AddImage(images, guess);
    tracker.EvaluateTrackResiduals(0, tracker.GetImagePyramid(),
                                   tracker.GetCurrentTracks());

    if (!is_manual_mode) {
      tracker.OptimizeTracks(-1, optimize_landmarks, optimize_pose);
    }
    tracker.PruneTracks();

    if ((tracker.num_successful_tracks() < 10) &&
        has_imu && use_imu_measurements) {
      std::cerr << "Tracking failed. using guess." << std::endl;
      tracking_failed = true;
      tracker.set_t_ba(guess);
    }

    // Update the pose t_ab based on the result from the tracker.
    UpdateCurrentPose();
    if (follow_camera) {
      FollowCamera(gui_vars, poses.back()->t_wp);
    }
  }

  if (do_keyframing) {
    const double track_ratio = (double)tracker.num_successful_tracks() /
        (double)keyframe_tracks;
    const double total_trans = tracker.t_ba().translation().norm();
    const double total_rot = tracker.t_ba().so3().log().norm();

    bool keyframe_condition = track_ratio < 0.8 || total_trans > 0.2 ||
        total_rot > 0.1 /*|| tracker.num_successful_tracks() < 64*/;

    std::cerr << "\tRatio: " << track_ratio << " trans: " << total_trans <<
                 " rot: " << total_rot << std::endl;

    {
      std::lock_guard<std::mutex> lock(aac_mutex);
      if (keyframe_tracks != 0) {
        if (keyframe_condition) {
          is_keyframe = true;
        } else {
          is_keyframe = false;
        }
      }

      // If this is a keyframe, set it as one on the tracker.
      prev_delta_t_ba = tracker.t_ba() * prev_t_ba.inverse();

      if (is_keyframe) {
        tracker.AddKeyframe();
      }
      is_prev_keyframe = is_keyframe;
    }
  } else {
    std::lock_guard<std::mutex> lock(aac_mutex);
    tracker.AddKeyframe();
  }

  std::cerr << "Num successful : " << tracker.num_successful_tracks() <<
               " keyframe tracks: " << keyframe_tracks << std::endl;

  if (!is_manual_mode) {
    BaAndStartNewLandmarks();
  }

  if (do_self_cal || unknown_cam_calibration || unknown_imu_calibration) {
    const bool imu_plots_needed = has_imu && use_imu_measurements &&
        do_imu_self_cal;
    const uint32_t num_cam_params = rig.cameras_[0]->NumParams();
    if (candidate_window.mean.rows() == 0) {
      candidate_window.mean = rig.cameras_[0]->GetParams();
    }

    for (size_t ii = 0; ii < num_cam_params; ++ii) {
      plot_logs[ii].Log(rig.cameras_[0]->GetParams()[ii],
//         old_rig.cameras[0].camera.GenericParams()[ii],
          candidate_window.mean[ii]);
    }

    if (imu_plots_needed) {
      Eigen::Vector6d error =
          (rig.cameras_[0]->Pose().inverse() * rig.cameras_[0]->Pose()).log();
      for (size_t ii = num_cam_params; ii < num_cam_params + 6; ++ii) {
        plot_logs[ii].Log(error[ii - num_cam_params]);
      }
    }

    analysis_logs[0].Log(last_window_kl_divergence,
                         last_added_window_kl_divergence);
    analysis_logs[1].Log(tracker.num_successful_tracks());
    analysis_logs[2].Log(total_last_frame_proj_norm);
  }

  if (is_keyframe) {
    std::cerr << "KEYFRAME." << std::endl;
    keyframe_tracks = tracker.GetCurrentTracks().size();
    std::cerr << "New keyframe tracks: " << keyframe_tracks << std::endl;
  } else {
    std::cerr << "NOT KEYFRAME." << std::endl;
  }

  current_tracks = &tracker.GetCurrentTracks();

#ifdef CHECK_NANS
  _MM_SET_EXCEPTION_MASK(_MM_GET_EXCEPTION_MASK() |
                         (_MM_MASK_INVALID | _MM_MASK_OVERFLOW |
                          _MM_MASK_DIV_ZERO));
#endif

  std::cerr << "FRAME : " << frame_count << " KEYFRAME: " << poses.size() <<
               " FPS: " << frame_count / sdtrack::Toc(start_time) << std::endl;
}

void DrawImageData(uint32_t cam_id)
{
  if (cam_id == 0) {
    gui_vars.handler->track_centers.clear();
  }

  SceneGraph::AxisAlignedBoundingBox aabb;
  line_strip->Clear();
  for (uint32_t ii = 0; ii < poses.size() ; ++ii) {
    axes[ii]->SetPose(poses[ii]->t_wp.matrix());
    aabb.Insert(poses[ii]->t_wp.translation());
    Eigen::Vector3f vertex = poses[ii]->t_wp.translation().cast<float>();
    line_strip->AddVertex(vertex);
  }
  gui_vars.grid.set_bounds(aabb);

  // Draw the tracks
  for (std::shared_ptr<sdtrack::DenseTrack>& track : *current_tracks) {
    Eigen::Vector2d center;
    if (track->keypoints.back()[cam_id].tracked ||
        track->keypoints.size() <= 2 ) {
      DrawTrackData(track, image_width, image_height, center,
                    gui_vars.handler->selected_track == track, cam_id);
    }
    if (cam_id == 0) {
      gui_vars.handler->track_centers.push_back(
            std::pair<Eigen::Vector2d, std::shared_ptr<sdtrack::DenseTrack>>(
              center, track));
    }
  }

  // Populate the first column with the reference from the selected track.
  if (gui_vars.handler->selected_track != nullptr) {
    DrawTrackPatches(gui_vars.handler->selected_track, gui_vars.patches);
  }

  for (size_t cam_id = 0; cam_id < rig.cameras_.size(); ++cam_id) {
    gui_vars.camera_view[cam_id]->RenderChildren();
  }
}

void Run()
{
  std::vector<pangolin::GlTexture> gl_tex;

  // pangolin::Timer timer;
  bool capture_success = false;
  std::shared_ptr<hal::ImageArray> images = hal::ImageArray::Create();
  camera_device.Capture(*images);
  while(!pangolin::ShouldQuit()) {
    capture_success = false;
    const bool go = is_stepping;
    if (!is_running) {
      is_stepping = false;
    }
    // usleep(20000);

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glColor4f(1.0f,1.0f,1.0f,1.0f);

    if (go) {
      capture_success = camera_device.Capture(*images);
    }

    if (capture_success) {
      double timestamp = use_system_time ? images->Ref().system_time() :
                                           images->Ref().device_time();

      // Wait until we have enough measurements to interpolate this frame's
      // timestamp
      if (has_imu && use_imu_measurements) {
        const double start_time = sdtrack::Tic();
        while (imu_buffer.end_time < timestamp &&
               sdtrack::Toc(start_time) < 0.1) {
          usleep(10);
        }
      }

      gl_tex.resize(images->Size());

      for (int cam_id = 0 ; cam_id < images->Size() ; ++cam_id) {
        if (!gl_tex[cam_id].tid) {
          camera_img = images->at(cam_id);
          GLint internal_format = (camera_img->Format() == GL_LUMINANCE ?
                                     GL_LUMINANCE : GL_RGBA);
          // Only initialise now we know format.
          gl_tex[cam_id].Reinitialise(
                camera_img->Width(), camera_img->Height(), internal_format,
                false, 0, camera_img->Format(), camera_img->Type(), 0);
        }
      }

      camera_img = images->at(0);
      image_width = camera_img->Width();
      image_height = camera_img->Height();
      gui_vars.handler->image_height = image_height;
      gui_vars.handler->image_width = image_width;

      std::vector<cv::Mat> cvmat_images;
      for (int ii = 0; ii < images->Size() ; ++ii) {
        cvmat_images.push_back(images->at(ii)->Mat());
      }
      ProcessImage(cvmat_images, timestamp);
    }

    if (camera_img && camera_img->data()) {
      for (size_t cam_id = 0 ; cam_id < rig.cameras_.size() &&
           cam_id < (uint32_t)images->Size(); ++cam_id) {
        camera_img = images->at(cam_id);
        gui_vars.camera_view[cam_id]->ActivateAndScissor();
        gl_tex[cam_id].Upload(camera_img->data(), camera_img->Format(),
                      camera_img->Type());
        gl_tex[cam_id].RenderToViewportFlipY();
        DrawImageData(cam_id);
      }

      gui_vars.grid_view->ActivateAndScissor(gui_vars.gl_render3d);

      const ba::ImuCalibrationT<Scalar>& imu =
          vi_bundle_adjuster.GetImuCalibration();
      std::vector<ba::ImuPoseT<Scalar>> imu_poses;

      glLineWidth(1.0f);
      // Draw the inertial residual

      for (uint32_t id : ba_imu_residual_ids) {
        const ba::ImuResidualT<Scalar>& res = vi_bundle_adjuster.GetImuResidual(id);
        const ba::PoseT<Scalar>& pose = vi_bundle_adjuster.GetPose(res.pose1_id);
        std::vector<ba::ImuMeasurementT<Scalar> > meas =
            imu_buffer.GetRange(res.measurements.front().time,
                                res.measurements.back().time +
                                imu_extra_integration_time);
        res.IntegrateResidual(pose, meas, pose.b.head<3>(), pose.b.tail<3>(),
                              imu.g_vec, imu_poses);
        // std::cerr << "integrating residual with " << res.measurements.size() <<
        //              " measurements " << std::endl;
        if (pose.is_active) {
          glColor3f(1.0, 0.0, 1.0);
        } else {
          glColor3f(1.0, 0.2, 0.5);
        }

        for (size_t ii = 1 ; ii < imu_poses.size() ; ++ii) {
          ba::ImuPoseT<Scalar>& prev_imu_pose = imu_poses[ii - 1];
          ba::ImuPoseT<Scalar>& imu_pose = imu_poses[ii];
          pangolin::glDrawLine(prev_imu_pose.t_wp.translation()[0],
              prev_imu_pose.t_wp.translation()[1],
              prev_imu_pose.t_wp.translation()[2],
              imu_pose.t_wp.translation()[0],
              imu_pose.t_wp.translation()[1],
              imu_pose.t_wp.translation()[2]);
        }
      }

      if (draw_landmarks) {
        DrawLandmarks(min_lm_measurements_for_drawing, poses, rig,
                      gui_vars.handler, selected_track_id);
      }
    }
    pangolin::FinishFrame();
  }
}

void InitGui() {
  InitTrackerGui(gui_vars, window_width, window_height , image_width,
                 image_height, rig.cameras_.size());
  line_strip.reset(new SceneGraph::GLPrimitives<>);
  gui_vars.scene_graph.AddChild(line_strip.get());

  pangolin::RegisterKeyPressCallback(
        pangolin::PANGO_SPECIAL + pangolin::PANGO_KEY_RIGHT,
        [&]() {
    is_stepping = true;
  });

  pangolin::RegisterKeyPressCallback(
        pangolin::PANGO_CTRL + 's',
        [&]() {
    // write all the poses to a file.
    std::ofstream pose_file("poses.txt", std::ios_base::trunc);
    for (auto pose : poses) {
      pose_file << pose->t_wp.translation().transpose().format(
      sdtrack::kLongCsvFmt) << std::endl;
    }

    std::ofstream lm_file("landmarks.txt", std::ios_base::trunc);
    for (std::shared_ptr<sdtrack::TrackerPose> pose : poses) {
      for (std::shared_ptr<sdtrack::DenseTrack> track : pose->tracks) {
        if ((int)track->num_good_tracked_frames < min_lm_measurements_for_drawing) {
          continue;
        }
        Eigen::Vector4d ray;
        ray.head<3>() = track->ref_keypoint.ray;
        ray[3] = track->ref_keypoint.rho;
        ray = sdtrack::MultHomogeneous(pose->t_wp * rig.cameras_[0]->Pose(), ray);
        ray /= ray[3];
        lm_file << ray.transpose().format(sdtrack::kLongCsvFmt) << std::endl;
      }
    }
  });

  pangolin::RegisterKeyPressCallback('r', [&]() {
    for (uint32_t ii = 0; ii < plot_views.size(); ++ii) {
      plot_views[ii]->Keyboard(*plot_views[ii], 'a', 0, 0, true);
    }

    for (uint32_t ii = 0; ii < analysis_views.size(); ++ii) {
      analysis_views[ii]->Keyboard(*analysis_views[ii], 'a', 0, 0, true);
    }
  });

  pangolin::RegisterKeyPressCallback(' ', [&]() {
    is_running = !is_running;
  });

  pangolin::RegisterKeyPressCallback('f', [&]() {
    follow_camera = !follow_camera;
  });

  pangolin::RegisterKeyPressCallback('c', [&]() {
    do_self_cal = !do_self_cal;
  });

  pangolin::RegisterKeyPressCallback('u', [&]() {
    unknown_cam_calibration = true;
    unknown_cam_calibration_start_pose = poses.size() - 2;
    std::cerr << "Unknown calibration = true with start pose " <<
                 unknown_cam_calibration_start_pose << std::endl;
    online_calib.ClearQueue();
  });

  pangolin::RegisterKeyPressCallback('b', [&]() {
    last_optimization_level = 0;
    tracker.OptimizeTracks();
  });

  pangolin::RegisterKeyPressCallback('B', [&]() {
    do_bundle_adjustment = !do_bundle_adjustment;
    std::cerr << "Do BA:" << do_bundle_adjustment << std::endl;
  });

  pangolin::RegisterKeyPressCallback('k', [&]() {
    is_keyframe = !is_keyframe;
    std::cerr << "is_keyframe:" << is_keyframe << std::endl;
  });

  pangolin::RegisterKeyPressCallback('S', [&]() {
    do_start_new_landmarks = !do_start_new_landmarks;
    std::cerr << "Do SNL:" << do_start_new_landmarks << std::endl;
  });

  pangolin::RegisterKeyPressCallback('2', [&]() {
    last_optimization_level = 2;
    tracker.OptimizeTracks(last_optimization_level, optimize_landmarks,
                           optimize_pose);
    UpdateCurrentPose();
  });

  pangolin::RegisterKeyPressCallback('3', [&]() {
    last_optimization_level = 3;
    tracker.OptimizeTracks(last_optimization_level, optimize_landmarks,
                           optimize_pose);
    UpdateCurrentPose();
  });

  pangolin::RegisterKeyPressCallback('1', [&]() {
    last_optimization_level = 1;
    tracker.OptimizeTracks(last_optimization_level, optimize_landmarks,
                           optimize_pose);
    UpdateCurrentPose();
  });

  pangolin::RegisterKeyPressCallback('0', [&]() {
    last_optimization_level = 0;
    tracker.OptimizeTracks(last_optimization_level, optimize_landmarks,
                           optimize_pose);
    UpdateCurrentPose();
  });

  pangolin::RegisterKeyPressCallback('9', [&]() {
    last_optimization_level = 0;
    tracker.OptimizeTracks(-1, optimize_landmarks,
                           optimize_pose);
    UpdateCurrentPose();
  });

  pangolin::RegisterKeyPressCallback('p', [&]() {
    tracker.PruneTracks();
    // Update the pose t_ab based on the result from the tracker.
    UpdateCurrentPose();
    BaAndStartNewLandmarks();
  });

  pangolin::RegisterKeyPressCallback('l', [&]() {
    optimize_landmarks = !optimize_landmarks;
    std::cerr << "optimize landmarks: " << optimize_landmarks << std::endl;
  });

  pangolin::RegisterKeyPressCallback('c', [&]() {
    optimize_pose = !optimize_pose;
    std::cerr << "optimize pose: " << optimize_pose << std::endl;
  });

  pangolin::RegisterKeyPressCallback('m', [&]() {
    is_manual_mode = !is_manual_mode;
    std::cerr << "Manual mode:" << is_manual_mode << std::endl;
  });

  // set up the plotters.
  if (do_self_cal) {
    params_plot_view = &pangolin::Display("plot").SetLayout(
          pangolin::LayoutEqualVertical);
    pangolin::Display("multi").AddDisplay(*params_plot_view);

    const bool imu_plots_needed = has_imu && use_imu_measurements &&
        do_imu_self_cal;
    const uint32_t num_cam_params = rig.cameras_[0]->NumParams();
    const uint32_t num_plots = num_cam_params + (imu_plots_needed ? 6 : 0);

    plot_views.resize(num_plots);
    plot_logs.resize(num_plots);

    plot_logs[0].SetLabels({"fx - p.q.", "fx - candidate seg."});
    plot_logs[1].SetLabels({"fy - p.q.", "fy - candidate seg."});
    plot_logs[2].SetLabels({"cx - p.q.", "cx - candidate seg."});
    plot_logs[3].SetLabels({"cy - p.q.", "cy - candidate seg."});
    plot_logs[4].SetLabels({"w - p.q.", "w - candidate seg."});

    for (size_t ii = 0; ii < num_cam_params; ++ii) {
      plot_views[ii] = new pangolin::Plotter(&plot_logs[ii]);
      params_plot_view->AddDisplay(*plot_views[ii]);
      double param = rig.cameras_[0]->GetParams()[ii];
      pangolin::XYRange range(0, 500, param - param * 0.5,
                              param + param * 0.5);
      plot_views[ii]->SetDefaultView(range);
      plot_views[ii]->SetViewSmooth(range);
      plot_views[ii]->ToggleTracking();
    }


    // Add the t_vs displays.
    if (imu_plots_needed) {
      imu_plot_view = &pangolin::Display("imu_plot").SetLayout(
            pangolin::LayoutEqualVertical);
      pangolin::Display("multi").AddDisplay(*imu_plot_view);

      for (size_t ii = num_cam_params; ii < 6 + num_cam_params; ++ii) {
        plot_views[ii] = new pangolin::Plotter(&plot_logs[ii]);
        imu_plot_view->AddDisplay(*plot_views[ii]);
        pangolin::XYRange range(0, 500, -0.5, 0.5);
        plot_views[ii]->SetDefaultView(range);
        plot_views[ii]->SetViewSmooth(range);
        plot_views[ii]->ToggleTracking();
      }
    }

    analysis_plot_view = &pangolin::Display("analysis_plot").SetLayout(
          pangolin::LayoutEqualVertical);
    pangolin::Display("multi").AddDisplay(*analysis_plot_view);

    analysis_views.resize(3);
    analysis_logs.resize(3);

    analysis_logs[0].SetLabels({"p-value (candidate seg.)",
                                "p-value (last p.q. window)"});
    analysis_logs[1].SetLabels({"num. successful tracks"});
    analysis_logs[2].SetLabels({"last frame mean reproj. error"});

    for (size_t ii = 0; ii < analysis_views.size(); ++ii) {
      analysis_views[ii] = new pangolin::Plotter(&analysis_logs[ii]);
      analysis_plot_view->AddDisplay(*analysis_views[ii]);
      analysis_views[ii]->ToggleTracking();
    }
  }
}

bool LoadCameras(GetPot& cl)
{
  LoadCameraAndRig(cl, camera_device, rig);

  for (uint32_t cam_id = 0; cam_id < rig.cameras_.size(); ++cam_id) {
    selfcal_rig.AddCamera(rig.cameras_[cam_id]);
    aac_rig.AddCamera(rig.cameras_[cam_id]);
  }

  // Load the imu
  std::string imu_str = cl.follow("","-imu");
  if (!imu_str.empty()) {
    try {
      imu_device = hal::IMU(imu_str);
    } catch (hal::DeviceException& e) {
      LOG(ERROR) << "Error loading imu device: " << e.what()
                 << " ... proceeding without.";
    }
    has_imu = true;
    imu_device.RegisterIMUDataCallback(&ImuCallback);
  }


  // If we require self-calibration from an unknown initial calibration, then
  // perturb the values (camera calibraiton parameters only)
  if (unknown_cam_calibration) {
    Eigen::VectorXd params = rig.cameras_[0]->GetParams();
    // fov in rads.
    const double fov_rads = 90 * M_PI / 180.0;
    const double f_x =
        0.5 * rig.cameras_[0]->Height() / tan(fov_rads / 2);
    std::cerr << "Changing fx from " << params[0] << " to " << f_x << std::endl;
    std::cerr << "Changing fy from " << params[1] << " to " << f_x << std::endl;
    params[0] = f_x;
    params[1] = f_x;
    params[2] = rig.cameras_[0]->Width() / 2;
    params[3] = rig.cameras_[0]->Height() / 2;
    if (params.rows() > 4) {
      params[4] = 1.0;
    }


    rig.cameras_[0]->SetParams(params);
    selfcal_rig.cameras_[0]->SetParams(params);
    aac_rig.cameras_[0]->SetParams(params);
    // Add a marker in the batch file for this initial, unknown calibration.
    Eigen::VectorXd initial_covariance(params.rows());
    initial_covariance.setOnes();
    std::ofstream("batch.txt", std::ios_base::app) << 0 << ", " <<
      initial_covariance.transpose().format(sdtrack::kLongCsvFmt) <<
      ", " << 0 << ", " << params.transpose().format(sdtrack::kLongCsvFmt) <<
      std::endl;
  }

  if (has_imu && unknown_imu_calibration) {

    rig.cameras_[0]->Pose().so3() = rig.cameras_[0]->Pose().so3() *
        Sophus::SO3d::exp(
          (Eigen::Vector3d() << 0.1, 0.2, 0.3).finished());

    for (uint32_t cam_id = 0; cam_id < rig.cameras_.size(); ++cam_id) {
      selfcal_rig.cameras_[cam_id]->Pose() = rig.cameras_[cam_id]->Pose();
      aac_rig.cameras_[cam_id]->Pose() = aac_rig.cameras_[cam_id]->Pose();
    }

  }


  if (has_imu && use_imu_measurements) {
    // Capture an image so we have some IMU data.
    std::shared_ptr<hal::ImageArray> images = hal::ImageArray::Create();
    while (imu_buffer.elements.size() == 0) {
      camera_device.Capture(*images);
    }
  }

  return true;
}

int main(int argc, char** argv) {
  // Clear the log files.
  {
    std::ofstream sigmas_file("sigmas.txt", std::ios_base::trunc);
    std::ofstream pq_file("pq.txt", std::ios_base::trunc);
    std::ofstream batch_file("batch.txt", std::ios_base::trunc);
    std::ofstream pose_file("timings.txt", std::ios_base::trunc);
  }
  srand(0);
  google::InitGoogleLogging(argv[0]);

  GetPot cl(argc, argv);
  if (cl.search("--help")) {
    LOG(INFO) << g_usage;
    exit(-1);
  }

  if (cl.search("-use_system_time")) {
    use_system_time = true;
  }

  if (cl.search("-startnow")) {
    is_running = true;
  }

  LOG(INFO) << "Initializing camera...";
  LoadCameras(cl);

  pyramid_levels = 3;
  patch_size = 7;
  sdtrack::KeypointOptions keypoint_options;
  keypoint_options.gftt_feature_block_size = patch_size;
  keypoint_options.max_num_features = num_features * 2;
  keypoint_options.gftt_min_distance_between_features = 3;
  keypoint_options.gftt_absolute_strength_threshold = 0.005;
  sdtrack::TrackerOptions tracker_options;
  tracker_options.pyramid_levels = pyramid_levels;
  tracker_options.detector_type = sdtrack::TrackerOptions::Detector_GFTT;
  tracker_options.num_active_tracks = num_features;
  tracker_options.use_robust_norm_ = false;
  tracker_options.robust_norm_threshold_ = 30;
  tracker_options.patch_dim = patch_size;
  tracker_options.default_rho = 1.0/5.0;
  tracker_options.feature_cells = feature_cells;
  tracker_options.iteration_exponent = 2;
  tracker_options.dense_ncc_threshold = ncc_threshold;
  tracker_options.harris_score_threshold = 2e6;
  tracker_options.gn_scaling = 1.0;
  tracker.Initialize(keypoint_options, tracker_options, &rig);


  // Initialize the online calibration component.
  Eigen::VectorXd weights(rig.cameras_[0]->NumParams());
  if (weights.rows() > 4) {
    weights << 1.0, 1.0, 1.7, 1.7, 320000;
    //weights << 1.0, 1.0, 1.0, 1.0, 1.0;
  } else {
    weights << 1.0, 1.0, 1.7, 1.7;
  }

  /// ZZZZZZZ : TEMPORARY FOR IMU
  // weights = Eigen::VectorXd(6);
  // weights << 1.0, 1.0, 1.0, 1.0, 1.0, 1.0;

  online_calib.Init(&aac_mutex, &selfcal_rig, num_self_cal_segments,
                    self_cal_segment_length, weights,
                    imu_time_offset, &imu_buffer);

  InitGui();

  //////////////////////////
  /// ZZZZZZZZZZZZZZZZZZ: Get rid of this. Only valid for ICRA test rig
  imu_time_offset = -0.0697;

  aac_thread = std::shared_ptr<std::thread>(new std::thread(&DoAAC));

  Run();

  return 0;
}
