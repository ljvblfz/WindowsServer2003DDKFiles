/*++

Copyright (c) Microsoft Corporation.  All rights reserved.

    THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY
    KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A PARTICULAR
    PURPOSE.


Module Name:

    firefly.c

Abstract:

    This module contains filter driver code for the firefly device.

Environment:

    Kernel mode

Revision History:

    Adrian J. Oney - Written March 17th, 2001
                     (based on Toaster DDK filter sample)

    Eliyas Yakub - Cleaned up for DDK - 18-Oct-2002                     

--*/

#include "firefly.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, FireflyAddDevice)
#pragma alloc_text(PAGE, FireflyUnload)
#pragma alloc_text(PAGE, FireflyDispatchPnp)
#pragma alloc_text(PAGE, FireflyDispatchPower)
#pragma alloc_text(PAGE, FireflyDispatchWmi)
#pragma alloc_text(PAGE, FireflyRegisterWmi)
#pragma alloc_text(PAGE, FireflySetWmiDataItem)
#pragma alloc_text(PAGE, FireflySetWmiDataBlock)
#pragma alloc_text(PAGE, FireflyQueryWmiDataBlock)
#pragma alloc_text(PAGE, FireflyQueryWmiRegInfo)
#endif

UNICODE_STRING GlobalRegistryPath;

//
// Defines for the various WMI blocks we are exposing
//
#define WMI_FIREFLY_DRIVER_INFORMATION  0

//
// Guids for the various WMI blocks we are exposing
//
WMIGUIDREGINFO FireflyWmiGuidList[1] = {

    { &FIREFLY_WMI_STD_DATA_GUID, 1, 0 } // driver information
};

//
// Where they are described.
//
#define MOFRESOURCENAME L"FireflyWMI"

NTSTATUS
DriverEntry(
    IN PDRIVER_OBJECT  DriverObject,
    IN PUNICODE_STRING RegistryPath
    )
/*++

Routine Description:

    Installable driver initialization entry point.
    This entry point is called directly by the I/O system.

Arguments:

    DriverObject - pointer to the driver object

    RegistryPath - pointer to a unicode string representing the path,
                   to driver-specific key in the registry.

Return Value:

    NT status code
    
--*/
{
    USHORT              regPathLen;
    ULONG               ulIndex;
    PDRIVER_DISPATCH  * dispatch;

    DebugPrint(("Entered the Driver Entry\n"));

    //
    // Cache away the registry path.
    //
    regPathLen = RegistryPath->Length;
    GlobalRegistryPath.MaximumLength = regPathLen + sizeof(UNICODE_NULL);
    GlobalRegistryPath.Length = regPathLen;
    GlobalRegistryPath.Buffer = ExAllocatePoolWithTag(
                                            PagedPool,
                                            GlobalRegistryPath.MaximumLength,
                                            POOL_TAG
                                            );

    if (GlobalRegistryPath.Buffer == NULL) {

        DebugPrint(("Couldn't allocate pool for registry path!\n"));
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyUnicodeString(&GlobalRegistryPath, RegistryPath);

    //
    // Create dispatch points
    //
    for (ulIndex = 0, dispatch = DriverObject->MajorFunction;
         ulIndex <= IRP_MJ_MAXIMUM_FUNCTION;
         ulIndex++, dispatch++) {

        *dispatch = FireflyForwardIrp;
    }

    DriverObject->MajorFunction[IRP_MJ_PNP]            = FireflyDispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER]          = FireflyDispatchPower;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = FireflyDispatchWmi;
    DriverObject->DriverExtension->AddDevice           = FireflyAddDevice;
    DriverObject->DriverUnload                         = FireflyUnload;

    return STATUS_SUCCESS;
}


NTSTATUS
FireflyAddDevice(
    IN PDRIVER_OBJECT DriverObject,
    IN PDEVICE_OBJECT PhysicalDeviceObject
    )
