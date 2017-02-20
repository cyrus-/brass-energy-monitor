#include <gazebo/gazebo.hh>
#include <gazebo/physics/physics.hh>
#include <gazebo/common/common.hh>

#include <boost/thread/mutex.hpp>

#include <cmath>
#include <thread>

#include "ros/ros.h"
#include "ros/callback_queue.h"
#include "ros/subscribe_options.h"
#include "std_msgs/Float64.h"
#include "std_msgs/Bool.h"

namespace gazebo {
  class EnergyMonitorPlugin : public ModelPlugin {
    public: 
		EnergyMonitorPlugin() : ModelPlugin() {
				gzdbg << "Constructed energy_monitor." << "\n";
            }

		~EnergyMonitorPlugin() {
			this->rosNode->shutdown();
		}
	
	private:
		const double SEC_PER_HR = 3600.0;

		// A node use for ROS transport
		std::unique_ptr<ros::NodeHandle> rosNode;

		// A ROS subscriber
		ros::Subscriber rosSub;

		// A ROS publisher
		ros::Publisher chargeStatePub;

		// A lock to prevent ticks from happening at the same time as messages.
		boost::mutex lock;

		// A ROS callbackqueue that helps process messages
		ros::CallbackQueue rosQueue;

		// A thread the keeps the running rosQueue
		std::thread rosQueueThread;

		// Charge level
		bool charging;
		const double charge_rate = 500.0 / SEC_PER_HR /* mwh / sec */;


		const double battery_capacity /* mwh */ = 10000.0; // TODO get actual number 
		
		const double delta_base_FULLSPEED /* mwh / sec */ = 14004.0 /* mwh / hr */ / SEC_PER_HR; 
		const double delta_base_HALFSPEED /* mwh / sec */= 6026.0 / SEC_PER_HR;
		const double delta_base_STOPPED /* mwh / sec */ = 0.0 / SEC_PER_HR;
		enum Speed { FULLSPEED, HALFSPEED, STOPPED };
		Speed speed = FULLSPEED;
		double delta_base_of(Speed speed) {
			switch(speed) {
				case FULLSPEED: return delta_base_FULLSPEED; break;
				case HALFSPEED: return delta_base_HALFSPEED; break;
				case STOPPED:   return delta_base_STOPPED; break;
			}
		}

		const double delta_kinect_USED /* mwh / sec */ = 5132.0 / SEC_PER_HR;
		const double delta_kinect_UNUSED /* mwh / sec */ = 250.0 / SEC_PER_HR;
		enum KinectState { USED, UNUSED };
		KinectState kinectState = USED;
		double delta_kinect_of(KinectState kinectState) {
			switch(kinectState) {
				case USED: return delta_kinect_USED; break;
				case UNUSED: return delta_kinect_UNUSED; break;
			}
		}

		const double delta_nuc /* mwh / sec */ = 11088.0 / SEC_PER_HR;

		double cur_charge /* mwh */;

		// Time management
		double last_time /* s */;
		double last_print_time /* s */ = -1.0;

		// gazebo stuff
		physics::WorldPtr world;
		event::ConnectionPtr updateConnection;

    public: 
		void Load(physics::ModelPtr _model, sdf::ElementPtr _sdf) {
			this->world = _model->GetWorld();

			last_time = this->world->GetSimTime().Double(); 
			gzdbg << "Initial time: " << last_time << "\n";

			cur_charge = battery_capacity;
			charging = false;

			// Register callback for every simulation tick
			this->updateConnection = event::Events::ConnectWorldUpdateBegin( 
					boost::bind(&EnergyMonitorPlugin::UpdateChild, this)); 

			// Initialize ros, if it has not already bee initialized.
			if (!ros::isInitialized())
			{
			  int argc = 0;
			  char **argv = NULL;
			  ros::init(argc, argv, "energy_monitor_client",
				  ros::init_options::NoSigintHandler);
			}

			// Create our ROS node. This acts in a similar manner to
			// the Gazebo node
			this->rosNode.reset(new ros::NodeHandle("energy_monitor_client"));

			// Create a named topic, and subscribe to it.
			ros::SubscribeOptions so =
			  ros::SubscribeOptions::create<std_msgs::Bool>(
				  "/energy_monitor/set_charging",
				  1,
				  boost::bind(&EnergyMonitorPlugin::OnSetChargingMsg, this, _1),
				  ros::VoidPtr(), &this->rosQueue);
			this->rosSub = this->rosNode->subscribe(so);

			// Publish a topic
			this->chargeStatePub = this->rosNode->advertise<std_msgs::Float64>(
					"/energy_monitor/power_state",
					1);

			// Spin up the queue helper thread.
			this->rosQueueThread =
			  std::thread(std::bind(&EnergyMonitorPlugin::QueueThread, this));

			gzdbg << "Loaded energy_monitor." << "\n";
		}

		void UpdateChild() {
			// Update time
			double curr_time = this->world->GetSimTime().Double(); // measured in seconds
			double dt = curr_time - last_time; 
			last_time = curr_time;
		    
			if (charging) {
				cur_charge += charge_rate * dt;
			} else {
				double delta_base = delta_base_of(speed);
				double delta_kinect = delta_kinect_of(kinectState);
				double delta_discharging_energy = 
					- (delta_base + delta_kinect + delta_nuc);
				cur_charge += delta_discharging_energy * dt;
			}

			if (cur_charge < 0.0) {
				cur_charge = 0.0;
			}
			
			if ((curr_time - last_print_time) >= 1.0) {
				gzdbg << "current charge: " << cur_charge << "\n";
				last_print_time = curr_time;
			}

			std_msgs::Float64 msg;
			msg.data = cur_charge;
			lock.lock();
			this->chargeStatePub.publish(msg);
			lock.unlock();
		}

		// Handle an incoming message from ROS
		public: void OnSetChargingMsg(const std_msgs::BoolConstPtr &_msg)
		{
			lock.lock();
			charging = _msg->data;
			gzdbg << "received message" << charging << "\n";
			lock.unlock();
		}

		/// ROS helper function that processes messages
		private: void QueueThread()
		{
		  static const double timeout = 0.01;
		  while (this->rosNode->ok())
		  {
			this->rosQueue.callAvailable(ros::WallDuration(timeout));
		  }
		}
  };
  GZ_REGISTER_MODEL_PLUGIN(EnergyMonitorPlugin)
}

