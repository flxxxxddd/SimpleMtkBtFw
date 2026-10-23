//
//  SimpleMtkBtPatcher.cpp
//  SimpleMtkBtFw
//
//  Created by laobamac on 2026/5/30.
//  Copyright © 2026 laobamac. All rights reserved.
//

//
//  SimpleMtkBtPatcher.cpp
//  SimpleMtkBtPatcher
//
//  Created by zxystd <zxystd@foxmail.com> on 2021/2/8.
//

#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <Headers/plugin_start.hpp>
#include <kern/thread_call.h>

#include "SimpleMtkBtPatcher.hpp"

static CSimpleMtkBtPatcher mtkBtPatcher;
static CSimpleMtkBtPatcher *callbackMtkBtPatcher = nullptr;

static const char *bootargOff[] {
    "-smbtcompatoff"
};

static const char *bootargDebug[] {
    "-smbtcompatdbg"
};

static const char *bootargBeta[] {
    "-smbtcompatbeta"
};

PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
    bootargOff,
    arrsize(bootargOff),
    bootargDebug,
    arrsize(bootargDebug),
    bootargBeta,
    arrsize(bootargBeta),
    KernelVersion::MountainLion,
    KernelVersion::Tahoe,
    []() {
        mtkBtPatcher.init();
    }
};

static const char *SimpleMtkBtPatcher_IOBluetoothFamily[] { "/System/Library/Extensions/IOBluetoothFamily.kext/Contents/MacOS/IOBluetoothFamily" };

static KernelPatcher::KextInfo SimpleMtkBtPatcher_IOBluetoothInfo {
    "com.apple.iokit.IOBluetoothFamily",
    SimpleMtkBtPatcher_IOBluetoothFamily,
    1,
    {true, true},
    {},
    KernelPatcher::KextInfo::Unloaded
};

static const char *SimpleMtkBtPatcher_IOUSBHostFamily[] {
    "/System/Library/Extensions/IOUSBHostFamily.kext/Contents/MacOS/IOUSBHostFamily"
};

static KernelPatcher::KextInfo SimpleMtkBtPatcher_IOUsbHostInfo {
    "com.apple.iokit.IOUSBHostFamily",
    SimpleMtkBtPatcher_IOUSBHostFamily,
    1,
    {true, true},
    {},
    KernelPatcher::KextInfo::Unloaded
};

bool CSimpleMtkBtPatcher::_randomAddressInit = false;
bool CSimpleMtkBtPatcher::_randomAddressHack = false;
bool CSimpleMtkBtPatcher::_lePhyFix = false;

/*
 * Tahoe BLE readiness experiment.
 *
 * K251 on MT7922 reaches LE connection complete and remote-features complete,
 * but Tahoe waits for a PHY Update Complete event before advancing.  We keep
 * the proven fake-PHY workaround, then ask the controller to perform a real
 * per-link DLE negotiation with HCI LE Set Data Length (0x2022).
 */
#define HCI_OP_LE_READ_REMOTE_FEATURES  0x2016
#define HCI_OP_LE_SET_DATA_LENGTH       0x2022

static int remoteReadSCount = 0;
static int leFeaturesEventStage = 0;

static IOUSBHostDevice *dleHostDevice = nullptr;
static IOService *dleProvider = nullptr;
static StandardUSB::DeviceRequest dleRequestTemplate {};
static unsigned int dleTimeout = 0;

static uint32_t lePhyFakeCount = 0;
static uint32_t leSetDataLengthSentCount = 0;
static int32_t leSetDataLengthLastIOReturn = 0;

/*
 * MT7922 (0489:E0D8) accepts LE Set Data Length (Command Complete status 0)
 * but does not emit LE Data Length Change on Tahoe. Send one private 0x2022
 * command and rewrite its Command Complete directly into the missing LE Meta
 * Data Length Change event. bluetoothd never issued the private command, so it
 * does not need to see that Command Complete.
 */
static int dleResponsesPending = 0;
static uint32_t leDataLengthFakeCount = 0;

static void scheduleDle(uint16_t handle);

static void clearDleTemplate()
{
    if (dleProvider != nullptr) {
        dleProvider->release();
        dleProvider = nullptr;
    }

    if (dleHostDevice != nullptr) {
        dleHostDevice->release();
        dleHostDevice = nullptr;
    }
}

static void saveDleTemplate(void *that,
                            IOService *provider,
                            const StandardUSB::DeviceRequest &request,
                            unsigned int timeout)
{
    clearDleTemplate();

    dleHostDevice = static_cast<IOUSBHostDevice *>(that);
    dleProvider = provider;
    dleRequestTemplate = request;
    dleTimeout = timeout;

    if (dleHostDevice != nullptr)
        dleHostDevice->retain();

    if (dleProvider != nullptr)
        dleProvider->retain();
}

