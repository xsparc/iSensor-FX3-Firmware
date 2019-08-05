/**
  * Copyright (c) Analog Devices Inc, 2018 - 2019
  * All Rights Reserved.
  * 
  * THIS SOFTWARE UTILIZES LIBRARIES DEVELOPED
  * AND MAINTAINED BY CYPRESS INC. THE LICENSE INCLUDED IN
  * THIS REPOSITORY DOES NOT EXTEND TO CYPRESS PROPERTY.
  * 
  * Use of this file is governed by the license agreement
  * included in this repository.
  * 
  * @file		PinFunctions.c
  * @date		8/1/2019
  * @author		A. Nolan (alex.nolan@analog.com)
  * @author 	J. Chong (juan.chong@analog.com)
  * @brief
 **/

#include "PinFunctions.h"

/* Tell the compiler where to find the needed globals */
extern uint8_t USBBuffer[4096];
extern uint8_t BulkBuffer[12288];
extern CyU3PDmaBuffer_t ManualDMABuffer;
extern BoardState FX3State;
extern CyU3PDmaChannel ChannelToPC;
extern CyU3PEvent GpioHandler;

/*
 * Function: AdiMeasureBusyPulse(uint16_t transferLength)
 *
 * Sets a user configurable trigger condition and then measures the following GPIO pulse.
 * This function is approx. microsecond accurate.
 *
 * transferLength: The amount of data (in bytes) to read from the USB buffer
 *
 * Returns: A status code indicating the success of the measure pulse operation
 */
