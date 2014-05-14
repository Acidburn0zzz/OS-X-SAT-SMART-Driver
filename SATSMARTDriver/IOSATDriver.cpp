/*
 *  IOSATDriver.cpp
 *
 *  Created by Jarkko Sonninen on 3.2.2012.
 *  Copyright 2012 Jarkko Sonninen. All rights reserved.
 *
 */

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/scsi/SCSICmds_INQUIRY_Definitions.h>
#include <IOKit/scsi/SCSICommandOperationCodes.h>
#include <IOKit/scsi/SCSITask.h>
#include <IOKit/IOKitKeys.h>
#include </usr/include/AssertMacros.h>

#include <IOKit/storage/IOBlockStorageDriver.h>
#include <IOKit/scsi/IOSCSIProtocolServices.h>
#include <IOKit/scsi/IOSCSIPeripheralDeviceNub.h>

#include <IOKit/storage/ata/IOATAStorageDefines.h>
#include <IOKit/storage/IOStorageDeviceCharacteristics.h>

#include "IOSATBusCommand.h"
#include "IOSATDriver.h"
#include "IOSATServices.h"

extern const double SATSMARTDriverVersionNumber;

#ifdef DEBUG
#define DEBUG_LOG IOLog
#else
#define DEBUG_LOG(...)
#endif

#define ERROR_LOG IOLog

#define getClassName() "IOSATDriver"

#define Endian16_Swap(value) \
((((UInt16)((value) & 0x00FF)) << 8) | \
(((UInt16)((value) & 0xFF00)) >> 8))

#define kSCSICmd_PASS_THROUGH_16 0x85
#define kSCSICmd_PASS_THROUGH_12 0xA1

#define super IOSCSIPeripheralDeviceType00
OSDefineMetaClassAndStructors(org_dungeon_driver_IOSATDriver, IOSCSIPeripheralDeviceType00)

/////////////
static void hexdump16(UInt16 *data, int start, int len) {
    int i, j;
    for (i = start; i < start + len; i+=8) {
        DEBUG_LOG ( "%d:", i);
        for (j = 0; j < 8 && i + j < start + len; j++) {
            DEBUG_LOG ( " %04x", (int)data[i+j]);
        }
        DEBUG_LOG ( "\n");
    }
}

static void trimcpy(char * target, const char *source, int maxlen) {
    const char *p;
    char *q;
    int i;
    p = source;
    q = target;
    for (i = 0; *p == ' ' && i < maxlen-1; i++, p++) ;
    for (; i < maxlen-1; i++, p++, q++) *q = *p;
    *q = 0;
    i--; q--;
    for(; *q == ' ' && i >= 0; i--, q--) *q = 0;
}

void swapbytes(IOBufferMemoryDescriptor *        buffer) {
    int i;
    UInt16 *ptr = ( UInt16 * ) buffer->getBytesNoCopy ( );
    for (i = 0; i < buffer->getLength()/sizeof(UInt16); i++) {
        ptr[i]=Endian16_Swap(ptr[i]);
    }
}

int countzeros(IOBufferMemoryDescriptor *        buffer) {
    int i, count = 0;
    UInt8 *ptr = ( UInt8 * ) buffer->getBytesNoCopy ( );
    for (i = 0; i < buffer->getLength(); i++) {
        if (ptr[i] == 0) {
            count ++;
        }
    }
    return count;
}


UInt8 checksum(IOBufferMemoryDescriptor *        buffer) {
    int i;
    UInt8 sum = 0;
    UInt8 *ptr = ( UInt8 * ) buffer->getBytesNoCopy ( );
    for (i = 0; i < buffer->getLength(); i++) {
        sum += ptr[i];
    }
    return sum;
}

///////


bool org_dungeon_driver_IOSATDriver::init(OSDictionary *dict)
{
    DEBUG_LOG("%s[%p]::%s\n", getClassName(), this, __FUNCTION__);
    bool result = super::init(dict);
    memset(serial, 0, sizeof(serial));
    memset(model, 0, sizeof(model));
    memset(revision, 0, sizeof(revision));
    DEBUG_LOG("%s[%p]::%s result %d\n", getClassName(), this,  __FUNCTION__, result);
    return result;
}

void org_dungeon_driver_IOSATDriver::free(void)
{
    DEBUG_LOG("%s[%p]::%s\n", getClassName(), this, __FUNCTION__);
    super::free();
}

IOService *org_dungeon_driver_IOSATDriver::probe(IOService *provider,
                                                 SInt32 *score)
{
    DEBUG_LOG("%s[%p]::%s score %d\n", getClassName(), this, __FUNCTION__, score ? (int)*score : -1);
    IOService *result = super::probe(provider, score);
    //if (result != NULL) {
    // the following doesn not work
    //if (start(provider)) {
    //		Send_ATA_IDENTIFY();
    //		stop(provider);
    //	}
    //if (!fSATSMARTCapable) result = 0;
    //}

    OSNumber *idVendor = OSDynamicCast(OSNumber, getParentProperty("idVendor"));
    OSNumber *idProduct = OSDynamicCast(OSNumber, getParentProperty("idProduct"));
    OSString *vendorID = OSDynamicCast ( OSString, getParentProperty("Vendor Identification"));
    OSString *productID = OSDynamicCast ( OSString, getParentProperty("Product Identification"));

    OSDictionary*   dict= OSDynamicCast ( OSDictionary, getProperty("Identifiers"));
    
    if (dict) {
        OSDictionary* details = 0;
        OSString *key = 0;
            OSCollectionIterator *iterator = OSCollectionIterator::withCollection(dict);
            OSObject *object; 
            while (object = iterator->getNextObject()) { 
                key = OSDynamicCast (OSString, object);
                if (key) {
                    details= OSDynamicCast ( OSDictionary, dict->getObject(key));
                    if (details) {
                        OSNumber *idVendor2 = OSDynamicCast ( OSNumber, details->getObject("idVendor"));
                        OSNumber *idProduct2 = OSDynamicCast ( OSNumber, details->getObject("idProduct"));
                        OSString *vendorID2 = OSDynamicCast ( OSString, details->getObject("Vendor Identification"));
                        OSString *productID2 = OSDynamicCast ( OSString, details->getObject("Product Identification"));
                        if (idVendor && idVendor2 && idProduct && idProduct2 
                            && idVendor->unsigned32BitValue() == idVendor2->unsigned32BitValue()
                                && idProduct->unsigned32BitValue() == idProduct2->unsigned32BitValue()) {
                            DEBUG_LOG("%s[%p]::%s '%s' MATCH %04x:%04x\n", getClassName(), this, __FUNCTION__, key->getCStringNoCopy(), idVendor->unsigned32BitValue(), idProduct->unsigned32BitValue());
                            break;
                        }
                        if (vendorID && vendorID2 && productID && productID2 
                            && vendorID->isEqualTo(vendorID2) && productID->isEqualTo(productID2)) {
                            DEBUG_LOG("%s[%p]::%s '%s' MATCH '%s':'%s'\n", getClassName(), this, __FUNCTION__, key->getCStringNoCopy(), vendorID->getCStringNoCopy(), productID->getCStringNoCopy());
                            break;
                        }
                        details = 0;
                    }
                }
            } 
            iterator->release(); 
        if (details) {
            setProperty(kEnclosureName, key->getCStringNoCopy());
            OSString *options = OSDynamicCast ( OSString, details->getObject(kPassThroughMode));
            if (options) {
                DEBUG_LOG("%s[%p]::%s '%s' PassThroughMode %s\n", getClassName(), this, __FUNCTION__, key->getCStringNoCopy(), options->getCStringNoCopy());
                if (options->isEqualTo("DISABLE")) {
                    DEBUG_LOG("%s[%p]::%s DISABLED\n", getClassName(), this, __FUNCTION__);
                    *score = -5000;
                } else {
                    *score += 5000;
                }
                setProperty(kPassThroughMode, options->getCStringNoCopy());
            }
            OSBoolean *permissive = OSDynamicCast ( OSBoolean, details->getObject(kPermissiveKey));
            if (permissive) {
                setProperty(kPermissiveKey, permissive);
            }
        } else {
            char buffer[30];
            sprintf(buffer, "Unknown %04x:%04x", idVendor ? idVendor->unsigned32BitValue() : 0, idProduct ? idProduct->unsigned32BitValue() : 0);
            setProperty(kEnclosureName, buffer);
        }
    }
    
    DEBUG_LOG("%s[%p]::%s result %p score %d\n", getClassName(), this,  __FUNCTION__, result, score ? (int)*score : -1);
    return result;
}

OSObject *org_dungeon_driver_IOSATDriver::getParentProperty(const char *key) {
    IOService *parent = this;
    OSObject *value = NULL;
    while ((parent = parent->getProvider())) {
        value = parent->getProperty(key);
        if (value) {
            return value;
        }
    }
    return 0;
}

bool org_dungeon_driver_IOSATDriver::start(IOService *provider)
{
    DEBUG_LOG("%s[%p]::%s\n", getClassName(), this, __FUNCTION__);
    bool result = super::start(provider);
    OSString *name = OSDynamicCast(OSString, getProperty(kEnclosureName));
    require (result, ErrorExit);
    
    if (fSATSMARTCapable) {
        IOLog("SATSMARTDriver v%d.%d: enclosure '%s', disk serial '%s', revision '%s', model '%s'\n",
              (int)SATSMARTDriverVersionNumber, ((int)(10*SATSMARTDriverVersionNumber))%10, 
              name ? name->getCStringNoCopy() : "unknown",
              serial, revision, model);
    } else {
        IOLog("SATSMARTDriver v%d.%d: enclosure '%s', disk is not SAT capable\n",
              (int)SATSMARTDriverVersionNumber, ((int)(10*SATSMARTDriverVersionNumber))%10,
              name ? name->getCStringNoCopy() : "unknown");
        //result = false;
    }
    if (!result) {
        // Stop is not called on failure
        //TerminateDeviceSupport();
        //stop(provider);
    }
ErrorExit:
    DEBUG_LOG("%s[%p]::%s result %d\n", getClassName(), this,  __FUNCTION__, result);
    return result;
}

void org_dungeon_driver_IOSATDriver::stop(IOService *provider)
{
    DEBUG_LOG("%s[%p]::%s\n", getClassName(), this, __FUNCTION__);
    super::stop(provider);
}

