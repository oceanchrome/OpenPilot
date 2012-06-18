/**
 ******************************************************************************
 * @addtogroup OpenPilotModules OpenPilot Modules
 * @{
 * @addtogroup Attitude Copter Control Attitude Estimation
 * @brief Acquires sensor data and computes attitude estimate
 * Specifically updates the the @ref AttitudeActual "AttitudeActual" and @ref AttitudeRaw "AttitudeRaw" settings objects
 * @{
 *
 * @file       attitude.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @brief      Module to handle all comms to the AHRS on a periodic basis.
 *
 * @see        The GNU Public License (GPL) Version 3
 *
 ******************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/**
 * Input objects: None, takes sensor data via pios
 * Output objects: @ref AttitudeRaw @ref AttitudeActual
 *
 * This module computes an attitude estimate from the sensor data
 *
 * The module executes in its own thread.
 *
 * UAVObjects are automatically generated by the UAVObjectGenerator from
 * the object definition XML file.
 *
 * Modules have no API, all communication to other modules is done through UAVObjects.
 * However modules may use the API exposed by shared libraries.
 * See the OpenPilot wiki for more details.
 * http://www.openpilot.org/OpenPilot_Application_Architecture
 *
 */

#include "pios.h"
#include "attitude.h"
#include "magnetometer.h"
#include "accels.h"
#include "gyros.h"
#include "gyrosbias.h"
#include "attitudeactual.h"
#include "attitudesettings.h"
#include "positionactual.h"
#include "velocityactual.h"
#include "gpsposition.h"
#include "baroaltitude.h"
#include "flightstatus.h"
#include "homelocation.h"
#include "CoordinateConversions.h"

// Private constants
#define STACK_SIZE_BYTES 5540
#define TASK_PRIORITY (tskIDLE_PRIORITY+3)
#define FAILSAFE_TIMEOUT_MS 10

#define F_PI 3.14159265358979323846f
#define PI_MOD(x) (fmodf(x + F_PI, F_PI * 2) - F_PI)
// Private types

// Private variables
static xTaskHandle attitudeTaskHandle;

static xQueueHandle gyroQueue;
static xQueueHandle accelQueue;
static xQueueHandle magQueue;
static xQueueHandle baroQueue;
static xQueueHandle gpsQueue;
const uint32_t SENSOR_QUEUE_SIZE = 10;

// Private functions
static void AttitudeTask(void *parameters);

static int32_t updateAttitudeComplimentary(bool first_run);
static int32_t updateAttitudeINSGPS(bool first_run);
static void settingsUpdatedCb(UAVObjEvent * objEv);

static float accelKi = 0;
static float accelKp = 0;
static float yawBiasRate = 0;
static float gyroGain = 0.42;
static int16_t accelbias[3];
static float R[3][3];
static int8_t rotate = 0;
static bool zero_during_arming = false;


/**
 * API for sensor fusion algorithms:
 * Configure(xQueueHandle gyro, xQueueHandle accel, xQueueHandle mag, xQueueHandle baro)
 *   Stores all the queues the algorithm will pull data from
 * FinalizeSensors() -- before saving the sensors modifies them based on internal state (gyro bias)
 * Update() -- queries queues and updates the attitude estiamte
 */


