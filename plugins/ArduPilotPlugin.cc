/*
 * Copyright (C) 2016 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/
#include <functional>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <mutex>
#include <string>
#include <vector>
#include <sdf/sdf.hh>
#include <ignition/math/Filter.hh>
#include <gazebo/common/Assert.hh>
#include <gazebo/common/Plugin.hh>
#include <gazebo/msgs/msgs.hh>
#include <gazebo/sensors/sensors.hh>
#include <gazebo/transport/transport.hh>
#include "plugins/ArduPilotPlugin.hh"

#define MAX_MOTORS 255

using namespace gazebo;

GZ_REGISTER_MODEL_PLUGIN(ArduPilotPlugin)

/// \brief Obtains a parameter from sdf.
/// \param[in] _sdf Pointer to the sdf object.
/// \param[in] _name Name of the parameter.
/// \param[out] _param Param Variable to write the parameter to.
/// \param[in] _default_value Default value, if the parameter not available.
/// \param[in] _verbose If true, gzerror if the parameter is not available.
/// \return True if the parameter was found in _sdf, false otherwise.
template<class T>
bool getSdfParam(sdf::ElementPtr _sdf, const std::string &_name,
  T &_param, const T &_defaultValue, const bool &_verbose = false)
{
  if (_sdf->HasElement(_name))
  {
    _param = _sdf->GetElement(_name)->Get<T>();
    return true;
  }

  _param = _defaultValue;
  if (_verbose)
  {
    gzerr << "[ArduPilotPlugin] Please specify a value for parameter ["
      << _name << "].\n";
  }
  return false;
}

/// \brief A servo packet.
struct ServoPacket
{
  /// \brief Motor speed data.
  /// should rename to servo_command here and in ArduPilot SIM_Gazebo.cpp
  float motorSpeed[MAX_MOTORS];
};

/// \brief Flight Dynamics Model packet that is sent back to the ArduPilot
struct fdmPacket
{
  /// \brief packet timestamp
  double timestamp;

  /// \brief IMU angular velocity
  double imuAngularVelocityRPY[3];

  /// \brief IMU linear acceleration
  double imuLinearAccelerationXYZ[3];

  /// \brief IMU quaternion orientation
  double imuOrientationQuat[4];

  /// \brief Model velocity in NED frame
  double velocityXYZ[3];

  /// \brief Model position in NED frame
  double positionXYZ[3];
};

/// \brief Control class
class Control
{
  /// \brief Constructor
  public: Control()
  {
    // most of these coefficients are not used yet.
    this->rotorVelocitySlowdownSim = this->kDefaultRotorVelocitySlowdownSim;
    this->frequencyCutoff = this->kDefaultFrequencyCutoff;
    this->samplingRate = this->kDefaultSamplingRate;

    this->pid.Init(0.1, 0, 0, 0, 0, 1.0, -1.0);
  }

  /// \brief control id / channel
  public: int channel = 0;

  /// \brief Next command to be applied to the propeller
  public: double cmd = 0;

  /// \brief Velocity PID for motor control
  public: common::PID pid;

  /// \brief Control type. Can be:
  /// VELOCITY control velocity of joint
  /// POSITION control position of joint
  /// EFFORT control effort of joint
  public: std::string type;

  /// \brief Control propeller joint.
  public: std::string jointName;

  /// \brief Control propeller joint.
  public: physics::JointPtr joint;

  /// \brief direction multiplier for this control
  public: double multiplier = 1;

  /// \brief input command offset
  public: double offset = 0;

  /// \brief unused coefficients
  public: double rotorVelocitySlowdownSim;
  public: double frequencyCutoff;
  public: double samplingRate;
  public: ignition::math::OnePole<double> filter;

  public: static constexpr double kDefaultRotorVelocitySlowdownSim = 10.0;
  public: static constexpr double kDefaultFrequencyCutoff = 5.0;
  public: static constexpr double kDefaultSamplingRate = 0.2;
};

// Private data class
class gazebo::ArduPilotPluginPrivate
{
  /// \brief Bind to an adress and port
  /// \param[in] _address Address to bind to.
  /// \param[in] _port Port to bind to.
  /// \return True on success.
  public: bool Bind(const char *_address, const uint16_t _port)
  {
    struct sockaddr_in sockaddr;
    this->MakeSockAddr(_address, _port, sockaddr);

    if (bind(this->handle, (struct sockaddr *)&sockaddr, sizeof(sockaddr)) != 0)
    {
      shutdown(this->handle, 0);
      close(this->handle);
      return false;
    }
    return true;
  }

  /// \brief Make a socket
  /// \param[in] _address Socket address.
  /// \param[in] _port Socket port
  /// \param[out] _sockaddr New socket address structure.
  public: void MakeSockAddr(const char *_address, const uint16_t _port,
    struct sockaddr_in &_sockaddr)
  {
    memset(&_sockaddr, 0, sizeof(_sockaddr));

    #ifdef HAVE_SOCK_SIN_LEN
      _sockaddr.sin_len = sizeof(_sockaddr);
    #endif

    _sockaddr.sin_port = htons(_port);
    _sockaddr.sin_family = AF_INET;
    _sockaddr.sin_addr.s_addr = inet_addr(_address);
  }

  /// \brief Receive data
  /// \param[out] _buf Buffer that receives the data.
  /// \param[in] _size Size of the buffer.
  /// \param[in] _timeoutMS Milliseconds to wait for data.
  public: ssize_t Recv(void *_buf, const size_t _size, uint32_t _timeoutMs)
  {
    fd_set fds;
    struct timeval tv;

    FD_ZERO(&fds);
    FD_SET(this->handle, &fds);

    tv.tv_sec = _timeoutMs / 1000;
    tv.tv_usec = (_timeoutMs % 1000) * 1000UL;

    if (select(this->handle+1, &fds, NULL, NULL, &tv) != 1)
    {
        return -1;
    }

    return recv(this->handle, _buf, _size, 0);
  }

  /// \brief Pointer to the update event connection.
  public: event::ConnectionPtr updateConnection;

  /// \brief Pointer to the model;
  public: physics::ModelPtr model;

  /// \brief array of propellers
  public: std::vector<Control> controls;

  /// \brief keep track of controller update sim-time.
  public: gazebo::common::Time lastControllerUpdateTime;

  /// \brief Controller update mutex.
  public: std::mutex mutex;

  /// \brief Socket handle
  public: int handle;

  /// \brief Pointer to an IMU sensor
  public: sensors::ImuSensorPtr imuSensor;

  /// \brief false before ardupilot controller is online
  /// to allow gazebo to continue without waiting
  public: bool arduPilotOnline;

  /// \brief number of times ArduCotper skips update
  public: int connectionTimeoutCount;

  /// \brief number of times ArduCotper skips update
  /// before marking ArduPilot offline
  public: int connectionTimeoutMaxCount;
};

////////////////////////////////////////////////////////////////////////////////
ArduPilotPlugin::ArduPilotPlugin()
  : dataPtr(new ArduPilotPluginPrivate)
{
  // socket
  this->dataPtr->handle = socket(AF_INET, SOCK_DGRAM /*SOCK_STREAM*/, 0);
  fcntl(this->dataPtr->handle, F_SETFD, FD_CLOEXEC);
  int one = 1;
  setsockopt(this->dataPtr->handle, IPPROTO_TCP, TCP_NODELAY,
      &one, sizeof(one));

  if (!this->dataPtr->Bind("127.0.0.1", 9002))
  {
    gzerr << "failed to bind with 127.0.0.1:9002, aborting plugin.\n";
    return;
  }

  this->dataPtr->arduPilotOnline = false;

  this->dataPtr->connectionTimeoutCount = 0;

  setsockopt(this->dataPtr->handle, SOL_SOCKET, SO_REUSEADDR,
      &one, sizeof(one));

  fcntl(this->dataPtr->handle, F_SETFL,
      fcntl(this->dataPtr->handle, F_GETFL, 0) | O_NONBLOCK);
}

