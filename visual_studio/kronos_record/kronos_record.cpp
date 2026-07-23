#include <windows.h>
#include <stdio.h>
#include <conio.h>
#include <iostream>
#include "BlueVelvetC.h"
#include "BlueVelvetCUtils.h"
#include "BlueVelvetCConversion.h"
#include "BlueHANC.h"

#define BUFFER_COUNT 8
#define MAX_BUFFER_SIZE_VIDEO (4096 * 2160 * 8)

/////////////////////////CONFIG SETTINGS///////////////////////////
const bool USE_DUAL_INPUT = true;  // Set to false for single input channel from first below
const EBlueVideoChannel INPUT_CHANNEL_1 = BLUE_VIDEO_INPUT_CHANNEL_3;
const EBlueVideoChannel INPUT_CHANNEL_2 = BLUE_VIDEO_INPUT_CHANNEL_4;
const EBlueVideoChannel OUTPUT_CHANNEL_1 = BLUE_VIDEO_OUTPUT_CHANNEL_3;
const EBlueVideoChannel OUTPUT_CHANNEL_2 = BLUE_VIDEO_OUTPUT_CHANNEL_4;
///////////////////////////////////////////////////////////////////

void PlaybackFrame(BLUEVELVETC_HANDLE hBvcOutput1, BLUEVELVETC_HANDLE hBvcOutput2,
    void* pVideoBuffer1, void* pVideoBuffer2, size_t BufferSize, bool isDualInput)
{
    static BLUE_S32 BufferId = 0;
    BufferId = (BufferId + 1) % 4;

    EDMADataType DMAImageType = BLUE_DMA_DATA_TYPE_IMAGE_FIELD;

    // If in single input mode, use the same buffer for both outputs
    void* buffer2 = isDualInput ? pVideoBuffer2 : pVideoBuffer1;

    // Write to output channels
    bfcDmaWriteToCardAsync(hBvcOutput1, (BLUE_U8*)pVideoBuffer1, BufferSize,
        nullptr, BlueImage_DMABuffer(BufferId, DMAImageType), 0);
    bfcDmaWriteToCardAsync(hBvcOutput2, (BLUE_U8*)buffer2, BufferSize,
        nullptr, BlueImage_DMABuffer(BufferId, DMAImageType), 0);

    // Update both output channels
    bfcRenderBufferUpdate(hBvcOutput1, BlueBuffer_Image(BufferId));
    bfcRenderBufferUpdate(hBvcOutput2, BlueBuffer_Image(BufferId));
}