bool CSimpleMtkBtPatcher::init()
{
    DBGLOG(DRV_NAME, "%s", __PRETTY_FUNCTION__);
    callbackMtkBtPatcher = this;
    if (getKernelVersion() < KernelVersion::Monterey) {
        lilu.onKextLoadForce(&SimpleMtkBtPatcher_IOBluetoothInfo, 1,
        [](void *user, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
            callbackMtkBtPatcher->processKext(patcher, index, address, size);
        }, this);
    } else {
        lilu.onKextLoadForce(&SimpleMtkBtPatcher_IOUsbHostInfo, 1,
        [](void *user, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
            callbackMtkBtPatcher->processKext(patcher, index, address, size);
        }, this);
    }
    return true;
}

void CSimpleMtkBtPatcher::free()
{
    DBGLOG(DRV_NAME, "%s", __PRETTY_FUNCTION__);
    clearDleTemplate();
}

void CSimpleMtkBtPatcher::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size)
{
    DBGLOG(DRV_NAME, "%s", __PRETTY_FUNCTION__);
    if (getKernelVersion() < KernelVersion::Monterey) {
        if (SimpleMtkBtPatcher_IOBluetoothInfo.loadIndex == index) {
            DBGLOG(DRV_NAME, "%s", SimpleMtkBtPatcher_IOBluetoothInfo.id);

            KernelPatcher::RouteRequest findQueueRequestRequest {
                "__ZN25IOBluetoothHostController17FindQueuedRequestEtP22BluetoothDeviceAddresstbPP21IOBluetoothHCIRequest",
                newFindQueueRequest,
                oldFindQueueRequest
            };
            patcher.routeMultiple(index, &findQueueRequestRequest, 1, address, size);
            if (patcher.getError() == KernelPatcher::Error::NoError) {
                DBGLOG(DRV_NAME, "routed %s", findQueueRequestRequest.symbol);
            } else {
                SYSLOG(DRV_NAME, "failed to resolve %s, error = %d", findQueueRequestRequest.symbol, patcher.getError());
                patcher.clearError();
            }

        }
    } else {
        if (SimpleMtkBtPatcher_IOUsbHostInfo.loadIndex == index) {
            SYSLOG(DRV_NAME, "%s", SimpleMtkBtPatcher_IOUsbHostInfo.id);

            KernelPatcher::RouteRequest pipeIoRequest {
                "__ZN13IOUSBHostPipe2ioEP18IOMemoryDescriptorjP19IOUSBHostCompletionj",
                newPipeIo,
                oldPipeIo
            };
            patcher.routeMultiple(index, &pipeIoRequest, 1, address, size);
            if (patcher.getError() == KernelPatcher::Error::NoError) {
                SYSLOG(DRV_NAME, "routed %s", pipeIoRequest.symbol);
            } else {
                SYSLOG(DRV_NAME, "failed to resolve %s, error = %d", pipeIoRequest.symbol, patcher.getError());
                patcher.clearError();
            }

            /*
             * deviceRequest carries HCI commands (EP0). It is hooked for the
             * Read Clock fix; the LE random address workaround (written for
             * Intel controllers) stays opt-in via -smbtrandaddr.
             */
            _randomAddressHack = checkKernelArgument("-smbtrandaddr");
            _lePhyFix = checkKernelArgument("-smbtlefix");

            KernelPatcher::RouteRequest hostDeviceRequest {
            "__ZN15IOUSBHostDevice13deviceRequestEP9IOServiceRN11StandardUSB13DeviceRequestEPvP18IOMemoryDescriptorRjP19IOUSBHostCompletionj",
                newHostDeviceRequest,
                oldHostDeviceRequest
            };
            patcher.routeMultiple(index, &hostDeviceRequest, 1, address, size);
            if (patcher.getError() == KernelPatcher::Error::NoError) {
                SYSLOG(DRV_NAME, "routed %s", hostDeviceRequest.symbol);
            } else {
                SYSLOG(DRV_NAME, "failed to resolve %s, error = %d", hostDeviceRequest.symbol, patcher.getError());
                patcher.clearError();
            }

            /*
             * Tahoe routes the interrupt-IN path through IOUSBHostIOSource::io
             * overloads.  Cover all three paths used by the successful Tahoe
             * BLE-HID workaround.
             */
            if (_lePhyFix && getKernelVersion() >= KernelVersion::Tahoe) {
                KernelPatcher::RouteRequest iosourceIOCompletionRequest {
                    "__ZN17IOUSBHostIOSource2ioEP18IOMemoryDescriptorjP19IOUSBHostCompletionj",
                    newIOSourceIOCompletion,
                    oldIOSourceIOCompletion
                };
                patcher.routeMultiple(index, &iosourceIOCompletionRequest, 1, address, size);
                if (patcher.getError() == KernelPatcher::Error::NoError) {
                    SYSLOG(DRV_NAME, "routed %s", iosourceIOCompletionRequest.symbol);
                } else {
                    SYSLOG(DRV_NAME, "failed to resolve %s, error = %d", iosourceIOCompletionRequest.symbol, patcher.getError());
                    patcher.clearError();
                }

                KernelPatcher::RouteRequest iosourceIORjjRequest {
                    "__ZN17IOUSBHostIOSource2ioEP18IOMemoryDescriptorjRjj",
                    newIOSourceIORjj,
                    oldIOSourceIORjj
                };
                patcher.routeMultiple(index, &iosourceIORjjRequest, 1, address, size);
                if (patcher.getError() == KernelPatcher::Error::NoError) {
                    SYSLOG(DRV_NAME, "routed %s", iosourceIORjjRequest.symbol);
                } else {
                    SYSLOG(DRV_NAME, "failed to resolve %s, error = %d", iosourceIORjjRequest.symbol, patcher.getError());
                    patcher.clearError();
                }

                KernelPatcher::RouteRequest iosourceIOBundledRequest {
                    "__ZN17IOUSBHostIOSource2ioEP18IOMemoryDescriptorjP26IOUSBHostBundledCompletionj",
                    newIOSourceIOBundled,
                    oldIOSourceIOBundled
                };
                patcher.routeMultiple(index, &iosourceIOBundledRequest, 1, address, size);
                if (patcher.getError() == KernelPatcher::Error::NoError) {
                    SYSLOG(DRV_NAME, "routed %s", iosourceIOBundledRequest.symbol);
                } else {
                    SYSLOG(DRV_NAME, "failed to resolve %s, error = %d", iosourceIOBundledRequest.symbol, patcher.getError());
                    patcher.clearError();
                }
            }
        }
    }
}