/////////////////////////////////////////////////
ArduPilotPlugin::~ArduPilotPlugin()
{
}

/////////////////////////////////////////////////
void ArduPilotPlugin::Load(physics::ModelPtr _model, sdf::ElementPtr _sdf)
{
  GZ_ASSERT(_model, "ArduPilotPlugin _model pointer is null");
  GZ_ASSERT(_sdf, "ArduPilotPlugin _sdf pointer is null");

  this->dataPtr->model = _model;

  // per control channel
  sdf::ElementPtr controlSDF;
  if (_sdf->HasElement("control"))
  {
    controlSDF = _sdf->GetElement("control");
  }
  else if (_sdf->HasElement("rotor"))
  {
    gzwarn << "please deprecate <rotor> block, use <control> block instead.\n";
    controlSDF = _sdf->GetElement("rotor");
  }

  while (controlSDF)
  {
    Control control;

    if (controlSDF->HasAttribute("channel"))
    {
      control.channel =
        atoi(controlSDF->GetAttribute("channel")->GetAsString().c_str());
    }
    else if (controlSDF->HasAttribute("id"))
    {
      gzwarn << "please deprecate attribute id, use channel instead.\n";
      control.channel =
        atoi(controlSDF->GetAttribute("id")->GetAsString().c_str());
    }
    else
    {
      control.channel = this->dataPtr->controls.size();
      gzwarn << "id/channel attribute not specified, use order parsed ["
             << control.channel << "].\n";
    }

    if (controlSDF->HasElement("type"))
    {
      control.type = controlSDF->Get<std::string>("type");
    }
    else
    {
      gzerr << "Control type not specified,"
            << " using velocity control by default.\n";
      control.type = "VELOCITY";
    }

    if (control.type != "VELOCITY" &&
        control.type != "POSITION" &&
        control.type != "EFFORT")
    {
      gzwarn << "Control type [" << control.type
             << "] not recognized, must be one of VELOCITY, POSITION, EFFORT."
             << " default to VELOCITY.\n";
      control.type = "VELOCITY";
    }

    if (controlSDF->HasElement("jointName"))
    {
      control.jointName = controlSDF->Get<std::string>("jointName");
    }
    else
    {
      gzerr << "Please specify a jointName,"
        << " where the control channel is attached.\n";
    }

    // Get the pointer to the joint.
    control.joint = _model->GetJoint(control.jointName);
    if (control.joint == nullptr)
    {
      gzerr << "Couldn't find specified joint ["
          << control.jointName << "]. This plugin will not run.\n";
      return;
    }

    if (controlSDF->HasElement("multiplier"))
    {
      // overwrite turningDirection, deprecated.
      control.multiplier = controlSDF->Get<double>("multiplier");;
    }
    else if (controlSDF->HasElement("turningDirection"))
    {
      gzwarn << "<turningDirection> is deprecated. Please use"
             << " <multiplier>. Map 'cw' to '-1' and 'ccw' to '1'.\n";
      std::string turningDirection = controlSDF->Get<std::string>(
          "turningDirection");
      // special cases mimic from controls_gazebo_plugins
      if (turningDirection == "cw")
      {
        control.multiplier = -1;
      }
      else if (turningDirection == "ccw")
      {
        control.multiplier = 1;
      }
      else
      {
        gzdbg << "not string, check turningDirection as float\n";
        control.multiplier = controlSDF->Get<double>("turningDirection");
      }
    }
    else
    {
      gzdbg << "<multiplier> (or deprecated <turningDirection>) not specified,"
            << " Default 1 (or deprecated <turningDirection> 'ccw').\n";
      control.multiplier = 1;
    }

    if (controlSDF->HasElement("offset"))
    {
      control.offset = controlSDF->Get<double>("offset");;
    }
    else
    {
      gzdbg << "<offset> not specified, default to 0.\n";
      control.offset = 0;
    }

    getSdfParam<double>(controlSDF, "rotorVelocitySlowdownSim",
        control.rotorVelocitySlowdownSim, 1);

    if (ignition::math::equal(control.rotorVelocitySlowdownSim, 0.0))
    {
      gzwarn << "control for joint [" << control.jointName
             << "] rotorVelocitySlowdownSim is zero,"
             << " assume no slowdown.\n";
      control.rotorVelocitySlowdownSim = 1.0;
    }

    getSdfParam<double>(controlSDF, "frequencyCutoff",
        control.frequencyCutoff, control.frequencyCutoff);
    getSdfParam<double>(controlSDF, "samplingRate",
        control.samplingRate, control.samplingRate);

    // use gazebo::math::Filter
    control.filter.Fc(control.frequencyCutoff, control.samplingRate);

    // initialize filter to zero value
    control.filter.Set(0.0);

    // note to use this filter, do
    // stateFiltered = filter.Process(stateRaw);

    // Overload the PID parameters if they are available.
    double param;
    // carry over from ArduCopter plugin
    getSdfParam<double>(controlSDF, "vel_p_gain", param,
      control.pid.GetPGain());
    control.pid.SetPGain(param);

    getSdfParam<double>(controlSDF, "vel_i_gain", param,
      control.pid.GetIGain());
    control.pid.SetIGain(param);

    getSdfParam<double>(controlSDF, "vel_d_gain", param,
       control.pid.GetDGain());
    control.pid.SetDGain(param);

    getSdfParam<double>(controlSDF, "vel_i_max", param,
      control.pid.GetIMax());
    control.pid.SetIMax(param);

    getSdfParam<double>(controlSDF, "vel_i_min", param,
      control.pid.GetIMin());
    control.pid.SetIMin(param);

    getSdfParam<double>(controlSDF, "vel_cmd_max", param,
        control.pid.GetCmdMax());
    control.pid.SetCmdMax(param);

    getSdfParam<double>(controlSDF, "vel_cmd_min", param,
        control.pid.GetCmdMin());
    control.pid.SetCmdMin(param);

    // new params, overwrite old params if exist
    getSdfParam<double>(controlSDF, "p_gain", param,
      control.pid.GetPGain());
    control.pid.SetPGain(param);

    getSdfParam<double>(controlSDF, "i_gain", param,
      control.pid.GetIGain());
    control.pid.SetIGain(param);

    getSdfParam<double>(controlSDF, "d_gain", param,
       control.pid.GetDGain());
    control.pid.SetDGain(param);

    getSdfParam<double>(controlSDF, "i_max", param,
      control.pid.GetIMax());
    control.pid.SetIMax(param);

    getSdfParam<double>(controlSDF, "i_min", param,
      control.pid.GetIMin());
    control.pid.SetIMin(param);

    getSdfParam<double>(controlSDF, "cmd_max", param,
        control.pid.GetCmdMax());
    control.pid.SetCmdMax(param);

    getSdfParam<double>(controlSDF, "cmd_min", param,
        control.pid.GetCmdMin());
    control.pid.SetCmdMin(param);

    // set pid initial command
    control.pid.SetCmd(0.0);

    this->dataPtr->controls.push_back(control);
    controlSDF = controlSDF->GetNextElement("control");
  }

  // Get sensors
  std::string imuName;
  getSdfParam<std::string>(_sdf, "imuName", imuName, "imu_sensor");
  std::string imuScopedName = this->dataPtr->model->GetWorld()->GetName()
      + "::" + this->dataPtr->model->GetScopedName()
      + "::" + imuName;
  this->dataPtr->imuSensor = std::dynamic_pointer_cast<sensors::ImuSensor>
    (sensors::SensorManager::Instance()->GetSensor(imuScopedName));

  if (!this->dataPtr->imuSensor)
  {
    gzerr << "imu_sensor [" << imuScopedName
          << "] not found, abort ArduPilot plugin.\n" << "\n";
    return;
  }

  // Controller time control.
  this->dataPtr->lastControllerUpdateTime = 0;

  // Missed update count before we declare arduPilotOnline status false
  getSdfParam<int>(_sdf, "connectionTimeoutMaxCount",
    this->dataPtr->connectionTimeoutMaxCount, 10);

  // Listen to the update event. This event is broadcast every simulation
  // iteration.
  this->dataPtr->updateConnection = event::Events::ConnectWorldUpdateBegin(
      std::bind(&ArduPilotPlugin::OnUpdate, this));

  gzlog << "ArduPilot ready to fly. The force will be with you" << std::endl;
}

