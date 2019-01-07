/***************************************************************************

    file                 : simu.cpp
    created              : Sun Mar 19 00:07:53 CET 2000
    copyright            : (C) 2000-2017 by Eric Espie, Bernhard Wymann
    email                : torcs@free.fr
    version              : $Id$

 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/
#include <fstream>
#include <stdlib.h>
#include <stdio.h>
#include <memory.h>
#include <math.h>
#ifdef WIN32
#include <windows.h>
#include <float.h>
#define isnan _isnan
#endif

#include <tgf.h>
#include <robottools.h>
#include "sim.h"

tCar *SimCarTable = 0;
tdble SimDeltaTime;
int SimTelemetry;
static int SimNbCars = 0;

tdble rulesFuelFactor = 1.0f;
tdble rulesDamageFactor = 1.0f;
tdble rulesTireFactor = 0.0f;

// declare outputfiles
const int nCarMax = 30;
std::ofstream outputFile[nCarMax];
//

/*
 * Check the input control from robots
 */
static void
ctrlCheck(tCar *car)
{
    tTransmission	*trans = &(car->transmission);
    tClutch		*clutch = &(trans->clutch);

    /* sanity check */
#if defined WIN32 || defined sun
    if (isnan(car->ctrl->accelCmd)) car->ctrl->accelCmd = 0;
    if (isnan(car->ctrl->brakeCmd)) car->ctrl->brakeCmd = 0;
    if (isnan(car->ctrl->clutchCmd)) car->ctrl->clutchCmd = 0;
    if (isnan(car->ctrl->steer)) car->ctrl->steer = 0;
#else
    if (isnan(car->ctrl->accelCmd) || isinf(car->ctrl->accelCmd)) car->ctrl->accelCmd = 0;
    if (isnan(car->ctrl->brakeCmd) || isinf(car->ctrl->brakeCmd)) car->ctrl->brakeCmd = 0;
    if (isnan(car->ctrl->clutchCmd) || isinf(car->ctrl->clutchCmd)) car->ctrl->clutchCmd = 0;
    if (isnan(car->ctrl->steer) || isinf(car->ctrl->steer)) car->ctrl->steer = 0;
#endif

    /* When the car is broken try to send it on the track side */
    if (car->carElt->_state & RM_CAR_STATE_BROKEN) {
	car->ctrl->accelCmd = 0.0f;
	car->ctrl->brakeCmd = 0.1f;
	car->ctrl->gear = 0;
	if (car->trkPos.toRight >  car->trkPos.seg->width / 2.0) {
	    car->ctrl->steer = 0.1f;
	} else {
	    car->ctrl->steer = -0.1f;
	}
    } else if (car->carElt->_state & RM_CAR_STATE_ELIMINATED) {
	car->ctrl->accelCmd = 0.0f;
	car->ctrl->brakeCmd = 0.1f;
	car->ctrl->gear = 0;
	if (car->trkPos.toRight >  car->trkPos.seg->width / 2.0) {
	    car->ctrl->steer = 0.1f;
	} else {
	    car->ctrl->steer = -0.1f;
	}
    } else if (car->carElt->_state & RM_CAR_STATE_FINISH) {
	/* when the finish line is passed, continue at "slow" pace */
	car->ctrl->accelCmd = MIN(car->ctrl->accelCmd, 0.20);
	if (car->DynGC.vel.x > 30.0) {
	    car->ctrl->brakeCmd = MAX(car->ctrl->brakeCmd, 0.05);
	}
    }

    /* check boundaries */
    if (car->ctrl->accelCmd > 1.0) {
	car->ctrl->accelCmd = 1.0;
    } else if (car->ctrl->accelCmd < 0.0) {
	car->ctrl->accelCmd = 0.0;
    }
    if (car->ctrl->brakeCmd > 1.0) {
	car->ctrl->brakeCmd = 1.0;
    } else if (car->ctrl->brakeCmd < 0.0) {
	car->ctrl->brakeCmd = 0.0;
    }
    if (car->ctrl->clutchCmd > 1.0) {
	car->ctrl->clutchCmd = 1.0;
    } else if (car->ctrl->clutchCmd < 0.0) {
	car->ctrl->clutchCmd = 0.0;
    }
    if (car->ctrl->steer > 1.0) {
	car->ctrl->steer = 1.0;
    } else if (car->ctrl->steer < -1.0) {
	car->ctrl->steer = -1.0;
    }

    clutch->transferValue = 1.0 - car->ctrl->clutchCmd;
}