// This function will be called when the user process calls IORegistryEntrySetCFProperties on
// this driver. You can add your custom functionality to this function.
IOReturn org_dungeon_driver_IOSATDriver::setProperties(OSObject* properties)
{
    OSDictionary*   dict;
    OSNumber*       number;
    IOReturn result = kIOReturnSuccess;
    DEBUG_LOG("%s[%p]::%s\n", getClassName(), this, __FUNCTION__);
    
    dict = OSDynamicCast(OSDictionary, properties);
    require_action(dict, ErrorExit, result = kIOReturnBadArgument);
    
    number = OSDynamicCast(OSNumber, dict->getObject(kMyPropertyKey));
    if (!number) {
        result = super::setProperties(properties);
    } else {
        UInt32 value= number->unsigned32BitValue();
        
        DEBUG_LOG("%s[%p]::%s(%p) got value %u\n", getName(), this, __FUNCTION__, properties, (unsigned int)value);
        
        // Some code to experiment the SAT commands
#if 1
        switch (value) {
            case 1:
                SendBuiltInINQUIRY ( );
                break;
            case 2: {
                int ret = Send_ATA_IDENTIFY();
                IOLog("identify %d\n", ret);
                if (ret) {
                IOLog("SATSMARTDriver v%d.%d: disk serial '%s', revision '%s', model '%s'\n",
                      (int)SATSMARTDriverVersionNumber, ((int)(10*SATSMARTDriverVersionNumber))%10, 
                      serial, revision, model);
                }
                break;
            }
            case 3:
                IOLog("smart read %d\n", Send_ATA_SMART_READ_DATA());
                break;
            case 4:
            {
                UInt8 status = -1;
                IOService *fProvider = getProvider();
                if (fProvider) {
                    OSNumber *lun = OSDynamicCast ( OSNumber, fProvider->getProperty("IOUnitLUN"));
                    if (lun) {
                        DEBUG_LOG("%s[%p]::%s LUN %d\n", getClassName(), this, __FUNCTION__, lun->unsigned32BitValue());
                        
                        //value->release();
                    }
                }
                if (JMicron_get_registers(0x720f, & status, sizeof status)) {
                    DEBUG_LOG("%s[%p]::%s register value %02x\n", getClassName(), this, __FUNCTION__, (int) status);
                }
            }
                
                //SendBuiltInQUERYLUNS ( );
                break;
            case 12: {
                //fPort = 0;
                //fDevice = 0;
                int oldPassThroughMode = fPassThroughMode;
                fPassThroughMode = kPassThroughModeJMicron; // FIXME race condition
                int ret = Send_ATA_IDENTIFY();
                fPassThroughMode = oldPassThroughMode;
                IOLog("JMicron identify %d port %d device %d\n", ret, fPort, fDevice);
                if (ret) {
                    IOLog("JMicron v%d.%d: disk serial '%s', revision '%s', model '%s'\n",
                          (int)SATSMARTDriverVersionNumber, ((int)(10*SATSMARTDriverVersionNumber))%10, 
                          serial, revision, model);
                }
                break;
            }
            case 13: {
                //fPort = 0;
                //fDevice = 0;
                int oldPassThroughMode = fPassThroughMode;
                fPassThroughMode = kPassThroughModeJMicron; // FIXME race condition
                IOLog("JMicron smart read %d\n", Send_ATA_SMART_READ_DATA());
                fPassThroughMode = oldPassThroughMode;
                break;
            }
            case 14: {
                fPort = fPort ^ 1;
                IOLog("JMicron port %d device %d\n", fPort, fDevice);
                break;
            }
            case 15: {
                fDevice = fDevice ^ 1;
                IOLog("JMicron port %d device %d\n", fPort, fDevice);
                break;
            }
                
            case 20:
		IOLog("SRST %d\n", Send_ATA_SEND_SOFT_RESET());
                break;
            case 21:
                IOLog("sat %d %p\n", fDeviceHasSATTranslation, GetProtocolDriver());
                break;
            case 22:
                IOLog("targetreset %d\n", GetProtocolDriver()->TargetReset());
                break;
            case 23:
		IOLog("standby %d\n", Send_ATA_STANDBY(0));
		break;
            case 24:
		IOLog("idle %d\n", Send_ATA_IDLE(0));
		break;
            case 25:
		IOLog("standby %d\n", Send_ATA_STANDBY(12)); // 12 * 5 = 60s
 		break;
            case 26: {
                int mode = -2;
                int ret = Send_ATA_CHECK_POWER_MODE(&mode);
		IOLog("power mode %d %d\n", ret, mode);
		break;
            }
            case 27:
		IOLog("idle %d\n", Send_ATA_IDLE_IMMEDIATE());
		break;
            case 28:
		IOLog("idle %d\n", Send_ATA_STANDBY_IMMEDIATE());
		break;
        }
#endif
        
        result = kIOReturnSuccess;
    }
    
ErrorExit:
    DEBUG_LOG("%s[%p]::%s result %d %s\n", getClassName(), this,  __FUNCTION__, result, stringFromReturn(result));
    return result;
}


bool
org_dungeon_driver_IOSATDriver::attach ( IOService * provider )
{
    DEBUG_LOG("%s[%p]::%s\n", getClassName(), this, __FUNCTION__);
    bool result = false;
    
    require_string ( super::attach ( provider ), ErrorExit,
                    "Superclass didn't attach" );
    
    result = true;
    
ErrorExit:
    DEBUG_LOG("%s[%p]::%s result %d\n", getClassName(), this,  __FUNCTION__, result);
    return result;
}

void
org_dungeon_driver_IOSATDriver::detach ( IOService * provider )
{
    DEBUG_LOG("%s[%p]::%s\n", getClassName(), this, __FUNCTION__);
    super::detach ( provider );
}


void
org_dungeon_driver_IOSATDriver::CreateStorageServiceNub ( void )
{
    DEBUG_LOG("%s[%p]::%s\n", getClassName(), this, __FUNCTION__);
    IOService *         nub = NULL;

    // Query device identification and check SAT capability
    IdentifyDevice();

    if (!fSATSMARTCapable) {
        super::CreateStorageServiceNub();
        return;
    }
    //nub = OSTypeAlloc ( IOBlockStorageServices );
    nub = OSTypeAlloc ( IOSATServices );
    require_quiet ( nub, ErrorExit );
    
    nub->init ( );
    require ( nub->attach ( this ), ReleaseNub );
    nub->registerService ( );
    nub->release ( );
    return;
    
ReleaseNub:
    nub->release ( );
    
ErrorExit:
    super::CreateStorageServiceNub();
    ERROR_LOG("%s::%s result failed\n", getClassName(), __FUNCTION__);
    return;
}

IOReturn org_dungeon_driver_IOSATDriver::sendSMARTCommand ( IOSATCommand * command )
{
    DEBUG_LOG("%s[%p]::%s\n", getClassName(), this, __FUNCTION__);
    
    SCSIServiceResponse serviceResponse = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
    SCSITaskIdentifier request            = NULL;
    int ataResult = kATAErrUnknownType;
    IOReturn err = kIOReturnInvalid;
    int direction, count, protocol;
    
    IOSATBusCommand* cmd = OSDynamicCast( IOSATBusCommand, command);
    require_action_string(cmd, ErrorExit, err = kIOReturnBadArgument, "Command is not a IOSATBusCommand");
    
    require (fSATSMARTCapable, ErrorExit );
    
    request = GetSCSITask ( );
    require (request, ErrorExit );
    
    DEBUG_LOG("buffer %p\n", cmd->getBuffer());
    DEBUG_LOG("bytecount %d\n", (int)cmd->getByteCount());
    DEBUG_LOG("features %x\n", cmd->getErrorReg());
    DEBUG_LOG("opcode %x\n", cmd->getOpcode());
    DEBUG_LOG("timeout %d\n", (int)cmd->getTimeoutMS());
    DEBUG_LOG("sector count %x\n", cmd->getSectorCount());
    DEBUG_LOG("sector num %x\n", cmd->getSectorNumber());
    DEBUG_LOG("cyllo %x\n", cmd->getCylLo());
    DEBUG_LOG("cylhi %x\n", cmd->getCylHi());
    DEBUG_LOG("device %x\n", cmd->getDevice_Head());
    DEBUG_LOG("command %x\n", cmd->getStatus());
    DEBUG_LOG("flags %x\n", cmd->getFlags());
    
    direction = (cmd->getFlags() & mATAFlagIORead) ? 1 : 0;
    count = 0;
    if (cmd->getBuffer() && cmd->getByteCount() == 512) {
        count = 1;
    }
    protocol = kIOSATProtocolNonData;
    if (count) {
        protocol = direction ? kIOSATProtocolPIODataIn : kIOSATProtocolPIODataOut;
    }
    DEBUG_LOG("%s[%p]::%s direction %d, count %d, protocol %d\n", getClassName(), this, __FUNCTION__, direction, count, protocol);
    // FIXME temporarily disable for SAT16, since it is broken
    if (fPassThroughMode == kPassThroughModeSAT16) goto ErrorExit;
    if ( PASS_THROUGH_12or16 ( request,
                              cmd->getBuffer(),
                              0,               //     MULTIPLE_COUNT,
                              protocol,               //     PROTOCOL,
                              0,               //     EXTEND,
                              0,               //     OFF_LINE,
                              0,               //     CK_COND,
                              direction,               //     T_DIR,
                              1,               //     BYT_BLOK,
                              count ? 0x02 : 0,               //     T_LENGTH,
                              cmd->getErrorReg(),               //	FEATURES
                              count,               //	SECTOR_COUNT,
                              cmd->getSectorNumber(),               //	LBA_LOW,
                              cmd->getCylLo(),               //	LBA_MID,
                              cmd->getCylHi(),               //	LBA_HIGH,
                              0,               //	DEVICE,
                              cmd->getStatus(),               //	COMMAND, smart
                              0x00)               // CONTROL
        == true)
    {
        serviceResponse = SendCommand ( request, cmd->getTimeoutMS() );
    }
    if ( ( serviceResponse == kSCSIServiceResponse_TASK_COMPLETE ) &&
        GetTaskStatus ( request ) == kSCSITaskStatus_GOOD )
    {
        DEBUG_LOG("%s[%p]::%s success, service response %d, task status %d\n",
                  getClassName(), this, __FUNCTION__, serviceResponse, GetTaskStatus ( request ));
        ataResult = kATANoErr;
        err=kIOReturnSuccess;
#if 0
        ERROR_LOG("REQUEST OK: [ %02x %02x %02x %02x %02x %02x %02x ]"
        ", flags 0x%x"
        ", opcode 0x%x"
        ", timeout %dms, direction %d, count %d, protocol %d bytecount %d\n"
                  , cmd->getErrorReg()
                  , cmd->getSectorCount()
		  , cmd->getSectorNumber()
		  , cmd->getCylLo()
                  , cmd->getCylHi()
		  , cmd->getDevice_Head()
		  , cmd->getStatus()
                  , cmd->getFlags()
		  , cmd->getOpcode()
		  , (int)cmd->getTimeoutMS()
		  , direction, count, protocol, (int)cmd->getByteCount());
#endif
    }
    else
    {
        ERROR_LOG("%s::%s failed, service response %d, task status %d\n",
                  getClassName(), __FUNCTION__, serviceResponse, GetTaskStatus ( request ));
        if (GetTaskStatus ( request ) == kSCSITaskStatus_TaskTimeoutOccurred) {
            ataResult = kATATimeoutErr;
            err=kIOReturnSuccess;
        } else {
            ataResult = kATAErrUnknownType;
            err=kIOReturnError;
        }
        ERROR_LOG("REQUEST: [ %02x %02x %02x %02x %02x %02x %02x ]"
        ", flags 0x%x"
        ", opcode 0x%x"
        ", timeout %d direction %d, count %d, protocol %d bytecount %d\n"
                  , cmd->getErrorReg()
                  , cmd->getSectorCount()
		  , cmd->getSectorNumber()
		  , cmd->getCylLo()
                  , cmd->getCylHi()
		  , cmd->getDevice_Head()
		  , cmd->getStatus()
                  , cmd->getFlags()
		  , cmd->getOpcode()
		  , (int)cmd->getTimeoutMS(), direction, count, protocol, (int)cmd->getByteCount());
        LogAutoSenseData (request) ;
    }
        
ReleaseTask:
    require_quiet ( ( request != NULL ), ErrorExit );
    ReleaseSCSITask ( request );
    request = NULL;
    
ErrorExit:
    
    if (err == kIOReturnSuccess) {
        cmd->setResult(ataResult);
        cmd->executeCallback();
    }
    
    DEBUG_LOG("%s[%p]::%s result %d %d %s\n", getClassName(), this,  __FUNCTION__, err, ataResult, stringFromReturn(err));
    return err;
}


