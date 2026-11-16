// OpenRack Bridge - a virtual audio device for macOS.
//
// This is a CoreAudio "AudioServerPlugIn" (a user-space driver loaded by
// coreaudiod). It shows up as an audio device called "OpenRack Bridge":
// whatever a DAW plays to it as an output can be recorded from it as an input.
// OpenRack uses that input, runs the audio through the rack, and plays it out
// to your real speakers or interface.
//
// Design: a single device with one input stream and one output stream, both
// stereo 32-bit float, sharing a ring buffer. The output side writes into the
// ring and the input side reads (and clears) it, both indexed by sample time.

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stddef.h>
#include <string.h>

//==============================================================================
#define kDeviceName   "OpenRack Bridge"
#define kDeviceUID    "OpenRackBridge_UID"
#define kModelUID     "OpenRackBridge_ModelUID"
#define kManufacturer "OpenRack"

enum
{
    kObjectID_PlugIn        = kAudioObjectPlugInObject,
    kObjectID_Device        = 2,
    kObjectID_Stream_Input  = 3,
    kObjectID_Stream_Output = 4
};

enum
{
    kChannels = 2,
    kRingFrames = 65536,            // size of the loopback ring buffer, in frames
    kZeroTimeStampPeriod = 16384    // frames between clock "zero" timestamps
};

#define kBytesPerFrame (kChannels * sizeof(Float32))

static const Float64 kSampleRates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
#define kNumSampleRates ((UInt32) (sizeof(kSampleRates) / sizeof(kSampleRates[0])))

//==============================================================================
// State
static pthread_mutex_t gStateMutex = PTHREAD_MUTEX_INITIALIZER;
static UInt32 gRefCount = 0;
static AudioServerPlugInHostRef gHost = NULL;
static Float64 gSampleRate = 48000.0;
static UInt64 gIOClients = 0;
static Float64 gHostTicksPerFrame = 0.0;
static UInt64 gNumberTimeStamps = 0;
static UInt64 gAnchorHostTime = 0;
static Float32 gRing[kRingFrames * kChannels];

static void UpdateTiming(void)
{
    struct mach_timebase_info tb;
    mach_timebase_info(&tb);
    Float64 ticksPerSecond = ((Float64) tb.denom / (Float64) tb.numer) * 1000000000.0;
    gHostTicksPerFrame = ticksPerSecond / gSampleRate;
}

static Boolean IsSupportedRate(Float64 rate)
{
    for (UInt32 i = 0; i < kNumSampleRates; ++i)
        if (kSampleRates[i] == rate)
            return true;
    return false;
}

static AudioStreamBasicDescription MakeFormat(Float64 rate)
{
    AudioStreamBasicDescription f;
    memset(&f, 0, sizeof(f));
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = (UInt32) kBytesPerFrame;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = (UInt32) kBytesPerFrame;
    f.mChannelsPerFrame = kChannels;
    f.mBitsPerChannel = 32;
    return f;
}

//==============================================================================
// Reply helpers: every property function can be called just to learn the size
// (out == NULL) or to fetch the data.
static OSStatus ReplyBytes(const void* src, UInt32 bytes, UInt32 inSize, UInt32* outSize, void* out)
{
    if (out != NULL)
    {
        if (inSize < bytes)
            return kAudioHardwareBadPropertySizeError;
        if (bytes > 0)
            memcpy(out, src, bytes);
    }
    *outSize = bytes;
    return kAudioHardwareNoError;
}

static OSStatus ReplyArray(const void* src, UInt32 elemSize, UInt32 count,
                           UInt32 inSize, UInt32* outSize, void* out)
{
    if (out == NULL)
    {
        *outSize = elemSize * count;
        return kAudioHardwareNoError;
    }

    UInt32 fit = inSize / elemSize;
    if (fit > count)
        fit = count;
    if (fit > 0)
        memcpy(out, src, fit * elemSize);
    *outSize = fit * elemSize;
    return kAudioHardwareNoError;
}

#define REPLY_VALUE(type, value) \
    do { type v_ = (value); return ReplyBytes(&v_, (UInt32) sizeof(type), inSize, outSize, out); } while (0)

