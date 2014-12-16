/**
 */

#ifndef SR_MVSIM_NODE_CORE_H
#define SR_MVSIM_NODE_CORE_H

// ROS includes.
#include <ros/ros.h>
#include <ros/time.h>
#include <rosgraph_msgs/Clock.h>

// Dynamic reconfigure includes.
#include <dynamic_reconfigure/server.h>

#include <tf/transform_broadcaster.h>

// Auto-generated from cfg/ directory.
#include <mvsim/mvsimNodeConfig.h>

#include <mvsim/mvsim.h>  // the mvsim library
#include <mrpt/system/threads.h> // sleep(), thread handles
#include <mrpt/utils/CTicTac.h>

/** A class to wrap libmvsim as a ROS node
  */
class MVSimNode
{
public:
	/** Constructor. */
	MVSimNode(ros::NodeHandle &n);
	/** Destructor. */
	~MVSimNode();

	void loadWorldModel(const std::string &world_xml_file);

	void spin(); //!< Process pending msgs, run real-time simulation, etc.

	/** Callback function for dynamic reconfigure server */
	void configCallback(mvsim::mvsimNodeConfig &config, uint32_t level);


	mvsim::World  mvsim_world_; //!< The mvsim library simulated world (includes everything: vehicles, obstacles, etc.)

	double realtime_factor_; //!< (Defaul=1.0) >1: speed-up, <1: slow-down
	int    gui_refresh_period_ms_; //!< Default:25
	bool   m_show_gui;   //!< Default= true

protected:
	ros::NodeHandle &m_n;
	ros::NodeHandle m_localn;

	// === ROS Publishers ====
	ros::Publisher m_pub_map_ros, m_pub_map_metadata; //!< used for simul_map publication
	ros::Publisher m_pub_clock;

	tf::TransformBroadcaster tf_br_; //!< Use to send data to TF
	ros::Publisher m_odo_publisher;
	// === End ROS Publishers ====

	rosgraph_msgs::Clock m_clockMsg;
	ros::Time      m_sim_time; //!< Current simulation time
	ros::Time      m_base_last_cmd;  //!< Last time we received a vel_cmd (for watchdog)
	ros::Duration  m_base_watchdog_timeout;


	struct TThreadParams
	{
		MVSimNode *obj;
		volatile bool closing;
		TThreadParams(): obj(NULL), closing(false) {}
	};
	TThreadParams thread_params_;
	mrpt::utils::CTicTac realtime_tictac_;

	double t_old_; // = realtime_tictac_.Tac();
	bool   world_init_ok_; //!< will be true after a success call to loadWorldModel()

	double m_period_ms_publish_tf;    //!< Minimum period between publication of TF transforms & /*/odom topics (In ms)
	mrpt::utils::CTicTac   m_tim_publish_tf;

	double m_period_ms_teleop_refresh; //!< Minimum period between update of live info & read of teleop key strokes in GUI (In ms)
	mrpt::utils::CTicTac   m_tim_teleop_refresh;

	size_t m_teleop_idx_veh;  //!< for teleoperation from the GUI (selects the "focused" vehicle)
	mvsim::World::TGUIKeyEvent m_gui_key_events;
	std::string m_msg2gui;

	mrpt::system::TThreadHandle thGUI_;
	static void thread_update_GUI(TThreadParams &thread_params);

	/** Publish relevant stuff whenever a new world model is loaded (grid maps, etc.) */
	void notifyROSWorldIsUpdated();

	/** Publish everything to be published at each simulation iteration */
	void spinNotifyROS();

	/** Publish the ground truth pose of a robot to tf as: map -> <ROBOT>/base_pose_ground_truth */
	void broadcastTF_GTPose(
		const mrpt::math::TPose3D &pose,
		const std::string &robotName = std::string("r1"));

	/** Publish "odometry" for a robot to tf as: odom -> <ROBOT>/base_link */
	void broadcastTF_Odom(
		const mrpt::math::TPose3D &pose,
		const std::string &robotName = std::string("r1"));

	/** Publish pose to tf: parentFrame -> <ROBOT>/base_link */
	void broadcastTF(
		const mrpt::math::TPose3D &pose,
		const std::string &parentFrame,
		const std::string &childFrame);


	struct MVSimVisitor_notifyROSWorldIsUpdated :
			public mvsim::World::VehicleVisitorBase,
			public mvsim::World::WorldElementVisitorBase
	{
		void visit(mvsim::VehicleBase *obj);
		void visit(mvsim::WorldElementBase *obj);

		MVSimVisitor_notifyROSWorldIsUpdated(MVSimNode &parent) : m_parent(parent) {}
		MVSimNode &m_parent;
	};

}; // end class

#endif // SR_MVSIM_NODE_CORE_H