bool
org_dungeon_driver_IOSATDriver::JMicron_get_registers ( UInt16 address, UInt8 *ptr, UInt16 length )
{
    SCSIServiceResponse serviceResponse = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
    IOMemoryDescriptor *        buffer            = NULL;
    SCSITaskIdentifier request            = NULL;
    SCSI_Sense_Data senseData;
    bool result = false;
    
    DEBUG_LOG("%s[%p]::%s\n", getClassName(), this, __FUNCTION__);
    
    buffer = IOMemoryDescriptor::withAddress(ptr, length, kIODirectionIn);
    require ( ( buffer != NULL ), ErrorExit );
    bzero ( ptr, buffer->getLength ( ) );
    request = GetSCSITask ( );
    require ( ( request != NULL ), ReleaseBuffer );
    require ( ( buffer->prepare ( ) == kIOReturnSuccess ), ReleaseTask );

    if ( PASS_THROUGH_JMicron( request,
                              buffer,
                              kIOSATProtocolPIODataIn,               //     PROTOCOL,
                              kIOSATTDirectionFromDevice,               //     T_DIR,
                              0,               //	FEATURES,
                              address >> 8,               //	SECTOR_COUNT,
                              address & 0xff,               //	LBA_LOW,
                              0,               //	LBA_MID,
                              0,               //	LBA_HIGH,
                              0,               //	DEVICE,
                              0xFD,               //	COMMAND
                              0x00)               // CONTROL
        == true)
    {
        serviceResponse = SendCommand ( request, kTenSecondTimeoutInMS );
    }
    if ( ( serviceResponse == kSCSIServiceResponse_TASK_COMPLETE ) &&
        (GetTaskStatus ( request ) == kSCSITaskStatus_GOOD) )
    {
        DEBUG_LOG("%s[%p]::%s register %04x value %02x\n", getClassName(), this,  __FUNCTION__, (int)address, (int)*ptr );
        result = true;
    }
    else
    {
        GetAutoSenseData( request, &senseData, sizeof(senseData) );
        if (((senseData.VALID_RESPONSE_CODE == 0x70) || (senseData.VALID_RESPONSE_CODE == 0x72)) && (senseData.SENSE_KEY == 5)) {
            // Illegal Request - The disk (or for example a flash memory) does not support the command
            DEBUG_LOG("%s::%s failed %d %d illegal request\n",
                      getClassName(), __FUNCTION__, serviceResponse,GetTaskStatus ( request ));
        } else if (senseData.VALID_RESPONSE_CODE == 0x70 && senseData.ADDITIONAL_SENSE_CODE == 0x3A) {
            DEBUG_LOG("%s::%s failed %d %d no media\n",
                      getClassName(), __FUNCTION__, serviceResponse,GetTaskStatus ( request ));
            *ptr = 0;
            result = true; // use this command to probe JMicron
        } else {
            ERROR_LOG("%s::%s failed %d %d\n",
                      getClassName(), __FUNCTION__, serviceResponse,GetTaskStatus ( request ));
            LogAutoSenseData(request);
        }
    }
    
    buffer->complete ( );
    
ReleaseTask:
    require_quiet ( ( request != NULL ), ReleaseBuffer );
    ReleaseSCSITask ( request );
    request = NULL;
    
ReleaseBuffer:
    require_quiet ( ( buffer != NULL ), ErrorExit );
    buffer->release ( );
    buffer = NULL;
    
ErrorExit:
    DEBUG_LOG("%s[%p]::%s result %d\n", getClassName(), this,  __FUNCTION__, (int)result);
    return result;
}



bool
org_dungeon_driver_IOSATDriver::IdentifyDevice ( void )
{
    bool result = false;
    DEBUG_LOG("%s[%p]::%s\n", getClassName(), this, __FUNCTION__);
    OSBoolean *permissive;
    OSString *value;
    
    permissive = OSDynamicCast ( OSBoolean, getProperty(kPermissiveKey));
    if  (permissive != NULL && permissive->isTrue()) {
        fPermissive = true;
    } else {
        fPermissive = false;
    }
    value = OSDynamicCast ( OSString, getProperty(kPassThroughMode));
    if (value) {
        if (!strcmp(value->getCStringNoCopy(), "sat12")) {
            fPassThroughMode = kPassThroughModeSAT12;
        } else if (value->isEqualTo("sat16")) {
            fPassThroughMode = kPassThroughModeSAT16;
        } else if (value->isEqualTo("jmicron")) {
            fPassThroughMode = kPassThroughModeJMicron;
        } else if (value->isEqualTo("auto")) {
            fPassThroughMode = kPassThroughModeAuto;
        } else if (value->isEqualTo("none")) {
            fPassThroughMode = kPassThroughModeNone;
        } else {
            fPassThroughMode = kPassThroughModeAuto;
        }
      } else {
          fPassThroughMode = kPassThroughModeAuto;
      }
    
    //SendBuiltInINQUIRY ( );
    boolean_t autodetect = (fPassThroughMode  == kPassThroughModeAuto);
    if (autodetect) {
            UInt8 status;
            // JMicron needs the port value. Unfortunately this is not reliable
            IOService *fProvider = getProvider();
            if (fProvider) {
                OSNumber *lun = OSDynamicCast ( OSNumber, fProvider->getProperty("IOUnitLUN"));
                if (lun) {
                    DEBUG_LOG("%s[%p]::%s LUN %d\n", getClassName(), this, __FUNCTION__, lun->unsigned32BitValue());
                    fPort = lun->unsigned32BitValue() & 1;
                }
            }
            // Probe for JMicron
            if (JMicron_get_registers(0x720f, & status, sizeof status)) {
                DEBUG_LOG("%s[%p]::%s register value %02x USE JMICRON!\n", getClassName(), this, __FUNCTION__, (int) status);
                fPassThroughMode = kPassThroughModeJMicron;
                setProperty(kPassThroughMode, "jmicron");
                if (status & 0x40) {
                    // This does not work for me. Status is always 4 in my enclosure
                    fPort = 1;
                }
            } else {
                ERROR_LOG("%s[%p]::%s JMicron probe failed, trying with PassThrough16\n", getClassName(), this,  __FUNCTION__);
                fPassThroughMode = kPassThroughModeSAT16;
                setProperty(kPassThroughMode, "sat16");
            }
    }
    if (!Send_ATA_IDENTIFY() && autodetect) {
	ERROR_LOG("%s[%p]::%s SAT PassThrough16 failed, retrying with PassThrough12\n", getClassName(), this,  __FUNCTION__);
	fSATSMARTCapable = true;
	fPassThroughMode = kPassThroughModeSAT12;
        setProperty(kPassThroughMode, "sat12");
	Send_ATA_IDENTIFY();
    }
    
    if (fSATSMARTCapable) {
        //setProperty (kIOPropertyVendorNameKey, GetVendorString() );
        setProperty (kProductModelKey, model);
        setProperty (kIOPropertyProductNameKey, model);
        setProperty (kIOPropertyProductRevisionLevelKey, revision);
        setProperty (kIOPropertyProductSerialNumberKey, serial);
        
	Send_ATA_SMART_READ_DATA();
    }
    
    if (fSATSMARTCapable) {
	unsigned long features = kIOATAFeatureSMART;
	OSNumber *number;
	number = OSNumber::withNumber(features, 32);
	require_string(number, ErrorExit, "OSNumber::withNumber");
	setProperty ( kIOATASupportedFeaturesKey, number );
	number->release();
        result = true;
    } else {
        //setProperty(kPassThroughMode, "none");
    }
    setProperty(kSATSMARTCapableKey, fSATSMARTCapable);
    
ErrorExit:
    DEBUG_LOG("%s[%p]::%s result %d\n", getClassName(), this,  __FUNCTION__, result);
    return result;
}