CyU3PReturnStatus_t AdiMeasureBusyPulse(uint16_t transferLength)
{
	CyU3PReturnStatus_t status = CY_U3P_SUCCESS;
	uint16_t *bytesRead = 0;
	uint16_t busyPin, triggerPin;
	CyBool_t pinValue, exitCondition, SpiTriggerMode, validPin, busyPolarity, triggerPolarity;
	uint32_t currentTime, lastTime, timeout, rollOverCount, driveTime;
	CyU3PGpioSimpleConfig_t gpioConfig;

	//Read config data into USBBuffer
	CyU3PUsbGetEP0Data(transferLength, USBBuffer, bytesRead);

	//Parse general request data from USBBuffer
	busyPin = USBBuffer[0];
	busyPin = busyPin + (USBBuffer[1] << 8);
	busyPolarity = (CyBool_t) USBBuffer[2];
	timeout = USBBuffer[3];
	timeout = timeout + (USBBuffer[4] << 8);
	timeout = timeout + (USBBuffer[5] << 16);
	timeout = timeout + (USBBuffer[6] << 24);

	//Get the trigger mode
	SpiTriggerMode = USBBuffer[7];

	//Convert ms to timer ticks
	timeout = timeout * MS_TO_TICKS_MULT;

	//Check that busy pin specified is configured as input
	status = CyU3PGpioSimpleGetValue(busyPin, &pinValue);
	validPin = CyTrue;
	if(status != CY_U3P_SUCCESS)
	{
		//If initial pin read fails try and configure as input
		gpioConfig.outValue = CyFalse;
		gpioConfig.inputEn = CyTrue;
		gpioConfig.driveLowEn = CyFalse;
		gpioConfig.driveHighEn = CyFalse;
		gpioConfig.intrMode = CY_U3P_GPIO_NO_INTR;
		status = CyU3PGpioSetSimpleConfig(busyPin, &gpioConfig);

		//get the pin value again after configuring
		status = CyU3PGpioSimpleGetValue(busyPin, &pinValue);

		//If pin setup not successful skip wait operation and return -1
		if(status != CY_U3P_SUCCESS)
		{
			validPin = CyFalse;
		}
	}

	//Set default for trigger pin to invalid value
	triggerPin = 0xFFFF;

	//Only perform pulse wait if the pin is able to act as an input
	if(validPin)
	{
		//parse the trigger specific data and trigger
		if(SpiTriggerMode)
		{
			uint16_t RegAddr, RegValue;
			uint8_t spiBuf[2];

			//parse register address
			RegAddr = USBBuffer[8];
			RegAddr = RegAddr + (USBBuffer[9] << 8);

			//parse register write value
			RegValue = USBBuffer[10];
			RegValue = RegValue + (USBBuffer[11] << 8);

			//transmit addr and least significant byte of SpiValue
			spiBuf[0] = RegAddr | 0x80;
			spiBuf[1] = RegValue & 0xFF;
			CyU3PSpiTransmitWords(spiBuf, 2);

			//wait for stall
			AdiSleepForMicroSeconds(FX3State.StallTime);

			//transmit (addr + 1) and most significant byte of SpiValue
			spiBuf[0] = (RegAddr + 1) | 0x80;
			spiBuf[1] = (RegValue & 0xFF00) >> 8;
			CyU3PSpiTransmitWords(spiBuf, 2);
		}
		else
		{
			//parse parameters from USB Buffer
			triggerPin = USBBuffer[8];
			triggerPin = triggerPin + (USBBuffer[9] << 8);

			triggerPolarity = USBBuffer[10];

			driveTime = USBBuffer[11];
			driveTime = driveTime + (USBBuffer[12] << 8);
			driveTime = driveTime + (USBBuffer[13] << 16);
			driveTime = driveTime + (USBBuffer[14] << 24);

			//convert drive time (ms) to ticks
			driveTime = driveTime * MS_TO_TICKS_MULT;

			//want to configure the trigger pin to act as an output
			status = CyU3PDeviceGpioOverride(triggerPin, CyTrue);
			if(status != CY_U3P_SUCCESS)
			{
				CyU3PDebugPrint (4, "Error! GPIO override to exit PWM mode failed, error code: 0x%x\r\n", status);
				return status;
			}

			//Disable the GPIO
			status = CyU3PGpioDisable(triggerPin);
			if(status != CY_U3P_SUCCESS)
			{
				CyU3PDebugPrint (4, "Error! Pin disable while exiting PWM mode failed, error code: 0x%x\r\n", status);
			}

			//Reset the pin timer register to 0
			GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].timer = 0;
			//Disable interrupts on the timer pin
			GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].status &= ~(CY_U3P_LPP_GPIO_INTRMODE_MASK);
			//Set the pin timer period to 0xFFFFFFFF;
			GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].period = 0xFFFFFFFF;

			//Configure the pin to act as an output and drive
			gpioConfig.outValue = triggerPolarity;
			gpioConfig.inputEn = CyFalse;
			gpioConfig.driveLowEn = CyTrue;
			gpioConfig.driveHighEn = CyTrue;
			gpioConfig.intrMode = CY_U3P_GPIO_NO_INTR;
			status = CyU3PGpioSetSimpleConfig(triggerPin, &gpioConfig);
		    if (status != CY_U3P_SUCCESS)
		    {
		    	CyU3PDebugPrint (4, "Error! GPIO config to exit PWM mode failed, error code: 0x%x\r\n", status);
		    }
		}

		//Wait until the busy pin reaches the selected polarity
		while(((GPIO->lpp_gpio_simple[busyPin] & CY_U3P_LPP_GPIO_IN_VALUE) >> 1) != busyPolarity);

		//In pin mode subtract the wait time from the total drive time
		if(!SpiTriggerMode)
		{
			//Set the pin config for sample now mode
			GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].status = (FX3State.TimerPinConfig | (CY_U3P_GPIO_MODE_SAMPLE_NOW << CY_U3P_LPP_GPIO_MODE_POS));
			//wait for sample to finish
			while (GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].status & CY_U3P_LPP_GPIO_MODE_MASK);
			//read timer value
			driveTime = driveTime - GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].threshold;
		}

		//Reset the pin timer register to 0
		GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].timer = 0;
		//Disable interrupts on the timer pin
		GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].status &= ~(CY_U3P_LPP_GPIO_INTRMODE_MASK);
		//Set the pin timer period to 0xFFFFFFFF;
		GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].period = 0xFFFFFFFF;

		//Reset state variables
		currentTime = 0;
		rollOverCount = 0;
		lastTime = 0;
		exitCondition = CyFalse;

		//Wait for the GPIO pin the reach the desired level or timeout
		while(!exitCondition)
		{
			//Store previous time
			lastTime = currentTime;

			//Set the pin config for sample now mode
			GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].status = (FX3State.TimerPinConfig | (CY_U3P_GPIO_MODE_SAMPLE_NOW << CY_U3P_LPP_GPIO_MODE_POS));
			//wait for sample to finish
			while (GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].status & CY_U3P_LPP_GPIO_MODE_MASK);
			//read timer value
			currentTime = GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].threshold;

			//Read the pin value
			pinValue = ((GPIO->lpp_gpio_simple[busyPin] & CY_U3P_LPP_GPIO_IN_VALUE) >> 1);

			//Check if rollover occured
			if(currentTime < lastTime)
			{
				rollOverCount++;
			}

			//update the exit condition
			if(timeout)
			{
				exitCondition = ((pinValue != busyPolarity) || (currentTime >= timeout));
			}
			else
			{
				exitCondition = (pinValue != busyPolarity);
			}

			//Check if the pin drive can stop
			if(!SpiTriggerMode)
			{
				if(currentTime > driveTime)
				{
					//drive the opposite polarity
					CyU3PGpioSimpleSetValue(triggerPin, ~triggerPolarity);
				}
				//Set trigger mode to true to prevent this loop from hitting again
				SpiTriggerMode = CyTrue;
			}
		}

		//add 2us to current time (fudge factor, calibrated using DSLogic Pro)
		if(currentTime < (0xFFFFFFFF - 20))
		{
			currentTime = currentTime + 20;
		}
		else
		{
			currentTime = 0;
			rollOverCount++;
		}
	}
	else
	{
		//Case where pin could not be configured as input
		currentTime = 0xFFFFFFFF;
	}

	//Reset trigger pin to input if needed
	if(triggerPin != 0xFFFF)
	{
		CyU3PGpioDisable(triggerPin);
		gpioConfig.outValue = CyFalse;
		gpioConfig.inputEn = CyTrue;
		gpioConfig.driveLowEn = CyFalse;
		gpioConfig.driveHighEn = CyFalse;
		gpioConfig.intrMode = CY_U3P_GPIO_NO_INTR;
		status = CyU3PGpioSetSimpleConfig(triggerPin, &gpioConfig);
	}

	//Return pulse wait data over ChannelToPC
	BulkBuffer[0] = status & 0xFF;
	BulkBuffer[1] = (status & 0xFF00) >> 8;
	BulkBuffer[2] = (status & 0xFF0000) >> 16;
	BulkBuffer[3] = (status & 0xFF000000) >> 24;
	BulkBuffer[4] = currentTime & 0xFF;
	BulkBuffer[5] = (currentTime & 0xFF00) >> 8;
	BulkBuffer[6] = (currentTime & 0xFF0000) >> 16;
	BulkBuffer[7] = (currentTime & 0xFF000000) >> 24;
	BulkBuffer[8] = rollOverCount & 0xFF;
	BulkBuffer[9] = (rollOverCount & 0xFF00) >> 8;
	BulkBuffer[10] = (rollOverCount & 0xFF0000) >> 16;
	BulkBuffer[11] = (rollOverCount & 0xFF000000) >> 24;
	BulkBuffer[12] = MS_TO_TICKS_MULT & 0xFF;
	BulkBuffer[13] = (MS_TO_TICKS_MULT & 0xFF00) >> 8;
	BulkBuffer[14] = (MS_TO_TICKS_MULT & 0xFF0000) >> 16;
	BulkBuffer[15] = (MS_TO_TICKS_MULT & 0xFF000000) >> 24;

	ManualDMABuffer.buffer = BulkBuffer;
	ManualDMABuffer.size = sizeof(BulkBuffer);
	ManualDMABuffer.count = 16;

	//Send the data to PC
	CyU3PDmaChannelSetupSendBuffer(&ChannelToPC, &ManualDMABuffer);

	return status;
}