/*++

Routine Description:

   The Plug & Play subsystem is handing us a brand new PDO, for which we (by
   means of INF registration) have been asked to provide a driver.

   We need to determine if we need to be in the driver stack for the device.
   Create a function device object to attach to the stack
   Initialize that device object
   Return status success.

   Remember: We can NOT actually send ANY non pnp IRPS to the given driver
   stack, UNTIL we have received an IRP_MN_START_DEVICE.

Arguments:

   DeviceObject - pointer to a device object.

   PhysicalDeviceObject -  pointer to a device object created by the
                           underlying bus driver.

Return Value:

   NT status code.

--*/
{
    NTSTATUS                status;
    PDEVICE_OBJECT          deviceObject;
    PDEVICE_EXTENSION       deviceExtension = NULL;

    PAGED_CODE();

    //
    // Create a filter device object.
    //
    
    status = IoCreateDevice(
        DriverObject,
        sizeof(DEVICE_EXTENSION),
        NULL,                       // No Name
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN | FILE_AUTOGENERATED_DEVICE_NAME,
        FALSE,
        &deviceObject
        );

    if (!NT_SUCCESS(status)) {

        //
        // Returning failure here prevents the entire stack from functioning,
        // but most likely the rest of the stack will not be able to create
        // device objects either, so it is still OK.
        //
        return status;
    }

    DebugPrint((
        "AddDevice PDO (0x%x) FDO (0x%x)\n",
        PhysicalDeviceObject,
        deviceObject
        ));

    //
    // Get a pointer to our device extension.
    //
    deviceExtension = (PDEVICE_EXTENSION) deviceObject->DeviceExtension;

    //
    // Save some information for later.
    //
    deviceExtension->Self = deviceObject;
    deviceExtension->PhysicalDeviceObject = PhysicalDeviceObject;

    deviceExtension->NextLowerDriver = IoAttachDeviceToDeviceStack(
        deviceObject,
        PhysicalDeviceObject
        );

    //
    // Failure for attachment is an indication of a broken plug & play system.
    //
    if (NULL == deviceExtension->NextLowerDriver) {
        IoDeleteDevice(deviceObject);
        return STATUS_UNSUCCESSFUL;
    }

    deviceObject->Flags |= deviceExtension->NextLowerDriver->Flags &
                            (DO_BUFFERED_IO | DO_DIRECT_IO |
                            DO_POWER_PAGABLE  | DO_POWER_INRUSH);

    deviceObject->DeviceType = deviceExtension->NextLowerDriver->DeviceType;

    deviceObject->Characteristics =
                          deviceExtension->NextLowerDriver->Characteristics;

    //
    // Set the initial state of the Filter DO
    //
    INITIALIZE_PNP_STATE(deviceExtension);

    //
    // Let us use remove lock to keep count of IRPs so that we don't 
    // deteach or delete our deviceobject until all pending I/Os in our
    // devstack are completed. Remlock is required to protect us from
    // various race conditions.
    //
    
    IoInitializeRemoveLock (&deviceExtension->RemoveLock , 
                            POOL_TAG,
                            1, // MaxLockedMinutes 
                            5); // HighWatermark, this parameter is 
                                // used only on checked build.
                                
    DebugPrint((
        "AddDevice: %x to %x->%x \n",
        deviceObject,
        deviceExtension->NextLowerDriver,
        PhysicalDeviceObject
        ));

    deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;
}


VOID
FireflyUnload(
    IN PDRIVER_OBJECT DriverObject
    )
/*++

Routine Description:

    Free all the allocated resources in DriverEntry, etc.

Arguments:

    DriverObject - pointer to a driver object.

Return Value:

    VOID.

--*/
{
    PAGED_CODE();

    //
    // There should be no remaining device object(s) now.
    //
    ASSERT(DriverObject->DeviceObject == NULL);

    //
    // Free the registry path we cached away.
    //
    if (GlobalRegistryPath.Buffer) {

        ExFreePool(GlobalRegistryPath.Buffer);
    }

    //
    // We should not be unloaded until all the devices we control
    // have been removed from our queue.
    //
    DebugPrint(("unload\n"));

    return;
}


NTSTATUS
FireflyDispatchPnp(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP           Irp
    )
