/*********************************************************************
* Software License Agreement (BSD License)
*
*  Copyright (c) 2008, Willow Garage, Inc.
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of Willow Garage, Inc. nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
********************************************************************/

#include "rosbag_rec_server/recorder.h"

#include <sys/stat.h>
#include <boost/filesystem.hpp>
// Boost filesystem v3 is default in 1.46.0 and above
// Fallback to original posix code (*nix only) if this is not true
#if BOOST_FILESYSTEM_VERSION < 3
  #include <sys/statvfs.h>
#endif
#include <time.h>

#include <queue>
#include <set>
#include <sstream>
#include <string>

#include <boost/foreach.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/regex.hpp>
#include <boost/thread.hpp>
#include <boost/thread/xtime.hpp>
#include <boost/date_time/local_time/local_time.hpp>
#include <boost/program_options.hpp>
#include <boost/tokenizer.hpp>

#include <ros/ros.h>
#include <topic_tools/shape_shifter.h>

#include "ros/network.h"
#include "ros/xmlrpc_manager.h"
#include "XmlRpc.h"

#include "rosbag/exceptions.h"

#define foreach BOOST_FOREACH

using std::cout;
using std::endl;
using std::set;
using std::string;
using std::vector;
using boost::shared_ptr;
using ros::Time;

namespace po = boost::program_options;