// Owned-object lists can be filtered by a list of class IDs in the qualifier.
static Boolean WantClass(UInt32 qSize, const void* q, AudioClassID cls)
{
    if (qSize == 0 || q == NULL)
        return true;

    const AudioClassID* list = (const AudioClassID*) q;
    UInt32 n = qSize / (UInt32) sizeof(AudioClassID);
    for (UInt32 i = 0; i < n; ++i)
        if (list[i] == cls || list[i] == kAudioObjectClassID)
            return true;
    return false;
}

//==============================================================================
// Properties: plug-in object
static OSStatus PlugIn_Property(const AudioObjectPropertyAddress* a, UInt32 qSize, const void* q,
                                UInt32 inSize, UInt32* outSize, void* out)
{
    switch (a->mSelector)
    {
        case kAudioObjectPropertyBaseClass:   REPLY_VALUE(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass:       REPLY_VALUE(AudioClassID, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner:       REPLY_VALUE(AudioObjectID, kAudioObjectUnknown);
        case kAudioObjectPropertyManufacturer: REPLY_VALUE(CFStringRef, CFSTR(kManufacturer));

        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
        {
            AudioObjectID ids[1] = { kObjectID_Device };
            UInt32 n = 1;
            if (a->mSelector == kAudioObjectPropertyOwnedObjects && ! WantClass(qSize, q, kAudioDeviceClassID))
                n = 0;
            return ReplyArray(ids, (UInt32) sizeof(AudioObjectID), n, inSize, outSize, out);
        }

        case kAudioPlugInPropertyTranslateUIDToDevice:
        {
            if (out == NULL)
            {
                *outSize = (UInt32) sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            }
            if (qSize != sizeof(CFStringRef) || q == NULL)
                return kAudioHardwareBadPropertySizeError;

            CFStringRef uid = *((const CFStringRef*) q);
            AudioObjectID id = kAudioObjectUnknown;
            if (uid != NULL && CFStringCompare(uid, CFSTR(kDeviceUID), 0) == kCFCompareEqualTo)
                id = kObjectID_Device;
            REPLY_VALUE(AudioObjectID, id);
        }

        case kAudioPlugInPropertyResourceBundle: REPLY_VALUE(CFStringRef, CFSTR(""));

        default: return kAudioHardwareUnknownPropertyError;
    }
}

//==============================================================================
// Properties: the device
static OSStatus Device_Property(const AudioObjectPropertyAddress* a, UInt32 qSize, const void* q,
                                UInt32 inSize, UInt32* outSize, void* out)
{
    switch (a->mSelector)
    {
        case kAudioObjectPropertyBaseClass:    REPLY_VALUE(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass:        REPLY_VALUE(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner:        REPLY_VALUE(AudioObjectID, kObjectID_PlugIn);
        case kAudioObjectPropertyName:         REPLY_VALUE(CFStringRef, CFSTR(kDeviceName));
        case kAudioObjectPropertyManufacturer: REPLY_VALUE(CFStringRef, CFSTR(kManufacturer));

        case kAudioObjectPropertyOwnedObjects:
        {
            AudioObjectID ids[2];
            UInt32 n = 0;
            if (WantClass(qSize, q, kAudioStreamClassID))
            {
                ids[n++] = kObjectID_Stream_Input;
                ids[n++] = kObjectID_Stream_Output;
            }
            return ReplyArray(ids, (UInt32) sizeof(AudioObjectID), n, inSize, outSize, out);
        }

        case kAudioDevicePropertyDeviceUID:    REPLY_VALUE(CFStringRef, CFSTR(kDeviceUID));
        case kAudioDevicePropertyModelUID:     REPLY_VALUE(CFStringRef, CFSTR(kModelUID));
        case kAudioDevicePropertyTransportType: REPLY_VALUE(UInt32, kAudioDeviceTransportTypeVirtual);

        case kAudioDevicePropertyRelatedDevices:
        {
            AudioObjectID ids[1] = { kObjectID_Device };
            return ReplyArray(ids, (UInt32) sizeof(AudioObjectID), 1, inSize, outSize, out);
        }

        case kAudioDevicePropertyClockDomain:  REPLY_VALUE(UInt32, 0);
        case kAudioDevicePropertyDeviceIsAlive: REPLY_VALUE(UInt32, 1);

        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceIsRunningSomewhere:
        {
            pthread_mutex_lock(&gStateMutex);
            UInt32 running = gIOClients > 0 ? 1 : 0;
            pthread_mutex_unlock(&gStateMutex);
            REPLY_VALUE(UInt32, running);
        }

        case kAudioDevicePropertyDeviceCanBeDefaultDevice:       REPLY_VALUE(UInt32, 1);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: REPLY_VALUE(UInt32, 1);
        case kAudioDevicePropertyLatency:                        REPLY_VALUE(UInt32, 0);
        case kAudioDevicePropertySafetyOffset:                   REPLY_VALUE(UInt32, 0);
        case kAudioDevicePropertyIsHidden:                       REPLY_VALUE(UInt32, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod:            REPLY_VALUE(UInt32, kZeroTimeStampPeriod);
        case kAudioDevicePropertyConfigurationApplication:
            REPLY_VALUE(CFStringRef, CFSTR("com.apple.audio.AudioMIDISetup"));

        case kAudioDevicePropertyStreams:
        {
            AudioObjectID ids[2];
            UInt32 n = 0;
            if (a->mScope == kAudioObjectPropertyScopeGlobal || a->mScope == kAudioObjectPropertyScopeInput)
                ids[n++] = kObjectID_Stream_Input;
            if (a->mScope == kAudioObjectPropertyScopeGlobal || a->mScope == kAudioObjectPropertyScopeOutput)
                ids[n++] = kObjectID_Stream_Output;
            return ReplyArray(ids, (UInt32) sizeof(AudioObjectID), n, inSize, outSize, out);
        }

        case kAudioObjectPropertyControlList:
            return ReplyArray(NULL, (UInt32) sizeof(AudioObjectID), 0, inSize, outSize, out);

        case kAudioDevicePropertyNominalSampleRate:
        {
            pthread_mutex_lock(&gStateMutex);
            Float64 rate = gSampleRate;
            pthread_mutex_unlock(&gStateMutex);
            REPLY_VALUE(Float64, rate);
        }

        case kAudioDevicePropertyAvailableNominalSampleRates:
        {
            AudioValueRange ranges[kNumSampleRates];
            for (UInt32 i = 0; i < kNumSampleRates; ++i)
            {
                ranges[i].mMinimum = kSampleRates[i];
                ranges[i].mMaximum = kSampleRates[i];
            }
            return ReplyArray(ranges, (UInt32) sizeof(AudioValueRange), kNumSampleRates, inSize, outSize, out);
        }

        case kAudioDevicePropertyPreferredChannelsForStereo:
        {
            UInt32 channels[2] = { 1, 2 };
            return ReplyArray(channels, (UInt32) sizeof(UInt32), 2, inSize, outSize, out);
        }

        case kAudioDevicePropertyPreferredChannelLayout:
        {
            UInt32 size = (UInt32) (offsetof(AudioChannelLayout, mChannelDescriptions)
                                    + kChannels * sizeof(AudioChannelDescription));
            if (out == NULL)
            {
                *outSize = size;
                return kAudioHardwareNoError;
            }
            if (inSize < size)
                return kAudioHardwareBadPropertySizeError;

            AudioChannelLayout* layout = (AudioChannelLayout*) out;
            memset(layout, 0, size);
            layout->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            layout->mNumberChannelDescriptions = kChannels;
            for (UInt32 i = 0; i < kChannels; ++i)
                layout->mChannelDescriptions[i].mChannelLabel = kAudioChannelLabel_Left + i;
            *outSize = size;
            return kAudioHardwareNoError;
        }

        default: return kAudioHardwareUnknownPropertyError;
    }
}

//==============================================================================
// Properties: the two streams
static OSStatus Stream_Property(AudioObjectID obj, const AudioObjectPropertyAddress* a, UInt32 qSize,
                                const void* q, UInt32 inSize, UInt32* outSize, void* out)
{
    (void) qSize;
    (void) q;

    switch (a->mSelector)
    {
        case kAudioObjectPropertyBaseClass: REPLY_VALUE(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass:     REPLY_VALUE(AudioClassID, kAudioStreamClassID);
        case kAudioObjectPropertyOwner:     REPLY_VALUE(AudioObjectID, kObjectID_Device);

        case kAudioObjectPropertyOwnedObjects:
            return ReplyArray(NULL, (UInt32) sizeof(AudioObjectID), 0, inSize, outSize, out);

        case kAudioStreamPropertyIsActive:      REPLY_VALUE(UInt32, 1);
        case kAudioStreamPropertyDirection:     REPLY_VALUE(UInt32, obj == kObjectID_Stream_Input ? 1 : 0);
        case kAudioStreamPropertyTerminalType:  REPLY_VALUE(UInt32, kAudioStreamTerminalTypeLine);
        case kAudioStreamPropertyStartingChannel: REPLY_VALUE(UInt32, 1);
        case kAudioStreamPropertyLatency:       REPLY_VALUE(UInt32, 0);

        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
        {
            pthread_mutex_lock(&gStateMutex);
            AudioStreamBasicDescription f = MakeFormat(gSampleRate);
            pthread_mutex_unlock(&gStateMutex);
            REPLY_VALUE(AudioStreamBasicDescription, f);
        }

        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
        {
            AudioStreamRangedDescription formats[kNumSampleRates];
            for (UInt32 i = 0; i < kNumSampleRates; ++i)
            {
                formats[i].mFormat = MakeFormat(kSampleRates[i]);
                formats[i].mSampleRateRange.mMinimum = kSampleRates[i];
                formats[i].mSampleRateRange.mMaximum = kSampleRates[i];
            }
            return ReplyArray(formats, (UInt32) sizeof(AudioStreamRangedDescription),
                              kNumSampleRates, inSize, outSize, out);
        }

        default: return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus GetProperty(AudioObjectID obj, const AudioObjectPropertyAddress* a, UInt32 qSize,
                            const void* q, UInt32 inSize, UInt32* outSize, void* out)
{
    switch (obj)
    {
        case kObjectID_PlugIn: return PlugIn_Property(a, qSize, q, inSize, outSize, out);
        case kObjectID_Device: return Device_Property(a, qSize, q, inSize, outSize, out);
        case kObjectID_Stream_Input:
        case kObjectID_Stream_Output: return Stream_Property(obj, a, qSize, q, inSize, outSize, out);
        default: return kAudioHardwareBadObjectError;
    }
}

//==============================================================================
// Driver interface
static HRESULT Drv_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface);
static ULONG Drv_AddRef(void* inDriver);
static ULONG Drv_Release(void* inDriver);

static OSStatus Drv_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost)
{
    (void) inDriver;
    gHost = inHost;
    pthread_mutex_lock(&gStateMutex);
    UpdateTiming();
    pthread_mutex_unlock(&gStateMutex);
    return kAudioHardwareNoError;
}

static OSStatus Drv_CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc,
                                 const AudioServerPlugInClientInfo* client, AudioObjectID* outID)
{
    (void) d; (void) desc; (void) client; (void) outID;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus Drv_DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID id)
{
    (void) d; (void) id;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus Drv_AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id,
                                    const AudioServerPlugInClientInfo* client)
{
    (void) d; (void) id; (void) client;
    return kAudioHardwareNoError;
}

static OSStatus Drv_RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id,
                                       const AudioServerPlugInClientInfo* client)
{
    (void) d; (void) id; (void) client;
    return kAudioHardwareNoError;
}

// Sample-rate changes are requested through the host and applied here.
static OSStatus Drv_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID id,
                                                     UInt64 action, void* info)
{
    (void) d; (void) info;
    if (id != kObjectID_Device)
        return kAudioHardwareBadObjectError;

    Float64 rate = (Float64) action;
    if (! IsSupportedRate(rate))
        return kAudioHardwareIllegalOperationError;

    pthread_mutex_lock(&gStateMutex);
    gSampleRate = rate;
    UpdateTiming();
    memset(gRing, 0, sizeof(gRing));
    pthread_mutex_unlock(&gStateMutex);
    return kAudioHardwareNoError;
}

static OSStatus Drv_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID id,
                                                   UInt64 action, void* info)
{
    (void) d; (void) id; (void) action; (void) info;
    return kAudioHardwareNoError;
}

static Boolean Drv_HasProperty(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                               const AudioObjectPropertyAddress* a)
{
    (void) d; (void) pid;
    UInt32 size = 0;
    return GetProperty(obj, a, 0, NULL, 0, &size, NULL) == kAudioHardwareNoError;
}

static OSStatus Drv_IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                                       const AudioObjectPropertyAddress* a, Boolean* outSettable)
{
    (void) d; (void) pid;
    UInt32 size = 0;
    if (GetProperty(obj, a, 0, NULL, 0, &size, NULL) != kAudioHardwareNoError)
        return kAudioHardwareUnknownPropertyError;

    *outSettable = false;
    if (obj == kObjectID_Device && a->mSelector == kAudioDevicePropertyNominalSampleRate)
        *outSettable = true;
    if ((obj == kObjectID_Stream_Input || obj == kObjectID_Stream_Output)
        && (a->mSelector == kAudioStreamPropertyIsActive
            || a->mSelector == kAudioStreamPropertyVirtualFormat
            || a->mSelector == kAudioStreamPropertyPhysicalFormat))
        *outSettable = true;
    return kAudioHardwareNoError;
}

static OSStatus Drv_GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                                        const AudioObjectPropertyAddress* a, UInt32 qSize,
                                        const void* q, UInt32* outSize)
{
    (void) d; (void) pid;
    return GetProperty(obj, a, qSize, q, 0, outSize, NULL);
}