/*++

Routine Description:

   The plug and play dispatch routines.

   Most of these the driver will completely ignore. In all cases it must pass
   on the IRP to the lower driver.

Arguments:

   DeviceObject - pointer to a device object.

   Irp - pointer to an I/O Request Packet.

Return Value:

   NT status code

--*/
{
    PDEVICE_EXTENSION           deviceExtension;
    PIO_STACK_LOCATION          irpStack, nextStack;
    NTSTATUS                    status;
    KEVENT                      event;

    PAGED_CODE();

    deviceExtension = (PDEVICE_EXTENSION) DeviceObject->DeviceExtension;
    irpStack = IoGetCurrentIrpStackLocation(Irp);

    DebugPrint((
        "FilterDO %s IRP:0x%x \n",
        PnPMinorFunctionString(irpStack->MinorFunction),
        Irp
        ));

   status = IoAcquireRemoveLock (&deviceExtension->RemoveLock, Irp);
    if (!NT_SUCCESS (status)) {
        Irp->IoStatus.Status = status;
        IoCompleteRequest (Irp, IO_NO_INCREMENT);
        return status;
    }
    
    switch(irpStack->MinorFunction) {

    case IRP_MN_START_DEVICE:

        //
        // The device is starting.
        //
        // We cannot touch the device (send it any non pnp irps) until a
        // start device has been passed down to the lower drivers.
        //
        KeInitializeEvent(&event,
                          NotificationEvent,
                          FALSE
                          );

        IoCopyCurrentIrpStackLocationToNext(Irp);

        IoSetCompletionRoutine(
            Irp,
            FireflySynchronousCompletionRoutine,
            &event,
            TRUE,
            TRUE,
            TRUE
            );

        status = IoCallDriver(deviceExtension->NextLowerDriver, Irp);

        if (STATUS_PENDING == status) {

            KeWaitForSingleObject(
               &event,
               Executive, // Waiting for reason of a driver
               KernelMode, // Waiting in kernel mode
               FALSE, // No alert
               NULL // No timeout
               );

            status = Irp->IoStatus.Status;
        }

        if (NT_SUCCESS(status) && 
                        deviceExtension->DevicePnPState != Stopped) {
                status = FireflyRegisterWmi(deviceExtension);
        }

        if (NT_SUCCESS(status)) {
            //
            // Set the state of the device to started
            //
            SET_NEW_PNP_STATE(deviceExtension, Started);
        }

        //
        // We must now complete the IRP, since we stopped it in the
        // completion routine with STATUS_MORE_PROCESSING_REQUIRED.
        //
        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        IoReleaseRemoveLock(&deviceExtension->RemoveLock, Irp); 
        return status;

    case IRP_MN_REMOVE_DEVICE:

        SET_NEW_PNP_STATE(deviceExtension, Deleted);

        IoWMIRegistrationControl(
                            deviceExtension->Self,
                            WMIREG_ACTION_DEREGISTER
                            );

        //
        // Wait for all outstanding requests to complete
        //
        DebugPrint(("Waiting for outstanding requests\n"));
        IoReleaseRemoveLockAndWait(&deviceExtension->RemoveLock, Irp);
        
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(deviceExtension->NextLowerDriver, Irp);

        IoDetachDevice(deviceExtension->NextLowerDriver);
        IoDeleteDevice(DeviceObject);
        return status;

    case IRP_MN_QUERY_STOP_DEVICE:
        SET_NEW_PNP_STATE(deviceExtension, StopPending);
        status = STATUS_SUCCESS;
        break;

    case IRP_MN_CANCEL_STOP_DEVICE:

        //
        // Check to see whether you have received cancel-stop
        // without first receiving a query-stop. This could happen if someone
        // above us fails a query-stop and passes down the subsequent
        // cancel-stop.
        //

        if(StopPending == deviceExtension->DevicePnPState)
        {
            //
            // We did receive a query-stop, so restore.
            //
            RESTORE_PREVIOUS_PNP_STATE(deviceExtension);
        }
        status = STATUS_SUCCESS; // We must not fail this IRP.
        break;

    case IRP_MN_STOP_DEVICE:
        SET_NEW_PNP_STATE(deviceExtension, StopPending);
        status = STATUS_SUCCESS;
        break;

    case IRP_MN_QUERY_REMOVE_DEVICE:

        SET_NEW_PNP_STATE(deviceExtension, RemovePending);
        status = STATUS_SUCCESS;
        break;

    case IRP_MN_SURPRISE_REMOVAL:

        SET_NEW_PNP_STATE(deviceExtension, SurpriseRemovePending);
        status = STATUS_SUCCESS;
        break;

    case IRP_MN_CANCEL_REMOVE_DEVICE:

        //
        // Check to see whether you have received cancel-remove
        // without first receiving a query-remove. This could happen if
        // someone above us fails a query-remove and passes down the
        // subsequent cancel-remove.
        //
        if (RemovePending == deviceExtension->DevicePnPState) {

            //
            // We did receive a query-remove, so restore.
            //
            RESTORE_PREVIOUS_PNP_STATE(deviceExtension);
        }

        status = STATUS_SUCCESS; // We must not fail this IRP.
        break;

    default:

        //
        // If you don't handle any IRP you must leave the
        // status as is.
        //
        status = Irp->IoStatus.Status;
        break;
    }

    //
    // Pass the IRP down and forget it.
    //
    Irp->IoStatus.Status = status;
    IoSkipCurrentIrpStackLocation (Irp);
    status = IoCallDriver (deviceExtension->NextLowerDriver, Irp);
    IoReleaseRemoveLock(&deviceExtension->RemoveLock, Irp); 
    return status;
}