#pragma mark - For Bigsur-, patch unhandled 0x2019 opcode

#define HCI_OP_LE_START_ENCRYPTION 0x2019

IOReturn CSimpleMtkBtPatcher::newFindQueueRequest(void *that, unsigned short arg1, void *addr, unsigned short arg2, bool arg3, void **hciRequestPtr)
{
    IOReturn ret = FunctionCast(newFindQueueRequest, callbackMtkBtPatcher->oldFindQueueRequest)(that, arg1, addr, arg2, arg3, hciRequestPtr);
    if (ret != 0 && arg1 == HCI_OP_LE_START_ENCRYPTION) {
        ret = FunctionCast(newFindQueueRequest, callbackMtkBtPatcher->oldFindQueueRequest)(that, arg1, addr, 0xffff, arg3, hciRequestPtr);
        DBGLOG(DRV_NAME, "%s ret: %d arg1: 0x%04x arg2: 0x%04x arg3: %d ptr: %p", __FUNCTION__, ret, arg1, arg2, arg3, *hciRequestPtr);
    }
    return ret;
}

#pragma mark - For Monterey+ patch for intercept HCI REQ and RESP

StandardUSB::DeviceRequest randomAddressRequest;
// Hardcoded Random Address HCI Command
const uint8_t randomAddressHci[9] = {0x05, 0x20, 0x06, 0x94, 0x50, 0x64, 0xD0, 0x78, 0x6B};
IOBufferMemoryDescriptor *writeHCIDescriptor = nullptr;

#define MAX_HCI_BUF_LEN             255
#define HCI_OP_RESET                0x0c03
#define HCI_OP_LE_SET_SCAN_PARAM    0x200B
#define HCI_OP_LE_SET_SCAN_ENABLE   0x200C

