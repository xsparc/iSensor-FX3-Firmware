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
  * @file		AppThread.c
  * @date		8/1/2019
  * @author		A. Nolan (alex.nolan@analog.com)
  * @author 	J. Chong (juan.chong@analog.com)
  * @brief
 **/

#include "AppThread.h"

/* Tell the compiler where to find the needed globals */
extern CyU3PEvent EventHandler;
extern char serial_number[];

/*
 * Function: AdiDebugInit()
 *
 * This function initializes the UART controller to send debug messages.
 * The debug prints are routed to the UART and can be seen using a UART console
 * running at 115200 baud rate.
 *
 * Returns: Void
 */
void AdiDebugInit(void)
{
    CyU3PUartConfig_t uartConfig;
    CyU3PReturnStatus_t status = CY_U3P_SUCCESS;

    /* Initialize the UART for printing debug messages */
    status = CyU3PUartInit();
    if (status != CY_U3P_SUCCESS)
    {
        /* Error handling */
        AdiAppErrorHandler(status);
    }

    /* Set UART configuration */
    CyU3PMemSet ((uint8_t *)&uartConfig, 0, sizeof (uartConfig));
    uartConfig.baudRate = CY_U3P_UART_BAUDRATE_115200;
    uartConfig.stopBit = CY_U3P_UART_ONE_STOP_BIT;
    uartConfig.parity = CY_U3P_UART_NO_PARITY;
    uartConfig.txEnable = CyTrue;
    uartConfig.rxEnable = CyFalse;
    uartConfig.flowCtrl = CyFalse;
    uartConfig.isDma = CyTrue;

    /* Set the UART configuration */
    status = CyU3PUartSetConfig (&uartConfig, NULL);
    if (status != CY_U3P_SUCCESS)
    {
    	AdiAppErrorHandler(status);
    }

    /* Set the UART transfer to a really large value. */
    status = CyU3PUartTxSetBlockXfer (0xFFFFFFFF);
    if (status != CY_U3P_SUCCESS)
    {
    	AdiAppErrorHandler(status);
    }

    /* Initialize the debug module. */
    status = CyU3PDebugInit (CY_U3P_LPP_SOCKET_UART_CONS, 8);
    if (status != CY_U3P_SUCCESS)
    {
    	AdiAppErrorHandler(status);
    }

    /* Turn off the preamble to the debug messages. */
    CyU3PDebugPreamble(CyFalse);

    /* Send a success command over the newly-created debug port. */
    CyU3PDebugPrint (4, "\r\n");
    CyU3PDebugPrint (4, "Debugger successfully initialized!\r\n");
}

/* Function: AdiAppInit()
 *
 * This function initializes the USB module and attaches core event handlers.
 *
 * Returns: void
 */