NTSTATUS
FireflySynchronousCompletionRoutine(
    IN PDEVICE_OBJECT   DeviceObject,
    IN PIRP             Irp,
    IN PVOID            Context
    )
/*++

Routine Description:

    A completion routine for use when calling the lower device objects to
    which our filter deviceobject is attached.

Arguments:

    DeviceObject - Pointer to deviceobject

    Irp          - Pointer to a PnP Irp.

    Context      - Pointer to an event object

Return Value:

    NT Status is returned.

--*/
{
    UNREFERENCED_PARAMETER(DeviceObject);

    //
    // We could switch on the major and minor functions of the IRP to perform
    // different functions, but we know that Context is an event that needs
    // to be set.
    //
    KeSetEvent((PKEVENT) Context, IO_NO_INCREMENT, FALSE);

    //
    // Allows the caller to use the IRP after it is completed
    //
    return STATUS_MORE_PROCESSING_REQUIRED;
}


NTSTATUS
FireflyDispatchPower(
    IN PDEVICE_OBJECT    DeviceObject,
    IN PIRP              Irp
    )
/*++

Routine Description:

    This routine is the dispatch routine for power irps.

Arguments:

    DeviceObject - Pointer to the device object.

    Irp - Pointer to the request packet.

Return Value:

    NT Status code

--*/
{
    PDEVICE_EXTENSION deviceExtension;
    NTSTATUS status;
    
    PAGED_CODE();

    deviceExtension = (PDEVICE_EXTENSION) DeviceObject->DeviceExtension;

    status = IoAcquireRemoveLock (&deviceExtension->RemoveLock, Irp);
    if (!NT_SUCCESS (status)) { // may be device is being removed.
        Irp->IoStatus.Status = status;
        PoStartNextPowerIrp(Irp);
        IoCompleteRequest (Irp, IO_NO_INCREMENT);
        return status;
    }

    PoStartNextPowerIrp(Irp);

    IoSkipCurrentIrpStackLocation(Irp);
    status =  PoCallDriver(deviceExtension->NextLowerDriver, Irp);
    IoReleaseRemoveLock(&deviceExtension->RemoveLock, Irp); 
    return status;
    
}


NTSTATUS
FireflyDispatchWmi(
    IN PDEVICE_OBJECT    DeviceObject,
    IN PIRP              Irp
    )