IOReturn CSimpleMtkBtPatcher::newHostDeviceRequest(void *that, IOService *provider, StandardUSB::DeviceRequest &request, void *data, IOMemoryDescriptor *descriptor, unsigned int &length, IOUSBHostCompletion *completion, unsigned int timeout)
{
    HciCommandHdr *hdr = nullptr;
    uint32_t hdrLen = 0;
    char hciBuf[MAX_HCI_BUF_LEN] = {0};

    fixReadClock(request, data, descriptor);

    if (data == nullptr) {
        if (descriptor != nullptr &&
            (getKernelVersion() < KernelVersion::Sequoia || !descriptor->prepare(kIODirectionOut))) {
            if (descriptor->getLength() > 0) {
                descriptor->readBytes(0, hciBuf, min(descriptor->getLength(), MAX_HCI_BUF_LEN));
                hdrLen = (uint32_t)min(descriptor->getLength(), MAX_HCI_BUF_LEN);
            }
            if (getKernelVersion() >= KernelVersion::Sequoia)
                descriptor->complete(kIODirectionOut);
        }
        hdr = (HciCommandHdr *)hciBuf;

        if (_randomAddressHack && hdr->opcode == HCI_OP_LE_SET_SCAN_PARAM) {
            if (!_randomAddressInit) {
                randomAddressRequest.bmRequestType = makeDeviceRequestbmRequestType(kRequestDirectionOut, kRequestTypeClass, kRequestRecipientInterface);
                randomAddressRequest.bRequest = 0xE0;
                randomAddressRequest.wIndex = 0;
                randomAddressRequest.wValue = 0;
                randomAddressRequest.wLength = 9;
                length = 9;
                if (writeHCIDescriptor == nullptr)
                    writeHCIDescriptor = IOBufferMemoryDescriptor::withBytes(randomAddressHci, 9, kIODirectionOut);
                writeHCIDescriptor->prepare(kIODirectionOut);
                IOReturn ret = FunctionCast(newHostDeviceRequest, callbackMtkBtPatcher->oldHostDeviceRequest)(that, provider, randomAddressRequest, nullptr, writeHCIDescriptor, length, nullptr, timeout);
                writeHCIDescriptor->complete();
                const char *randAddressDump = _hexDumpHCIData((uint8_t *)randomAddressHci, 9);
                if (randAddressDump) {
                    SYSLOG(DRV_NAME, "[PATCH] Sending Random Address HCI %d %s", ret, randAddressDump);
                    IOFree((void *)randAddressDump, 9 * 3 + 1);
                }
                _randomAddressInit = true;
                SYSLOG(DRV_NAME, "[PATCH] Resend LE SCAN PARAM HCI %d", ret);
            }
        } else if (_lePhyFix && hdr->opcode == HCI_OP_LE_READ_REMOTE_FEATURES) {
            /*
             * The normal call at the bottom sends one Read Remote Features.
             * Send ONE extra copy.  The real response passes through; the
             * duplicate response is rewritten as PHY Update Complete.
             */
            remoteReadSCount += 1;
            leFeaturesEventStage = 0;
            saveDleTemplate(that, provider, request, timeout);

            FunctionCast(newHostDeviceRequest, callbackMtkBtPatcher->oldHostDeviceRequest)(
                that,
                provider,
                request,
                nullptr,
                descriptor,
                length,
                nullptr,
                timeout
            );
        }
    } else {
        hdr = (HciCommandHdr *)data;
        hdrLen = request.wLength - 3;
    }

    if (hdr) {
        // HCI reset: reset transient LE workaround state too.
        if (hdr->opcode == HCI_OP_RESET) {
            _randomAddressInit = false;
            remoteReadSCount = 0;
            leFeaturesEventStage = 0;
            dleResponsesPending = 0;
            clearDleTemplate();
        }

#if DEBUG
        DBGLOG(DRV_NAME, "[%s] bRequest: 0x%x direction: %s type: %s recipient: %s wValue: 0x%02x wIndex: 0x%02x opcode: 0x%04x len: %d length: %d async: %d", provider->getName(), request.bRequest, requestDirectionNames[(request.bmRequestType & kDeviceRequestDirectionMask) >> kDeviceRequestDirectionPhase], requestRecipientNames[(request.bmRequestType & kDeviceRequestRecipientMask) >> kDeviceRequestRecipientPhase], requestTypeNames[(request.bmRequestType & kDeviceRequestTypeMask) >> kDeviceRequestTypePhase], request.wValue, request.wIndex, hdr->opcode, hdr->len, request.wLength, completion != nullptr);
        if (hdrLen) {
            const char *dump = _hexDumpHCIData((uint8_t *)hdr, hdrLen);
            if (dump) {
                DBGLOG(DRV_NAME, "[Request]: %s", dump);
                IOFree((void *)dump, hdrLen * 3 + 1);
            }
        }
#endif
    }

    return FunctionCast(newHostDeviceRequest, callbackMtkBtPatcher->oldHostDeviceRequest)(that, provider, request, data, descriptor, length, completion, timeout);
}

#pragma mark - Tahoe LE PHY readiness + real DLE request

#define HCI_EVT_LE_META                                0x3E
#define HCI_EVT_LE_META_READ_REMOTE_FEATURES_COMPLETE  0x04

/*
 * LE PHY Update Complete:
 * 3E 06 0C 00 <handle_lo> <handle_hi> 02 02
 */
static uint8_t fakePhyUpdateCompleteEvent[8] = {
    0x3E, 0x06, 0x0C, 0x00,
    0x00, 0x00,
    0x02, 0x02
};

/*
 * LE Data Length Change:
 * 3E 0B 07 <handle_lo> <handle_hi>
 * 1B 00 48 01 1B 00 48 01
 *
 * Report the conservative 27-octet / 328-us values Tahoe already considers
 * valid for this link.  The purpose here is to advance bluetoothd's readiness
 * state, not to claim a larger negotiated payload than the controller reports.
 */
static uint8_t fakeDataLengthChangeEvent[13] = {
    0x3E, 0x0B, 0x07,
    0x00, 0x00,
    0x1B, 0x00,
    0x48, 0x01,
    0x1B, 0x00,
    0x48, 0x01
};

static void publishLePhyCount()
{
    IOService *res = IOService::getResourceService();
    if (!res)
        return;

    OSNumber *n = OSNumber::withNumber(lePhyFakeCount, 32);
    if (n) {
        res->setProperty("SimpleMtkBtLEPhyFakeCount", n);
        n->release();
    }
}