void AdiAppInit (void)
{
    CyU3PReturnStatus_t status = CY_U3P_SUCCESS;

    /* Get USB serial number from FX3 die id */
    static uint32_t *EFUSE_DIE_ID = ((uint32_t *)0xE0055010);
    static const char hex_digit[16] = "0123456789ABCDEF";
    uint32_t die_id[2];

	/* Write FX3 die ID to USB serial number descriptor and a global variable */
	CyU3PReadDeviceRegisters(EFUSE_DIE_ID, 2, die_id);
	for (int i = 0; i < 2; i++)
	{
		//Access via the USB descriptor
		CyFxUSBSerialNumDesc[i*16+ 2] = hex_digit[(die_id[1-i] >> 28) & 0xF];
		CyFxUSBSerialNumDesc[i*16+ 4] = hex_digit[(die_id[1-i] >> 24) & 0xF];
		CyFxUSBSerialNumDesc[i*16+ 6] = hex_digit[(die_id[1-i] >> 20) & 0xF];
		CyFxUSBSerialNumDesc[i*16+ 8] = hex_digit[(die_id[1-i] >> 16) & 0xF];
		CyFxUSBSerialNumDesc[i*16+10] = hex_digit[(die_id[1-i] >> 12) & 0xF];
		CyFxUSBSerialNumDesc[i*16+12] = hex_digit[(die_id[1-i] >>  8) & 0xF];
		CyFxUSBSerialNumDesc[i*16+14] = hex_digit[(die_id[1-i] >>  4) & 0xF];
		CyFxUSBSerialNumDesc[i*16+16] = hex_digit[(die_id[1-i] >>  0) & 0xF];

		//Access via a vendor command
		serial_number[i*16+ 0] = hex_digit[(die_id[1-i] >> 28) & 0xF];
		serial_number[i*16+ 2] = hex_digit[(die_id[1-i] >> 24) & 0xF];
		serial_number[i*16+ 4] = hex_digit[(die_id[1-i] >> 20) & 0xF];
		serial_number[i*16+ 6] = hex_digit[(die_id[1-i] >> 16) & 0xF];
		serial_number[i*16+ 8] = hex_digit[(die_id[1-i] >> 12) & 0xF];
		serial_number[i*16+10] = hex_digit[(die_id[1-i] >>  8) & 0xF];
		serial_number[i*16+12] = hex_digit[(die_id[1-i] >>  4) & 0xF];
		serial_number[i*16+14] = hex_digit[(die_id[1-i] >>  0) & 0xF];
	}

	/* Start the USB functionality. */
    status = CyU3PUsbStart();
    if (status != CY_U3P_SUCCESS)
    {
    	CyU3PDebugPrint (4, "CyU3PUsbStart failed to Start, Error code = 0x%x\r\n", status);
        AdiAppErrorHandler(status);
    }
    else
    {
    	CyU3PDebugPrint (4, "USB OK\r\n");
    }

    /* The fast enumeration is the easiest way to setup a USB connection,
     * where all enumeration phase is handled by the library. Only the
     * class / vendor requests need to be handled by the application. */
    CyU3PUsbRegisterSetupCallback(AdiControlEndpointHandler, CyTrue);

    /* Setup the callback to handle the USB events */
    CyU3PUsbRegisterEventCallback(AdiUSBEventHandler);

    /* Register a callback to handle LPM requests from the USB host */
    CyU3PUsbRegisterLPMRequestCallback(AdiLPMRequestHandler);

    /* Set the USB Enumeration descriptors */

    /* Super speed device descriptor. */
    status = CyU3PUsbSetDesc(CY_U3P_USB_SET_SS_DEVICE_DESCR, 0, (uint8_t *)CyFxUSB30DeviceDscr);
    if (status != CY_U3P_SUCCESS)
    {
        CyU3PDebugPrint (4, "USB set device descriptor failed, Error code = 0x%x\r\n", status);
        AdiAppErrorHandler(status);
    }

    /* Full speed configuration descriptor */
    status = CyU3PUsbSetDesc(CY_U3P_USB_SET_FS_CONFIG_DESCR, 0, (uint8_t *)CyFxUSBFSConfigDscr);
    if (status != CY_U3P_SUCCESS)
    {
        CyU3PDebugPrint (4, "USB Set Configuration Descriptor failed, Error Code = 0x%x\r\n", status);
        AdiAppErrorHandler(status);
    }

    /* Super speed configuration descriptor */
    status = CyU3PUsbSetDesc(CY_U3P_USB_SET_SS_CONFIG_DESCR, 0, (uint8_t *)CyFxUSBSSConfigDscr);
    if (status != CY_U3P_SUCCESS)
    {
        CyU3PDebugPrint (4, "USB set configuration descriptor failed, Error code = 0x%x\r\n", status);
        AdiAppErrorHandler(status);
    }

    /* BOS descriptor */
    status = CyU3PUsbSetDesc(CY_U3P_USB_SET_SS_BOS_DESCR, 0, (uint8_t *)CyFxUSBBOSDscr);
    if (status != CY_U3P_SUCCESS)
    {
        CyU3PDebugPrint (4, "USB set configuration descriptor failed, Error code = 0x%x\r\n", status);
        AdiAppErrorHandler(status);
    }

    /* High speed device descriptor. */
    status = CyU3PUsbSetDesc(CY_U3P_USB_SET_HS_DEVICE_DESCR, 0, (uint8_t *)CyFxUSB20DeviceDscr);
    if (status != CY_U3P_SUCCESS)
    {
        CyU3PDebugPrint (4, "USB set device descriptor failed, Error code = 0x%x\r\n", status);
        AdiAppErrorHandler(status);
    }

    /* Device qualifier descriptor */
    status = CyU3PUsbSetDesc(CY_U3P_USB_SET_DEVQUAL_DESCR, 0, (uint8_t *)CyFxUSBDeviceQualDscr);
    if (status != CY_U3P_SUCCESS)
    {
        CyU3PDebugPrint (4, "USB set device qualifier descriptor failed, Error code = 0x%x\r\n", status);
        AdiAppErrorHandler(status);
    }

    /* High speed configuration descriptor */
    status = CyU3PUsbSetDesc(CY_U3P_USB_SET_HS_CONFIG_DESCR, 0, (uint8_t *)CyFxUSBHSConfigDscr);
    if (status != CY_U3P_SUCCESS)
    {
        CyU3PDebugPrint (4, "USB Set Other Speed Descriptor failed, Error Code = 0x%x\r\n", status);
        AdiAppErrorHandler(status);
    }

    /* String descriptor 0 */
    status = CyU3PUsbSetDesc(CY_U3P_USB_SET_STRING_DESCR, 0, (uint8_t *)CyFxUSBStringLangIDDscr);
    if (status != CY_U3P_SUCCESS)
    {
        CyU3PDebugPrint (4, "USB set string descriptor failed, Error code = 0x%x\r\n", status);
        AdiAppErrorHandler(status);
    }

    /* String descriptor 1 */
    status = CyU3PUsbSetDesc(CY_U3P_USB_SET_STRING_DESCR, 1, (uint8_t *)CyFxUSBManufactureDscr);
    if (status != CY_U3P_SUCCESS)
    {
        CyU3PDebugPrint (4, "USB set string descriptor failed, Error code = 0x%x\r\n", status);
        AdiAppErrorHandler(status);
    }

    /* String descriptor 2 */
    status = CyU3PUsbSetDesc(CY_U3P_USB_SET_STRING_DESCR, 2, (uint8_t *)CyFxUSBProductDscr);
    if (status != CY_U3P_SUCCESS)
    {
        CyU3PDebugPrint (4, "USB set string descriptor failed, Error code = 0x%x\r\n", status);
        AdiAppErrorHandler(status);
    }

    /* Serial number descriptor */
    status = CyU3PUsbSetDesc(CY_U3P_USB_SET_STRING_DESCR, 3, (uint8_t *)CyFxUSBSerialNumDesc);
    if (status != CY_U3P_SUCCESS)
    {
      CyU3PDebugPrint (4, "USB set serial number descriptor failed, Error code = 0x%x\r\n", status);
      AdiAppErrorHandler(status);
    }

    /* Connect the USB Pins with high speed operation enabled (USB 2.0 for better compatibility) */
    status = CyU3PConnectState (CyTrue, CyFalse);
    if (status != CY_U3P_SUCCESS)
    {
        CyU3PDebugPrint (4, "USB Connect failed, Error code = 0x%x\r\n", status);
        AdiAppErrorHandler(status);
    }
}

