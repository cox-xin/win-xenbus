/* Copyright (c) Citrix Systems Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source 1and binary forms, 
 * with or without modification, are permitted provided 
 * that the following conditions are met:
 * 
 * *   Redistributions of source code must retain the above 
 *     copyright notice, this list of conditions and the23 
 *     following disclaimer.
 * *   Redistributions in binary form must reproduce the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer in the documentation and/or other 
 *     materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND 
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, 
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR 
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING 
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE.
 */

#define INITGUID 1

#include <ntddk.h>
#include <wdmguid.h>
#include <ntstrsafe.h>
#include <stdlib.h>
#include <util.h>
#include <xen.h>

#include <unplug_interface.h>

#include "names.h"
#include "registry.h"
#include "fdo.h"
#include "pdo.h"
#include "thread.h"
#include "mutex.h"
#include "shared_info.h"
#include "evtchn.h"
#include "debug.h"
#include "store.h"
#include "cache.h"
#include "gnttab.h"
#include "suspend.h"
#include "sync.h"
#include "balloon.h"
#include "driver.h"
#include "range_set.h"
#include "dbg_print.h"
#include "assert.h"

#define XENBUS_FDO_TAG 'ODF'

#define MAXNAMELEN  128

typedef enum _XENBUS_RESOURCE_TYPE {
    MEMORY_RESOURCE = 0,
    INTERRUPT_RESOURCE,
    RESOURCE_COUNT
} XENBUS_RESOURCE_TYPE, *PXENBUS_RESOURCE_TYPE;

typedef struct _XENBUS_RESOURCE {
    CM_PARTIAL_RESOURCE_DESCRIPTOR Raw;
    CM_PARTIAL_RESOURCE_DESCRIPTOR Translated;
} XENBUS_RESOURCE, *PXENBUS_RESOURCE;

struct _XENBUS_FDO {
    PXENBUS_DX                      Dx;
    PDEVICE_OBJECT                  LowerDeviceObject;
    PDEVICE_OBJECT                  PhysicalDeviceObject;
    DEVICE_CAPABILITIES             LowerDeviceCapabilities;
    BUS_INTERFACE_STANDARD          LowerBusInterface;
    ULONG                           Usage[DeviceUsageTypeDumpFile + 1];
    BOOLEAN                         NotDisableable;

    PXENBUS_THREAD                  SystemPowerThread;
    PIRP                            SystemPowerIrp;
    PXENBUS_THREAD                  DevicePowerThread;
    PIRP                            DevicePowerIrp;

    CHAR                            VendorName[MAXNAMELEN];
    BOOLEAN                         Active;

    MUTEX                           Mutex;
    ULONG                           References;

    PXENBUS_THREAD                  ScanThread;
    KEVENT                          ScanEvent;
    PXENBUS_STORE_WATCH             ScanWatch;

    PXENBUS_THREAD                  SuspendThread;
    KEVENT                          SuspendEvent;
    PXENBUS_STORE_WATCH             SuspendWatch;

    PXENBUS_THREAD                  BalloonThread;
    KEVENT                          BalloonEvent;
    PXENBUS_STORE_WATCH             BalloonWatch;
    MUTEX                           BalloonSuspendMutex;

    XENBUS_RESOURCE                 Resource[RESOURCE_COUNT];
    PKINTERRUPT                     InterruptObject;

    PXENBUS_SUSPEND_CONTEXT         SuspendContext;
    PXENBUS_SHARED_INFO_CONTEXT     SharedInfoContext;
    PXENBUS_EVTCHN_CONTEXT          EvtchnContext;
    PXENBUS_DEBUG_CONTEXT           DebugContext;
    PXENBUS_STORE_CONTEXT           StoreContext;
    PXENBUS_RANGE_SET_CONTEXT       RangeSetContext;
    PXENBUS_CACHE_CONTEXT           CacheContext;
    PXENBUS_GNTTAB_CONTEXT          GnttabContext;
    PXENBUS_BALLOON_CONTEXT         BalloonContext;

    XENBUS_DEBUG_INTERFACE          DebugInterface;
    XENBUS_SUSPEND_INTERFACE        SuspendInterface;
    XENBUS_EVTCHN_INTERFACE         EvtchnInterface;
    XENBUS_STORE_INTERFACE          StoreInterface;
    XENBUS_RANGE_SET_INTERFACE      RangeSetInterface;
    XENBUS_BALLOON_INTERFACE        BalloonInterface;
    XENFILT_UNPLUG_INTERFACE        UnplugInterface;

    PXENBUS_EVTCHN_CHANNEL          Channel;
    PXENBUS_SUSPEND_CALLBACK        SuspendCallbackLate;
    PXENBUS_RANGE_SET               RangeSet;
};

static FORCEINLINE PVOID
__FdoAllocate(
    IN  ULONG   Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, XENBUS_FDO_TAG);
}

static FORCEINLINE VOID
__FdoFree(
    IN  PVOID   Buffer
    )
{
    ExFreePoolWithTag(Buffer, XENBUS_FDO_TAG);
}

static FORCEINLINE VOID
__FdoSetDevicePnpState(
    IN  PXENBUS_FDO         Fdo,
    IN  DEVICE_PNP_STATE    State
    )
{
    PXENBUS_DX              Dx = Fdo->Dx;

    // We can never transition out of the deleted state
    ASSERT(Dx->DevicePnpState != Deleted || State == Deleted);

    Dx->PreviousDevicePnpState = Dx->DevicePnpState;
    Dx->DevicePnpState = State;
}

static FORCEINLINE VOID
__FdoRestoreDevicePnpState(
    IN  PXENBUS_FDO         Fdo,
    IN  DEVICE_PNP_STATE    State
    )
{
    PXENBUS_DX              Dx = Fdo->Dx;

    if (Dx->DevicePnpState == State)
        Dx->DevicePnpState = Dx->PreviousDevicePnpState;
}

static FORCEINLINE DEVICE_PNP_STATE
__FdoGetDevicePnpState(
    IN  PXENBUS_FDO Fdo
    )
{
    PXENBUS_DX      Dx = Fdo->Dx;

    return Dx->DevicePnpState;
}

static FORCEINLINE VOID
__FdoSetDevicePowerState(
    IN  PXENBUS_FDO         Fdo,
    IN  DEVICE_POWER_STATE  State
    )
{
    PXENBUS_DX              Dx = Fdo->Dx;

    Dx->DevicePowerState = State;
}

static FORCEINLINE DEVICE_POWER_STATE
__FdoGetDevicePowerState(
    IN  PXENBUS_FDO Fdo
    )
{
    PXENBUS_DX      Dx = Fdo->Dx;

    return Dx->DevicePowerState;
}

static FORCEINLINE VOID
__FdoSetSystemPowerState(
    IN  PXENBUS_FDO         Fdo,
    IN  SYSTEM_POWER_STATE  State
    )
{
    PXENBUS_DX              Dx = Fdo->Dx;

    Dx->SystemPowerState = State;
}

static FORCEINLINE SYSTEM_POWER_STATE
__FdoGetSystemPowerState(
    IN  PXENBUS_FDO Fdo
    )
{
    PXENBUS_DX      Dx = Fdo->Dx;

    return Dx->SystemPowerState;
}

static FORCEINLINE PDEVICE_OBJECT
__FdoGetPhysicalDeviceObject(
    IN  PXENBUS_FDO Fdo
    )
{
    return Fdo->PhysicalDeviceObject;
}

PDEVICE_OBJECT
FdoGetPhysicalDeviceObject(
    IN  PXENBUS_FDO Fdo
    )
{
    return __FdoGetPhysicalDeviceObject(Fdo);
}

PDMA_ADAPTER
FdoGetDmaAdapter(
    IN  PXENBUS_FDO         Fdo,
    IN  PDEVICE_DESCRIPTION DeviceDescriptor,
    OUT PULONG              NumberOfMapRegisters
    )
{
    PBUS_INTERFACE_STANDARD LowerBusInterface;

    LowerBusInterface = &Fdo->LowerBusInterface;

    return LowerBusInterface->GetDmaAdapter(LowerBusInterface->Context,
                                            DeviceDescriptor,
                                            NumberOfMapRegisters);
}

BOOLEAN
FdoTranslateBusAddress(
    IN      PXENBUS_FDO         Fdo,
    IN      PHYSICAL_ADDRESS    BusAddress,
    IN      ULONG               Length,
    IN OUT  PULONG              AddressSpace,
    OUT     PPHYSICAL_ADDRESS   TranslatedAddress
    )
{
    PBUS_INTERFACE_STANDARD LowerBusInterface;

    LowerBusInterface = &Fdo->LowerBusInterface;

    return LowerBusInterface->TranslateBusAddress(LowerBusInterface->Context,
                                                  BusAddress,
                                                  Length,
                                                  AddressSpace,
                                                  TranslatedAddress);
}

ULONG
FdoSetBusData(
    IN  PXENBUS_FDO     Fdo,
    IN  ULONG           DataType,
    IN  PVOID           Buffer,
    IN  ULONG           Offset,
    IN  ULONG           Length
    )
{
    PBUS_INTERFACE_STANDARD LowerBusInterface;

    LowerBusInterface = &Fdo->LowerBusInterface;

    return LowerBusInterface->SetBusData(LowerBusInterface->Context,
                                         DataType,
                                         Buffer,
                                         Offset,
                                         Length);
}

ULONG
FdoGetBusData(
    IN  PXENBUS_FDO     Fdo,
    IN  ULONG           DataType,
    IN  PVOID           Buffer,
    IN  ULONG           Offset,
    IN  ULONG           Length
    )
{
    PBUS_INTERFACE_STANDARD LowerBusInterface;

    LowerBusInterface = &Fdo->LowerBusInterface;

    return LowerBusInterface->GetBusData(LowerBusInterface->Context,
                                         DataType,
                                         Buffer,
                                         Offset,
                                         Length);
}

static FORCEINLINE VOID
__FdoSetVendorName(
    IN  PXENBUS_FDO Fdo,
    IN  USHORT      DeviceID
    )
{
    NTSTATUS        status;

    status = RtlStringCbPrintfA(Fdo->VendorName,
                                MAXNAMELEN,
                                "XS%04X",
                                DeviceID);
    ASSERT(NT_SUCCESS(status));
}

static FORCEINLINE PCHAR
__FdoGetVendorName(
    IN  PXENBUS_FDO Fdo
    )
{
    return Fdo->VendorName;
}

PCHAR
FdoGetVendorName(
    IN  PXENBUS_FDO Fdo
    )
{
    return __FdoGetVendorName(Fdo);
}

static FORCEINLINE VOID
__FdoSetName(
    IN  PXENBUS_FDO Fdo
    )
{
    PXENBUS_DX      Dx = Fdo->Dx;
    NTSTATUS        status;

    status = RtlStringCbPrintfA(Dx->Name,
                                MAXNAMELEN,
                                "%s XENBUS",
                                __FdoGetVendorName(Fdo));
    ASSERT(NT_SUCCESS(status));
}

static FORCEINLINE PCHAR
__FdoGetName(
    IN  PXENBUS_FDO Fdo
    )
{
    PXENBUS_DX      Dx = Fdo->Dx;

    return Dx->Name;
}

PCHAR
FdoGetName(
    IN  PXENBUS_FDO Fdo
    )
{
    return __FdoGetName(Fdo);
}

static FORCEINLINE VOID
__FdoSetActive(
    IN  PXENBUS_FDO Fdo,
    IN  BOOLEAN     Active
    )
{
    Fdo->Active = Active;
}

static FORCEINLINE BOOLEAN
__FdoIsActive(
    IN  PXENBUS_FDO Fdo
    )
{
    return Fdo->Active;
}

#define DEFINE_FDO_GET_CONTEXT(_Interface, _Type)               \
static FORCEINLINE _Type                                        \
__FdoGet ## _Interface ## Context(                              \
    IN  PXENBUS_FDO Fdo                                         \
    )                                                           \
{                                                               \
    return Fdo-> ## _Interface ## Context;                      \
}                                                               \
                                                                \
_Type                                                           \
FdoGet ## _Interface ## Context(                                \
    IN  PXENBUS_FDO Fdo                                         \
    )                                                           \
{                                                               \
    return __FdoGet ## _Interface ## Context(Fdo);              \
}

DEFINE_FDO_GET_CONTEXT(Suspend, PXENBUS_SUSPEND_CONTEXT)
DEFINE_FDO_GET_CONTEXT(SharedInfo, PXENBUS_SHARED_INFO_CONTEXT)
DEFINE_FDO_GET_CONTEXT(Evtchn, PXENBUS_EVTCHN_CONTEXT)
DEFINE_FDO_GET_CONTEXT(Debug, PXENBUS_DEBUG_CONTEXT)
DEFINE_FDO_GET_CONTEXT(Store, PXENBUS_STORE_CONTEXT)
DEFINE_FDO_GET_CONTEXT(RangeSet, PXENBUS_RANGE_SET_CONTEXT)
DEFINE_FDO_GET_CONTEXT(Cache, PXENBUS_CACHE_CONTEXT)
DEFINE_FDO_GET_CONTEXT(Gnttab, PXENBUS_GNTTAB_CONTEXT)
DEFINE_FDO_GET_CONTEXT(Balloon, PXENBUS_BALLOON_CONTEXT)