/**
 * Initialise the module.  Called before the start function
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t AttitudeInitialize(void)
{
	AttitudeActualInitialize();
	AttitudeSettingsInitialize();
	PositionActualInitialize();
	VelocityActualInitialize();
	
	// Initialize this here while we aren't setting the homelocation in GPS
	HomeLocationInitialize();

	// Initialize quaternion
	AttitudeActualData attitude;
	AttitudeActualGet(&attitude);
	attitude.q1 = 1;
	attitude.q2 = 0;
	attitude.q3 = 0;
	attitude.q4 = 0;
	AttitudeActualSet(&attitude);
	
	// Cannot trust the values to init right above if BL runs
	GyrosBiasData gyrosBias;
	GyrosBiasGet(&gyrosBias);
	gyrosBias.x = 0;
	gyrosBias.y = 0;
	gyrosBias.z = 0;
	GyrosBiasSet(&gyrosBias);
	
	for(uint8_t i = 0; i < 3; i++)
		for(uint8_t j = 0; j < 3; j++)
			R[i][j] = 0;
	
	AttitudeSettingsConnectCallback(&settingsUpdatedCb);
	
	return 0;
}

/**
 * Start the task.  Expects all objects to be initialized by this point.
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t AttitudeStart(void)
{
	// Create the queues for the sensors
	gyroQueue = xQueueCreate(1, sizeof(UAVObjEvent));
	accelQueue = xQueueCreate(1, sizeof(UAVObjEvent));
	magQueue = xQueueCreate(1, sizeof(UAVObjEvent));
	baroQueue = xQueueCreate(1, sizeof(UAVObjEvent));
	gpsQueue = xQueueCreate(1, sizeof(UAVObjEvent));
			
	// Start main task
	xTaskCreate(AttitudeTask, (signed char *)"Attitude", STACK_SIZE_BYTES/4, NULL, TASK_PRIORITY, &attitudeTaskHandle);
	TaskMonitorAdd(TASKINFO_RUNNING_ATTITUDE, attitudeTaskHandle);
	PIOS_WDG_RegisterFlag(PIOS_WDG_ATTITUDE);
	
	GyrosConnectQueue(gyroQueue);
	AccelsConnectQueue(accelQueue);
	MagnetometerConnectQueue(magQueue);
	BaroAltitudeConnectQueue(baroQueue);
	GPSPositionConnectQueue(gpsQueue);
	
	return 0;
}

MODULE_INITCALL(AttitudeInitialize, AttitudeStart)

/**
 * Module thread, should not return.
 */
static void AttitudeTask(void *parameters)
{
	AlarmsClear(SYSTEMALARMS_ALARM_ATTITUDE);

	// Force settings update to make sure rotation loaded
	settingsUpdatedCb(AttitudeSettingsHandle());
	
	bool first_run = true;
	
	// Wait for all the sensors be to read
	vTaskDelay(100);
	
	// Main task loop
	while (1) {
	
		// This  function blocks on data queue
		if(1) 
			updateAttitudeComplimentary(first_run);
		else
			updateAttitudeINSGPS(first_run);
			
		if (first_run)
			first_run = false;
		
		PIOS_WDG_UpdateFlag(PIOS_WDG_ATTITUDE);
	}
}

float accel_mag;
float qmag;
float attitudeDt;
float mag_err[3];
float magKi = 0.000001f;
float magKp = 0.0001f;

