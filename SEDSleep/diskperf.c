/*++
Copyright (C) Microsoft Corporation, 1991 - 1999

Module Name:

    diskperf.c

Abstract:

    This driver monitors disk accesses capturing performance data.

Environment:

    kernel mode only

Notes:

--*/


#define INITGUID

#include "ntddk.h"
#include "ntdddisk.h"
#include "stdarg.h"
#include "stdio.h"
#include "stddef.h"
#include <ntddvol.h>

#include <mountdev.h>
#include "wmistr.h"
#include "wmidata.h"
#include "wmiguid.h"
#include "wmilib.h"

#include "ntstrsafe.h"
#include <ntddscsi.h>
#include "ntintsafe.h"


#include "send5.h"
#include "send7.h"
#include "send7mbr.h"
#include "send9.h"

#ifdef POOL_TAGGING
#ifdef ExAllocatePool
#undef ExAllocatePool
#endif
#define ExAllocatePool(a,b) ExAllocatePoolWithTag(a,b,'frPD')
#endif

#define DISKPERF_MAXSTR         64

#define SEDSLEEP_SCSI_BUFFER_SIZE 2048

#define IOCTL_HURR_DURR_IM_A_GOAT      CTL_CODE(FILE_DEVICE_DISK, 0x4628, METHOD_BUFFERED, FILE_READ_DATA)

typedef enum _ATACOMMAND {
    IF_RECV = 0x5c,
    IF_SEND = 0x5e,
    IDENTIFY = 0xec,
} ATACOMMAND;

//
// Macro used to convert a ULONG into a 4 byte array (as big-endian)
//
#define Get4ByteArrayFromUlong(ULongValue, UCharArray)                                 \
         ((UNALIGNED UCHAR *)(UCharArray))[3] = ((UNALIGNED UCHAR *)&(ULongValue))[0]; \
         ((UNALIGNED UCHAR *)(UCharArray))[2] = ((UNALIGNED UCHAR *)&(ULongValue))[1]; \
         ((UNALIGNED UCHAR *)(UCharArray))[1] = ((UNALIGNED UCHAR *)&(ULongValue))[2]; \
         ((UNALIGNED UCHAR *)(UCharArray))[0] = ((UNALIGNED UCHAR *)&(ULongValue))[3];

//
// Device Extension
//

typedef struct _DEVICE_EXTENSION {

    //
    // Back pointer to device object
    //

    PDEVICE_OBJECT DeviceObject;

    //
    // Target Device Object
    //

    PDEVICE_OBJECT TargetDeviceObject;

    //
    // Physical device object
    //
    PDEVICE_OBJECT PhysicalDeviceObject;

    //
    // RemoveLock prevents removal of a device while it is busy.
    //

    IO_REMOVE_LOCK RemoveLock;

    //
    // Disk number for reference in WMI
    //

    ULONG       DiskNumber;

    //
    // If device is enabled for counting always
    //

    LONG        EnabledAlways;


    //
    // Use to keep track of Volume info from ntddvol.h
    //

    WCHAR StorageManagerName[8];

    //
    // Disk performance counters
    // and locals used to compute counters
    //

    ULONG   Processors;
    PDISK_PERFORMANCE DiskCounters;    // per processor counters
    LARGE_INTEGER LastIdleClock;
    LONG QueueDepth;
    LONG CountersEnabled;

    //
    // must synchronize paging path notifications
    //
    KEVENT PagingPathCountEvent;
    LONG  PagingPathCount;

    //
    // Physical Device name or WMI Instance Name
    //

    UNICODE_STRING PhysicalDeviceName;
    WCHAR PhysicalDeviceNameBuffer[DISKPERF_MAXSTR];

    UCHAR Sleepy;

    UCHAR ScsiSendBuffer[SEDSLEEP_SCSI_BUFFER_SIZE];
    UCHAR ScsiRecvBuffer[SEDSLEEP_SCSI_BUFFER_SIZE];

} DEVICE_EXTENSION, * PDEVICE_EXTENSION;

#define DEVICE_EXTENSION_SIZE sizeof(DEVICE_EXTENSION)
#define PROCESSOR_COUNTERS_SIZE FIELD_OFFSET(DISK_PERFORMANCE, QueryTime)

/*
Layout of Per Processor Counters is a contiguous block of memory:
    Processor 1
+-----------------------+     +-----------------------+
|PROCESSOR_COUNTERS_SIZE| ... |PROCESSOR_COUNTERS_SIZE|
+-----------------------+     +-----------------------+
where PROCESSOR_COUNTERS_SIZE is less than sizeof(DISK_PERFORMANCE) since
we only put those we actually use for counting.
*/

UNICODE_STRING DiskPerfRegistryPath;


//
// Function declarations
//

DRIVER_INITIALIZE DriverEntry;

DRIVER_ADD_DEVICE DiskPerfAddDevice;

DRIVER_DISPATCH DiskPerfForwardIrpSynchronous;

_Dispatch_type_(IRP_MJ_PNP)
DRIVER_DISPATCH DiskPerfDispatchPnp;

_Dispatch_type_(IRP_MJ_POWER)
DRIVER_DISPATCH DiskPerfDispatchPower;

DRIVER_DISPATCH DiskPerfSendToNextDriver;

_Dispatch_type_(IRP_MJ_CREATE)
DRIVER_DISPATCH DiskPerfCreate;

_Dispatch_type_(IRP_MJ_READ)
_Dispatch_type_(IRP_MJ_WRITE)
DRIVER_DISPATCH DiskPerfReadWrite;

_Dispatch_type_(IRP_MJ_DEVICE_CONTROL)
DRIVER_DISPATCH DiskPerfDeviceControl;

_Dispatch_type_(IRP_MJ_FLUSH_BUFFERS)
_Dispatch_type_(IRP_MJ_SHUTDOWN)
DRIVER_DISPATCH DiskPerfShutdownFlush;

DRIVER_DISPATCH DiskPerfStartDevice;
DRIVER_DISPATCH DiskPerfRemoveDevice;

DRIVER_UNLOAD DiskPerfUnload;


VOID
DiskPerfLogError(
    IN PDEVICE_OBJECT DeviceObject,
    IN ULONG UniqueId,
    IN NTSTATUS ErrorCode,
    IN NTSTATUS Status
);

NTSTATUS
DiskPerfRegisterDevice(
    IN PDEVICE_OBJECT DeviceObject
);

VOID
DiskPerfSyncFilterWithTarget(
    IN PDEVICE_OBJECT FilterDevice,
    IN PDEVICE_OBJECT TargetDevice
);

#if DBG

ULONG DiskPerfDebug = 0;

VOID
DiskPerfDebugPrint(
    ULONG DebugPrintLevel,
    PCCHAR DebugMessage,
    ...
);

#define DebugPrint(x)   DiskPerfDebugPrint x

#else

#define DebugPrint(x)

#endif


VOID SEDSleepUnlockDrive(
    IN PDEVICE_OBJECT DeviceObject
);

VOID SEDSleepSendOPALCommand(
    IN PDEVICE_OBJECT DeviceObject,
    UCHAR* src,
    size_t len
);

VOID SEDSleepSendSCSICommand(
    IN PDEVICE_OBJECT DeviceObject,
    ATACOMMAND cmd,
    UCHAR protocol,
    USHORT comID,
    const void* src,
    size_t len
);