/*
 * Function: AdiConfigurePWM(CyBool_t EnablePWM)
 *
 * This function configures the FX3 PWM options. The pin number, threshold value,
 * and period are provided in the USBBuffer, and are calculated in the FX3Api.
 *
 * enablePWM: If the PWM should be enabled or disabled.
 *
 * Returns: status
 */
CyU3PReturnStatus_t AdiConfigurePWM(CyBool_t EnablePWM)
{
	CyU3PReturnStatus_t status = CY_U3P_SUCCESS;
	uint16_t pinNumber;
	uint32_t threshold, period;

	//Get the pin number
	pinNumber = USBBuffer[0];
	pinNumber |= (USBBuffer[1] << 8);

	if(EnablePWM)
	{
		//get the period
		period = USBBuffer[2];
		period |= (USBBuffer[3] << 8);
		period |= (USBBuffer[4] << 16);
		period |= (USBBuffer[5] << 24);

		//get the threshold
		threshold = USBBuffer[6];
		threshold |= (USBBuffer[7] << 8);
		threshold |= (USBBuffer[8] << 16);
		threshold |= (USBBuffer[9] << 24);

#ifdef VERBOSE_MODE
		CyU3PDebugPrint (4, "Setting up PWM with period %d, threshold %d, for pin %d\r\n", period, threshold, pinNumber);
#endif

		//Override the selected pin to run as a complex GPIO
		status = CyU3PDeviceGpioOverride(pinNumber, CyFalse);
		if(status != CY_U3P_SUCCESS)
		{
			CyU3PDebugPrint (4, "Error! GPIO override for PWM mode failed, error code: 0x%s\r\n", status);
			return status;
		}

		//configure the selected pin in PWM mode
		CyU3PGpioComplexConfig_t gpioComplexConfig;
		CyU3PMemSet ((uint8_t *)&gpioComplexConfig, 0, sizeof (gpioComplexConfig));
		gpioComplexConfig.outValue = CyFalse;
		gpioComplexConfig.inputEn = CyFalse;
		gpioComplexConfig.driveLowEn = CyTrue;
		gpioComplexConfig.driveHighEn = CyTrue;
		gpioComplexConfig.pinMode = CY_U3P_GPIO_MODE_PWM;
		gpioComplexConfig.intrMode = CY_U3P_GPIO_NO_INTR;
		gpioComplexConfig.timerMode = CY_U3P_GPIO_TIMER_HIGH_FREQ;
		gpioComplexConfig.timer = 0;
		gpioComplexConfig.period = period;
		gpioComplexConfig.threshold = threshold;
		status = CyU3PGpioSetComplexConfig(pinNumber, &gpioComplexConfig);
	    if (status != CY_U3P_SUCCESS)
	    {
	    	CyU3PDebugPrint (4, "Error! GPIO config for PWM mode failed, error code: 0x%s\r\n", status);
	    	return status;
	    }
	}
	else
	{
		//want to reset the specified pin to simple state without output driven
		status = CyU3PDeviceGpioOverride(pinNumber, CyTrue);
		if(status != CY_U3P_SUCCESS)
		{
			CyU3PDebugPrint (4, "Error! GPIO override to exit PWM mode failed, error code: 0x%s\r\n", status);
			return status;
		}

		//Disable the GPIO
		status = CyU3PGpioDisable(pinNumber);
		if(status != CY_U3P_SUCCESS)
		{
			CyU3PDebugPrint (4, "Error! Pin disable while exiting PWM mode failed, error code: 0x%s\r\n", status);
			return status;
		}

		/* Set the GPIO configuration for each GPIO that was just overridden */
		CyU3PGpioSimpleConfig_t gpioConfig;
		CyU3PMemSet ((uint8_t *)&gpioConfig, 0, sizeof (gpioConfig));
		gpioConfig.outValue = CyFalse;
		gpioConfig.inputEn = CyTrue;
		gpioConfig.driveLowEn = CyFalse;
		gpioConfig.driveHighEn = CyFalse;
		gpioConfig.intrMode = CY_U3P_GPIO_NO_INTR;

		status = CyU3PGpioSetSimpleConfig(pinNumber, &gpioConfig);
	    if (status != CY_U3P_SUCCESS)
	    {
	    	CyU3PDebugPrint (4, "Error! GPIO config to exit PWM mode failed, error code: 0x%s\r\n", status);
	    	return status;
	    }
	}
	return status;
}

/*
 * Function: AdiPulseDrive()
 *
 * This function drives a GPIO pin for a specified number of milliseconds. If the
 * selected GPIO pin is not configured as an output, this function configures the pin.
 *
 * pin: The GPIO pin number to drive
 *
 * polarity: The polarity of the pin (True - High, False - Low)
 *
 * driveTime: The number of milliseconds to drive the pin for
 *
 * Returns: The status of the pin drive operation
 */