static OSStatus Drv_GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                                    const AudioObjectPropertyAddress* a, UInt32 qSize, const void* q,
                                    UInt32 inSize, UInt32* outSize, void* outData)
{
    (void) d; (void) pid;
    if (outData == NULL)
        return kAudioHardwareIllegalOperationError;
    return GetProperty(obj, a, qSize, q, inSize, outSize, outData);
}

static OSStatus RequestRate(Float64 rate)
{
    if (! IsSupportedRate(rate))
        return kAudioDeviceUnsupportedFormatError;

    pthread_mutex_lock(&gStateMutex);
    Float64 current = gSampleRate;
    pthread_mutex_unlock(&gStateMutex);

    if (rate != current && gHost != NULL)
        gHost->RequestDeviceConfigurationChange(gHost, kObjectID_Device, (UInt64) rate, NULL);
    return kAudioHardwareNoError;
}

static OSStatus Drv_SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                                    const AudioObjectPropertyAddress* a, UInt32 qSize, const void* q,
                                    UInt32 inSize, const void* inData)
{
    (void) d; (void) pid; (void) qSize; (void) q;

    if (obj == kObjectID_Device && a->mSelector == kAudioDevicePropertyNominalSampleRate)
    {
        if (inSize != sizeof(Float64))
            return kAudioHardwareBadPropertySizeError;
        return RequestRate(*((const Float64*) inData));
    }

    if (obj == kObjectID_Stream_Input || obj == kObjectID_Stream_Output)
    {
        if (a->mSelector == kAudioStreamPropertyIsActive)
            return kAudioHardwareNoError;

        if (a->mSelector == kAudioStreamPropertyVirtualFormat
            || a->mSelector == kAudioStreamPropertyPhysicalFormat)
        {
            if (inSize != sizeof(AudioStreamBasicDescription))
                return kAudioHardwareBadPropertySizeError;

            const AudioStreamBasicDescription* f = (const AudioStreamBasicDescription*) inData;
            if (f->mFormatID != kAudioFormatLinearPCM
                || f->mChannelsPerFrame != kChannels
                || f->mBitsPerChannel != 32
                || (f->mFormatFlags & kAudioFormatFlagIsFloat) == 0)
                return kAudioDeviceUnsupportedFormatError;

            return RequestRate(f->mSampleRate);
        }
    }

    return kAudioHardwareUnknownPropertyError;
}