/*++

Routine Description:

    This routine is the dispatch routine for Ioctls.

Arguments:

    DeviceObject - Pointer to the device object.

    Irp - Pointer to the request packet.

Return Value:

    NT Status code
--*/
{
    SYSCTL_IRP_DISPOSITION  disposition;
    NTSTATUS                status;
    PDEVICE_EXTENSION       deviceExtension;

    PAGED_CODE();

    deviceExtension = (PDEVICE_EXTENSION) DeviceObject->DeviceExtension;

    status = IoAcquireRemoveLock (&deviceExtension->RemoveLock, Irp);
    if (!NT_SUCCESS (status)) { // may be device is being removed.
        Irp->IoStatus.Status = status;
        IoCompleteRequest (Irp, IO_NO_INCREMENT);
        return status;
    }
    
    status = WmiSystemControl(
        &deviceExtension->WmiLibInfo,
        DeviceObject,
        Irp,
        &disposition
        );

    switch(disposition) {

        case IrpProcessed:

            //
            // This irp has been processed and may be completed or pending.
            //
            break;

        case IrpNotCompleted:

            //
            // This irp has not been completed, but has been fully processed.
            // we will complete it now
            //
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            break;

        case IrpForward:
        case IrpNotWmi:

            //
            // This irp is either not a WMI irp or is a WMI irp targeted at a
            // device lower in the stack.
            //
            IoSkipCurrentIrpStackLocation (Irp);
            status = IoCallDriver (deviceExtension->NextLowerDriver, Irp);
            break;

        default:

            //
            // We really should never get here, but if we do just forward....
            //
            ASSERT(FALSE);
            IoSkipCurrentIrpStackLocation (Irp);
            status = IoCallDriver (deviceExtension->NextLowerDriver, Irp);
            break;
    }

    IoReleaseRemoveLock(&deviceExtension->RemoveLock, Irp); 
    return status;
}


NTSTATUS
FireflyForwardIrp(
    IN PDEVICE_OBJECT   DeviceObject,
    IN PIRP             Irp
    )
/*++

Routine Description:

    The default dispatch routine.  If this driver does not recognize the
    IRP, then it should send it down, unmodified.
    If the device holds iris, this IRP must be queued in the device extension
    No completion routine is required.

    For demonstrative purposes only, we will pass all the (non-PnP) Irps down
    on the stack (as we are a filter driver). A real driver might choose to
    service some of these Irps.

    As we have NO idea which function we are happily passing on, we can make
    NO assumptions about whether or not it will be called at raised IRQL.
    For this reason, this function must be in put into non-paged pool
    (aka the default location).

Arguments:

   DeviceObject - pointer to a device object.

   Irp - pointer to an I/O Request Packet.

Return Value:

      NT status code

--*/
{
    PDEVICE_EXTENSION deviceExtension;
    NTSTATUS status;
    
    deviceExtension = (PDEVICE_EXTENSION) DeviceObject->DeviceExtension;

    status = IoAcquireRemoveLock (&deviceExtension->RemoveLock, Irp);
    if (!NT_SUCCESS (status)) {
        Irp->IoStatus.Status = status;
        IoCompleteRequest (Irp, IO_NO_INCREMENT);
        return status;
    }
    
   IoSkipCurrentIrpStackLocation (Irp);
   status = IoCallDriver (deviceExtension->NextLowerDriver, Irp);
   IoReleaseRemoveLock(&deviceExtension->RemoveLock, Irp); 
   return status;
   
}


NTSTATUS
FireflyRegisterWmi(
    IN  PDEVICE_EXTENSION   DeviceExtension
    )
{
    ULONG guidCount;

    PAGED_CODE();

    //
    // Initialize our WMI state.
    //
    DeviceExtension->StdDeviceData.TailLit = TRUE;

    //
    // Register with the WMI library.
    //
    guidCount = ARRAYLEN(FireflyWmiGuidList);
    ASSERT(1 == guidCount);

    DeviceExtension->WmiLibInfo.GuidCount = guidCount;
    DeviceExtension->WmiLibInfo.GuidList = FireflyWmiGuidList;
    DeviceExtension->WmiLibInfo.QueryWmiRegInfo = FireflyQueryWmiRegInfo;
    DeviceExtension->WmiLibInfo.QueryWmiDataBlock = FireflyQueryWmiDataBlock;
    DeviceExtension->WmiLibInfo.SetWmiDataBlock = FireflySetWmiDataBlock;
    DeviceExtension->WmiLibInfo.SetWmiDataItem = FireflySetWmiDataItem;
    DeviceExtension->WmiLibInfo.ExecuteWmiMethod = NULL;
    DeviceExtension->WmiLibInfo.WmiFunctionControl = NULL;

    return IoWMIRegistrationControl(
        DeviceExtension->Self,
        WMIREG_ACTION_REGISTER
        );
}