/////////////////////////////////////////////////
void ArduPilotPlugin::OnUpdate()
{
  std::lock_guard<std::mutex> lock(this->dataPtr->mutex);

  gazebo::common::Time curTime = this->dataPtr->model->GetWorld()->GetSimTime();

  // Update the control surfaces and publish the new state.
  if (curTime > this->dataPtr->lastControllerUpdateTime)
  {
    this->ReceiveMotorCommand();
    if (this->dataPtr->arduPilotOnline)
    {
      this->ApplyMotorForces((curTime -
        this->dataPtr->lastControllerUpdateTime).Double());
      this->SendState();
    }
  }

  this->dataPtr->lastControllerUpdateTime = curTime;
}

/////////////////////////////////////////////////
void ArduPilotPlugin::ResetPIDs()
{
  // Reset velocity PID for controls
  for (size_t i = 0; i < this->dataPtr->controls.size(); ++i)
  {
    this->dataPtr->controls[i].cmd = 0;
    // this->dataPtr->controls[i].pid.Reset();
  }
}

/////////////////////////////////////////////////
void ArduPilotPlugin::ApplyMotorForces(const double _dt)
{
  // update velocity PID for controls and apply force to joint
  for (size_t i = 0; i < this->dataPtr->controls.size(); ++i)
  {
    if (this->dataPtr->controls[i].type == "VELOCITY")
    {
      double velTarget = this->dataPtr->controls[i].cmd /
        this->dataPtr->controls[i].rotorVelocitySlowdownSim;
      double vel = this->dataPtr->controls[i].joint->GetVelocity(0);
      double error = vel - velTarget;
      double force = this->dataPtr->controls[i].pid.Update(error, _dt);
      this->dataPtr->controls[i].joint->SetForce(0, force);
    }
    else if (this->dataPtr->controls[i].type == "POSITION")
    {
      double posTarget = this->dataPtr->controls[i].cmd;
      double pos = this->dataPtr->controls[i].joint->GetAngle(0).Radian();
      double error = pos - posTarget;
      double force = this->dataPtr->controls[i].pid.Update(error, _dt);
      this->dataPtr->controls[i].joint->SetForce(0, force);
    }
    else if (this->dataPtr->controls[i].type == "EFFORT")
    {
      double force = this->dataPtr->controls[i].cmd;
      this->dataPtr->controls[i].joint->SetForce(0, force);
    }
    else
    {
      // do nothing
    }
  }
}