static OSStatus Drv_StartIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 clientID)
{
    (void) d; (void) clientID;
    if (id != kObjectID_Device)
        return kAudioHardwareBadObjectError;

    pthread_mutex_lock(&gStateMutex);
    if (gIOClients == 0)
    {
        gNumberTimeStamps = 0;
        gAnchorHostTime = mach_absolute_time();
        memset(gRing, 0, sizeof(gRing));
    }
    ++gIOClients;
    pthread_mutex_unlock(&gStateMutex);
    return kAudioHardwareNoError;
}

static OSStatus Drv_StopIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 clientID)
{
    (void) d; (void) clientID;
    if (id != kObjectID_Device)
        return kAudioHardwareBadObjectError;

    pthread_mutex_lock(&gStateMutex);
    if (gIOClients > 0)
        --gIOClients;
    pthread_mutex_unlock(&gStateMutex);
    return kAudioHardwareNoError;
}

// The device clock: a "zero" timestamp every kZeroTimeStampPeriod frames,
// derived from the machine's high-resolution timer.
static OSStatus Drv_GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 clientID,
                                     Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed)
{
    (void) d; (void) clientID;
    if (id != kObjectID_Device)
        return kAudioHardwareBadObjectError;

    pthread_mutex_lock(&gStateMutex);

    UInt64 now = mach_absolute_time();
    Float64 ticksPerPeriod = gHostTicksPerFrame * (Float64) kZeroTimeStampPeriod;
    Float64 nextOffset = ((Float64) gNumberTimeStamps + 1.0) * ticksPerPeriod;
    UInt64 nextHostTime = gAnchorHostTime + (UInt64) nextOffset;

    if (nextHostTime <= now)
        ++gNumberTimeStamps;

    *outSampleTime = (Float64) (gNumberTimeStamps * (UInt64) kZeroTimeStampPeriod);
    *outHostTime = gAnchorHostTime + (UInt64) ((Float64) gNumberTimeStamps * ticksPerPeriod);
    *outSeed = 1;

    pthread_mutex_unlock(&gStateMutex);
    return kAudioHardwareNoError;
}

