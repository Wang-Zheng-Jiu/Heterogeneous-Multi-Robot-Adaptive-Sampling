cmake_minimum_required(VERSION 2.8.3)
project(sampling_msgs)

find_package(catkin REQUIRED COMPONENTS
  std_msgs
  geometry_msgs
  message_generation
)

add_message_files(
  FILES
  Sample.msg
  AgentLocation.msg
  SamplingPerformance.msg
)

add_service_files(
  FILES
  RequestLocation.srv
  SamplingGoal.srv
  RequestTemperatureMeasurement.srv
  MeasurementService.srv
  ReportSampleService.srv
  StopAgent.srv
  RequestMeasurement.srv
  AddSampleToModel.srv
  AddTestPositionToModel.srv
  ModelPredict.srv
  KillAgent.srv
)

generate_messages(
  DEPENDENCIES
  std_msgs
  geometry_msgs
)

catkin_package(
   CATKIN_DEPENDS std_msgs message_runtime
)