//
// WMI System Call back functions
//
NTSTATUS
FireflySetWmiDataItem(
    IN PDEVICE_OBJECT   DeviceObject,
    IN PIRP             Irp,
    IN ULONG            GuidIndex,
    IN ULONG            InstanceIndex,
    IN ULONG            DataItemId,
    IN ULONG            BufferSize,
    IN PUCHAR           Buffer
    )
/*++

Routine Description:

    This routine is a callback into the driver to set for the contents of a
    data block. When the driver has finished filling the data block it must
    call WmiCompleteRequest to complete the irp. The driver can return
    STATUS_PENDING if the irp cannot be completed immediately.

Arguments:

    DeviceObject is the device whose data block is being queried

    Irp is the Irp that makes this request

    GuidIndex is the index into the list of guids provided when the device
        registered

    InstanceIndex is the index that denotes which instance of the data block
        is being queried.

    DataItemId has the id of the data item being set

    BufferSize has the size of the data item passed

    Buffer has the new values for the data item

Return Value:

    status

--*/
{
    PDEVICE_EXTENSION deviceExtension;
    NTSTATUS status;

    PAGED_CODE();

    deviceExtension = (PDEVICE_EXTENSION) DeviceObject->DeviceExtension;

    switch(GuidIndex) {

        case WMI_FIREFLY_DRIVER_INFORMATION:

            if (DataItemId == 1 && BufferSize >= sizeof(BOOLEAN) ) {

               deviceExtension->StdDeviceData.TailLit =
                   (BOOLEAN) *((PULONG)Buffer);

               status = FireflySetFeature(
                   deviceExtension->PhysicalDeviceObject,
                   TAILLIGHT_PAGE,
                   TAILLIGHT_FEATURE,
                   deviceExtension->StdDeviceData.TailLit
                   );

            } else {

                status = STATUS_WMI_READ_ONLY;
            }

            break;

        default:

            status = STATUS_WMI_GUID_NOT_FOUND;
    }

    return WmiCompleteRequest(
        DeviceObject,
        Irp,
        status,
        0,
        IO_NO_INCREMENT
        );
}


NTSTATUS
FireflySetWmiDataBlock(
    IN PDEVICE_OBJECT   DeviceObject,
    IN PIRP             Irp,
    IN ULONG            GuidIndex,
    IN ULONG            InstanceIndex,
    IN ULONG            BufferSize,
    IN PUCHAR           Buffer
    )
/*++

Routine Description:

    This routine is a callback into the driver to set the contents of
    a data block. When the driver has finished filling the data block it
    must call WmiCompleteRequest to complete the irp. The driver can
    return STATUS_PENDING if the irp cannot be completed immediately.

Arguments:

    DeviceObject is the device whose data block is being queried

    Irp is the Irp that makes this request

    GuidIndex is the index into the list of guids provided when the
        device registered

    InstanceIndex is the index that denotes which instance of the data block
        is being queried.

    BufferSize has the size of the data block passed

    Buffer has the new values for the data block


Return Value:

    status

--*/
{
    PDEVICE_EXTENSION deviceExtension;
    NTSTATUS status;

    PAGED_CODE();

    DebugPrint(("Entered FireflySetWmiDataBlock\n"));

    deviceExtension = (PDEVICE_EXTENSION) DeviceObject->DeviceExtension;

    switch(GuidIndex) {

        case WMI_FIREFLY_DRIVER_INFORMATION:

            //
            // We will update only writable elements.
            //
            if (BufferSize >= sizeof(BOOLEAN) ) {
                deviceExtension->StdDeviceData.TailLit =
                    ((PFIREFLY_WMI_STD_DATA)Buffer)->TailLit;

                status = FireflySetFeature(
                    deviceExtension->PhysicalDeviceObject,
                    TAILLIGHT_PAGE,
                    TAILLIGHT_FEATURE,
                    deviceExtension->StdDeviceData.TailLit
                    );
            } else {
                status = STATUS_BUFFER_TOO_SMALL;            
            }
            
            break;

        default:
            status = STATUS_WMI_GUID_NOT_FOUND;
    }

    return WmiCompleteRequest(
        DeviceObject,
        Irp,
        status,
        0,
        IO_NO_INCREMENT
        );
}


