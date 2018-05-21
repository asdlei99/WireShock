/*++

Module Name:

    device.c - Device handling events for example driver.

Abstract:

   This file contains the device entry points and callbacks.

Environment:

    Kernel-mode Driver Framework

--*/

#include "driver.h"
#include "device.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, WireShockCreateDevice)
#pragma alloc_text (PAGE, WireShockEvtDevicePrepareHardware)
#endif


NTSTATUS
WireShockCreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
/*++

Routine Description:

    Worker routine called to create a device and its software resources.

Arguments:

    DeviceInit - Pointer to an opaque init structure. Memory for this
                    structure will be freed by the framework when the WdfDeviceCreate
                    succeeds. So don't access the structure after that point.

Return Value:

    NTSTATUS

--*/
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
    WDF_OBJECT_ATTRIBUTES   deviceAttributes;
    PDEVICE_CONTEXT deviceContext;
    WDFDEVICE device;
    NTSTATUS status;
    WDF_CHILD_LIST_CONFIG childListCfg;

    PAGED_CODE();

    WDF_CHILD_LIST_CONFIG_INIT(
        &childListCfg,
        sizeof(PDO_IDENTIFICATION_DESCRIPTION),
        WireShockEvtWdfChildListCreateDevice
    );

    childListCfg.AddressDescriptionSize = sizeof(PDO_ADDRESS_DESCRIPTION);
    childListCfg.EvtChildListAddressDescriptionCleanup = WireShockEvtWdfChildListAddressDescriptionCleanup;

    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_BUS_EXTENDER);
    WdfDeviceInitSetExclusive(DeviceInit, TRUE);

    WdfFdoInitSetDefaultChildListConfig(DeviceInit,
        &childListCfg,
        WDF_NO_OBJECT_ATTRIBUTES);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDevicePrepareHardware = WireShockEvtDevicePrepareHardware;
    pnpPowerCallbacks.EvtDeviceD0Entry = WireShockEvtDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);

    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);

    if (NT_SUCCESS(status)) {
        //
        // Get a pointer to the device context structure that we just associated
        // with the device object. We define this structure in the device.h
        // header file. DeviceGetContext is an inline function generated by
        // using the WDF_DECLARE_CONTEXT_TYPE_WITH_NAME macro in device.h.
        // This function will do the type checking and return the device context.
        // If you pass a wrong object handle it will return NULL and assert if
        // run under framework verifier mode.
        //
        deviceContext = DeviceGetContext(device);


        //
        // Create a device interface so that applications can find and talk
        // to us.
        //
        status = WdfDeviceCreateDeviceInterface(
            device,
            &GUID_DEVINTERFACE_WireShock,
            NULL // ReferenceString
        );

        if (NT_SUCCESS(status)) {
            //
            // Initialize the I/O Package and any Queues
            //
            status = WireShockQueueInitialize(device);
        }
    }

    return status;
}