static OSStatus Drv_WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 clientID,
                                      UInt32 opID, Boolean* outWillDo, Boolean* outWillDoInPlace)
{
    (void) d; (void) clientID;
    if (id != kObjectID_Device)
        return kAudioHardwareBadObjectError;

    *outWillDo = (opID == kAudioServerPlugInIOOperationReadInput
                  || opID == kAudioServerPlugInIOOperationWriteMix);
    *outWillDoInPlace = true;
    return kAudioHardwareNoError;
}

static OSStatus Drv_BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 clientID,
                                     UInt32 opID, UInt32 frames, const AudioServerPlugInIOCycleInfo* info)
{
    (void) d; (void) id; (void) clientID; (void) opID; (void) frames; (void) info;
    return kAudioHardwareNoError;
}

// Output side writes into the ring; input side reads out of it and clears what
// it read, so that silence (not stale audio) plays when the DAW stops.
static OSStatus Drv_DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, AudioObjectID stream,
                                  UInt32 clientID, UInt32 opID, UInt32 frames,
                                  const AudioServerPlugInIOCycleInfo* info,
                                  void* ioMainBuffer, void* ioSecondaryBuffer)
{
    (void) d; (void) clientID; (void) ioSecondaryBuffer;
    if (id != kObjectID_Device)
        return kAudioHardwareBadObjectError;
    if (frames == 0 || frames > kRingFrames)
        return kAudioHardwareNoError;

    if (opID == kAudioServerPlugInIOOperationWriteMix && stream == kObjectID_Stream_Output)
    {
        UInt64 start = ((UInt64) info->mOutputTime.mSampleTime) % kRingFrames;
        UInt32 first = (UInt32) (kRingFrames - start);
        if (first > frames)
            first = frames;

        const Float32* src = (const Float32*) ioMainBuffer;
        memcpy(&gRing[start * kChannels], src, first * kBytesPerFrame);
        if (frames > first)
            memcpy(&gRing[0], src + (size_t) first * kChannels, (frames - first) * kBytesPerFrame);
    }
    else if (opID == kAudioServerPlugInIOOperationReadInput && stream == kObjectID_Stream_Input)
    {
        UInt64 start = ((UInt64) info->mInputTime.mSampleTime) % kRingFrames;
        UInt32 first = (UInt32) (kRingFrames - start);
        if (first > frames)
            first = frames;

        Float32* dst = (Float32*) ioMainBuffer;
        memcpy(dst, &gRing[start * kChannels], first * kBytesPerFrame);
        memset(&gRing[start * kChannels], 0, first * kBytesPerFrame);
        if (frames > first)
        {
            memcpy(dst + (size_t) first * kChannels, &gRing[0], (frames - first) * kBytesPerFrame);
            memset(&gRing[0], 0, (frames - first) * kBytesPerFrame);
        }
    }

    return kAudioHardwareNoError;
}

