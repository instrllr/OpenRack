// OpenRack Virtual Audio: a loopback Core Audio device (AudioServerPlugIn).
//
// Whatever any app plays to "OpenRack Virtual Audio" (16 channels) comes back out of the same device's
// 16 input channels, so a DAW, browser or game can be routed into OpenRack by choosing this device as
// OpenRack's audio input.
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define kChannels      16
#define kRingFrames    65536
#define kZTSPeriod     16384
#define kDeviceName    "OpenRack Virtual Audio"
#define kDeviceUID     "com.openrack.driver.device"
#define kModelUID      "com.openrack.driver.model"
#define kMaker         "OpenRack"

enum
{
    kObjDevice    = 2,
    kObjInStream  = 3,     // input scope: what apps / OpenRack record
    kObjOutStream = 4      // output scope: what apps play
};

static const Float64 kRates[] = { 44100.0, 48000.0, 88200.0, 96000.0 };
#define kNumRates 4

static pthread_mutex_t gMutex = PTHREAD_MUTEX_INITIALIZER;
static AudioServerPlugInDriverRef gDriverRef = NULL;
static AudioServerPlugInHostRef gHost = NULL;
static UInt32 gRefCount = 1;
static Float64 gRate = 48000.0;
static UInt32 gIOCount = 0;
static Float64 gTicksPerFrame = 0.0;
static UInt64 gAnchorHost = 0;
static UInt64 gNumTimeStamps = 0;
static Float32* gRing = NULL;
static _Atomic UInt64 gLastWriteEnd = 0;

//------------------------------------------------------------------------------------------------
static void recomputeClock(void)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    const Float64 ticksPerSecond = 1.0e9 * (Float64) tb.denom / (Float64) tb.numer;
    gTicksPerFrame = ticksPerSecond / gRate;
}

static AudioStreamBasicDescription currentFormat(void)
{
    AudioStreamBasicDescription f;
    memset(&f, 0, sizeof f);
    f.mSampleRate = gRate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = kChannels * sizeof(Float32);
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = kChannels * sizeof(Float32);
    f.mChannelsPerFrame = kChannels;
    f.mBitsPerChannel = 32;
    return f;
}

//------------------------------------------------------------------------------------------------
// COM plumbing
static HRESULT QueryInterface(void* driver, REFIID iid, LPVOID* out)
{
    (void) driver;
    CFUUIDRef req = CFUUIDCreateFromUUIDBytes(NULL, iid);
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(req, IUnknownUUID) || CFEqual(req, kAudioServerPlugInDriverInterfaceUUID))
    {
        pthread_mutex_lock(&gMutex);
        ++gRefCount;
        pthread_mutex_unlock(&gMutex);
        *out = gDriverRef;
        r = S_OK;
    }
    CFRelease(req);
    return r;
}

static ULONG AddRef(void* driver)
{
    (void) driver;
    pthread_mutex_lock(&gMutex);
    const ULONG n = ++gRefCount;
    pthread_mutex_unlock(&gMutex);
    return n;
}

static ULONG Release(void* driver)
{
    (void) driver;
    pthread_mutex_lock(&gMutex);
    if (gRefCount > 0) --gRefCount;
    const ULONG n = gRefCount;
    pthread_mutex_unlock(&gMutex);
    return n;
}

static OSStatus Initialize(AudioServerPlugInDriverRef driver, AudioServerPlugInHostRef host)
{
    (void) driver;
    gHost = host;
    gRing = (Float32*) calloc((size_t) kRingFrames * kChannels, sizeof(Float32));
    recomputeClock();
    return gRing != NULL ? kAudioHardwareNoError : kAudioHardwareUnspecifiedError;
}

static OSStatus CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo* c, AudioObjectID* out)
{ (void) d; (void) desc; (void) c; (void) out; return kAudioHardwareUnsupportedOperationError; }

static OSStatus DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID id)
{ (void) d; (void) id; return kAudioHardwareUnsupportedOperationError; }

static OSStatus AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id, const AudioServerPlugInClientInfo* c)
{ (void) d; (void) id; (void) c; return kAudioHardwareNoError; }

static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id, const AudioServerPlugInClientInfo* c)
{ (void) d; (void) id; (void) c; return kAudioHardwareNoError; }

static OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID id, UInt64 action, void* info)
{
    (void) d; (void) id; (void) info;
    pthread_mutex_lock(&gMutex);
    gRate = (Float64) action;
    recomputeClock();
    pthread_mutex_unlock(&gMutex);
    return kAudioHardwareNoError;
}

static OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID id, UInt64 action, void* info)
{ (void) d; (void) id; (void) action; (void) info; return kAudioHardwareNoError; }

//------------------------------------------------------------------------------------------------
// Properties
static Boolean isRate(Float64 r)
{
    for (int i = 0; i < kNumRates; ++i)
        if (kRates[i] == r) return true;
    return false;
}

#define RETURN_U32(v) \
    do { *outSize = sizeof(UInt32); \
         if (! sizeOnly) { if (inSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError; *(UInt32*) out = (v); } \
         return kAudioHardwareNoError; } while (0)

#define RETURN_F64(v) \
    do { *outSize = sizeof(Float64); \
         if (! sizeOnly) { if (inSize < sizeof(Float64)) return kAudioHardwareBadPropertySizeError; *(Float64*) out = (v); } \
         return kAudioHardwareNoError; } while (0)

#define RETURN_STR(s) \
    do { *outSize = sizeof(CFStringRef); \
         if (! sizeOnly) { if (inSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError; *(CFStringRef*) out = CFStringCreateWithCString(NULL, (s), kCFStringEncodingUTF8); } \
         return kAudioHardwareNoError; } while (0)

static OSStatus returnIDs(const AudioObjectID* ids, UInt32 n, UInt32 inSize, UInt32* outSize, void* out, int sizeOnly)
{
    if (sizeOnly) { *outSize = n * (UInt32) sizeof(AudioObjectID); return kAudioHardwareNoError; }
    UInt32 cnt = inSize / (UInt32) sizeof(AudioObjectID);
    if (cnt > n) cnt = n;
    if (cnt > 0) memcpy(out, ids, cnt * sizeof(AudioObjectID));
    *outSize = cnt * (UInt32) sizeof(AudioObjectID);
    return kAudioHardwareNoError;
}

static OSStatus getProperty(AudioObjectID obj, const AudioObjectPropertyAddress* a, UInt32 qualSize, const void* qual,
                            UInt32 inSize, UInt32* outSize, void* out, int sizeOnly)
{
    const AudioObjectPropertyScope scope = a->mScope;

    if (obj == kAudioObjectPlugInObject)
    {
        switch (a->mSelector)
        {
            case kAudioObjectPropertyBaseClass:  RETURN_U32(kAudioObjectClassID);
            case kAudioObjectPropertyClass:      RETURN_U32(kAudioPlugInClassID);
            case kAudioObjectPropertyOwner:      RETURN_U32(kAudioObjectUnknown);
            case kAudioObjectPropertyManufacturer: RETURN_STR(kMaker);
            case kAudioObjectPropertyOwnedObjects:
            case kAudioPlugInPropertyDeviceList:
            {
                const AudioObjectID ids[1] = { kObjDevice };
                return returnIDs(ids, 1, inSize, outSize, out, sizeOnly);
            }
            case kAudioPlugInPropertyTranslateUIDToDevice:
            {
                *outSize = sizeof(AudioObjectID);
                if (sizeOnly) return kAudioHardwareNoError;
                if (inSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                AudioObjectID id = kAudioObjectUnknown;
                if (qualSize == sizeof(CFStringRef) && qual != NULL)
                {
                    CFStringRef uid = *(const CFStringRef*) qual;
                    CFStringRef mine = CFStringCreateWithCString(NULL, kDeviceUID, kCFStringEncodingUTF8);
                    if (uid != NULL && CFStringCompare(uid, mine, 0) == kCFCompareEqualTo)
                        id = kObjDevice;
                    CFRelease(mine);
                }
                *(AudioObjectID*) out = id;
                return kAudioHardwareNoError;
            }
            default: return kAudioHardwareUnknownPropertyError;
        }
    }

    if (obj == kObjDevice)
    {
        switch (a->mSelector)
        {
            case kAudioObjectPropertyBaseClass:  RETURN_U32(kAudioObjectClassID);
            case kAudioObjectPropertyClass:      RETURN_U32(kAudioDeviceClassID);
            case kAudioObjectPropertyOwner:      RETURN_U32(kAudioObjectPlugInObject);
            case kAudioObjectPropertyName:       RETURN_STR(kDeviceName);
            case kAudioObjectPropertyManufacturer: RETURN_STR(kMaker);
            case kAudioDevicePropertyDeviceUID:  RETURN_STR(kDeviceUID);
            case kAudioDevicePropertyModelUID:   RETURN_STR(kModelUID);
            case kAudioDevicePropertyTransportType: RETURN_U32(kAudioDeviceTransportTypeVirtual);
            case kAudioDevicePropertyClockDomain: RETURN_U32(0);
            case kAudioDevicePropertyDeviceIsAlive: RETURN_U32(1);
            case kAudioDevicePropertyDeviceIsRunning:
            case kAudioDevicePropertyDeviceIsRunningSomewhere: RETURN_U32(gIOCount > 0 ? 1 : 0);
            case kAudioDevicePropertyDeviceCanBeDefaultDevice: RETURN_U32(1);
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: RETURN_U32(0);
            case kAudioDevicePropertyLatency:    RETURN_U32(0);
            case kAudioDevicePropertySafetyOffset: RETURN_U32(0);
            case kAudioDevicePropertyIsHidden:   RETURN_U32(0);
            case kAudioDevicePropertyZeroTimeStampPeriod: RETURN_U32(kZTSPeriod);
            case kAudioDevicePropertyNominalSampleRate: RETURN_F64(gRate);
            case kAudioDevicePropertyRelatedDevices:
            {
                const AudioObjectID ids[1] = { kObjDevice };
                return returnIDs(ids, 1, inSize, outSize, out, sizeOnly);
            }
            case kAudioObjectPropertyOwnedObjects:
            case kAudioDevicePropertyStreams:
            {
                AudioObjectID ids[2];
                UInt32 n = 0;
                if (scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeInput)  ids[n++] = kObjInStream;
                if (scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeOutput) ids[n++] = kObjOutStream;
                return returnIDs(ids, n, inSize, outSize, out, sizeOnly);
            }
            case kAudioObjectPropertyControlList:
                *outSize = 0;
                return kAudioHardwareNoError;
            case kAudioDevicePropertyAvailableNominalSampleRates:
            {
                if (sizeOnly) { *outSize = kNumRates * (UInt32) sizeof(AudioValueRange); return kAudioHardwareNoError; }
                UInt32 cnt = inSize / (UInt32) sizeof(AudioValueRange);
                if (cnt > kNumRates) cnt = kNumRates;
                AudioValueRange* v = (AudioValueRange*) out;
                for (UInt32 i = 0; i < cnt; ++i) { v[i].mMinimum = kRates[i]; v[i].mMaximum = kRates[i]; }
                *outSize = cnt * (UInt32) sizeof(AudioValueRange);
                return kAudioHardwareNoError;
            }
            case kAudioDevicePropertyPreferredChannelsForStereo:
            {
                *outSize = 2 * sizeof(UInt32);
                if (! sizeOnly)
                {
                    if (inSize < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                    ((UInt32*) out)[0] = 1;
                    ((UInt32*) out)[1] = 2;
                }
                return kAudioHardwareNoError;
            }
            default: return kAudioHardwareUnknownPropertyError;
        }
    }

    if (obj == kObjInStream || obj == kObjOutStream)
    {
        switch (a->mSelector)
        {
            case kAudioObjectPropertyBaseClass:  RETURN_U32(kAudioObjectClassID);
            case kAudioObjectPropertyClass:      RETURN_U32(kAudioStreamClassID);
            case kAudioObjectPropertyOwner:      RETURN_U32(kObjDevice);
            case kAudioObjectPropertyOwnedObjects: *outSize = 0; return kAudioHardwareNoError;
            case kAudioStreamPropertyIsActive:   RETURN_U32(1);
            case kAudioStreamPropertyDirection:  RETURN_U32(obj == kObjInStream ? 1 : 0);
            case kAudioStreamPropertyTerminalType: RETURN_U32(obj == kObjInStream ? kAudioStreamTerminalTypeLine : kAudioStreamTerminalTypeLine);
            case kAudioStreamPropertyStartingChannel: RETURN_U32(1);
            case kAudioStreamPropertyLatency:    RETURN_U32(0);
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat:
            {
                *outSize = sizeof(AudioStreamBasicDescription);
                if (! sizeOnly)
                {
                    if (inSize < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
                    *(AudioStreamBasicDescription*) out = currentFormat();
                }
                return kAudioHardwareNoError;
            }
            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats:
            {
                if (sizeOnly) { *outSize = kNumRates * (UInt32) sizeof(AudioStreamRangedDescription); return kAudioHardwareNoError; }
                UInt32 cnt = inSize / (UInt32) sizeof(AudioStreamRangedDescription);
                if (cnt > kNumRates) cnt = kNumRates;
                AudioStreamRangedDescription* v = (AudioStreamRangedDescription*) out;
                const Float64 keep = gRate;
                for (UInt32 i = 0; i < cnt; ++i)
                {
                    gRate = kRates[i];
                    v[i].mFormat = currentFormat();
                    v[i].mSampleRateRange.mMinimum = kRates[i];
                    v[i].mSampleRateRange.mMaximum = kRates[i];
                }
                gRate = keep;
                *outSize = cnt * (UInt32) sizeof(AudioStreamRangedDescription);
                return kAudioHardwareNoError;
            }
            default: return kAudioHardwareUnknownPropertyError;
        }
    }

    return kAudioHardwareBadObjectError;
}

static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress* a)
{
    (void) d; (void) pid;
    UInt32 size = 0;
    return getProperty(obj, a, 0, NULL, 0, &size, NULL, 1) == kAudioHardwareNoError;
}

static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress* a, Boolean* out)
{
    (void) d; (void) pid;
    UInt32 size = 0;
    const OSStatus r = getProperty(obj, a, 0, NULL, 0, &size, NULL, 1);
    if (r != kAudioHardwareNoError) return r;
    *out = (a->mSelector == kAudioDevicePropertyNominalSampleRate
            || a->mSelector == kAudioStreamPropertyVirtualFormat
            || a->mSelector == kAudioStreamPropertyPhysicalFormat);
    return kAudioHardwareNoError;
}

static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress* a,
                                    UInt32 qualSize, const void* qual, UInt32* outSize)
{
    (void) d; (void) pid;
    return getProperty(obj, a, qualSize, qual, 0, outSize, NULL, 1);
}

static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress* a,
                                UInt32 qualSize, const void* qual, UInt32 inSize, UInt32* outSize, void* out)
{
    (void) d; (void) pid;
    pthread_mutex_lock(&gMutex);
    const OSStatus r = getProperty(obj, a, qualSize, qual, inSize, outSize, out, 0);
    pthread_mutex_unlock(&gMutex);
    return r;
}

static OSStatus SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress* a,
                                UInt32 qualSize, const void* qual, UInt32 inSize, const void* data)
{
    (void) d; (void) pid; (void) qualSize; (void) qual;
    Float64 rate = 0.0;

    if (obj == kObjDevice && a->mSelector == kAudioDevicePropertyNominalSampleRate)
    {
        if (inSize < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        rate = *(const Float64*) data;
    }
    else if ((obj == kObjInStream || obj == kObjOutStream)
             && (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat))
    {
        if (inSize < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
        const AudioStreamBasicDescription* f = (const AudioStreamBasicDescription*) data;
        if (f->mFormatID != kAudioFormatLinearPCM || f->mChannelsPerFrame != kChannels || f->mBitsPerChannel != 32
            || ! (f->mFormatFlags & kAudioFormatFlagIsFloat))
            return kAudioDeviceUnsupportedFormatError;
        rate = f->mSampleRate;
    }
    else
        return kAudioHardwareUnknownPropertyError;

    if (! isRate(rate)) return kAudioHardwareIllegalOperationError;
    if (rate != gRate && gHost != NULL)
        gHost->RequestDeviceConfigurationChange(gHost, kObjDevice, (UInt64) rate, NULL);
    return kAudioHardwareNoError;
}

//------------------------------------------------------------------------------------------------
// IO
static OSStatus StartIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client)
{
    (void) d; (void) id; (void) client;
    pthread_mutex_lock(&gMutex);
    if (gIOCount == 0)
    {
        gNumTimeStamps = 0;
        gAnchorHost = mach_absolute_time();
        atomic_store(&gLastWriteEnd, 0);
        memset(gRing, 0, (size_t) kRingFrames * kChannels * sizeof(Float32));
    }
    ++gIOCount;
    pthread_mutex_unlock(&gMutex);
    return kAudioHardwareNoError;
}

static OSStatus StopIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client)
{
    (void) d; (void) id; (void) client;
    pthread_mutex_lock(&gMutex);
    if (gIOCount > 0) --gIOCount;
    pthread_mutex_unlock(&gMutex);
    return kAudioHardwareNoError;
}