static void publishDleCounters()
{
    IOService *res = IOService::getResourceService();
    if (!res)
        return;

    OSNumber *count = OSNumber::withNumber(leSetDataLengthSentCount, 32);
    OSNumber *last = OSNumber::withNumber(static_cast<uint32_t>(leSetDataLengthLastIOReturn), 32);
    OSNumber *fake = OSNumber::withNumber(leDataLengthFakeCount, 32);

    if (count) {
        res->setProperty("SimpleMtkBtLESetDataLengthSentCount", count);
        count->release();
    }

    if (last) {
        res->setProperty("SimpleMtkBtLESetDataLengthLastIOReturn", last);
        last->release();
    }

    if (fake) {
        res->setProperty("SimpleMtkBtLEDataLengthFakeCount", fake);
        fake->release();
    }
}

struct DleWorkContext {
    IOUSBHostDevice *device;
    IOService *provider;
    StandardUSB::DeviceRequest request;
    uint16_t handle;
    unsigned int timeout;
};

using HostDeviceRequestFn = IOReturn (*)(
    void *,
    IOService *,
    StandardUSB::DeviceRequest &,
    void *,
    IOMemoryDescriptor *,
    unsigned int &,
    IOUSBHostCompletion *,
    unsigned int
);

static void sendDleWorker(thread_call_param_t param0, thread_call_param_t param1)
{
    DleWorkContext *ctx = static_cast<DleWorkContext *>(param0);
    thread_call_t call = static_cast<thread_call_t>(param1);

    /*
     * Run off the USB completion path and give bluetoothd a tiny window to
     * consume the synthetic PHY event before the DLE sequence starts.
     */
    IOSleep(2);

    /*
     * HCI LE Set Data Length (0x2022), little-endian:
     *   handle
     *   TX_Octets = 251 (0x00FB)
     *   TX_Time   = 2120 us (0x0848)
     *
     * This is a private command injected below bluetoothd. Send it ONCE.
     * Its successful Command Complete is later rewritten into the missing
     * LE Data Length Change event.
     */
    uint8_t dleHci[9] = {
        0x22, 0x20, 0x06,
        static_cast<uint8_t>(ctx->handle & 0xFF),
        static_cast<uint8_t>((ctx->handle >> 8) & 0xFF),
        0xFB, 0x00,
        0x48, 0x08
    };

    StandardUSB::DeviceRequest request = ctx->request;
    request.wLength = sizeof(dleHci);

    IOReturn lastRet = kIOReturnNoMemory;
    uint32_t successfulSends = 0;

    IOBufferMemoryDescriptor *descriptor =
        IOBufferMemoryDescriptor::withBytes(dleHci, sizeof(dleHci), kIODirectionOut);

    if (descriptor != nullptr) {
        if (descriptor->prepare(kIODirectionOut) == kIOReturnSuccess) {
            auto oldHostDeviceRequest =
                reinterpret_cast<HostDeviceRequestFn>(callbackMtkBtPatcher->oldHostDeviceRequest);

            /*
             * Arm the inbound IOSource hooks BEFORE the private command goes out.
             * Exactly one Command Complete for opcode 0x2022 is expected.
             */
            dleResponsesPending = 1;

            unsigned int length = sizeof(dleHci);

            lastRet = oldHostDeviceRequest(
                ctx->device,
                ctx->provider,
                request,
                nullptr,
                descriptor,
                length,
                nullptr,
                ctx->timeout
            );

            if (lastRet == kIOReturnSuccess)
                ++successfulSends;

            descriptor->complete(kIODirectionOut);
        }

        descriptor->release();
    }

    leSetDataLengthLastIOReturn = static_cast<int32_t>(lastRet);
    leSetDataLengthSentCount += successfulSends;

    /*
     * If neither transfer was submitted successfully, do not leave the
     * interrupt path armed waiting for replies that can never arrive.
     */
    if (successfulSends == 0) {
        dleResponsesPending = 0;
    }

    publishDleCounters();

    ctx->provider->release();
    ctx->device->release();
    delete ctx;

    if (call != nullptr)
        thread_call_free(call);
}

static void scheduleDle(uint16_t handle)
{
    if (dleHostDevice == nullptr || dleProvider == nullptr)
        return;

    DleWorkContext *ctx = new DleWorkContext {};
    if (ctx == nullptr)
        return;

    ctx->device = dleHostDevice;
    ctx->provider = dleProvider;
    ctx->request = dleRequestTemplate;
    ctx->handle = handle;
    ctx->timeout = dleTimeout;

    ctx->device->retain();
    ctx->provider->retain();

    thread_call_t call = thread_call_allocate(sendDleWorker, ctx);
    if (call == nullptr) {
        ctx->provider->release();
        ctx->device->release();
        delete ctx;
        return;
    }

    thread_call_enter1(call, call);
}