CyU3PReturnStatus_t AdiPulseDrive()
{
	CyU3PReturnStatus_t status = CY_U3P_SUCCESS;
	uint16_t pinNumber;
	CyBool_t polarity, exit;
	uint32_t timerTicks, timerRollovers, rolloverCount, currentTime, lastTime;

	//Parse request data from USBBuffer
	pinNumber = USBBuffer[0];
	pinNumber = pinNumber + (USBBuffer[1] << 8);
	polarity = (CyBool_t) USBBuffer[2];
	timerTicks = USBBuffer[3];
	timerTicks = timerTicks + (USBBuffer[4] << 8);
	timerTicks = timerTicks + (USBBuffer[5] << 16);
	timerTicks = timerTicks + (USBBuffer[6] << 24);
	timerRollovers = USBBuffer[7];
	timerRollovers = timerRollovers + (USBBuffer[8] << 8);
	timerRollovers = timerRollovers + (USBBuffer[9] << 16);
	timerRollovers = timerRollovers + (USBBuffer[10] << 24);

	//Configure the GPIO pin as a driven output
	CyU3PGpioSimpleConfig_t gpioConfig;
	gpioConfig.outValue = polarity;
	gpioConfig.inputEn = CyFalse;
	gpioConfig.driveLowEn = CyTrue;
	gpioConfig.driveHighEn = CyTrue;
	gpioConfig.intrMode = CY_U3P_GPIO_NO_INTR;
	status = CyU3PGpioSetSimpleConfig(pinNumber, &gpioConfig);

	//Reset the pin timer register to 0
	GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].timer = 0;
	//Disable interrupts on the timer pin
	GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].status &= ~(CY_U3P_LPP_GPIO_INTRMODE_MASK);
	//Set the pin timer period to 0xFFFFFFFF;
	GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].period = 0xFFFFFFFF;

	//If config fails try to disable and reconfigure
	if(status != CY_U3P_SUCCESS)
	{
		CyU3PDeviceGpioOverride(pinNumber, CyTrue);
		CyU3PGpioDisable(pinNumber);
		status = CyU3PGpioSetSimpleConfig(pinNumber, &gpioConfig);
		if(status != CY_U3P_SUCCESS)
		{
			CyU3PDebugPrint (4, "Error! Unable to configure selected pin as output, status error: 0x%x\r\n", status);
			return status;
		}
	}

	exit = CyFalse;
	rolloverCount = 0;
	currentTime = 0;
	lastTime = 0;
	while(!exit)
	{
		//Store previous time
		lastTime = currentTime;

		//Set the pin config for sample now mode
		GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].status = (FX3State.TimerPinConfig | (CY_U3P_GPIO_MODE_SAMPLE_NOW << CY_U3P_LPP_GPIO_MODE_POS));
		//wait for sample to finish
		while (GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].status & CY_U3P_LPP_GPIO_MODE_MASK);
		//read timer value
		currentTime = GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].threshold;

		//Check if rollover occured
		if(currentTime < lastTime)
		{
			rolloverCount++;
		}

		exit = (currentTime >= timerTicks) && (rolloverCount >= timerRollovers);
	}

	//set the pin to opposite polarity
	CyU3PGpioSetValue(pinNumber, !polarity);

	//configure the selected pin as input and tristate
	CyU3PDeviceGpioOverride(pinNumber, CyTrue);

	//Disable the GPIO
	CyU3PGpioDisable(pinNumber);

	gpioConfig.outValue = CyFalse;
	gpioConfig.inputEn = CyTrue;
	gpioConfig.driveLowEn = CyFalse;
	gpioConfig.driveHighEn = CyFalse;
	gpioConfig.intrMode = CY_U3P_GPIO_NO_INTR;

	status = CyU3PGpioSetSimpleConfig(pinNumber, &gpioConfig);

	//return the status
	return status;
}

/*
 * Function: AdiPulseWait(uint16_t transferLength)
 *
 * This function waits for a pin to reach a selected logic level. The PulseWait parameters are
 * passed in the USB buffer.
 *
 * pin is the GPIO pin number to poll
 *
 * polarity is the pin polarity which will trigger an exit condition
 *
 * delay is the wait time (in ms) from when the function starts before pin polling starts
 *
 * timeout is the time (in ms) to wait for the pin level before exiting
 */
