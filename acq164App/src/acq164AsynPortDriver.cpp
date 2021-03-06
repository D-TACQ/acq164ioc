/* ------------------------------------------------------------------------- */
/* acq164asynPortDriver.cpp
 * Project: ACQ420_FMC
 * Created: Thu Dec 31 15:16:04 2020                      / User: pgm
 * ------------------------------------------------------------------------- *
 *   Copyright (C) 2020/2021 Peter Milne, D-TACQ Solutions Ltd         *
 *                      <peter dot milne at D hyphen TACQ dot com>           *
 *                                                                           *
 *  This program is free software; you can redistribute it and/or modify     *
 *  it under the terms of Version 2 of the GNU General Public License        *
 *  as published by the Free Software Foundation;                            *
 *                                                                           *
 *  This program is distributed in the hope that it will be useful,          *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of           *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the            *
 *  GNU General Public License for more details.                             *
 *                                                                           *
 *  You should have received a copy of the GNU General Public License        *
 *  along with this program; if not, write to the Free Software              *
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.                *
 *
 * TODO
 * TODO
\* ------------------------------------------------------------------------- */

/*
 * adapted from testAsynPortDriver.cpp
 *
 * Asyn driver that inherits from the asynPortDriver class to demonstrate its use.
 * It simulates a digital scope looking at a 1kHz 1000-point noisy sine wave.  Controls are
 * provided for time/division, volts/division, volt offset, trigger delay, noise amplitude, update time,
 * and run/stop.
 * Readbacks are provides for the waveform data, min, max and mean values.
 *
 * Author: Mark Rivers
 *
 * Created Feb. 5, 2009
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <math.h>

#include <epicsTypes.h>
#include <epicsTime.h>
#include <epicsThread.h>
#include <epicsString.h>
#include <epicsTimer.h>
#include <epicsMutex.h>
#include <epicsEvent.h>
#include <iocsh.h>

#include "acq164AsynPortDriver.h"
#include <epicsExport.h>

#define FREQUENCY 1000       /* Frequency in Hz */
#define AMPLITUDE 1.0        /* Plus and minus peaks of sin wave */
#define NUM_DIVISIONS 10     /* Number of scope divisions in X and Y */
#define MIN_UPDATE_TIME 0.02 /* Minimum update time, to prevent CPU saturation */

#define MAX_ENUM_STRING_SIZE 20

static const char *driverName="acq164AsynPortDriver";

void task_runner(void *drvPvt)
{
    acq164AsynPortDriver *pPvt = (acq164AsynPortDriver *)drvPvt;

    pPvt->task();
}

/** Constructor for the testAsynPortDriver class.
  * Calls constructor for the asynPortDriver base class.
  * \param[in] portName The name of the asyn port driver to be created.
  * \param[in] maxPoints The maximum  number of points in the volt and time arrays */
acq164AsynPortDriver::acq164AsynPortDriver(const char *portName, int maxPoints, int _nchan)
   : asynPortDriver(portName,
                    _nchan, /* maxAddr */
                    asynInt32Mask | asynFloat64Mask | asynFloat64ArrayMask | asynEnumMask | asynDrvUserMask, /* Interface mask */
                    asynInt32Mask | asynFloat64Mask | asynFloat64ArrayMask | asynEnumMask,  /* Interrupt mask */
                    0, /* asynFlags.  This driver does not block and it is not multi-device, so flag is 0 */
                    1, /* Autoconnect */
                    0, /* Default priority */
                    0) /* Default stack size*/,
					nchan(_nchan),
					acc(_nchan)
{
    asynStatus status;
    int i;

    /* Make sure maxPoints is positive */
    if (maxPoints < 1) maxPoints = 100;

    /* Allocate the waveform array */
    pData_ = (epicsFloat64 *)calloc(maxPoints*nchan, sizeof(epicsFloat64));

    /* Allocate the time base array */
    pTimeBase_ = (epicsFloat64 *)calloc(maxPoints, sizeof(epicsFloat64));

    /* Set the time base array */
    for (i=0; i<maxPoints; i++) pTimeBase_[i] = (double)i / (maxPoints-1) * NUM_DIVISIONS;

    eventId_ = epicsEventCreate(epicsEventEmpty);
    createParam(P_RunString,                asynParamInt32,         &P_Run);
    createParam(P_MaxPointsString,          asynParamInt32,         &P_MaxPoints);
    createParam(P_NoiseAmplitudeString,     asynParamFloat64,       &P_NoiseAmplitude);
    createParam(P_UpdateTimeString,         asynParamFloat64,       &P_UpdateTime);
    createParam(P_WaveformString,           asynParamFloat64Array,  &P_Waveform);
    createParam(P_ScalarString,				asynParamFloat64,		&P_Scalar);
    createParam(P_TimeBaseString,           asynParamFloat64Array,  &P_TimeBase);
    createParam(P_MinValueString,           asynParamFloat64,       &P_MinValue);
    createParam(P_MaxValueString,           asynParamFloat64,       &P_MaxValue);
    createParam(P_MeanValueString,          asynParamFloat64,       &P_MeanValue);
    createParam(PS_SCAN_FREQ,          		asynParamInt32,       	&P_ScanFreq);

    /* Set the initial values of some parameters */
    setIntegerParam(P_MaxPoints,         maxPoints);
    setIntegerParam(P_Run,               0);

    setDoubleParam (P_UpdateTime,        0.5);
    setDoubleParam (P_NoiseAmplitude,    0.1);
    setDoubleParam (P_MinValue,          0.0);
    setDoubleParam (P_MaxValue,          3.3);
    setDoubleParam (P_MeanValue,         0.0);



    /* Create the thread that computes the waveforms in the background */
    status = (asynStatus)(epicsThreadCreate("testAsynPortDriverTask",
                          epicsThreadPriorityMedium,
                          epicsThreadGetStackSize(epicsThreadStackMedium),
						  (EPICSTHREADFUNC)::task_runner,
                          this) == NULL);
    if (status) {
        printf("%s:%s: epicsThreadCreate failure\n", driverName, __FUNCTION__);
        return;
    }
}