/* Initial configuration */
void
SimConfig(tCarElt *carElt, RmInfo *info)
{
    tCar *car = &(SimCarTable[carElt->index]);

    memset(car, 0, sizeof(tCar));

    car->carElt = carElt;
    car->DynGCg = car->DynGC = carElt->_DynGC;
    car->trkPos = carElt->_trkPos;
    car->ctrl   = &carElt->ctrl;
    car->params = carElt->_carHandle;

    SimCarConfig(car);

    SimCarCollideConfig(car, info->track);
    sgMakeCoordMat4(carElt->pub.posMat, carElt->_pos_X, carElt->_pos_Y, carElt->_pos_Z - carElt->_statGC_z,
		    RAD2DEG(carElt->_yaw), RAD2DEG(carElt->_roll), RAD2DEG(carElt->_pitch));
}

/* After pit stop */
void SimReConfig(tCarElt *carElt)
{
	tCar *car = &(SimCarTable[carElt->index]);
	if (carElt->pitcmd.fuel > 0) {
		car->fuel += carElt->pitcmd.fuel;
		if (car->fuel > car->tank) car->fuel = car->tank;
	}

	if (carElt->pitcmd.repair > 0) {
		car->dammage -= carElt->pitcmd.repair;
		if (car->dammage < 0) car->dammage = 0;
	}

	int i;
	SimSteerReConfig(car);
	SimBrakeSystemReConfig(car);

	for (i = 0; i < 2; i++) {
		SimWingReConfig(car, i);
		SimAxleReConfig(car, i);
	}

	for (i = 0; i < 4; i++) {
		SimWheelReConfig(car, i);
		if (carElt->pitcmd.tireChange == tCarPitCmd::ALL) {
			SimWheelResetWear(car, i);
		}
	}

	SimTransmissionReConfig(car);
}