static int32_t updateAttitudeComplimentary(bool first_run)
{
	UAVObjEvent ev;
	GyrosData gyrosData;
	AccelsData accelsData;
	static int32_t timeval;
	float dT;
	static uint8_t init = 0;

	// Wait until the AttitudeRaw object is updated, if a timeout then go to failsafe
	if ( xQueueReceive(gyroQueue, &ev, FAILSAFE_TIMEOUT_MS / portTICK_RATE_MS) != pdTRUE )
	{
		AlarmsSet(SYSTEMALARMS_ALARM_ATTITUDE,SYSTEMALARMS_ALARM_WARNING);
		return -1;
	}
	if ( xQueueReceive(accelQueue, &ev, 0) != pdTRUE )
	{
		// When one of these is updated so should the other
		AlarmsSet(SYSTEMALARMS_ALARM_ATTITUDE,SYSTEMALARMS_ALARM_WARNING);
		return -1;
	}
	
	// During initialization and 
	FlightStatusData flightStatus;
	FlightStatusGet(&flightStatus);
	if(first_run)
		init = 0;

	if((init == 0 && xTaskGetTickCount() < 7000) && (xTaskGetTickCount() > 1000)) {
		// For first 7 seconds use accels to get gyro bias
		accelKp = 1;
		accelKi = 0.9;
		yawBiasRate = 0.23;
	} else if (zero_during_arming && (flightStatus.Armed == FLIGHTSTATUS_ARMED_ARMING)) {
		accelKp = 1;
		accelKi = 0.9;
		yawBiasRate = 0.23;
		init = 0;
	} else if (init == 0) {
		// Reload settings (all the rates)
		AttitudeSettingsAccelKiGet(&accelKi);
		AttitudeSettingsAccelKpGet(&accelKp);
		AttitudeSettingsYawBiasRateGet(&yawBiasRate);
		init = 1;
	}	
	
	GyrosGet(&gyrosData);
	AccelsGet(&accelsData);

	// Compute the dT using the cpu clock
	dT = PIOS_DELAY_DiffuS(timeval) / 1000000.0f;
	timeval = PIOS_DELAY_GetRaw();
	
	float q[4];
	
	AttitudeActualData attitudeActual;
	AttitudeActualGet(&attitudeActual);

	float grot[3];
	float accel_err[3];
	
	// Get the current attitude estimate
	quat_copy(&attitudeActual.q1, q);
	
	// Rotate gravity to body frame and cross with accels
	grot[0] = -(2 * (q[1] * q[3] - q[0] * q[2]));
	grot[1] = -(2 * (q[2] * q[3] + q[0] * q[1]));
	grot[2] = -(q[0] * q[0] - q[1]*q[1] - q[2]*q[2] + q[3]*q[3]);
	CrossProduct((const float *) &accelsData.x, (const float *) grot, accel_err);

	// Account for accel magnitude
	accel_mag = accelsData.x*accelsData.x + accelsData.y*accelsData.y + accelsData.z*accelsData.z;
	accel_mag = sqrtf(accel_mag);
	accel_err[0] /= accel_mag;
	accel_err[1] /= accel_mag;
	accel_err[2] /= accel_mag;	
	
	if ( xQueueReceive(magQueue, &ev, 0) != pdTRUE )
	{
		// Rotate gravity to body frame and cross with accels
		float brot[3];
		float Rbe[3][3];
		MagnetometerData mag;
		HomeLocationData home;

		Quaternion2R(q, Rbe);
		MagnetometerGet(&mag);
		HomeLocationGet(&home);
		rot_mult(Rbe, home.Be, brot, FALSE);
		
		float mag_len = sqrtf(mag.x * mag.x + mag.y * mag.y + mag.z * mag.z);
		mag.x /= mag_len;
		mag.y /= mag_len;
		mag.z /= mag_len;
		
		float bmag = sqrtf(brot[0] * brot[0] + brot[1] * brot[1] + brot[2] * brot[2]);
		brot[0] /= bmag;
		brot[1] /= bmag;
		brot[2] /= bmag;
		
		// Only compute if neither vector is null
		if (bmag < 1 || mag_len < 1)
			mag_err[0] = mag_err[1] = mag_err[2] = 0;
		else
			CrossProduct((const float *) &mag.x, (const float *) brot, mag_err);
	} else {
		mag_err[0] = mag_err[1] = mag_err[2] = 0;
	}
	
	// Accumulate integral of error.  Scale here so that units are (deg/s) but Ki has units of s
	GyrosBiasData gyrosBias;
	GyrosBiasGet(&gyrosBias);
	gyrosBias.x += accel_err[0] * accelKi;
	gyrosBias.y += accel_err[1] * accelKi;
	gyrosBias.z += mag_err[2] * magKi;
	GyrosBiasSet(&gyrosBias);

	// Correct rates based on error, integral component dealt with in updateSensors
	gyrosData.x += accel_err[0] * accelKp / dT;
	gyrosData.y += accel_err[1] * accelKp / dT;
	gyrosData.z += accel_err[2] * accelKp / dT + mag_err[2] * magKp / dT;

	// Work out time derivative from INSAlgo writeup
	// Also accounts for the fact that gyros are in deg/s
	float qdot[4];
	qdot[0] = (-q[1] * gyrosData.x - q[2] * gyrosData.y - q[3] * gyrosData.z) * dT * F_PI / 180 / 2;
	qdot[1] = (q[0] * gyrosData.x - q[3] * gyrosData.y + q[2] * gyrosData.z) * dT * F_PI / 180 / 2;
	qdot[2] = (q[3] * gyrosData.x + q[0] * gyrosData.y - q[1] * gyrosData.z) * dT * F_PI / 180 / 2;
	qdot[3] = (-q[2] * gyrosData.x + q[1] * gyrosData.y + q[0] * gyrosData.z) * dT * F_PI / 180 / 2;
	
	// Take a time step
	q[0] = q[0] + qdot[0];
	q[1] = q[1] + qdot[1];
	q[2] = q[2] + qdot[2];
	q[3] = q[3] + qdot[3];
	
	if(q[0] < 0) {
		q[0] = -q[0];
		q[1] = -q[1];
		q[2] = -q[2];
		q[3] = -q[3];
	}
	
	// Renomalize
	qmag = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
	q[0] = q[0] / qmag;
	q[1] = q[1] / qmag;
	q[2] = q[2] / qmag;
	q[3] = q[3] / qmag;

	// If quaternion has become inappropriately short or is nan reinit.
	// THIS SHOULD NEVER ACTUALLY HAPPEN
	if((fabs(qmag) < 1.0e-3f) || (qmag != qmag)) {
		q[0] = 1;
		q[1] = 0;
		q[2] = 0;
		q[3] = 0;
	}

	quat_copy(q, &attitudeActual.q1);

	// Convert into eueler degrees (makes assumptions about RPY order)
	Quaternion2RPY(&attitudeActual.q1,&attitudeActual.Roll);

	AttitudeActualSet(&attitudeActual);
	
	// Flush these queues for avoid errors
	if ( xQueueReceive(baroQueue, &ev, 0) != pdTRUE )
	{
	}
	if ( xQueueReceive(gpsQueue, &ev, 0) != pdTRUE )
	{
	}

	AlarmsClear(SYSTEMALARMS_ALARM_ATTITUDE);

	return 0;
}

