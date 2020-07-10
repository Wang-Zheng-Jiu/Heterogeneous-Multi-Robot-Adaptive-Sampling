#pragma once

#include <ros/ros.h>
#include <sampling_msgs/measurement.h>
#include <sensor_msgs/NavSatFix.h>

#include <boost/optional.hpp>
#include <queue>
#include <string>
#include <unordered_map>

#include "sampling_core/gmm.h"
#include "sampling_core/gpmm.h"
#include "sampling_core/informative_point_selection.h"
#include "sampling_core/sampling_visualization.h"
#include "sampling_core/utils.h"
#include "sampling_core/voronoi.h"
#include "sampling_msgs/SamplingGoal.h"

namespace sampling {
namespace core {

class GPSHashFunction {
 public:
  double operator()(const Eigen::MatrixXd &GPS) const {
    return (GPS(0, 1) + 180.0) * 180.0 + GPS(0, 0);
  }
};

using pq = std::priority_queue<std::pair<double, int>,
                               std::vector<std::pair<double, int>>,
                               std::less<std::pair<double, int>>>;

class SamplingCore {
 public:
  SamplingCore(const ros::NodeHandle &nh, const ros::NodeHandle &rh);

  bool Init();

  bool AssignInterestPoint(sampling_msgs::SamplingGoal::Request &req,
                           sampling_msgs::SamplingGoal::Response &res);

  bool ParseFromRosParam();

  bool InitializeVisualization();

  void UpdateModel();

  void UpdateVisualization();

  void Update();

  bool LoadMapParam(XmlRpc::XmlRpcValue &YamlNode,
                    visualization::MAP_PARAM &param);

  double RMSError(const Eigen::VectorXd &val1, const Eigen::VectorXd &val2);

 private:
  // ROS
  ros::NodeHandle nh_, rh_;
  ros::Publisher distribution_visualization_pub_, Jackal_position_pub_,
      Pelican_position_pub_;
  ros::Subscriber sample_sub_;
  ros::Subscriber Jackal_GPS_sub_;
  ros::Subscriber Pelican_GPS_sub_;

  Eigen::VectorXd gt_mean_, gt_var_;

  void CollectSampleCallback(const sampling_msgs::measurement &msg);

  void JackalGPSCallback(const sensor_msgs::NavSatFix &msg);

  void PelicanGPSCallback(const sensor_msgs::NavSatFix &msg);

  boost::optional<double> Jackal_latitude_, Jackal_longitude_;
  boost::optional<double> Pelican_latitude_, Pelican_longitude_;

  // interest point assignment

  // model parameter
  int num_gaussian_;
  std::vector<std::vector<double>> gp_hyperparams_;
  int max_iteration_;
  double eps_;
  double map_scale_;
  int gt_num_gaussian_;
  std::vector<std::vector<double>> gt_gp_hyperparams_;

  std::unique_ptr<gpmm::GaussianProcessMixtureModel> model_;
  std::unique_ptr<gpmm::GaussianProcessMixtureModel> gt_model_;
  int model_update_rate_;

  // data
  Eigen::MatrixXd location_;
  Eigen::MatrixXd ground_truth_location_;
  Eigen::MatrixXd ground_truth_temperature_;
  Eigen::MatrixXd init_sample_location_;
  Eigen::MatrixXd init_sample_temperature_;
  Eigen::VectorXd collected_temperatures_;
  Eigen::MatrixXd collected_locations_;

  // prediction
  Eigen::VectorXd mean_prediction_;
  Eigen::VectorXd var_prediction_;

  // Visualization
  std::unordered_map<std::string,
                     std::unique_ptr<visualization::SamplingVisualization>>
      visualization_node_;

  std::unique_ptr<visualization::RobotVisualization> Jackal_visualization_node_;
  std::unique_ptr<visualization::RobotVisualization>
      Pelican_visualization_node_;

  std::vector<visualization::MAP_PARAM> visualization_params_;

  // Informative Selection Params
  SAMPLINGMODE selection_mode_;
  double variance_coef_;
  std::unique_ptr<informative_sampling::InformativeSampling>
      informative_sampling_node_;

  // sampling
  bool update_flag_;
  int sample_size_;
  voronoi::Voronoi voronoi_node_;
  std::string Jackal_id_;
  std::string Pelican_id_;
  ros::ServiceClient Jackal_GPS_client_;
  ros::ServiceClient Pelican_GPS_client_;
  ros::ServiceServer interest_point_assignment_ser_;
  std::unordered_map<std::string, int> agent_id_;
};
}  // namespace core
}  // namespace sampling