static void
RemoveCar(tCar *car, tSituation *s)
{
	int i;
	tCarElt *carElt;
	tTrkLocPos trkPos;
	int trkFlag;
	tdble travelTime;
	tdble dang;

	static tdble PULL_Z_OFFSET = 3.0;
	static tdble PULL_SPD = 0.5;

	carElt = car->carElt;

	if (carElt->_state & RM_CAR_STATE_PULLUP) {
		carElt->_pos_Z += car->restPos.vel.z * SimDeltaTime;
		carElt->_yaw += car->restPos.vel.az * SimDeltaTime;
		carElt->_roll += car->restPos.vel.ax * SimDeltaTime;
		carElt->_pitch += car->restPos.vel.ay * SimDeltaTime;
		sgMakeCoordMat4(carElt->pub.posMat, carElt->_pos_X, carElt->_pos_Y, carElt->_pos_Z - carElt->_statGC_z,
			RAD2DEG(carElt->_yaw), RAD2DEG(carElt->_roll), RAD2DEG(carElt->_pitch));

		if (carElt->_pos_Z > (car->restPos.pos.z + PULL_Z_OFFSET)) {
			carElt->_state &= ~RM_CAR_STATE_PULLUP;
			carElt->_state |= RM_CAR_STATE_PULLSIDE;
			// Moved pullside velocity computation down due to floating point error accumulation.
		}
		return;
	}


	if (carElt->_state & RM_CAR_STATE_PULLSIDE) {
		// Recompute speed to avoid missing the parking point due to error accumulation (the pos might be
		// in the 0-10000 range, depending on the track and vel*dt is around 0-0.001, so basically all
		// but the most significant digits are lost under bad conditions, happens e.g on e-track-4).
		// Should not lead to a division by zero because the pullside process stops if the car is within
		// [0.5, 0.5]. Do not move it back.
		travelTime = DIST(car->restPos.pos.x, car->restPos.pos.y, carElt->_pos_X, carElt->_pos_Y) / PULL_SPD;
		car->restPos.vel.x = (car->restPos.pos.x - carElt->_pos_X) / travelTime;
		car->restPos.vel.y = (car->restPos.pos.y - carElt->_pos_Y) / travelTime;

		carElt->_pos_X += car->restPos.vel.x * SimDeltaTime;
		carElt->_pos_Y += car->restPos.vel.y * SimDeltaTime;
		sgMakeCoordMat4(carElt->pub.posMat, carElt->_pos_X, carElt->_pos_Y, carElt->_pos_Z - carElt->_statGC_z,
			RAD2DEG(carElt->_yaw), RAD2DEG(carElt->_roll), RAD2DEG(carElt->_pitch));

		if ((fabs(car->restPos.pos.x - carElt->_pos_X) < 0.5) && (fabs(car->restPos.pos.y - carElt->_pos_Y) < 0.5)) {
			carElt->_state &= ~RM_CAR_STATE_PULLSIDE;
			carElt->_state |= RM_CAR_STATE_PULLDN;
		}
		return;
	}


	if (carElt->_state & RM_CAR_STATE_PULLDN) {
		carElt->_pos_Z -= car->restPos.vel.z * SimDeltaTime;
		sgMakeCoordMat4(carElt->pub.posMat, carElt->_pos_X, carElt->_pos_Y, carElt->_pos_Z - carElt->_statGC_z,
			RAD2DEG(carElt->_yaw), RAD2DEG(carElt->_roll), RAD2DEG(carElt->_pitch));

		if (carElt->_pos_Z < car->restPos.pos.z) {
			carElt->_state &= ~RM_CAR_STATE_PULLDN;
			carElt->_state |= RM_CAR_STATE_OUT;
		}
		return;
	}


	if (carElt->_state & (RM_CAR_STATE_NO_SIMU & ~RM_CAR_STATE_PIT)) {
		return;
	}

	if (carElt->_state & RM_CAR_STATE_PIT) {
		if ((s->_maxDammage) && (car->dammage > s->_maxDammage)) {
			// Broken during pit stop.
			carElt->_state &= ~RM_CAR_STATE_PIT;
			carElt->_pit->pitCarIndex = TR_PIT_STATE_FREE;
		} else {
			return;
		}
	}

	if ((s->_maxDammage) && (car->dammage > s->_maxDammage)) {
		carElt->_state |= RM_CAR_STATE_BROKEN;
	} else {
		carElt->_state |= RM_CAR_STATE_OUTOFGAS;
	}

	carElt->_gear = car->transmission.gearbox.gear = 0;
	carElt->_enginerpm = car->engine.rads = 0;

	if (!(carElt->_state & RM_CAR_STATE_DNF)) {
		if (fabs(carElt->_speed_x) > 1.0) {
			return;
		}
	}

	carElt->_state |= RM_CAR_STATE_PULLUP;
	// RM_CAR_STATE_NO_SIMU evaluates to > 0 from here, so we remove the car from the
	// collision detection.
	SimCollideRemoveCar(car, s->_ncars);

	carElt->priv.simcollision = carElt->priv.collision = car->collision = 0;
	for(i = 0; i < 4; i++) {
		carElt->_skid[i] = 0;
		carElt->_wheelSpinVel(i) = 0;
		carElt->_brakeTemp(i) = 0;
	}

	carElt->pub.DynGC = car->DynGC;
	carElt->_speed_x = 0;

	// Compute the target zone for the wrecked car.
	trkPos = car->trkPos;
	if (trkPos.toRight >  trkPos.seg->width / 2.0) {
		while (trkPos.seg->lside != 0) {
			trkPos.seg = trkPos.seg->lside;
		}
		trkPos.toLeft = -3.0;
		trkFlag = TR_TOLEFT;
	} else {
		while (trkPos.seg->rside != 0) {
			trkPos.seg = trkPos.seg->rside;
		}
		trkPos.toRight = -3.0;
		trkFlag = TR_TORIGHT;
	}

	trkPos.type = TR_LPOS_SEGMENT;
	RtTrackLocal2Global(&trkPos, &(car->restPos.pos.x), &(car->restPos.pos.y), trkFlag);
	car->restPos.pos.z = RtTrackHeightL(&trkPos) + carElt->_statGC_z;
	car->restPos.pos.az = RtTrackSideTgAngleL(&trkPos);
	car->restPos.pos.ax = 0;
	car->restPos.pos.ay = 0;

	car->restPos.vel.z = PULL_SPD;
	travelTime = (car->restPos.pos.z + PULL_Z_OFFSET - carElt->_pos_Z) / car->restPos.vel.z;
	dang = car->restPos.pos.az - carElt->_yaw;
	NORM_PI_PI(dang);
	car->restPos.vel.az = dang / travelTime;
	dang = car->restPos.pos.ax - carElt->_roll;
	NORM_PI_PI(dang);
	car->restPos.vel.ax = dang / travelTime;
	dang = car->restPos.pos.ay - carElt->_pitch;
	NORM_PI_PI(dang);
	car->restPos.vel.ay = dang / travelTime;
}