#include "insgps.h"
int32_t ins_failed = 0;
extern struct NavStruct Nav;
static int32_t updateAttitudeINSGPS(bool first_run)
{
	UAVObjEvent ev;
	GyrosData gyrosData;
	AccelsData accelsData;
	MagnetometerData magData;
	BaroAltitudeData baroData;
	
	static uint32_t ins_last_time = 0;

	static bool inited;
	if (first_run)
		inited = false;
	
	// Wait until the gyro and accel object is updated, if a timeout then go to failsafe
	if ( (xQueueReceive(gyroQueue, &ev, 10 / portTICK_RATE_MS) != pdTRUE) ||
		(xQueueReceive(accelQueue, &ev, 10 / portTICK_RATE_MS) != pdTRUE) )
	{
		AlarmsSet(SYSTEMALARMS_ALARM_ATTITUDE,SYSTEMALARMS_ALARM_WARNING);
		return -1;
	}
	
	// Get most recent data
	// TODO: Acquire all data in a queue
	GyrosGet(&gyrosData);
	AccelsGet(&accelsData);
	MagnetometerGet(&magData);
	BaroAltitudeGet(&baroData);
	
	bool mag_updated;
	bool baro_updated;
	bool gps_updated;
	
	if (inited) {
		mag_updated = 0;
		baro_updated = 0;
	}
	
	mag_updated |= xQueueReceive(magQueue, &ev, 0 / portTICK_RATE_MS) == pdTRUE;
	baro_updated |= xQueueReceive(baroQueue, &ev, 0 / portTICK_RATE_MS) == pdTRUE;
	gps_updated |= xQueueReceive(gpsQueue, &ev, 0 / portTICK_RATE_MS) == pdTRUE;
	
	if (!inited && (!mag_updated || !baro_updated || !gps_updated)) {
		// Don't initialize until all sensors are read
		return -1;
	} else if (!inited ) {
		inited = true;

		float Rbe[3][3], q[4];
		float ge[3]={0.0f,0.0f,-9.81f};
		float zeros[3]={0.0f,0.0f,0.0f};
		float Pdiag[16]={25.0f,25.0f,25.0f,5.0f,5.0f,5.0f,1e-5f,1e-5f,1e-5f,1e-5f,1e-5f,1e-5f,1e-5f,1e-4f,1e-4f,1e-4f};
		float vel[3], NED[3];
		
		INSGPSInit();
		
		HomeLocationData home;
		HomeLocationGet(&home);
		
		GPSPositionData gpsPosition;
		GPSPositionGet(&gpsPosition);
		
		vel[0] = gpsPosition.Groundspeed * cosf(gpsPosition.Heading * F_PI / 180.0f);
		vel[1] = gpsPosition.Groundspeed * sinf(gpsPosition.Heading * F_PI / 180.0f);
		vel[2] = 0;
		
		// convert from cm back to meters
		float LLA[3] = {(float) gpsPosition.Latitude / 1e7f, (float) gpsPosition.Longitude / 1e7f, (float) (gpsPosition.GeoidSeparation + gpsPosition.Altitude)};
		// put in local NED frame
		float ECEF[3] = {(float) (home.ECEF[0] / 100.0f), (float) (home.ECEF[1] / 100.0f), (float) (home.ECEF[2] / 100.0f)};
		LLA2Base(LLA, ECEF, (float (*)[3]) home.RNE, NED);

		RotFrom2Vectors(&accelsData.x, ge, &magData.x, home.Be, Rbe);
		R2Quaternion(Rbe,q);
		INSSetState(NED, vel, q, &gyrosData.x, zeros);
		INSSetGyroBias(&gyrosData.x);
		INSResetP(Pdiag);
		
		ins_last_time = PIOS_DELAY_GetRaw();	
		return 0;
	}
	
	// Perform the update	
	uint16_t sensors = 0;
	float dT;
	
	dT = PIOS_DELAY_DiffuS(ins_last_time) / 1.0e6f;
	ins_last_time = PIOS_DELAY_GetRaw();
	
	// This should only happen at start up or at mode switches
	if(dT > 0.01f)
		dT = 0.01f;
	else if(dT <= 0.001f)
		dT = 0.001f;
	
	
	GyrosBiasData gyrosBias;
	GyrosBiasGet(&gyrosBias);

	float gyros[3] = {(gyrosData.x + gyrosBias.x) * F_PI / 180.0f, 
		(gyrosData.y + gyrosBias.y) * F_PI / 180.0f, 
		(gyrosData.z + gyrosBias.z) * F_PI / 180.0f};
	
	// Advance the state estimate
	INSStatePrediction(gyros, &accelsData.x, dT);
	
	// Copy the attitude into the UAVO
	AttitudeActualData attitude;
	AttitudeActualGet(&attitude);
	attitude.q1 = Nav.q[0];
	attitude.q2 = Nav.q[1];
	attitude.q3 = Nav.q[2];
	attitude.q4 = Nav.q[3];
	Quaternion2RPY(&attitude.q1,&attitude.Roll);
	AttitudeActualSet(&attitude);
	
	// Copy the gyro bias into the UAVO
	gyrosBias.x = Nav.gyro_bias[0];
	gyrosBias.y = Nav.gyro_bias[1];
	gyrosBias.z = Nav.gyro_bias[2];
	GyrosBiasSet(&gyrosBias);
	
	// Advance the covariance estimate
	INSCovariancePrediction(dT);
	
	if(mag_updated)
		sensors |= MAG_SENSORS;
	if(baro_updated)
		sensors |= BARO_SENSOR;

	float NED[3] = {0,0,0};
	float vel[3] = {0,0,0};
	
	if(gps_updated)
	{
		sensors = HORIZ_SENSORS | VERT_SENSORS;
		GPSPositionData gpsPosition;
		GPSPositionGet(&gpsPosition);
		
		vel[0] = gpsPosition.Groundspeed * cosf(gpsPosition.Heading * F_PI / 180.0f);
		vel[1] = gpsPosition.Groundspeed * sinf(gpsPosition.Heading * F_PI / 180.0f);
		vel[2] = 0;

		HomeLocationData home;
		HomeLocationGet(&home);

		// convert from cm back to meters
		float LLA[3] = {(float) gpsPosition.Latitude / 1e7f, (float) gpsPosition.Longitude / 1e7f, (float) (gpsPosition.GeoidSeparation + gpsPosition.Altitude)};
		// put in local NED frame
		float ECEF[3] = {(float) (home.ECEF[0] / 100.0f), (float) (home.ECEF[1] / 100.0f), (float) (home.ECEF[2] / 100.0f)};
		LLA2Base(LLA, ECEF, (float (*)[3]) home.RNE, NED);
	}
	
	/*
	 * TODO: Need to add a general sanity check for all the inputs to make sure their kosher
	 * although probably should occur within INS itself
	 */
	INSCorrection(&magData.x, NED, vel, baroData.Altitude, sensors);
	
	// Copy the position and velocity into the UAVO
	PositionActualData positionActual;
	PositionActualGet(&positionActual);
	positionActual.North = Nav.Pos[0];
	positionActual.East = Nav.Pos[1];
	positionActual.Down = Nav.Pos[2];
	PositionActualSet(&positionActual);
	
	VelocityActualData velocityActual;
	VelocityActualGet(&velocityActual);
	velocityActual.North = Nav.Vel[0];
	velocityActual.East = Nav.Vel[1];
	velocityActual.Down = Nav.Vel[2];
	VelocityActualSet(&velocityActual);
	

	if(fabs(Nav.gyro_bias[0]) > 0.1f || fabs(Nav.gyro_bias[1]) > 0.1f || fabs(Nav.gyro_bias[2]) > 0.1f) {
		float zeros[3] = {0.0f,0.0f,0.0f};
		INSSetGyroBias(zeros);
	}

	return 0;
}

