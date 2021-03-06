/*
 *  Mikrokopter Flight control Serial Interface
 *  Copyright (C) 2010, CYPHY lab
 *  Inkyu Sa <i.sa@qut.edu.au>
 *
 *  http://wiki.qut.edu.au/display/cyphy
 *
 *
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "flightcontrol.h"

ros::Timer timer;
ros::WallTimer walltimer;

unsigned char th_cnt=0;


unsigned char g_buf[300];
unsigned char g_buf_debug[300];
//unsigned char g_txd_buffer[TXD_BUFFER_LEN];
unsigned char g_rxd_buffer[RXD_BUFFER_LEN];
unsigned char g_ReceivedBytes;
unsigned char *g_pRxData;
unsigned char g_RxDataLen;
long g_nCounter=0;
long g_nCounter1=0;
double g_pitch_temp;
double g_roll_temp;
double g_pitch_new;
double g_pitch_cmd;
double g_roll_cmd;
double g_roll_new;
double g_yaw_new;
double g_yaw_temp;
double dblDistance;
double g_height;
int g_throttle_cmd;
double g_goal_height;

long counter=0;
long counter1=0;

using namespace miko;

struct str_Data3D g_Data3D,g_Data3D_Temp;
DebugOut_t g_DebugData,g_DebugData_Temp;
Position_t g_PositionFromAcc,g_PostPosition,g_EstimatePosition,g_CurPosition,g_GoalPosition,g_GoalVel,g_errorPos;
Control_t g_StickControl,g_PostStickControl;
Velocity_t g_W_Vel,g_B_Vel,g_B_ACC,g_B_esti_Vel,g_B_predic_esti_Vel,g_B_goal_Vel;

// **********************************************
/// MAIN IS HERE!!
int main(int argc, char **argv)
{
//	int interval=0;
//	int nLength;
	ros::init(argc, argv, "flightcontrol");
  FlightControl flightcontrol;

  ros::AsyncSpinner spinner(2); // Use 2 threads
  spinner.start();
  ros::waitForShutdown();
	return 0;
}

namespace miko
{
FlightControl::FlightControl()
{
	ROS_INFO ("Creating FlightControl Interface");

	ros::NodeHandle nh;

  //the debug data rate sent to request how fast debug data will be sent over serial
  ros::param::param<int>("~debug_data_rate", data_rate_, 50);
  //This number is multiplied by 10 and then used as milliseconds for a rate at which to send debug data

//  std::cout << "(1/10th) Data rate is now: " << data_rate_ << std::endl;

  pub = nh.advertise<mikro_serial::mikoImu>("mikoImu", 1);

  /// Only allow 1 message to be waiting on the ROS queue - I don't want to process control commands late.  If we skip
  /// one, that is better than applying them late
  mikoCmdSubscriber = nh.subscribe ("mikoCmd", 1, &FlightControl::mikoCmdCallback, this);


	// **** parameter Setting
//	freq_ = 50.0;
  port_ = "/dev/ttyUSB0";
	speed_ = 57600;
	//speed_ = 115200;
//	if (freq_ <= 0.0) ROS_FATAL ("Invalid frequency param");
//	ros::Duration d (1.0 / freq_);

	// Reference input initialization
//	ExternControl.Digital[0] =0;
//	ExternControl.Digital[1] =0;
//	ExternControl.RemoteButtons =0;
//	ExternControl.Pitch =0;
//	ExternControl.Roll =0;
//	ExternControl.Yaw =0;
//	ExternControl.Throttle =0;
//	ExternControl.Height =0;
//	ExternControl.free =0;
//	ExternControl.Frame =0;
//	ExternControl.Config =1;
	Throttle_Direction=true;

        //Initialize the entries of the MK Debug Data Digital Array to 0
	g_DebugData.Digital[0]=0;
	g_DebugData.Digital[1]=0;

        //Initialize all the entries of the MK Debug Data Analog Array to 0
        for(int i=0;i<32;i++) g_DebugData.Analog[i]=0;

	g_ReceivedBytes=0;
	g_pRxData=0;
	g_RxDataLen=0;

	g_height=0;
	g_throttle_cmd=0;
	g_goal_height=1.0; // 1m


  // **** set up interfaces
  serialInterface_ = new SerialInterface (port_, speed_);
	serialInterface_->serialport_bytes_rx_ = 0;
	serialInterface_->serialport_bytes_tx_ = 0;

  double rate;
  rate = (double)data_rate_/100.0;
  std::cout << "the rate is (in seconds): " << rate << std::endl;
  timer= nh.createTimer(ros::Duration(rate), &FlightControl::spin,this);
  ROS_INFO ("Mikrokopter Serial setup completed Successfully!");

//  std::string file_name = "recieved_commands";
//  file_name.append(".txt");
//  log_file_.open(file_name.c_str(), std::ios::out | std::ios::trunc);
}
  

FlightControl::~FlightControl()
{
//	fclose(fd);
//	fclose(fd_h);
  delete serialInterface_;
  //std::cout << "Destroying FlightControl Interface" << std::endl;
}


void FlightControl::spin(const ros::TimerEvent & e)
{
	int nLength=0;
  uint8_t interval= (uint8_t)data_rate_;


	#ifdef CHECK_DURA
	struct timeval start, end , total_start,total_end,acc_start,acc_end;
	gettimeofday(&start, NULL);
	gettimeofday(&total_start, NULL);
	gettimeofday(&acc_start, NULL);
	#endif

	SendOutData('d', FC_ADDRESS, 1,&interval,sizeof(interval)); // Request debug data from FC

	#ifdef CHECK_DURA
	gettimeofday(&end, NULL);
	double dur = ((end.tv_sec   * 1000000 + end.tv_usec  ) - 
		(start.tv_sec * 1000000 + start.tv_usec)) / 1000.0;

	ROS_INFO("Request debug data dur:\t %.3f ms", dur);
	#endif


	mikoImu.header.stamp = ros::Time::now();


	#ifdef CHECK_DURA
	gettimeofday(&start, NULL);
	#endif

  ros::WallDuration(0.1).sleep(); // delay

	#ifdef CHECK_DURA
	gettimeofday(&end, NULL);
		dur = ((end.tv_sec   * 1000000 + end.tv_usec  ) - 
		(start.tv_sec * 1000000 + start.tv_usec)) / 1000.0;

	ROS_INFO("Check delay dur:\t %.3f ms", dur);
	#endif



	#ifdef CHECK_DURA
	gettimeofday(&start, NULL);
	#endif

	nLength=serialInterface_->getdata(g_rxd_buffer, 255);


	#ifdef CHECK_DURA
	gettimeofday(&end, NULL);
	dur = ((end.tv_sec   * 1000000 + end.tv_usec  ) - 
		(start.tv_sec * 1000000 + start.tv_usec)) / 1000.0;
	ROS_INFO("getdata dur:\t %.3f ms", dur);
	#endif


	if(nLength>0)
	{

		#ifdef CHECK_DURA
		gettimeofday(&start, NULL);
		#endif


		serialInterface_->Decode64();


		#ifdef CHECK_DURA
		gettimeofday(&end, NULL);
		dur = ((end.tv_sec   * 1000000 + end.tv_usec  ) - 
			(start.tv_sec * 1000000 + start.tv_usec)) / 1000.0;
		ROS_INFO("Decode64 dur:\t %.3f ms", dur);
		#endif


		#ifdef CHECK_DURA
		gettimeofday(&start, NULL);
		#endif

		serialInterface_->ParsingData();


		#ifdef CHECK_DURA
		gettimeofday(&end, NULL);
		dur = ((end.tv_sec   * 1000000 + end.tv_usec  ) - 
			(start.tv_sec * 1000000 + start.tv_usec)) / 1000.0;
		ROS_INFO("ParsingData dur:\t %.3f ms", dur);
		#endif

		//=================================================================
		// Coordinate and unit conversion from Mikrokopter to Cyphy model.
		//=================================================================

    // Put negative(minus) all angles to covert all axes into a right hand coordinate.
    MyAttitude.AnglePitch = -POINT_ONE_G_RADS_TO_RADS(g_DebugData.Analog[ANGLE_PITCH]); //This will display pitch in radians
    MyAttitude.AngleRoll = -POINT_ONE_G_RADS_TO_RADS(g_DebugData.Analog[ANGLE_ROLL]); //This will display roll in radians
    MyAttitude.AngleYaw = CONVERT_YAW(g_DebugData.Analog[ANGLE_YAW]); //This will display yaw in radians

    // Normalize Acceleration data as 1G.  //### Not sure if this is correct or not.  Our firmware may be different.
    MyAttitude.ACCX = -(g_DebugData.Analog[ACCEL_X]/ACCEL_X_FAKTOR); // m/s^2
    MyAttitude.ACCY = (g_DebugData.Analog[ACCEL_Y]/ACCEL_Y_FAKTOR); // m/s^2
    MyAttitude.ACCZ = -(g_DebugData.Analog[ACCEL_Z]/ACCEL_Z_FAKTOR); // m/s^2

                //Fill the fields of the message with the appropriate output
		mikoImu.anglePitch = MyAttitude.AnglePitch; 
		mikoImu.angleRoll = MyAttitude.AngleRoll;
		mikoImu.angleYaw = MyAttitude.AngleYaw;
    mikoImu.gyroPitch = 0;//-g_DebugData.Analog[GYRO_PITCH]/150.0;
    mikoImu.gyroRoll = 0;//-g_DebugData.Analog[GYRO_ROLL]/150.0;
    mikoImu.gyroYaw = g_DebugData.Analog[GYRO_YAW]/150.0;
    mikoImu.accelX = -(g_DebugData.Analog[ACCEL_X]/ACCEL_X_FAKTOR); // m/s^2
    mikoImu.accelY = (g_DebugData.Analog[ACCEL_Y]/ACCEL_Y_FAKTOR); // m/s^2
    mikoImu.accelZ = -(g_DebugData.Analog[ACCEL_Z]/ACCEL_Z_FAKTOR); // m/s^2
    mikoImu.motor1 = g_DebugData.Analog[MOTOR1];
    mikoImu.motor2 = g_DebugData.Analog[MOTOR2];
    mikoImu.motor3 = g_DebugData.Analog[MOTOR3];
    mikoImu.motor4 = g_DebugData.Analog[MOTOR4];
    mikoImu.motor5 = 0;//g_DebugData.Analog[MOTOR5];
    mikoImu.motor6 = 0;//g_DebugData.Analog[MOTOR6];
		mikoImu.linear_acceleration.x = MyAttitude.ACCX;
		mikoImu.linear_acceleration.y = MyAttitude.ACCY;
		mikoImu.linear_acceleration.z = MyAttitude.ACCZ;
    mikoImu.baromHeight = g_DebugData.Analog[BAROMHEIGHT];

    if(g_DebugData.Analog[BATT]<BATT_MAX)
        mikoImu.batt = g_DebugData.Analog[BATT];
    else
        mikoImu.batt = 255;

		pub.publish(mikoImu);
		counter++;
	
		}
}

void FlightControl::SendOutData(uint8_t cmd, uint8_t addr, uint8_t numofbuffers, ...) // uint8_t *pdata, uint8_t len, ...
{ 
	va_list ap;
	uint16_t pt = 0;
	uint8_t a,b,c;
	uint8_t ptr = 0;

	uint8_t *pdata = 0;
	int len = 0;

  unsigned char g_txd_buffer[TXD_BUFFER_LEN];

	g_txd_buffer[pt++] = '#';			// Start character
	g_txd_buffer[pt++] = 'a' + addr;	// Address (a=0; b=1,...)
	g_txd_buffer[pt++] = cmd;			// Command

	va_start(ap, numofbuffers);
	if(numofbuffers)
	{
		pdata = va_arg(ap, uint8_t*);
		len = va_arg(ap, int);
		ptr = 0;
		numofbuffers--;
	}

	while(len)
	{
		if(len)
		{
			a = pdata[ptr++];
			len--;
			if((!len) && numofbuffers)
			{
				pdata = va_arg(ap, uint8_t*);
				len = va_arg(ap, int);
				ptr = 0;
				numofbuffers--;
			}
		}
		else a = 0;
		if(len)
		{
			b = pdata[ptr++];
			len--;
			if((!len) && numofbuffers)
			{
				pdata = va_arg(ap, uint8_t*);
				len = va_arg(ap, int);
				ptr = 0;
				numofbuffers--;
			}
		}
		else b = 0;
		if(len)
		{
			c = pdata[ptr++];
			len--;
			if((!len) && numofbuffers)
			{
				pdata = va_arg(ap, uint8_t*);
				len = va_arg(ap, int);
				ptr = 0;
				numofbuffers--;
			}
		}
		else c = 0;
		g_txd_buffer[pt++] = '=' + (a >> 2);
		g_txd_buffer[pt++] = '=' + (((a & 0x03) << 4) | ((b & 0xf0) >> 4));
		g_txd_buffer[pt++] = '=' + (((b & 0x0f) << 2) | ((c & 0xc0) >> 6));
		g_txd_buffer[pt++] = '=' + ( c & 0x3f);
	} //while
	va_end(ap);
  AddCRC(g_txd_buffer,pt); // add checksum after data block and initates the transmission
}

void FlightControl::AddCRC(unsigned char *bffer, uint16_t datalen)
{
	uint16_t tmpCRC = 0, i;
	for(i = 0; i < datalen; i++)
	{
    tmpCRC += bffer[i];
	}
	tmpCRC %= 4096;
  bffer[datalen++] = '=' + tmpCRC / 64;
  bffer[datalen++] = '=' + tmpCRC % 64;
  bffer[datalen++] = '\r';
  serialInterface_->output(bffer,datalen);
}

void FlightControl::mikoCmdCallback(const mikro_serial::mikoCmd& msg)
{

  //ros::Time timestamp = ros::Time::now();
  //log_file_.precision(20);
  //log_file_ << timestamp.toSec() << std::endl;
  //ROS_INFO("message recieved now: %15.4f",timestamp.toSec());

   ExternControl_t	ExternControl1;

   ExternControl1.Digital[0] =0;
   ExternControl1.Digital[1] =0;
   ExternControl1.RemoteButtons =0;
   ExternControl1.Pitch = msg.pitch;
   ExternControl1.Roll = msg.roll;
   ExternControl1.Yaw = msg.yaw;
   ExternControl1.Throttle = msg.throttle;
   ExternControl1.Height =0;
   ExternControl1.free =0;
   ExternControl1.Frame =0;
   ExternControl1.Config =1;

   //The only saturation that should take place is to keep it within the limits of the possible commands.
   //Other saturation should be done with the controller.
   if(ExternControl1.Pitch >=127) ExternControl1.Pitch=127;
   if(ExternControl1.Pitch <=-127) ExternControl1.Pitch=-127;

   if(ExternControl1.Roll >=127) ExternControl1.Roll=127;
   if(ExternControl1.Roll <=-127) ExternControl1.Roll=-127;
	 
   if(ExternControl1.Yaw >=127) ExternControl1.Yaw=127;
   if(ExternControl1.Yaw <=-127) ExternControl1.Yaw=-127;

   if(ExternControl1.Throttle <= 0) ExternControl1.Throttle =0;
   if(ExternControl1.Throttle >=MAX_THROTTLE) ExternControl1.Throttle = MAX_THROTTLE;	 
   SendOutData('b', FC_ADDRESS,1,(uint8_t*)&ExternControl1, sizeof(ExternControl1));
}

} // namespace miko