void
SimUpdate(tSituation *s, double deltaTime, int telemetry)
{
	int i;
	int ncar;
	tCarElt *carElt;
	tCar *car;

	SimDeltaTime = deltaTime;
	SimTelemetry = telemetry;
	for (ncar = 0; ncar < s->_ncars; ncar++) {
		SimCarTable[ncar].collision = 0;
		SimCarTable[ncar].blocked = 0;
	}

	for (ncar = 0; ncar < s->_ncars; ncar++) {
		car = &(SimCarTable[ncar]);
		carElt = car->carElt;

		if (carElt->_state & RM_CAR_STATE_NO_SIMU) {
			RemoveCar(car, s);
			continue;
		} else if (((s->_maxDammage) && (car->dammage > s->_maxDammage)) ||
			(car->fuel == 0) ||
			(car->carElt->_state & RM_CAR_STATE_ELIMINATED))  {
			RemoveCar(car, s);
			if (carElt->_state & RM_CAR_STATE_NO_SIMU) {
				continue;
			}
		}

		if (s->_raceState & RM_RACE_PRESTART) {
			car->ctrl->gear = 0;
		}

		SimAtmosphereUpdate(car, s);
		CHECK(car);
		ctrlCheck(car);
		CHECK(car);
		SimSteerUpdate(car);
		CHECK(car);
		SimGearboxUpdate(car);
		CHECK(car);
		SimEngineUpdateTq(car);
		CHECK(car);

		if (!(s->_raceState & RM_RACE_PRESTART)) {

			SimCarUpdateWheelPos(car);
			CHECK(car);
			SimBrakeSystemUpdate(car);
			CHECK(car);
			SimAeroUpdate(car, s);
			CHECK(car);
			for (i = 0; i < 2; i++){
				SimWingUpdate(car, i, s);
			}
			CHECK(car);
			for (i = 0; i < 4; i++){
				SimWheelUpdateRide(car, i);
			}
			CHECK(car);
			for (i = 0; i < 2; i++){
				SimAxleUpdate(car, i);
			}
			CHECK(car);
			for (i = 0; i < 4; i++){
				SimWheelUpdateForce(car, i);
				SimWheelUpdateTire(car, i);

				// Reset tire damage in pre simulation state (when the model is thrown in the world)
				if (s->_raceState == 0) {
					SimWheelResetWear(car, i);
				}
			}
			CHECK(car);
			SimTransmissionUpdate(car);
			CHECK(car);
			SimWheelUpdateRotation(car);
			CHECK(car);
			SimCarUpdate(car, s);
			// wueli: Set Car 1 postion to const
			if (ncar == 1){
			SimCarSetPos(car, 11.194, 201.479, 0.3, 1.5); // x, y, z, az
			};


			CHECK(car);
		} else {
			SimEngineUpdateRpm(car, 0.0);
		}
	}

	SimCarCollideCars(s);

	/* printf ("%f - ", s->currentTime); */

	for (ncar = 0; ncar < s->_ncars; ncar++) {
		car = &(SimCarTable[ncar]);
		CHECK(car);
		carElt = car->carElt;

		if (carElt->_state & RM_CAR_STATE_NO_SIMU) {
			continue;
		}

		CHECK(car);
		SimCarUpdate2(car, s); /* telemetry */
		SimWriteData(car, s, ncar); /* write out data */

		/* copy back the data to carElt */

		carElt->pub.DynGC = car->DynGC;
		carElt->pub.DynGCg = car->DynGCg;
		carElt->pub.speed = car->speed;
		sgMakeCoordMat4(carElt->pub.posMat, carElt->_pos_X, carElt->_pos_Y, carElt->_pos_Z - carElt->_statGC_z,
				RAD2DEG(carElt->_yaw), RAD2DEG(carElt->_roll), RAD2DEG(carElt->_pitch));
		carElt->_trkPos = car->trkPos;

		for (i = 0; i < 4; i++) {
			tWheelState* const wheelState = &(carElt->priv.wheel[i]);
			const tWheel* const wheel = &(car->wheel[i]);

			wheelState->relPos = wheel->relPos;
			wheelState->seg = wheel->trkPos.seg;
			wheelState->brakeTemp = wheel->brake.temp;
			wheelState->currentGraining = wheel->currentGraining;
			wheelState->currentPressure = wheel->currentPressure;
			wheelState->currentTemperature = wheel->currentTemperature;
			wheelState->currentWear = wheel->currentWear;

			carElt->pub.corner[i] = car->corner[i].pos;
		}

		carElt->_gear = car->transmission.gearbox.gear;
		carElt->_enginerpm = car->engine.rads;
		carElt->_fuel = car->fuel;
		carElt->priv.collision |= car->collision;
		carElt->priv.simcollision = car->collision;
		carElt->_dammage = car->dammage;
		carElt->priv.localPressure = car->localPressure;
	}
}