CyU3PReturnStatus_t AdiPulseWait(uint16_t transferLength)
{
	CyU3PReturnStatus_t status = CY_U3P_SUCCESS;
	uint16_t pin;
    uint16_t *bytesRead = 0;
	CyBool_t polarity, validPin, pinValue, exitCondition;
	uint32_t currentTime, lastTime, delay, timeoutTicks, timeoutRollover, rollOverCount;

	//Disable interrupts on the timer pin
	GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].status &= ~(CY_U3P_LPP_GPIO_INTRMODE_MASK);
	//Set the pin timer period to 0xFFFFFFFF;
	GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].period = 0xFFFFFFFF;
	//Reset the pin timer register to 0
	GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].timer = 0;

	//Read config data into USBBuffer
	CyU3PUsbGetEP0Data(transferLength, USBBuffer, bytesRead);

	//Parse request data from USBBuffer
	pin = USBBuffer[0];
	pin = pin + (USBBuffer[1] << 8);
	polarity = (CyBool_t) USBBuffer[2];
	delay = USBBuffer[3];
	delay = delay + (USBBuffer[4] << 8);
	delay = delay + (USBBuffer[5] << 16);
	delay = delay + (USBBuffer[6] << 24);
	timeoutTicks = USBBuffer[7];
	timeoutTicks = timeoutTicks + (USBBuffer[8] << 8);
	timeoutTicks = timeoutTicks + (USBBuffer[9] << 16);
	timeoutTicks = timeoutTicks + (USBBuffer[10] << 24);
	timeoutRollover = USBBuffer[11];
	timeoutRollover = timeoutRollover + (USBBuffer[12] << 8);
	timeoutRollover = timeoutRollover + (USBBuffer[13] << 16);
	timeoutRollover = timeoutRollover + (USBBuffer[14] << 24);

	//Convert ms to timer ticks
	delay = delay * MS_TO_TICKS_MULT;

	//Check that input pin specified is configured as input
	status = CyU3PGpioSimpleGetValue(pin, &pinValue);
	validPin = CyTrue;
	if(status != CY_U3P_SUCCESS)
	{
		//If initial pin read fails try and configure as input
		CyU3PGpioSimpleConfig_t gpioConfig;
		gpioConfig.outValue = CyFalse;
		gpioConfig.inputEn = CyTrue;
		gpioConfig.driveLowEn = CyFalse;
		gpioConfig.driveHighEn = CyFalse;
		gpioConfig.intrMode = CY_U3P_GPIO_NO_INTR;
		status = CyU3PGpioSetSimpleConfig(pin, &gpioConfig);

		//get the pin value again after configuring
		status = CyU3PGpioSimpleGetValue(pin, &pinValue);

		//If pin setup not successful skip wait operation and return -1
		if(status != CY_U3P_SUCCESS)
		{
			validPin = CyFalse;
		}
	}

	//Only perform pulse wait if the pin is able to act as an input
	if(validPin)
	{
		//Wait for the delay, if needed
		currentTime = 0;
		rollOverCount = 0;
		lastTime = 0;
		exitCondition = CyFalse;
		if(delay > 0)
		{
			while(currentTime < delay)
			{
				//Set the pin config for sample now mode
				GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].status = (FX3State.TimerPinConfig | (CY_U3P_GPIO_MODE_SAMPLE_NOW << CY_U3P_LPP_GPIO_MODE_POS));
				//wait for sample to finish
				while (GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].status & CY_U3P_LPP_GPIO_MODE_MASK);
				//read timer value
				currentTime = GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].threshold;
			}
		}

		//Wait for the GPIO pin the reach the desired level or timeout
		while(!exitCondition)
		{
			//Store previous time
			lastTime = currentTime;

			//Set the pin config for sample now mode
			GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].status = (FX3State.TimerPinConfig | (CY_U3P_GPIO_MODE_SAMPLE_NOW << CY_U3P_LPP_GPIO_MODE_POS));
			//wait for sample to finish
			while (GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].status & CY_U3P_LPP_GPIO_MODE_MASK);
			//read timer value
			currentTime = GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].threshold;

			//Check if rollover occured
			if(currentTime < lastTime)
			{
				rollOverCount++;
			}

			//Read the pin value
			pinValue = ((GPIO->lpp_gpio_simple[pin] & CY_U3P_LPP_GPIO_IN_VALUE) >> 1);

			//update the exit condition (will always have valid timeout)
			//exits when pin reaches the desired polarity or timer reaches timeout
			exitCondition = ((pinValue == polarity) || ((currentTime >= timeoutTicks) && (rollOverCount >= timeoutRollover)));
		}

	}
	else
	{
		//Case where pin could not be configured as input
		currentTime = 0xFFFFFFFF;
	}

	//Return pulse wait data over ChannelToPC
	BulkBuffer[0] = status & 0xFF;
	BulkBuffer[1] = (status & 0xFF00) >> 8;
	BulkBuffer[2] = (status & 0xFF0000) >> 16;
	BulkBuffer[3] = (status & 0xFF000000) >> 24;
	BulkBuffer[4] = currentTime & 0xFF;
	BulkBuffer[5] = (currentTime & 0xFF00) >> 8;
	BulkBuffer[6] = (currentTime & 0xFF0000) >> 16;
	BulkBuffer[7] = (currentTime & 0xFF000000) >> 24;
	BulkBuffer[8] = rollOverCount & 0xFF;
	BulkBuffer[9] = (rollOverCount & 0xFF00) >> 8;
	BulkBuffer[10] = (rollOverCount & 0xFF0000) >> 16;
	BulkBuffer[11] = (rollOverCount & 0xFF000000) >> 24;

	ManualDMABuffer.buffer = BulkBuffer;
	ManualDMABuffer.size = sizeof(BulkBuffer);
	ManualDMABuffer.count = 12;

	//Send the data to PC
	CyU3PDmaChannelSetupSendBuffer(&ChannelToPC, &ManualDMABuffer);

	return status;
}


/*
 * Function: AdiSetPin(uint16_t pinNumber, CyBool_t polarity)
 *
 * This function configures the specified pin as an output and sets the value
 *
 * pinNumber: The GPIO index of the pin to be set
 *
 * polarity: The polarity of the pin to be set (True - High, False - Low)
 *
 * Returns: The status of the operation
 */
CyU3PReturnStatus_t AdiSetPin(uint16_t pinNumber, CyBool_t polarity)
{
	CyU3PReturnStatus_t status = CY_U3P_SUCCESS;
	//Configure pin as output and set the drive value
	CyU3PGpioSimpleConfig_t gpioConfig;
	gpioConfig.outValue = polarity;
	gpioConfig.inputEn = CyFalse;
	gpioConfig.driveLowEn = CyTrue;
	gpioConfig.driveHighEn = CyTrue;
	gpioConfig.intrMode = CY_U3P_GPIO_NO_INTR;
	status = CyU3PGpioSetSimpleConfig(pinNumber, &gpioConfig);
	return status;
}

/*
 * Function: AdiSleepForMicroSeconds(uint32_t numTicks)
 *
 * This function blocks thread execution for a specified number of timer ticks.
 * It uses a complex GPIO timer which is based on the system clock.
 *
 * numTicks: The number of timer ticks to wait for.
 *
 * Returns: status
 */
CyU3PReturnStatus_t AdiSleepForMicroSeconds(uint32_t numMicroSeconds)
{
	CyU3PReturnStatus_t status = CY_U3P_SUCCESS;
	uint32_t finalTickCount, currentTime;

	//reset the timer register first to reduce overhead
	GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].timer = 0;

	//Offset the timer value
	if(numMicroSeconds > ADI_MICROSECONDS_SLEEP_OFFSET)
	{
		numMicroSeconds = numMicroSeconds - ADI_MICROSECONDS_SLEEP_OFFSET;
	}
	else
	{
		return status;
	}

	//Check if sleep is too long (would overflow on multiply) and return if needed
	//Use the CyU3PThreadSleep() if longer sleep is needed
	if(numMicroSeconds > 426172)
	{
		CyU3PDebugPrint (4, "ERROR: Sleep of %d microseconds not achievable with AdiSleepForMicroseconds, use system sleep call!\r\n", numMicroSeconds);
		return CY_U3P_ERROR_BAD_ARGUMENT;
	}

	//calculate final tick count using MS multiplier
	finalTickCount = numMicroSeconds * MS_TO_TICKS_MULT;

	//scale back to microseconds
	finalTickCount = finalTickCount / 1000;

	currentTime = 0;
	while(currentTime < finalTickCount)
	{
		//Set the pin config for sample now mode
		GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].status = (FX3State.TimerPinConfig | (CY_U3P_GPIO_MODE_SAMPLE_NOW << CY_U3P_LPP_GPIO_MODE_POS));
		//wait for sample to finish
		while (GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].status & CY_U3P_LPP_GPIO_MODE_MASK);
		//read timer value
		currentTime = GPIO->lpp_gpio_pin[ADI_TIMER_PIN_INDEX].threshold;
	}
	return status;
}