_Success_(return != NULL)
_Post_maybenull_
_Must_inspect_result_
__drv_allocatesMem(Mem)
_Post_writable_byte_size_(*BytesAllocated)
_When_(((PoolType & 0x1)) != 0, _IRQL_requires_max_(APC_LEVEL))
_When_(((PoolType & 0x1)) == 0, _IRQL_requires_max_(DISPATCH_LEVEL))
_When_((PoolType& NonPagedPoolMustSucceed) != 0,
    __drv_reportError("Must succeed pool allocations are forbidden. "
        "Allocation failures cause a system crash"))
    PVOID
    DsmpAllocateAlignedPool(
        _In_ IN POOL_TYPE PoolType,
        _In_ IN SIZE_T NumberOfBytes,
        _In_ IN ULONG AlignmentMask,
        _Out_ OUT SIZE_T* BytesAllocated
    );

/*
//
// Define the sections that allow for discarding (i.e. paging) some of
// the code.
//

#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, DiskPerfCreate)
#pragma alloc_text (PAGE, DiskPerfAddDevice)
#pragma alloc_text (PAGE, DiskPerfDispatchPnp)
#pragma alloc_text (PAGE, DiskPerfStartDevice)
#pragma alloc_text (PAGE, DiskPerfRemoveDevice)
#pragma alloc_text (PAGE, DiskPerfUnload)
#pragma alloc_text (PAGE, DiskPerfRegisterDevice)
#pragma alloc_text (PAGE, DiskPerfSyncFilterWithTarget)
#endif*/


NTSTATUS
DriverEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegistryPath
)

/*++

Routine Description:

    Installable driver initialization entry point.
    This entry point is called directly by the I/O manager to set up the disk
    performance driver. The driver object is set up and then the Pnp manager
    calls DiskPerfAddDevice to attach to the boot devices.

Arguments:

    DriverObject - The disk performance driver object.

    RegistryPath - pointer to a unicode string representing the path,
                   to driver-specific key in the registry.

Return Value:

    STATUS_SUCCESS if successful

--*/

{

    ULONG               ulIndex;
    PDRIVER_DISPATCH* dispatch;

    //
    // Remember registry path
    //

    DiskPerfRegistryPath.MaximumLength = RegistryPath->Length
        + sizeof(UNICODE_NULL);
    DiskPerfRegistryPath.Buffer = ExAllocatePool(
        PagedPool,
        DiskPerfRegistryPath.MaximumLength);
    if (DiskPerfRegistryPath.Buffer != NULL)
    {
        RtlCopyUnicodeString(&DiskPerfRegistryPath, RegistryPath);
    }
    else {
        DiskPerfRegistryPath.Length = 0;
        DiskPerfRegistryPath.MaximumLength = 0;
    }

    //
    // Create dispatch points
    //
    for (ulIndex = 0, dispatch = DriverObject->MajorFunction;
        ulIndex <= IRP_MJ_MAXIMUM_FUNCTION;
        ulIndex++, dispatch++) {

        *dispatch = DiskPerfSendToNextDriver;
    }

    //
    // Set up the device driver entry points.
    //

    DriverObject->MajorFunction[IRP_MJ_CREATE] = DiskPerfCreate;
    DriverObject->MajorFunction[IRP_MJ_READ] = DiskPerfReadWrite;
    DriverObject->MajorFunction[IRP_MJ_WRITE] = DiskPerfReadWrite;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DiskPerfDeviceControl;
    //DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = DiskPerfWmi;

    DriverObject->MajorFunction[IRP_MJ_SHUTDOWN] = DiskPerfShutdownFlush;
    DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS] = DiskPerfShutdownFlush;
    DriverObject->MajorFunction[IRP_MJ_PNP] = DiskPerfDispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = DiskPerfDispatchPower;

    DriverObject->DriverExtension->AddDevice = DiskPerfAddDevice;
    DriverObject->DriverUnload = DiskPerfUnload;

    return(STATUS_SUCCESS);

} // end DriverEntry()

#define FILTER_DEVICE_PROPOGATE_FLAGS            0
#define FILTER_DEVICE_PROPOGATE_CHARACTERISTICS (FILE_REMOVABLE_MEDIA |  \
                                                 FILE_READ_ONLY_DEVICE | \
                                                 FILE_FLOPPY_DISKETTE    \
                                                 )

VOID
DiskPerfSyncFilterWithTarget(
    IN PDEVICE_OBJECT FilterDevice,
    IN PDEVICE_OBJECT TargetDevice
)
{
    ULONG                   propFlags;

    PAGED_CODE();

    //
    // Propogate all useful flags from target to diskperf. MountMgr will look
    // at the diskperf object capabilities to figure out if the disk is
    // a removable and perhaps other things.
    //
    propFlags = TargetDevice->Flags & FILTER_DEVICE_PROPOGATE_FLAGS;
    FilterDevice->Flags |= propFlags;

    propFlags = TargetDevice->Characteristics & FILTER_DEVICE_PROPOGATE_CHARACTERISTICS;
    FilterDevice->Characteristics |= propFlags;
}

NTSTATUS
DiskPerfAddDevice(
    IN PDRIVER_OBJECT DriverObject,
    IN PDEVICE_OBJECT PhysicalDeviceObject
)
/*++
Routine Description:

    Creates and initializes a new filter device object FiDO for the
    corresponding PDO.  Then it attaches the device object to the device
    stack of the drivers for the device.

Arguments:

    DriverObject - Disk performance driver object.
    PhysicalDeviceObject - Physical Device Object from the underlying layered driver

Return Value:

    NTSTATUS
--*/

{
    NTSTATUS                status;
    PDEVICE_OBJECT          filterDeviceObject;
    PDEVICE_EXTENSION       deviceExtension;
    //PCHAR                   buffer;
    //ULONG                   buffersize;

    PAGED_CODE();

    //
    // Create a filter device object for this device (partition).
    //

    DebugPrint((2, "DiskPerfAddDevice: DriverObject 0x%p DeviceObject 0x%p\n",
        DriverObject, PhysicalDeviceObject));

    status = IoCreateDevice(DriverObject,
        DEVICE_EXTENSION_SIZE,
        NULL,
        FILE_DEVICE_DISK,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &filterDeviceObject);

    if (!NT_SUCCESS(status)) {
        DebugPrint((1, "DiskPerfAddDevice: Cannot create filterDeviceObject\n"));
        return status;
    }

    filterDeviceObject->Flags |= DO_DIRECT_IO;

    deviceExtension = (PDEVICE_EXTENSION)filterDeviceObject->DeviceExtension;

    RtlZeroMemory(deviceExtension, DEVICE_EXTENSION_SIZE);

    //
    // Attaches the device object to the highest device object in the chain and
    // return the previously highest device object, which is passed to
    // IoCallDriver when pass IRPs down the device stack
    //

    deviceExtension->PhysicalDeviceObject = PhysicalDeviceObject;

    deviceExtension->TargetDeviceObject =
        IoAttachDeviceToDeviceStack(filterDeviceObject, PhysicalDeviceObject);

    if (deviceExtension->TargetDeviceObject == NULL) {
        ExFreePool(deviceExtension->DiskCounters);
        deviceExtension->DiskCounters = NULL;
        IoDeleteDevice(filterDeviceObject);
        DebugPrint((1, "DiskPerfAddDevice: Unable to attach 0x%p to target 0x%p\n",
            filterDeviceObject, PhysicalDeviceObject));
        return STATUS_NO_SUCH_DEVICE;
    }


    //
    // Initialise the remove lock
    //

    IoInitializeRemoveLock(&deviceExtension->RemoveLock, 'repD', 1, 0);

    //
    // Save the filter device object in the device extension
    //
    deviceExtension->DeviceObject = filterDeviceObject;

    deviceExtension->PhysicalDeviceName.Buffer
        = deviceExtension->PhysicalDeviceNameBuffer;

    KeInitializeEvent(&deviceExtension->PagingPathCountEvent,
        NotificationEvent, TRUE);

    //
    // default to DO_POWER_PAGABLE
    //

    filterDeviceObject->Flags |= DO_POWER_PAGABLE;

    //
    // Clear the DO_DEVICE_INITIALIZING flag
    //

    filterDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;

} // end DiskPerfAddDevice()


