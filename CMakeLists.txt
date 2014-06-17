cmake_minimum_required(VERSION 2.8.3)
project(rosbag_rec_server)
find_package(catkin REQUIRED COMPONENTS rosbag_storage rosconsole roscpp topic_tools xmlrpcpp rospy std_msgs message_generation)
find_package(Boost REQUIRED COMPONENTS date_time regex program_options filesystem)
find_package(BZip2 REQUIRED)

# Support large bags (>2GB) on 32-bit systems
add_definitions(-D_FILE_OFFSET_BITS=64)

add_service_files(
  FILES
  RecServer.srv
)

generate_messages(
  DEPENDENCIES
  std_msgs
)

catkin_package(
  LIBRARIES rosbag_rec_server
  INCLUDE_DIRS include
  CATKIN_DEPENDS rosbag_storage rosconsole roscpp topic_tools xmlrpcpp rospy std_msgs message_runtime)

include_directories(include ${catkin_INCLUDE_DIRS} ${Boost_INCLUDE_DIRS})

add_library(rosbag_rec_server
  src/recorder.cpp
  src/time_translator.cpp)
add_dependencies(rosbag_rec_server rosbag_rec_server_gencpp)
target_link_libraries(rosbag_rec_server ${catkin_LIBRARIES} ${Boost_LIBRARIES})

add_executable(record_server src/record.cpp)
add_dependencies(record_server rosbag_rec_server_gencpp)
target_link_libraries(record_server rosbag_rec_server)

install(DIRECTORY include/${PROJECT_NAME}/
  DESTINATION ${CATKIN_PACKAGE_INCLUDE_DESTINATION}
  FILES_MATCHING PATTERN "*.h")
install(TARGETS rosbag_rec_server
  ARCHIVE DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
  LIBRARY DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
  RUNTIME DESTINATION ${CATKIN_GLOBAL_BIN_DESTINATION})
install(TARGETS record_server
  ARCHIVE DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
  LIBRARY DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
  RUNTIME DESTINATION ${CATKIN_PACKAGE_BIN_DESTINATION})