void org_dungeon_driver_IOSATDriver::LogAutoSenseData (SCSITaskIdentifier request) {
    SCSI_Sense_Data senseData;
    GetAutoSenseData( request, &senseData, sizeof(senseData) );
    ERROR_LOG( "senseData: VALID_RESPONSE_CODE=%d (7=valid),\n"
              ":          SEGMENT_NUMBER=%d,\n"
              ":          SENSE_KEY=%d (7 = FILEMARK, 6 = EOM, 5 = ILI, 3-0 = SENSE KEY)\n"
              ":          INFORMATION_1,_2,_3,_4=%d,%d,%d,%d,\n"
              ":          ADDITIONAL_SENSE_LENGTH=%d,\n"
              ":          COMMAND_SPECIFIC_INFORMATION_1,_2,_3,_4=%d,%d,%d,%d,\n"
              ":          ADDITIONAL_SENSE_CODE=%d,\n"
              ":          ADDITIONAL_SENSE_CODE_QUALIFIER=%d,\n"
              ":          FIELD_REPLACEABLE_UNIT_CODE=%d,\n"
              ":          SKSV_SENSE_KEY_SPECIFIC_MSB=%d (7 = Sense Key Specific Valid bit, 6-0 Sense Key Specific MSB),\n"
              ":          SENSE_KEY_SPECIFIC_MID=%d,\n"
              ":          SENSE_KEY_SPECIFIC_LSB=%d\n",
              senseData.VALID_RESPONSE_CODE, senseData.SEGMENT_NUMBER, senseData.SENSE_KEY,
              senseData.INFORMATION_1, senseData.INFORMATION_2,
              senseData.INFORMATION_3, senseData.INFORMATION_4, senseData.ADDITIONAL_SENSE_LENGTH,
              senseData.COMMAND_SPECIFIC_INFORMATION_1, senseData.COMMAND_SPECIFIC_INFORMATION_2,
              senseData.COMMAND_SPECIFIC_INFORMATION_3, senseData.COMMAND_SPECIFIC_INFORMATION_4,
              senseData.ADDITIONAL_SENSE_CODE, senseData.ADDITIONAL_SENSE_CODE_QUALIFIER,
              senseData.FIELD_REPLACEABLE_UNIT_CODE, senseData.SKSV_SENSE_KEY_SPECIFIC_MSB,
              senseData.SENSE_KEY_SPECIFIC_MID, senseData.SENSE_KEY_SPECIFIC_LSB
              );
}

bool
org_dungeon_driver_IOSATDriver::InitializeDeviceSupport ( void )
{
    bool result;
    DEBUG_LOG("%s[%p]::%s\n", getClassName(), this, __FUNCTION__);
    result = super::InitializeDeviceSupport ( );
    DEBUG_LOG("%s[%p]::%s result %d\n", getClassName(), this,  __FUNCTION__, result);
    return result;
}

void org_dungeon_driver_IOSATDriver::TerminateDeviceSupport ( void ) {
    DEBUG_LOG("%s[%p]::%s\n", getClassName(), this, __FUNCTION__);
    super::TerminateDeviceSupport();
}


char *
org_dungeon_driver_IOSATDriver::GetVendorString ( void ) {
    //serial[sizeof(serial)-1]=0;
    //if (*serial) return serial;
    return super::GetVendorString();
}

char *
org_dungeon_driver_IOSATDriver::GetProductString ( void ) {
    model[sizeof(model)-1]=0;
    if (*model) return model;
    return super::GetProductString();
}

char *
org_dungeon_driver_IOSATDriver::GetRevisionString ( void ) {
    revision[sizeof(revision)-1] =0;
    if (*revision) return revision;
    return super::GetRevisionString();
}

bool
org_dungeon_driver_IOSATDriver::Send_ATA_IDENTIFY ( void )
{
    SCSIServiceResponse serviceResponse = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
    IOBufferMemoryDescriptor *        buffer            = NULL;
    SCSITaskIdentifier request            = NULL;
    SCSI_Sense_Data senseData;
    UInt8 *                            ptr                = NULL;
    UInt16 * ataIdentify = NULL;
    bool result = false;
    
    DEBUG_LOG("%s[%p]::%s\n", getClassName(), this, __FUNCTION__);
    
    // Get a new IOBufferMemoryDescriptor object with a buffer large enough
    // to hold the SCSICmd_INQUIRY_StandardData structure (defined in
    // SCSICmds_INQUIRY_Definitions.h).
    buffer = IOBufferMemoryDescriptor::withCapacity ( 512, kIODirectionIn, false );
    
    // Return immediately if the buffer wasn't created.
    require ( ( buffer != NULL ), ErrorExit );
    
    // Get the address of the beginning of the buffer and zero-fill the buffer.
    ptr = ( UInt8 * ) buffer->getBytesNoCopy ( );
    bzero ( ptr, buffer->getLength ( ) );
    
    // Create a new SCSITask object; if unsuccessful, release the buffer and return.
    request = GetSCSITask ( );
    require ( ( request != NULL ), ReleaseBuffer );
    
    // Prepare the buffer for an I/O transaction. This call must be
    // balanced by a call to the complete method (shown just before
    // ReleaseTask).
    require ( ( buffer->prepare ( ) == kIOReturnSuccess ), ReleaseTask );
    
    // The BuildINQUIRY function shows how you can design and use a
    // command-building function to create a custom command to send
    // to your device. Although the BuildINQUIRY function builds a standard INQUIRY
    // command from the passed-in values, you do not create a custom function to
    // build a standard command in a real driver. Instead, you use the SCSI
    // Architecture Model family's built-in command-building functions. The
    // BuildINQUIRY function uses INQUIRY as an example merely because
    // it is a well-understood command.
    if ( PASS_THROUGH_12or16 ( request,
                              buffer,
                              0,               //     MULTIPLE_COUNT,
                              kIOSATProtocolPIODataIn,               //     PROTOCOL,
                              0,               //     EXTEND,
                              kIOSATOffline0s,               //     OFF_LINE,
                              0,               //     CK_COND,
                              kIOSATTDirectionFromDevice,               //     T_DIR,
                              kIOSATTLengthBlocks,               //     BYT_BLOK,
                              kIOSATTLengthInSectorCount,               //     T_LENGTH,
                              0,               //	FEATURES,
                              1,               //	SECTOR_COUNT,
                              0,               //	LBA_LOW,
                              0,               //	LBA_MID,
                              0,               //	LBA_HIGH,
                              0,               //	DEVICE,
                              0xEC,               //	COMMAND, identify
                              0x00)               // CONTROL
        == true)
    {
        serviceResponse = SendCommand ( request, kTenSecondTimeoutInMS );
    }
    if ( ( serviceResponse == kSCSIServiceResponse_TASK_COMPLETE ) &&
        (GetTaskStatus ( request ) == kSCSITaskStatus_GOOD) )
    {
        DEBUG_LOG("%s[%p]::%s checksum %d\n", getClassName(), this,  __FUNCTION__, checksum(buffer) );
        swapbytes(buffer);
        ataIdentify = ( UInt16 * ) buffer->getBytesNoCopy ( );
        
        trimcpy(serial, (char*)(ataIdentify+kATAIdentifySerialNumber), sizeof(serial));
        trimcpy(revision, (char*)(ataIdentify+kATAIdentifyFirmwareRevision), sizeof(revision));
        trimcpy(model, (char*)(ataIdentify+kATAIdentifyModelNumber), sizeof(model));
        
        if ((*revision == 0 && *model == 0) || (countzeros(buffer) == buffer->getLength ( ))) {
	    // Some disk enclosures return success, but data is no good
            ERROR_LOG("%s::%s zero response\n", getClassName(), __FUNCTION__);
            //hexdump16(ataIdentify, 0, buffer->getLength ( ));
	    fSATSMARTCapable = false;
	    result = false;
            
        } else {
	    capabilities = ataIdentify[kATAIdentifyDriveCapabilities]&0xffff;
	    fSATSMARTCapable = true;
	    result = true;
	}
    }
    else
    {
        GetAutoSenseData( request, &senseData, sizeof(senseData) );
        if (((senseData.VALID_RESPONSE_CODE == 0x70) || (senseData.VALID_RESPONSE_CODE == 0x72)) && (senseData.SENSE_KEY == 5)) {
            // Illegal Request - The disk (or for example a flash memory) does not support the command
            DEBUG_LOG("%s::%s failed %d %d illegal request\n",
                      getClassName(), __FUNCTION__, serviceResponse,GetTaskStatus ( request ));
        } else if (senseData.VALID_RESPONSE_CODE == 0x70 && senseData.ADDITIONAL_SENSE_CODE == 0x3A) {
            DEBUG_LOG("%s::%s failed %d %d no media\n",
                      getClassName(), __FUNCTION__, serviceResponse,GetTaskStatus ( request ));
        } else {
            ERROR_LOG("%s::%s failed %d %d\n",
                      getClassName(), __FUNCTION__, serviceResponse,GetTaskStatus ( request ));
            LogAutoSenseData(request);
        }
        fSATSMARTCapable = false;
    }
    
    buffer->complete ( );
    
ReleaseTask:
    require_quiet ( ( request != NULL ), ReleaseBuffer );
    ReleaseSCSITask ( request );
    request = NULL;
    
ReleaseBuffer:
    require_quiet ( ( buffer != NULL ), ErrorExit );
    buffer->release ( );
    buffer = NULL;
    
ErrorExit:
    DEBUG_LOG("%s[%p]::%s result %d\n", getClassName(), this,  __FUNCTION__, (int)result);
    return result;
}