/////////////////////////////////////////////////
void ArduPilotPlugin::ReceiveMotorCommand()
{
  // Added detection for whether ArduPilot is online or not.
  // If ArduPilot is detected (receive of fdm packet from someone),
  // then socket receive wait time is increased from 1ms to 1 sec
  // to accomodate network jitter.
  // If ArduPilot is not detected, receive call blocks for 1ms
  // on each call.
  // Once ArduPilot presence is detected, it takes this many
  // missed receives before declaring the FCS offline.

  ServoPacket pkt;
  int waitMs = 1;
  if (this->dataPtr->arduPilotOnline)
  {
    // increase timeout for receive once we detect a packet from
    // ArduPilot FCS.
    waitMs = 1000;
  }
  else
  {
    // Otherwise skip quickly and do not set control force.
    waitMs = 1;
  }
  ssize_t recvSize = this->dataPtr->Recv(&pkt, sizeof(ServoPacket), waitMs);
  ssize_t expectedPktSize = sizeof(float)*this->dataPtr->controls.size();
  ssize_t recvChannels = recvSize/sizeof(float);
  if (recvSize == -1)
  {
    // didn't receive a packet
    // gzdbg << "no packet\n";
    gazebo::common::Time::NSleep(100);
    if (this->dataPtr->arduPilotOnline)
    {
      gzwarn << "Broken ArduPilot connection, count ["
             << this->dataPtr->connectionTimeoutCount
             << "/" << this->dataPtr->connectionTimeoutMaxCount
             << "]\n";
      if (++this->dataPtr->connectionTimeoutCount >
        this->dataPtr->connectionTimeoutMaxCount)
      {
        this->dataPtr->connectionTimeoutCount = 0;
        this->dataPtr->arduPilotOnline = false;
        gzwarn << "Broken ArduPilot connection, resetting motor control.\n";
        this->ResetPIDs();
      }
    }
  }
  else
  {
    if (recvSize < expectedPktSize)
    {
      gzerr << "got less than model needs. Got: " << recvSize
            << "commands, expected size: " << expectedPktSize << "\n";
    }

    // for(unsigned int i = 0; i < recvChannels; ++i)
    // {
    //   gzdbg << "servo_command [" << i << "]: " << pkt.motorSpeed[i] << "\n";
    // }

    if (!this->dataPtr->arduPilotOnline)
    {
      gzdbg << "ArduPilot controller online detected.\n";
      // made connection, set some flags
      this->dataPtr->connectionTimeoutCount = 0;
      this->dataPtr->arduPilotOnline = true;
    }

    // compute command based on requested motorSpeed
    for (unsigned i = 0; i < this->dataPtr->controls.size(); ++i)
    {
      if (i < MAX_MOTORS)
      {
        if (this->dataPtr->controls[i].channel < recvChannels)
        {
          this->dataPtr->controls[i].cmd =
            this->dataPtr->controls[i].multiplier *
            (this->dataPtr->controls[i].offset +
            pkt.motorSpeed[this->dataPtr->controls[i].channel]);
          // gzdbg << "apply input chan[" << this->dataPtr->controls[i].channel
          //       << "] to control chan[" << i
          //       << "] with joint name ["
          //       << this->dataPtr->controls[i].jointName
          //       << "] servo_command [" << this->dataPtr->controls[i].cmd
          //       << "].\n";
        }
        else
        {
          gzerr << "control[" << i << "] channel ["
                << this->dataPtr->controls[i].channel
                << "] is greater than incoming commands size["
                << recvChannels
                << "], control not applied.\n";
        }
      }
      else
      {
        gzerr << "too many motors, skipping [" << i
              << " > " << MAX_MOTORS << "].\n";
      }
    }
  }
}