/** Called when asyn clients call pasynInt32->write().
  * This function sends a signal to the simTask thread if the value of P_Run has changed.
  * For all parameters it sets the value in the parameter library and calls any registered callbacks..
  * \param[in] pasynUser pasynUser structure that encodes the reason and address.
  * \param[in] value Value to write. */
asynStatus acq164AsynPortDriver::writeInt32(asynUser *pasynUser, epicsInt32 value)
{
    int function = pasynUser->reason;
    asynStatus status = asynSuccess;
    const char *paramName;
    const char* functionName = "writeInt32";

    /* Set the parameter in the parameter library. */
    status = (asynStatus) setIntegerParam(function, value);

    /* Fetch the parameter string name for possible use in debugging */
    getParamName(function, &paramName);

    if (function == P_Run) {
        /* If run was set then wake up the simulation task */
        if (value) epicsEventSignal(eventId_);
    }
    else {
        /* All other parameters just get set in parameter list, no need to
         * act on them here */
    }

    /* Do callbacks so higher layers see any changes */
    status = (asynStatus) callParamCallbacks();

    if (status)
        epicsSnprintf(pasynUser->errorMessage, pasynUser->errorMessageSize,
                  "%s:%s: status=%d, function=%d, name=%s, value=%d",
                  driverName, functionName, status, function, paramName, value);
    else
        asynPrint(pasynUser, ASYN_TRACEIO_DRIVER,
              "%s:%s: function=%d, name=%s, value=%d\n",
              driverName, functionName, function, paramName, value);
    return status;
}

/** Called when asyn clients call pasynFloat64->write().
  * This function sends a signal to the simTask thread if the value of P_UpdateTime has changed.
  * For all  parameters it  sets the value in the parameter library and calls any registered callbacks.
  * \param[in] pasynUser pasynUser structure that encodes the reason and address.
  * \param[in] value Value to write. */
asynStatus acq164AsynPortDriver::writeFloat64(asynUser *pasynUser, epicsFloat64 value)
{
    int function = pasynUser->reason;
    asynStatus status = asynSuccess;
    epicsInt32 run;
    const char *paramName;
    const char* functionName = "writeFloat64";

    /* Set the parameter in the parameter library. */
    status = (asynStatus) setDoubleParam(function, value);

    /* Fetch the parameter string name for possible use in debugging */
    getParamName(function, &paramName);

    if (function == P_UpdateTime) {
        /* Make sure the update time is valid. If not change it and put back in parameter library */
        if (value < MIN_UPDATE_TIME) {
            asynPrint(pasynUser, ASYN_TRACE_WARNING,
                "%s:%s: warning, update time too small, changed from %f to %f\n",
                driverName, functionName, value, MIN_UPDATE_TIME);
            value = MIN_UPDATE_TIME;
            setDoubleParam(P_UpdateTime, value);
        }
        /* If the update time has changed and we are running then wake up the simulation task */
        getIntegerParam(P_Run, &run);
        if (run) epicsEventSignal(eventId_);
    } else {
        /* All other parameters just get set in parameter list, no need to
         * act on them here */
    }

    /* Do callbacks so higher layers see any changes */
    status = (asynStatus) callParamCallbacks();

    if (status)
        epicsSnprintf(pasynUser->errorMessage, pasynUser->errorMessageSize,
                  "%s:%s: status=%d, function=%d, name=%s, value=%f",
                  driverName, functionName, status, function, paramName, value);
    else
        asynPrint(pasynUser, ASYN_TRACEIO_DRIVER,
              "%s:%s: function=%d, name=%s, value=%f\n",
              driverName, functionName, function, paramName, value);
    return status;
}