NTSTATUS
WireShockEvtDevicePrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourceList,
    _In_ WDFCMRESLIST ResourceListTranslated
)
/*++

Routine Description:

    In this callback, the driver does whatever is necessary to make the
    hardware ready to use.  In the case of a USB device, this involves
    reading and selecting descriptors.

Arguments:

    Device - handle to a device

Return Value:

    NT status value

--*/
{
    NTSTATUS status;
    PDEVICE_CONTEXT pDeviceContext;
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS configParams;
    WDFUSBPIPE                          pipe;
    WDF_USB_PIPE_INFORMATION            pipeInfo;
    UCHAR                               index;
    UCHAR                               numberConfiguredPipes;

    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(ResourceListTranslated);

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Entry");

    status = STATUS_SUCCESS;
    pDeviceContext = DeviceGetContext(Device);

    //
    // Create a USB device handle so that we can communicate with the
    // underlying USB stack. The WDFUSBDEVICE handle is used to query,
    // configure, and manage all aspects of the USB device.
    // These aspects include device properties, bus properties,
    // and I/O creation and synchronization. We only create the device the first time
    // PrepareHardware is called. If the device is restarted by pnp manager
    // for resource rebalance, we will use the same device handle but then select
    // the interfaces again because the USB stack could reconfigure the device on
    // restart.
    //
    if (pDeviceContext->UsbDevice == NULL) {

        status = WdfUsbTargetDeviceCreate(Device,
            WDF_NO_OBJECT_ATTRIBUTES,
            &pDeviceContext->UsbDevice
        );

        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                "WdfUsbTargetDeviceCreateWithParameters failed 0x%x", status);
            return status;
        }
    }

    //
    // Select the first configuration of the device, using the first alternate
    // setting of each interface
    //
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_MULTIPLE_INTERFACES(&configParams,
        0,
        NULL
    );
    status = WdfUsbTargetDeviceSelectConfig(pDeviceContext->UsbDevice,
        WDF_NO_OBJECT_ATTRIBUTES,
        &configParams
    );

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "WdfUsbTargetDeviceSelectConfig failed 0x%x", status);
        return status;
    }

    pDeviceContext->UsbInterface =
        WdfUsbTargetDeviceGetInterface(pDeviceContext->UsbDevice, 0);

    if (NULL == pDeviceContext->UsbInterface) {
        status = STATUS_UNSUCCESSFUL;
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "WdfUsbTargetDeviceGetInterface 0 failed %!STATUS!",
            status);
        return status;
    }

    numberConfiguredPipes = WdfUsbInterfaceGetNumConfiguredPipes(pDeviceContext->UsbInterface);

    //
    // Get pipe handles
    //
    for (index = 0; index < numberConfiguredPipes; index++) {

        WDF_USB_PIPE_INFORMATION_INIT(&pipeInfo);

        pipe = WdfUsbInterfaceGetConfiguredPipe(
            pDeviceContext->UsbInterface,
            index, //PipeIndex,
            &pipeInfo
        );
        //
        // Tell the framework that it's okay to read less than
        // MaximumPacketSize
        //
        WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(pipe);

        if (WdfUsbPipeTypeInterrupt == pipeInfo.PipeType) {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                "Interrupt Pipe is 0x%p", pipe);
            pDeviceContext->InterruptPipe = pipe;
        }

        if (WdfUsbPipeTypeBulk == pipeInfo.PipeType &&
            WdfUsbTargetPipeIsInEndpoint(pipe)) {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                "BulkInput Pipe is 0x%p", pipe);
            pDeviceContext->BulkReadPipe = pipe;
        }

        if (WdfUsbPipeTypeBulk == pipeInfo.PipeType &&
            WdfUsbTargetPipeIsOutEndpoint(pipe)) {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                "BulkOutput Pipe is 0x%p", pipe);
            pDeviceContext->BulkWritePipe = pipe;
        }
    }

    //
    // If we didn't find all the 3 pipes, fail the start.
    //
    if (!(pDeviceContext->BulkWritePipe
        && pDeviceContext->BulkReadPipe && pDeviceContext->InterruptPipe)) {
        status = STATUS_INVALID_DEVICE_STATE;
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "Device is not configured properly %!STATUS!",
            status);

        return status;
    }

    status = WireShockConfigContReaderForInterruptEndPoint(Device);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "WireShockConfigContReaderForInterruptEndPoint failed with status %!STATUS!",
            status);
        return status;
    }

    status = WireShockConfigContReaderForBulkReadEndPoint(pDeviceContext);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "WireShockConfigContReaderForBulkReadEndPoint failed with status %!STATUS!",
            status);
        return status;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Exit");

    return status;
}

_Use_decl_annotations_
NTSTATUS
WireShockEvtDeviceD0Entry(
    WDFDEVICE  Device,
    WDF_POWER_DEVICE_STATE  PreviousState
)
{
    PDEVICE_CONTEXT         pDeviceContext;
    NTSTATUS                status;
    BOOLEAN                 isTargetStarted;

    pDeviceContext = DeviceGetContext(Device);
    isTargetStarted = FALSE;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

    UNREFERENCED_PARAMETER(PreviousState);

    //
    // Since continuous reader is configured for this interrupt-pipe, we must explicitly start
    // the I/O target to get the framework to post read requests.
    //
    status = WdfIoTargetStart(WdfUsbTargetPipeGetIoTarget(pDeviceContext->InterruptPipe));
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "Failed to start interrupt pipe %!STATUS!", status);
        goto End;
    }

    status = WdfIoTargetStart(WdfUsbTargetPipeGetIoTarget(pDeviceContext->BulkReadPipe));
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "Failed to start bulk read pipe %!STATUS!", status);
        goto End;
    }

    status = WdfIoTargetStart(WdfUsbTargetPipeGetIoTarget(pDeviceContext->BulkWritePipe));
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "Failed to start bulk write pipe %!STATUS!", status);
        goto End;
    }

    isTargetStarted = TRUE;

End:

    if (!NT_SUCCESS(status)) {
        //
        // Failure in D0Entry will lead to device being removed. So let us stop the continuous
        // reader in preparation for the ensuing remove.
        //
        if (isTargetStarted) {
            WdfIoTargetStop(WdfUsbTargetPipeGetIoTarget(pDeviceContext->InterruptPipe), WdfIoTargetCancelSentIo);
            WdfIoTargetStop(WdfUsbTargetPipeGetIoTarget(pDeviceContext->BulkReadPipe), WdfIoTargetCancelSentIo);
            WdfIoTargetStop(WdfUsbTargetPipeGetIoTarget(pDeviceContext->BulkWritePipe), WdfIoTargetCancelSentIo);
        }
    }

    HCI_Command_Reset(pDeviceContext);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit");

    return status;
}
