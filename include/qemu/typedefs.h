#ifndef QEMU_TYPEDEFS_H
#define QEMU_TYPEDEFS_H

/* A load of opaque types so that device init declarations don't have to
   pull in all the real definitions.  */

/* Please keep this list in case-insensitive alphabetical order */
typedef struct AdapterInfo AdapterInfo;
typedef struct AddressSpace AddressSpace;
typedef struct AioContext AioContext;
typedef struct AllwinnerAHCIState AllwinnerAHCIState;
typedef struct AudioState AudioState;
typedef struct BdrvDirtyBitmap BdrvDirtyBitmap;
typedef struct BdrvDirtyBitmapIter BdrvDirtyBitmapIter;
typedef struct BlockBackend BlockBackend;
typedef struct BlockBackendRootState BlockBackendRootState;
typedef struct BlockDriverState BlockDriverState;
typedef struct BusClass BusClass;
typedef struct BusState BusState;
typedef struct Chardev Chardev;
typedef struct CompatProperty CompatProperty;
typedef struct CoMutex CoMutex;
typedef struct CPUAddressSpace CPUAddressSpace;
typedef struct CPUState CPUState;
typedef struct DeviceListener DeviceListener;
typedef struct DeviceState DeviceState;
typedef struct DirtyBitmapSnapshot DirtyBitmapSnapshot;
typedef struct DisplayChangeListener DisplayChangeListener;
typedef struct DisplayState DisplayState;
typedef struct DisplaySurface DisplaySurface;
typedef struct DriveInfo DriveInfo;
typedef struct Error Error;
typedef struct EventNotifier EventNotifier;
typedef struct FlatView FlatView;
typedef struct FWCfgEntry FWCfgEntry;
typedef struct FWCfgIoState FWCfgIoState;
typedef struct FWCfgMemState FWCfgMemState;
typedef struct FWCfgState FWCfgState;
typedef struct HCIInfo HCIInfo;
typedef struct HVFX86EmulatorState HVFX86EmulatorState;
typedef struct I2CBus I2CBus;
typedef struct I2SCodec I2SCodec;
typedef struct IOMMUMemoryRegion IOMMUMemoryRegion;
typedef struct ISABus ISABus;
typedef struct ISADevice ISADevice;
typedef struct IsaDma IsaDma;
typedef struct MACAddr MACAddr;
typedef struct MachineClass MachineClass;
typedef struct MachineState MachineState;
typedef struct MemoryListener MemoryListener;
typedef struct MemoryMappingList MemoryMappingList;
typedef struct MemoryRegion MemoryRegion;
typedef struct MemoryRegionCache MemoryRegionCache;
typedef struct MemoryRegionSection MemoryRegionSection;
typedef struct MigrationIncomingState MigrationIncomingState;
typedef struct MigrationState MigrationState;
typedef struct Monitor Monitor;
typedef struct MonitorDef MonitorDef;
typedef struct MouseTransformInfo MouseTransformInfo;
typedef struct MSIMessage MSIMessage;
typedef struct NetClientState NetClientState;
typedef struct NetFilterState NetFilterState;
typedef struct NICInfo NICInfo;
typedef struct NodeInfo NodeInfo;
typedef struct NumaNodeMem NumaNodeMem;
typedef struct ObjectClass ObjectClass;
typedef struct PCIBridge PCIBridge;
typedef struct PCIBus PCIBus;
typedef struct PCIDevice PCIDevice;
typedef struct PCIEAERErr PCIEAERErr;
typedef struct PCIEAERLog PCIEAERLog;
typedef struct PCIEAERMsg PCIEAERMsg;
typedef struct PCIEPort PCIEPort;
typedef struct PCIESlot PCIESlot;
typedef struct PCIExpressDevice PCIExpressDevice;
typedef struct PCIExpressHost PCIExpressHost;
typedef struct PCIHostDeviceAddress PCIHostDeviceAddress;
typedef struct PCIHostState PCIHostState;
typedef struct PCMachineClass PCMachineClass;
typedef struct PCMachineState PCMachineState;
typedef struct PCMCIACardState PCMCIACardState;
typedef struct PixelFormat PixelFormat;
typedef struct PostcopyDiscardState PostcopyDiscardState;
typedef struct Property Property;
typedef struct PropertyInfo PropertyInfo;
typedef struct QBool QBool;
typedef struct QDict QDict;
typedef struct QEMUBH QEMUBH;
typedef struct QemuConsole QemuConsole;
typedef struct QemuDmaBuf QemuDmaBuf;
typedef struct QEMUFile QEMUFile;
typedef struct QemuLockable QemuLockable;
typedef struct QemuMutex QemuMutex;
typedef struct QemuOpt QemuOpt;
typedef struct QemuOpts QemuOpts;
typedef struct QemuOptsList QemuOptsList;
typedef struct QEMUSGList QEMUSGList;
typedef struct QemuSpin QemuSpin;
typedef struct QEMUTimer QEMUTimer;
typedef struct QEMUTimerListGroup QEMUTimerListGroup;
typedef struct QJSON QJSON;
typedef struct QList QList;
typedef struct QNull QNull;
typedef struct QNum QNum;
typedef struct QObject QObject;
typedef struct QString QString;
typedef struct RAMBlock RAMBlock;
typedef struct Range Range;
typedef struct SerialState SerialState;
typedef struct SHPCDevice SHPCDevice;
typedef struct SMBusDevice SMBusDevice;
typedef struct SSIBus SSIBus;
typedef struct uWireSlave uWireSlave;
typedef struct VirtIODevice VirtIODevice;
typedef struct Visitor Visitor;
typedef void SaveStateHandler(QEMUFile *f, void *opaque);
typedef int LoadStateHandler(QEMUFile *f, void *opaque, int version_id);

#endif /* QEMU_TYPEDEFS_H */