/** Called when asyn clients call pasynFloat64Array->read().
  * Returns the value of the P_Waveform or P_TimeBase arrays.
  * \param[in] pasynUser pasynUser structure that encodes the reason and address.
  * \param[in] value Pointer to the array to read.
  * \param[in] nElements Number of elements to read.
  * \param[out] nIn Number of elements actually read. */
asynStatus acq164AsynPortDriver::readFloat64Array(asynUser *pasynUser, epicsFloat64 *value,
                                         size_t nElements, size_t *nIn)
{
    int function = pasynUser->reason;
    size_t ncopy;
    epicsInt32 itemp;
    asynStatus status = asynSuccess;
    epicsTimeStamp timeStamp;
    const char *functionName = "readFloat64Array";

    getTimeStamp(&timeStamp);
    pasynUser->timestamp = timeStamp;
    getIntegerParam(P_MaxPoints, &itemp); ncopy = itemp;
    if (nElements < ncopy) ncopy = nElements;
    if (function == P_Waveform) {
    	assert(0);
        *nIn = ncopy;
    }
    else if (function == P_TimeBase) {
        memcpy(value, pTimeBase_, ncopy*sizeof(epicsFloat64));
        *nIn = ncopy;
    }
    if (status)
        epicsSnprintf(pasynUser->errorMessage, pasynUser->errorMessageSize,
                  "%s:%s: status=%d, function=%d",
                  driverName, functionName, status, function);
    else
        asynPrint(pasynUser, ASYN_TRACEIO_DRIVER,
              "%s:%s: function=%d\n",
              driverName, functionName, function);
    return status;
}

asynStatus acq164AsynPortDriver::readEnum(asynUser *pasynUser, char *strings[], int values[], int severities[], size_t nElements, size_t *nIn)
{
    //int function = pasynUser->reason;
    size_t i;

    if (0) {
    	;
    }
    else {
        *nIn = 0;
        return asynError;
    }
    *nIn = i;
    return asynSuccess;
}



//#include "local.h"
#include "acq2xx_api.h"
#include "acq_transport.h"
#include "AcqType.h"
#include "Frame.h"

#include "DataStreamer.h"
#include "DirfileFrameHandler.h"

class Acq164Device: public acq164AsynPortDriver, FrameHandler {
	int verbose;
	int cursor;
	virtual void onFrame(
			Acq2xx& _card, const AcqType& _acqType,
			const Frame* frame);

	double* eslo;
	double* eoff;

	void compute_cal(Acq2xx& card);
	void setup(Acq2xx& card);
public:
	Acq164Device(const char *portName, int maxArraySize, int nchan) :
		acq164AsynPortDriver(portName, maxArraySize, nchan),
		cursor(0)
	{
		const char* key = ::getenv("ACQ164DEVICE_VERBOSE");
		if (key){
			verbose = ::strtoul(key, 0, 0);
		}
		key = ::getenv("ACQ200_DEBUG");
		if (key){
			acq200_debug = ::strtoul(key, 0, 0);
		}
	}
	virtual void task();
};

/*
 *  y = mx + c
 *  (y - Y1)/(x-X1) = (Y2-Y1)/(X2-X1)
 *
 *  y = Y1 + (x-X1)*(Y2-Y1)/(X2-X1)
 *  y = x*ESLO + Y1-X1*ESLO
 *
 *  ESLO = (Y2-Y1)/(X2-X1) = (Y2-Y1)/(1<<24)
 *  EOFF = Y1 -X1*ESLO
 */
void Acq164Device::compute_cal(Acq2xx& card)
{
	acq2xx_VRange* ranges = new acq2xx_VRange[nchan+2];  /* +2? Bug in getChannelRanges() ? */
	eslo = new double[nchan];
	eoff = new double[nchan];
	int X1 = -(1<<23);
	int X2 = 1<<23;

	if (verbose) printf("nchan:%d\n", nchan);

	card.getChannelRanges(ranges, nchan+1);
	for (int ii = 0; ii < nchan; ++ii){
		double Y1 = ranges[ii+1].vmin;
		double Y2 = ranges[ii+1].vmax;
		double ESLO = (Y2-Y1)/(X2-X1);
		double EOFF = Y1 - X1*ESLO;
		if (verbose) printf("[%2d] Y1:%.2f Y2:%.2f %x ESLO:%.5g EOFF:%.5f\n", ii, Y1, Y2, X2-X1, ESLO, EOFF);
		eslo[ii] = ESLO;
		eoff[ii] = EOFF;
	}

	delete[] ranges;
}