NTSTATUS
DiskPerfDispatchPnp(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
)
/*++

Routine Description:

    Dispatch for PNP

Arguments:

    DeviceObject    - Supplies the device object.

    Irp             - Supplies the I/O request packet.

Return Value:

    NTSTATUS

--*/

{
    PIO_STACK_LOCATION  irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS            status = Irp->IoStatus.Status;
    PDEVICE_EXTENSION   deviceExtension = DeviceObject->DeviceExtension;
    BOOLEAN lockHeld = FALSE;

    PAGED_CODE();

    DebugPrint((2, "DiskPerfDispatchPnp: DeviceObject 0x%p Irp 0x%p\n",
        DeviceObject, Irp));

    //
    // Acquire the remove lock. If this fails, fail the I/O.
    //

    status = IoAcquireRemoveLock(&deviceExtension->RemoveLock, Irp);

    if (!NT_SUCCESS(status)) {

        DebugPrint((2, "IoAcquireRemoveLock failed: DeviceObject %p PNP Irp type [%#02x] %p Status: %!STATUS!.\n",
            DeviceObject, irpSp->MinorFunction, status));
        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        return status;
    }

    //
    // Indicate that the remove lock is held.
    //

    lockHeld = TRUE;

    switch (irpSp->MinorFunction) {

    case IRP_MN_START_DEVICE:
        //
        // Call the Start Routine handler to schedule a completion routine
        //
        DebugPrint((3,
            "DiskPerfDispatchPnp: Schedule completion for START_DEVICE\n"));
        status = DiskPerfStartDevice(DeviceObject, Irp);
        break;

    case IRP_MN_REMOVE_DEVICE:
    {
        //
        // In this case a completion routine is not required
        // Free resources, pass the IRP down to the next driver
        // Detach and Delete the device. 
        //
        DebugPrint((3,
            "DiskPerfDispatchPnp: Processing REMOVE_DEVICE\n"));
        status = DiskPerfRemoveDevice(DeviceObject, Irp);

        //
        // Remove locked released by FpFilterRemoveDevice
        //
        lockHeld = FALSE;

        break;
    }
    case IRP_MN_DEVICE_USAGE_NOTIFICATION:
    {
        PIO_STACK_LOCATION irpStack;
        BOOLEAN setPagable;

        DebugPrint((3,
            "DiskPerfDispatchPnp: Processing DEVICE_USAGE_NOTIFICATION\n"));
        irpStack = IoGetCurrentIrpStackLocation(Irp);

        if (irpStack->Parameters.UsageNotification.Type != DeviceUsageTypePaging) {
            status = DiskPerfSendToNextDriver(DeviceObject, Irp);
            IoReleaseRemoveLock(&deviceExtension->RemoveLock, Irp);
            lockHeld = FALSE;
            break; // out of case statement
        }

        deviceExtension = DeviceObject->DeviceExtension;

        //
        // wait on the paging path event
        //

        status = KeWaitForSingleObject(&deviceExtension->PagingPathCountEvent,
            Executive, KernelMode,
            FALSE, NULL);

        UNREFERENCED_PARAMETER(status);

        //
        // if removing last paging device, need to set DO_POWER_PAGABLE
        // bit here, and possible re-set it below on failure.
        //

        setPagable = FALSE;
        if (!irpStack->Parameters.UsageNotification.InPath &&
            deviceExtension->PagingPathCount == 1) {

            //
            // removing the last paging file
            // must have DO_POWER_PAGABLE bits set
            //

            if (DeviceObject->Flags & DO_POWER_INRUSH) {
                DebugPrint((3, "DiskPerfDispatchPnp: last paging file "
                    "removed but DO_POWER_INRUSH set, so not "
                    "setting PAGABLE bit "
                    "for DO %p\n", DeviceObject));
            }
            else {
                DebugPrint((2, "DiskPerfDispatchPnp: Setting  PAGABLE "
                    "bit for DO %p\n", DeviceObject));
                DeviceObject->Flags |= DO_POWER_PAGABLE;
                setPagable = TRUE;
            }

        }

        //
        // send the irp synchronously
        //

        status = DiskPerfForwardIrpSynchronous(DeviceObject, Irp);

        //
        // now deal with the failure and success cases.
        // note that we are not allowed to fail the irp
        // once it is sent to the lower drivers.
        //

        if (NT_SUCCESS(status)) {

            IoAdjustPagingPathCount(
                &deviceExtension->PagingPathCount,
                irpStack->Parameters.UsageNotification.InPath);

            if (irpStack->Parameters.UsageNotification.InPath) {
                if (deviceExtension->PagingPathCount == 1) {

                    //
                    // first paging file addition
                    //

                    DebugPrint((3, "DiskPerfDispatchPnp: Clearing PAGABLE bit "
                        "for DO %p\n", DeviceObject));
                    DeviceObject->Flags &= ~DO_POWER_PAGABLE;
                }
            }

        }
        else {

            //
            // cleanup the changes done above
            //

            if (setPagable == TRUE) {
                DeviceObject->Flags &= ~DO_POWER_PAGABLE;
                setPagable = FALSE;
            }

        }

        //
        // set the event so the next one can occur.
        //

        KeSetEvent(&deviceExtension->PagingPathCountEvent,
            IO_NO_INCREMENT, FALSE);

        //
        // and complete the irp
        //

        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        //
        // Release the remove lock
        //

        IoReleaseRemoveLock(&deviceExtension->RemoveLock, Irp);

        return status;
        break;

    }

    default:
        DebugPrint((3,
            "DiskPerfDispatchPnp: Forwarding irp\n"));

        //
        // Simply forward all other Irps
        //

        status = DiskPerfSendToNextDriver(DeviceObject, Irp);

    }


    //
    // If the lock is still held, release it now.
    //

    if (lockHeld) {

        DebugPrint((2, "DiskPerfDispatchPnp : Releasing Lock: DeviceObject 0x%p Irp 0x%p\n",
            DeviceObject, Irp));
        //
        // Release the remove lock
        //
        IoReleaseRemoveLock(&deviceExtension->RemoveLock, Irp);
    }

    return status;


} // end DiskPerfDispatchPnp()


NTSTATUS
DiskPerfIrpCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_reads_opt_(_Inexpressible_("varies")) PVOID Context
)