static void settingsUpdatedCb(UAVObjEvent * objEv) 
{
	AttitudeSettingsData attitudeSettings;
	AttitudeSettingsGet(&attitudeSettings);


	accelKp = attitudeSettings.AccelKp;
	accelKi = attitudeSettings.AccelKi;
	yawBiasRate = attitudeSettings.YawBiasRate;
	gyroGain = attitudeSettings.GyroGain;

	zero_during_arming = attitudeSettings.ZeroDuringArming == ATTITUDESETTINGS_ZERODURINGARMING_TRUE;

	accelbias[0] = attitudeSettings.AccelBias[ATTITUDESETTINGS_ACCELBIAS_X];
	accelbias[1] = attitudeSettings.AccelBias[ATTITUDESETTINGS_ACCELBIAS_Y];
	accelbias[2] = attitudeSettings.AccelBias[ATTITUDESETTINGS_ACCELBIAS_Z];

	GyrosBiasData gyrosBias;
	GyrosBiasGet(&gyrosBias);
	gyrosBias.x = attitudeSettings.GyroBias[ATTITUDESETTINGS_GYROBIAS_X] / 100.0f;
	gyrosBias.y = attitudeSettings.GyroBias[ATTITUDESETTINGS_GYROBIAS_Y] / 100.0f;
	gyrosBias.z = attitudeSettings.GyroBias[ATTITUDESETTINGS_GYROBIAS_Z] / 100.0f;
	GyrosBiasSet(&gyrosBias);
	
	// Indicates not to expend cycles on rotation
	if(attitudeSettings.BoardRotation[0] == 0 && attitudeSettings.BoardRotation[1] == 0 &&
	   attitudeSettings.BoardRotation[2] == 0) {
		rotate = 0;

		// Shouldn't be used but to be safe
		float rotationQuat[4] = {1,0,0,0};
		Quaternion2R(rotationQuat, R);
	} else {
		float rotationQuat[4];
		const float rpy[3] = {attitudeSettings.BoardRotation[ATTITUDESETTINGS_BOARDROTATION_ROLL],
			attitudeSettings.BoardRotation[ATTITUDESETTINGS_BOARDROTATION_PITCH],
			attitudeSettings.BoardRotation[ATTITUDESETTINGS_BOARDROTATION_YAW]};
		RPY2Quaternion(rpy, rotationQuat);
		Quaternion2R(rotationQuat, R);
		rotate = 1;
	}
}
/**
  * @}
  * @}
  */