NTSTATUS
FireflyQueryWmiDataBlock(
    IN      PDEVICE_OBJECT  DeviceObject,
    IN      PIRP            Irp,
    IN      ULONG           GuidIndex,
    IN      ULONG           InstanceIndex,
    IN      ULONG           InstanceCount,
    IN OUT  PULONG          InstanceLengthArray,
    IN      ULONG           OutBufferSize,
    OUT     PUCHAR          Buffer
    )
/*++

Routine Description:

    This routine is a callback into the driver to query for the contents of
    a data block. When the driver has finished filling the data block it
    must call WmiCompleteRequest to complete the irp. The driver can
    return STATUS_PENDING if the irp cannot be completed immediately.

Arguments:

    DeviceObject is the device whose data block is being queried

    Irp is the Irp that makes this request

    GuidIndex is the index into the list of guids provided when the
        device registered

    InstanceIndex is the index that denotes which instance of the data block
        is being queried.

    InstanceCount is the number of instances expected to be returned for
        the data block.

    InstanceLengthArray is a pointer to an array of ULONG that returns the
        lengths of each instance of the data block. If this is NULL then
        there was not enough space in the output buffer to fulfill the request
        so the irp should be completed with the buffer needed.

    BufferAvail on has the maximum size available to write the data
        block.

    Buffer on return is filled with the returned data block

Return Value:

    status

--*/
{
    PDEVICE_EXTENSION   deviceExtension;
    ULONG               size;
    NTSTATUS            status;

    PAGED_CODE();

    deviceExtension = (PDEVICE_EXTENSION) DeviceObject->DeviceExtension;

    //
    // Only ever registers 1 instance per guid
    //
    ASSERT((InstanceIndex == 0) &&
           (InstanceCount == 1));

    switch(GuidIndex) {

        case WMI_FIREFLY_DRIVER_INFORMATION:

            size = sizeof(FIREFLY_WMI_STD_DATA);
            if (OutBufferSize < size ) {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            *(PFIREFLY_WMI_STD_DATA) Buffer = deviceExtension->StdDeviceData;
            *InstanceLengthArray = size;
            status = STATUS_SUCCESS;
            break;

        default:

            size = 0;
            status = STATUS_WMI_GUID_NOT_FOUND;
    }

    return WmiCompleteRequest(
        DeviceObject,
        Irp,
        status,
        size,
        IO_NO_INCREMENT
        );
}


NTSTATUS
FireflyQueryWmiRegInfo(
    IN  PDEVICE_OBJECT      DeviceObject,
    OUT ULONG              *RegFlags,
    OUT PUNICODE_STRING     InstanceName,
    OUT PUNICODE_STRING    *RegistryPath,
    OUT PUNICODE_STRING     MofResourceName,
    OUT PDEVICE_OBJECT     *Pdo
    )
/*++

Routine Description:

    This routine is a callback into the driver to retrieve the list of
    guids or data blocks that the driver wants to register with WMI. This
    routine may not pend or block. Driver should NOT call
    WmiCompleteRequest.

Arguments:

    DeviceObject is the device whose data block is being queried

    *RegFlags returns with a set of flags that describe the guids being
        registered for this device. If the device wants enable and disable
        collection callbacks before receiving queries for the registered
        guids then it should return the WMIREG_FLAG_EXPENSIVE flag. Also the
        returned flags may specify WMIREG_FLAG_INSTANCE_PDO in which case
        the instance name is determined from the PDO associated with the
        device object. Note that the PDO must have an associated devnode. If
        WMIREG_FLAG_INSTANCE_PDO is not set then Name must return a unique
        name for the device.

    InstanceName returns with the instance name for the guids if
        WMIREG_FLAG_INSTANCE_PDO is not set in the returned *RegFlags. The
        caller will call ExFreePool with the buffer returned.

    *RegistryPath returns with the registry path of the driver

    *MofResourceName returns with the name of the MOF resource attached to
        the binary file. If the driver does not have a mof resource attached
        then this can be returned as NULL.

    *Pdo returns with the device object for the PDO associated with this
        device if the WMIREG_FLAG_INSTANCE_PDO flag is returned in
        *RegFlags.

Return Value:

    status

--*/
{
    PDEVICE_EXTENSION deviceExtension;

    PAGED_CODE();

    deviceExtension = (PDEVICE_EXTENSION) DeviceObject->DeviceExtension;

    *RegFlags = WMIREG_FLAG_INSTANCE_PDO;
    *RegistryPath = &GlobalRegistryPath;
    *Pdo = deviceExtension->PhysicalDeviceObject;
    RtlInitUnicodeString(MofResourceName, MOFRESOURCENAME);

    return STATUS_SUCCESS;
}


#if DBG

PCHAR
PnPMinorFunctionString(
    IN UCHAR    MinorFunction
    )
{
    switch (MinorFunction)
    {
        case IRP_MN_START_DEVICE:
            return "IRP_MN_START_DEVICE";
        case IRP_MN_QUERY_REMOVE_DEVICE:
            return "IRP_MN_QUERY_REMOVE_DEVICE";
        case IRP_MN_REMOVE_DEVICE:
            return "IRP_MN_REMOVE_DEVICE";
        case IRP_MN_CANCEL_REMOVE_DEVICE:
            return "IRP_MN_CANCEL_REMOVE_DEVICE";
        case IRP_MN_STOP_DEVICE:
            return "IRP_MN_STOP_DEVICE";
        case IRP_MN_QUERY_STOP_DEVICE:
            return "IRP_MN_QUERY_STOP_DEVICE";
        case IRP_MN_CANCEL_STOP_DEVICE:
            return "IRP_MN_CANCEL_STOP_DEVICE";
        case IRP_MN_QUERY_DEVICE_RELATIONS:
            return "IRP_MN_QUERY_DEVICE_RELATIONS";
        case IRP_MN_QUERY_INTERFACE:
            return "IRP_MN_QUERY_INTERFACE";
        case IRP_MN_QUERY_CAPABILITIES:
            return "IRP_MN_QUERY_CAPABILITIES";
        case IRP_MN_QUERY_RESOURCES:
            return "IRP_MN_QUERY_RESOURCES";
        case IRP_MN_QUERY_RESOURCE_REQUIREMENTS:
            return "IRP_MN_QUERY_RESOURCE_REQUIREMENTS";
        case IRP_MN_QUERY_DEVICE_TEXT:
            return "IRP_MN_QUERY_DEVICE_TEXT";
        case IRP_MN_FILTER_RESOURCE_REQUIREMENTS:
            return "IRP_MN_FILTER_RESOURCE_REQUIREMENTS";
        case IRP_MN_READ_CONFIG:
            return "IRP_MN_READ_CONFIG";
        case IRP_MN_WRITE_CONFIG:
            return "IRP_MN_WRITE_CONFIG";
        case IRP_MN_EJECT:
            return "IRP_MN_EJECT";
        case IRP_MN_SET_LOCK:
            return "IRP_MN_SET_LOCK";
        case IRP_MN_QUERY_ID:
            return "IRP_MN_QUERY_ID";
        case IRP_MN_QUERY_PNP_DEVICE_STATE:
            return "IRP_MN_QUERY_PNP_DEVICE_STATE";
        case IRP_MN_QUERY_BUS_INFORMATION:
            return "IRP_MN_QUERY_BUS_INFORMATION";
        case IRP_MN_DEVICE_USAGE_NOTIFICATION:
            return "IRP_MN_DEVICE_USAGE_NOTIFICATION";
        case IRP_MN_SURPRISE_REMOVAL:
            return "IRP_MN_SURPRISE_REMOVAL";

        default:
            return "IRP_MN_?????";
    }
}

#endif