static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client,
                                 Float64* outSample, UInt64* outHost, UInt64* outSeed)
{
    (void) d; (void) id; (void) client;
    pthread_mutex_lock(&gMutex);
    const UInt64 now = mach_absolute_time();
    const Float64 ticksPerPeriod = gTicksPerFrame * (Float64) kZTSPeriod;
    const UInt64 next = gAnchorHost + (UInt64) ((Float64) (gNumTimeStamps + 1) * ticksPerPeriod);
    if (next <= now)
        ++gNumTimeStamps;
    *outSample = (Float64) (gNumTimeStamps * (UInt64) kZTSPeriod);
    *outHost = gAnchorHost + (UInt64) ((Float64) gNumTimeStamps * ticksPerPeriod);
    *outSeed = 1;
    pthread_mutex_unlock(&gMutex);
    return kAudioHardwareNoError;
}

static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, UInt32 op,
                                  Boolean* willDo, Boolean* inPlace)
{
    (void) d; (void) id; (void) client;
    *willDo = (op == kAudioServerPlugInIOOperationReadInput || op == kAudioServerPlugInIOOperationWriteMix);
    *inPlace = true;
    return kAudioHardwareNoError;
}

static OSStatus BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, UInt32 op,
                                 UInt32 frames, const AudioServerPlugInIOCycleInfo* info)
{ (void) d; (void) id; (void) client; (void) op; (void) frames; (void) info; return kAudioHardwareNoError; }