void Acq164Device::setup(Acq2xx& card)
{
	enum STATE state;
	if (card.getState(state) != STATUS_OK){
		fprintf(stderr, "ERROR: failed to get state\n");
		exit(1);
	}
	if (state != ST_STOP){
		fprintf(stderr, "card state:%d let it run, or abort if you want it to be reconfigured\n", state);
		return;
	}

	char response[80];

	card.getTransport()->acq2sh("set.dtacq channel_mask 1", response, 80);
	card.getTransport()->acq2sh("set.acq164.role MASTER 20", response, 80);
	card.getTransport()->acqcmd("setMode SOFT_CONTINUOUS 1", response, 80);
	card.getTransport()->acqcmd("setArm", response, 80);
}

void Acq164Device::onFrame(
		Acq2xx& _card, const AcqType& _acqType,
		const Frame* frame)
{
	const ConcreteFrame<int> *cf =
				dynamic_cast<const ConcreteFrame<int> *>(frame);
	const int maxPoints = get_maxPoints();

	for (int ic = 0; ic < nchan; ++ic){
		int ix0 = ic*maxPoints;
		int consecutive_zeros = 0;
		const int* raw = cf->getChannel(ic+1);
		for (int id = 0; id < FRAME_SAMPLES; ++id){
			int yy = raw[id];
			if (yy == 0){
				if (++consecutive_zeros > 60){
					printf("%s zeros detected at %lld\n", __FUNCTION__, cf->getStartSampleNumber());
					exit(1);
				}
			}
			double volts = eslo[ic]*yy + eoff[ic];
			pData_[ix0+cursor+id] = volts;
			acc.set(ic, volts);
		}
	}
	cursor += FRAME_SAMPLES;

	int scan_freq;
	getIntegerParam(P_ScanFreq, &scan_freq);

	if (acc.update_timestamp(NSPS/scan_freq)){
		for (int ic = 0; ic < nchan; ++ic){
			if (verbose && ic < 3) printf("setDoubleParam(%d %d %f\n", ic, P_Scalar, acc.get(ic));
			setDoubleParam(ic, P_Scalar, acc.get(ic));
			callParamCallbacks(ic);
		}
		acc.clear();
	}

	if (cursor >= maxPoints){
		//printf("%s %lld\n", __FUNCTION__, cf->getStartSampleNumber());
		setDoubleParam(P_UpdateTime, cf->getStartSampleNumber());
		callParamCallbacks();

		for (int ic=0; ic< nchan; ic++){
			int ix0 = ic*maxPoints;
		  	doCallbacksFloat64Array(pData_+ix0, maxPoints, P_Waveform, ic);
		}
		cursor = 0;
	}
}



void Acq164Device::task(void)
{
	Transport *t = Transport::getTransport(portName);
	Acq2xx card(t);
	DataStreamer* dataStreamer = DataStreamer::create(
				card, AcqType::getAcqType(card));
	compute_cal(card);
	setup(card);
	dataStreamer->addFrameHandler(this);
/*
	dataStreamer->addFrameHandler(
				DataStreamer::createMeanHandler(
					AcqType::getAcqType(card), 1));
	dataStreamer->addFrameHandler(
				DataStreamer::createNewlineHandler());
*/
	dataStreamer->streamData();
}


int acq164AsynPortDriver::factory(const char *portName, int maxPoints, int nchan)
{
	new Acq164Device(portName, maxPoints, nchan);
	return(asynSuccess);
}

/* Configuration routine.  Called directly, or from the iocsh function below */

extern "C" {

/** EPICS iocsh callable function to call constructor for the testAsynPortDriver class.
  * \param[in] portName The name of the asyn port driver to be created.
  * \param[in] maxPoints The maximum  number of points in the volt and time arrays */
int acq164AsynPortDriverConfigure(const char *portName, int maxPoints, int nchan)
{
	return acq164AsynPortDriver::factory(portName, maxPoints, nchan);
}


/* EPICS iocsh shell commands */

static const iocshArg initArg0 = { "portName",iocshArgString};
static const iocshArg initArg1 = { "max points",iocshArgInt};
static const iocshArg initArg2 = { "max chan",iocshArgInt};
static const iocshArg * const initArgs[] = {&initArg0, &initArg1, &initArg2};
static const iocshFuncDef initFuncDef = {"acq164AsynPortDriverConfigure",3,initArgs};
static void initCallFunc(const iocshArgBuf *args)
{
	acq164AsynPortDriverConfigure(args[0].sval, args[1].ival, args[2].ival);
}

void acq164AsynPortDriverRegister(void)
{
    iocshRegister(&initFuncDef,initCallFunc);
}

epicsExportRegistrar(acq164AsynPortDriverRegister);

}