__drv_functionClass(IO_COMPLETION_ROUTINE)
__drv_sameIRQL
static NTSTATUS
FdoDelegateIrpCompletion(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PIRP            Irp,
    IN  PVOID           Context
    )
{
    PKEVENT             Event = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent(Event, IO_NO_INCREMENT, FALSE);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS
FdoDelegateIrp(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PDEVICE_OBJECT      DeviceObject;
    PIO_STACK_LOCATION  StackLocation;
    PIRP                SubIrp;
    KEVENT              Event;
    PIO_STACK_LOCATION  SubStackLocation;
    NTSTATUS            status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    // Find the top of the FDO stack and hold a reference
    DeviceObject = IoGetAttachedDeviceReference(Fdo->Dx->DeviceObject);

    // Get a new IRP for the FDO stack
    SubIrp = IoAllocateIrp(DeviceObject->StackSize, FALSE);

    status = STATUS_NO_MEMORY;
    if (SubIrp == NULL)
        goto done;

    // Copy in the information from the original IRP
    SubStackLocation = IoGetNextIrpStackLocation(SubIrp);

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    RtlCopyMemory(SubStackLocation, StackLocation,
                  FIELD_OFFSET(IO_STACK_LOCATION, CompletionRoutine));
    SubStackLocation->Control = 0;

    IoSetCompletionRoutine(SubIrp,
                           FdoDelegateIrpCompletion,
                           &Event,
                           TRUE,
                           TRUE,
                           TRUE);

    // Default completion status
    SubIrp->IoStatus.Status = Irp->IoStatus.Status;

    status = IoCallDriver(DeviceObject, SubIrp);
    if (status == STATUS_PENDING) {
        (VOID) KeWaitForSingleObject(&Event,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);
        status = SubIrp->IoStatus.Status;
    } else {
        ASSERT3U(status, ==, SubIrp->IoStatus.Status);
    }

    IoFreeIrp(SubIrp);

done:
    ObDereferenceObject(DeviceObject);

    return status;
}

__drv_functionClass(IO_COMPLETION_ROUTINE)
__drv_sameIRQL
static NTSTATUS
FdoForwardIrpSynchronouslyCompletion(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PIRP            Irp,
    IN  PVOID           Context
    )
{
    PKEVENT             Event = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent(Event, IO_NO_INCREMENT, FALSE);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS
FdoForwardIrpSynchronously(
    IN  PXENBUS_FDO Fdo,
    IN  PIRP        Irp
    )
{
    KEVENT          Event;
    NTSTATUS        status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           FdoForwardIrpSynchronouslyCompletion,
                           &Event,
                           TRUE,
                           TRUE,
                           TRUE);

    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
    if (status == STATUS_PENDING) {
        (VOID) KeWaitForSingleObject(&Event,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);
        status = Irp->IoStatus.Status;
    } else {
        ASSERT3U(status, ==, Irp->IoStatus.Status);
    }

    return status;
}

VOID
FdoAddPhysicalDeviceObject(
    IN  PXENBUS_FDO     Fdo,
    IN  PXENBUS_PDO     Pdo
    )
{
    PDEVICE_OBJECT      DeviceObject;
    PXENBUS_DX          Dx;

    DeviceObject = PdoGetDeviceObject(Pdo);
    Dx = (PXENBUS_DX)DeviceObject->DeviceExtension;
    ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

    InsertTailList(&Fdo->Dx->ListEntry, &Dx->ListEntry);
    ASSERT3U(Fdo->References, !=, 0);
    Fdo->References++;

    PdoResume(Pdo);
}

VOID
FdoRemovePhysicalDeviceObject(
    IN  PXENBUS_FDO     Fdo,
    IN  PXENBUS_PDO     Pdo
    )
{
    PDEVICE_OBJECT      DeviceObject;
    PXENBUS_DX          Dx;

    DeviceObject = PdoGetDeviceObject(Pdo);
    Dx = (PXENBUS_DX)DeviceObject->DeviceExtension;
    ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

    PdoSuspend(Pdo);

    RemoveEntryList(&Dx->ListEntry);
    ASSERT3U(Fdo->References, !=, 0);
    --Fdo->References;

    if (Fdo->ScanThread)
        ThreadWake(Fdo->ScanThread);
}

static FORCEINLINE VOID
__FdoAcquireMutex(
    IN  PXENBUS_FDO     Fdo
    )
{
    AcquireMutex(&Fdo->Mutex);
}

VOID
FdoAcquireMutex(
    IN  PXENBUS_FDO     Fdo
    )
{
    __FdoAcquireMutex(Fdo);
}

static FORCEINLINE VOID
__FdoReleaseMutex(
    IN  PXENBUS_FDO     Fdo
    )
{
    ReleaseMutex(&Fdo->Mutex);
}

VOID
FdoReleaseMutex(
    IN  PXENBUS_FDO     Fdo
    )
{
    __FdoReleaseMutex(Fdo);

    if (Fdo->References == 0)
        FdoDestroy(Fdo);
}

static BOOLEAN
FdoEnumerate(
    IN  PXENBUS_FDO     Fdo,
    IN  PANSI_STRING    Classes
    )
{
    BOOLEAN             NeedInvalidate;
    HANDLE              ParametersKey;
    ULONG               Enumerate;
    PLIST_ENTRY         ListEntry;
    ULONG               Index;
    NTSTATUS            status;

    Trace("====>\n");

    NeedInvalidate = FALSE;

    ParametersKey = DriverGetParametersKey();

    status = RegistryQueryDwordValue(ParametersKey,
                                     "Enumerate",
                                     &Enumerate);
    if (!NT_SUCCESS(status))
        Enumerate = 1;

    if (Enumerate == 0)
        goto done;

    __FdoAcquireMutex(Fdo);

    ListEntry = Fdo->Dx->ListEntry.Flink;
    while (ListEntry != &Fdo->Dx->ListEntry) {
        PLIST_ENTRY     Next = ListEntry->Flink;
        PXENBUS_DX      Dx = CONTAINING_RECORD(ListEntry, XENBUS_DX, ListEntry);
        PCHAR           Name = Dx->Name;
        PXENBUS_PDO     Pdo = Dx->Pdo;
        BOOLEAN         Missing;

        Name = PdoGetName(Pdo);
        Missing = TRUE;

        // If the PDO already exists ans its name is in the class list then
        // we don't want to remove it.
        for (Index = 0; Classes[Index].Buffer != NULL; Index++) {
            PANSI_STRING Class = &Classes[Index];

            if (Class->Length == 0)
                continue;

            if (strcmp(Name, Class->Buffer) == 0) {
                Missing = FALSE;
                Class->Length = 0;  // avoid duplication
                break;
            }
        }

        if (Missing &&
            !PdoIsMissing(Pdo) &&
            PdoGetDevicePnpState(Pdo) != Deleted) {
            PdoSetMissing(Pdo, "device disappeared");

            // If the PDO has not yet been enumerated then we can go ahead
            // and mark it as deleted, otherwise we need to notify PnP manager and
            // wait for the REMOVE_DEVICE IRP.
            if (PdoGetDevicePnpState(Pdo) == Present) {
                PdoSetDevicePnpState(Pdo, Deleted);
                PdoDestroy(Pdo);
            } else {
                NeedInvalidate = TRUE;
            }
        }

        ListEntry = Next;
    }

    // Walk the class list and create PDOs for any new classes
    for (Index = 0; Classes[Index].Buffer != NULL; Index++) {
        PANSI_STRING Class = &Classes[Index];

        if (Class->Length != 0) {
            NTSTATUS    status;

            status = PdoCreate(Fdo, Class);
            if (NT_SUCCESS(status))
                NeedInvalidate = TRUE;
        }
    }

    __FdoReleaseMutex(Fdo);

done:
    Trace("<====\n");

    return NeedInvalidate;
}

static PANSI_STRING
FdoMultiSzToUpcaseAnsi(
    IN  PCHAR       Buffer
    )
{
    PANSI_STRING    Ansi;
    LONG            Index;
    LONG            Count;
    NTSTATUS        status;

    Index = 0;
    Count = 0;
    for (;;) {
        if (Buffer[Index] == '\0') {
            Count++;
            Index++;

            // Check for double NUL
            if (Buffer[Index] == '\0')
                break;
        } else {
            Buffer[Index] = (CHAR)toupper(Buffer[Index]);
            Index++;
        }
    }

    Ansi = __FdoAllocate(sizeof (ANSI_STRING) * (Count + 1));

    status = STATUS_NO_MEMORY;
    if (Ansi == NULL)
        goto fail1;

    for (Index = 0; Index < Count; Index++) {
        ULONG   Length;

        Length = (ULONG)strlen(Buffer);
        Ansi[Index].MaximumLength = (USHORT)(Length + 1);
        Ansi[Index].Buffer = __FdoAllocate(Ansi[Index].MaximumLength);

        status = STATUS_NO_MEMORY;
        if (Ansi[Index].Buffer == NULL)
            goto fail2;

        RtlCopyMemory(Ansi[Index].Buffer, Buffer, Length);
        Ansi[Index].Length = (USHORT)Length;

        Buffer += Length + 1;
    }

    return Ansi;

fail2:
    Error("fail2\n");

    while (--Index >= 0)
        __FdoFree(Ansi[Index].Buffer);

    __FdoFree(Ansi);

fail1:
    Error("fail1 (%08x)\n", status);

    return NULL;
}

static VOID
FdoFreeAnsi(
    IN  PANSI_STRING    Ansi
    )
{
    ULONG               Index;

    for (Index = 0; Ansi[Index].Buffer != NULL; Index++)
        __FdoFree(Ansi[Index].Buffer);
        
    __FdoFree(Ansi);
}

static PANSI_STRING
FdoCombineAnsi(
    IN  PANSI_STRING    AnsiA,
    IN  PANSI_STRING    AnsiB
    )
{
    LONG                Count;
    ULONG               Index;
    PANSI_STRING        Ansi;
    NTSTATUS            status;

    Count = 0;

    for (Index = 0;
         AnsiA != NULL && AnsiA[Index].Buffer != NULL; 
         Index++)
        Count++;

    for (Index = 0;
         AnsiB != NULL && AnsiB[Index].Buffer != NULL; 
         Index++)
        Count++;

    Ansi = __FdoAllocate(sizeof (ANSI_STRING) * (Count + 1));

    status = STATUS_NO_MEMORY;
    if (Ansi == NULL)
        goto fail1;

    Count = 0;

    for (Index = 0;
         AnsiA != NULL && AnsiA[Index].Buffer != NULL; 
         Index++) {
        USHORT  Length;

        Length = AnsiA[Index].MaximumLength;

        Ansi[Count].MaximumLength = Length;
        Ansi[Count].Buffer = __FdoAllocate(Length);

        status = STATUS_NO_MEMORY;
        if (Ansi[Count].Buffer == NULL)
            goto fail2;

        RtlCopyMemory(Ansi[Count].Buffer, AnsiA[Index].Buffer, Length);
        Ansi[Count].Length = AnsiA[Index].Length;

        Count++;
    }

    for (Index = 0;
         AnsiB != NULL && AnsiB[Index].Buffer != NULL; 
         Index++) {
        USHORT  Length;

        Length = AnsiB[Index].MaximumLength;

        Ansi[Count].MaximumLength = Length;
        Ansi[Count].Buffer = __FdoAllocate(Length);

        status = STATUS_NO_MEMORY;
        if (Ansi[Count].Buffer == NULL)
            goto fail3;

        RtlCopyMemory(Ansi[Count].Buffer, AnsiB[Index].Buffer, Length);
        Ansi[Count].Length = AnsiB[Index].Length;

        Count++;
    }

    return Ansi;

fail3:
    Error("fail3\n");

fail2:
    Error("fail2\n");

    while (--Count >= 0) 
        __FdoFree(Ansi[Count].Buffer);

    __FdoFree(Ansi);

fail1:
    Error("fail1 (%08x)\n", status);

    return NULL;
}

static NTSTATUS
FdoScan(
    IN  PXENBUS_THREAD  Self,
    IN  PVOID           Context
    )
{
    PXENBUS_FDO         Fdo = Context;
    PKEVENT             Event;
    HANDLE              ParametersKey;
    NTSTATUS            status;

    Info("====>\n");

    Event = ThreadGetEvent(Self);

    ParametersKey = DriverGetParametersKey();

    for (;;) {
        PCHAR                   Buffer;
        PANSI_STRING            StoreClasses;
        PANSI_STRING            SyntheticClasses;
        PANSI_STRING            SupportedClasses;
        PANSI_STRING            Classes;
        ULONG                   Index;
        BOOLEAN                 NeedInvalidate;

        Trace("waiting...\n");

        (VOID) KeWaitForSingleObject(Event,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);
        KeClearEvent(Event);

        Trace("awake\n");

        if (ThreadIsAlerted(Self))
            break;

        // It is not safe to use interfaces before this point
        if (__FdoGetDevicePnpState(Fdo) != Started)
            goto loop;

        status = XENBUS_STORE(Directory,
                              &Fdo->StoreInterface,
                              NULL,
                              NULL,
                              "device",
                              &Buffer);
        if (NT_SUCCESS(status)) {
            StoreClasses = FdoMultiSzToUpcaseAnsi(Buffer);

            XENBUS_STORE(Free,
                         &Fdo->StoreInterface,
                         Buffer);
        } else {
            StoreClasses = NULL;
        }

        status = RegistryQuerySzValue(ParametersKey,
                                      "SyntheticClasses",
                                      &SyntheticClasses);
        if (!NT_SUCCESS(status))
            SyntheticClasses = NULL;

        Classes = FdoCombineAnsi(StoreClasses, SyntheticClasses);

        if (StoreClasses != NULL)
            FdoFreeAnsi(StoreClasses);

        if (SyntheticClasses != NULL)
            RegistryFreeSzValue(SyntheticClasses);

        if (Classes == NULL)
            goto loop;

        if (ParametersKey != NULL) {
            status = RegistryQuerySzValue(ParametersKey,
                                          "SupportedClasses",
                                          &SupportedClasses);
            if (!NT_SUCCESS(status))
                SupportedClasses = NULL;
        } else {
            SupportedClasses = NULL;
        }

        // NULL out anything in the Classes list that not in the
        // SupportedClasses list    
        for (Index = 0; Classes[Index].Buffer != NULL; Index++) {
            PANSI_STRING    Class = &Classes[Index];
            ULONG           Entry;
            BOOLEAN         Supported;

            Supported = FALSE;

            for (Entry = 0;
                 SupportedClasses != NULL && SupportedClasses[Entry].Buffer != NULL;
                 Entry++) {
                if (strncmp(Class->Buffer,
                            SupportedClasses[Entry].Buffer,
                            Class->Length) == 0) {
                    Supported = TRUE;
                    break;
                }
            }

            if (!Supported)
                Class->Length = 0;
        }

        if (SupportedClasses != NULL)
            RegistryFreeSzValue(SupportedClasses);

        NeedInvalidate = FdoEnumerate(Fdo, Classes);

        FdoFreeAnsi(Classes);

        if (NeedInvalidate) {
            NeedInvalidate = FALSE;
            IoInvalidateDeviceRelations(__FdoGetPhysicalDeviceObject(Fdo), 
                                        BusRelations);
        }

loop:
        KeSetEvent(&Fdo->ScanEvent, IO_NO_INCREMENT, FALSE);
    }

    KeSetEvent(&Fdo->ScanEvent, IO_NO_INCREMENT, FALSE);

    Info("<====\n");
    return STATUS_SUCCESS;
}

static FORCEINLINE NTSTATUS
__FdoSuspendSetActive(
    IN  PXENBUS_FDO     Fdo
    )
{
    if (!TryAcquireMutex(&Fdo->BalloonSuspendMutex))
        goto fail1;

    Trace("<===>\n");

    return STATUS_SUCCESS;

fail1:
    return STATUS_UNSUCCESSFUL;
}

static FORCEINLINE VOID
__FdoSuspendClearActive(
    IN  PXENBUS_FDO     Fdo
    )
{
    ReleaseMutex(&Fdo->BalloonSuspendMutex);

    Trace("<===>\n");

    //
    // We may have missed initiating a balloon
    // whilst suspending/resuming.
    //
    if (Fdo->BalloonInterface.Interface.Context != NULL)
        ThreadWake(Fdo->BalloonThread);
}

static NTSTATUS
FdoSuspend(
    IN  PXENBUS_THREAD  Self,
    IN  PVOID           Context
    )
{
    PXENBUS_FDO         Fdo = Context;
    PKEVENT             Event;

    Info("====>\n");

    // We really want to know what CPU this thread will run on
    KeSetSystemAffinityThread((KAFFINITY)1);

    Event = ThreadGetEvent(Self);

    for (;;) {
        PCHAR       Buffer;
        BOOLEAN     Suspend;
        NTSTATUS    status;

        Trace("waiting...\n");

        (VOID) KeWaitForSingleObject(Event,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);
        KeClearEvent(Event);

        Trace("awake\n");

        if (ThreadIsAlerted(Self))
            break;

        // It is not safe to use interfaces before this point
        if (__FdoGetDevicePowerState(Fdo) != PowerDeviceD0)
            goto loop;

        status = XENBUS_STORE(Read,
                              &Fdo->StoreInterface,
                              NULL,
                              "control",
                              "shutdown",
                              &Buffer);
        if (NT_SUCCESS(status)) {
            Suspend = (strcmp(Buffer, "suspend") == 0) ? TRUE : FALSE;
                
            XENBUS_STORE(Free,
                         &Fdo->StoreInterface,
                         Buffer);
        } else {
            Suspend = FALSE;
        }

        if (!Suspend) {
            Trace("nothing to do\n");
            goto loop;
        }

        status = __FdoSuspendSetActive(Fdo);
        if (!NT_SUCCESS(status))
            goto loop;

        (VOID) XENBUS_STORE(Remove,
                            &Fdo->StoreInterface,
                            NULL,
                            "control",
                            "shutdown");

        XENBUS_SUSPEND(Trigger, &Fdo->SuspendInterface);

        __FdoSuspendClearActive(Fdo);

loop:
        KeSetEvent(&Fdo->SuspendEvent, IO_NO_INCREMENT, FALSE);
    }

    KeSetEvent(&Fdo->SuspendEvent, IO_NO_INCREMENT, FALSE);

    Info("<====\n");
    return STATUS_SUCCESS;
}

#define TIME_US(_us)            ((_us) * 10)
#define TIME_MS(_ms)            (TIME_US((_ms) * 1000))
#define TIME_S(_s)              (TIME_MS((_s) * 1000))
#define TIME_RELATIVE(_t)       (-(_t))

static FORCEINLINE NTSTATUS
__FdoBalloonSetActive(
    IN  PXENBUS_FDO         Fdo
    )
{
    if (!TryAcquireMutex(&Fdo->BalloonSuspendMutex))
        goto fail1;

    Trace("<===>\n");

    (VOID) XENBUS_STORE(Printf,
                        &Fdo->StoreInterface,
                        NULL,
                        "control",
                        "balloon-active",
                        "%u",
                        1);

    return STATUS_SUCCESS;

fail1:
    return STATUS_UNSUCCESSFUL;
}

static FORCEINLINE VOID
__FdoBalloonClearActive(
    IN  PXENBUS_FDO     Fdo
    )
{
    (VOID) XENBUS_STORE(Printf,
                        &Fdo->StoreInterface,
                        NULL,
                        "control",
                        "balloon-active",
                        "%u",
                        0);

    ReleaseMutex(&Fdo->BalloonSuspendMutex);

    Trace("<===>\n");

    //
    // We may have missed initiating a suspend
    // whilst the balloon was active.
    //
    ThreadWake(Fdo->SuspendThread);
}

#define XENBUS_BALLOON_RETRY_PERIOD 1

static NTSTATUS
FdoBalloon(
    IN  PXENBUS_THREAD  Self,
    IN  PVOID           Context
    )
{
    PXENBUS_FDO         Fdo = Context;
    PKEVENT             Event;
    LARGE_INTEGER       Timeout;
    ULONGLONG           StaticMax;
    BOOLEAN             Initialized;
    BOOLEAN             Active;
    NTSTATUS            status;

    Info("====>\n");

    Event = ThreadGetEvent(Self);

    Timeout.QuadPart = TIME_RELATIVE(TIME_S(XENBUS_BALLOON_RETRY_PERIOD));

    StaticMax = 0;
    Initialized = FALSE;
    Active = FALSE;

    for (;;) {
        PCHAR                   Buffer;
        ULONGLONG               Target;
        ULONGLONG               Size;

        Trace("waiting%s...\n", (Active) ? " (Active)" : "");

        (VOID) KeWaitForSingleObject(Event,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     (Active) ?
                                     &Timeout :
                                     NULL);
        KeClearEvent(Event);
        
        Trace("awake\n");

        if (ThreadIsAlerted(Self))
            break;

        // It is not safe to use interfaces before this point
        if (__FdoGetDevicePowerState(Fdo) != PowerDeviceD0) {
            if (Active) {
                Active = FALSE;

                __FdoBalloonClearActive(Fdo);
            }

            goto loop;
        }

        if (!Initialized) {
            ULONGLONG   VideoRAM;

            ASSERT(!Active);

            status = XENBUS_STORE(Read,
                                  &Fdo->StoreInterface,
                                  NULL,
                                  "memory",
                                  "static-max",
                                  &Buffer);
            if (!NT_SUCCESS(status))
                goto loop;

            StaticMax = _strtoui64(Buffer, NULL, 10);

            XENBUS_STORE(Free,
                         &Fdo->StoreInterface,
                         Buffer);

            if (StaticMax == 0)
                goto loop;

            status = XENBUS_STORE(Read,
                                  &Fdo->StoreInterface,
                                  NULL,
                                  "memory",
                                  "videoram",
                                  &Buffer);
            if (NT_SUCCESS(status)) {
                VideoRAM = _strtoui64(Buffer, NULL, 10);

                XENBUS_STORE(Free,
                             &Fdo->StoreInterface,
                             Buffer);
            } else {
                VideoRAM = 0;
            }

            if (StaticMax < VideoRAM)
                goto loop;

            StaticMax -= VideoRAM;
            StaticMax /= 4;   // We need the value in pages

            Initialized = TRUE;
        }

        ASSERT(Initialized);

        status = XENBUS_STORE(Read,
                              &Fdo->StoreInterface,
                              NULL,
                              "memory",
                              "target",
                              &Buffer);
        if (!NT_SUCCESS(status))
            goto loop;

        Target = _strtoui64(Buffer, NULL, 10) / 4;

        XENBUS_STORE(Free,
                     &Fdo->StoreInterface,
                     Buffer);

        if (Target > StaticMax)
            Target = StaticMax;

        Size = StaticMax - Target;

        if (XENBUS_BALLOON(GetSize,
                           &Fdo->BalloonInterface) == Size) {
            Trace("nothing to do\n");
            goto loop;
        }

        if (!Active) {
            status = __FdoBalloonSetActive(Fdo);
            if (!NT_SUCCESS(status))
                goto loop;

            Active = TRUE;
        }

        status = XENBUS_BALLOON(Adjust,
                                &Fdo->BalloonInterface,
                                Size);
        if (!NT_SUCCESS(status))
            goto loop;

        ASSERT(Active);
        Active = FALSE;

        __FdoBalloonClearActive(Fdo);

loop:
        if (!Active)
            KeSetEvent(&Fdo->BalloonEvent, IO_NO_INCREMENT, FALSE);
    }

    ASSERT3U(XENBUS_BALLOON(GetSize,
                            &Fdo->BalloonInterface), ==, 0);

    KeSetEvent(&Fdo->BalloonEvent, IO_NO_INCREMENT, FALSE);

    Info("<====\n");
    return STATUS_SUCCESS;
}

static VOID
FdoParseResources(
    IN  PXENBUS_FDO             Fdo,
    IN  PCM_RESOURCE_LIST       RawResourceList,
    IN  PCM_RESOURCE_LIST       TranslatedResourceList
    )
{
    PCM_PARTIAL_RESOURCE_LIST   RawPartialList;
    PCM_PARTIAL_RESOURCE_LIST   TranslatedPartialList;
    ULONG                       Index;

    ASSERT3U(RawResourceList->Count, ==, 1);
    RawPartialList = &RawResourceList->List[0].PartialResourceList;

    ASSERT3U(RawPartialList->Version, ==, 1);
    ASSERT3U(RawPartialList->Revision, ==, 1);

    ASSERT3U(TranslatedResourceList->Count, ==, 1);
    TranslatedPartialList = &TranslatedResourceList->List[0].PartialResourceList;

    ASSERT3U(TranslatedPartialList->Version, ==, 1);
    ASSERT3U(TranslatedPartialList->Revision, ==, 1);

    for (Index = 0; Index < TranslatedPartialList->Count; Index++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR RawPartialDescriptor;
        PCM_PARTIAL_RESOURCE_DESCRIPTOR TranslatedPartialDescriptor;

        RawPartialDescriptor = &RawPartialList->PartialDescriptors[Index];
        TranslatedPartialDescriptor = &TranslatedPartialList->PartialDescriptors[Index];

        Trace("%s: [%d] %02x:%s\n",
              __FdoGetName(Fdo),
              Index,
              TranslatedPartialDescriptor->Type,
              PartialResourceDescriptorTypeName(TranslatedPartialDescriptor->Type));

        switch (TranslatedPartialDescriptor->Type) {
        case CmResourceTypeMemory:
            Trace("RAW: SharedDisposition=%02x Flags=%04x Start = %08x.%08x Length = %08x\n",
                  RawPartialDescriptor->ShareDisposition,
                  RawPartialDescriptor->Flags,
                  RawPartialDescriptor->u.Memory.Start.HighPart,
                  RawPartialDescriptor->u.Memory.Start.LowPart,
                  RawPartialDescriptor->u.Memory.Length);

            Trace("TRANSLATED: SharedDisposition=%02x Flags=%04x Start = %08x.%08x Length = %08x\n",
                  TranslatedPartialDescriptor->ShareDisposition,
                  TranslatedPartialDescriptor->Flags,
                  TranslatedPartialDescriptor->u.Memory.Start.HighPart,
                  TranslatedPartialDescriptor->u.Memory.Start.LowPart,
                  TranslatedPartialDescriptor->u.Memory.Length);

            Fdo->Resource[MEMORY_RESOURCE].Raw = *RawPartialDescriptor;
            Fdo->Resource[MEMORY_RESOURCE].Translated = *TranslatedPartialDescriptor;

            break;

        case CmResourceTypeInterrupt:
            Trace("RAW: SharedDisposition=%02x Flags=%04x Level = %08x Vector = %08x Affinity = %p\n",
                  RawPartialDescriptor->ShareDisposition,
                  RawPartialDescriptor->Flags,
                  RawPartialDescriptor->u.Interrupt.Level,
                  RawPartialDescriptor->u.Interrupt.Vector,
                  (PVOID)RawPartialDescriptor->u.Interrupt.Affinity);

            Trace("TRANSLATED: SharedDisposition=%02x Flags=%04x Level = %08x Vector = %08x Affinity = %p\n",
                  TranslatedPartialDescriptor->ShareDisposition,
                  TranslatedPartialDescriptor->Flags,
                  TranslatedPartialDescriptor->u.Interrupt.Level,
                  TranslatedPartialDescriptor->u.Interrupt.Vector,
                  (PVOID)TranslatedPartialDescriptor->u.Interrupt.Affinity);

            Fdo->Resource[INTERRUPT_RESOURCE].Raw = *RawPartialDescriptor;
            Fdo->Resource[INTERRUPT_RESOURCE].Translated = *TranslatedPartialDescriptor;

            break;

        default:
            break;
        }
    }

    Trace("<====\n");
}

static FORCEINLINE PXENBUS_RESOURCE
__FdoGetResource(
    IN  PXENBUS_FDO             Fdo,
    IN  XENBUS_RESOURCE_TYPE    Type
    )
{
    ASSERT3U(Type, <, RESOURCE_COUNT);

    return &Fdo->Resource[Type];
}

static
_Function_class_(KSERVICE_ROUTINE)
_IRQL_requires_(HIGH_LEVEL)
_IRQL_requires_same_
BOOLEAN
FdoInterrupt(
    IN  PKINTERRUPT             InterruptObject,
    IN  PVOID                   Context
    )
{
    PXENBUS_FDO                 Fdo = Context;

    UNREFERENCED_PARAMETER(InterruptObject);

    ASSERT(Fdo != NULL);

    return EvtchnInterrupt(__FdoGetEvtchnContext(Fdo));
}

static NTSTATUS
FdoConnectInterrupt(
    IN  PXENBUS_FDO                 Fdo
    )
{
    PXENBUS_RESOURCE                Interrupt;
    IO_CONNECT_INTERRUPT_PARAMETERS Connect;
    NTSTATUS                        status;

    Trace("====>\n");

    Interrupt = __FdoGetResource(Fdo, INTERRUPT_RESOURCE);

    RtlZeroMemory(&Connect, sizeof (IO_CONNECT_INTERRUPT_PARAMETERS));
    Connect.Version = CONNECT_FULLY_SPECIFIED;
    Connect.FullySpecified.PhysicalDeviceObject = __FdoGetPhysicalDeviceObject(Fdo);
    Connect.FullySpecified.SynchronizeIrql = (KIRQL)Interrupt->Translated.u.Interrupt.Level;
    Connect.FullySpecified.ShareVector = (BOOLEAN)(Interrupt->Translated.ShareDisposition == CmResourceShareShared);
    Connect.FullySpecified.Vector = Interrupt->Translated.u.Interrupt.Vector;
    Connect.FullySpecified.Irql = (KIRQL)Interrupt->Translated.u.Interrupt.Level;
    Connect.FullySpecified.InterruptMode = (Interrupt->Translated.Flags & CM_RESOURCE_INTERRUPT_LATCHED) ?
                                           Latched :
                                           LevelSensitive;
    Connect.FullySpecified.ProcessorEnableMask = Interrupt->Translated.u.Interrupt.Affinity;
    Connect.FullySpecified.InterruptObject = &Fdo->InterruptObject;
    Connect.FullySpecified.ServiceRoutine = FdoInterrupt;
    Connect.FullySpecified.ServiceContext = Fdo;

    status = IoConnectInterruptEx(&Connect);
    if (!NT_SUCCESS(status))
        goto fail1;

    Trace("<====\n");

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

_IRQL_requires_max_(HIGH_LEVEL)
_IRQL_saves_
_IRQL_raises_(HIGH_LEVEL)
KIRQL
FdoAcquireInterruptLock(
    IN  PXENBUS_FDO Fdo
    )
{
    PKINTERRUPT     InterruptObject = Fdo->InterruptObject;

    return KeAcquireInterruptSpinLock(InterruptObject);
}
                      
_IRQL_requires_(HIGH_LEVEL)
VOID
FdoReleaseInterruptLock(
    IN  PXENBUS_FDO                 Fdo,
    IN  __drv_restoresIRQL KIRQL    Irql
    )
{
    PKINTERRUPT                     InterruptObject = Fdo->InterruptObject;

    KeReleaseInterruptSpinLock(InterruptObject, Irql);
}

ULONG
FdoGetInterruptVector(
    IN  PXENBUS_FDO     Fdo
    )
{
    PXENBUS_RESOURCE    Interrupt;

    Interrupt = __FdoGetResource(Fdo, INTERRUPT_RESOURCE);

    return Interrupt->Raw.u.Interrupt.Vector;
}

static VOID
FdoDisconnectInterrupt(
    IN  PXENBUS_FDO                     Fdo
    )
{
    PKINTERRUPT                         InterruptObject;
    IO_DISCONNECT_INTERRUPT_PARAMETERS  Disconnect;

    Trace("====>\n");

    InterruptObject = Fdo->InterruptObject;
    Fdo->InterruptObject = NULL;

    RtlZeroMemory(&Disconnect, sizeof (IO_DISCONNECT_INTERRUPT_PARAMETERS));
    Disconnect.Version = CONNECT_FULLY_SPECIFIED;
    Disconnect.ConnectionContext.InterruptObject = InterruptObject;

    IoDisconnectInterruptEx(&Disconnect);

    Trace("<====\n");
}

static
_Function_class_(KSERVICE_ROUTINE)
_IRQL_requires_(HIGH_LEVEL)
_IRQL_requires_same_
BOOLEAN
FdoEvtchnCallback(
    IN  PKINTERRUPT         InterruptObject,
    IN  PVOID               Argument
    )
{
    PXENBUS_FDO             Fdo = Argument;

    UNREFERENCED_PARAMETER(InterruptObject);

    ASSERT(Fdo != NULL);

    XENBUS_DEBUG(Trigger, &Fdo->DebugInterface, NULL);

    return TRUE;
}

#define BALLOON_PAUSE   60

static FORCEINLINE NTSTATUS
__FdoD3ToD0(
    IN  PXENBUS_FDO Fdo
    )
{
    BOOLEAN         Pending;
    NTSTATUS        status;

    Trace("====>\n");

    ASSERT3U(KeGetCurrentIrql(), ==, DISPATCH_LEVEL);

    Fdo->Channel = XENBUS_EVTCHN(Open,
                                 &Fdo->EvtchnInterface,
                                 XENBUS_EVTCHN_TYPE_VIRQ,
                                 FdoEvtchnCallback,
                                 Fdo,
                                 VIRQ_DEBUG);

    status = STATUS_UNSUCCESSFUL;
    if (Fdo->Channel == NULL)
        goto fail1;

    Pending = XENBUS_EVTCHN(Unmask,
                            &Fdo->EvtchnInterface,
                            Fdo->Channel,
                            FALSE);
    if (Pending)
        XENBUS_EVTCHN(Trigger,
                      &Fdo->EvtchnInterface,
                      Fdo->Channel);

    status = XENBUS_STORE(WatchAdd,
                          &Fdo->StoreInterface,
                          NULL,
                          "device",
                          ThreadGetEvent(Fdo->ScanThread),
                          &Fdo->ScanWatch);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = XENBUS_STORE(WatchAdd,
                          &Fdo->StoreInterface,
                          "control",
                          "shutdown",
                          ThreadGetEvent(Fdo->SuspendThread),
                          &Fdo->SuspendWatch);
    if (!NT_SUCCESS(status))
        goto fail3;

    (VOID) XENBUS_STORE(Printf,
                        &Fdo->StoreInterface,
                        NULL,
                        "control",
                        "feature-suspend",
                        "%u",
                        1);

    if (Fdo->BalloonInterface.Interface.Context != NULL) {
        status = XENBUS_STORE(WatchAdd,
                              &Fdo->StoreInterface,
                              "memory",
                              "target",
                              ThreadGetEvent(Fdo->BalloonThread),
                              &Fdo->BalloonWatch);
        if (!NT_SUCCESS(status))
            goto fail4;

        (VOID) XENBUS_STORE(Printf,
                            &Fdo->StoreInterface,
                            NULL,
                            "control",
                            "feature-balloon",
                            "%u",
                            1);
    }

    Trace("<====\n");

    return STATUS_SUCCESS;

fail4:
    Error("fail4\n");

    (VOID) XENBUS_STORE(Remove,
                        &Fdo->StoreInterface,
                        NULL,
                        "control",
                        "feature-suspend");

    (VOID) XENBUS_STORE(WatchRemove,
                        &Fdo->StoreInterface,
                        Fdo->SuspendWatch);
    Fdo->SuspendWatch = NULL;

fail3:
    Error("fail3\n");

    (VOID) XENBUS_STORE(WatchRemove,
                        &Fdo->StoreInterface,
                        Fdo->ScanWatch);
    Fdo->ScanWatch = NULL;

fail2:
    Error("fail2\n");

    XENBUS_EVTCHN(Close,
                  &Fdo->EvtchnInterface,
                  Fdo->Channel);
    Fdo->Channel = NULL;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static FORCEINLINE VOID
__FdoD0ToD3(
    IN  PXENBUS_FDO Fdo
    )
{
    Trace("====>\n");

    ASSERT3U(KeGetCurrentIrql(), ==, DISPATCH_LEVEL);

    if (Fdo->BalloonInterface.Interface.Context != NULL) {
        (VOID) XENBUS_STORE(Remove,
                            &Fdo->StoreInterface,
                            NULL,
                            "control",
                            "feature-balloon");

        (VOID) XENBUS_STORE(WatchRemove,
                            &Fdo->StoreInterface,
                            Fdo->BalloonWatch);
        Fdo->BalloonWatch = NULL;
    }

    (VOID) XENBUS_STORE(Remove,
                        &Fdo->StoreInterface,
                        NULL,
                        "control",
                        "feature-suspend");

    (VOID) XENBUS_STORE(WatchRemove,
                        &Fdo->StoreInterface,
                        Fdo->SuspendWatch);
    Fdo->SuspendWatch = NULL;

    (VOID) XENBUS_STORE(WatchRemove,
                        &Fdo->StoreInterface,
                        Fdo->ScanWatch);
    Fdo->ScanWatch = NULL;

    XENBUS_EVTCHN(Close,
                  &Fdo->EvtchnInterface,
                  Fdo->Channel);
    Fdo->Channel = NULL;

    Trace("<====\n");
}

static VOID
FdoSuspendCallbackLate(
    IN  PVOID   Argument
    )
{
    PXENBUS_FDO Fdo = Argument;
    NTSTATUS    status;

    __FdoD0ToD3(Fdo);

    status = __FdoD3ToD0(Fdo);
    ASSERT(NT_SUCCESS(status));
}

static NTSTATUS
FdoCreateIoSpace(
    IN  PXENBUS_FDO     Fdo
    )
{
    PXENBUS_RESOURCE    Memory;
    NTSTATUS            status;

    Memory = __FdoGetResource(Fdo, MEMORY_RESOURCE);

    status = XENBUS_RANGE_SET(Create,
                              &Fdo->RangeSetInterface,
                              "io_space",
                              &Fdo->RangeSet);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = XENBUS_RANGE_SET(Put,
                              &Fdo->RangeSetInterface,
                              Fdo->RangeSet,
                              Memory->Translated.u.Memory.Start.QuadPart,
                              Memory->Translated.u.Memory.Length);
    if (!NT_SUCCESS(status))
        goto fail2;

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

    XENBUS_RANGE_SET(Destroy,
                     &Fdo->RangeSetInterface,
                     Fdo->RangeSet);
    Fdo->RangeSet = NULL;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

NTSTATUS
FdoAllocateIoSpace(
    IN  PXENBUS_FDO         Fdo,
    IN  ULONG               Size,
    OUT PPHYSICAL_ADDRESS   Address
    )
{
    NTSTATUS                status;

    ASSERT3U(Size & (PAGE_SIZE - 1), ==, 0);

    status = XENBUS_RANGE_SET(Pop,
                              &Fdo->RangeSetInterface,
                              Fdo->RangeSet,
                              Size,
                              &Address->QuadPart);
    if (!NT_SUCCESS(status))
        goto fail1;

    ASSERT3U(Address->QuadPart & (PAGE_SIZE - 1), ==, 0);

    return STATUS_SUCCESS;;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
FdoFreeIoSpace(
    IN  PXENBUS_FDO         Fdo,
    IN  PHYSICAL_ADDRESS    Address,
    IN  ULONG               Size
    )
{
    NTSTATUS                status;

    ASSERT3U(Address.QuadPart & (PAGE_SIZE - 1), ==, 0);
    ASSERT3U(Size & (PAGE_SIZE - 1), ==, 0);

    status = XENBUS_RANGE_SET(Put,
                              &Fdo->RangeSetInterface,
                              Fdo->RangeSet,
                              Address.QuadPart,
                              Size);
    ASSERT(NT_SUCCESS(status));
}

static VOID
FdoDestroyIoSpace(
    IN  PXENBUS_FDO     Fdo
    )
{
    PXENBUS_RESOURCE    Memory;
    NTSTATUS            status;

    Memory = __FdoGetResource(Fdo, MEMORY_RESOURCE);

    status = XENBUS_RANGE_SET(Get,
                              &Fdo->RangeSetInterface,
                              Fdo->RangeSet,
                              Memory->Translated.u.Memory.Start.QuadPart,
                              Memory->Translated.u.Memory.Length);
    ASSERT(NT_SUCCESS(status));

    XENBUS_RANGE_SET(Destroy,
                     &Fdo->RangeSetInterface,
                     Fdo->RangeSet);
    Fdo->RangeSet = NULL;
}

// This function must not touch pageable code or data
static NTSTATUS
FdoD3ToD0(
    IN  PXENBUS_FDO             Fdo
    )
{
    POWER_STATE                 PowerState;
    KIRQL                       Irql;
    PLIST_ENTRY                 ListEntry;
    NTSTATUS                    status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);
    ASSERT3U(__FdoGetDevicePowerState(Fdo), ==, PowerDeviceD3);

    Trace("====>\n");

    if (!__FdoIsActive(Fdo))
        goto not_active;

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);

    status = XENBUS_DEBUG(Acquire, &Fdo->DebugInterface);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = XENBUS_SUSPEND(Acquire, &Fdo->SuspendInterface);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = XENBUS_RANGE_SET(Acquire, &Fdo->RangeSetInterface);
    if (!NT_SUCCESS(status))
        goto fail3;

    // Subsequent interfaces require use of BAR space
    status = FdoCreateIoSpace(Fdo);
    if (!NT_SUCCESS(status))
        goto fail4;

    status = XENBUS_EVTCHN(Acquire, &Fdo->EvtchnInterface);
    if (!NT_SUCCESS(status))
        goto fail5;

    status = XENBUS_STORE(Acquire, &Fdo->StoreInterface);
    if (!NT_SUCCESS(status))
        goto fail6;

    if (Fdo->BalloonInterface.Interface.Context != NULL) {
        status = XENBUS_BALLOON(Acquire, &Fdo->BalloonInterface);
        if (!NT_SUCCESS(status))
            goto fail7;
    }

    if (Fdo->UnplugInterface.Interface.Context != NULL) {
        status = XENFILT_UNPLUG(Acquire, &Fdo->UnplugInterface);
        if (!NT_SUCCESS(status))
            goto fail8;
    }

    status = __FdoD3ToD0(Fdo);
    if (!NT_SUCCESS(status))
        goto fail9;

    status = XENBUS_SUSPEND(Register,
                            &Fdo->SuspendInterface,
                            SUSPEND_CALLBACK_LATE,
                            FdoSuspendCallbackLate,
                            Fdo,
                            &Fdo->SuspendCallbackLate);
    if (!NT_SUCCESS(status))
        goto fail10;

    KeLowerIrql(Irql);

not_active:
    __FdoSetDevicePowerState(Fdo, PowerDeviceD0);

    PowerState.DeviceState = PowerDeviceD0;
    PoSetPowerState(Fdo->Dx->DeviceObject,
                    DevicePowerState,
                    PowerState);

    __FdoAcquireMutex(Fdo);

    for (ListEntry = Fdo->Dx->ListEntry.Flink;
         ListEntry != &Fdo->Dx->ListEntry;
         ListEntry = ListEntry->Flink) {
        PXENBUS_DX  Dx = CONTAINING_RECORD(ListEntry, XENBUS_DX, ListEntry);
        PXENBUS_PDO Pdo = Dx->Pdo;

        ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

        PdoResume(Pdo);
    }

    __FdoReleaseMutex(Fdo);

    Trace("<====\n");

    return STATUS_SUCCESS;

fail10:
    Error("fail10\n");

    __FdoD0ToD3(Fdo);

fail9:
    Error("fail9\n");

    if (Fdo->UnplugInterface.Interface.Context != NULL)
        XENFILT_UNPLUG(Release, &Fdo->UnplugInterface);

fail8:
    Error("fail8\n");

    if (Fdo->BalloonInterface.Interface.Context != NULL)
        XENBUS_BALLOON(Release, &Fdo->BalloonInterface);

fail7:
    Error("fail7\n");

    XENBUS_STORE(Release, &Fdo->StoreInterface);

fail6:
    Error("fail6\n");

    XENBUS_EVTCHN(Release, &Fdo->EvtchnInterface);

fail5:
    Error("fail5\n");

    FdoDestroyIoSpace(Fdo);

fail4:
    Error("fail4\n");

    XENBUS_RANGE_SET(Release, &Fdo->RangeSetInterface);

fail3:
    Error("fail3\n");

    XENBUS_SUSPEND(Release, &Fdo->SuspendInterface);

fail2:
    Error("fail2\n");

    XENBUS_DEBUG(Release, &Fdo->DebugInterface);

fail1:
    Error("fail1 (%08x)\n", status);

    KeLowerIrql(Irql);

    return status;
}

// This function must not touch pageable code or data
static VOID
FdoD0ToD3(
    IN  PXENBUS_FDO Fdo
    )
{
    POWER_STATE     PowerState;
    PLIST_ENTRY     ListEntry;
    KIRQL           Irql;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);
    ASSERT3U(__FdoGetDevicePowerState(Fdo), ==, PowerDeviceD0);

    Trace("====>\n");

    __FdoAcquireMutex(Fdo);

    for (ListEntry = Fdo->Dx->ListEntry.Flink;
         ListEntry != &Fdo->Dx->ListEntry;
         ListEntry = ListEntry->Flink) {
        PXENBUS_DX  Dx = CONTAINING_RECORD(ListEntry, XENBUS_DX, ListEntry);
        PXENBUS_PDO Pdo = Dx->Pdo;

        ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

        if (PdoGetDevicePnpState(Pdo) == Deleted ||
            PdoIsMissing(Pdo))
            continue;

        PdoSuspend(Pdo);
    }

    __FdoReleaseMutex(Fdo);

    PowerState.DeviceState = PowerDeviceD3;
    PoSetPowerState(Fdo->Dx->DeviceObject,
                    DevicePowerState,
                    PowerState);

    __FdoSetDevicePowerState(Fdo, PowerDeviceD3);

    if (!__FdoIsActive(Fdo))
        goto not_active;

    if (Fdo->BalloonInterface.Interface.Context != NULL) {
        Trace("waiting for balloon thread...\n");

        KeClearEvent(&Fdo->BalloonEvent);
        ThreadWake(Fdo->BalloonThread);

        (VOID) KeWaitForSingleObject(&Fdo->BalloonEvent,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);

        Trace("done\n");
    }

    Trace("waiting for suspend thread...\n");

    KeClearEvent(&Fdo->SuspendEvent);
    ThreadWake(Fdo->SuspendThread);

    (VOID) KeWaitForSingleObject(&Fdo->SuspendEvent,
                                 Executive,
                                 KernelMode,
                                 FALSE,
                                 NULL);

    Trace("done\n");

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);

    XENBUS_SUSPEND(Deregister,
                   &Fdo->SuspendInterface,
                   Fdo->SuspendCallbackLate);
    Fdo->SuspendCallbackLate = NULL;

    __FdoD0ToD3(Fdo);

    if (Fdo->UnplugInterface.Interface.Context != NULL)
        XENFILT_UNPLUG(Release, &Fdo->UnplugInterface);

    if (Fdo->BalloonInterface.Interface.Context != NULL)
        XENBUS_BALLOON(Release, &Fdo->BalloonInterface);

    XENBUS_STORE(Release, &Fdo->StoreInterface);

    XENBUS_EVTCHN(Release, &Fdo->EvtchnInterface);

    FdoDestroyIoSpace(Fdo);

    XENBUS_RANGE_SET(Release, &Fdo->RangeSetInterface);

    XENBUS_SUSPEND(Release, &Fdo->SuspendInterface);

    XENBUS_DEBUG(Release, &Fdo->DebugInterface);

    KeLowerIrql(Irql);

not_active:
    Trace("<====\n");
}

// This function must not touch pageable code or data
static VOID
FdoS4ToS3(
    IN  PXENBUS_FDO Fdo
    )
{
    KIRQL           Irql;

    Trace("====>\n");

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);
    ASSERT3U(__FdoGetSystemPowerState(Fdo), ==, PowerSystemHibernate);

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);

    HypercallPopulate();

    if (Fdo->UnplugInterface.Interface.Context != NULL)
        XENFILT_UNPLUG(Replay, &Fdo->UnplugInterface);

    KeLowerIrql(Irql);

    __FdoSetSystemPowerState(Fdo, PowerSystemSleeping3);

    Trace("<====\n");
}