static OSStatus Drv_EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 clientID,
                                   UInt32 opID, UInt32 frames, const AudioServerPlugInIOCycleInfo* info)
{
    (void) d; (void) id; (void) clientID; (void) opID; (void) frames; (void) info;
    return kAudioHardwareNoError;
}

//==============================================================================
static AudioServerPlugInDriverInterface gDriverInterface =
{
    ._reserved = NULL,
    .QueryInterface = Drv_QueryInterface,
    .AddRef = Drv_AddRef,
    .Release = Drv_Release,
    .Initialize = Drv_Initialize,
    .CreateDevice = Drv_CreateDevice,
    .DestroyDevice = Drv_DestroyDevice,
    .AddDeviceClient = Drv_AddDeviceClient,
    .RemoveDeviceClient = Drv_RemoveDeviceClient,
    .PerformDeviceConfigurationChange = Drv_PerformDeviceConfigurationChange,
    .AbortDeviceConfigurationChange = Drv_AbortDeviceConfigurationChange,
    .HasProperty = Drv_HasProperty,
    .IsPropertySettable = Drv_IsPropertySettable,
    .GetPropertyDataSize = Drv_GetPropertyDataSize,
    .GetPropertyData = Drv_GetPropertyData,
    .SetPropertyData = Drv_SetPropertyData,
    .StartIO = Drv_StartIO,
    .StopIO = Drv_StopIO,
    .GetZeroTimeStamp = Drv_GetZeroTimeStamp,
    .WillDoIOOperation = Drv_WillDoIOOperation,
    .BeginIOOperation = Drv_BeginIOOperation,
    .DoIOOperation = Drv_DoIOOperation,
    .EndIOOperation = Drv_EndIOOperation
};

