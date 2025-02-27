#include "selfdrive/modeld/models/driving.h"

#include <fcntl.h>
#include <unistd.h>

#include <cassert>
#include <cstring>

#include <eigen3/Eigen/Dense>

#include "common/clutil.h"
#include "common/params.h"
#include "common/timing.h"
#include "common/swaglog.h"


// #define DUMP_YUV

void model_init(ModelState* s, cl_device_id device_id, cl_context context) {
  s->frame = new ModelFrame(device_id, context);
  s->wide_frame = new ModelFrame(device_id, context);

#ifdef USE_THNEED
  s->m = std::make_unique<ThneedModel>("models/supercombo.thneed",
#elif USE_ONNX_MODEL
  s->m = std::make_unique<ONNXModel>("models/supercombo.onnx",
#else
  s->m = std::make_unique<SNPEModel>("models/supercombo.dlc",
#endif
   &s->output[0], NET_OUTPUT_SIZE, USE_GPU_RUNTIME, false, context);

  s->m->addInput("input_imgs", NULL, 0);
  s->m->addInput("big_input_imgs", NULL, 0);

  // TODO: the input is important here, still need to fix this
#ifdef DESIRE
  s->m->addInput("desire_pulse", s->pulse_desire, DESIRE_LEN*(HISTORY_BUFFER_LEN+1));
#endif

#ifdef TRAFFIC_CONVENTION
  s->m->addInput("traffic_convention", s->traffic_convention, TRAFFIC_CONVENTION_LEN);
#endif

#ifdef DRIVING_STYLE
  s->m->addInput("driving_style", s->driving_style, DRIVING_STYLE_LEN);
#endif

#ifdef NAV
  s->m->addInput("nav_features", s->nav_features, NAV_FEATURE_LEN);
#endif

#ifdef TEMPORAL
  s->m->addInput("feature_buffer", &s->feature_buffer[0], TEMPORAL_SIZE);
#endif

}

ModelOutput* model_eval_frame(ModelState* s, VisionBuf* buf, VisionBuf* wbuf,
                              const mat3 &transform, const mat3 &transform_wide, float *desire_in, bool is_rhd, float *driving_style, float *nav_features, bool prepare_only) {
#ifdef DESIRE
  std::memmove(&s->pulse_desire[0], &s->pulse_desire[DESIRE_LEN], sizeof(float) * DESIRE_LEN*HISTORY_BUFFER_LEN);
  if (desire_in != NULL) {
    for (int i = 1; i < DESIRE_LEN; i++) {
      // Model decides when action is completed
      // so desire input is just a pulse triggered on rising edge
      if (desire_in[i] - s->prev_desire[i] > .99) {
        s->pulse_desire[DESIRE_LEN*HISTORY_BUFFER_LEN+i] = desire_in[i];
      } else {
        s->pulse_desire[DESIRE_LEN*HISTORY_BUFFER_LEN+i] = 0.0;
      }
      s->prev_desire[i] = desire_in[i];
    }
  }
LOGT("Desire enqueued");
#endif

#ifdef NAV
  std::memcpy(s->nav_features, nav_features, sizeof(float)*NAV_FEATURE_LEN);
#endif

#ifdef DRIVING_STYLE
  std::memcpy(s->driving_style, driving_style, sizeof(float)*DRIVING_STYLE_LEN);
#endif

  int rhd_idx = is_rhd;
  s->traffic_convention[rhd_idx] = 1.0;
  s->traffic_convention[1-rhd_idx] = 0.0;

  // if getInputBuf is not NULL, net_input_buf will be
  auto net_input_buf = s->frame->prepare(buf->buf_cl, buf->width, buf->height, buf->stride, buf->uv_offset, transform, static_cast<cl_mem*>(s->m->getCLBuffer("input_imgs")));
  s->m->setInputBuffer("input_imgs", net_input_buf, s->frame->buf_size);
  LOGT("Image added");

  if (wbuf != nullptr) {
    auto net_extra_buf = s->wide_frame->prepare(wbuf->buf_cl, wbuf->width, wbuf->height, wbuf->stride, wbuf->uv_offset, transform_wide, static_cast<cl_mem*>(s->m->getCLBuffer("big_input_imgs")));
    s->m->setInputBuffer("big_input_imgs", net_extra_buf, s->wide_frame->buf_size);
    LOGT("Extra image added");
  }

  if (prepare_only) {
    return nullptr;
  }

  s->m->execute();
  LOGT("Execution finished");

  #ifdef TEMPORAL
    std::memmove(&s->feature_buffer[0], &s->feature_buffer[FEATURE_LEN], sizeof(float) * FEATURE_LEN*(HISTORY_BUFFER_LEN-1));
    std::memcpy(&s->feature_buffer[FEATURE_LEN*(HISTORY_BUFFER_LEN-1)], &s->output[OUTPUT_SIZE], sizeof(float) * FEATURE_LEN);
    LOGT("Features enqueued");
  #endif

  return (ModelOutput*)&s->output;
}

void model_free(ModelState* s) {
  delete s->frame;
  delete s->wide_frame;
}

void fill_lead(cereal::ModelDataV2::LeadDataV3::Builder lead, const ModelOutputLeads &leads, int t_idx, float prob_t) {
  std::array<float, LEAD_TRAJ_LEN> lead_t = {0.0, 2.0, 4.0, 6.0, 8.0, 10.0};
  const auto &best_prediction = leads.get_best_prediction(t_idx);
  lead.setProb(sigmoid(leads.prob[t_idx]));
  lead.setProbTime(prob_t);
  std::array<float, LEAD_TRAJ_LEN> lead_x, lead_y, lead_v, lead_a;
  std::array<float, LEAD_TRAJ_LEN> lead_x_std, lead_y_std, lead_v_std, lead_a_std;
  for (int i=0; i<LEAD_TRAJ_LEN; i++) {
    lead_x[i] = best_prediction.mean[i].x;
    lead_y[i] = best_prediction.mean[i].y;
    lead_v[i] = best_prediction.mean[i].velocity;
    lead_a[i] = best_prediction.mean[i].acceleration;
    lead_x_std[i] = exp(best_prediction.std[i].x);
    lead_y_std[i] = exp(best_prediction.std[i].y);
    lead_v_std[i] = exp(best_prediction.std[i].velocity);
    lead_a_std[i] = exp(best_prediction.std[i].acceleration);
  }
  lead.setT(to_kj_array_ptr(lead_t));
  lead.setX(to_kj_array_ptr(lead_x));
  lead.setY(to_kj_array_ptr(lead_y));
  lead.setV(to_kj_array_ptr(lead_v));
  lead.setA(to_kj_array_ptr(lead_a));
  lead.setXStd(to_kj_array_ptr(lead_x_std));
  lead.setYStd(to_kj_array_ptr(lead_y_std));
  lead.setVStd(to_kj_array_ptr(lead_v_std));
  lead.setAStd(to_kj_array_ptr(lead_a_std));
}

void fill_meta(cereal::ModelDataV2::MetaData::Builder meta, const ModelOutputMeta &meta_data, PublishState &ps) {
  std::array<float, DESIRE_LEN> desire_state_softmax;
  softmax(meta_data.desire_state_prob.array.data(), desire_state_softmax.data(), DESIRE_LEN);

  std::array<float, DESIRE_PRED_LEN * DESIRE_LEN> desire_pred_softmax;
  for (int i=0; i<DESIRE_PRED_LEN; i++) {
    softmax(meta_data.desire_pred_prob[i].array.data(), desire_pred_softmax.data() + (i * DESIRE_LEN), DESIRE_LEN);
  }

  std::array<float, DISENGAGE_LEN> lat_long_t = {2,4,6,8,10};
  std::array<float, DISENGAGE_LEN> gas_disengage_sigmoid, brake_disengage_sigmoid, steer_override_sigmoid,
                                   brake_3ms2_sigmoid, brake_4ms2_sigmoid, brake_5ms2_sigmoid;
  for (int i=0; i<DISENGAGE_LEN; i++) {
    gas_disengage_sigmoid[i] = sigmoid(meta_data.disengage_prob[i].gas_disengage);
    brake_disengage_sigmoid[i] = sigmoid(meta_data.disengage_prob[i].brake_disengage);
    steer_override_sigmoid[i] = sigmoid(meta_data.disengage_prob[i].steer_override);
    brake_3ms2_sigmoid[i] = sigmoid(meta_data.disengage_prob[i].brake_3ms2);
    brake_4ms2_sigmoid[i] = sigmoid(meta_data.disengage_prob[i].brake_4ms2);
    brake_5ms2_sigmoid[i] = sigmoid(meta_data.disengage_prob[i].brake_5ms2);
    //gas_pressed_sigmoid[i] = sigmoid(meta_data.disengage_prob[i].gas_pressed);
  }

  std::memmove(ps.prev_brake_5ms2_probs.data(), &ps.prev_brake_5ms2_probs[1], 4*sizeof(float));
  std::memmove(ps.prev_brake_3ms2_probs.data(), &ps.prev_brake_3ms2_probs[1], 2*sizeof(float));
  ps.prev_brake_5ms2_probs[4] = brake_5ms2_sigmoid[0];
  ps.prev_brake_3ms2_probs[2] = brake_3ms2_sigmoid[0];

  bool above_fcw_threshold = true;
  for (int i=0; i<ps.prev_brake_5ms2_probs.size(); i++) {
    float threshold = i < 2 ? FCW_THRESHOLD_5MS2_LOW : FCW_THRESHOLD_5MS2_HIGH;
    above_fcw_threshold = above_fcw_threshold && ps.prev_brake_5ms2_probs[i] > threshold;
  }
  for (int i=0; i<ps.prev_brake_3ms2_probs.size(); i++) {
    above_fcw_threshold = above_fcw_threshold && ps.prev_brake_3ms2_probs[i] > FCW_THRESHOLD_3MS2;
  }

  auto disengage = meta.initDisengagePredictions();
  disengage.setT(to_kj_array_ptr(lat_long_t));
  disengage.setGasDisengageProbs(to_kj_array_ptr(gas_disengage_sigmoid));
  disengage.setBrakeDisengageProbs(to_kj_array_ptr(brake_disengage_sigmoid));
  disengage.setSteerOverrideProbs(to_kj_array_ptr(steer_override_sigmoid));
  disengage.setBrake3MetersPerSecondSquaredProbs(to_kj_array_ptr(brake_3ms2_sigmoid));
  disengage.setBrake4MetersPerSecondSquaredProbs(to_kj_array_ptr(brake_4ms2_sigmoid));
  disengage.setBrake5MetersPerSecondSquaredProbs(to_kj_array_ptr(brake_5ms2_sigmoid));

  meta.setEngagedProb(sigmoid(meta_data.engaged_prob));
  meta.setDesirePrediction(to_kj_array_ptr(desire_pred_softmax));
  meta.setDesireState(to_kj_array_ptr(desire_state_softmax));
  meta.setHardBrakePredicted(above_fcw_threshold);
}

void fill_confidence(cereal::ModelDataV2::Builder &framed, PublishState &ps) {
  if (framed.getFrameId() % (2*MODEL_FREQ) == 0) {
    // update every 2s to match predictions interval
    auto dbps = framed.getMeta().getDisengagePredictions().getBrakeDisengageProbs();
    auto dgps = framed.getMeta().getDisengagePredictions().getGasDisengageProbs();
    auto dsps = framed.getMeta().getDisengagePredictions().getSteerOverrideProbs();

    float any_dp[DISENGAGE_LEN];
    float dp_ind[DISENGAGE_LEN];

    for (int i = 0; i < DISENGAGE_LEN; i++) {
      any_dp[i] = 1 - ((1-dbps[i])*(1-dgps[i])*(1-dsps[i])); // any disengage prob
    }

    dp_ind[0] = any_dp[0];
    for (int i = 0; i < DISENGAGE_LEN-1; i++) {
      dp_ind[i+1] = (any_dp[i+1] - any_dp[i]) / (1 - any_dp[i]); // independent disengage prob for each 2s slice
    }

    // rolling buf for 2, 4, 6, 8, 10s
    std::memmove(&ps.disengage_buffer[0], &ps.disengage_buffer[DISENGAGE_LEN], sizeof(float) * DISENGAGE_LEN * (DISENGAGE_LEN-1));
    std::memcpy(&ps.disengage_buffer[DISENGAGE_LEN * (DISENGAGE_LEN-1)], &dp_ind[0], sizeof(float) * DISENGAGE_LEN);
  }

  float score = 0;
  for (int i = 0; i < DISENGAGE_LEN; i++) {
    score += ps.disengage_buffer[i*DISENGAGE_LEN+DISENGAGE_LEN-1-i] / DISENGAGE_LEN;
  }

  if (score < RYG_GREEN) {
    framed.setConfidence(cereal::ModelDataV2::ConfidenceClass::GREEN);
  } else if (score < RYG_YELLOW) {
    framed.setConfidence(cereal::ModelDataV2::ConfidenceClass::YELLOW);
  } else {
    framed.setConfidence(cereal::ModelDataV2::ConfidenceClass::RED);
  }
}

template<size_t size>
void fill_xyzt(cereal::XYZTData::Builder xyzt, const std::array<float, size> &t,
               const std::array<float, size> &x, const std::array<float, size> &y, const std::array<float, size> &z) {
  xyzt.setT(to_kj_array_ptr(t));
  xyzt.setX(to_kj_array_ptr(x));
  xyzt.setY(to_kj_array_ptr(y));
  xyzt.setZ(to_kj_array_ptr(z));
}

template<size_t size>
void fill_xyzt(cereal::XYZTData::Builder xyzt, const std::array<float, size> &t,
               const std::array<float, size> &x, const std::array<float, size> &y, const std::array<float, size> &z,
               const std::array<float, size> &x_std, const std::array<float, size> &y_std, const std::array<float, size> &z_std) {
  fill_xyzt(xyzt, t, x, y, z);
  xyzt.setXStd(to_kj_array_ptr(x_std));
  xyzt.setYStd(to_kj_array_ptr(y_std));
  xyzt.setZStd(to_kj_array_ptr(z_std));
}

void fill_plan(cereal::ModelDataV2::Builder &framed, const ModelOutputPlanPrediction &plan) {
  std::array<float, TRAJECTORY_SIZE> pos_x, pos_y, pos_z;
  std::array<float, TRAJECTORY_SIZE> pos_x_std, pos_y_std, pos_z_std;
  std::array<float, TRAJECTORY_SIZE> vel_x, vel_y, vel_z;
  std::array<float, TRAJECTORY_SIZE> rot_x, rot_y, rot_z;
  std::array<float, TRAJECTORY_SIZE> acc_x, acc_y, acc_z;
  std::array<float, TRAJECTORY_SIZE> rot_rate_x, rot_rate_y, rot_rate_z;

  for(int i=0; i<TRAJECTORY_SIZE; i++) {
    pos_x[i] = plan.mean[i].position.x;
    pos_y[i] = plan.mean[i].position.y;
    pos_z[i] = plan.mean[i].position.z;
    pos_x_std[i] = exp(plan.std[i].position.x);
    pos_y_std[i] = exp(plan.std[i].position.y);
    pos_z_std[i] = exp(plan.std[i].position.z);
    vel_x[i] = plan.mean[i].velocity.x;
    vel_y[i] = plan.mean[i].velocity.y;
    vel_z[i] = plan.mean[i].velocity.z;
    acc_x[i] = plan.mean[i].acceleration.x;
    acc_y[i] = plan.mean[i].acceleration.y;
    acc_z[i] = plan.mean[i].acceleration.z;
    rot_x[i] = plan.mean[i].rotation.x;
    rot_y[i] = plan.mean[i].rotation.y;
    rot_z[i] = plan.mean[i].rotation.z;
    rot_rate_x[i] = plan.mean[i].rotation_rate.x;
    rot_rate_y[i] = plan.mean[i].rotation_rate.y;
    rot_rate_z[i] = plan.mean[i].rotation_rate.z;
  }

  fill_xyzt(framed.initPosition(), T_IDXS_FLOAT, pos_x, pos_y, pos_z, pos_x_std, pos_y_std, pos_z_std);
  fill_xyzt(framed.initVelocity(), T_IDXS_FLOAT, vel_x, vel_y, vel_z);
  fill_xyzt(framed.initAcceleration(), T_IDXS_FLOAT, acc_x, acc_y, acc_z);
  fill_xyzt(framed.initOrientation(), T_IDXS_FLOAT, rot_x, rot_y, rot_z);
  fill_xyzt(framed.initOrientationRate(), T_IDXS_FLOAT, rot_rate_x, rot_rate_y, rot_rate_z);
}

void fill_lane_lines(cereal::ModelDataV2::Builder &framed, const std::array<float, TRAJECTORY_SIZE> &plan_t,
                     const ModelOutputLaneLines &lanes) {
  std::array<float, TRAJECTORY_SIZE> left_far_y, left_far_z;
  std::array<float, TRAJECTORY_SIZE> left_near_y, left_near_z;
  std::array<float, TRAJECTORY_SIZE> right_near_y, right_near_z;
  std::array<float, TRAJECTORY_SIZE> right_far_y, right_far_z;
  for (int j=0; j<TRAJECTORY_SIZE; j++) {
    left_far_y[j] = lanes.mean.left_far[j].y;
    left_far_z[j] = lanes.mean.left_far[j].z;
    left_near_y[j] = lanes.mean.left_near[j].y;
    left_near_z[j] = lanes.mean.left_near[j].z;
    right_near_y[j] = lanes.mean.right_near[j].y;
    right_near_z[j] = lanes.mean.right_near[j].z;
    right_far_y[j] = lanes.mean.right_far[j].y;
    right_far_z[j] = lanes.mean.right_far[j].z;
  }

  auto lane_lines = framed.initLaneLines(4);
  fill_xyzt(lane_lines[0], plan_t, X_IDXS_FLOAT, left_far_y, left_far_z);
  fill_xyzt(lane_lines[1], plan_t, X_IDXS_FLOAT, left_near_y, left_near_z);
  fill_xyzt(lane_lines[2], plan_t, X_IDXS_FLOAT, right_near_y, right_near_z);
  fill_xyzt(lane_lines[3], plan_t, X_IDXS_FLOAT, right_far_y, right_far_z);

  framed.setLaneLineStds({
    exp(lanes.std.left_far[0].y),
    exp(lanes.std.left_near[0].y),
    exp(lanes.std.right_near[0].y),
    exp(lanes.std.right_far[0].y),
  });

  framed.setLaneLineProbs({
    sigmoid(lanes.prob.left_far.val),
    sigmoid(lanes.prob.left_near.val),
    sigmoid(lanes.prob.right_near.val),
    sigmoid(lanes.prob.right_far.val),
  });
}

void fill_road_edges(cereal::ModelDataV2::Builder &framed, const std::array<float, TRAJECTORY_SIZE> &plan_t,
                     const ModelOutputRoadEdges &edges) {
  std::array<float, TRAJECTORY_SIZE> left_y, left_z;
  std::array<float, TRAJECTORY_SIZE> right_y, right_z;
  for (int j=0; j<TRAJECTORY_SIZE; j++) {
    left_y[j] = edges.mean.left[j].y;
    left_z[j] = edges.mean.left[j].z;
    right_y[j] = edges.mean.right[j].y;
    right_z[j] = edges.mean.right[j].z;
  }

  auto road_edges = framed.initRoadEdges(2);
  fill_xyzt(road_edges[0], plan_t, X_IDXS_FLOAT, left_y, left_z);
  fill_xyzt(road_edges[1], plan_t, X_IDXS_FLOAT, right_y, right_z);

  framed.setRoadEdgeStds({
    exp(edges.std.left[0].y),
    exp(edges.std.right[0].y),
  });
}

void fill_model(cereal::ModelDataV2::Builder &framed, const ModelOutput &net_outputs, PublishState &ps) {
  const auto &best_plan = net_outputs.plans.get_best_prediction();
  std::array<float, TRAJECTORY_SIZE> plan_t;
  std::fill_n(plan_t.data(), plan_t.size(), NAN);
  plan_t[0] = 0.0;
  for (int xidx=1, tidx=0; xidx<TRAJECTORY_SIZE; xidx++) {
    // increment tidx until we find an element that's further away than the current xidx
    for (int next_tid = tidx + 1; next_tid < TRAJECTORY_SIZE && best_plan.mean[next_tid].position.x < X_IDXS[xidx]; next_tid++) {
      tidx++;
    }
    if (tidx == TRAJECTORY_SIZE - 1) {
      // if the Plan doesn't extend far enough, set plan_t to the max value (10s), then break
      plan_t[xidx] = T_IDXS[TRAJECTORY_SIZE - 1];
      break;
    }

    // interpolate to find `t` for the current xidx
    float current_x_val = best_plan.mean[tidx].position.x;
    float next_x_val = best_plan.mean[tidx+1].position.x;
    float p = (X_IDXS[xidx] - current_x_val) / (next_x_val - current_x_val);
    plan_t[xidx] = p * T_IDXS[tidx+1] + (1 - p) * T_IDXS[tidx];
  }

  fill_plan(framed, best_plan);
  fill_lane_lines(framed, plan_t, net_outputs.lane_lines);
  fill_road_edges(framed, plan_t, net_outputs.road_edges);

  // meta
  fill_meta(framed.initMeta(), net_outputs.meta, ps);

  // confidence
  fill_confidence(framed, ps);

  // leads
  auto leads = framed.initLeadsV3(LEAD_MHP_SELECTION);
  std::array<float, LEAD_MHP_SELECTION> t_offsets = {0.0, 2.0, 4.0};
  for (int i=0; i<LEAD_MHP_SELECTION; i++) {
    fill_lead(leads[i], net_outputs.leads, i, t_offsets[i]);
  }

  // temporal pose
  const auto &v_mean = net_outputs.temporal_pose.velocity_mean;
  const auto &r_mean = net_outputs.temporal_pose.rotation_mean;
  const auto &v_std = net_outputs.temporal_pose.velocity_std;
  const auto &r_std = net_outputs.temporal_pose.rotation_std;
  auto temporal_pose = framed.initTemporalPose();
  temporal_pose.setTrans({v_mean.x, v_mean.y, v_mean.z});
  temporal_pose.setRot({r_mean.x, r_mean.y, r_mean.z});
  temporal_pose.setTransStd({exp(v_std.x), exp(v_std.y), exp(v_std.z)});
  temporal_pose.setRotStd({exp(r_std.x), exp(r_std.y), exp(r_std.z)});
}

void model_publish(PubMaster &pm, uint32_t vipc_frame_id, uint32_t vipc_frame_id_extra, uint32_t frame_id, float frame_drop,
                   const ModelOutput &net_outputs, ModelState &s, PublishState &ps, uint64_t timestamp_eof, uint64_t timestamp_llk,
                   float model_execution_time, const bool nav_enabled, const bool valid) {
  const uint32_t frame_age = (frame_id > vipc_frame_id) ? (frame_id - vipc_frame_id) : 0;
  MessageBuilder msg;
  auto framed = msg.initEvent(valid).initModelV2();
  framed.setFrameId(vipc_frame_id);
  framed.setFrameIdExtra(vipc_frame_id_extra);
  framed.setFrameAge(frame_age);
  framed.setFrameDropPerc(frame_drop * 100);
  framed.setTimestampEof(timestamp_eof);
  framed.setLocationMonoTime(timestamp_llk);
  framed.setModelExecutionTime(model_execution_time);
  framed.setNavEnabled(nav_enabled);
  if (send_raw_pred) {
    framed.setRawPredictions((kj::ArrayPtr<const float>(s.output.data(), s.output.size())).asBytes());
  }
  fill_model(framed, net_outputs, ps);
  pm.send("modelV2", msg);
}

void posenet_publish(PubMaster &pm, uint32_t vipc_frame_id, uint32_t vipc_dropped_frames,
                     const ModelOutput &net_outputs, uint64_t timestamp_eof, const bool valid) {
  MessageBuilder msg;
  const auto &v_mean = net_outputs.pose.velocity_mean;
  const auto &r_mean = net_outputs.pose.rotation_mean;
  const auto &t_mean = net_outputs.wide_from_device_euler.mean;
  const auto &v_std = net_outputs.pose.velocity_std;
  const auto &r_std = net_outputs.pose.rotation_std;
  const auto &t_std = net_outputs.wide_from_device_euler.std;
  const auto &road_transform_trans_mean = net_outputs.road_transform.position_mean;
  const auto &road_transform_trans_std = net_outputs.road_transform.position_std;

  auto posenetd = msg.initEvent(valid && (vipc_dropped_frames < 1)).initCameraOdometry();
  posenetd.setTrans({v_mean.x, v_mean.y, v_mean.z});
  posenetd.setRot({r_mean.x, r_mean.y, r_mean.z});
  posenetd.setWideFromDeviceEuler({t_mean.x, t_mean.y, t_mean.z});
  posenetd.setRoadTransformTrans({road_transform_trans_mean.x, road_transform_trans_mean.y, road_transform_trans_mean.z});
  posenetd.setTransStd({exp(v_std.x), exp(v_std.y), exp(v_std.z)});
  posenetd.setRotStd({exp(r_std.x), exp(r_std.y), exp(r_std.z)});
  posenetd.setWideFromDeviceEulerStd({exp(t_std.x), exp(t_std.y), exp(t_std.z)});
  posenetd.setRoadTransformTransStd({exp(road_transform_trans_std.x), exp(road_transform_trans_std.y), exp(road_transform_trans_std.z)});

  posenetd.setTimestampEof(timestamp_eof);
  posenetd.setFrameId(vipc_frame_id);

  pm.send("cameraOdometry", msg);
}