bool
org_dungeon_driver_IOSATDriver::Send_ATA_SMART_READ_DATA ( void )
{
    SCSIServiceResponse serviceResponse = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
    IOBufferMemoryDescriptor *        buffer            = NULL;
    SCSITaskIdentifier request            = NULL;
    SCSI_Sense_Data senseData;
    UInt8 *                            ptr                = NULL;
    UInt16 * ptr16;
    bool result = false;
    
    DEBUG_LOG("%s[%p]::%s\n", getClassName(), this, __FUNCTION__);
    
    buffer = IOBufferMemoryDescriptor::withCapacity ( 512, kIODirectionIn, false );
    
    require ( ( buffer != NULL ), ErrorExit );
    ptr = ( UInt8 * ) buffer->getBytesNoCopy ( );
    bzero ( ptr, buffer->getLength ( ) );
    ptr16 = ( UInt16 * ) buffer->getBytesNoCopy ( );
    
    request = GetSCSITask ( );
    require ( ( request != NULL ), ReleaseBuffer );
    require ( ( buffer->prepare ( ) == kIOReturnSuccess ), ReleaseTask );
    
    if ( PASS_THROUGH_12or16 ( request,
                              buffer,
                              0,               //     MULTIPLE_COUNT,
                              kIOSATProtocolPIODataIn,               //     PROTOCOL,
                              0,               //     EXTEND,
                              kIOSATOffline0s,               //     OFF_LINE,
                              0,               //     CK_COND,
                              kIOSATTDirectionFromDevice,               //     T_DIR,
                              kIOSATTLengthBlocks,               //     BYT_BLOK,
                              kIOSATTLengthInSectorCount,               //     T_LENGTH,
                              0xd0,               //	FEATURES, read data
                              1,               //	SECTOR_COUNT,
                              0,               //	LBA_LOW,
                              0x4f,               //	LBA_MID,
                              0xc2,               //	LBA_HIGH,
                              0,               //	DEVICE,
                              0xB0,               //	COMMAND, smart
                              0x00)               // CONTROL
        == true)
    {
        serviceResponse = SendCommand ( request, kTenSecondTimeoutInMS );
    }
    // 20120521 RJVB: it appears we should accept a kSCSITaskStatus_CHECK_CONDITION status too:
    if ( ( serviceResponse == kSCSIServiceResponse_TASK_COMPLETE ) &&
        (GetTaskStatus ( request ) == kSCSITaskStatus_GOOD ||
         (fPermissive && (GetTaskStatus(request) == kSCSITaskStatus_CHECK_CONDITION))) )
    {
        DEBUG_LOG("%s[%p]::%s success checksum %d\n",
                  getClassName(), this,  __FUNCTION__, checksum(buffer));
        swapbytes(buffer);
        hexdump16(ptr16, 368, 10);
	result = true;
    }
    else
    {
        GetAutoSenseData( request, &senseData, sizeof(senseData) );
        if (((senseData.VALID_RESPONSE_CODE == 0x70) || (senseData.VALID_RESPONSE_CODE == 0x72)) && (senseData.SENSE_KEY == 5)) {
            // Illegal Request - The disk (or for example a flash memory) does not support the command
            DEBUG_LOG("%s::%s failed %d %d illegal request\n",
                      getClassName(), __FUNCTION__, serviceResponse,GetTaskStatus ( request ));
        } else if (senseData.VALID_RESPONSE_CODE == 0x70 && senseData.ADDITIONAL_SENSE_CODE == 0x3A) {
            DEBUG_LOG("%s::%s failed %d %d no media\n",
                      getClassName(), __FUNCTION__, serviceResponse,GetTaskStatus ( request ));
        } else {
            ERROR_LOG("%s::%s failed %d %d\n",
                      getClassName(), __FUNCTION__, serviceResponse,GetTaskStatus ( request ));
            LogAutoSenseData  (request);
        }
        fSATSMARTCapable = false;
    }
    
    buffer->complete ( );
    
ReleaseTask:
    require_quiet ( ( request != NULL ), ReleaseBuffer );
    ReleaseSCSITask ( request );
    request = NULL;
    
ReleaseBuffer:
    require_quiet ( ( buffer != NULL ), ErrorExit );
    buffer->release ( );
    buffer = NULL;
    
ErrorExit:
    DEBUG_LOG("%s[%p]::%s result %d\n", getClassName(), this,  __FUNCTION__, (int)result);
    return result;
}


bool
org_dungeon_driver_IOSATDriver::Send_ATA_IDLE(UInt8 value)
{
    SCSIServiceResponse serviceResponse = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
    SCSITaskIdentifier request            = NULL;
    bool result = false;
    
    DEBUG_LOG("%s[%p]::%s value %d\n", getClassName(), this, __FUNCTION__, (int) value);
    
    request = GetSCSITask ( );
    require ( ( request != NULL ), ReleaseBuffer );
    
    if ( PASS_THROUGH_12or16 ( request,
                              0,               // buffer
                              0,               //     MULTIPLE_COUNT,
                              kIOSATProtocolNonData,     //     PROTOCOL, soft reset
                              0,               //     EXTEND,
                              kIOSATOffline0s, //     OFF_LINE,
                              0,               //     CK_COND,
                              0,               //     T_DIR,
                              0,               //     BYT_BLOK,
                              kIOSATTLengthNoData,   //     T_LENGTH,
                              0x00,               //	FEATURES
                              value,               //	SECTOR_COUNT,
                              0,               //	LBA_LOW,
                              0x00,               //	LBA_MID,
                              0x00,               //	LBA_HIGH,
                              0,               //	DEVICE,
                              0xe3,               //	COMMAND, idle
                              0x00)               // CONTROL
        == true)
    {
        serviceResponse = SendCommand ( request, kTenSecondTimeoutInMS );
    }
    if ( ( serviceResponse == kSCSIServiceResponse_TASK_COMPLETE ) &&
        GetTaskStatus ( request ) == kSCSITaskStatus_GOOD )
    {
        DEBUG_LOG("%s[%p]::%s success %d %d\n",
                  getClassName(), this,  __FUNCTION__, serviceResponse,GetTaskStatus ( request ));
	result = true;
    }
    else
    {
        ERROR_LOG("%s::%s failed %d %d\n",
                  getClassName(), __FUNCTION__, serviceResponse,GetTaskStatus ( request ));
    }
    
ReleaseTask:
    require_quiet ( ( request != NULL ), ReleaseBuffer );
    ReleaseSCSITask ( request );
    request = NULL;
    
ReleaseBuffer:
    
ErrorExit:
    DEBUG_LOG("%s[%p]::%s result %d\n", getClassName(), this,  __FUNCTION__, (int)result);
    return result;
}

bool
org_dungeon_driver_IOSATDriver::Send_ATA_IDLE_IMMEDIATE()
{
    SCSIServiceResponse serviceResponse = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
    SCSITaskIdentifier request            = NULL;
    bool result = false;
    
    DEBUG_LOG("%s[%p]::%s\n", getClassName(), this, __FUNCTION__);
    
    request = GetSCSITask ( );
    require ( ( request != NULL ), ReleaseBuffer );
    
    if ( PASS_THROUGH_12or16 ( request,
                              0,               // buffer
                              0,               //     MULTIPLE_COUNT,
                              kIOSATProtocolNonData,     //     PROTOCOL
                              0,               //     EXTEND,
                              kIOSATOffline2s, //     OFF_LINE, 1 == 2s,  2 == 6s, 3 == 14s
                              0,               //     CK_COND,
                              0,               //     T_DIR,
                              0,               //     BYT_BLOK,
                              kIOSATTLengthNoData,   //     T_LENGTH,
                              0x00,               //	FEATURES
                              0x00,               //	SECTOR_COUNT,
                              0,               //	LBA_LOW,
                              0x00,               //	LBA_MID,
                              0x00,               //	LBA_HIGH,
                              0,               //	DEVICE,
                              0xe1,               //	COMMAND, idle immediate
                              0x00)               // CONTROL
        == true)
    {
        serviceResponse = SendCommand ( request, kTenSecondTimeoutInMS );
    }
    if ( ( serviceResponse == kSCSIServiceResponse_TASK_COMPLETE ) &&
        GetTaskStatus ( request ) == kSCSITaskStatus_GOOD )
    {
        DEBUG_LOG("%s[%p]::%s success %d %d\n",
                  getClassName(), this,  __FUNCTION__, serviceResponse,GetTaskStatus ( request ));
	result = true;
    }
    else
    {
        ERROR_LOG("%s::%s failed %d %d\n",
                  getClassName(), __FUNCTION__, serviceResponse,GetTaskStatus ( request ));
    }
    
ReleaseTask:
    require_quiet ( ( request != NULL ), ReleaseBuffer );
    ReleaseSCSITask ( request );
    request = NULL;
    
ReleaseBuffer:
    
ErrorExit:
    DEBUG_LOG("%s[%p]::%s result %d\n", getClassName(), this,  __FUNCTION__, (int)result);
    return result;
}

bool
org_dungeon_driver_IOSATDriver::Send_ATA_STANDBY(UInt8 value)
{
    SCSIServiceResponse serviceResponse = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
    SCSITaskIdentifier request            = NULL;
    bool result = false;
    
    DEBUG_LOG("%s[%p]::%s value %d\n", getClassName(), this, __FUNCTION__, (int) value);
    
    request = GetSCSITask ( );
    require ( ( request != NULL ), ReleaseBuffer );
    
    if ( PASS_THROUGH_12or16 ( request,
                              0,               // buffer
                              0,               //     MULTIPLE_COUNT,
                              kIOSATProtocolNonData,     //     PROTOCOL
                              0,               //     EXTEND,
                              kIOSATOffline2s, //     OFF_LINE, 1 == 2s,  2 == 6s, 3 == 14s
                              0,               //     CK_COND,
                              0,               //     T_DIR,
                              0,               //     BYT_BLOK,
                              kIOSATTLengthNoData,   //     T_LENGTH,
                              0x00,               //	FEATURES
                              value,               //	SECTOR_COUNT,
                              0,               //	LBA_LOW,
                              0x00,               //	LBA_MID,
                              0x00,               //	LBA_HIGH,
                              0,               //	DEVICE,
                              0xe2,               //	COMMAND, standby
                              0x00)               // CONTROL
        == true)
    {
        serviceResponse = SendCommand ( request, kTenSecondTimeoutInMS );
    }
    if ( ( serviceResponse == kSCSIServiceResponse_TASK_COMPLETE ) &&
        GetTaskStatus ( request ) == kSCSITaskStatus_GOOD )
    {
        DEBUG_LOG("%s[%p]::%s success %d %d\n",
                  getClassName(), this,  __FUNCTION__, serviceResponse,GetTaskStatus ( request ));
	result = true;
    }
    else
    {
        ERROR_LOG("%s::%s failed %d %d\n",
                  getClassName(), __FUNCTION__, serviceResponse,GetTaskStatus ( request ));
    }
    
ReleaseTask:
    require_quiet ( ( request != NULL ), ReleaseBuffer );
    ReleaseSCSITask ( request );
    request = NULL;
    
ReleaseBuffer:
    
ErrorExit:
    DEBUG_LOG("%s[%p]::%s result %d\n", getClassName(), this,  __FUNCTION__, (int)result);
    return result;
}

