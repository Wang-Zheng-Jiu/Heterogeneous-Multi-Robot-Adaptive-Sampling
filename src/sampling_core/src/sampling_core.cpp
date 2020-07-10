#include "sampling_core/sampling_core.h"

#include <ros/ros.h>
#include <visualization_msgs/MarkerArray.h>

#include "sampling_msgs/RequestLocation.h"

namespace sampling {
namespace core {

SamplingCore::SamplingCore(const ros::NodeHandle &nh, const ros::NodeHandle &rh)
    : nh_(nh), rh_(rh) {}

bool SamplingCore::Init() {
  Jackal_visualization_node_ = nullptr;
  Pelican_visualization_node_ = nullptr;
  Jackal_longitude_ = boost::optional<double>{};
  Jackal_latitude_ = boost::optional<double>{};
  Pelican_longitude_ = boost::optional<double>{};
  Pelican_latitude_ = boost::optional<double>{};
  agent_id_["Jackal"] = 0;
  agent_id_["Pelican"] = 1;
  // Load parameters
  if (!ParseFromRosParam()) {
    ROS_ERROR_STREAM("Missing required ros parameter");
    return false;
  }

  // Initialize visualization
  if (!InitializeVisualization()) {
    ROS_ERROR_STREAM("Failed to initialize visualization");
    return false;
  }

  interest_point_assignment_ser_ =
      nh_.advertiseService("interest_point_service_channel",
                           &SamplingCore::AssignInterestPoint, this);

  sample_sub_ = nh_.subscribe("temperature_update_channel", 1,
                              &SamplingCore::CollectSampleCallback, this);

  Jackal_GPS_sub_ = nh_.subscribe("Jackal_GPS_channel", 1,
                                  &SamplingCore::JackalGPSCallback, this);

  Pelican_GPS_sub_ = nh_.subscribe("Pelican_GPS_channel", 1,
                                   &SamplingCore::PelicanGPSCallback, this);
  // Robot agent
  Jackal_GPS_client_ = nh_.serviceClient<sampling_msgs::RequestLocation>(
      "Jackal_request_GPS_channel");

  Pelican_GPS_client_ = nh_.serviceClient<sampling_msgs::RequestLocation>(
      "Pelican_request_GPS_channel");

  distribution_visualization_pub_ =
      nh_.advertise<visualization_msgs::MarkerArray>("sampling_visualization",
                                                     1);
  Jackal_position_pub_ =
      nh_.advertise<visualization_msgs::Marker>("jackal_visualization", 1);

  Pelican_position_pub_ =
      nh_.advertise<visualization_msgs::Marker>("pelican_visualization", 1);

  voronoi_node_ = voronoi::Voronoi(location_);
  update_flag_ = false;
  sample_size_ = 0;

  // Model initialization
  if (!gt_gp_hyperparams_.empty()) {
    gt_model_ = std::unique_ptr<gpmm::GaussianProcessMixtureModel>(
        new gpmm::GaussianProcessMixtureModel(
            gt_num_gaussian_, gt_gp_hyperparams_, max_iteration_, eps_));
  } else
    gt_model_ = nullptr;

  model_ = std::unique_ptr<gpmm::GaussianProcessMixtureModel>(
      new gpmm::GaussianProcessMixtureModel(num_gaussian_, gp_hyperparams_,
                                            max_iteration_, eps_));

  // Information selection initialization
  informative_sampling_node_ =
      std::unique_ptr<informative_sampling::InformativeSampling>(
          new informative_sampling::InformativeSampling(
              location_, selection_mode_, variance_coef_));

  // Initial data
  if (init_sample_temperature_.rows() > 0) {
    model_->Train(init_sample_temperature_, init_sample_temperature_);
  }

  if (ground_truth_temperature_.rows() > 0) {
    gt_model_->Train(ground_truth_temperature_, ground_truth_location_);
    gt_model_->Predict(location_, gt_mean_, gt_var_);
  }
  ROS_INFO_STREAM("Finish initialization!");
  return true;
}

bool SamplingCore::LoadMapParam(XmlRpc::XmlRpcValue &YamlNode,
                                visualization::MAP_PARAM &param) {
  if (!utils::GetParam(YamlNode, "map_frame", param.map_frame)) {
    return false;
  }
  if (!utils::GetParam(YamlNode, "map_id", param.map_id)) {
    return false;
  }
  if (!utils::GetParam(YamlNode, "x_scale", param.x_scale)) {
    return false;
  }
  if (!utils::GetParam(YamlNode, "y_scale", param.y_scale)) {
    return false;
  }
  if (!utils::GetParam(YamlNode, "x_offset", param.x_offset)) {
    return false;
  }
  if (!utils::GetParam(YamlNode, "y_offset", param.y_offset)) {
    return false;
  }
  if (!utils::GetParam(YamlNode, "lower_bound", param.lower_bound)) {
    return false;
  }
  if (!utils::GetParam(YamlNode, "upper_bound", param.upper_bound)) {
    return false;
  }
  return true;
}

bool SamplingCore::AssignInterestPoint(
    sampling_msgs::SamplingGoal::Request &req,
    sampling_msgs::SamplingGoal::Response &res) {
  ROS_INFO_STREAM(
      "Master Computer received request from robot : " << req.robot_id);

  // Update Voronoi map
  int agent_id = agent_id_[req.robot_id];
  Eigen::MatrixXd agent_locations(2, 2);
  agent_locations(0, 0) = Jackal_latitude_.get();
  agent_locations(0, 1) = Jackal_longitude_.get();
  agent_locations(1, 0) = Pelican_latitude_.get();
  agent_locations(1, 1) = Pelican_longitude_.get();

  std::vector<int> cell_index =
      voronoi_node_.GetSingleVoronoiCellIndex(agent_locations, agent_id);

  std::pair<double, double> next_location =
      informative_sampling_node_->SelectInformativeLocation(
          mean_prediction_, var_prediction_, cell_index);
  res.latitude = next_location.first;
  res.longitude = next_location.second;
  return true;
}

void SamplingCore::CollectSampleCallback(
    const sampling_msgs::measurement &msg) {
  if (msg.valid) {
    ROS_INFO_STREAM("Master received temperature : " << msg.measurement);
    sample_size_++;
    if (sample_size_ % model_update_rate_ == 0) {
      update_flag_ = true;
    }
    Eigen::MatrixXd new_location, new_feature;
    utils::MsgToMatrix(msg, new_location, new_feature);
    new_location(0, 0) = new_location(0, 0) * map_scale_;
    new_location(0, 1) = new_location(0, 1) * map_scale_;

    collected_temperatures_.conservativeResize(collected_temperatures_.size() +
                                               1);
    collected_temperatures_(collected_temperatures_.size() - 1) =
        msg.measurement;
    collected_locations_.conservativeResize(collected_locations_.rows() + 1,
                                            collected_locations_.cols());
    collected_locations_(collected_locations_.rows() - 1, 0) =
        new_location(0, 0);
    collected_locations_(collected_locations_.rows() - 1, 1) =
        new_location(0, 1);
  } else {
    ROS_INFO_STREAM(
        "Master computer received invalid sample from : " << msg.robot_id);
  }
}

bool SamplingCore::ParseFromRosParam() {
  /// learning data
  XmlRpc::XmlRpcValue data_list;
  if (!rh_.getParam("data_path", data_list)) {
    ROS_ERROR_STREAM("Missing necessary data");
    return false;
  } else {
    if (data_list.size() == 0) {
      ROS_ERROR_STREAM("Empty data path parameters!");
      return false;
    }
    XmlRpc::XmlRpcValue data_path = data_list[0];
    std::string location_data;
    if (!utils::GetParamData(data_path, "location_data", location_)) {
      return false;
    }

    if (!utils::GetParamData(data_path, "ground_truth_temperature_data",
                             ground_truth_temperature_)) {
    }

    if (!utils::GetParamData(data_path, "ground_truth_location_data",
                             ground_truth_location_)) {
    }

    if (!utils::GetParamData(data_path, "initial_location_data",
                             init_sample_location_)) {
      return false;
    }

    if (!utils::GetParamData(data_path, "initial_temperature_data",
                             init_sample_temperature_)) {
      return false;
    }
    collected_locations_ = init_sample_temperature_;
    collected_temperatures_ = init_sample_location_;

    ROS_INFO_STREAM("Successfully loaded data!");
  }

  // learning EM learning parameter
  XmlRpc::XmlRpcValue learning_param_list;
  if (!rh_.getParam("learning_parameters", learning_param_list)) {
    ROS_ERROR_STREAM("Missing EM learning parameters");
    return false;
  } else {
    if (learning_param_list.size() == 0) {
      ROS_ERROR_STREAM("Empty learning parameters!");
      return false;
    }
    XmlRpc::XmlRpcValue learning_param = learning_param_list[0];

    if (!utils::GetParam(learning_param, "model_update_rate",
                         model_update_rate_)) {
      return false;
    }

    ROS_INFO_STREAM("Successfully loaded EM learning parameters!");
  }

  // learning Model parameters
  XmlRpc::XmlRpcValue model_param_list;
  if (!rh_.getParam("model_parameters", model_param_list)) {
    ROS_ERROR_STREAM("Missing model parameters");
    return false;
  } else {
    if (model_param_list.size() == 0) {
      ROS_ERROR_STREAM("Empty model parameters!");
      return false;
    }
    for (int32_t i = 0; i < model_param_list.size(); ++i) {
      XmlRpc::XmlRpcValue model_param = model_param_list[i];
      bool is_gt = false;
      utils::GetParam(model_param, "gt", is_gt);
      if (is_gt) {
        if (!utils::GetParam(model_param, "num_gaussian", gt_num_gaussian_)) {
          return false;
        }
        for (int i = 0; i < gt_num_gaussian_; ++i) {
          std::string param_name = "param" + std::to_string(i);
          std::vector<double> gp_hyper_param;
          if (!utils::GetParam(model_param, param_name, gp_hyper_param)) {
            return false;
          }
          gt_gp_hyperparams_.push_back(gp_hyper_param);
        }
      } else {
        if (!utils::GetParam(model_param, "num_gaussian", num_gaussian_)) {
          return false;
        }
        for (int i = 0; i < num_gaussian_; ++i) {
          std::string param_name = "param" + std::to_string(i);
          std::vector<double> gp_hyper_param;
          if (!utils::GetParam(model_param, param_name, gp_hyper_param)) {
            return false;
          }
          gp_hyperparams_.push_back(gp_hyper_param);
        }
      }
      if (!utils::GetParam(model_param, "max_iteration", max_iteration_)) {
        return false;
      }
      if (!utils::GetParam(model_param, "eps", eps_)) {
        return false;
      }
      if (!utils::GetParam(model_param, "map_scale", map_scale_)) {
        return false;
      }
    }
    ROS_INFO_STREAM("Successfully loaded model parameters!");
  }

  // learning sampling parameter
  XmlRpc::XmlRpcValue sampling_param_list;
  if (!rh_.getParam("sampling_parameters", sampling_param_list)) {
    ROS_ERROR_STREAM("Missing sampling parameters");
    return false;
  } else {
    if (sampling_param_list.size() == 0) {
      ROS_ERROR_STREAM("Empty sampling parameters!");
      return false;
    }
    XmlRpc::XmlRpcValue sampling_param = sampling_param_list[0];

    int selection_model;
    if (!utils::GetParam(sampling_param, "selection_model", selection_model)) {
      return false;
    } else {
      switch (selection_model) {
        case 0: {
          selection_mode_ = VARIANCE;
          break;
        }
        case 1: {
          selection_mode_ = UCB;
          break;
        }
        default: {
          selection_mode_ = VARIANCE;
          break;
        }
      }
    }

    if (!utils::GetParam(sampling_param, "selection_model", selection_model)) {
      return false;
    }

    if (!utils::GetParam(sampling_param, "Jackal_id", Jackal_id_)) {
      return false;
    }

    if (!utils::GetParam(sampling_param, "Pelican_id", Pelican_id_)) {
      return false;
    }

    if (!utils::GetParam(sampling_param, "variance_coef_", variance_coef_)) {
      return false;
    }
    ROS_INFO_STREAM("Successfully loaded sampling parameters!");
  }

  /// visualization node
  XmlRpc::XmlRpcValue visualization_param_list;
  rh_.getParam("visualization_parameters", visualization_param_list);

  for (int32_t i = 0; i < visualization_param_list.size(); ++i) {
    XmlRpc::XmlRpcValue visualization_param = visualization_param_list[i];
    visualization::MAP_PARAM param;
    if (!LoadMapParam(visualization_param, param)) {
      return false;
    }
    visualization_params_.push_back(param);
  }

  ROS_INFO_STREAM("Finish loading data!");
  return true;
}

bool SamplingCore::InitializeVisualization() {
  for (const visualization::MAP_PARAM &param : visualization_params_) {
    std::string frame = param.map_frame;
    if (frame.compare("gt") == 0) {
      continue;
      // if (ground_truth_temperature_.rows() > 0) {
      //   visualization_node_[frame] =
      //       std::unique_ptr<visualization::SamplingVisualization>(
      //           new visualization::SamplingVisualization(nh_, param,
      //                                                    location_));
      // }
    } else if (frame.compare("mean") == 0) {
      visualization_node_[frame] =
          std::unique_ptr<visualization::SamplingVisualization>(
              new visualization::SamplingVisualization(nh_, param, location_));
    } else if (frame.compare("variance") == 0) {
      visualization_node_[frame] =
          std::unique_ptr<visualization::SamplingVisualization>(
              new visualization::SamplingVisualization(nh_, param, location_));
    } else if (frame.compare("raw") == 0) {
      visualization_node_[frame] =
          std::unique_ptr<visualization::SamplingVisualization>(
              new visualization::SamplingVisualization(nh_, param,
                                                       init_sample_location_));
      visualization_node_[frame]->UpdateMap(init_sample_temperature_.col(0));
    } else if (frame.compare("Jackal") == 0) {
      std_msgs::ColorRGBA Jackal_color;
      Jackal_color.r = 0.0;
      Jackal_color.g = 0.0;
      Jackal_color.b = 0.0;
      Jackal_color.a = 1.0;
      Jackal_visualization_node_.reset(new visualization::RobotVisualization(
          nh_, param, Jackal_color, location_));
    } else if (frame.compare("Pelican") == 0) {
      std_msgs::ColorRGBA Pelican_color;
      Pelican_color.r = 0.0;
      Pelican_color.g = 0.0;
      Pelican_color.b = 1.0;
      Pelican_color.a = 1.0;
      Pelican_visualization_node_.reset(new visualization::RobotVisualization(
          nh_, param, Pelican_color, location_));
    } else {
      ROS_ERROR_STREAM("Known visualization frame " << frame);
    }
  }
  return true;
}

void SamplingCore::UpdateModel() {
  model_->Train(collected_temperatures_, collected_locations_);
  model_->Predict(location_, mean_prediction_, var_prediction_);
}

double SamplingCore::RMSError(const Eigen::VectorXd &val1,
                              const Eigen::VectorXd &val2) {
  double rms = 0.0;
  if (val1.size() != val2.size()) {
    ROS_INFO_STREAM("RMS size doest not match!");
    ROS_INFO_STREAM("Size 1 :" << val1.size() << " size 2 : " << val2.size());
    return rms;
  }

  for (size_t i = 0; i < val1.size(); ++i) {
    rms = rms + (val1(i) - val2(i)) * (val1(i) - val2(i));
  }
  rms = std::sqrt(rms / (double)val1.size());
  return rms;
}

void SamplingCore::UpdateVisualization() {
  visualization_node_["mean"]->UpdateMap(mean_prediction_);
  visualization_node_["variance"]->UpdateMap(var_prediction_);
  visualization_msgs::MarkerArray marker_array;
  for (auto it = visualization_node_.begin(); it != visualization_node_.end();
       ++it) {
    marker_array.markers.push_back(it->second->GetMarker());
  }
  // if (Jackal_visualization_node_ && Jackal_latitude_.is_initialized() &&
  //     Jackal_longitude_.is_initialized()) {
  //   marker_array.markers.push_back(Jackal_visualization_node_->GetMarker());
  //   marker_array.markers.push_back(Jackal_visualization_node_->GetTarget());
  // }
  // if (Pelican_visualization_node_ && Pelican_latitude_.is_initialized() &&
  //     Pelican_longitude_.is_initialized()) {
  //   marker_array.markers.push_back(Pelican_visualization_node_->GetMarker());
  //   marker_array.markers.push_back(Pelican_visualization_node_->GetTarget());
  // }
  distribution_visualization_pub_.publish(marker_array);
}

void SamplingCore::JackalGPSCallback(const sensor_msgs::NavSatFix &msg) {
  Jackal_latitude_ = boost::optional<double>{msg.location_x * map_scale_};
  Jackal_longitude_ = boost::optional<double>{msg.location_y * map_scale_};
  Jackal_visualization_node_->UpdateMap(Jackal_latitude_.get(),
                                        Jackal_longitude_.get());
  visualization_msgs::Marker marker = Jackal_visualization_node_->GetMarker();
  Jackal_position_pub_.publish(marker);
}

void SamplingCore::PelicanGPSCallback(const sensor_msgs::NavSatFix &msg) {
  Pelican_latitude_ = boost::optional<double>{msg.location_x * map_scale_};
  Pelican_longitude_ = boost::optional<double>{msg.location_y * map_scale_};

  Pelican_visualization_node_->UpdateMap(Pelican_latitude_.get(),
                                         Pelican_longitude_.get());
  visualization_msgs::Marker marker = Pelican_visualization_node_->GetMarker();
  Pelican_position_pub_.publish(marker);
}

void SamplingCore::Update() {
  if (update_flag_) {
    ROS_INFO_STREAM("Update!");
    update_flag_ = false;
    UpdateModel();
    double rms = RMSError(gt_mean_, mean_prediction_);
    ROS_INFO_STREAM("RME Error : " << rms);
  }
  UpdateVisualization();
}

}  // namespace core
}  // namespace sampling