// This function must not touch pageable code or data
static VOID
FdoS3ToS4(
    IN  PXENBUS_FDO Fdo
    )
{
    Trace("====>\n");

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);
    ASSERT3U(__FdoGetSystemPowerState(Fdo), ==, PowerSystemSleeping3);

    __FdoSetSystemPowerState(Fdo, PowerSystemHibernate);

    Trace("<====\n");
}

static NTSTATUS
FdoStartDevice(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    NTSTATUS            status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    FdoParseResources(Fdo,
                      StackLocation->Parameters.StartDevice.AllocatedResources,
                      StackLocation->Parameters.StartDevice.AllocatedResourcesTranslated);

    if (!__FdoIsActive(Fdo))
        goto not_active;

    status = FdoConnectInterrupt(Fdo);
    if (!NT_SUCCESS(status))
        goto fail2;

    KeInitializeEvent(&Fdo->ScanEvent, NotificationEvent, FALSE);

    status = ThreadCreate(FdoScan, Fdo, &Fdo->ScanThread);
    if (!NT_SUCCESS(status))
        goto fail3;

    InitializeMutex(&Fdo->BalloonSuspendMutex);

    KeInitializeEvent(&Fdo->SuspendEvent, NotificationEvent, FALSE);

    status = ThreadCreate(FdoSuspend, Fdo, &Fdo->SuspendThread);
    if (!NT_SUCCESS(status))
        goto fail4;

    if (Fdo->BalloonInterface.Interface.Context != NULL) {
        KeInitializeEvent(&Fdo->BalloonEvent, NotificationEvent, FALSE);

        status = ThreadCreate(FdoBalloon, Fdo, &Fdo->BalloonThread);
        if (!NT_SUCCESS(status))
            goto fail5;
    }

not_active:
    status = FdoD3ToD0(Fdo);
    if (!NT_SUCCESS(status))
        goto fail6;

    if (Fdo->BalloonInterface.Interface.Context != NULL) {
        BOOLEAN Warned;

        ASSERT(__FdoIsActive(Fdo));

        Warned = FALSE;

        for (;;) {
            LARGE_INTEGER   Timeout;

            Timeout.QuadPart = TIME_RELATIVE(TIME_S(BALLOON_PAUSE));

            status = KeWaitForSingleObject(&Fdo->BalloonEvent,
                                           Executive,
                                           KernelMode,
                                           FALSE,
                                           &Timeout);
            if (status != STATUS_TIMEOUT)
                break;

            if (!Warned) {
                Warning("waiting for balloon\n");
                Warned = TRUE;
            }
        }
    }

    __FdoSetDevicePnpState(Fdo, Started);

    if (__FdoIsActive(Fdo))
        ThreadWake(Fdo->ScanThread);

    status = Irp->IoStatus.Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;

fail6:
    Error("fail6\n");

    if (!__FdoIsActive(Fdo))
        goto fail2;
    
    if (Fdo->BalloonInterface.Interface.Context != NULL) {
        ThreadAlert(Fdo->BalloonThread);
        ThreadJoin(Fdo->BalloonThread);
        Fdo->BalloonThread = NULL;
    }

fail5:
    Error("fail5\n");

    if (Fdo->BalloonInterface.Interface.Context != NULL)
        RtlZeroMemory(&Fdo->BalloonEvent, sizeof (KEVENT));

    ThreadAlert(Fdo->SuspendThread);
    ThreadJoin(Fdo->SuspendThread);
    Fdo->SuspendThread = NULL;

fail4:
    Error("fail4\n");

    RtlZeroMemory(&Fdo->SuspendEvent, sizeof (KEVENT));

    RtlZeroMemory(&Fdo->BalloonSuspendMutex, sizeof (MUTEX));

    ThreadAlert(Fdo->ScanThread);
    ThreadJoin(Fdo->ScanThread);
    Fdo->ScanThread = NULL;

fail3:
    Error("fail3\n");

    RtlZeroMemory(&Fdo->ScanEvent, sizeof (KEVENT));

    FdoDisconnectInterrupt(Fdo);

fail2:
    Error("fail2\n");

    RtlZeroMemory(&Fdo->Resource, sizeof (XENBUS_RESOURCE) * RESOURCE_COUNT);

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
FdoQueryStopDevice(
    IN  PXENBUS_FDO Fdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    status = STATUS_UNSUCCESSFUL;
    if (Fdo->BalloonInterface.Interface.Context != NULL &&
        XENBUS_BALLOON(GetSize,
                       &Fdo->BalloonInterface) != 0)
        goto fail1;

    __FdoSetDevicePnpState(Fdo, StopPending);
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
FdoCancelStopDevice(
    IN  PXENBUS_FDO Fdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    Irp->IoStatus.Status = STATUS_SUCCESS;

    __FdoRestoreDevicePnpState(Fdo, StopPending);

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;
}

static NTSTATUS
FdoStopDevice(
    IN  PXENBUS_FDO Fdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    if (__FdoGetDevicePowerState(Fdo) == PowerDeviceD0)
        FdoD0ToD3(Fdo);

    if (!__FdoIsActive(Fdo))
        goto not_active;

    if (Fdo->BalloonInterface.Interface.Context != NULL) {
        ThreadAlert(Fdo->BalloonThread);
        ThreadJoin(Fdo->BalloonThread);
        Fdo->BalloonThread = NULL;

        RtlZeroMemory(&Fdo->BalloonEvent, sizeof (KEVENT));
    }

    ThreadAlert(Fdo->SuspendThread);
    ThreadJoin(Fdo->SuspendThread);
    Fdo->SuspendThread = NULL;

    RtlZeroMemory(&Fdo->SuspendEvent, sizeof (KEVENT));

    RtlZeroMemory(&Fdo->BalloonSuspendMutex, sizeof (MUTEX));

    ThreadAlert(Fdo->ScanThread);
    ThreadJoin(Fdo->ScanThread);
    Fdo->ScanThread = NULL;

    RtlZeroMemory(&Fdo->ScanEvent, sizeof (KEVENT));

    FdoDisconnectInterrupt(Fdo);

not_active:
    RtlZeroMemory(&Fdo->Resource, sizeof (XENBUS_RESOURCE) * RESOURCE_COUNT);

    __FdoSetDevicePnpState(Fdo, Stopped);
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;
}

static NTSTATUS
FdoQueryRemoveDevice(
    IN  PXENBUS_FDO Fdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    status = STATUS_UNSUCCESSFUL;
    if (Fdo->BalloonInterface.Interface.Context != NULL &&
        XENBUS_BALLOON(GetSize,
                       &Fdo->BalloonInterface) != 0)
        goto fail1;

    __FdoSetDevicePnpState(Fdo, RemovePending);
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
FdoCancelRemoveDevice(
    IN  PXENBUS_FDO Fdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    __FdoRestoreDevicePnpState(Fdo, RemovePending);

    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;
}

static NTSTATUS
FdoSurpriseRemoval(
    IN  PXENBUS_FDO Fdo,
    IN  PIRP        Irp
    )
{
    PLIST_ENTRY     ListEntry;
    NTSTATUS        status;

    __FdoSetDevicePnpState(Fdo, SurpriseRemovePending);

    __FdoAcquireMutex(Fdo);

    for (ListEntry = Fdo->Dx->ListEntry.Flink;
         ListEntry != &Fdo->Dx->ListEntry;
         ListEntry = ListEntry->Flink) {
        PXENBUS_DX  Dx = CONTAINING_RECORD(ListEntry, XENBUS_DX, ListEntry);
        PXENBUS_PDO Pdo = Dx->Pdo;

        ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

        if (!PdoIsMissing(Pdo))
            PdoSetMissing(Pdo, "FDO surprise removed");
    }

    __FdoReleaseMutex(Fdo);

    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;
}

static NTSTATUS
FdoRemoveDevice(
    IN  PXENBUS_FDO                     Fdo,
    IN  PIRP                            Irp
    )
{
    PLIST_ENTRY                         ListEntry;
    NTSTATUS                            status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    if (__FdoIsActive(Fdo)) {
        Trace("waiting for scan thread...\n");

        KeClearEvent(&Fdo->ScanEvent);
        ThreadWake(Fdo->ScanThread);

        (VOID) KeWaitForSingleObject(&Fdo->ScanEvent,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);

        Trace("done\n");
    }

    __FdoAcquireMutex(Fdo);

    ListEntry = Fdo->Dx->ListEntry.Flink;
    while (ListEntry != &Fdo->Dx->ListEntry) {
        PLIST_ENTRY Flink = ListEntry->Flink;
        PXENBUS_DX  Dx = CONTAINING_RECORD(ListEntry, XENBUS_DX, ListEntry);
        PXENBUS_PDO Pdo = Dx->Pdo;

        ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

        if (!PdoIsMissing(Pdo))
            PdoSetMissing(Pdo, "FDO removed");

        if (PdoGetDevicePnpState(Pdo) != SurpriseRemovePending)
            PdoSetDevicePnpState(Pdo, Deleted);

        if (PdoGetDevicePnpState(Pdo) == Deleted)
            PdoDestroy(Pdo);

        ListEntry = Flink;
    }

    __FdoReleaseMutex(Fdo);

    if (__FdoGetDevicePowerState(Fdo) == PowerDeviceD0)
        FdoD0ToD3(Fdo);

    if (!__FdoIsActive(Fdo))
        goto not_active;

    if (Fdo->BalloonInterface.Interface.Context != NULL) {
        ThreadAlert(Fdo->BalloonThread);
        ThreadJoin(Fdo->BalloonThread);
        Fdo->BalloonThread = NULL;

        RtlZeroMemory(&Fdo->BalloonEvent, sizeof (KEVENT));
    }

    ThreadAlert(Fdo->SuspendThread);
    ThreadJoin(Fdo->SuspendThread);
    Fdo->SuspendThread = NULL;

    RtlZeroMemory(&Fdo->SuspendEvent, sizeof (KEVENT));

    RtlZeroMemory(&Fdo->BalloonSuspendMutex, sizeof (MUTEX));

    ThreadAlert(Fdo->ScanThread);
    ThreadJoin(Fdo->ScanThread);
    Fdo->ScanThread = NULL;

    RtlZeroMemory(&Fdo->ScanEvent, sizeof (KEVENT));

    FdoDisconnectInterrupt(Fdo);

not_active:
    RtlZeroMemory(&Fdo->Resource, sizeof (XENBUS_RESOURCE) * RESOURCE_COUNT);

    __FdoSetDevicePnpState(Fdo, Deleted);

    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    __FdoAcquireMutex(Fdo);
    ASSERT3U(Fdo->References, !=, 0);
    --Fdo->References;
    __FdoReleaseMutex(Fdo);

    if (Fdo->References == 0)
        FdoDestroy(Fdo);

    return status;
}

#define SCAN_PAUSE  10

static NTSTATUS
FdoQueryDeviceRelations(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    ULONG               Size;
    PDEVICE_RELATIONS   Relations;
    ULONG               Count;
    PLIST_ENTRY         ListEntry;
    BOOLEAN             Warned;
    NTSTATUS            status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    status = Irp->IoStatus.Status;

    if (StackLocation->Parameters.QueryDeviceRelations.Type != BusRelations) {
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

        goto done;
    }

    Warned = FALSE;

    for (;;) {
        LARGE_INTEGER   Timeout;

        if (!__FdoIsActive(Fdo))
            break;

        Timeout.QuadPart = TIME_RELATIVE(TIME_S(SCAN_PAUSE));

        status = KeWaitForSingleObject(&Fdo->ScanEvent,
                                       Executive,
                                       KernelMode,
                                       FALSE,
                                       &Timeout);
        if (status != STATUS_TIMEOUT)
            break;

        if (!Warned) {
            Warning("waiting for device enumeration\n");
            Warned = TRUE;
        }
    }

    __FdoAcquireMutex(Fdo);

    Count = 0;
    for (ListEntry = Fdo->Dx->ListEntry.Flink;
         ListEntry != &Fdo->Dx->ListEntry;
         ListEntry = ListEntry->Flink)
        Count++;

    Size = FIELD_OFFSET(DEVICE_RELATIONS, Objects) + (sizeof (DEVICE_OBJECT) * __min(Count, 1));

    Relations = __AllocatePoolWithTag(PagedPool, Size, 'SUB');

    status = STATUS_NO_MEMORY;
    if (Relations == NULL)
        goto fail1;

    for (ListEntry = Fdo->Dx->ListEntry.Flink;
         ListEntry != &Fdo->Dx->ListEntry;
         ListEntry = ListEntry->Flink) {
        PXENBUS_DX  Dx = CONTAINING_RECORD(ListEntry, XENBUS_DX, ListEntry);
        PXENBUS_PDO Pdo = Dx->Pdo;

        ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

        if (PdoGetDevicePnpState(Pdo) == Deleted &&
            !PdoIsMissing(Pdo))
            PdoSetMissing(Pdo, "surprise remove");

        if (PdoIsMissing(Pdo))
            continue;

        if (PdoGetDevicePnpState(Pdo) == Present)
            PdoSetDevicePnpState(Pdo, Enumerated);

        ObReferenceObject(Dx->DeviceObject);
        Relations->Objects[Relations->Count++] = Dx->DeviceObject;
    }

    ASSERT3U(Relations->Count, <=, Count);

    Trace("%d PDO(s)\n", Relations->Count);

    __FdoReleaseMutex(Fdo);

    Irp->IoStatus.Information = (ULONG_PTR)Relations;
    Irp->IoStatus.Status = STATUS_SUCCESS;

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    if (!NT_SUCCESS(status))
        goto fail2;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    __FdoAcquireMutex(Fdo);

    for (ListEntry = Fdo->Dx->ListEntry.Flink;
         ListEntry != &Fdo->Dx->ListEntry;
         ListEntry = ListEntry->Flink) {
        PXENBUS_DX  Dx = CONTAINING_RECORD(ListEntry, XENBUS_DX, ListEntry);
        PXENBUS_PDO Pdo = Dx->Pdo;

        ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

        if (PdoGetDevicePnpState(Pdo) == Deleted &&
            PdoIsMissing(Pdo))
            PdoDestroy(Pdo);
    }

    __FdoReleaseMutex(Fdo);

done:
    return status;

fail2:
    Error("fail2\n");

    __FdoAcquireMutex(Fdo);

fail1:
    Error("fail1 (%08x)\n", status);

    __FdoReleaseMutex(Fdo);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
FdoQueryCapabilities(
    IN  PXENBUS_FDO         Fdo,
    IN  PIRP                Irp
    )
{
    PIO_STACK_LOCATION      StackLocation;
    PDEVICE_CAPABILITIES    Capabilities;
    SYSTEM_POWER_STATE      SystemPowerState;
    NTSTATUS                status;

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    Capabilities = StackLocation->Parameters.DeviceCapabilities.Capabilities;

    Fdo->LowerDeviceCapabilities = *Capabilities;

    // Make sure that the FDO is non-removable
    Capabilities->Removable = 0;

    for (SystemPowerState = 0; SystemPowerState < PowerSystemMaximum; SystemPowerState++) {
        DEVICE_POWER_STATE  DevicePowerState;

        DevicePowerState = Fdo->LowerDeviceCapabilities.DeviceState[SystemPowerState];
    }

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
FdoDeviceUsageNotification(
    IN  PXENBUS_FDO                 Fdo,
    IN  PIRP                        Irp
    )
{
    PIO_STACK_LOCATION              StackLocation;
    DEVICE_USAGE_NOTIFICATION_TYPE  Type;
    BOOLEAN                         InPath;
    BOOLEAN                         NotDisableable;
    NTSTATUS                        status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    Type = StackLocation->Parameters.UsageNotification.Type;
    InPath = StackLocation->Parameters.UsageNotification.InPath;

    if (InPath) {
        Trace("%s: ADDING %s\n",
              __FdoGetName(Fdo),
              DeviceUsageNotificationTypeName(Type));
        Fdo->Usage[Type]++;
    } else {
        if (Fdo->Usage[Type] != 0) {
            Trace("%s: REMOVING %s\n",
                  __FdoGetName(Fdo),
                  DeviceUsageNotificationTypeName(Type));
            --Fdo->Usage[Type];
        }
    }

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    NotDisableable = FALSE;    
    for (Type = 0; Type <= DeviceUsageTypeDumpFile; Type++) {
        if (Fdo->Usage[Type] != 0) {
            NotDisableable = TRUE;
            break;
        }
    }

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    if (Fdo->NotDisableable != NotDisableable) {
        Fdo->NotDisableable = NotDisableable;
    
        IoInvalidateDeviceState(__FdoGetPhysicalDeviceObject(Fdo));
    }

    return status;

fail1:
    Error("fail1 (%08x)\n", status);

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
FdoQueryPnpDeviceState(
    IN  PXENBUS_FDO                 Fdo,
    IN  PIRP                        Irp
    )
{
    ULONG_PTR                       State;
    NTSTATUS                        status;

    if (Irp->IoStatus.Status == STATUS_SUCCESS)
        State = Irp->IoStatus.Information;
    else if (Irp->IoStatus.Status == STATUS_NOT_SUPPORTED)
        State = 0;
    else
        goto done;

    if (Fdo->NotDisableable) {
        Info("%s: not disableable\n", __FdoGetName(Fdo));
        State |= PNP_DEVICE_NOT_DISABLEABLE;
    }

    Irp->IoStatus.Information = State;
    Irp->IoStatus.Status = STATUS_SUCCESS;

done:
    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;
}

static NTSTATUS
FdoDispatchPnp(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    UCHAR               MinorFunction;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    MinorFunction = StackLocation->MinorFunction;

    Trace("====> (%02x:%s)\n",
          MinorFunction, 
          PnpMinorFunctionName(MinorFunction)); 

    switch (StackLocation->MinorFunction) {
    case IRP_MN_START_DEVICE:
        status = FdoStartDevice(Fdo, Irp);
        break;

    case IRP_MN_QUERY_STOP_DEVICE:
        status = FdoQueryStopDevice(Fdo, Irp);
        break;

    case IRP_MN_CANCEL_STOP_DEVICE:
        status = FdoCancelStopDevice(Fdo, Irp);
        break;

    case IRP_MN_STOP_DEVICE:
        status = FdoStopDevice(Fdo, Irp);
        break;

    case IRP_MN_QUERY_REMOVE_DEVICE:
        status = FdoQueryRemoveDevice(Fdo, Irp);
        break;

    case IRP_MN_SURPRISE_REMOVAL:
        status = FdoSurpriseRemoval(Fdo, Irp);
        break;

    case IRP_MN_REMOVE_DEVICE:
        status = FdoRemoveDevice(Fdo, Irp);
        break;

    case IRP_MN_CANCEL_REMOVE_DEVICE:
        status = FdoCancelRemoveDevice(Fdo, Irp);
        break;

    case IRP_MN_QUERY_DEVICE_RELATIONS:
        status = FdoQueryDeviceRelations(Fdo, Irp);
        break;

    case IRP_MN_QUERY_CAPABILITIES:
        status = FdoQueryCapabilities(Fdo, Irp);
        break;

    case IRP_MN_DEVICE_USAGE_NOTIFICATION:
        status = FdoDeviceUsageNotification(Fdo, Irp);
        break;

    case IRP_MN_QUERY_PNP_DEVICE_STATE:
        status = FdoQueryPnpDeviceState(Fdo, Irp);
        break;

    default:
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
        break;
    }

    Trace("<==== (%02x:%s)(%08x)\n",
          MinorFunction, 
          PnpMinorFunctionName(MinorFunction),
          status); 

    return status;
}

static NTSTATUS
FdoSetDevicePowerUp(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    DEVICE_POWER_STATE  DeviceState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    DeviceState = StackLocation->Parameters.Power.State.DeviceState;

    ASSERT3U(DeviceState, <,  __FdoGetDevicePowerState(Fdo));

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    if (!NT_SUCCESS(status))
        goto done;

    Info("%s: %s -> %s\n",
         __FdoGetName(Fdo),
         DevicePowerStateName(__FdoGetDevicePowerState(Fdo)),
         DevicePowerStateName(DeviceState));

    ASSERT3U(DeviceState, ==, PowerDeviceD0);
    status = FdoD3ToD0(Fdo);
    ASSERT(NT_SUCCESS(status));

done:
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
FdoSetDevicePowerDown(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    DEVICE_POWER_STATE  DeviceState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    DeviceState = StackLocation->Parameters.Power.State.DeviceState;

    ASSERT3U(DeviceState, >,  __FdoGetDevicePowerState(Fdo));

    Info("%s: %s -> %s\n",
         __FdoGetName(Fdo),
         DevicePowerStateName(__FdoGetDevicePowerState(Fdo)),
         DevicePowerStateName(DeviceState));

    ASSERT3U(DeviceState, ==, PowerDeviceD3);

    if (__FdoGetDevicePowerState(Fdo) == PowerDeviceD0)
        FdoD0ToD3(Fdo);

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
FdoSetDevicePower(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    DEVICE_POWER_STATE  DeviceState;
    POWER_ACTION        PowerAction;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    DeviceState = StackLocation->Parameters.Power.State.DeviceState;
    PowerAction = StackLocation->Parameters.Power.ShutdownType;

    Trace("====> (%s:%s)\n",
          DevicePowerStateName(DeviceState), 
          PowerActionName(PowerAction));

    ASSERT3U(PowerAction, <,  PowerActionShutdown);

    if (DeviceState == __FdoGetDevicePowerState(Fdo)) {
        status = FdoForwardIrpSynchronously(Fdo, Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        goto done;
    }

    status = (DeviceState < __FdoGetDevicePowerState(Fdo)) ?
             FdoSetDevicePowerUp(Fdo, Irp) :
             FdoSetDevicePowerDown(Fdo, Irp);

done:
    Trace("<==== (%s:%s)(%08x)\n",
          DevicePowerStateName(DeviceState), 
          PowerActionName(PowerAction),
          status);
    return status;
}

__drv_functionClass(REQUEST_POWER_COMPLETE)
__drv_sameIRQL
VOID
FdoRequestSetDevicePowerCompletion(
    IN  PDEVICE_OBJECT      DeviceObject,
    IN  UCHAR               MinorFunction,
    IN  POWER_STATE         PowerState,
    IN  PVOID               Context,
    IN  PIO_STATUS_BLOCK    IoStatus
    )
{
    PKEVENT                 Event = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(MinorFunction);
    UNREFERENCED_PARAMETER(PowerState);

    ASSERT(NT_SUCCESS(IoStatus->Status));

    KeSetEvent(Event, IO_NO_INCREMENT, FALSE);
}

static VOID
FdoRequestSetDevicePower(
    IN  PXENBUS_FDO         Fdo,
    IN  DEVICE_POWER_STATE  DeviceState
    )
{
    POWER_STATE             PowerState;
    KEVENT                  Event;
    NTSTATUS                status;

    Trace("%s\n", DevicePowerStateName(DeviceState));

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    PowerState.DeviceState = DeviceState;
    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    status = PoRequestPowerIrp(Fdo->LowerDeviceObject,
                               IRP_MN_SET_POWER,
                               PowerState,
                               FdoRequestSetDevicePowerCompletion,
                               &Event,
                               NULL);
    ASSERT(NT_SUCCESS(status));

    (VOID) KeWaitForSingleObject(&Event,
                                 Executive,
                                 KernelMode,
                                 FALSE,
                                 NULL);
}

static NTSTATUS
FdoSetSystemPowerUp(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{

    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;
    DEVICE_POWER_STATE  DeviceState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;

    ASSERT3U(SystemState, <,  __FdoGetSystemPowerState(Fdo));

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    if (!NT_SUCCESS(status))
        goto done;

    Info("%s: %s -> %s\n",
         __FdoGetName(Fdo),
         SystemPowerStateName(__FdoGetSystemPowerState(Fdo)),
         SystemPowerStateName(SystemState));

    if (SystemState < PowerSystemHibernate &&
        __FdoGetSystemPowerState(Fdo) >= PowerSystemHibernate) {
        __FdoSetSystemPowerState(Fdo, PowerSystemHibernate);
        FdoS4ToS3(Fdo);
    }

    __FdoSetSystemPowerState(Fdo, SystemState);

    DeviceState = Fdo->LowerDeviceCapabilities.DeviceState[SystemState];
    FdoRequestSetDevicePower(Fdo, DeviceState);

done:
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
FdoSetSystemPowerDown(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;
    DEVICE_POWER_STATE  DeviceState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;

    ASSERT3U(SystemState, >,  __FdoGetSystemPowerState(Fdo));

    DeviceState = Fdo->LowerDeviceCapabilities.DeviceState[SystemState];

    FdoRequestSetDevicePower(Fdo, DeviceState);

    Info("%s: %s -> %s\n",
         __FdoGetName(Fdo),
         SystemPowerStateName(__FdoGetSystemPowerState(Fdo)),
         SystemPowerStateName(SystemState));

    if (SystemState >= PowerSystemHibernate &&
        __FdoGetSystemPowerState(Fdo) < PowerSystemHibernate) {
        __FdoSetSystemPowerState(Fdo, PowerSystemSleeping3);
        FdoS3ToS4(Fdo);
    }

    __FdoSetSystemPowerState(Fdo, SystemState);

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
FdoSetSystemPower(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;
    POWER_ACTION        PowerAction;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;
    PowerAction = StackLocation->Parameters.Power.ShutdownType;

    Trace("====> (%s:%s)\n",
          SystemPowerStateName(SystemState), 
          PowerActionName(PowerAction));

    ASSERT3U(PowerAction, <,  PowerActionShutdown);

    if (SystemState == __FdoGetSystemPowerState(Fdo)) {
        status = FdoForwardIrpSynchronously(Fdo, Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        goto done;
    }

    status = (SystemState < __FdoGetSystemPowerState(Fdo)) ?
             FdoSetSystemPowerUp(Fdo, Irp) :
             FdoSetSystemPowerDown(Fdo, Irp);

done:
    Trace("<==== (%s:%s)(%08x)\n",
          SystemPowerStateName(SystemState), 
          PowerActionName(PowerAction),
          status);
    return status;
}

static NTSTATUS
FdoQueryDevicePowerUp(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    DEVICE_POWER_STATE  DeviceState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    DeviceState = StackLocation->Parameters.Power.State.DeviceState;

    ASSERT3U(DeviceState, <,  __FdoGetDevicePowerState(Fdo));

    status = FdoForwardIrpSynchronously(Fdo, Irp);

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
FdoQueryDevicePowerDown(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    DEVICE_POWER_STATE  DeviceState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    DeviceState = StackLocation->Parameters.Power.State.DeviceState;

    ASSERT3U(DeviceState, >,  __FdoGetDevicePowerState(Fdo));

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
FdoQueryDevicePower(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    DEVICE_POWER_STATE  DeviceState;
    POWER_ACTION        PowerAction;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    DeviceState = StackLocation->Parameters.Power.State.DeviceState;
    PowerAction = StackLocation->Parameters.Power.ShutdownType;

    Trace("====> (%s:%s)\n",
          DevicePowerStateName(DeviceState), 
          PowerActionName(PowerAction));

    ASSERT3U(PowerAction, <,  PowerActionShutdown);

    if (DeviceState == __FdoGetDevicePowerState(Fdo)) {
        status = FdoForwardIrpSynchronously(Fdo, Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        goto done;
    }

    status = (DeviceState < __FdoGetDevicePowerState(Fdo)) ?
             FdoQueryDevicePowerUp(Fdo, Irp) :
             FdoQueryDevicePowerDown(Fdo, Irp);

done:
    Trace("<==== (%s:%s)(%08x)\n",
          DevicePowerStateName(DeviceState), 
          PowerActionName(PowerAction),
          status);
    return status;
}

__drv_functionClass(REQUEST_POWER_COMPLETE)
__drv_sameIRQL
VOID
FdoRequestQueryDevicePowerCompletion(
    IN  PDEVICE_OBJECT      DeviceObject,
    IN  UCHAR               MinorFunction,
    IN  POWER_STATE         PowerState,
    IN  PVOID               Context,
    IN  PIO_STATUS_BLOCK    IoStatus
    )
{
    PKEVENT                 Event = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(MinorFunction);
    UNREFERENCED_PARAMETER(PowerState);

    ASSERT(NT_SUCCESS(IoStatus->Status));

    KeSetEvent(Event, IO_NO_INCREMENT, FALSE);
}

static VOID
FdoRequestQueryDevicePower(
    IN  PXENBUS_FDO         Fdo,
    IN  DEVICE_POWER_STATE  DeviceState
    )
{
    POWER_STATE             PowerState;
    KEVENT                  Event;
    NTSTATUS                status;

    Trace("%s\n", DevicePowerStateName(DeviceState));

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    PowerState.DeviceState = DeviceState;
    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    status = PoRequestPowerIrp(Fdo->LowerDeviceObject,
                               IRP_MN_QUERY_POWER,
                               PowerState,
                               FdoRequestQueryDevicePowerCompletion,
                               &Event,
                               NULL);
    ASSERT(NT_SUCCESS(status));

    (VOID) KeWaitForSingleObject(&Event,
                                 Executive,
                                 KernelMode,
                                 FALSE,
                                 NULL);
}

static NTSTATUS
FdoQuerySystemPowerUp(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{

    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;
    DEVICE_POWER_STATE  DeviceState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;

    ASSERT3U(SystemState, <,  __FdoGetSystemPowerState(Fdo));

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    if (!NT_SUCCESS(status))
        goto done;

    DeviceState = Fdo->LowerDeviceCapabilities.DeviceState[SystemState];

    FdoRequestQueryDevicePower(Fdo, DeviceState);

done:
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
FdoQuerySystemPowerDown(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;
    DEVICE_POWER_STATE  DeviceState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;

    ASSERT3U(SystemState, >,  __FdoGetSystemPowerState(Fdo));

    DeviceState = Fdo->LowerDeviceCapabilities.DeviceState[SystemState];

    FdoRequestQueryDevicePower(Fdo, DeviceState);

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
FdoQuerySystemPower(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;
    POWER_ACTION        PowerAction;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;
    PowerAction = StackLocation->Parameters.Power.ShutdownType;

    Trace("====> (%s:%s)\n",
          SystemPowerStateName(SystemState), 
          PowerActionName(PowerAction));

    ASSERT3U(PowerAction, <,  PowerActionShutdown);

    if (SystemState == __FdoGetSystemPowerState(Fdo)) {
        status = FdoForwardIrpSynchronously(Fdo, Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        goto done;
    }

    status = (SystemState < __FdoGetSystemPowerState(Fdo)) ?
             FdoQuerySystemPowerUp(Fdo, Irp) :
             FdoQuerySystemPowerDown(Fdo, Irp);

done:
    Trace("<==== (%s:%s)(%08x)\n",
          SystemPowerStateName(SystemState), 
          PowerActionName(PowerAction),
          status);

    return status;
}

static NTSTATUS
FdoDevicePower(
    IN  PXENBUS_THREAD  Self,
    IN  PVOID           Context
    )
{
    PXENBUS_FDO         Fdo = Context;
    PKEVENT             Event;

    Event = ThreadGetEvent(Self);

    for (;;) {
        PIRP                Irp;
        PIO_STACK_LOCATION  StackLocation;
        UCHAR               MinorFunction;

        if (Fdo->DevicePowerIrp == NULL) {
            (VOID) KeWaitForSingleObject(Event,
                                         Executive,
                                         KernelMode,
                                         FALSE,
                                         NULL);
            KeClearEvent(Event);
        }

        if (ThreadIsAlerted(Self))
            break;

        Irp = Fdo->DevicePowerIrp;

        if (Irp == NULL)
            continue;

        Fdo->DevicePowerIrp = NULL;
        KeMemoryBarrier();

        StackLocation = IoGetCurrentIrpStackLocation(Irp);
        MinorFunction = StackLocation->MinorFunction;

        switch (StackLocation->MinorFunction) {
        case IRP_MN_SET_POWER:
            (VOID) FdoSetDevicePower(Fdo, Irp);
            break;

        case IRP_MN_QUERY_POWER:
            (VOID) FdoQueryDevicePower(Fdo, Irp);
            break;

        default:
            ASSERT(FALSE);
            break;
        }
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
FdoSystemPower(
    IN  PXENBUS_THREAD  Self,
    IN  PVOID           Context
    )
{
    PXENBUS_FDO         Fdo = Context;
    PKEVENT             Event;

    Event = ThreadGetEvent(Self);

    for (;;) {
        PIRP                Irp;
        PIO_STACK_LOCATION  StackLocation;
        UCHAR               MinorFunction;

        if (Fdo->SystemPowerIrp == NULL) {
            (VOID) KeWaitForSingleObject(Event,
                                         Executive,
                                         KernelMode,
                                         FALSE,
                                         NULL);
            KeClearEvent(Event);
        }

        if (ThreadIsAlerted(Self))
            break;

        Irp = Fdo->SystemPowerIrp;

        if (Irp == NULL)
            continue;

        Fdo->SystemPowerIrp = NULL;
        KeMemoryBarrier();

        StackLocation = IoGetCurrentIrpStackLocation(Irp);
        MinorFunction = StackLocation->MinorFunction;

        switch (StackLocation->MinorFunction) {
        case IRP_MN_SET_POWER:
            (VOID) FdoSetSystemPower(Fdo, Irp);
            break;

        case IRP_MN_QUERY_POWER:
            (VOID) FdoQuerySystemPower(Fdo, Irp);
            break;

        default:
            ASSERT(FALSE);
            break;
        }
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
FdoDispatchPower(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    UCHAR               MinorFunction;
    POWER_STATE_TYPE    PowerType;
    POWER_ACTION        PowerAction;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    MinorFunction = StackLocation->MinorFunction;

    if (MinorFunction != IRP_MN_QUERY_POWER &&
        MinorFunction != IRP_MN_SET_POWER) {
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

        goto done;
    }

    PowerType = StackLocation->Parameters.Power.Type;
    PowerAction = StackLocation->Parameters.Power.ShutdownType;

    if (PowerAction >= PowerActionShutdown) {
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

        goto done;
    }

    switch (PowerType) {
    case DevicePowerState:
        IoMarkIrpPending(Irp);

        ASSERT3P(Fdo->DevicePowerIrp, ==, NULL);
        Fdo->DevicePowerIrp = Irp;
        KeMemoryBarrier();

        ThreadWake(Fdo->DevicePowerThread);

        status = STATUS_PENDING;
        break;

    case SystemPowerState:
        IoMarkIrpPending(Irp);

        ASSERT3P(Fdo->SystemPowerIrp, ==, NULL);
        Fdo->SystemPowerIrp = Irp;
        KeMemoryBarrier();

        ThreadWake(Fdo->SystemPowerThread);

        status = STATUS_PENDING;
        break;

    default:
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
        break;
    }

done:
    return status;
}

static NTSTATUS
FdoDispatchDefault(
    IN  PXENBUS_FDO Fdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;
}

NTSTATUS
FdoDispatch(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    switch (StackLocation->MajorFunction) {
    case IRP_MJ_PNP:
        status = FdoDispatchPnp(Fdo, Irp);
        break;

    case IRP_MJ_POWER:
        status = FdoDispatchPower(Fdo, Irp);
        break;

    default:
        status = FdoDispatchDefault(Fdo, Irp);
        break;
    }

    return status;
}

static NTSTATUS
FdoAcquireLowerBusInterface(
    IN  PXENBUS_FDO     Fdo
    )
{
    KEVENT              Event;
    IO_STATUS_BLOCK     StatusBlock;
    PIRP                Irp;
    PIO_STACK_LOCATION  StackLocation;
    NTSTATUS            status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    RtlZeroMemory(&StatusBlock, sizeof(IO_STATUS_BLOCK));

    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP,
                                       Fdo->LowerDeviceObject,
                                       NULL,
                                       0,
                                       NULL,
                                       &Event,
                                       &StatusBlock);

    status = STATUS_UNSUCCESSFUL;
    if (Irp == NULL)
        goto fail1;

    StackLocation = IoGetNextIrpStackLocation(Irp);
    StackLocation->MinorFunction = IRP_MN_QUERY_INTERFACE;

    StackLocation->Parameters.QueryInterface.InterfaceType = &GUID_BUS_INTERFACE_STANDARD;
    StackLocation->Parameters.QueryInterface.Size = sizeof (BUS_INTERFACE_STANDARD);
    StackLocation->Parameters.QueryInterface.Version = 1;
    StackLocation->Parameters.QueryInterface.Interface = (PINTERFACE)&Fdo->LowerBusInterface;
    
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
    if (status == STATUS_PENDING) {
        (VOID) KeWaitForSingleObject(&Event,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);
        status = StatusBlock.Status;
    }

    if (!NT_SUCCESS(status))
        goto fail2;

    status = STATUS_INVALID_PARAMETER;
    if (Fdo->LowerBusInterface.Version != 1)
        goto fail3;

    return STATUS_SUCCESS;

fail3:
    Error("fail3\n");

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static VOID
FdoReleaseLowerBusInterface(
    IN  PXENBUS_FDO         Fdo
    )
{
    PBUS_INTERFACE_STANDARD BusInterface;

    BusInterface = &Fdo->LowerBusInterface;
    BusInterface->InterfaceDereference(BusInterface->Context);

    RtlZeroMemory(BusInterface, sizeof (BUS_INTERFACE_STANDARD));
}

#define SERVICES_KEY        L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services"

static NTSTATUS
FdoQueryInterface(
    IN  PXENBUS_FDO     Fdo,
    IN  const WCHAR     *ProviderName,
    IN  const CHAR      *InterfaceName,
    IN  const GUID      *Guid,
    IN  ULONG           Version,
    OUT PINTERFACE      Interface,
    IN  ULONG           Size,
    IN  BOOLEAN         Optional
    )
{
    UNICODE_STRING      Unicode;
    HANDLE              InterfacesKey;
    HANDLE              SubscriberKey;
    KEVENT              Event;
    IO_STATUS_BLOCK     StatusBlock;
    PIRP                Irp;
    PIO_STACK_LOCATION  StackLocation;
    NTSTATUS            status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    Unicode.MaximumLength = (USHORT)((wcslen(SERVICES_KEY) +
                                      1 +
                                      wcslen(ProviderName) +
                                      1 +
                                      wcslen(L"Interfaces") +
                                      1) * sizeof (WCHAR));

    Unicode.Buffer = __FdoAllocate(Unicode.MaximumLength);

    status = STATUS_NO_MEMORY;
    if (Unicode.Buffer == NULL)
        goto fail1;

    status = RtlStringCbPrintfW(Unicode.Buffer,
                                Unicode.MaximumLength,
                                SERVICES_KEY L"\\%ws\\Interfaces",
                                ProviderName);
    ASSERT(NT_SUCCESS(status));

    Unicode.Length = (USHORT)(wcslen(Unicode.Buffer) * sizeof (WCHAR));

    status = RegistryOpenKey(NULL, &Unicode, KEY_READ, &InterfacesKey);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = RegistryCreateSubKey(InterfacesKey, 
                                  "XENBUS", 
                                  REG_OPTION_NON_VOLATILE, 
                                  &SubscriberKey);
    if (!NT_SUCCESS(status))
        goto fail3;
                   
    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    RtlZeroMemory(&StatusBlock, sizeof(IO_STATUS_BLOCK));

    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP,
                                       Fdo->LowerDeviceObject,
                                       NULL,
                                       0,
                                       NULL,
                                       &Event,
                                       &StatusBlock);

    status = STATUS_UNSUCCESSFUL;
    if (Irp == NULL)
        goto fail4;

    StackLocation = IoGetNextIrpStackLocation(Irp);
    StackLocation->MinorFunction = IRP_MN_QUERY_INTERFACE;

    StackLocation->Parameters.QueryInterface.InterfaceType = Guid;
    StackLocation->Parameters.QueryInterface.Size = (USHORT)Size;
    StackLocation->Parameters.QueryInterface.Version = (USHORT)Version;
    StackLocation->Parameters.QueryInterface.Interface = Interface;
    
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
    if (status == STATUS_PENDING) {
        (VOID) KeWaitForSingleObject(&Event,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);
        status = StatusBlock.Status;
    }

    if (!NT_SUCCESS(status)) {
        if (status == STATUS_NOT_SUPPORTED && Optional)
            goto done;

        goto fail5;
    }

    status = RegistryUpdateDwordValue(SubscriberKey,
                                      (PCHAR)InterfaceName,
                                      Version);
    if (!NT_SUCCESS(status))
        goto fail6;

done:
    RegistryCloseKey(SubscriberKey);

    RegistryCloseKey(InterfacesKey);

    __FdoFree(Unicode.Buffer);

    return STATUS_SUCCESS;

fail6:
    Error("fail6\n");

fail5:
    Error("fail5\n");

fail4:
    Error("fail4\n");

    RegistryCloseKey(SubscriberKey);

fail3:
    Error("fail3\n");

    RegistryCloseKey(InterfacesKey);

fail2:
    Error("fail2\n");

    __FdoFree(Unicode.Buffer);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

#define FDO_QUERY_INTERFACE(                                                            \
    _Fdo,                                                                               \
    _ProviderName,                                                                      \
    _InterfaceName,                                                                     \
    _Version,                                                                           \
    _Interface,                                                                         \
    _Size,                                                                              \
    _Optional)                                                                          \
    FdoQueryInterface((_Fdo),                                                           \
                        L ## #_ProviderName,                                            \
                        #_InterfaceName,                                                \
                        &GUID_ ## _ProviderName ## _ ## _InterfaceName ## _INTERFACE,   \
                        (_Version),                                                     \
                        (_Interface),                                                   \
                        (_Size),                                                        \
                        (_Optional))

VOID
FdoGetUnplugInterface(
    IN  PXENBUS_FDO                 Fdo,
    OUT PXENFILT_UNPLUG_INTERFACE   UnplugInterface
    )
{
    *UnplugInterface = Fdo->UnplugInterface;
}

static BOOLEAN
FdoIsBalloonEnabled(
    IN  PXENBUS_FDO Fdo
    )
{
    CHAR            Key[] = "XEN:BALLOON=";
    PANSI_STRING    Option;
    PCHAR           Value;
    BOOLEAN         Enabled;
    NTSTATUS        status;

    UNREFERENCED_PARAMETER(Fdo);

    Enabled = TRUE;

    status = RegistryQuerySystemStartOption(Key, &Option);
    if (!NT_SUCCESS(status))
        goto done;

    Value = Option->Buffer + sizeof (Key) - 1;

    if (strcmp(Value, "OFF") == 0)
        Enabled = FALSE;

    RegistryFreeSzValue(Option);

done:
    return Enabled;
}

NTSTATUS
FdoCreate(
    IN  PDEVICE_OBJECT          PhysicalDeviceObject,
    IN  BOOLEAN                 Active      
    )
{
    PDEVICE_OBJECT              FunctionDeviceObject;
    PXENBUS_DX                  Dx;
    PXENBUS_FDO                 Fdo;
    PBUS_INTERFACE_STANDARD     BusInterface;
    USHORT                      DeviceID;
    NTSTATUS                    status;

#pragma prefast(suppress:28197) // Possibly leaking memory 'FunctionDeviceObject'
    status = IoCreateDevice(DriverGetDriverObject(),
                            sizeof (XENBUS_DX),
                            NULL,
                            FILE_DEVICE_BUS_EXTENDER,
                            FILE_DEVICE_SECURE_OPEN,
                            FALSE,
                            &FunctionDeviceObject);
    if (!NT_SUCCESS(status))
        goto fail1;

    Dx = (PXENBUS_DX)FunctionDeviceObject->DeviceExtension;
    RtlZeroMemory(Dx, sizeof (XENBUS_DX));

    Dx->Type = FUNCTION_DEVICE_OBJECT;
    Dx->DeviceObject = FunctionDeviceObject;
    Dx->DevicePnpState = Added;
    Dx->SystemPowerState = PowerSystemWorking;
    Dx->DevicePowerState = PowerDeviceD3;

    Fdo = __FdoAllocate(sizeof (XENBUS_FDO));

    status = STATUS_NO_MEMORY;
    if (Fdo == NULL)
        goto fail2;

    Fdo->Dx = Dx;
    Fdo->PhysicalDeviceObject = PhysicalDeviceObject;
    Fdo->LowerDeviceObject = IoAttachDeviceToDeviceStack(FunctionDeviceObject,
                                                         PhysicalDeviceObject);

    status = ThreadCreate(FdoSystemPower, Fdo, &Fdo->SystemPowerThread);
    if (!NT_SUCCESS(status))
        goto fail3;

    status = ThreadCreate(FdoDevicePower, Fdo, &Fdo->DevicePowerThread);
    if (!NT_SUCCESS(status))
        goto fail4;

    status = FdoAcquireLowerBusInterface(Fdo);
    if (!NT_SUCCESS(status))
        goto fail5;

    BusInterface = &Fdo->LowerBusInterface;

    status = STATUS_UNSUCCESSFUL;
    if (BusInterface->GetBusData(BusInterface->Context,
                                 PCI_WHICHSPACE_CONFIG,
                                 &DeviceID,
                                 FIELD_OFFSET(PCI_COMMON_HEADER, DeviceID),
                                 FIELD_SIZE(PCI_COMMON_HEADER, DeviceID)) == 0)
        goto fail6;

    __FdoSetVendorName(Fdo, DeviceID);

    __FdoSetName(Fdo);

    __FdoSetActive(Fdo, Active);

    if (!__FdoIsActive(Fdo))
        goto done;

    status = DebugInitialize(Fdo, &Fdo->DebugContext);
    if (!NT_SUCCESS(status))
        goto fail7;

    status = SuspendInitialize(Fdo, &Fdo->SuspendContext);
    if (!NT_SUCCESS(status))
        goto fail8;

    status = SharedInfoInitialize(Fdo, &Fdo->SharedInfoContext);
    if (!NT_SUCCESS(status))
        goto fail9;

    status = EvtchnInitialize(Fdo, &Fdo->EvtchnContext);
    if (!NT_SUCCESS(status))
        goto fail10;

    status = StoreInitialize(Fdo, &Fdo->StoreContext);
    if (!NT_SUCCESS(status))
        goto fail11;

    status = RangeSetInitialize(Fdo, &Fdo->RangeSetContext);
    if (!NT_SUCCESS(status))
        goto fail12;

    status = CacheInitialize(Fdo, &Fdo->CacheContext);
    if (!NT_SUCCESS(status))
        goto fail13;

    status = GnttabInitialize(Fdo, &Fdo->GnttabContext);
    if (!NT_SUCCESS(status))
        goto fail14;

    if (FdoIsBalloonEnabled(Fdo)) {
        status = BalloonInitialize(Fdo, &Fdo->BalloonContext);
        if (!NT_SUCCESS(status))
            goto fail15;
    }

    status = DebugGetInterface(__FdoGetDebugContext(Fdo),
                               XENBUS_DEBUG_INTERFACE_VERSION_MAX,
                               (PINTERFACE)&Fdo->DebugInterface,
                               sizeof (Fdo->DebugInterface));
    ASSERT(NT_SUCCESS(status));
    ASSERT(Fdo->DebugInterface.Interface.Context != NULL);

    status = SuspendGetInterface(__FdoGetSuspendContext(Fdo),
                                 XENBUS_SUSPEND_INTERFACE_VERSION_MAX,
                                 (PINTERFACE)&Fdo->SuspendInterface,
                                 sizeof (Fdo->SuspendInterface));
    ASSERT(NT_SUCCESS(status));
    ASSERT(Fdo->SuspendInterface.Interface.Context != NULL);

    status = EvtchnGetInterface(__FdoGetEvtchnContext(Fdo),
                                XENBUS_EVTCHN_INTERFACE_VERSION_MAX,
                                (PINTERFACE)&Fdo->EvtchnInterface,
                                sizeof (Fdo->EvtchnInterface));
    ASSERT(NT_SUCCESS(status));
    ASSERT(Fdo->EvtchnInterface.Interface.Context != NULL);

    status = StoreGetInterface(__FdoGetStoreContext(Fdo),
                               XENBUS_STORE_INTERFACE_VERSION_MAX,
                               (PINTERFACE)&Fdo->StoreInterface,
                               sizeof (Fdo->StoreInterface));
    ASSERT(NT_SUCCESS(status));
    ASSERT(Fdo->StoreInterface.Interface.Context != NULL);

    status = RangeSetGetInterface(__FdoGetRangeSetContext(Fdo),
                                  XENBUS_RANGE_SET_INTERFACE_VERSION_MAX,
                                  (PINTERFACE)&Fdo->RangeSetInterface,
                                  sizeof (Fdo->RangeSetInterface));
    ASSERT(NT_SUCCESS(status));
    ASSERT(Fdo->RangeSetInterface.Interface.Context != NULL);

    status = BalloonGetInterface(__FdoGetBalloonContext(Fdo),
                                 XENBUS_BALLOON_INTERFACE_VERSION_MAX,
                                 (PINTERFACE)&Fdo->BalloonInterface,
                                 sizeof (Fdo->BalloonInterface));
    ASSERT(NT_SUCCESS(status));

    status = FDO_QUERY_INTERFACE(Fdo,
                                 XENFILT,
                                 UNPLUG,
                                 XENFILT_UNPLUG_INTERFACE_VERSION_MAX,
                                 (PINTERFACE)&Fdo->UnplugInterface,
                                 sizeof (Fdo->UnplugInterface),
                                 TRUE);
    if (!NT_SUCCESS(status))
        goto fail16;

done:
    InitializeMutex(&Fdo->Mutex);
    InitializeListHead(&Dx->ListEntry);
    Fdo->References = 1;

    Info("%p (%s) %s\n",
         FunctionDeviceObject,
         __FdoGetName(Fdo),
         (__FdoIsActive(Fdo)) ? "[ACTIVE]" : "");

    Dx->Fdo = Fdo;
    FunctionDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;

fail16:
    Error("fail16\n");

    RtlZeroMemory(&Fdo->BalloonInterface,
                  sizeof (XENBUS_BALLOON_INTERFACE));

    RtlZeroMemory(&Fdo->RangeSetInterface,
                  sizeof (XENBUS_RANGE_SET_INTERFACE));

    RtlZeroMemory(&Fdo->StoreInterface,
                  sizeof (XENBUS_STORE_INTERFACE));

    RtlZeroMemory(&Fdo->EvtchnInterface,
                  sizeof (XENBUS_EVTCHN_INTERFACE));

    RtlZeroMemory(&Fdo->SuspendInterface,
                  sizeof (XENBUS_SUSPEND_INTERFACE));

    RtlZeroMemory(&Fdo->DebugInterface,
                  sizeof (XENBUS_DEBUG_INTERFACE));

    if (Fdo->BalloonContext != NULL) {
        BalloonTeardown(Fdo->BalloonContext);
        Fdo->BalloonContext = NULL;
    }

fail15:
    Error("fail15\n");

    GnttabTeardown(Fdo->GnttabContext);
    Fdo->GnttabContext = NULL;

fail14:
    Error("fail14\n");

    CacheTeardown(Fdo->CacheContext);
    Fdo->CacheContext = NULL;

fail13:
    Error("fail13\n");

    RangeSetTeardown(Fdo->RangeSetContext);
    Fdo->RangeSetContext = NULL;

fail12:
    Error("fail12\n");

    StoreTeardown(Fdo->StoreContext);
    Fdo->StoreContext = NULL;

fail11:
    Error("fail11\n");

    EvtchnTeardown(Fdo->EvtchnContext);
    Fdo->EvtchnContext = NULL;

fail10:
    Error("fail10\n");

    SharedInfoTeardown(Fdo->SharedInfoContext);
    Fdo->SharedInfoContext = NULL;

fail9:
    Error("fail9\n");

    SuspendTeardown(Fdo->SuspendContext);
    Fdo->SuspendContext = NULL;

fail8:
    Error("fail8\n");

    DebugTeardown(Fdo->DebugContext);
    Fdo->DebugContext = NULL;

fail7:
    Error("fail7\n");

    __FdoSetActive(Fdo, FALSE);

    RtlZeroMemory(Fdo->VendorName, MAXNAMELEN);

fail6:
    Error("fail6\n");

    FdoReleaseLowerBusInterface(Fdo);

fail5:
    Error("fail5\n");

    ThreadAlert(Fdo->DevicePowerThread);
    ThreadJoin(Fdo->DevicePowerThread);
    Fdo->DevicePowerThread = NULL;
    
fail4:
    Error("fail4\n");

    ThreadAlert(Fdo->SystemPowerThread);
    ThreadJoin(Fdo->SystemPowerThread);
    Fdo->SystemPowerThread = NULL;
    
fail3:
    Error("fail3\n");

#pragma prefast(suppress:28183) // Fdo->LowerDeviceObject could be NULL
    IoDetachDevice(Fdo->LowerDeviceObject);

    Fdo->PhysicalDeviceObject = NULL;
    Fdo->LowerDeviceObject = NULL;
    Fdo->Dx = NULL;

    ASSERT(IsZeroMemory(Fdo, sizeof (XENBUS_FDO)));
    __FdoFree(Fdo);

fail2:
    Error("fail2\n");

    IoDeleteDevice(FunctionDeviceObject);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
FdoDestroy(
    IN  PXENBUS_FDO Fdo
    )
{
    PXENBUS_DX      Dx = Fdo->Dx;
    PDEVICE_OBJECT  FunctionDeviceObject = Dx->DeviceObject;

    ASSERT(IsListEmpty(&Dx->ListEntry));
    ASSERT3U(Fdo->References, ==, 0);
    ASSERT3U(__FdoGetDevicePnpState(Fdo), ==, Deleted);

    Fdo->NotDisableable = FALSE;

    Info("%p (%s)\n",
         FunctionDeviceObject,
         __FdoGetName(Fdo));

    Dx->Fdo = NULL;

    RtlZeroMemory(&Fdo->Mutex, sizeof (MUTEX));

    if (__FdoIsActive(Fdo)) {
        RtlZeroMemory(&Fdo->UnplugInterface,
                      sizeof (XENFILT_UNPLUG_INTERFACE));

        RtlZeroMemory(&Fdo->BalloonInterface,
                      sizeof (XENBUS_BALLOON_INTERFACE));

        RtlZeroMemory(&Fdo->RangeSetInterface,
                      sizeof (XENBUS_RANGE_SET_INTERFACE));

        RtlZeroMemory(&Fdo->StoreInterface,
                      sizeof (XENBUS_STORE_INTERFACE));

        RtlZeroMemory(&Fdo->EvtchnInterface,
                      sizeof (XENBUS_EVTCHN_INTERFACE));

        RtlZeroMemory(&Fdo->SuspendInterface,
                      sizeof (XENBUS_SUSPEND_INTERFACE));

        RtlZeroMemory(&Fdo->DebugInterface,
                      sizeof (XENBUS_DEBUG_INTERFACE));

        if (Fdo->BalloonContext != NULL) {
            BalloonTeardown(Fdo->BalloonContext);
            Fdo->BalloonContext = NULL;
        }

        GnttabTeardown(Fdo->GnttabContext);
        Fdo->GnttabContext = NULL;

        CacheTeardown(Fdo->CacheContext);
        Fdo->CacheContext = NULL;

        RangeSetTeardown(Fdo->RangeSetContext);
        Fdo->RangeSetContext = NULL;

        StoreTeardown(Fdo->StoreContext);
        Fdo->StoreContext = NULL;

        EvtchnTeardown(Fdo->EvtchnContext);
        Fdo->EvtchnContext = NULL;

        SharedInfoTeardown(Fdo->SharedInfoContext);
        Fdo->SharedInfoContext = NULL;

        SuspendTeardown(Fdo->SuspendContext);
        Fdo->SuspendContext = NULL;

        DebugTeardown(Fdo->DebugContext);
        Fdo->DebugContext = NULL;

        __FdoSetActive(Fdo, FALSE);
    }

    RtlZeroMemory(Fdo->VendorName, MAXNAMELEN);

    FdoReleaseLowerBusInterface(Fdo);

    ThreadAlert(Fdo->DevicePowerThread);
    ThreadJoin(Fdo->DevicePowerThread);
    Fdo->DevicePowerThread = NULL;

    ThreadAlert(Fdo->SystemPowerThread);
    ThreadJoin(Fdo->SystemPowerThread);
    Fdo->SystemPowerThread = NULL;

    IoDetachDevice(Fdo->LowerDeviceObject);

    RtlZeroMemory(&Fdo->LowerDeviceCapabilities, sizeof (DEVICE_CAPABILITIES));
    Fdo->LowerDeviceObject = NULL;
    Fdo->PhysicalDeviceObject = NULL;
    Fdo->Dx = NULL;

    ASSERT(IsZeroMemory(Fdo, sizeof (XENBUS_FDO)));
    __FdoFree(Fdo);

    ASSERT3U(Dx->DevicePowerState, ==, PowerDeviceD3);
    ASSERT3U(Dx->SystemPowerState, ==, PowerSystemWorking);

    IoDeleteDevice(FunctionDeviceObject);
}