bool
org_dungeon_driver_IOSATDriver::Send_ATA_STANDBY_IMMEDIATE()
{
    SCSIServiceResponse serviceResponse = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
    SCSITaskIdentifier request            = NULL;
    bool result = false;
    
    DEBUG_LOG("%s[%p]::%s\n", getClassName(), this, __FUNCTION__);
    
    request = GetSCSITask ( );
    require ( ( request != NULL ), ReleaseBuffer );
    
    if ( PASS_THROUGH_12or16 ( request,
                              0,               // buffer
                              0,               //     MULTIPLE_COUNT,
                              kIOSATProtocolNonData,     //     PROTOCOL
                              0,               //     EXTEND,
                              kIOSATOffline0s, //     OFF_LINE, 1 == 2s,  2 == 6s, 3 == 14s
                              0,               //     CK_COND,
                              0,               //     T_DIR,
                              0,               //     BYT_BLOK,
                              kIOSATTLengthNoData,   //     T_LENGTH,
                              0x00,               //	FEATURES
                              0x00,               //	SECTOR_COUNT,
                              0,               //	LBA_LOW,
                              0x00,               //	LBA_MID,
                              0x00,               //	LBA_HIGH,
                              0,               //	DEVICE,
                              0xe0,               //	COMMAND, standby immediate
                              0x00)               // CONTROL
        == true)
    {
        serviceResponse = SendCommand ( request, kTenSecondTimeoutInMS );
    }
    if ( ( serviceResponse == kSCSIServiceResponse_TASK_COMPLETE ) &&
        GetTaskStatus ( request ) == kSCSITaskStatus_GOOD )
    {
        DEBUG_LOG("%s[%p]::%s success %d %d\n",
                  getClassName(), this,  __FUNCTION__, serviceResponse,GetTaskStatus ( request ));
	result = true;
    }
    else
    {
        ERROR_LOG("%s::%s failed %d %d\n",
                  getClassName(), __FUNCTION__, serviceResponse,GetTaskStatus ( request ));
    }
    
ReleaseTask:
    require_quiet ( ( request != NULL ), ReleaseBuffer );
    ReleaseSCSITask ( request );
    request = NULL;
    
ReleaseBuffer:
    
ErrorExit:
    DEBUG_LOG("%s[%p]::%s result %d\n", getClassName(), this,  __FUNCTION__, (int)result);
    return result;
}

bool
org_dungeon_driver_IOSATDriver::Send_ATA_CHECK_POWER_MODE(int *mode)
{
    SCSIServiceResponse serviceResponse = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
    SCSITaskIdentifier request            = NULL;
    bool result = false;
    
    DEBUG_LOG("%s[%p]::%s\n", getClassName(), this, __FUNCTION__);
    
    request = GetSCSITask ( );
    require ( ( request != NULL ), ReleaseBuffer );
    
    if ( PASS_THROUGH_12or16 ( request,
                              0,               // buffer
                              0,               //     MULTIPLE_COUNT,
                              kIOSATProtocolNonData,     //     PROTOCOL
                              0,               //     EXTEND,
                              kIOSATOffline0s, //     OFF_LINE, 1 == 2s,  2 == 6s, 3 == 14s
                              1,               //     CK_COND,
                              0,               //     T_DIR,
                              0,               //     BYT_BLOK,
                              kIOSATTLengthNoData,   //     T_LENGTH,
                              0x00,               //	FEATURES
                              0x00,               //	SECTOR_COUNT,
                              0,               //	LBA_LOW,
                              0x00,               //	LBA_MID,
                              0x00,               //	LBA_HIGH,
                              0,               //	DEVICE,
                              0xe5,               //	COMMAND, check power mode
                              0x00)               // CONTROL
        == true)
    {
        serviceResponse = SendCommand ( request, kTenSecondTimeoutInMS );
    }
    if ( ( serviceResponse == kSCSIServiceResponse_TASK_COMPLETE ) &&
        GetTaskStatus ( request ) == kSCSITaskStatus_GOOD )
    {
        DEBUG_LOG("%s[%p]::%s success %d %d \n",
                  getClassName(), this,  __FUNCTION__, serviceResponse,GetTaskStatus ( request ));
        result = true;
    }
    else
    {
        ERROR_LOG("%s::%s failed %d %d\n",
                  getClassName(), __FUNCTION__, serviceResponse,GetTaskStatus ( request ));
    }
    LogAutoSenseData(request);
    if (mode) {
        *mode = 42;
    }
    
ReleaseTask:
    require_quiet ( ( request != NULL ), ReleaseBuffer );
    ReleaseSCSITask ( request );
    request = NULL;
    
ReleaseBuffer:
    
ErrorExit:
    DEBUG_LOG("%s[%p]::%s result %d\n", getClassName(), this,  __FUNCTION__, (int)result);
    return result;
}

bool
org_dungeon_driver_IOSATDriver::Send_ATA_SEND_SOFT_RESET ( void )
{
    SCSIServiceResponse serviceResponse = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
    SCSITaskIdentifier request            = NULL;
    bool result = false;
    
    DEBUG_LOG("%s[%p]::%s\n", getClassName(), this, __FUNCTION__);
    
    request = GetSCSITask ( );
    require ( ( request != NULL ), ReleaseBuffer );
    
    if ( PASS_THROUGH_12or16 ( request,
                              0,               // buffer
                              0,               //     MULTIPLE_COUNT,
                              kIOSATProtocolSRST,               //     PROTOCOL, soft reset
                              0,               //     EXTEND,
                              kIOSATOffline6s,               //     OFF_LINE, 1 == 2s,  2 == 6s, 3 == 14s
                              0,               //     CK_COND,
                              0,               //     T_DIR,
                              0,               //     BYT_BLOK,
                              kIOSATTLengthNoData,               //     T_LENGTH,
                              0x00,               //	FEATURES
                              0,               //	SECTOR_COUNT,
                              0,               //	LBA_LOW,
                              0x00,               //	LBA_MID,
                              0x00,               //	LBA_HIGH,
                              0,               //	DEVICE,
                              0x00,               //	COMMAND
                              0x00)               // CONTROL
        == true)
    {
        serviceResponse = SendCommand ( request, kTenSecondTimeoutInMS );
    }
    if ( ( serviceResponse == kSCSIServiceResponse_TASK_COMPLETE ) &&
        GetTaskStatus ( request ) == kSCSITaskStatus_GOOD )
    {
        DEBUG_LOG("%s[%p]::%s success %d %d\n",
                  getClassName(), this,  __FUNCTION__, serviceResponse,GetTaskStatus ( request ));
	result = true;
    }
    else
    {
        ERROR_LOG("%s::%s failed %d %d\n",
                  getClassName(),  __FUNCTION__, serviceResponse,GetTaskStatus ( request ));
    }
    
ReleaseTask:
    require_quiet ( ( request != NULL ), ReleaseBuffer );
    ReleaseSCSITask ( request );
    request = NULL;
    
ReleaseBuffer:
    
ErrorExit:
    DEBUG_LOG("%s[%p]::%s result %d\n", getClassName(), this,  __FUNCTION__, (int)result);
    return result;
}

//////////////

bool
org_dungeon_driver_IOSATDriver::PASS_THROUGH_JMicron (
                                                 SCSITaskIdentifier request,
                                                 IOMemoryDescriptor *    dataBuffer,
                                                      SCSICmdField4Bit PROTOCOL,
                                                      SCSICmdField1Bit T_DIR,
                                                 SCSICmdField1Byte FEATURES,
                                                 SCSICmdField1Byte SECTOR_COUNT,
                                                 SCSICmdField1Byte LBA_LOW,
                                                 SCSICmdField1Byte LBA_MID,
                                                 SCSICmdField1Byte LBA_HIGH,
                                                 SCSICmdField1Byte DEVICE,
                                                 SCSICmdField1Byte COMMAND,
                                                 SCSICmdField1Byte CONTROL)
{
    bool result = false;
    int direction = kSCSIDataTransfer_NoDataTransfer;
    int transferCount = 0;
    int dxfer_len;
    dxfer_len = 1; // FIXME in SMART_READ
    
    DEBUG_LOG("%s[%p]::%s\n", getClassName(), this, __FUNCTION__);
    
    // Validate the parameters here.
    require ( ( request != NULL ), ErrorExit );
    require ( ResetForNewTask ( request ), ErrorExit );
    
    // The helper functions ensure that the parameters fit within the
    // CDB fields and that the buffer passed in is large enough for
    // the transfer length.
    require ( IsParameterValid ( PROTOCOL, kSCSICmdFieldMask4Bit ), ErrorExit );
    require ( IsParameterValid ( T_DIR, kSCSICmdFieldMask1Bit ), ErrorExit );
    require ( IsParameterValid ( FEATURES, kSCSICmdFieldMask1Byte ), ErrorExit );
    require ( IsParameterValid ( SECTOR_COUNT, kSCSICmdFieldMask1Byte ), ErrorExit );
    require ( IsParameterValid ( LBA_LOW, kSCSICmdFieldMask1Byte ), ErrorExit );
    require ( IsParameterValid ( LBA_MID, kSCSICmdFieldMask1Byte ), ErrorExit );
    require ( IsParameterValid ( LBA_HIGH, kSCSICmdFieldMask1Byte ), ErrorExit );
    require ( IsParameterValid ( DEVICE, kSCSICmdFieldMask1Byte ), ErrorExit );
    require ( IsParameterValid ( COMMAND, kSCSICmdFieldMask1Byte ), ErrorExit );
    require ( IsParameterValid ( CONTROL, kSCSICmdFieldMask1Byte ), ErrorExit );
    
    switch (PROTOCOL) {
        case kIOSATProtocolHardReset:     // Hard Reset
        case kIOSATProtocolSRST:     // SRST
        case kIOSATProtocolDEVICERESET:     // DEVICE RESET
        case kIOSATProtocolNonData:     // Non-data
            transferCount = 0;
            direction = kSCSIDataTransfer_NoDataTransfer;
            break;
        case kIOSATProtocolPIODataIn:     // PIO Data-In
        case kIOSATProtocolUDMADataIn:     // UDMA Data In
            require (T_DIR == kIOSATTDirectionFromDevice, ErrorExit);
            require (dataBuffer, ErrorExit);
            require ( IsMemoryDescriptorValid ( dataBuffer, dataBuffer->getLength() ), ErrorExit );
            transferCount = (int) dataBuffer->getLength();
            direction =  kSCSIDataTransfer_FromTargetToInitiator;
            break;
        case kIOSATProtocolPIODataOut:     // PIO Data-Out
        case kIOSATProtocolUDMADataOut:     // UDMA Data Out
            require (T_DIR == kIOSATTDirectionToDevice, ErrorExit);
            require (dataBuffer, ErrorExit);
            require ( IsMemoryDescriptorValid ( dataBuffer, dataBuffer->getLength() ), ErrorExit );
            transferCount = (int) dataBuffer->getLength();
            direction = kSCSIDataTransfer_FromInitiatorToTarget;
            break;
        case kIOSATProtocolDMA:     // DMA
        case kIOSATProtocolDMAQueued:     // DMA Queued
        case kIOSATProtocolDeviceDiagnostic:     // Device Diagnostic
        case kIOSATProtocolFPDMA:     //FPDMA
        default:
            if (!dataBuffer) {
                direction = kSCSIDataTransfer_NoDataTransfer;
                transferCount = 0;
            } else {
                require (dataBuffer, ErrorExit);
                require ( IsMemoryDescriptorValid ( dataBuffer, dataBuffer->getLength() ), ErrorExit );
                transferCount = (int) dataBuffer->getLength();
                if (T_DIR == kIOSATTDirectionFromDevice) {
                    direction = kSCSIDataTransfer_FromTargetToInitiator;
                } else {
                    direction = kSCSIDataTransfer_FromInitiatorToTarget;
                }
            }
    }
    dxfer_len = transferCount;
    
    SetCommandDescriptorBlock ( request,
                               0xdf,
                               (direction == kSCSIDataTransfer_FromInitiatorToTarget) ? 0 : 0x10,
                               0,
                               dxfer_len >> 8,
                               dxfer_len & 0xff,
                               (FEATURES & 0xff),
                               (SECTOR_COUNT & 0xff),
                               (LBA_LOW & 0xff),
                               (LBA_MID & 0xff),
                               (LBA_HIGH & 0xff),
                               fDevice | (fPort ? 0xa0 : 0xb0),
                               COMMAND);
    
    SetTimeoutDuration ( request, 0 );
    SetDataTransferDirection ( request, direction);
    SetRequestedDataTransferCount ( request, transferCount );
    if (transferCount > 0) {
        SetDataBuffer ( request, dataBuffer );
    }
    
    result = true;
    
ErrorExit:
    DEBUG_LOG("%s[%p]::%s result %d\n", getClassName(), this,  __FUNCTION__, result);
    return result;
}