/*
 * Function AdiAppThreadEntry(uint32_t input)
 *
 * This is the entry point for the main application thread. It performs the device initialization
 * and then handles streaming start/stop commands for the various streaming methods.
 *
 * input: Unused input argument required by the thread manager
 *
 * Returns: void
 */
void AdiAppThreadEntry (uint32_t input)
{
    uint32_t eventMask =
    		ADI_RT_STREAMING_DONE|
    		ADI_RT_STREAMING_START|
    		ADI_RT_STREAMING_STOP|
    		ADI_GENERIC_STREAMING_DONE|
    		ADI_GENERIC_STREAMING_START|
    		ADI_GENERIC_STREAMING_STOP|
    		ADI_BURST_STREAMING_DONE|
    		ADI_BURST_STREAMING_START|
    		ADI_BURST_STREAMING_STOP;
    uint32_t eventFlag;

    /* Initialize UART debugging */
    AdiDebugInit ();

    /* Initialize the ADI application */
    AdiAppInit ();

    for (;;)
    {
    	//Wait for event handler flags to occur and handle them
    	if (CyU3PEventGet(&EventHandler, eventMask, CYU3P_EVENT_OR_CLEAR, &eventFlag, CYU3P_WAIT_FOREVER) == CY_U3P_SUCCESS)
    	{
			//Handle real-time stream commands
			if (eventFlag & ADI_RT_STREAMING_START)
			{
				AdiRealTimeStreamStart();
#ifdef VERBOSE_MODE
				CyU3PDebugPrint (4, "Real time stream start command received.\r\n");
#endif
			}
			if (eventFlag & ADI_RT_STREAMING_STOP)
			{
				AdiStopAnyDataStream();
#ifdef VERBOSE_MODE
				CyU3PDebugPrint (4, "Real time stream stop command received.\r\n");
#endif
			}
			if (eventFlag & ADI_RT_STREAMING_DONE)
			{
				AdiRealTimeStreamFinished();
#ifdef VERBOSE_MODE
				CyU3PDebugPrint (4, "Real time stream finished.\r\n");
#endif
			}

			//Handle generic data stream commands
			if (eventFlag & ADI_GENERIC_STREAMING_START)
			{
				AdiGenericStreamStart();
#ifdef VERBOSE_MODE
				CyU3PDebugPrint (4, "Generic stream start command received.\r\n");
#endif
			}
			if (eventFlag & ADI_GENERIC_STREAMING_STOP)
			{
				AdiStopAnyDataStream();
#ifdef VERBOSE_MODE
				CyU3PDebugPrint (4, "Stop generic stream command detected.\r\n");
#endif
			}
			if (eventFlag & ADI_GENERIC_STREAMING_DONE)
			{
				AdiGenericStreamFinished();
#ifdef VERBOSE_MODE
				CyU3PDebugPrint (4, "Generic data stream finished.\r\n");
#endif
			}

			//Handle burst data stream commands
			if (eventFlag & ADI_BURST_STREAMING_START)
			{
				AdiBurstStreamStart();
#ifdef VERBOSE_MODE
				CyU3PDebugPrint (4, "Burst stream start command received.\r\n");
#endif
			}
			if (eventFlag & ADI_BURST_STREAMING_STOP)
			{
				AdiStopAnyDataStream();
#ifdef VERBOSE_MODE
				CyU3PDebugPrint (4, "Stop burst stream command detected.\r\n");
#endif
			}
			if (eventFlag & ADI_BURST_STREAMING_DONE)
			{
				AdiBurstStreamFinished();
#ifdef VERBOSE_MODE
				CyU3PDebugPrint (4, "Burst data stream finished.\r\n");
#endif
			}

    	}
        /* Allow other ready threads to run. */
        CyU3PThreadRelinquish ();
    }
}