/*++

Routine Description:

    Forwarded IRP completion routine. Set an event and return
    STATUS_MORE_PROCESSING_REQUIRED. Irp forwarder will wait on this
    event and then re-complete the irp after cleaning up.

Arguments:

    DeviceObject is the device object of the WMI driver
    Irp is the WMI irp that was just completed
    Context is a PKEVENT that forwarder will wait on

Return Value:

    STATUS_MORE_PORCESSING_REQUIRED

--*/

{
    PKEVENT Event = (PKEVENT)Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    if (Event != NULL) {
        KeSetEvent(Event, IO_NO_INCREMENT, FALSE);
    }

    return(STATUS_MORE_PROCESSING_REQUIRED);

} // end DiskPerfIrpCompletion()


NTSTATUS
DiskPerfStartDevice(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
)
/*++

Routine Description:

    This routine is called when a Pnp Start Irp is received.
    It will schedule a completion routine to initialize and register with WMI.

Arguments:

    DeviceObject - a pointer to the device object

    Irp - a pointer to the irp


Return Value:

    Status of processing the Start Irp

--*/
{
    PDEVICE_EXTENSION   deviceExtension;
    NTSTATUS            status;

    PAGED_CODE();

    deviceExtension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;

    status = DiskPerfForwardIrpSynchronous(DeviceObject, Irp);

    DiskPerfSyncFilterWithTarget(DeviceObject,
        deviceExtension->TargetDeviceObject);

    //
    // Complete WMI registration
    //
    DiskPerfRegisterDevice(DeviceObject);

    //
    // Complete the Irp
    //
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}


NTSTATUS
DiskPerfRemoveDevice(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
)
/*++

Routine Description:

    This routine is called when the device is to be removed.
    It will de-register itself from WMI first, pass the Irp down the stack
    then detach itself from the stack before deleting itself.

Arguments:

    DeviceObject - a pointer to the device object

    Irp - a pointer to the irp


Return Value:

    Status of removing the device

--*/
{
    NTSTATUS            status;
    PDEVICE_EXTENSION   deviceExtension;

    PAGED_CODE();

    deviceExtension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;


    //
    // Call Remove lock and wait to ensure all outstanding operations
    // have completed
    //
    IoReleaseRemoveLockAndWait(&deviceExtension->RemoveLock, Irp);

    //
    // Forward the Removal Irp below as per the DDK
    // We aren't required to complete this Irp status should
    // be the return status from the next driver in the stack
    //
    status = DiskPerfSendToNextDriver(DeviceObject, Irp);

    //
    //Detach us from the stack 
    //

    IoDetachDevice(deviceExtension->TargetDeviceObject);
    IoDeleteDevice(DeviceObject);

    return status;
}


NTSTATUS
DiskPerfSendToNextDriver(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
)

/*++

Routine Description:

    This routine sends the Irp to the next driver in line
    when the Irp is not processed by this driver.

Arguments:

    DeviceObject
    Irp

Return Value:

    NTSTATUS

--*/

{
    PDEVICE_EXTENSION   deviceExtension;

    IoSkipCurrentIrpStackLocation(Irp);
    deviceExtension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;

    return IoCallDriver(deviceExtension->TargetDeviceObject, Irp);

} // end DiskPerfSendToNextDriver()

NTSTATUS
DiskPerfDispatchPower(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
)
{
    /*

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    deviceExtension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;
    
    DebugPrint((0, "POWER HO 0x%p Irp 0x%p %d %d %d\n", DeviceObject, Irp, irpSp->MinorFunction, irpSp->Parameters.Power.Type, irpSp->Parameters.Power.State.SystemState));
    if (irpSp->MinorFunction == IRP_MN_SET_POWER)
    {
        if (irpSp->Parameters.Power.Type == SystemPowerState)
        {
            //if (irpSp->Parameters.Power.State.SystemState == PowerSystemSleeping3)
            {
                deviceExtension->Sleepy = TRUE;
                DebugPrint((0, "Sleep sleep motherfucker 0x%p Irp 0x%p Sleepy %d\n", DeviceObject, Irp, deviceExtension->Sleepy));
                //KeStallExecutionProcessor(5000000);

                SEDSleepUnlockDrive(DeviceObject);
            }
        }
    }
    //KeStallExecutionProcessor(100000);

    // hopefully this is a passthrough or some shit
    IoSkipCurrentIrpStackLocation(Irp);


    return IoCallDriver(deviceExtension->TargetDeviceObject, Irp);*/


    PDEVICE_EXTENSION deviceExtension;
    NTSTATUS            status;
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    deviceExtension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;

    status = DiskPerfForwardIrpSynchronous(DeviceObject, Irp);

    if (irpSp->MinorFunction == IRP_MN_SET_POWER)
    {
        if (irpSp->Parameters.Power.Type == SystemPowerState)
        {
            if (irpSp->Parameters.Power.State.SystemState == PowerSystemWorking)
            {
                SEDSleepUnlockDrive(DeviceObject);
            }
        }
    }

    //
    // Complete the Irp
    //
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;


} // end DiskPerfDispatchPower

NTSTATUS
DiskPerfForwardIrpSynchronous(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
)

/*++

Routine Description:

    This routine sends the Irp to the next driver in line
    when the Irp needs to be processed by the lower drivers
    prior to being processed by this one.

Arguments:

    DeviceObject
    Irp

Return Value:

    NTSTATUS

--*/

{
    PDEVICE_EXTENSION   deviceExtension;
    KEVENT event;
    NTSTATUS status;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    deviceExtension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;

    //
    // copy the irpstack for the next device
    //

    IoCopyCurrentIrpStackLocationToNext(Irp);

    //
    // set a completion routine
    //

    IoSetCompletionRoutine(Irp, DiskPerfIrpCompletion,
        &event, TRUE, TRUE, TRUE);

    //
    // call the next lower device
    //

    status = IoCallDriver(deviceExtension->TargetDeviceObject, Irp);

    //
    // wait for the actual completion
    //
    __analysis_assume(status != STATUS_PENDING);
    __analysis_assume(IoGetCurrentIrpStackLocation(Irp)->MinorFunction != IRP_MN_START_DEVICE);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = Irp->IoStatus.Status;
    }

    return status;

} // end DiskPerfForwardIrpSynchronous()


NTSTATUS
DiskPerfCreate(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
)

/*++

Routine Description:

    This routine services open commands. It establishes
    the driver's existance by returning status success.

Arguments:

    DeviceObject - Context for the activity.
    Irp          - The device control argument block.

Return Value:

    NT Status

--*/

{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(DeviceObject);

    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;

} // end DiskPerfCreate()


NTSTATUS
DiskPerfReadWrite(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
)

/*++

Routine Description:

    This is the driver entry point for read and write requests
    to disks to which the diskperf driver has attached.
    This driver collects statistics and then sets a completion
    routine so that it can collect additional information when
    the request completes. Then it calls the next driver below
    it.

Arguments:

    DeviceObject
    Irp

Return Value:

    NTSTATUS

--*/