bool
org_dungeon_driver_IOSATDriver::PASS_THROUGH_12 (
                                                 SCSITaskIdentifier request,
                                                 IOMemoryDescriptor *    dataBuffer,
                                                 SCSICmdField3Bit MULTIPLE_COUNT,
                                                 SCSICmdField4Bit PROTOCOL,
                                                 SCSICmdField1Bit EXTEND,
                                                 SCSICmdField2Bit OFF_LINE,
                                                 SCSICmdField1Bit CK_COND,
                                                 SCSICmdField1Bit T_DIR,
                                                 SCSICmdField1Bit BYT_BLOK,
                                                 SCSICmdField2Bit T_LENGTH,
                                                 SCSICmdField1Byte FEATURES,
                                                 SCSICmdField1Byte SECTOR_COUNT,
                                                 SCSICmdField1Byte LBA_LOW,
                                                 SCSICmdField1Byte LBA_MID,
                                                 SCSICmdField1Byte LBA_HIGH,
                                                 SCSICmdField1Byte DEVICE,
                                                 SCSICmdField1Byte COMMAND,
                                                 SCSICmdField1Byte CONTROL)
{
    bool result = false;
    int direction = kSCSIDataTransfer_NoDataTransfer;
    int transferCount = 0;
    DEBUG_LOG("%s[%p]::%s\n", getClassName(), this, __FUNCTION__);
    
    // Validate the parameters here.
    require ( ( request != NULL ), ErrorExit );
    require ( ResetForNewTask ( request ), ErrorExit );
    
    // The helper functions ensure that the parameters fit within the
    // CDB fields and that the buffer passed in is large enough for
    // the transfer length.
    require ( IsParameterValid ( MULTIPLE_COUNT, kSCSICmdFieldMask3Bit ), ErrorExit );
    require ( IsParameterValid ( PROTOCOL, kSCSICmdFieldMask4Bit ), ErrorExit );
    require ( IsParameterValid ( EXTEND, kSCSICmdFieldMask1Bit ), ErrorExit );
    require ( IsParameterValid ( OFF_LINE, kSCSICmdFieldMask2Bit ), ErrorExit );
    require ( IsParameterValid ( CK_COND, kSCSICmdFieldMask1Bit ), ErrorExit );
    require ( IsParameterValid ( T_DIR, kSCSICmdFieldMask1Bit ), ErrorExit );
    require ( IsParameterValid ( BYT_BLOK, kSCSICmdFieldMask1Bit ), ErrorExit );
    require ( IsParameterValid ( T_LENGTH, kSCSICmdFieldMask2Bit ), ErrorExit );
    require ( IsParameterValid ( FEATURES, kSCSICmdFieldMask1Byte ), ErrorExit );
    require ( IsParameterValid ( SECTOR_COUNT, kSCSICmdFieldMask1Byte ), ErrorExit );
    require ( IsParameterValid ( LBA_LOW, kSCSICmdFieldMask1Byte ), ErrorExit );
    require ( IsParameterValid ( LBA_MID, kSCSICmdFieldMask1Byte ), ErrorExit );
    require ( IsParameterValid ( LBA_HIGH, kSCSICmdFieldMask1Byte ), ErrorExit );
    require ( IsParameterValid ( DEVICE, kSCSICmdFieldMask1Byte ), ErrorExit );
    require ( IsParameterValid ( COMMAND, kSCSICmdFieldMask1Byte ), ErrorExit );
    require ( IsParameterValid ( CONTROL, kSCSICmdFieldMask1Byte ), ErrorExit );
    
    switch (PROTOCOL) {
        case kIOSATProtocolHardReset:     // Hard Reset
        case kIOSATProtocolSRST:     // SRST
        case kIOSATProtocolDEVICERESET:     // DEVICE RESET
        case kIOSATProtocolNonData:     // Non-data
            transferCount = 0;
            direction = kSCSIDataTransfer_NoDataTransfer;
            break;
        case kIOSATProtocolPIODataIn:     // PIO Data-In
        case kIOSATProtocolUDMADataIn:     // UDMA Data In
            require (T_DIR == kIOSATTDirectionFromDevice, ErrorExit);
            require (dataBuffer, ErrorExit);
            require ( IsMemoryDescriptorValid ( dataBuffer, dataBuffer->getLength() ), ErrorExit );
            transferCount = (int) dataBuffer->getLength();
            direction =  kSCSIDataTransfer_FromTargetToInitiator;
            break;
        case kIOSATProtocolPIODataOut:     // PIO Data-Out
        case kIOSATProtocolUDMADataOut:     // UDMA Data Out
            require (T_DIR == kIOSATTDirectionToDevice, ErrorExit);
            require (dataBuffer, ErrorExit);
            require ( IsMemoryDescriptorValid ( dataBuffer, dataBuffer->getLength() ), ErrorExit );
            transferCount = (int) dataBuffer->getLength();
            direction = kSCSIDataTransfer_FromInitiatorToTarget;
            break;
        case kIOSATProtocolDMA:     // DMA
        case kIOSATProtocolDMAQueued:     // DMA Queued
        case kIOSATProtocolDeviceDiagnostic:     // Device Diagnostic
        case kIOSATProtocolFPDMA:     //FPDMA
        default:
            if (!dataBuffer) {
                direction = kSCSIDataTransfer_NoDataTransfer;
                transferCount = 0;
            } else {
                require (dataBuffer, ErrorExit);
                require ( IsMemoryDescriptorValid ( dataBuffer, dataBuffer->getLength() ), ErrorExit );
                transferCount = (int) dataBuffer->getLength();
                if (T_DIR == kIOSATTDirectionFromDevice) {
                    direction = kSCSIDataTransfer_FromTargetToInitiator;
                } else {
                    direction = kSCSIDataTransfer_FromInitiatorToTarget;
                }
            }
    }

    // This is a 12-byte command: fill out the CDB appropriately
    SetCommandDescriptorBlock ( request,
                               kSCSICmd_PASS_THROUGH_12,
                               (MULTIPLE_COUNT << 5) | (PROTOCOL << 1) | EXTEND,
                               (OFF_LINE<<6)|(CK_COND<<5)|(T_DIR<<3)|(BYT_BLOK<<2) | T_LENGTH,
                               (FEATURES & 0xff),
                               (SECTOR_COUNT & 0xff),
                               (LBA_LOW & 0xff),
                               (LBA_MID & 0xff),
                               (LBA_HIGH & 0xff),
                               DEVICE,
                               COMMAND,
                               0,
                               CONTROL);
    
    SetTimeoutDuration ( request, 0 );
    SetDataTransferDirection ( request, direction);
    SetRequestedDataTransferCount ( request, transferCount );
    if (transferCount > 0) {
        SetDataBuffer ( request, dataBuffer );
    }
    
    result = true;
    
ErrorExit:
    DEBUG_LOG("%s[%p]::%s result %d\n", getClassName(), this,  __FUNCTION__, result);
    return result;
}