/////////////////////////////////////////////////
void ArduPilotPlugin::SendState() const
{
  // send_fdm
  fdmPacket pkt;

  pkt.timestamp = this->dataPtr->model->GetWorld()->GetSimTime().Double();

  // asssumed that the imu orientation is:
  //   x forward
  //   y right
  //   z down

  // get linear acceleration in body frame
  ignition::math::Vector3d linearAccel =
    this->dataPtr->imuSensor->LinearAcceleration();

  // copy to pkt
  pkt.imuLinearAccelerationXYZ[0] = linearAccel.X();
  pkt.imuLinearAccelerationXYZ[1] = linearAccel.Y();
  pkt.imuLinearAccelerationXYZ[2] = linearAccel.Z();
  // gzerr << "lin accel [" << linearAccel << "]\n";

  // get angular velocity in body frame
  ignition::math::Vector3d angularVel =
    this->dataPtr->imuSensor->AngularVelocity();

  // copy to pkt
  pkt.imuAngularVelocityRPY[0] = angularVel.X();
  pkt.imuAngularVelocityRPY[1] = angularVel.Y();
  pkt.imuAngularVelocityRPY[2] = angularVel.Z();

  // get inertial pose and velocity
  // position of the uav in world frame
  // this position is used to calcualte bearing and distance
  // from starting location, then use that to update gps position.
  // The algorithm looks something like below (from ardupilot helper
  // libraries):
  //   bearing = to_degrees(atan2(position.y, position.x));
  //   distance = math.sqrt(self.position.x**2 + self.position.y**2)
  //   (self.latitude, self.longitude) = util.gps_newpos(
  //    self.home_latitude, self.home_longitude, bearing, distance)
  // where xyz is in the NED directions.
  // Gazebo world xyz is assumed to be N, -E, -D, so flip some stuff
  // around.
  // orientation of the uav in world NED frame -
  // assuming the world NED frame has xyz mapped to NED,
  // imuLink is NED - z down

  // gazeboToNED brings us from gazebo model: x-forward, y-right, z-down
  // to the aerospace convention: x-forward, y-left, z-up
  ignition::math::Pose3d gazeboToNED(0, 0, 0, IGN_PI, 0, 0);

  // model world pose brings us to model, x-forward, y-left, z-up
  // adding gazeboToNED gets us to the x-forward, y-right, z-down
  ignition::math::Pose3d worldToModel = gazeboToNED +
    this->dataPtr->model->GetWorldPose().Ign();

  // get transform from world NED to Model frame
  ignition::math::Pose3d NEDToModel = worldToModel - gazeboToNED;

  // gzerr << "ned to model [" << NEDToModel << "]\n";

  // N
  pkt.positionXYZ[0] = NEDToModel.Pos().X();

  // E
  pkt.positionXYZ[1] = NEDToModel.Pos().Y();

  // D
  pkt.positionXYZ[2] = NEDToModel.Pos().Z();

  // imuOrientationQuat is the rotation from world NED frame
  // to the uav frame.
  pkt.imuOrientationQuat[0] = NEDToModel.Rot().W();
  pkt.imuOrientationQuat[1] = NEDToModel.Rot().X();
  pkt.imuOrientationQuat[2] = NEDToModel.Rot().Y();
  pkt.imuOrientationQuat[3] = NEDToModel.Rot().Z();

  // gzdbg << "imu [" << worldToModel.rot.GetAsEuler() << "]\n";
  // gzdbg << "ned [" << gazeboToNED.rot.GetAsEuler() << "]\n";
  // gzdbg << "rot [" << NEDToModel.rot.GetAsEuler() << "]\n";

  // Get NED velocity in body frame *
  // or...
  // Get model velocity in NED frame
  ignition::math::Vector3d velGazeboWorldFrame =
    this->dataPtr->model->GetLink()->GetWorldLinearVel().Ign();
  ignition::math::Vector3d velNEDFrame =
    gazeboToNED.Rot().RotateVectorReverse(velGazeboWorldFrame);
  pkt.velocityXYZ[0] = velNEDFrame.X();
  pkt.velocityXYZ[1] = velNEDFrame.Y();
  pkt.velocityXYZ[2] = velNEDFrame.Z();

  struct sockaddr_in sockaddr;
  this->dataPtr->MakeSockAddr("127.0.0.1", 9003, sockaddr);

  ::sendto(this->dataPtr->handle, &pkt, sizeof(pkt), 0,
    (struct sockaddr *)&sockaddr, sizeof(sockaddr));
}