/*
 * Function: AdiWaitForPin(uint32_t pinNumber, PinWaitType waitType)
 *
 * This function blocks the execution of the current thread until an event happens on the
 * specified GPIO pin.
 *
 * pinNumber: The GPIO pin number to poll
 *
 * waitType: The event type to wait for, as a PinWaitType
 *
 * Returns: void
 */
CyU3PReturnStatus_t AdiWaitForPin(uint32_t pinNumber, CyU3PGpioIntrMode_t interruptSetting, uint32_t timeoutTicks)
{
	CyU3PReturnStatus_t status = CY_U3P_SUCCESS;
	uint32_t gpioEventFlag;
	CyU3PGpioSimpleConfig_t gpioConfig;

	//Configure the specified pin as an input and attach the correct pin interrupt
	gpioConfig.outValue = CyTrue;
	gpioConfig.inputEn = CyTrue;
	gpioConfig.driveLowEn = CyFalse;
	gpioConfig.driveHighEn = CyFalse;
	gpioConfig.intrMode = interruptSetting;

	status = CyU3PGpioSetSimpleConfig(pinNumber, &gpioConfig);

	//Catch unspecified timeout
	if (timeoutTicks == 0)
	{
		timeoutTicks = CYU3P_WAIT_FOREVER;
	}

	if (status == CY_U3P_SUCCESS)
	{
		//Enable GPIO interrupts (in case it's not enabled)
		CyU3PVicEnableInt(CY_U3P_VIC_GPIO_CORE_VECTOR);
		//Wait for GPIO interrupt flag
		status = CyU3PEventGet(&GpioHandler, pinNumber, CYU3P_EVENT_OR_CLEAR, &gpioEventFlag, timeoutTicks);
		//Disable GPIO interrupts until we need them again
		CyU3PVicDisableInt(CY_U3P_VIC_GPIO_CORE_VECTOR);
	}
	return status;
}

/*
 * Function: AdiMStoTicks(uint32_t timeInMS)
 *
 * Converts milliseconds to number of ticks and adjusts the resulting offset if below the
 * measurable minimum.
 *
 * timeInMS: The real stall time (in ms) desired.
 */
uint32_t AdiMStoTicks(uint32_t timeInMS)
{
	return timeInMS * MS_TO_TICKS_MULT;
}

/*
 * Function: AdiPinRead(uint16_t pin)
 *
 * This function handles Pin read control end point requests. It reads the value of a specified
 * GPIO pin, and sends that value over the control endpoint, along with the pin read status.
 *
 * Returns: The success of the pin read operation
 */
CyU3PReturnStatus_t AdiPinRead(uint16_t pin)
{
	CyU3PReturnStatus_t status = CY_U3P_SUCCESS;
	CyBool_t pinValue = CyFalse;

	//get the pin value
	status = CyU3PGpioSimpleGetValue(pin, &pinValue);
	if(status != CY_U3P_SUCCESS)
	{
		//If the initial pin read fails reconfigure the pin as in input
		CyU3PGpioSimpleConfig_t gpioConfig;
		gpioConfig.outValue = CyFalse;
		gpioConfig.inputEn = CyTrue;
		gpioConfig.driveLowEn = CyFalse;
		gpioConfig.driveHighEn = CyFalse;
		gpioConfig.intrMode = CY_U3P_GPIO_NO_INTR;
		status = CyU3PGpioSetSimpleConfig(pin, &gpioConfig);
		//If the config is successful, read the pin value
		if(status == CY_U3P_SUCCESS)
		{
			status = CyU3PGpioSimpleGetValue(pin, &pinValue);
		}
	}

	//Put pin register value in output buffer
	USBBuffer[0] = pinValue;
	USBBuffer[1] = status & 0xFF;
	USBBuffer[2] = (status & 0xFF00) >> 8;
	USBBuffer[3] = (status & 0xFF0000) >> 16;
	USBBuffer[4] = (status & 0xFF000000) >> 24;
	//Send the pin value
	CyU3PUsbSendEP0Data (5, (uint8_t *)USBBuffer);
	//Send a packet terminate
	CyU3PUsbSendEP0Data (0, NULL);
	return status;
}

/*
 * Function: AdiReadTimerValue()
 *
 * This function handles Timer read control endpoint requests. It reads the current value from the
 * complex GPIO timer and then sends the value over the control endpoint.
 *
 * Returns: The success of the timer read operation
 */
CyU3PReturnStatus_t AdiReadTimerValue()
{
	CyU3PReturnStatus_t status = CY_U3P_SUCCESS;
	uint32_t timerValue;
	status = CyU3PGpioComplexSampleNow(ADI_TIMER_PIN, &timerValue);
	if(status != CY_U3P_SUCCESS)
	{
		return status;
	}
	USBBuffer[0] = timerValue & 0xFF;
	USBBuffer[1] = (timerValue & 0xFF00) >> 8;
	USBBuffer[2] = (timerValue & 0xFF0000) >> 16;
	USBBuffer[3] = (timerValue & 0xFF000000) >> 24;
	status = CyU3PUsbSendEP0Data (4, USBBuffer);
	return status;
}