static OSStatus EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, UInt32 op,
                               UInt32 frames, const AudioServerPlugInIOCycleInfo* info)
{ (void) d; (void) id; (void) client; (void) op; (void) frames; (void) info; return kAudioHardwareNoError; }

static OSStatus DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, AudioObjectID stream, UInt32 client,
                              UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo* info,
                              void* mainBuffer, void* secondaryBuffer)
{
    (void) d; (void) id; (void) stream; (void) client; (void) secondaryBuffer;
    if (gRing == NULL || mainBuffer == NULL)
        return kAudioHardwareNoError;

    Float32* buf = (Float32*) mainBuffer;

    if (op == kAudioServerPlugInIOOperationWriteMix)
    {
        const UInt64 start = (UInt64) info->mOutputTime.mSampleTime;
        UInt32 done = 0;
        while (done < frames)
        {
            const UInt32 pos = (UInt32) ((start + done) % kRingFrames);
            UInt32 n = frames - done;
            if (n > kRingFrames - pos) n = kRingFrames - pos;
            memcpy(gRing + (size_t) pos * kChannels, buf + (size_t) done * kChannels, (size_t) n * kChannels * sizeof(Float32));
            done += n;
        }
        const UInt64 end = start + frames;
        if (end > atomic_load(&gLastWriteEnd))
            atomic_store(&gLastWriteEnd, end);
    }
    else if (op == kAudioServerPlugInIOOperationReadInput)
    {
        const UInt64 start = (UInt64) info->mInputTime.mSampleTime;
        const UInt64 written = atomic_load(&gLastWriteEnd);
        // Only hand out audio that has actually been written (and not yet overwritten).
        if (written == 0 || start + frames > written || start + kRingFrames < written)
        {
            memset(buf, 0, (size_t) frames * kChannels * sizeof(Float32));
            return kAudioHardwareNoError;
        }
        UInt32 done = 0;
        while (done < frames)
        {
            const UInt32 pos = (UInt32) ((start + done) % kRingFrames);
            UInt32 n = frames - done;
            if (n > kRingFrames - pos) n = kRingFrames - pos;
            memcpy(buf + (size_t) done * kChannels, gRing + (size_t) pos * kChannels, (size_t) n * kChannels * sizeof(Float32));
            done += n;
        }
    }
    return kAudioHardwareNoError;
}

//------------------------------------------------------------------------------------------------
static AudioServerPlugInDriverInterface gInterface =
{
    NULL,
    QueryInterface, AddRef, Release,
    Initialize, CreateDevice, DestroyDevice, AddDeviceClient, RemoveDeviceClient,
    PerformDeviceConfigurationChange, AbortDeviceConfigurationChange,
    HasProperty, IsPropertySettable, GetPropertyDataSize, GetPropertyData, SetPropertyData,
    StartIO, StopIO, GetZeroTimeStamp, WillDoIOOperation, BeginIOOperation, DoIOOperation, EndIOOperation
};
static AudioServerPlugInDriverInterface* gInterfacePtr = &gInterface;

void* OpenRackDriverCreate(CFAllocatorRef allocator, CFUUIDRef requestedType);
void* OpenRackDriverCreate(CFAllocatorRef allocator, CFUUIDRef requestedType)
{
    (void) allocator;
    if (! CFEqual(requestedType, kAudioServerPlugInTypeUUID))
        return NULL;
    gDriverRef = (AudioServerPlugInDriverRef) &gInterfacePtr;
    return gDriverRef;
}