int main()
{
    printf("BlueVelvetC version: %s\n", bfcGetVersion());

    // Check for available devices
    BLUE_S32 DeviceCount = bfcUtilsGetDeviceCount();
    if (DeviceCount < 1) {
        printf("No Bluefish card detected\n");
        return 1;
    }

    // Create handles
    BLUEVELVETC_HANDLE hBvcInput1 = bfcFactory();
    BLUEVELVETC_HANDLE hBvcInput2 = USE_DUAL_INPUT ? bfcFactory() : nullptr;
    BLUEVELVETC_HANDLE hBvcOutput1 = bfcFactory();
    BLUEVELVETC_HANDLE hBvcOutput2 = bfcFactory();

    // Setup input 1
    blue_setup_info InputSetup1 = bfcUtilsGetDefaultSetupInfoInput(INPUT_CHANNEL_1);
    InputSetup1.UpdateMethod = UPD_FMT_FIELD;
    InputSetup1.MemoryFormat = MEM_FMT_V210;
    InputSetup1.VideoEngine = VIDEO_ENGINE_AUTO_CAPTURE;

    // Setup input 2 if in dual input mode
    blue_setup_info InputSetup2;
    if (USE_DUAL_INPUT) {
        InputSetup2 = bfcUtilsGetDefaultSetupInfoInput(INPUT_CHANNEL_2);
        InputSetup2.UpdateMethod = UPD_FMT_FIELD;
        InputSetup2.MemoryFormat = MEM_FMT_V210;
        InputSetup2.VideoEngine = VIDEO_ENGINE_AUTO_CAPTURE;
    }

    // Get recommended setup with UHD preference for both inputs
    BErr Ret = bfcUtilsGetRecommendedSetupInfoInput(hBvcInput1, &InputSetup1, UHD_PREFERENCE_BASED_ON_VPID);
    if (BLUE_FAIL(Ret)) {
        printf("Error: Input 1 setup failed with %s\n", bfcUtilsGetStringForBErr(Ret));
        return 1;
    }

    if (USE_DUAL_INPUT) {
        Ret = bfcUtilsGetRecommendedSetupInfoInput(hBvcInput2, &InputSetup2, UHD_PREFERENCE_BASED_ON_VPID);
        if (BLUE_FAIL(Ret)) {
            printf("Error: Input 2 setup failed with %s\n", bfcUtilsGetStringForBErr(Ret));
            return 1;
        }
    }

    // Setup outputs
    blue_setup_info OutputConfig1 = bfcUtilsGetDefaultSetupInfoOutput(OUTPUT_CHANNEL_1, InputSetup1.VideoModeExt);
    blue_setup_info OutputConfig2 = bfcUtilsGetDefaultSetupInfoOutput(OUTPUT_CHANNEL_2, InputSetup1.VideoModeExt);

    OutputConfig1.MemoryFormat = OutputConfig2.MemoryFormat = MEM_FMT_V210;
    OutputConfig1.UpdateMethod = OutputConfig2.UpdateMethod = UPD_FMT_FIELD;
    OutputConfig1.Transport = OutputConfig2.Transport = HD_SDI_TRANSPORT_1_5G;


    // Initialize the devices
    if (BLUE_FAIL(bfcUtilsSetupInput(hBvcInput1, &InputSetup1)) ||
        (USE_DUAL_INPUT && BLUE_FAIL(bfcUtilsSetupInput(hBvcInput2, &InputSetup2))) ||
        BLUE_FAIL(bfcUtilsSetupOutput(hBvcOutput1, &OutputConfig1)) ||
        BLUE_FAIL(bfcUtilsSetupOutput(hBvcOutput2, &OutputConfig2))) {
        printf("Error: Device setup failed\n");
        return 1;
    }

    // Create internal buffers for auto-capture
    Ret = bfcAutoCaptureCreateInternalBuffers(hBvcInput1,
        EBlueBufferComponent(BUFFER_COMPONENT_VIDEO | BUFFER_COMPONENT_VANC | BUFFER_COMPONENT_HANC));
    if (BLUE_FAIL(Ret)) {
        printf("Error: Failed to create capture buffers for input 1\n");
        return 1;
    }

    if (USE_DUAL_INPUT) {
        Ret = bfcAutoCaptureCreateInternalBuffers(hBvcInput2,
            EBlueBufferComponent(BUFFER_COMPONENT_VIDEO | BUFFER_COMPONENT_VANC | BUFFER_COMPONENT_HANC));
        if (BLUE_FAIL(Ret)) {
            printf("Error: Failed to create capture buffers for input 2\n");
            return 1;
        }
    }

    // Start capture
    if (BLUE_FAIL(bfcVideoCaptureStart(hBvcInput1)) ||
        (USE_DUAL_INPUT && BLUE_FAIL(bfcVideoCaptureStart(hBvcInput2)))) {
        printf("Error: Failed to start capture\n");
        return 1;
    }

    printf("Capturing... press any key to stop\n");

    // Main capture loop
    blue_auto_buffer_info BufferInfo1 = {};
    blue_auto_buffer_info BufferInfo2 = {};
    while (!_kbhit()) {
        BufferInfo1.CardBufferId = -1;
        BufferInfo2.CardBufferId = -1;

        // Capture from input 1
        Ret = bfcAutoCaptureGetFilledBuffer(hBvcInput1, &BufferInfo1, RETURN_MODE_BLOCKING);

        std::cout << "Video pixel format: " << BufferInfo1.VideoPixelFormat << std::endl;

        // Capture from input 2 if in dual input mode
        if (USE_DUAL_INPUT) {
            Ret = bfcAutoCaptureGetFilledBuffer(hBvcInput2, &BufferInfo2, RETURN_MODE_BLOCKING);
        }

        if (BLUE_OK(Ret) && (BufferInfo1.CardBufferId >= 0)) {
            if (!BufferInfo1.BufferIncomplete && BufferInfo1.pBufferVideo) {
                // Play captured frames on output channels
                PlaybackFrame(hBvcOutput1, hBvcOutput2,
                    BufferInfo1.pBufferVideo,
                    USE_DUAL_INPUT ? BufferInfo2.pBufferVideo : nullptr,
                    BufferInfo1.SizeVideo,
                    USE_DUAL_INPUT);
            }

            bfcAutoCaptureReturnBuffer(hBvcInput1, &BufferInfo1);
            if (USE_DUAL_INPUT) {
                bfcAutoCaptureReturnBuffer(hBvcInput2, &BufferInfo2);
            }
        }
    }

    // Cleanup
    bfcVideoCaptureStop(hBvcInput1);
    if (USE_DUAL_INPUT) {
        bfcVideoCaptureStop(hBvcInput2);
    }

    bfcAutoCaptureDestroyInternalBuffers(hBvcInput1);
    if (USE_DUAL_INPUT) {
        bfcAutoCaptureDestroyInternalBuffers(hBvcInput2);
    }

    bfcDestroy(hBvcInput1);
    if (USE_DUAL_INPUT) {
        bfcDestroy(hBvcInput2);
    }
    bfcDestroy(hBvcOutput1);
    bfcDestroy(hBvcOutput2);

    printf("Capture stopped\n");
    return 0;
}