static void rewriteLePhyEvent(IOMemoryDescriptor *dataBuffer, uint32_t &bytesTransferred)
{
    if (dataBuffer == nullptr ||
        bytesTransferred < 2 ||
        (remoteReadSCount <= 0 && dleResponsesPending <= 0))
        return;

    const uint32_t originalSize = bytesTransferred;
    void *buffer = IOMalloc(originalSize);
    if (buffer == nullptr)
        return;

    if (dataBuffer->getLength() > 0 &&
        (getKernelVersion() < KernelVersion::Sequoia ||
         !dataBuffer->prepare(kIODirectionInOut))) {

        const size_t bytesRead = dataBuffer->readBytes(0, buffer, originalSize);

        if (bytesRead >= 2) {
            HciEventHdr *hdr = static_cast<HciEventHdr *>(buffer);

            /*
             * Phase 1: Remote Features -> synthetic PHY Update Complete.
             */
            if (remoteReadSCount > 0 &&
                bytesRead >= 14 &&
                hdr->evt == HCI_EVT_LE_META &&
                hdr->len >= 0x0C &&
                hdr->data[0] == HCI_EVT_LE_META_READ_REMOTE_FEATURES_COMPLETE) {

                if (leFeaturesEventStage == 0) {
                    /*
                     * #1 is the real Remote Features Complete.
                     * Leave it untouched.
                     */
                    leFeaturesEventStage = 1;
                } else {
                    /*
                     * #2 is our duplicate. Convert it to a successful
                     * LE PHY Update Complete and then queue the real
                     * LE Set Data Length commands for the same handle.
                     */
                    const uint16_t handle =
                        static_cast<uint16_t>(hdr->data[2] |
                                              (static_cast<uint16_t>(hdr->data[3]) << 8));

                    fakePhyUpdateCompleteEvent[4] = hdr->data[2];
                    fakePhyUpdateCompleteEvent[5] = hdr->data[3];

                    const size_t written =
                        dataBuffer->writeBytes(
                            0,
                            fakePhyUpdateCompleteEvent,
                            sizeof(fakePhyUpdateCompleteEvent)
                        );

                    if (written == sizeof(fakePhyUpdateCompleteEvent)) {
                        bytesTransferred = sizeof(fakePhyUpdateCompleteEvent);

                        ++lePhyFakeCount;
                        publishLePhyCount();

                        --remoteReadSCount;
                        leFeaturesEventStage = 0;

                        scheduleDle(handle);
                    }
                }
            }

            /*
             * Phase 2: MT7922 returns Command Complete for LE Set Data Length
             * (0x2022) with status 0, but never follows with LE Data Length
             * Change. Rewrite this private command's completion directly into
             * the missing LE Meta event.
             *
             * Command Complete bytes:
             *   0E 06 <num> 22 20 <status> <handle_lo> <handle_hi>
             *
             * With HciEventHdr::data:
             *   data[0] = num packets
             *   data[1] = opcode low
             *   data[2] = opcode high
             *   data[3] = status
             *   data[4] = handle low
             *   data[5] = handle high
             */
            else if (dleResponsesPending > 0 &&
                     bytesRead >= 8 &&
                     hdr->evt == 0x0E &&
                     hdr->len >= 0x06 &&
                     hdr->data[1] == 0x22 &&
                     hdr->data[2] == 0x20) {

                const uint8_t status = hdr->data[3];

                if (status != 0) {
                    /*
                     * Controller rejected the private command. Do not fake
                     * success; disarm and leave the error visible.
                     */
                    dleResponsesPending = 0;
                } else {
                    /*
                     * The private 0x2022 succeeded. bluetoothd did not issue
                     * this command and is not waiting for its Command Complete,
                     * so use this event itself as the carrier for the missing
                     * LE Data Length Change.
                     */
                    fakeDataLengthChangeEvent[3] = hdr->data[4];
                    fakeDataLengthChangeEvent[4] = hdr->data[5];

                    const size_t written =
                        dataBuffer->writeBytes(
                            0,
                            fakeDataLengthChangeEvent,
                            sizeof(fakeDataLengthChangeEvent)
                        );

                    if (written == sizeof(fakeDataLengthChangeEvent)) {
                        bytesTransferred = sizeof(fakeDataLengthChangeEvent);

                        ++leDataLengthFakeCount;
                        publishDleCounters();

                        dleResponsesPending = 0;
                    }
                }
            }
        }

        if (getKernelVersion() >= KernelVersion::Sequoia)
            dataBuffer->complete(kIODirectionInOut);
    }

    IOFree(buffer, originalSize);
}

static void lePhyCompletion(void *owner, void *parameter, IOReturn status, uint32_t bytesTransferred)
{
    AsyncOwnerData *asyncOwner = static_cast<AsyncOwnerData *>(owner);

    rewriteLePhyEvent(asyncOwner->dataBuffer, bytesTransferred);

    if (asyncOwner->action != nullptr)
        asyncOwner->action(asyncOwner->owner, parameter, status, bytesTransferred);

    delete asyncOwner;
}