namespace rosbag {

// http://stackoverflow.com/questions/18378798/use-boost-program-options-to-parse-an-arbitrary-string
// copy_if was left out of the C++03 standard, so mimic the C++11
// behaviour to support all predicate types.  The alternative is to
// use remove_copy_if, but it only works for adaptable functors
template <typename InputIterator,
          typename OutputIterator, 
          typename Predicate>
OutputIterator 
copy_if(InputIterator first,
        InputIterator last,
        OutputIterator result,
        Predicate pred)
{
  while(first != last)
  {
    if(pred(*first))
      *result++ = *first;
    ++first;
  }
  return result;
}

//! Parse the command-line arguments for recorder options
//rosbag::RecorderOptions parseOptions(int argc, char** argv) {
rosbag::RecorderOptions parseOptions(const std::string& args) {
    rosbag::RecorderOptions opts;

    po::options_description desc("Allowed options");

    desc.add_options()
      ("help,h", "produce help message")
      ("all,a", "record all topics")
      ("regex,e", "match topics using regular expressions")
      ("exclude,x", po::value<std::string>(), "exclude topics matching regular expressions")
      ("quiet,q", "suppress console output")
      ("output-prefix,o", po::value<std::string>(), "prepend PREFIX to beginning of bag name")
      ("output-name,O", po::value<std::string>(), "record bagnamed NAME.bag")
      ("buffsize,b", po::value<int>()->default_value(256), "Use an internal buffer of SIZE MB (Default: 256)")
      ("chunksize", po::value<int>()->default_value(768), "Set chunk size of message data, in KB (Default: 768. Advanced)")
      ("limit,l", po::value<int>()->default_value(0), "Only record NUM messages on each topic")
      ("bz2,j", "use BZ2 compression")
      ("split", po::value<int>()->implicit_value(0), "Split the bag file and continue recording when maximum size or maximum duration reached.")
      ("topic", po::value< std::vector<std::string> >(), "topic to record")
      ("size", po::value<int>(), "The maximum size of the bag to record in MB.")
      ("duration", po::value<std::string>(), "Record a bag of maximum duration in seconds, unless 'm', or 'h' is appended.")
      ("node", po::value<std::string>(), "Record all topics subscribed to by a specific node.");


    po::positional_options_description p;
    p.add("topic", -1);

    po::variables_map vm;

    try
    {
      typedef boost::escaped_list_separator<char> separator_type;
      separator_type separator("\\",    // The escape characters
                               "= ",    // The separator characters
                               "\"\'"); // The quote characters

      // Tokenise the intput
      boost::tokenizer<separator_type> tokens(args, separator);

      // Copy non-empty tokens from the tokenizer into the result
      std::vector<std::string> args_vec;
      rosbag::copy_if(tokens.begin(), tokens.end(), std::back_inserter(args_vec), 
              !boost::bind(&std::string::empty, _1));
          
      //po::store(po::command_line_parser(argc, argv).options(desc).positional(p).run(), vm);
      po::store(po::command_line_parser(args_vec).options(desc).positional(p).run(), vm);
    } catch (boost::program_options::invalid_command_line_syntax& e)
    {
      throw ros::Exception(e.what());
    }  catch (boost::program_options::unknown_option& e)
    {
      throw ros::Exception(e.what());
    }

    if (vm.count("help")) {
      std::cout << desc << std::endl;
      exit(0);
    }

    if (vm.count("all"))
      opts.record_all = true;
    if (vm.count("regex"))
      opts.regex = true;
    if (vm.count("exclude"))
    {
      opts.do_exclude = true;
      opts.exclude_regex = vm["exclude"].as<std::string>();
    }
    if (vm.count("quiet"))
      opts.quiet = true;
    if (vm.count("output-prefix"))
    {
      opts.prefix = vm["output-prefix"].as<std::string>();
      opts.append_date = true;
    }
    if (vm.count("output-name"))
    {
      opts.prefix = vm["output-name"].as<std::string>();
      opts.append_date = false;
    }
    if (vm.count("split"))
    {
      opts.split = true;

      int S = vm["split"].as<int>();
      if (S != 0)
      {
        ROS_WARN("Use of \"--split <MAX_SIZE>\" has been deprecated.  Please use --split --size <MAX_SIZE> or --split --duration <MAX_DURATION>");
        if (S < 0)
          throw ros::Exception("Split size must be 0 or positive");
        opts.max_size = 1048576 * S;
      }
    }
    if (vm.count("buffsize"))
    {
      int m = vm["buffsize"].as<int>();
      if (m < 0)
        throw ros::Exception("Buffer size must be 0 or positive");
      opts.buffer_size = 1048576 * m;
    }
    if (vm.count("chunksize"))
    {
      int chnk_sz = vm["chunksize"].as<int>();
      if (chnk_sz < 0)
        throw ros::Exception("Chunk size must be 0 or positive");
      opts.chunk_size = 1024 * chnk_sz;
    }
    if (vm.count("limit"))
    {
      opts.limit = vm["limit"].as<int>();
    }
    if (vm.count("bz2"))
    {
      opts.compression = rosbag::compression::BZ2;
    }
    if (vm.count("duration"))
    {
      std::string duration_str = vm["duration"].as<std::string>();

      double duration;
      double multiplier = 1.0;
      std::string unit("");

      std::istringstream iss(duration_str);
      if ((iss >> duration).fail())
        throw ros::Exception("Duration must start with a floating point number.");

      if ( (!iss.eof() && ((iss >> unit).fail())) )
      {
        throw ros::Exception("Duration unit must be s, m, or h");
      }
      if (unit == std::string(""))
        multiplier = 1.0;
      else if (unit == std::string("s"))
        multiplier = 1.0;
      else if (unit == std::string("m"))
        multiplier = 60.0;
      else if (unit == std::string("h"))
        multiplier = 3600.0;
      else
        throw ros::Exception("Duration unit must be s, m, or h");


      opts.max_duration = ros::Duration(duration * multiplier);
      if (opts.max_duration <= ros::Duration(0))
        throw ros::Exception("Duration must be positive.");
    }
    if (vm.count("size"))
    {
      opts.max_size = vm["size"].as<int>() * 1048576;
      if (opts.max_size <= 0)
        throw ros::Exception("Split size must be 0 or positive");
    }
    if (vm.count("node"))
    {
      opts.node = vm["node"].as<std::string>();
      std::cout << "Recording from: " << opts.node << std::endl;
    }

    // Every non-option argument is assumed to be a topic
    if (vm.count("topic"))
    {
      std::vector<std::string> bags = vm["topic"].as< std::vector<std::string> >();
      for (std::vector<std::string>::iterator i = bags.begin();
           i != bags.end();
           i++)
        opts.topics.push_back(*i);
    }


    // check that argument combinations make sense
    if(opts.exclude_regex.size() > 0 &&
            !(opts.record_all || opts.regex)) {
        fprintf(stderr, "Warning: Exclusion regex given, but no topics to subscribe to.\n"
                "Adding implicit 'record all'.");
        opts.record_all = true;
    }

    return opts;
}

// OutgoingMessage

OutgoingMessage::OutgoingMessage(string const& _topic, topic_tools::ShapeShifter::ConstPtr _msg, boost::shared_ptr<ros::M_string> _connection_header, Time _time) :
    topic(_topic), msg(_msg), connection_header(_connection_header), time(_time)
{
}

// OutgoingQueue

OutgoingQueue::OutgoingQueue(string const& _filename, boost::shared_ptr<std::queue<OutgoingMessage> > _queue, Time _time) :
    filename(_filename), queue(_queue), time(_time)
{
}

// RecorderOptions

RecorderOptions::RecorderOptions() :
    trigger(false),
    record_all(false),
    regex(false),
    do_exclude(false),
    quiet(false),
    append_date(true),
    snapshot(false),
    verbose(false),
    compression(compression::Uncompressed),
    prefix(""),
    name(""),
    exclude_regex(),
    buffer_size(1048576 * 256),
    chunk_size(1024 * 768),
    limit(0),
    split(false),
    max_size(0),
    max_duration(-1.0),
    node("")
{
}

// Recorder

//Recorder::Recorder(RecorderOptions const& options) :
Recorder::Recorder() :
    //options_(options),
    num_subscribers_(0),
    exit_code_(0),
    recording_(false),
    queue_size_(0),
    split_count_(0),
    writing_enabled_(true),
    halt_recording_(false)
{
}

Recorder::~Recorder() {
//    delete queue_;
}

int Recorder::run() {
    if (options_.trigger) {
        doTrigger();
        return 0;
    }

    if (options_.topics.size() == 0) {
        // Make sure limit is not specified with automatic topic subscription
        if (options_.limit > 0) {
            fprintf(stderr, "Specifing a count is not valid with automatic topic subscription.\n");
            return 1;
        }

        // Make sure topics are specified
        if (!options_.record_all && (options_.node == std::string(""))) {
            fprintf(stderr, "No topics specified.\n");
            return 1;
        }
    }

    ros::NodeHandle nh;
    if (!nh.ok())
        return 0;

    last_buffer_warn_ = Time();
    //queue_ = new std::queue<OutgoingMessage>;
    queue_ = boost::make_shared<std::queue<OutgoingMessage> >();

    // Subscribe to each topic
    if (!options_.regex) {
    	foreach(string const& topic, options_.topics)
			subscribe(topic);
    }

    if (!ros::Time::waitForValid(ros::WallDuration(2.0)))
      ROS_WARN("/use_sim_time set to true and no clock published.  Still waiting for valid time...");

    ros::Time::waitForValid();

    start_time_ = ros::Time::now();

    // Don't bother doing anything if we never got a valid time
    if (!nh.ok())
        return 0;

    ros::Subscriber trigger_sub;

    // Spin up a thread for writing to the file
    //boost::thread record_thread;
    if (options_.snapshot)
    {
        record_thread_ = boost::thread(boost::bind(&Recorder::doRecordSnapshotter, this));

        // Subscribe to the snapshot trigger
        trigger_sub = nh.subscribe<std_msgs::Empty>("snapshot_trigger", 100, boost::bind(&Recorder::snapshotTrigger, this, _1));
    }
    else
        record_thread_ = boost::thread(boost::bind(&Recorder::doRecord, this));



    ros::Timer check_master_timer;
    if (options_.record_all || options_.regex || (options_.node != std::string("")))
        check_master_timer = nh.createTimer(ros::Duration(1.0), boost::bind(&Recorder::doCheckMaster, this, _1, boost::ref(nh)));

    recording_ = true;

    //ros::MultiThreadedSpinner s(10);
    //ros::spin(s);
    //return stop();
    return 0;
}

int Recorder::stop() {
    if (recording_) {
        boost::unique_lock<boost::mutex> lock(queue_mutex_);
        halt_recording_ = true;
        lock.unlock();
        queue_condition_.notify_all();
        record_thread_.join();

        // restore the values set in constructor
        num_subscribers_ = 0;
        recording_ = false;
        queue_size_ = 0;
        split_count_ = 0;
        writing_enabled_ = true;
        halt_recording_ = false;
        //delete queue_;
    }
    return exit_code_;
}

bool Recorder::serviceCb(rosbag_rec_server::RecServer::Request& req,
               rosbag_rec_server::RecServer::Response& res) {

    if (req.command == 0 && recording_ == false) {
        // Parse the command-line options coming from service call
        //rosbag::RecorderOptions opts;
        try {
            options_ = rosbag::parseOptions(req.argv);
        }
        catch (ros::Exception const& ex) {
            ROS_ERROR("Error reading options: %s", ex.what());
            res.return_code = 1;
            return true;
        }
        catch(boost::regex_error const& ex) {
            ROS_ERROR("Error reading options: %s\n", ex.what());
            res.return_code = 1;
            return true;
        }
        run();
        res.return_code = 0;
    }
    else if (req.command == 1 && recording_ == true) {
        res.return_code = stop();
    }
    else {
        res.return_code = 1;
    }

    return true;
}

shared_ptr<ros::Subscriber> Recorder::subscribe(string const& topic) {
	ROS_INFO("Subscribing to %s", topic.c_str());

    ros::NodeHandle nh;
    shared_ptr<int> count(new int(options_.limit));
    shared_ptr<ros::Subscriber> sub(new ros::Subscriber);
    *sub = nh.subscribe<topic_tools::ShapeShifter>(topic, 100, boost::bind(&Recorder::doQueue, this, _1, topic, sub, count));
    currently_recording_.insert(topic);
    num_subscribers_++;

    return sub;
}

bool Recorder::isSubscribed(string const& topic) const {
    return currently_recording_.find(topic) != currently_recording_.end();
}

bool Recorder::shouldSubscribeToTopic(std::string const& topic, bool from_node) {
    // ignore already known topics
    if (isSubscribed(topic)) {
        return false;
    }

    // subtract exclusion regex, if any
    if(options_.do_exclude && boost::regex_match(topic, options_.exclude_regex)) {
        return false;
    }

    if(options_.record_all || from_node) {
        return true;
    }
    
    if (options_.regex) {
        // Treat the topics as regular expressions
        foreach(string const& regex_str, options_.topics) {
            boost::regex e(regex_str);
            boost::smatch what;
            if (boost::regex_match(topic, what, e, boost::match_extra))
                return true;
        }
    }
    else {
        foreach(string const& t, options_.topics)
            if (t == topic)
                return true;
    }
    
    return false;
}

template<class T>
std::string Recorder::timeToStr(T ros_t)
{
    std::stringstream msg;
    const boost::posix_time::ptime now=
        boost::posix_time::second_clock::local_time();
    boost::posix_time::time_facet *const f=
        new boost::posix_time::time_facet("%Y-%m-%d-%H-%M-%S");
    msg.imbue(std::locale(msg.getloc(),f));
    msg << now;
    return msg.str();
}

//! Callback to be invoked to save messages into a queue
void Recorder::doQueue(ros::MessageEvent<topic_tools::ShapeShifter const> msg_event, string const& topic, shared_ptr<ros::Subscriber> subscriber, shared_ptr<int> count) {
    //void Recorder::doQueue(topic_tools::ShapeShifter::ConstPtr msg, string const& topic, shared_ptr<ros::Subscriber> subscriber, shared_ptr<int> count) {
    Time rectime = Time::now();
    
    if (options_.verbose)
        cout << "Received message on topic " << subscriber->getTopic() << endl;

    OutgoingMessage out(topic, msg_event.getMessage(), msg_event.getConnectionHeaderPtr(), rectime);
    
    {
        boost::mutex::scoped_lock lock(queue_mutex_);

        queue_->push(out);
        queue_size_ += out.msg->size();
        
        // Check to see if buffer has been exceeded
        while (options_.buffer_size > 0 && queue_size_ > options_.buffer_size) {
            OutgoingMessage drop = queue_->front();
            queue_->pop();
            queue_size_ -= drop.msg->size();

            if (!options_.snapshot) {
                Time now = Time::now();
                if (now > last_buffer_warn_ + ros::Duration(5.0)) {
                    ROS_WARN("rosbag record buffer exceeded.  Dropping oldest queued message.");
                    last_buffer_warn_ = now;
                }
            }
        }
    }
  
    if (!options_.snapshot)
        queue_condition_.notify_all();

    // If we are book-keeping count, decrement and possibly shutdown
    if ((*count) > 0) {
        (*count)--;
        if ((*count) == 0) {
            subscriber->shutdown();

            num_subscribers_--;

            if (num_subscribers_ == 0)
                ros::shutdown();
        }
    }
}

void Recorder::updateFilenames() {
    vector<string> parts;

    std::string prefix = options_.prefix;
    uint32_t ind = prefix.rfind(".bag");

    if (ind == prefix.size() - 4)
    {
      prefix.erase(ind);
      ind = prefix.rfind(".bag");
    }

    if (prefix.length() > 0)
        parts.push_back(prefix);
    if (options_.append_date)
        parts.push_back(timeToStr(ros::WallTime::now()));
    if (options_.split)
        parts.push_back(boost::lexical_cast<string>(split_count_));

    target_filename_ = parts[0];
    for (unsigned int i = 1; i < parts.size(); i++)
        target_filename_ += string("_") + parts[i];

    target_filename_ += string(".bag");
    write_filename_ = target_filename_ + string(".active");
}

//! Callback to be invoked to actually do the recording
void Recorder::snapshotTrigger(std_msgs::Empty::ConstPtr trigger) {
    updateFilenames();
    
    ROS_INFO("Triggered snapshot recording with name %s.", target_filename_.c_str());
    
    {
        boost::mutex::scoped_lock lock(queue_mutex_);
        queue_queue_.push(OutgoingQueue(target_filename_, queue_, Time::now()));
        queue_.reset();
        //queue_      = new std::queue<OutgoingMessage>;
        queue_      = boost::make_shared<std::queue<OutgoingMessage> >();
        queue_size_ = 0;
    }

    queue_condition_.notify_all();
}

void Recorder::startWriting() {
    bag_.setCompression(options_.compression);
    bag_.setChunkThreshold(options_.chunk_size);

    updateFilenames();
    try {
        bag_.open(write_filename_, bagmode::Write);
    }
    catch (rosbag::BagException e) {
        ROS_ERROR("Error writing: %s", e.what());
        exit_code_ = 1;
        ros::shutdown();
    }
    ROS_INFO("Recording to %s.", target_filename_.c_str());
}

void Recorder::stopWriting() {
    ROS_INFO("Closing %s.", target_filename_.c_str());
    bag_.close();
    rename(write_filename_.c_str(), target_filename_.c_str());
}

bool Recorder::checkSize()
{
    if (options_.max_size > 0)
    {
        if (bag_.getSize() > options_.max_size)
        {
            if (options_.split)
            {
                stopWriting();
                split_count_++;
                startWriting();
            } else {
                ros::shutdown();
                return true;
            }
        }
    }
    return false;
}

bool Recorder::checkDuration(const ros::Time& t)
{
    if (options_.max_duration > ros::Duration(0))
    {
        if (t - start_time_ > options_.max_duration)
        {
            if (options_.split)
            {
                while (start_time_ + options_.max_duration < t)
                {
                    stopWriting();
                    split_count_++;
                    start_time_ += options_.max_duration;
                    startWriting();
                }
            } else {
                ros::shutdown();
                return true;
            }
        }
    }
    return false;
}


//! Thread that actually does writing to file.
void Recorder::doRecord() {
    // Open bag file for writing
    startWriting();

    // Schedule the disk space check
    warn_next_ = ros::WallTime();
    checkDisk();
    check_disk_next_ = ros::WallTime::now() + ros::WallDuration().fromSec(20.0);

    // Technically the queue_mutex_ should be locked while checking empty.
    // Except it should only get checked if the node is not ok, and thus
    // it shouldn't be in contention.
    ros::NodeHandle nh;
    while (nh.ok() || !queue_->empty()) {
        boost::unique_lock<boost::mutex> lock(queue_mutex_);

        bool finished = false;
        while (queue_->empty()) {
            if (!nh.ok() || halt_recording_ == true) {
                lock.release()->unlock();
                finished = true;
                break;
            }
            boost::xtime xt;
#if BOOST_VERSION >= 105000
            boost::xtime_get(&xt, boost::TIME_UTC_);
#else
            boost::xtime_get(&xt, boost::TIME_UTC);
#endif
            xt.nsec += 250000000;
            queue_condition_.timed_wait(lock, xt);
            if (checkDuration(ros::Time::now()))
            {
                finished = true;
                break;
            }
        }
        if (finished)
            break;

        OutgoingMessage out = queue_->front();
        queue_->pop();
        queue_size_ -= out.msg->size();
        
        lock.release()->unlock();
        
        if (checkSize())
            break;

        if (checkDuration(out.time))
            break;

        if (scheduledCheckDisk() && checkLogging())
            bag_.write(out.topic, out.time, *out.msg, out.connection_header);
    }

    stopWriting();
}

void Recorder::doRecordSnapshotter() {
    ros::NodeHandle nh;
  
    while (nh.ok() || !queue_queue_.empty()) {
        boost::unique_lock<boost::mutex> lock(queue_mutex_);
        while (queue_queue_.empty()) {
            if (!nh.ok())
                return;
            queue_condition_.wait(lock);
        }
        
        OutgoingQueue out_queue = queue_queue_.front();
        queue_queue_.pop();
        
        lock.release()->unlock();
        
        string target_filename = out_queue.filename;
        string write_filename  = target_filename + string(".active");
        
        try {
            bag_.open(write_filename, bagmode::Write);
        }
        catch (rosbag::BagException ex) {
            ROS_ERROR("Error writing: %s", ex.what());
            return;
        }

        while (!out_queue.queue->empty()) {
            OutgoingMessage out = out_queue.queue->front();
            out_queue.queue->pop();

            bag_.write(out.topic, out.time, *out.msg);
        }

        stopWriting();
    }
}

void Recorder::doCheckMaster(ros::TimerEvent const& e, ros::NodeHandle& node_handle) {
    ros::master::V_TopicInfo topics;
    if (ros::master::getTopics(topics)) {
		foreach(ros::master::TopicInfo const& t, topics) {
			if (shouldSubscribeToTopic(t.name))
				subscribe(t.name);
		}
    }
    
    if (options_.node != std::string(""))
    {

      XmlRpc::XmlRpcValue req;
      req[0] = ros::this_node::getName();
      req[1] = options_.node;
      XmlRpc::XmlRpcValue resp;
      XmlRpc::XmlRpcValue payload;

      if (ros::master::execute("lookupNode", req, resp, payload, true))
      {
        std::string peer_host;
        uint32_t peer_port;

        if (!ros::network::splitURI(static_cast<std::string>(resp[2]), peer_host, peer_port))
        {
          ROS_ERROR("Bad xml-rpc URI trying to inspect node at: [%s]", static_cast<std::string>(resp[2]).c_str());
        } else {

          XmlRpc::XmlRpcClient c(peer_host.c_str(), peer_port, "/");
          XmlRpc::XmlRpcValue req;
          XmlRpc::XmlRpcValue resp;
          req[0] = ros::this_node::getName();
          c.execute("getSubscriptions", req, resp);
          
          if (!c.isFault() && resp.size() > 0 && static_cast<int>(resp[0]) == 1)
          {
            for(int i = 0; i < resp[2].size(); i++)
            {
              if (shouldSubscribeToTopic(resp[2][i][0], true))
                subscribe(resp[2][i][0]);
            }
          } else {
            ROS_ERROR("Node at: [%s] failed to return subscriptions.", static_cast<std::string>(resp[2]).c_str());
          }
        }
      }
    }
}

void Recorder::doTrigger() {
    ros::NodeHandle nh;
    ros::Publisher pub = nh.advertise<std_msgs::Empty>("snapshot_trigger", 1, true);
    pub.publish(std_msgs::Empty());

    ros::Timer terminate_timer = nh.createTimer(ros::Duration(1.0), boost::bind(&ros::shutdown));
    ros::spin();
}

bool Recorder::scheduledCheckDisk() {
    boost::mutex::scoped_lock lock(check_disk_mutex_);

    if (ros::WallTime::now() < check_disk_next_)
        return true;

    check_disk_next_ += ros::WallDuration().fromSec(20.0);
    return checkDisk();
}

bool Recorder::checkDisk() {
#if BOOST_FILESYSTEM_VERSION < 3
    struct statvfs fiData;
    if ((statvfs(bag_.getFileName().c_str(), &fiData)) < 0)
    {
        ROS_WARN("Failed to check filesystem stats.");
        return true;
    }
    unsigned long long free_space = 0;
    free_space = (unsigned long long) (fiData.f_bsize) * (unsigned long long) (fiData.f_bavail);
    if (free_space < 1073741824ull)
    {
        ROS_ERROR("Less than 1GB of space free on disk with %s.  Disabling recording.", bag_.getFileName().c_str());
        writing_enabled_ = false;
        return false;
    }
    else if (free_space < 5368709120ull)
    {
        ROS_WARN("Less than 5GB of space free on disk with %s.", bag_.getFileName().c_str());
    }
    else
    {
        writing_enabled_ = true;
    }
#else
    boost::filesystem::path p(boost::filesystem::system_complete(bag_.getFileName().c_str()));
    p = p.parent_path();
    boost::filesystem::space_info info;
    try
    {
        info = boost::filesystem::space(p);
    }
    catch (boost::filesystem::filesystem_error &e) 
    { 
        ROS_WARN("Failed to check filesystem stats [%s].", e.what());
        writing_enabled_ = false;
        return false;
    }
    if ( info.available < 1073741824ull)
    {
        ROS_ERROR("Less than 1GB of space free on disk with %s.  Disabling recording.", bag_.getFileName().c_str());
        writing_enabled_ = false;
        return false;
    }
    else if (info.available < 5368709120ull)
    {
        ROS_WARN("Less than 5GB of space free on disk with %s.", bag_.getFileName().c_str());
        writing_enabled_ = true;
    }
    else
    {
        writing_enabled_ = true;
    }
#endif
    return true;
}

bool Recorder::checkLogging() {
    if (writing_enabled_)
        return true;

    ros::WallTime now = ros::WallTime::now();
    if (now >= warn_next_) {
        warn_next_ = now + ros::WallDuration().fromSec(5.0);
        ROS_WARN("Not logging message because logging disabled.  Most likely cause is a full disk.");
    }
    return false;
}

} // namespace rosbag