bool
org_dungeon_driver_IOSATDriver::PASS_THROUGH_16 (
                                                 SCSITaskIdentifier request,
                                                 IOMemoryDescriptor *    dataBuffer,
                                                 SCSICmdField3Bit MULTIPLE_COUNT,
                                                 SCSICmdField4Bit PROTOCOL,
                                                 SCSICmdField1Bit EXTEND,
                                                 SCSICmdField2Bit OFF_LINE,
                                                 SCSICmdField1Bit CK_COND,
                                                 SCSICmdField1Bit T_DIR,
                                                 SCSICmdField1Bit BYT_BLOK,
                                                 SCSICmdField2Bit T_LENGTH,
                                                 SCSICmdField2Byte FEATURES,
                                                 SCSICmdField2Byte SECTOR_COUNT,
                                                 SCSICmdField2Byte LBA_LOW,
                                                 SCSICmdField2Byte LBA_MID,
                                                 SCSICmdField2Byte LBA_HIGH,
                                                 SCSICmdField1Byte DEVICE,
                                                 SCSICmdField1Byte COMMAND,
                                                 SCSICmdField1Byte CONTROL)
{
    // TODO the EXTEND is always set to zero, should it be 1?
    EXTEND = 1;
    
    bool result = false;
    DEBUG_LOG("%s[%p]::%s\n", getClassName(), this, __FUNCTION__);
    
    // Validate the parameters here.
    require ( ( request != NULL ), ErrorExit );
    require ( ResetForNewTask ( request ), ErrorExit );
    
    // The helper functions ensure that the parameters fit within the
    // CDB fields and that the buffer passed in is large enough for
    // the transfer length.
    require ( IsParameterValid ( MULTIPLE_COUNT, kSCSICmdFieldMask3Bit ), ErrorExit );
    require ( IsParameterValid ( PROTOCOL, kSCSICmdFieldMask4Bit ), ErrorExit );
    require ( IsParameterValid ( EXTEND, kSCSICmdFieldMask1Bit ), ErrorExit );
    require ( IsParameterValid ( OFF_LINE, kSCSICmdFieldMask2Bit ), ErrorExit );
    require ( IsParameterValid ( CK_COND, kSCSICmdFieldMask1Bit ), ErrorExit );
    require ( IsParameterValid ( T_DIR, kSCSICmdFieldMask1Bit ), ErrorExit );
    require ( IsParameterValid ( BYT_BLOK, kSCSICmdFieldMask1Bit ), ErrorExit );
    require ( IsParameterValid ( T_LENGTH, kSCSICmdFieldMask2Bit ), ErrorExit );
    require ( IsParameterValid ( FEATURES, kSCSICmdFieldMask2Byte ), ErrorExit );
    require ( IsParameterValid ( SECTOR_COUNT, kSCSICmdFieldMask2Byte ), ErrorExit );
    require ( IsParameterValid ( LBA_LOW, kSCSICmdFieldMask2Byte ), ErrorExit );
    require ( IsParameterValid ( LBA_MID, kSCSICmdFieldMask2Byte ), ErrorExit );
    require ( IsParameterValid ( LBA_HIGH, kSCSICmdFieldMask2Byte ), ErrorExit );
    require ( IsParameterValid ( DEVICE, kSCSICmdFieldMask1Byte ), ErrorExit );
    require ( IsParameterValid ( COMMAND, kSCSICmdFieldMask1Byte ), ErrorExit );
    require ( IsParameterValid ( CONTROL, kSCSICmdFieldMask1Byte ), ErrorExit );
    require ( IsMemoryDescriptorValid ( dataBuffer, dataBuffer->getLength() ), ErrorExit );
    
    // This is a 16-byte command: fill out the CDB appropriately
    SetCommandDescriptorBlock ( request,
                               kSCSICmd_PASS_THROUGH_16,
                               (MULTIPLE_COUNT << 5) | (PROTOCOL << 1) | EXTEND,
                               (OFF_LINE<<6)|(CK_COND<<5)|(T_DIR<<3)|(BYT_BLOK<<2) | T_LENGTH,
                               (FEATURES>>8),
                               (FEATURES & 0xff),
                               (SECTOR_COUNT>>8),
                               (SECTOR_COUNT & 0xff),
                               (LBA_LOW>>8),
                               (LBA_LOW & 0xff),
                               (LBA_MID>>8),
                               (LBA_MID & 0xff),
                               (LBA_HIGH>>8),
                               (LBA_HIGH & 0xff),
                               DEVICE,
                               COMMAND,
                               CONTROL);
    
    SetDataTransferDirection ( request, kSCSIDataTransfer_FromTargetToInitiator );
    SetTimeoutDuration ( request, 0 );
    SetDataBuffer ( request, dataBuffer );
    SetRequestedDataTransferCount ( request, dataBuffer->getLength() );
    
    result = true;
    
ErrorExit:
    DEBUG_LOG("%s[%p]::%s result %d\n", getClassName(), this,  __FUNCTION__, result);
    return result;
}

bool
org_dungeon_driver_IOSATDriver::PASS_THROUGH_12or16 (
                                                     SCSITaskIdentifier request,
                                                     IOMemoryDescriptor *    dataBuffer,
                                                     SCSICmdField3Bit MULTIPLE_COUNT,
                                                     SCSICmdField4Bit PROTOCOL,
                                                     SCSICmdField1Bit EXTEND,
                                                     SCSICmdField2Bit OFF_LINE,
                                                     SCSICmdField1Bit CK_COND,
                                                     SCSICmdField1Bit T_DIR,
                                                     SCSICmdField1Bit BYT_BLOK,
                                                     SCSICmdField2Bit T_LENGTH,
                                                     SCSICmdField2Byte FEATURES,
                                                     SCSICmdField2Byte SECTOR_COUNT,
                                                     SCSICmdField2Byte LBA_LOW,
                                                     SCSICmdField2Byte LBA_MID,
                                                     SCSICmdField2Byte LBA_HIGH,
                                                     SCSICmdField1Byte DEVICE,
                                                     SCSICmdField1Byte COMMAND,
                                                     SCSICmdField1Byte CONTROL)
{
    bool result;
    
    if (fPassThroughMode == kPassThroughModeJMicron) {
        result = PASS_THROUGH_JMicron(request, dataBuffer,
                                 PROTOCOL, T_DIR, 
                                 FEATURES, SECTOR_COUNT,
                                 LBA_LOW, LBA_MID, LBA_HIGH,
                                 DEVICE, COMMAND, CONTROL );
        return result;
    }
    
    if (fPassThroughMode == kPassThroughModeSAT16) {
        result = PASS_THROUGH_16( request, dataBuffer,
                                 MULTIPLE_COUNT, PROTOCOL, EXTEND, OFF_LINE, CK_COND, T_DIR, BYT_BLOK, T_LENGTH,
                                 FEATURES, SECTOR_COUNT,
                                 LBA_LOW, LBA_MID, LBA_HIGH,
                                 DEVICE, COMMAND, CONTROL );
        return result;
    }
    
    result = PASS_THROUGH_12( request, dataBuffer,
                             MULTIPLE_COUNT, PROTOCOL, EXTEND, OFF_LINE, CK_COND, T_DIR, BYT_BLOK, T_LENGTH,
                             (SCSICmdField1Byte) FEATURES, (SCSICmdField1Byte) SECTOR_COUNT,
                             (SCSICmdField1Byte) LBA_LOW, (SCSICmdField1Byte) LBA_MID, (SCSICmdField1Byte) LBA_HIGH,
                             DEVICE, COMMAND, CONTROL );
    return result;
}

void
org_dungeon_driver_IOSATDriver::SendBuiltInINQUIRY ( void )
{
    // The Service Response represents the execution status of a service request.
    SCSIServiceResponse serviceResponse = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
    IOBufferMemoryDescriptor *         buffer  = NULL;
    SCSITaskIdentifier request = NULL;
    UInt8 *                            ptr     = NULL;
    SCSICmd_INQUIRY_StandardDataAll *   inquiryBuffer = NULL;
    
    DEBUG_LOG("%s[%p]::%s\n", getClassName(), this, __FUNCTION__);
    buffer = IOBufferMemoryDescriptor::withCapacity ( sizeof ( SCSICmd_INQUIRY_StandardDataAll ), kIODirectionIn, false );
    require ( ( buffer != NULL ), ErrorExit );
    
    ptr = ( UInt8 * ) buffer->getBytesNoCopy ( );
    bzero ( ptr, buffer->getLength ( ) );
    
    request = GetSCSITask ( );
    require ( ( request != NULL ), ReleaseBuffer );
    require ( ( buffer->prepare ( ) == kIOReturnSuccess ), ReleaseTask );
    
    if ( INQUIRY (  request,
                  buffer,
                  0,
                  0,
                  0x00,
                  sizeof ( SCSICmd_INQUIRY_StandardData ),
                  0 ) == true )
    {
        serviceResponse = SendCommand ( request, kTenSecondTimeoutInMS );
    }
    
    if ( ( serviceResponse == kSCSIServiceResponse_TASK_COMPLETE ) &&
        GetTaskStatus ( request ) == kSCSITaskStatus_GOOD )
    {
        inquiryBuffer = (SCSICmd_INQUIRY_StandardDataAll *)(buffer->getBytesNoCopy());
        DEBUG_LOG("%s[%p]::%s success %d '%s' '%s'\n", getClassName(), this,  __FUNCTION__, inquiryBuffer->ADDITIONAL_LENGTH, inquiryBuffer->VENDOR_IDENTIFICATION, inquiryBuffer->PRODUCT_IDENTIFICATION );
    }
    else
    {
        ERROR_LOG("%s::%s failed %d %d\n", getClassName(), __FUNCTION__, serviceResponse,GetTaskStatus ( request ) );
    }
    
    buffer->complete ( );
    
ReleaseTask:
    require_quiet ( ( request != NULL ), ReleaseBuffer );
    ReleaseSCSITask ( request );
    request = NULL;
    
ReleaseBuffer:
    require_quiet ( ( buffer != NULL ), ErrorExit );
    buffer->release ( );
    buffer = NULL;
    
ErrorExit:
    return;
}


// Padding for future binary compatibility.
OSMetaClassDefineReservedUnused(org_dungeon_driver_IOSATDriver, 0);
OSMetaClassDefineReservedUnused(org_dungeon_driver_IOSATDriver, 1);
OSMetaClassDefineReservedUnused(org_dungeon_driver_IOSATDriver, 2);
OSMetaClassDefineReservedUnused(org_dungeon_driver_IOSATDriver, 3);
OSMetaClassDefineReservedUnused(org_dungeon_driver_IOSATDriver, 4);
OSMetaClassDefineReservedUnused(org_dungeon_driver_IOSATDriver, 5);
OSMetaClassDefineReservedUnused(org_dungeon_driver_IOSATDriver, 6);
OSMetaClassDefineReservedUnused(org_dungeon_driver_IOSATDriver, 7);
OSMetaClassDefineReservedUnused(org_dungeon_driver_IOSATDriver, 8);
OSMetaClassDefineReservedUnused(org_dungeon_driver_IOSATDriver, 9);