static void lePhyBundledCompletion(void *owner,
                                   uint32_t ioCompletionCount,
                                   IOMemoryDescriptor **dataBufferArray,
                                   void **parameter,
                                   IOReturn *statusArray,
                                   uint32_t *actualByteCountArray)
{
    AsyncOwnerData *asyncOwner = static_cast<AsyncOwnerData *>(owner);

    if (asyncOwner->bundledAction != nullptr &&
        dataBufferArray != nullptr &&
        actualByteCountArray != nullptr) {

        for (uint32_t i = 0; i < ioCompletionCount; ++i) {
            uint32_t bytesTransferred = actualByteCountArray[i];
            rewriteLePhyEvent(dataBufferArray[i], bytesTransferred);
            actualByteCountArray[i] = bytesTransferred;
        }

        asyncOwner->bundledAction(
            asyncOwner->owner,
            ioCompletionCount,
            dataBufferArray,
            parameter,
            statusArray,
            actualByteCountArray
        );
    }

    delete asyncOwner;
}

IOReturn CSimpleMtkBtPatcher::newIOSourceIOCompletion(void *that,
                                                       IOMemoryDescriptor *dataBuffer,
                                                       uint32_t dataBufferLength,
                                                       IOUSBHostCompletion *completion,
                                                       uint32_t completionTimeoutMs)
{
    if (_lePhyFix && (remoteReadSCount > 0 || dleResponsesPending > 0) && completion != nullptr) {
        AsyncOwnerData *ownerData = new AsyncOwnerData {};

        if (ownerData != nullptr) {
            ownerData->owner = completion->owner;
            ownerData->action = completion->action;
            ownerData->dataBuffer = dataBuffer;
            ownerData->bundledAction = nullptr;

            IOUSBHostCompletion wrappedCompletion = *completion;
            wrappedCompletion.owner = ownerData;
            wrappedCompletion.action = lePhyCompletion;

            IOReturn ret =
                FunctionCast(newIOSourceIOCompletion, callbackMtkBtPatcher->oldIOSourceIOCompletion)(
                    that,
                    dataBuffer,
                    dataBufferLength,
                    &wrappedCompletion,
                    completionTimeoutMs
                );

            if (ret != kIOReturnSuccess)
                delete ownerData;

            return ret;
        }
    }

    return FunctionCast(newIOSourceIOCompletion, callbackMtkBtPatcher->oldIOSourceIOCompletion)(
        that,
        dataBuffer,
        dataBufferLength,
        completion,
        completionTimeoutMs
    );
}

IOReturn CSimpleMtkBtPatcher::newIOSourceIORjj(void *that,
                                                IOMemoryDescriptor *dataBuffer,
                                                uint32_t dataBufferLength,
                                                uint32_t &bytesTransferred,
                                                uint32_t completionTimeoutMs)
{
    IOReturn ret =
        FunctionCast(newIOSourceIORjj, callbackMtkBtPatcher->oldIOSourceIORjj)(
            that,
            dataBuffer,
            dataBufferLength,
            bytesTransferred,
            completionTimeoutMs
        );

    /*
     * This overload is synchronous/stack-owned: call Apple's implementation
     * first, then rewrite the returned event.
     */
    if (ret == kIOReturnSuccess && _lePhyFix && (remoteReadSCount > 0 || dleResponsesPending > 0))
        rewriteLePhyEvent(dataBuffer, bytesTransferred);

    return ret;
}

IOReturn CSimpleMtkBtPatcher::newIOSourceIOBundled(void *that,
                                                    IOMemoryDescriptor *dataBuffer,
                                                    uint32_t dataBufferLength,
                                                    IOUSBHostBundledCompletion *completion,
                                                    uint32_t completionTimeoutMs)
{
    if (_lePhyFix && (remoteReadSCount > 0 || dleResponsesPending > 0) && completion != nullptr) {
        AsyncOwnerData *ownerData = new AsyncOwnerData {};

        if (ownerData != nullptr) {
            ownerData->owner = completion->owner;
            ownerData->bundledAction = completion->action;
            ownerData->dataBuffer = dataBuffer;
            ownerData->action = nullptr;

            IOUSBHostBundledCompletion wrappedCompletion = *completion;
            wrappedCompletion.owner = ownerData;
            wrappedCompletion.action = lePhyBundledCompletion;

            IOReturn ret =
                FunctionCast(newIOSourceIOBundled, callbackMtkBtPatcher->oldIOSourceIOBundled)(
                    that,
                    dataBuffer,
                    dataBufferLength,
                    &wrappedCompletion,
                    completionTimeoutMs
                );

            if (ret != kIOReturnSuccess)
                delete ownerData;

            return ret;
        }
    }

    return FunctionCast(newIOSourceIOBundled, callbackMtkBtPatcher->oldIOSourceIOBundled)(
        that,
        dataBuffer,
        dataBufferLength,
        completion,
        completionTimeoutMs
    );
}

#pragma mark - HCI Read Clock for MediaTek

/*
 * bluetoothd reads the BT clock with HCI_Read_Clock(handle, Which_Clock=0).
 * For the local clock MediaTek returns Connection_Handle 0 instead of the
 * handle that was passed, so bluetoothd cannot match the result to the
 * connection ("Failed to read BT clock"). AAC-ELD streaming to AirPods
 * timestamps audio with that clock, and the headphones never start playing.
 * Ask for the piconet clock of the connection instead (Which_Clock=1): the
 * controller then returns the handle, and as central the piconet clock is
 * our own native clock.
 */