void
SimInit(int nbcars, tTrack* track, tdble fuelFactor, tdble damageFactor, tdble tireFactor)
{
	rulesFuelFactor = fuelFactor;
	rulesDamageFactor = damageFactor;
	rulesTireFactor = tireFactor;
    SimNbCars = nbcars;
    SimCarTable = (tCar*)calloc(nbcars, sizeof(tCar));
    SimCarCollideInit(track);
	InitOutputFiles(nbcars);
}

void
SimShutdown(void)
{
    tCar *car;
    int	 ncar;

    SimCarCollideShutdown(SimNbCars);
    if (SimCarTable) {
	for (ncar = 0; ncar < SimNbCars; ncar++) {
	    car = &(SimCarTable[ncar]);
	    SimEngineShutdown(car);
	}
	free(SimCarTable);
	SimCarTable = 0;
    }
}


bool SimAdjustPitCarSetupParam(tCarPitSetupValue* v)
{
	// If min == max there is nothing to adjust
	if (fabs(v->max - v->min) >= 0.0001f) {
		// Ensure that value is in intended borders
		if (v->value > v->max) {
			v->value = v->max;
		} else if (v->value < v->min) {
			v->value = v->min;
		}
		return true;
	}

	v->value = v->max;
	return false;
}

void InitOutputFiles(int nCars)
{
	for(int i = 0; i < nCars; i++)
	{
		std::string filename = "/home/ueli/SimSoft/code-torcs/torcs/torcs/data/sim/";
		filename += std::to_string(i);
		filename += ".csv";
		outputFile[i].open(filename.c_str());
		outputFile[i] << "pos_X,pos_Y,pos_Z,vel_X,vel_Y,vel_Z,acl_X,acl_Y,acl_Z,";
		outputFile[i] << "velocity,";
		outputFile[i] << "roll_X,pitch_Y,yaw_Z,roll_X_rate,pitch_Y_rate,yaw_Z_rate,roll_X_acl,pitch_Y_acl,yaw_Z_acl,";
		outputFile[i] << "steerCmd,wheel_rgt,wheel_lft,aclCmd,brakeCmd,clutchCmd,geraCmd\n";
	}
}



void SimWriteData(tCar *car, tSituation *, int i)
{
	// Positional Coordinates
	outputFile[i] << car->DynGC.pos.x;	outputFile[i] << ",";
	outputFile[i] << car->DynGC.pos.y;	outputFile[i] << ",";
	outputFile[i] << car->DynGC.pos.z;	outputFile[i] << ",";
	outputFile[i] << car->DynGC.vel.x;	outputFile[i] << ",";
	outputFile[i] << car->DynGC.vel.y;	outputFile[i] << ",";
	outputFile[i] << car->DynGC.vel.z;	outputFile[i] << ",";
	outputFile[i] << car->DynGC.acc.x;	outputFile[i] << ",";
	outputFile[i] << car->DynGC.acc.y;	outputFile[i] << ",";
	outputFile[i] << car->DynGC.acc.z;	outputFile[i] << ",";
	// Other Coordinates
	outputFile[i] << car->speed;		outputFile[i] << ",";
	// Angular Coordinates
	outputFile[i] << car->DynGC.pos.ax;	outputFile[i] << ",";
	outputFile[i] << car->DynGC.pos.ay;	outputFile[i] << ",";
	outputFile[i] << car->DynGC.pos.az;	outputFile[i] << ",";
	outputFile[i] << car->DynGC.vel.ax;	outputFile[i] << ",";
	outputFile[i] << car->DynGC.vel.ay;	outputFile[i] << ",";
	outputFile[i] << car->DynGC.vel.az;	outputFile[i] << ",";
	outputFile[i] << car->DynGC.acc.ax;	outputFile[i] << ",";
	outputFile[i] << car->DynGC.acc.ay;	outputFile[i] << ",";
	outputFile[i] << car->DynGC.acc.az;	outputFile[i] << ",";
	// Control Inputs
	outputFile[i] << car->ctrl->steer;		outputFile[i] << ",";
	outputFile[i] << car->wheel[FRNT_RGT].steer;		outputFile[i] << ",";
	outputFile[i] << car->wheel[FRNT_LFT].steer;		outputFile[i] << ",";

	outputFile[i] << car->ctrl->accelCmd;	outputFile[i] << ",";
	outputFile[i] << car->ctrl->brakeCmd;	outputFile[i] << ",";
	outputFile[i] << car->ctrl->clutchCmd;	outputFile[i] << ",";
	outputFile[i] << car->ctrl->gear;		outputFile[i] << "\n";
	// //file.close();
}