{
    PDEVICE_EXTENSION  deviceExtension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION currentIrpStack = IoGetCurrentIrpStackLocation(Irp);
    UNREFERENCED_PARAMETER(currentIrpStack);
    //LONG               queueLen;
    //PLARGE_INTEGER     timeStamp;
    NTSTATUS           status;

    //
    // As processors may be  dynamically  added, ensure
    // that there's a context for the current processor
    //
    //
    // Acquire the remove lock so that device will not be removed while
    // processing this irp.
    //
    status = IoAcquireRemoveLock(&deviceExtension->RemoveLock, Irp);

    //DebugPrint((0, "Hello, yes this is dog 0x%p Irp 0x%p\n", DeviceObject, Irp));

    if (!NT_SUCCESS(status))
    {
        DebugPrint((3, "DiskPerfReadWrite: Remove lock failed IOCTL Irp type [%x]\n",
            currentIrpStack->Parameters.DeviceIoControl.IoControlCode));
        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    //
    // Device is not initialized properly. Blindly pass the irp along
    //
    if (deviceExtension->PhysicalDeviceNameBuffer[0] == 0)
    {
        status = DiskPerfSendToNextDriver(DeviceObject, Irp);
        IoReleaseRemoveLock(&deviceExtension->RemoveLock, Irp);
        return (status);
    }

    //DebugPrint((0, "woof woof motherfucker 0x%p Irp 0x%p Sleepy %d\n", DeviceObject, Irp, deviceExtension->Sleepy));
    //
    // Increment queue depth counter.
    //

    //queueLen = InterlockedIncrement(&deviceExtension->QueueDepth);

    //
    // Copy current stack to next stack.
    //

    IoCopyCurrentIrpStackLocationToNext(Irp);


    // XXX: THIS IS A COOL PLACE TO HOOK


    //
    //
    // Return the results of the call to the disk driver.
    //

    return IoCallDriver(deviceExtension->TargetDeviceObject,
        Irp);

} // end DiskPerfReadWrite()


NTSTATUS
DiskPerfDeviceControl(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
)

/*++

Routine Description:

    This device control dispatcher handles only the disk performance
    device control. All others are passed down to the disk drivers.
    The disk performane device control returns a current snapshot of
    the performance data.

Arguments:

    DeviceObject - Context for the activity.
    Irp          - The device control argument block.

Return Value:

    Status is returned.

--*/

{
    PDEVICE_EXTENSION  deviceExtension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION currentIrpStack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS    status;


    //
    // Acquire the remove lock so that device will not be removed while
    // processing this irp.
    //
    status = IoAcquireRemoveLock(&deviceExtension->RemoveLock, Irp);

    DebugPrint((3, "DiskPerfDeviceControl: DeviceObject 0x%p Irp 0x%p Code %x\n",
        DeviceObject, Irp, currentIrpStack->Parameters.DeviceIoControl.IoControlCode));

    if (!NT_SUCCESS(status))
    {
        DebugPrint((3, "DiskPerfControl: Remove lock failed IOCTL Irp type [%x]\n",
            currentIrpStack->Parameters.DeviceIoControl.IoControlCode));
        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }


    if (currentIrpStack->Parameters.DeviceIoControl.IoControlCode ==
        IOCTL_HURR_DURR_IM_A_GOAT) {
        

        SEDSleepUnlockDrive(DeviceObject);

        //
        // Complete request.
        //

        Irp->IoStatus.Status = status;
        //
        // Release the remove lock
        //
        IoReleaseRemoveLock(&deviceExtension->RemoveLock, Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;

    }

    else {

        //
        // Set current stack back one.
        // We aren't doing anything with this IRP
        // so mark it pending
        //

        IoMarkIrpPending(Irp);
        IoSkipCurrentIrpStackLocation(Irp);

        //
        // Pass unrecognized device control requests
        // down to next driver layer. We ignore the 
        // return status since we need to return 
        // STATUS_PENDING due to the mark pending
        // call above.
        //

        IoCallDriver(deviceExtension->TargetDeviceObject, Irp);

        //
        // Release the remove lock
        //
        IoReleaseRemoveLock(&deviceExtension->RemoveLock, Irp);

        return (STATUS_PENDING);

    }
} // end DiskPerfDeviceControl()


NTSTATUS
DiskPerfShutdownFlush(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
)

/*++

Routine Description:

    This routine is called for a shutdown and flush IRPs.  These are sent by the
    system before it actually shuts down or when the file system does a flush.

Arguments:

    DriverObject - Pointer to device object to being shutdown by system.
    Irp          - IRP involved.

Return Value:

    NT Status

--*/

{
    PDEVICE_EXTENSION  deviceExtension = DeviceObject->DeviceExtension;

    DebugPrint((2, "DiskPerfShutdownFlush: DeviceObject 0x%p Irp 0x%p\n",
        DeviceObject, Irp));

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(deviceExtension->TargetDeviceObject, Irp);

} // end DiskPerfShutdownFlush()


VOID
DiskPerfUnload(
    IN PDRIVER_OBJECT DriverObject
)

/*++

Routine Description:

    Free all the allocated resources, etc.

Arguments:

    DriverObject - pointer to a driver object.

Return Value:

    VOID.

--*/
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(DriverObject);

    return;
}


NTSTATUS
DiskPerfRegisterDevice(
    IN PDEVICE_OBJECT DeviceObject
)

/*++

Routine Description:

    Routine to initialize a proper name for the device object, and
    register it with WMI

Arguments:

    DeviceObject - pointer to a device object to be initialized.

Return Value:

    Status of the initialization. NOTE: If the registration fails,
    the device name in the DeviceExtension will be left as empty.

--*/

{
    NTSTATUS                status;
    IO_STATUS_BLOCK         ioStatus;
    KEVENT                  event;
    PDEVICE_EXTENSION       deviceExtension;
    PIRP                    irp;
    STORAGE_DEVICE_NUMBER   number;
    //ULONG                   registrationFlag = 0;

    PAGED_CODE();

    DebugPrint((2, "DiskPerfRegisterDevice: DeviceObject 0x%p\n",
        DeviceObject));
    deviceExtension = DeviceObject->DeviceExtension;

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    //
    // Request for the device number
    //
    irp = IoBuildDeviceIoControlRequest(
        IOCTL_STORAGE_GET_DEVICE_NUMBER,
        deviceExtension->TargetDeviceObject,
        NULL,
        0,
        &number,
        sizeof(number),
        FALSE,
        &event,
        &ioStatus);
    if (!irp) {
        DiskPerfLogError(
            DeviceObject,
            256,
            STATUS_SUCCESS,
            IO_ERR_INSUFFICIENT_RESOURCES);
        DebugPrint((3, "DiskPerfRegisterDevice: Fail to build irp\n"));
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = IoCallDriver(deviceExtension->TargetDeviceObject, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = ioStatus.Status;
    }

    if (NT_SUCCESS(status)) {

        //
        // Remember the disk number for use as parameter in DiskIoNotifyRoutine
        //
        deviceExtension->DiskNumber = number.DeviceNumber;

        //
        // Create device name for each partition
        //

        RtlStringCbPrintfW(
            deviceExtension->PhysicalDeviceNameBuffer,
            sizeof(deviceExtension->PhysicalDeviceNameBuffer),
            L"\\Device\\Harddisk%d\\Partition%d",
            number.DeviceNumber, number.PartitionNumber);

        RtlInitUnicodeString(&deviceExtension->PhysicalDeviceName, &deviceExtension->PhysicalDeviceNameBuffer[0]);

        //
        // Set default name for physical disk
        //
        RtlCopyMemory(
            &(deviceExtension->StorageManagerName[0]),
            L"PhysDisk",
            8 * sizeof(WCHAR));
        DebugPrint((3, "DiskPerfRegisterDevice: Device name %ws\n",
            deviceExtension->PhysicalDeviceNameBuffer));
    }
    else {

        // request for partition's information failed, try volume

        ULONG           outputSize = sizeof(MOUNTDEV_NAME);
        PMOUNTDEV_NAME  output;
        VOLUME_NUMBER   volumeNumber;
        ULONG           nameSize;

        output = ExAllocatePool(PagedPool, outputSize);
        if (!output) {
            DiskPerfLogError(
                DeviceObject,
                257,
                STATUS_SUCCESS,
                IO_ERR_INSUFFICIENT_RESOURCES);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        KeInitializeEvent(&event, NotificationEvent, FALSE);
        irp = IoBuildDeviceIoControlRequest(
            IOCTL_MOUNTDEV_QUERY_DEVICE_NAME,
            deviceExtension->TargetDeviceObject, NULL, 0,
            output, outputSize, FALSE, &event, &ioStatus);
        if (!irp) {
            ExFreePool(output);
            DiskPerfLogError(
                DeviceObject,
                258,
                STATUS_SUCCESS,
                IO_ERR_INSUFFICIENT_RESOURCES);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        status = IoCallDriver(deviceExtension->TargetDeviceObject, irp);
        if (status == STATUS_PENDING) {
            KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
            status = ioStatus.Status;
        }

        if (status == STATUS_BUFFER_OVERFLOW) {
            outputSize = sizeof(MOUNTDEV_NAME) + output->NameLength;
            ExFreePool(output);
            output = ExAllocatePool(PagedPool, outputSize);

            if (!output) {
                DiskPerfLogError(
                    DeviceObject,
                    258,
                    STATUS_SUCCESS,
                    IO_ERR_INSUFFICIENT_RESOURCES);
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            KeInitializeEvent(&event, NotificationEvent, FALSE);
            irp = IoBuildDeviceIoControlRequest(
                IOCTL_MOUNTDEV_QUERY_DEVICE_NAME,
                deviceExtension->TargetDeviceObject, NULL, 0,
                output, outputSize, FALSE, &event, &ioStatus);
            if (!irp) {
                ExFreePool(output);
                DiskPerfLogError(
                    DeviceObject, 259,
                    STATUS_SUCCESS,
                    IO_ERR_INSUFFICIENT_RESOURCES);
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            status = IoCallDriver(deviceExtension->TargetDeviceObject, irp);
            if (status == STATUS_PENDING) {
                KeWaitForSingleObject(
                    &event,
                    Executive,
                    KernelMode,
                    FALSE,
                    NULL
                );
                status = ioStatus.Status;
            }
        }
        if (!NT_SUCCESS(status)) {
            ExFreePool(output);
            DiskPerfLogError(
                DeviceObject,
                260,
                STATUS_SUCCESS,
                IO_ERR_CONFIGURATION_ERROR);
            return status;
        }

        //
        // Since we get the volume name instead of the disk number,
        // set it to a dummy value
        // Todo: Instead of passing the disk number back to the user app.
        // for tracing, pass the STORAGE_DEVICE_NUMBER structure instead.

        deviceExtension->DiskNumber = (ULONG)-1;

        nameSize = min(output->NameLength, sizeof(deviceExtension->PhysicalDeviceNameBuffer) - sizeof(WCHAR));

        RtlStringCbCopyW(deviceExtension->PhysicalDeviceNameBuffer, nameSize, output->Name);

        RtlInitUnicodeString(&deviceExtension->PhysicalDeviceName, &deviceExtension->PhysicalDeviceNameBuffer[0]);

        ExFreePool(output);

        //
        // Now, get the VOLUME_NUMBER information
        //
        outputSize = sizeof(VOLUME_NUMBER);
        RtlZeroMemory(&volumeNumber, outputSize);

        KeInitializeEvent(&event, NotificationEvent, FALSE);
        irp = IoBuildDeviceIoControlRequest(
            IOCTL_VOLUME_QUERY_VOLUME_NUMBER,
            deviceExtension->TargetDeviceObject, NULL, 0,
            &volumeNumber,
            sizeof(VOLUME_NUMBER),
            FALSE, &event, &ioStatus);
        if (!irp) {
            DiskPerfLogError(
                DeviceObject,
                265,
                STATUS_SUCCESS,
                IO_ERR_INSUFFICIENT_RESOURCES);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        status = IoCallDriver(deviceExtension->TargetDeviceObject, irp);
        if (status == STATUS_PENDING) {
            KeWaitForSingleObject(&event, Executive,
                KernelMode, FALSE, NULL);
            status = ioStatus.Status;
        }
        if (!NT_SUCCESS(status) ||
            volumeNumber.VolumeManagerName[0] == (WCHAR)UNICODE_NULL) {

            RtlCopyMemory(
                &deviceExtension->StorageManagerName[0],
                L"LogiDisk",
                8 * sizeof(WCHAR));
            if (NT_SUCCESS(status))
                deviceExtension->DiskNumber = volumeNumber.VolumeNumber;
        }
        else {
            RtlCopyMemory(
                &deviceExtension->StorageManagerName[0],
                &volumeNumber.VolumeManagerName[0],
                8 * sizeof(WCHAR));
            deviceExtension->DiskNumber = volumeNumber.VolumeNumber;
        }
        DebugPrint((3, "DiskPerfRegisterDevice: Device name %ws\n",
            deviceExtension->PhysicalDeviceNameBuffer));
    }


    if (!NT_SUCCESS(status)) {
        DiskPerfLogError(
            DeviceObject,
            261,
            STATUS_SUCCESS,
            IO_ERR_INTERNAL_ERROR);
    }
    return status;
}


VOID
DiskPerfLogError(
    IN PDEVICE_OBJECT DeviceObject,
    IN ULONG UniqueId,
    IN NTSTATUS ErrorCode,
    IN NTSTATUS Status
)

/*++

Routine Description:

    Routine to log an error with the Error Logger

Arguments:

    DeviceObject - the device object responsible for the error
    UniqueId     - an id for the error
    Status       - the status of the error

Return Value:

    None

--*/

{
    PIO_ERROR_LOG_PACKET errorLogEntry = NULL;
    PVOID* DeviceObjectPtr = NULL;

    //
    // Check to make sure that the Log Entry is large enough to hold the Entry
    // The size of the Log Data Packet cannot be larger than 255 Bytes . 
    //

    errorLogEntry = (PIO_ERROR_LOG_PACKET)
        IoAllocateErrorLogEntry(
            DeviceObject,
            (UCHAR)(sizeof(IO_ERROR_LOG_PACKET) + sizeof(PDEVICE_OBJECT))
        );

    if (errorLogEntry != NULL) {
        errorLogEntry->ErrorCode = ErrorCode;
        errorLogEntry->UniqueErrorValue = UniqueId;
        errorLogEntry->FinalStatus = Status;
        errorLogEntry->DumpDataSize = sizeof(PDEVICE_OBJECT);
        //
        // Log the Device_Object Pointer as the dumpdata
        //
        DeviceObjectPtr = (PVOID*)errorLogEntry->DumpData;
        *DeviceObjectPtr = DeviceObject;

        IoWriteErrorLogEntry(errorLogEntry);
    }
}

#if DBG

VOID
DiskPerfDebugPrint(
    ULONG DebugPrintLevel,
    PCCHAR DebugMessage,
    ...
)

/*++

Routine Description:

    Debug print for all DiskPerf
    Since this is using the old style of debug prints the following is true :

    Debug levels are bit masks and are not cumulative. So if you want to see
    All errors and warnings you need to have bits 0 and 1 set.

    Mask for DISKPERF is in variable nt!Kd_DISKPERF_Mask

    The Registry can be setup with initial mask value for nt!Kd_DISKPERF_Mask by
    setting up a DWORD value named DISKPERF under key
    HKLM\System\CurrentControlSet\Control\Session Manager\Debug Print Filter

    Alternatively the nt!kd_DEFAULT_mask can be used since all drivers without a
    registry key entry are in the DEFAULT bucket

Arguments:

    Debug print level between 0 and 3, with 3 being the most verbose.

Return Value:

    None

--*/

{
    va_list ap;

    va_start(ap, DebugMessage);

    if ((DebugPrintLevel <= (DiskPerfDebug & 0x0000ffff)) ||
        ((1 << (DebugPrintLevel + 15)) & DiskPerfDebug)) {

        DbgPrint(DebugMessage, ap);
    }

    va_end(ap);

}
#endif
/*
// from some random google search
#define IfPrint(c) (c >= 32 && c < 127 ? c : '.')
void HexDump(unsigned char* p_Buffer, unsigned long p_Size)
{
    unsigned long l_Index;// , l_Idx;
    unsigned char l_Row[17];

    for (l_Index = l_Row[16] = 0; l_Index < p_Size || l_Index % 16; ++l_Index)
    {
        if (l_Index % 16 == 0) DbgPrint("%05x   ", l_Index);
        DbgPrint("%02x ", l_Row[l_Index % 16] = (l_Index < p_Size ? p_Buffer[l_Index] : 0));
        l_Row[l_Index % 16] = IfPrint(l_Row[l_Index % 16]);
        if ((l_Index + 1) % 16 == 0) DbgPrint("   %s\n", l_Row);
    }

    DbgPrint("\n");
}
*/

void HexDump(unsigned char* Bfr, size_t Count)
{
    for (; Count >= 16; Count -= 16, Bfr += 16)
    {
        KdPrint((" %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
            Bfr[0], Bfr[1], Bfr[2], Bfr[3], Bfr[4], Bfr[5], Bfr[6], Bfr[7],
            Bfr[8], Bfr[9], Bfr[10], Bfr[11], Bfr[12], Bfr[13], Bfr[14],
            Bfr[15]
            ));
    }

    if (Count)
    {
        char sz[80];
        for (int i = 0; i < Count; i++)
            sprintf(sz + i * 3, " %02x", Bfr[i]);
        KdPrint(("%s\n", sz));
    }
}


VOID SEDSleepUnlockDrive(
    IN PDEVICE_OBJECT DeviceObject
)
{
    SEDSleepSendOPALCommand(DeviceObject, send7_bin, send7_bin_len);
    SEDSleepSendOPALCommand(DeviceObject, send7mbr_bin, send7mbr_bin_len);
}

VOID SEDSleepSendOPALCommand(
    IN PDEVICE_OBJECT DeviceObject,
    UCHAR* src,
    size_t len
)
{
    DebugPrint((0, "Oh boi gonna send me some SCSI commands\n"));
    PDEVICE_EXTENSION deviceExtension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;
    SEDSleepSendSCSICommand(DeviceObject, IF_SEND, 1, 4100, send5_bin, send5_bin_len);
    SEDSleepSendSCSICommand(DeviceObject, IF_RECV, 1, 4100, NULL, 0);
    USHORT idThing;
    memcpy(&idThing, deviceExtension->ScsiRecvBuffer + 84, sizeof(idThing));
    DbgPrint("Got ID thing %x\n", idThing);
    memcpy(src + 22, &idThing, sizeof(idThing));
    memcpy(send9_bin + 22, &idThing, sizeof(idThing));
    SEDSleepSendSCSICommand(DeviceObject, IF_SEND, 1, 4100, src, len);
    SEDSleepSendSCSICommand(DeviceObject, IF_RECV, 1, 4100, NULL, 0);
    DebugPrint((0, "Send9...\n"));
    SEDSleepSendSCSICommand(DeviceObject, IF_SEND, 1, 4100, send9_bin, send9_bin_len);
    SEDSleepSendSCSICommand(DeviceObject, IF_RECV, 1, 4100, NULL, 0);
    HexDump(
        deviceExtension->ScsiRecvBuffer,
        SEDSLEEP_SCSI_BUFFER_SIZE);
}

VOID SEDSleepSendSCSICommand(
    IN PDEVICE_OBJECT DeviceObject,
    ATACOMMAND cmd,
    UCHAR protocol, 
    USHORT comID,
    const void* src,
    size_t len
)
{    
    IO_STATUS_BLOCK         ioStatus;
    KEVENT                  event;
    NTSTATUS status;
    PDEVICE_EXTENSION deviceExtension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;
    SIZE_T allocatedLength = 0;


    // Windows structure with 32-bit sense data
    typedef struct
    {
        SCSI_PASS_THROUGH_DIRECT Sptd;
        UCHAR sense[32];
    }
    SptdStruct;

    SptdStruct sptdS = { 0 };


    // initialize SCSI CDB
    switch (cmd)
    {
        default:
        {
            DbgPrint("SEDSleepSendSCSICommand: Bad command %x", cmd);
            return;
        }

        case IF_RECV:
        {
            memset(deviceExtension->ScsiSendBuffer, 0, SEDSLEEP_SCSI_BUFFER_SIZE);
            len = SEDSLEEP_SCSI_BUFFER_SIZE;

            sptdS.Sptd.Cdb[0] = 0xA2;                                /* Opcode */
            sptdS.Sptd.Cdb[1] = protocol;                            /* Security Protocol */
            sptdS.Sptd.Cdb[2] = comID >> 8;                          /* Security Protocol Specific - MSB */
            sptdS.Sptd.Cdb[3] = comID & 0xFF;                        /* Security Protocol Specific - LSB */
            sptdS.Sptd.Cdb[4] = 0x80;                                /* INC 512 */
            sptdS.Sptd.Cdb[6] = (UCHAR)((len / 512) >> 24);             /* Allocation Length - MSB */
            sptdS.Sptd.Cdb[7] = (UCHAR)(((len / 512) >> 16) & 0xFF);
            sptdS.Sptd.Cdb[8] = (UCHAR)(((len / 512) >> 8) & 0xFF);
            sptdS.Sptd.Cdb[9] = (UCHAR)((len / 512) & 0xFF);            /* Allocation Length - LSB */
            break;
        }

        case IF_SEND:
        {
            memcpy(deviceExtension->ScsiSendBuffer, src, len);
            memset(deviceExtension->ScsiSendBuffer + len, 0, SEDSLEEP_SCSI_BUFFER_SIZE - len);

            sptdS.Sptd.Cdb[0] = 0xB5;                                /* Opcode */
            sptdS.Sptd.Cdb[1] = protocol;                            /* Security Protocol */
            sptdS.Sptd.Cdb[2] = comID >> 8;                          /* Security Protocol Specific - MSB */
            sptdS.Sptd.Cdb[3] = comID & 0xFF;                        /* Security Protocol Specific - LSB */
            sptdS.Sptd.Cdb[4] = 0x80;                                /* INC 512 */
            sptdS.Sptd.Cdb[6] = (UCHAR)((len / 512) >> 24);             /* Transfer Length - MSB */
            sptdS.Sptd.Cdb[7] = (UCHAR)(((len / 512) >> 16) & 0xFF);
            sptdS.Sptd.Cdb[8] = (UCHAR)(((len / 512) >> 8) & 0xFF);
            sptdS.Sptd.Cdb[9] = (UCHAR)((len / 512) & 0xFF);            /* Transfer Length - LSB */
            break;
        }
    }

    PUCHAR dataBuffer = DsmpAllocateAlignedPool(NonPagedPoolNx,
        len,
        DeviceObject->AlignmentRequirement,
        &allocatedLength);
    memcpy(dataBuffer, deviceExtension->ScsiSendBuffer, len);

    sptdS.Sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    sptdS.Sptd.CdbLength = 12;
    sptdS.Sptd.DataIn = (cmd == IF_RECV) ? SCSI_IOCTL_DATA_IN : SCSI_IOCTL_DATA_OUT;
    sptdS.Sptd.SenseInfoLength = sizeof(sptdS.sense);
    sptdS.Sptd.DataTransferLength = (ULONG)len;
    sptdS.Sptd.TimeOutValue = 2;
    sptdS.Sptd.DataBuffer = dataBuffer;
    sptdS.Sptd.SenseInfoOffset = offsetof(SptdStruct, sense);

#if 0
    PFILE_OBJECT driveFile;
    PDEVICE_OBJECT driveDevice;
    static UNICODE_STRING driveName = RTL_CONSTANT_STRING(L"\\??\\PhysicalDrive0");
    //RtlInitUnicodeString(&driveName, L);
    status = IoGetDeviceObjectPointer(&driveName, FILE_READ_DATA | FILE_WRITE_DATA, &driveFile, &driveDevice);

    if (!NT_SUCCESS(status))
    {
        DebugPrint((0, "SEDSleepSendSCSICommand: IoGetDeviceObjectPointer exploded"));
        return;
    }
#else
    PDEVICE_OBJECT driveDevice = deviceExtension->TargetDeviceObject;
#endif

    //memset(deviceExtension->ScsiRecvBuffer, 0, SEDSLEEP_SCSI_BUFFER_SIZE);

    KeInitializeEvent(&event, SynchronizationEvent, FALSE);
    PIRP irp = IoBuildDeviceIoControlRequest(
        IOCTL_SCSI_PASS_THROUGH_DIRECT,
        driveDevice,
        &sptdS,
        sizeof(sptdS),
        &sptdS,
        sizeof(sptdS),
        FALSE,
        &event,
        &ioStatus
    );
    if (!irp) 
    {
        DebugPrint((0, "SEDSleepSendSCSICommand: Fail to build irp\n"));
        return;
    }
    status = IoCallDriver(driveDevice, irp);
    if (status == STATUS_PENDING) 
    {
        DebugPrint((0, "SEDSleepSendSCSICommand: Pending so we waiting\n"));
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        DebugPrint((0, "SEDSleepSendSCSICommand: Finished waiting\n"));
        status = ioStatus.Status;
    }
    if (sptdS.Sptd.ScsiStatus != 0 || !NT_SUCCESS(status))
    {
        DbgPrint("SEDSleepSendSCSICommand: ScsiStatus was %x, status was %x", sptdS.Sptd.ScsiStatus, status);
        DbgPrint("SEDSleepSendSCSICommand: CDB:");
        HexDump(sptdS.Sptd.Cdb, sizeof(sptdS.Sptd.Cdb));
        DbgPrint("SEDSleepSendSCSICommand: Sense:");
        HexDump(sptdS.sense, sizeof(sptdS.sense));
        DbgPrint("SEDSleepSendSCSICommand: Hurr:");
        HexDump((UCHAR*)(&sptdS), sizeof(sptdS));
        return;
    }
    memcpy(deviceExtension->ScsiRecvBuffer, dataBuffer, len);

    ExFreePool(dataBuffer);

    DebugPrint((0, "SEDSleepSendSCSICommand: It worked I think\n"));
}



_Success_(return != NULL)
_Post_maybenull_
_Must_inspect_result_
__drv_allocatesMem(Mem)
_Post_writable_byte_size_(*BytesAllocated)
_When_(((PoolType & 0x1)) != 0, _IRQL_requires_max_(APC_LEVEL))
_When_(((PoolType & 0x1)) == 0, _IRQL_requires_max_(DISPATCH_LEVEL))
_When_((PoolType& NonPagedPoolMustSucceed) != 0,
    __drv_reportError("Must succeed pool allocations are forbidden. "
        "Allocation failures cause a system crash"))
    PVOID
#pragma warning(suppress:28195) // Allocation is not guaranteed, caller needs to check return value
    DsmpAllocateAlignedPool(
        _In_ IN POOL_TYPE PoolType,
        _In_ IN SIZE_T NumberOfBytes,
        _In_ IN ULONG AlignmentMask,
        _Out_ OUT SIZE_T* BytesAllocated
    )
    /*+++
    Routine Description :
        Allocates memory from the specified pool using the given tag and alignment requirement.
        If the allocation is successful, the entire buffer will be zeroed.
    Arguements:
        PoolType - Pool to allocate from (NonPaged, Paged, etc)
        NumberOfBytes - Size of the buffer to allocate
        AlignmentMask - Alignment requirement specified by the device
        Tag - Tag (DSM_TAG_XXX) to be used for this allocation.
              These tags are defined in msdsm.h
        BytesAllocated - Returns the number of bytes allocated, if the routine was successful
    Return Value:
        Pointer to the buffer if allocation is successful
        NULL otherwise
    --*/
{
    PVOID Block = NULL;
    UINT_PTR align64 = (UINT_PTR)AlignmentMask;
    ULONG totalSize = (ULONG)NumberOfBytes;
    NTSTATUS status = STATUS_SUCCESS;

    if (BytesAllocated == NULL) {

        status = STATUS_INVALID_PARAMETER;
        goto __Exit;
    }

    *BytesAllocated = 0;

    if (AlignmentMask) {

        status = RtlULongAdd((ULONG)NumberOfBytes, AlignmentMask, &totalSize);
    }

    if (NT_SUCCESS(status)) {

#pragma warning(suppress: 6014 28118) // Block isn't leaked, this function is marked as an allocator; PoolType is simply passed through
        Block = ExAllocatePool(PoolType, totalSize);

        if (Block != NULL) {

            if (AlignmentMask) {

                Block = (PVOID)(((UINT_PTR)Block + align64) & ~align64);
            }
        }
        else {

            status = STATUS_INSUFFICIENT_RESOURCES;
        }
    }

__Exit:

    if (NT_SUCCESS(status)) {

        RtlZeroMemory(Block, totalSize);
        *BytesAllocated = totalSize;
    }
    
    return Block;
}