#define HCI_OP_READ_CLOCK 0x1407
static uint32_t readClockRewritten, aclPbRewritten;

static void publishCounters()
{
    IOService *res = IOService::getResourceService();
    if (!res)
        return;
    OSDictionary *d = OSDictionary::withCapacity(2);
    if (!d)
        return;
    OSNumber *n1 = OSNumber::withNumber(readClockRewritten, 32);
    OSNumber *n2 = OSNumber::withNumber(aclPbRewritten, 32);
    if (n1) { d->setObject("ReadClockRewritten", n1); n1->release(); }
    if (n2) { d->setObject("AclPbRewritten", n2); n2->release(); }
    res->setProperty("SimpleMtkBtPatcher", d);
    d->release();
}

void CSimpleMtkBtPatcher::fixReadClock(StandardUSB::DeviceRequest &request, void *data, IOMemoryDescriptor *descriptor)
{
    /* HCI command: class request to the interface, host to device */
    if ((request.bmRequestType & 0x80) != 0 || request.wLength != 6)
        return;
    uint8_t cmd[6];
    if (data != nullptr) {
        memcpy(cmd, data, sizeof(cmd));
    } else if (descriptor != nullptr && descriptor->prepare(kIODirectionInOut) == kIOReturnSuccess) {
        bool ok = descriptor->readBytes(0, cmd, sizeof(cmd)) == sizeof(cmd);
        if (ok && (cmd[0] | (cmd[1] << 8)) == HCI_OP_READ_CLOCK && cmd[2] == 3 && cmd[5] == 0) {
            cmd[5] = 1;
            if (descriptor->writeBytes(5, &cmd[5], 1) == 1) {
                readClockRewritten++;
                publishCounters();
            }
        }
        descriptor->complete(kIODirectionInOut);
        return;
    } else {
        return;
    }
    if ((cmd[0] | (cmd[1] << 8)) == HCI_OP_READ_CLOCK && cmd[2] == 3 && cmd[5] == 0) {
        static_cast<uint8_t *>(data)[5] = 1;
        readClockRewritten++;
        publishCounters();
    }
}

#pragma mark - Non-flushable ACL for MediaTek

/*
 * bluetoothd sends A2DP media as "first automatically flushable" ACL
 * fragments (packet boundary flag 0b10). MediaTek firmware flushes those
 * when a large L2CAP frame (AAC-ELD to AirPods, ~966 bytes) does not make
 * it in one baseband packet, so the headphones get broken frames, never
 * start playback and pause the stream. Linux always sends ACL data as
 * non-flushable (0b00) to these controllers. Rewrite the flag on HCI ACL
 * packets going to bulk OUT endpoints.
 */
static uint32_t pbRewritten;

IOReturn CSimpleMtkBtPatcher::newPipeIo(void *that, IOMemoryDescriptor *dataBuffer, uint32_t dataBufferLength, IOUSBHostCompletion *completion, uint32_t completionTimeoutMs)
{
    IOUSBHostPipe *pipe = static_cast<IOUSBHostPipe *>(that);
    if (dataBuffer != nullptr && dataBufferLength >= 4 && dataBufferLength <= 1100) {
        const StandardUSB::EndpointDescriptor *ep = pipe->getEndpointDescriptor();
        if (ep != nullptr &&
            (ep->bmAttributes & 0x03) == 0x02 &&        /* bulk */
            (ep->bEndpointAddress & 0x80) == 0 &&       /* OUT */
            dataBuffer->prepare(kIODirectionInOut) == kIOReturnSuccess) {
            uint8_t hdr[4];
            if (dataBuffer->readBytes(0, hdr, sizeof(hdr)) == sizeof(hdr)) {
                uint16_t handle = hdr[0] | (hdr[1] << 8);
                uint16_t len = hdr[2] | (hdr[3] << 8);
                /* looks like HCI ACL: sane handle, length matches, PB = 0b10 */
                if ((handle & 0x0fff) < 0x0f00 && len + 4u == dataBufferLength &&
                    ((handle >> 12) & 3) == 2) {
                    hdr[1] &= ~0x30;
                    if (dataBuffer->writeBytes(1, &hdr[1], 1) == 1) {
                        if (pbRewritten++ < 3)
                            SYSLOG(DRV_NAME, "ACL PB flushable->non-flushable handle 0x%03x len %u", handle & 0x0fff, len);
                        if ((++aclPbRewritten & 0x3ff) == 1)
                            publishCounters();
                    }
                }
            }
            dataBuffer->complete(kIODirectionInOut);
        }
    }
    return FunctionCast(newPipeIo, callbackMtkBtPatcher->oldPipeIo)(that, dataBuffer, dataBufferLength, completion, completionTimeoutMs);
}