/*
 * Function: AdiMeasureDR()
 *
 * This function measures two data ready pulses on a user-specified pin and reports
 * back the delta-time in ticks. The function also transmits the tick scale factor
 * and a timeout counter to notify the interface of timeouts that may have occurred
 * due to missing pulses. Data is transmitted over USB via the bulk endpoint. Inputs
 * are provided through the control endpoint. This function can be expanded to capture
 * as many samples as required.
 *
 * pin: The GPIO pin number to measure
 *
 * polarity: The polarity of the pin (1 - Low-to-High, 0 - High-to-Low)
 *
 * timeoutInMs: The specified timeout in milliseconds
 *
 * Returns: The status of the pin drive operation
 */
CyU3PReturnStatus_t AdiMeasureDR()
{
	CyU3PReturnStatus_t status = CY_U3P_SUCCESS;
	uint16_t pin;
	CyBool_t polarity, validPin, pinValue, sampleRollover;
	uint32_t timeWaited[2];
	uint32_t timeoutCounter = 0;
	uint32_t pinRegValue, startTime, currentTime, sampleEndTime, timeout, deltat;

	//Get the operation start time
	CyU3PGpioComplexSampleNow(ADI_TIMER_PIN, &startTime);
	currentTime = startTime;

	//Parse request data from USBBuffer
	pin = USBBuffer[0];
	pin = pin + (USBBuffer[1] << 8);
	polarity = (CyBool_t) USBBuffer[2];
	timeout = USBBuffer[7];
	timeout = timeout + (USBBuffer[8] << 8);
	timeout = timeout + (USBBuffer[9] << 16);
	timeout = timeout + (USBBuffer[10] << 24);

	//Convert from ms to timer ticks
	timeout = AdiMStoTicks(timeout);

	//Calculate if timer rollover is going to occur for sample period
	sampleRollover = (timeout > 0 && (startTime > (0xFFFFFFFF - timeout)));

	//Calculate the sample period end time
	if(sampleRollover)
	{
		sampleEndTime = timeout - (0xFFFFFFFF - startTime);
	}
	else
	{
		sampleEndTime = timeout + startTime;
	}

	//Check that input pin specified is configured as input
	status = CyU3PGpioSimpleGetValue(pin, &pinValue);
	if(status != CY_U3P_SUCCESS)
	{
		//If initial pin read fails try and configure as input
		CyU3PGpioSimpleConfig_t gpioConfig;
		gpioConfig.outValue = CyFalse;
		gpioConfig.inputEn = CyTrue;
		gpioConfig.driveLowEn = CyFalse;
		gpioConfig.driveHighEn = CyFalse;
		gpioConfig.intrMode = CY_U3P_GPIO_NO_INTR;
		status = CyU3PGpioSetSimpleConfig(pin, &gpioConfig);

		//get the pin value again after configuring
		status = CyU3PGpioSimpleGetValue(pin, &pinValue);

		//If pin setup not successful skip wait operation and return -1
		if(status != CY_U3P_SUCCESS)
		{
			validPin = CyFalse;
		}
	}
	else
	{
	validPin = CyTrue;
	}

	//If the pin is properly configured
	if(validPin)
	{
		//Loop through samples
		for(uint32_t i = 0; i <= 2; i++)
		{
			//Poll the input pin until a transition is detected or the timer runs out
			if(timeout)
			{
				//If the timeout will roll over
				if(sampleRollover)
				{
					//Low to high transition
					if(polarity == 1)
					{
						pinRegValue = GPIO->lpp_gpio_simple[pin];
						//If pin starts high wait for low transition
						if(pinRegValue & CY_U3P_LPP_GPIO_IN_VALUE)
						{
							while((pinRegValue & CY_U3P_LPP_GPIO_IN_VALUE) && (currentTime >= startTime || currentTime <= sampleEndTime))
							{
								CyU3PGpioComplexSampleNow(ADI_TIMER_PIN, &currentTime);
								pinRegValue = GPIO->lpp_gpio_simple[pin];
								//Increment timeout counter if a timeout occurs
								if(currentTime <= startTime || currentTime >= sampleEndTime)
								{
									timeoutCounter++;
								}
							}
						}
						//Wait for a low to high transition
						while((!(pinRegValue & CY_U3P_LPP_GPIO_IN_VALUE)) && (currentTime >= startTime || currentTime <= sampleEndTime))
						{
							CyU3PGpioComplexSampleNow(ADI_TIMER_PIN, &currentTime);
							pinRegValue = GPIO->lpp_gpio_simple[pin];
							//Increment timeout counter if a timeout occurs
							if(currentTime <= startTime || currentTime >= sampleEndTime)
							{
								timeoutCounter++;
							}
						}
					}
					//Default / high to low transition
					else
					{
						pinRegValue = GPIO->lpp_gpio_simple[pin];
						//If pin starts low wait for high transition
						if((!(pinRegValue & CY_U3P_LPP_GPIO_IN_VALUE)) && (currentTime >= startTime || currentTime <= sampleEndTime))
						{
							while(!(pinRegValue & CY_U3P_LPP_GPIO_IN_VALUE))
							{
								CyU3PGpioComplexSampleNow(ADI_TIMER_PIN, &currentTime);
								pinRegValue = GPIO->lpp_gpio_simple[pin];
								//Increment timeout counter if a timeout occurs
								if(currentTime <= startTime || currentTime >= sampleEndTime)
								{
									timeoutCounter++;
								}
							}
						}
						//Wait for a high to low transition
						while((pinRegValue & CY_U3P_LPP_GPIO_IN_VALUE) && (currentTime >= startTime || currentTime <= sampleEndTime))
						{
							CyU3PGpioComplexSampleNow(ADI_TIMER_PIN, &currentTime);
							pinRegValue = GPIO->lpp_gpio_simple[pin];
							//Increment timeout counter if a timeout occurs
							if(currentTime <= startTime || currentTime >= sampleEndTime)
							{
								timeoutCounter++;
							}
						}
					}
				}
				else
				{
					//Low to high transition
					if(polarity == 1)
					{
						pinRegValue = GPIO->lpp_gpio_simple[pin];
						//If pin starts high wait for low transition
						if(pinRegValue & CY_U3P_LPP_GPIO_IN_VALUE)
						{
							while((pinRegValue & CY_U3P_LPP_GPIO_IN_VALUE) && currentTime < sampleEndTime)
							{
								CyU3PGpioComplexSampleNow(ADI_TIMER_PIN, &currentTime);
								pinRegValue = GPIO->lpp_gpio_simple[pin];
								//Increment timeout counter if a timeout occurs
								if(currentTime > sampleEndTime)
								{
									timeoutCounter++;
								}
							}
						}
						//Wait for a low to high transition
						while((!(pinRegValue & CY_U3P_LPP_GPIO_IN_VALUE)) && currentTime < sampleEndTime)
						{
							CyU3PGpioComplexSampleNow(ADI_TIMER_PIN, &currentTime);
							pinRegValue = GPIO->lpp_gpio_simple[pin];
							//Increment timeout counter if a timeout occurs
							if(currentTime > sampleEndTime)
							{
								timeoutCounter++;
							}
						}
					}
					//Default / high to low transition
					else
					{
						pinRegValue = GPIO->lpp_gpio_simple[pin];
						//If pin starts low wait for high transition
						if((!(pinRegValue & CY_U3P_LPP_GPIO_IN_VALUE)) && currentTime < sampleEndTime)
						{
							while(!(pinRegValue & CY_U3P_LPP_GPIO_IN_VALUE))
							{
								CyU3PGpioComplexSampleNow(ADI_TIMER_PIN, &currentTime);
								pinRegValue = GPIO->lpp_gpio_simple[pin];
								//Increment timeout counter if a timeout occurs
								if(currentTime > sampleEndTime)
								{
									timeoutCounter++;
								}
							}
						}
						//Wait for a high to low transition
						while((pinRegValue & CY_U3P_LPP_GPIO_IN_VALUE) && currentTime < sampleEndTime)
						{
							CyU3PGpioComplexSampleNow(ADI_TIMER_PIN, &currentTime);
							pinRegValue = GPIO->lpp_gpio_simple[pin];
							//Increment timeout counter if a timeout occurs
							if(currentTime > sampleEndTime)
							{
								timeoutCounter++;
							}
						}
					}
				}
			}
			//If timeout is 0 treat it like there is no timeout
			else
			{
				//Low to high transition
				if(polarity == 1)
				{
					pinRegValue = GPIO->lpp_gpio_simple[pin];
					//If pin starts high wait for low transition
					if(pinRegValue & CY_U3P_LPP_GPIO_IN_VALUE)
					{
						while(pinRegValue & CY_U3P_LPP_GPIO_IN_VALUE)
						{
							pinRegValue = GPIO->lpp_gpio_simple[pin];
						}
					}
					//Wait for a low to high transition
					while(!(pinRegValue & CY_U3P_LPP_GPIO_IN_VALUE))
					{
						pinRegValue = GPIO->lpp_gpio_simple[pin];
					}
				}
				//Default / high to low transition
				else
				{
					pinRegValue = GPIO->lpp_gpio_simple[pin];
					//If pin starts low wait for high transition
					if(!(pinRegValue & CY_U3P_LPP_GPIO_IN_VALUE))
					{
						while(!(pinRegValue & CY_U3P_LPP_GPIO_IN_VALUE))
						{
							pinRegValue = GPIO->lpp_gpio_simple[pin];
						}
					}
					//Wait for a high to low transition
					while(pinRegValue & CY_U3P_LPP_GPIO_IN_VALUE)
					{
						pinRegValue = GPIO->lpp_gpio_simple[pin];
					}
				}
			}
			//Calculate the time waited
			CyU3PGpioComplexSampleNow(ADI_TIMER_PIN, &currentTime);
			if (currentTime > startTime)
			{
				timeWaited[i] = currentTime - startTime;
			}
			else
			{
				timeWaited[i] = currentTime + (0xFFFFFFFF - startTime);
			}
		}
		//Calculate delta time
		deltat = timeWaited[1] - timeWaited[0];
	}
	else
	{
		//Set the wait time to max when invalid pin is specified
		deltat = 0xFFFFFFFF;
	}
	//Return delta time over ChannelToPC
	//TODO: Stop sending MS_TO_TICKS_MULT since it's already
	//      part of the SPI config message
	BulkBuffer[0] = deltat & 0xFF;
	BulkBuffer[1] = (deltat & 0xFF00) >> 8;
	BulkBuffer[2] = (deltat & 0xFF0000) >> 16;
	BulkBuffer[3] = (deltat & 0xFF000000) >> 24;
	BulkBuffer[4] = MS_TO_TICKS_MULT & 0xFF;
	BulkBuffer[5] = (MS_TO_TICKS_MULT & 0xFF00) >> 8;
	BulkBuffer[6] = (MS_TO_TICKS_MULT & 0xFF00) >> 16;
	BulkBuffer[7] = (MS_TO_TICKS_MULT & 0xFF00) >> 24;
	BulkBuffer[8] = timeoutCounter;

	ManualDMABuffer.buffer = BulkBuffer;
	ManualDMABuffer.size = sizeof(BulkBuffer);
	ManualDMABuffer.count = 9;

	//Send the data to PC
	status = CyU3PDmaChannelSetupSendBuffer(&ChannelToPC, &ManualDMABuffer);
	if(status != CY_U3P_SUCCESS)
	{
		CyU3PDebugPrint (4, "Sending DR data to PC failed!, error code = 0x%x\r\n", status);
	}

	return status;
}