static AudioServerPlugInDriverInterface* gDriverInterfacePtr = &gDriverInterface;
static AudioServerPlugInDriverRef gDriverRef = &gDriverInterfacePtr;

static HRESULT Drv_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface)
{
    if (inDriver != gDriverRef)
        return kAudioHardwareBadObjectError;

    HRESULT result = E_NOINTERFACE;
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
    if (requested == NULL)
        return kAudioHardwareIllegalOperationError;

    if (CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID))
    {
        pthread_mutex_lock(&gStateMutex);
        ++gRefCount;
        pthread_mutex_unlock(&gStateMutex);
        *outInterface = gDriverRef;
        result = S_OK;
    }
    CFRelease(requested);
    return result;
}

static ULONG Drv_AddRef(void* inDriver)
{
    if (inDriver != gDriverRef)
        return 0;
    pthread_mutex_lock(&gStateMutex);
    if (gRefCount < 0xFFFFFFFFu)
        ++gRefCount;
    ULONG r = gRefCount;
    pthread_mutex_unlock(&gStateMutex);
    return r;
}

static ULONG Drv_Release(void* inDriver)
{
    if (inDriver != gDriverRef)
        return 0;
    pthread_mutex_lock(&gStateMutex);
    if (gRefCount > 0)
        --gRefCount;
    ULONG r = gRefCount;
    pthread_mutex_unlock(&gStateMutex);
    return r;
}

// Factory function named in Info.plist; coreaudiod calls this to load the driver.
void* OpenRackBridge_Create(CFAllocatorRef allocator, CFUUIDRef requestedTypeUUID)
{
    (void) allocator;
    if (CFEqual(requestedTypeUUID, kAudioServerPlugInTypeUUID))
        return gDriverRef;
    return NULL;
}
