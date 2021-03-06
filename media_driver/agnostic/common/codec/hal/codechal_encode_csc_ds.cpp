/*
* Copyright (c) 2017, Intel Corporation
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included
* in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
* OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
* OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
* ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
* OTHER DEALINGS IN THE SOFTWARE.
*/
//!
//! \file     codechal_encode_csc_ds.cpp
//! \brief    Defines base class for CSC and Downscaling
//!

#include "codechal_encoder.h"
#include "codechal_encoder_base.h"
#include "codechal_encode_csc_ds.h"

MOS_STATUS CodechalEncodeCscDs::AllocateSurfaceCsc()
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    MOS_STATUS eStatus = MOS_STATUS_SUCCESS;

    if (!m_cscFlag)
    {
        return eStatus;
    }

    m_cscBufCurrIdx = LookUpBufSlot();

    CODECHAL_ENCODE_CHK_COND_RETURN(m_cscBufCurrIdx >= CODEC_NUM_TRACKED_BUFFERS, "No CSC buffer is available!");

    auto trackedBuffer = &m_trackedBuffer[m_cscBufCurrIdx];

    if (!Mos_ResourceIsNull(&trackedBuffer->sCopiedSurface.OsResource))
    {
        return eStatus;
    }

    // initiate allocation paramters and lock flags
    MOS_ALLOC_GFXRES_PARAMS allocParamsNV12;
    MOS_ZeroMemory(&allocParamsNV12, sizeof(allocParamsNV12));
    allocParamsNV12.Type = MOS_GFXRES_2D;
    allocParamsNV12.TileType = MOS_TILE_Y;
    allocParamsNV12.pBufName = "Y Tile Surface for DS+Copy Kernel";

    uint32_t surfaceWidth, surfaceHeight;
    if (m_mode == CODECHAL_ENCODE_MODE_HEVC)
    {
        // The raw input surface to HEVC Enc should be 32 aligned because of VME hardware restriction as mentioned in DDI.
        surfaceWidth = MOS_ALIGN_CEIL(m_oriFrameWidth, 32);
        surfaceHeight = MOS_ALIGN_CEIL(m_oriFrameHeight, 32);
    }
    else
    {
        surfaceWidth = MOS_ALIGN_CEIL(m_oriFrameWidth, m_rawSurfAlignment);
        surfaceHeight = MOS_ALIGN_CEIL(m_oriFrameHeight, m_rawSurfAlignment);
    }

    if (cscColorY210 == m_colorRawSurface)
    {
        allocParamsNV12.Format = Format_YUY2;
        allocParamsNV12.dwWidth = surfaceWidth;
        allocParamsNV12.dwHeight = surfaceHeight << 1;
    }
    else
    {
        allocParamsNV12.Format = Format_NV12;
        allocParamsNV12.dwWidth = surfaceWidth;
        allocParamsNV12.dwHeight = surfaceHeight;
    }

    CODECHAL_ENCODE_CHK_STATUS_MESSAGE_RETURN(m_osInterface->pfnAllocateResource(
        m_osInterface,
        &allocParamsNV12,
        &trackedBuffer->sCopiedSurface.OsResource),
        "Failed to allocate Format converted Surface for Csc+Ds+Conversioin Kernel!");

    CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHal_GetResourceInfo(
        m_osInterface,
        &trackedBuffer->sCopiedSurface));

    if (Format_YUY2 == allocParamsNV12.Format)
    {
        if (cscColorYUY2 == m_colorRawSurface &&
            (uint8_t)HCP_CHROMA_FORMAT_YUV422 == m_outputChromaFormat)
        {
            trackedBuffer->sCopiedSurface.Format = Format_YUY2V;
        }
        else if (cscColorY210 == m_colorRawSurface)
        {
            trackedBuffer->sCopiedSurface.Format = Format_Y216V;
        }
        trackedBuffer->sCopiedSurface.dwWidth = surfaceWidth;
        trackedBuffer->sCopiedSurface.dwHeight = surfaceHeight;
    }

    return eStatus;
}

void CodechalEncodeCscDs::ReleaseSurfaceCsc(uint8_t index)
{
    // free CSC surface
    m_osInterface->pfnFreeResource(m_osInterface, &m_trackedBuffer[index].sCopiedSurface.OsResource);
}

MOS_STATUS CodechalEncodeCscDs::AllocateSurfaceDS()
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    MOS_STATUS eStatus = MOS_STATUS_SUCCESS;

    auto trackedBuffer = &m_trackedBuffer[m_currScalingIdx];

    if (!Mos_ResourceIsNull(&trackedBuffer->sScaled4xSurface.OsResource))
    {
        return eStatus;
    }

    // initiate allocation paramters
    MOS_ALLOC_GFXRES_PARAMS allocParamsForBufferNV12;
    MOS_ZeroMemory(&allocParamsForBufferNV12, sizeof(MOS_ALLOC_GFXRES_PARAMS));
    allocParamsForBufferNV12.Type = MOS_GFXRES_2D;
    allocParamsForBufferNV12.TileType = MOS_TILE_Y;
    allocParamsForBufferNV12.Format = Format_NV12;

    uint32_t downscaledSurfaceWidth4x, downscaledSurfaceHeight4x;
    if (m_useCommonKernel)
    {
        downscaledSurfaceWidth4x = CODECHAL_GET_4xDS_SIZE_32ALIGNED(m_frameWidth);
        downscaledSurfaceHeight4x = CODECHAL_GET_4xDS_SIZE_32ALIGNED(m_frameHeight);
    }
    else
    {
        // MB-alignment not required since dataport handles out-of-bound pixel replication, but IME requires this.
        downscaledSurfaceWidth4x = m_downscaledWidth4x;
        // Account for field case, offset needs to be 4K aligned if tiled for DI surface state.
        // Width will be allocated tile Y aligned, so also tile align height.
        downscaledSurfaceHeight4x = ((m_downscaledHeight4x/CODECHAL_MACROBLOCK_HEIGHT + 1) >> 1) * CODECHAL_MACROBLOCK_HEIGHT;
        downscaledSurfaceHeight4x = MOS_ALIGN_CEIL(downscaledSurfaceHeight4x, MOS_YTILE_H_ALIGNMENT) << 1;
    }

    allocParamsForBufferNV12.dwWidth = downscaledSurfaceWidth4x;
    allocParamsForBufferNV12.dwHeight = downscaledSurfaceHeight4x;
    allocParamsForBufferNV12.pBufName = "4x Scaled Surface";

    // allocate 4x DS surface
    CODECHAL_ENCODE_CHK_STATUS_MESSAGE_RETURN(m_osInterface->pfnAllocateResource(
        m_osInterface,
        &allocParamsForBufferNV12,
        &trackedBuffer->sScaled4xSurface.OsResource),
        "Failed to allocate 4xScaled surface.");

    CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHal_GetResourceInfo(
        m_osInterface,
        &trackedBuffer->sScaled4xSurface));

    // allocate 16x DS surface
    uint32_t downscaledSurfaceWidth16x, downscaledSurfaceHeight16x;
    if (m_16xMeSupported)
    {
        if (m_useCommonKernel)
        {
            downscaledSurfaceWidth16x = CODECHAL_GET_4xDS_SIZE_32ALIGNED(downscaledSurfaceWidth4x);
            downscaledSurfaceHeight16x = CODECHAL_GET_4xDS_SIZE_32ALIGNED(downscaledSurfaceHeight4x);
        }
        else
        {
            downscaledSurfaceWidth16x = m_downscaledWidth16x;
            // Account for field case, offset needs to be 4K aligned if tiled for DI surface state.
            // Width will be allocated tile Y aligned, so also tile align height.
            downscaledSurfaceHeight16x = ((m_downscaledHeight16x/CODECHAL_MACROBLOCK_HEIGHT + 1) >> 1) * CODECHAL_MACROBLOCK_HEIGHT;
            downscaledSurfaceHeight16x = MOS_ALIGN_CEIL(downscaledSurfaceHeight16x, MOS_YTILE_H_ALIGNMENT) << 1;
        }
        allocParamsForBufferNV12.dwWidth = downscaledSurfaceWidth16x;
        allocParamsForBufferNV12.dwHeight = downscaledSurfaceHeight16x;
        allocParamsForBufferNV12.pBufName = "16x Scaled Surface";

        CODECHAL_ENCODE_CHK_STATUS_MESSAGE_RETURN(m_osInterface->pfnAllocateResource(
            m_osInterface,
            &allocParamsForBufferNV12,
            &trackedBuffer->sScaled16xSurface.OsResource),
            "Failed to allocate 16xScaled surface.");

        CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHal_GetResourceInfo(
            m_osInterface,
            &trackedBuffer->sScaled16xSurface));
    }

    // allocate 32x DS surface
    if (m_32xMeSupported)
    {
        uint32_t downscaledSurfaceWidth32x, downscaledSurfaceHeight32x;
        if (m_useCommonKernel)
        {
            downscaledSurfaceWidth16x = CODECHAL_GET_4xDS_SIZE_32ALIGNED(downscaledSurfaceWidth4x);
            downscaledSurfaceHeight16x = CODECHAL_GET_4xDS_SIZE_32ALIGNED(downscaledSurfaceHeight4x);
            downscaledSurfaceWidth32x = CODECHAL_GET_2xDS_SIZE_32ALIGNED(downscaledSurfaceWidth16x);
            downscaledSurfaceHeight32x = CODECHAL_GET_2xDS_SIZE_32ALIGNED(downscaledSurfaceHeight16x);
        }
        else
        {
            downscaledSurfaceWidth32x = m_downscaledWidth32x;
            // Account for field case, offset needs to be 4K aligned if tiled for DI surface state.
            // Width will be allocated tile Y aligned, so also tile align height.
            downscaledSurfaceHeight32x = ((m_downscaledHeight32x/CODECHAL_MACROBLOCK_HEIGHT + 1) >> 1) * CODECHAL_MACROBLOCK_HEIGHT;
            downscaledSurfaceHeight32x = MOS_ALIGN_CEIL(downscaledSurfaceHeight32x, MOS_YTILE_H_ALIGNMENT) << 1;
        }
        allocParamsForBufferNV12.dwWidth = downscaledSurfaceWidth32x;
        allocParamsForBufferNV12.dwHeight = downscaledSurfaceHeight32x;
        allocParamsForBufferNV12.pBufName = "32x Scaled Surface";

        CODECHAL_ENCODE_CHK_STATUS_MESSAGE_RETURN(m_osInterface->pfnAllocateResource(
            m_osInterface,
            &allocParamsForBufferNV12,
            &trackedBuffer->sScaled32xSurface.OsResource),
            "Failed to allocate 32xScaled surface.");

        CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHal_GetResourceInfo(
            m_osInterface,
            &trackedBuffer->sScaled32xSurface));
    }

    if (!m_fieldScalingOutputInterleaved)
    {
        // Separated scaled surfaces
        // Height should be 4K aligned for DI surface state, assume always Y tiled
        m_scaledBottomFieldOffset = MOS_ALIGN_CEIL(
            (trackedBuffer->sScaled4xSurface.dwPitch *
            (trackedBuffer->sScaled4xSurface.dwHeight / 2)), CODECHAL_PAGE_SIZE);

        if (m_16xMeSupported)
        {
            // Height should be 4K aligned for DI surface state, assume always Y tiled
            m_scaled16xBottomFieldOffset = MOS_ALIGN_CEIL(
                (trackedBuffer->sScaled16xSurface.dwPitch *
                (trackedBuffer->sScaled16xSurface.dwHeight / 2)), CODECHAL_PAGE_SIZE);
        }

        if (m_32xMeSupported)
        {
            // Height should be 4K aligned for DI surface state, assume always Y tiled
            m_scaled32xBottomFieldOffset = MOS_ALIGN_CEIL(
                (trackedBuffer->sScaled32xSurface.dwPitch *
                (trackedBuffer->sScaled32xSurface.dwHeight / 2)), CODECHAL_PAGE_SIZE);
        }

    }
    else
    {
        // Interleaved scaled surfaces
        m_scaledBottomFieldOffset =
        m_scaled16xBottomFieldOffset =
        m_scaled32xBottomFieldOffset = 0;
    }

    return eStatus;
}

MOS_STATUS CodechalEncodeCscDs::AllocateSurface2xDS()
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    MOS_STATUS eStatus = MOS_STATUS_SUCCESS;

    auto trackedBuffer = &m_trackedBuffer[m_currScalingIdx];

    if (!Mos_ResourceIsNull(&trackedBuffer->sScaled2xSurface.OsResource))
    {
        return eStatus;
    }

    // initiate allocation paramters
    MOS_ALLOC_GFXRES_PARAMS allocParamsForBufferNV12;
    MOS_ZeroMemory(&allocParamsForBufferNV12, sizeof(MOS_ALLOC_GFXRES_PARAMS));
    allocParamsForBufferNV12.Type = MOS_GFXRES_2D;
    allocParamsForBufferNV12.TileType = MOS_TILE_Y;
    allocParamsForBufferNV12.Format = Format_NV12;

    uint32_t surfaceWidth, surfaceHeight;
    if (m_useCommonKernel)
    {
        surfaceWidth = CODECHAL_GET_2xDS_SIZE_32ALIGNED(m_frameWidth);
        surfaceHeight = CODECHAL_GET_2xDS_SIZE_32ALIGNED(m_frameHeight);
    }
    else
    {
        surfaceWidth = MOS_ALIGN_CEIL(m_frameWidth, 64) >> 1;
        surfaceHeight = MOS_ALIGN_CEIL(m_frameHeight, 64) >> 1;
    }

    if ((uint8_t)HCP_CHROMA_FORMAT_YUV422 == m_outputChromaFormat)
    {
        allocParamsForBufferNV12.Format = Format_YUY2;
        allocParamsForBufferNV12.dwWidth = surfaceWidth >> 1;
        allocParamsForBufferNV12.dwHeight = surfaceHeight << 1;
    }
    else
    {
        allocParamsForBufferNV12.dwWidth = surfaceWidth;
        allocParamsForBufferNV12.dwHeight = surfaceHeight;
    }
    allocParamsForBufferNV12.pBufName = "2x Scaled Surface";

    // allocate 2x DS surface
    CODECHAL_ENCODE_CHK_STATUS_MESSAGE_RETURN(m_osInterface->pfnAllocateResource(
        m_osInterface,
        &allocParamsForBufferNV12,
        &trackedBuffer->sScaled2xSurface.OsResource),
        "Failed to allocate 2xScaled surface.");

    CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHal_GetResourceInfo(
        m_osInterface,
        &trackedBuffer->sScaled2xSurface));

    if ((uint8_t)HCP_CHROMA_FORMAT_YUV422 == m_outputChromaFormat)
    {
        trackedBuffer->sScaled2xSurface.Format = Format_YUY2V;
        trackedBuffer->sScaled2xSurface.dwWidth = surfaceWidth;
        trackedBuffer->sScaled2xSurface.dwHeight = surfaceHeight;
    }

    return eStatus;
}

void CodechalEncodeCscDs::ReleaseSurfaceDS(uint8_t index)
{
    // free DS surface
    m_osInterface->pfnFreeResource(m_osInterface, &m_trackedBuffer[index].sScaled2xSurface.OsResource);
    m_osInterface->pfnFreeResource(m_osInterface, &m_trackedBuffer[index].sScaled4xSurface.OsResource);
    m_osInterface->pfnFreeResource(m_osInterface, &m_trackedBuffer[index].sScaled16xSurface.OsResource);
    m_osInterface->pfnFreeResource(m_osInterface, &m_trackedBuffer[index].sScaled32xSurface.OsResource);
}

uint8_t CodechalEncodeCscDs::LookUpBufSlot()
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    if (m_cscBufCountResize)
    {
        m_cscBufCountResize--;

        if (m_cscBufAnteIdx != m_cscBufPenuIdx && m_cscBufAnteIdx != m_cscBufLastIdx)
        {
            ReleaseSurfaceCsc(m_cscBufAnteIdx);
            CODECHAL_ENCODE_NORMALMESSAGE("CSC buffer = %d re-allocated", m_cscBufAnteIdx);
        }
    }

    uint8_t index = PICTURE_MAX_7BITS;
    if (m_useRawForRef)
    {
        index = m_currScalingIdx;
        m_waitCscSurf = false;
    }
    else
    {
        // if Raw won't be used as Ref, can use a ring buffer
        if (!m_waitForPak)
        {
            m_cscBufCountNonRef += m_cscBufCountNonRef <= CODEC_NUM_NON_REF_BUFFERS;
            CODECHAL_ENCODE_NORMALMESSAGE("CSC buffer count = %d", m_cscBufCountNonRef);
        }
        else
        {
            m_cscBufCountNonRef = 0;
        }
        m_waitCscSurf = m_cscBufCountNonRef > CODEC_NUM_NON_REF_BUFFERS;

        m_cscBufRingIdx = (m_cscBufRingIdx + 1) % CODEC_NUM_NON_REF_BUFFERS;
        index = CODEC_NUM_REF_BUFFERS + m_cscBufRingIdx;
    }

    // update the last 3 buffer index
    m_cscBufAnteIdx = m_cscBufPenuIdx;
    m_cscBufPenuIdx = m_cscBufLastIdx;
    m_cscBufLastIdx = index;

    return index;
}

MOS_STATUS CodechalEncodeCscDs::CheckRawColorFormat(MOS_FORMAT format)
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    MOS_STATUS eStatus = MOS_STATUS_SUCCESS;

    // check input color format, and set target traverse thread space size
    switch (format)
    {
    case Format_NV12:
        m_colorRawSurface = cscColorNv12Linear;
        m_cscRequireColor = 1;
        m_threadTraverseSizeX = 5;    // for NV12, thread space is 32x4
        break;
    case Format_YUY2:
    case Format_YUYV:
        m_colorRawSurface = cscColorYUY2;
        m_cscRequireColor = (uint8_t)HCP_CHROMA_FORMAT_YUV420 == m_outputChromaFormat;
        m_cscRequireConvTo8bPlanar = (uint8_t)HCP_CHROMA_FORMAT_YUV422 == m_outputChromaFormat;
        m_threadTraverseSizeX = 4;    // for YUY2, thread space is 16x4
        break;
    case Format_A8R8G8B8:
        m_colorRawSurface = cscColorARGB;
        m_cscRequireColor = 1;
        m_cscUsingSfc = m_cscEnableSfc ? 1 : 0;
        // Use EU for better performance in big resolution cases or TU1
        if ((m_cscRawSurfWidth * m_cscRawSurfHeight > 1920 * 1088) || m_16xMeSupported)
        {
            m_cscUsingSfc = 0;
        }
        m_threadTraverseSizeX = 3;    // for ARGB, thread space is 8x4
        break;
    case Format_A8B8G8R8:
        m_colorRawSurface = cscColorABGR;
        m_cscRequireColor = 1;
        m_threadTraverseSizeX = 3;    // for ABGR, thread space is 8x4
        break;
    case Format_P010:
        m_colorRawSurface = cscColorP010;
        m_cscRequireConvTo8bPlanar = 1;
        break;
    default:
        CODECHAL_ENCODE_ASSERTMESSAGE("Input color format = %d not supported!", format);
        eStatus = MOS_STATUS_INVALID_PARAMETER;
        break;
    }

    return eStatus;
}

MOS_STATUS CodechalEncodeCscDs::InitSfcState()
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    return CodecHalEncodeSfc_Initialize(m_hwInterface, m_osInterface);
}

MOS_STATUS CodechalEncodeCscDs::SetParamsSfc(CODECHAL_ENCODE_SFC_PARAMS* sfcParams)
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    MOS_STATUS eStatus = MOS_STATUS_SUCCESS;

    CODECHAL_ENCODE_CHK_NULL_RETURN(sfcParams);

    // color space parameters have been set to pSfcState already, no need set here
    sfcParams->pInputSurface = m_rawSurfaceToEnc;
    sfcParams->pOutputSurface = &m_trackedBuffer[m_cscBufCurrIdx].sCopiedSurface;
    sfcParams->rcInputSurfaceRegion.X = 0;
    sfcParams->rcInputSurfaceRegion.Y = 0;
    sfcParams->rcInputSurfaceRegion.Width = m_cscRawSurfWidth;
    sfcParams->rcInputSurfaceRegion.Height = m_cscRawSurfHeight;

    sfcParams->rcOutputSurfaceRegion.X = 0;
    sfcParams->rcOutputSurfaceRegion.Y = 0;
    sfcParams->rcOutputSurfaceRegion.Width = m_cscRawSurfWidth;
    sfcParams->rcOutputSurfaceRegion.Height = m_cscRawSurfHeight;

    sfcParams->uiChromaSitingType = MHW_CHROMA_SITING_HORZ_CENTER | MHW_CHROMA_SITING_VERT_CENTER;

    return eStatus;
}

MOS_STATUS CodechalEncodeCscDs::InitKernelStateCsc()
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    MOS_STATUS eStatus = MOS_STATUS_SUCCESS;

    auto kernelHeaderTable = (CscKernelHeader*)m_kernelBase;
    auto currKrnHeader = kernelHeaderTable->header;

    m_cscKernelState->KernelParams.iBTCount = cscNumSurfaces;
    m_cscKernelState->KernelParams.iThreadCount = m_hwInterface->GetRenderInterface()->GetHwCaps()->dwMaxThreads;
    m_cscKernelState->KernelParams.iCurbeLength = m_cscCurbeLength;
    m_cscKernelState->KernelParams.iBlockWidth = CODECHAL_MACROBLOCK_WIDTH;
    m_cscKernelState->KernelParams.iBlockHeight = CODECHAL_MACROBLOCK_HEIGHT;
    m_cscKernelState->KernelParams.iIdCount = 1;
    m_cscKernelState->KernelParams.iInlineDataLength = 0;
    m_cscKernelState->dwCurbeOffset = m_stateHeapInterface->GetSizeofCmdInterfaceDescriptorData();
    m_cscKernelState->KernelParams.pBinary =
        m_kernelBase + (currKrnHeader.KernelStartPointer << MHW_KERNEL_OFFSET_SHIFT);
    m_cscKernelState->KernelParams.iSize = m_combinedKernelSize - (currKrnHeader.KernelStartPointer << MHW_KERNEL_OFFSET_SHIFT);

    CODECHAL_ENCODE_CHK_STATUS_RETURN(m_stateHeapInterface->CalculateSshAndBtSizesRequested(
        m_cscKernelState->KernelParams.iBTCount,
        &m_cscKernelState->dwSshSize,
        &m_cscKernelState->dwBindingTableSize));

    CODECHAL_ENCODE_CHK_NULL_RETURN(m_renderInterface->m_stateHeapInterface);
    CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHal_MhwInitISH(m_renderInterface->m_stateHeapInterface, m_cscKernelState));

    return eStatus;
}

MOS_STATUS CodechalEncodeCscDs::SetKernelParamsCsc(KernelParams* params)
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    MOS_STATUS eStatus = MOS_STATUS_SUCCESS;

    CODECHAL_ENCODE_CHK_NULL_RETURN(params);

    /* calling mode for Ds+Copy kernel and/or 4x DS kernel
    *
    * For Progressive:
    * ------------------------------------------------------------------------------------------------
    *  bScalingEnabled  cscReqdForRawSurf  bFirstField     call Ds+Copy kernel?    call 4x DS kernel?
    * ------------------------------------------------------------------------------------------------
    *        1                  0               1                                        Yes
    *        1                  1               1             COPY_DS mode
    *        0                  0               1
    *        0                  1               1             COPY_ONLY mode
    *
    * For Interlaced:
    *        1                  0               1                                        Yes
    *        1                  1               1             COPY_ONLY mode             Yes, see note 2
    *        0                  0           dont care
    *        0                  1               1             COPY_ONLY mode
    *        0                  1               0             do nothing for 2nd field
    *
    * Note 1: bFirstField must be == 1 when (1) bScalingEnabled == 1, or (2) Progressive content
    * Note 2: so far Ds+Copy kernel does not support Interlaced, so we would have to do a COPY_ONLY, followed by 4x DS
    *         these 2 steps can combine into a single COPY_DS once Interlaced is supported
    */

    m_lastTaskInPhase = params->bLastTaskInPhaseCSC;
    m_currRefList->b4xScalingUsed = m_scalingEnabled;

    // setup Curbe
    m_curbeParams.dwInputPictureWidth = m_cscRawSurfWidth;
    m_curbeParams.dwInputPictureHeight = m_cscRawSurfHeight;
    m_curbeParams.bFlatnessCheckEnabled = m_flatnessCheckEnabled;
    m_curbeParams.bMBVarianceOutputEnabled = m_mbStatsEnabled;
    m_curbeParams.bMBPixelAverageOutputEnabled = m_mbStatsEnabled;
    m_curbeParams.bCscOrCopyOnly = !m_scalingEnabled;
    m_curbeParams.inputColorSpace = params->inputColorSpace;

    // setup surface states
    m_surfaceParamsCsc.psInputSurface = m_rawSurfaceToEnc;
    m_surfaceParamsCsc.psOutputCopiedSurface = &m_trackedBuffer[m_cscBufCurrIdx].sCopiedSurface;
    m_surfaceParamsCsc.psOutput4xDsSurface =
        m_scalingEnabled ? &m_trackedBuffer[m_currScalingIdx].sScaled4xSurface : nullptr;

    if (m_mbStatsSupported)
    {
        m_surfaceParamsCsc.bMBVProcStatsEnabled = true;
        m_surfaceParamsCsc.presMBVProcStatsBuffer = &m_resMbStatsBuffer;
    }
    else
    {
        m_surfaceParamsCsc.bFlatnessCheckEnabled = m_flatnessCheckEnabled;
        m_surfaceParamsCsc.psFlatnessCheckSurface = &m_flatnessCheckSurface;
    }

    // setup walker param
    m_walkerResolutionX = MOS_ROUNDUP_SHIFT(m_downscaledWidth4x, m_threadTraverseSizeX);
    m_walkerResolutionY = MOS_ROUNDUP_SHIFT(m_downscaledHeight4x, m_threadTraverseSizeY);

    return eStatus;
}

MOS_STATUS CodechalEncodeCscDs::SetCurbeCsc()
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    MOS_STATUS eStatus = MOS_STATUS_SUCCESS;

    CscKernelCurbeData curbe;

    curbe.DW0_InputPictureWidth = m_curbeParams.dwInputPictureWidth;
    curbe.DW0_InputPictureHeight = m_curbeParams.dwInputPictureHeight;

    curbe.DW1_SrcNV12SurfYIndex = cscSrcYPlane;
    curbe.DW2_DstYSurfIndex = cscDstDsYPlane;
    curbe.DW3_FlatDstSurfIndex = cscDstFlatOrMbStats;
    curbe.DW4_CopyDstNV12SurfIndex = cscDstCopyYPlane;

    if (m_curbeParams.bCscOrCopyOnly)
    {
        curbe.DW5_CscDsCopyOpCode = 0;    // Copy only
    }
    else
    {
        // Enable DS kernel (0  disable, 1  enable)
        curbe.DW5_CscDsCopyOpCode = 1;    // 0x01 to 0x7F: DS + Copy
    }

    if (cscColorNv12TileY == m_colorRawSurface ||
        cscColorNv12Linear == m_colorRawSurface)
    {
        curbe.DW5_InputColorFormat = 0;
    }
    else if (cscColorYUY2 == m_colorRawSurface)
    {
        curbe.DW5_InputColorFormat = 1;
    }
    else if (cscColorARGB == m_colorRawSurface)
    {
        curbe.DW5_InputColorFormat = 2;
    }

    if (m_curbeParams.bFlatnessCheckEnabled ||
        m_curbeParams.bMBVarianceOutputEnabled ||
        m_curbeParams.bMBPixelAverageOutputEnabled)
    {
        curbe.DW6_FlatnessThreshold = 128;
        curbe.DW7_EnableMBFlatnessCheck = true;
    }
    else
    {
        curbe.DW7_EnableMBFlatnessCheck = false;
    }

    curbe.DW8_SrcNV12SurfUVIndex = cscSrcUVPlane;

    CODECHAL_ENCODE_CHK_STATUS_RETURN(m_cscKernelState->m_dshRegion.AddData(
        &curbe,
        m_cscKernelState->dwCurbeOffset,
        sizeof(curbe)));

    return eStatus;
}

MOS_STATUS CodechalEncodeCscDs::SendSurfaceCsc(PMOS_COMMAND_BUFFER cmdBuffer)
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    MOS_STATUS eStatus = MOS_STATUS_SUCCESS;

    // Source surface/s
    CODECHAL_SURFACE_CODEC_PARAMS surfaceParams;
    MOS_ZeroMemory(&surfaceParams, sizeof(surfaceParams));
    surfaceParams.bIs2DSurface = true; // linear surface is not 2D -> changed kernel
    surfaceParams.bUseUVPlane = (cscColorNv12TileY == m_colorRawSurface ||
        cscColorNv12Linear == m_colorRawSurface);
    surfaceParams.bMediaBlockRW = true;
    surfaceParams.psSurface = m_surfaceParamsCsc.psInputSurface;
    surfaceParams.bUseARGB8Format = true;
    surfaceParams.dwCacheabilityControl =
        m_hwInterface->ComposeSurfaceCacheabilityControl(
            MOS_CODEC_RESOURCE_USAGE_SURFACE_HME_DOWNSAMPLED_ENCODE,
            (codechalL3 | codechalLLC));

    surfaceParams.dwVerticalLineStride = m_verticalLineStride;
    surfaceParams.dwBindingTableOffset = cscSrcYPlane;
    surfaceParams.dwUVBindingTableOffset = cscSrcUVPlane;
    CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHal_SetRcsSurfaceState(
        m_hwInterface,
        cmdBuffer,
        &surfaceParams,
        m_cscKernelState));

    // Destination surface/s - 4x downscaled surface
    if (m_surfaceParamsCsc.psOutput4xDsSurface)
    {
        MOS_ZeroMemory(&surfaceParams, sizeof(surfaceParams));
        surfaceParams.bIs2DSurface =
        surfaceParams.bIsWritable = true;
        surfaceParams.psSurface = m_surfaceParamsCsc.psOutput4xDsSurface;
        surfaceParams.dwCacheabilityControl = m_hwInterface->ComposeSurfaceCacheabilityControl(
            MOS_CODEC_RESOURCE_USAGE_SURFACE_HME_DOWNSAMPLED_ENCODE,
            codechalLLC);
        surfaceParams.dwVerticalLineStride = m_verticalLineStride;
        surfaceParams.dwBindingTableOffset = cscDstDsYPlane;
        CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHal_SetRcsSurfaceState(
            m_hwInterface,
            cmdBuffer,
            &surfaceParams,
            m_cscKernelState));
    }

    // FlatnessCheck or MbStats surface
    MOS_ZeroMemory(&surfaceParams, sizeof(surfaceParams));
    if (m_surfaceParamsCsc.bMBVProcStatsEnabled)
    {
        surfaceParams.bRawSurface =
        surfaceParams.bIsWritable = true;
        surfaceParams.dwSize = CODECHAL_GET_WIDTH_IN_MACROBLOCKS(m_surfaceParamsCsc.psInputSurface->dwWidth) *
            CODECHAL_GET_HEIGHT_IN_MACROBLOCKS(m_surfaceParamsCsc.psInputSurface->dwHeight) * 16 * sizeof(uint32_t);
        surfaceParams.presBuffer = m_surfaceParamsCsc.presMBVProcStatsBuffer;
		surfaceParams.dwCacheabilityControl =
			m_hwInterface->ComposeSurfaceCacheabilityControl(
				MOS_CODEC_RESOURCE_USAGE_MB_STATS_ENCODE,
				codechalLLC | codechalL3);
        surfaceParams.dwBindingTableOffset = cscDstFlatOrMbStats;
        CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHal_SetRcsSurfaceState(
            m_hwInterface,
            cmdBuffer,
            &surfaceParams,
            m_cscKernelState));
    }
    else if (m_surfaceParamsCsc.bFlatnessCheckEnabled)
    {
        surfaceParams.bIs2DSurface =
        surfaceParams.bMediaBlockRW =
        surfaceParams.bIsWritable = true;
        surfaceParams.psSurface = m_surfaceParamsCsc.psFlatnessCheckSurface;
        surfaceParams.dwCacheabilityControl =
            m_hwInterface->ComposeSurfaceCacheabilityControl(
                MOS_CODEC_RESOURCE_USAGE_SURFACE_FLATNESS_CHECK_ENCODE,
                codechalLLC | codechalL3);
        surfaceParams.dwBindingTableOffset = cscDstFlatOrMbStats;
        CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHal_SetRcsSurfaceState(
            m_hwInterface,
            cmdBuffer,
            &surfaceParams,
            m_cscKernelState));
    }

    // copy kernel output luma + chroma
    if (m_surfaceParamsCsc.psOutputCopiedSurface)
    {
        MOS_ZeroMemory(&surfaceParams, sizeof(surfaceParams));
        surfaceParams.bIs2DSurface =
        surfaceParams.bUseUVPlane =
        surfaceParams.bMediaBlockRW =
        surfaceParams.bIsWritable = true;
        surfaceParams.psSurface = m_surfaceParamsCsc.psOutputCopiedSurface;
        surfaceParams.dwCacheabilityControl = m_hwInterface->ComposeSurfaceCacheabilityControl(
            MOS_CODEC_RESOURCE_USAGE_SURFACE_HME_DOWNSAMPLED_ENCODE,
            codechalLLC);
        surfaceParams.dwBindingTableOffset = cscDstCopyYPlane;
        surfaceParams.dwUVBindingTableOffset = cscDstCopyUVPlane;
        CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHal_SetRcsSurfaceState(
            m_hwInterface,
            cmdBuffer,
            &surfaceParams,
            m_cscKernelState));
    }

    return eStatus;
}

MOS_STATUS CodechalEncodeCscDs::SetSurfacesToEncPak()
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    MOS_STATUS eStatus = MOS_STATUS_SUCCESS;

    auto trackedBuffer = &m_trackedBuffer[m_cscBufCurrIdx];

    // now we have the converted surface, do different things
    if (m_cscRequireCopy || m_cscRequireColor || m_cscRequireConvTo8bPlanar)
    {
        m_rawSurfaceToEnc = &trackedBuffer->sCopiedSurface;

        // update the RawBuffer and RefBuffer (if Raw is used as Ref)
        m_currRefList->sRefRawBuffer = trackedBuffer->sCopiedSurface;
        if (m_useRawForRef)
        {
            m_currRefList->sRefBuffer = trackedBuffer->sCopiedSurface;
        }
        CODECHAL_ENCODE_NORMALMESSAGE("Set m_rawSurfaceToEnc %d x %d, CSC buf index = %d",
            m_rawSurfaceToEnc->dwWidth, m_rawSurfaceToEnc->dwHeight, m_cscBufCurrIdx);
    }

    if (m_cscRequireCopy || m_cscRequireColor || m_cscRequireMmc)
    {
        m_rawSurfaceToPak = &trackedBuffer->sCopiedSurface;
        CODECHAL_ENCODE_NORMALMESSAGE("Set m_rawSurfaceToPak %d x %d, CSC buf index = %d",
            m_rawSurfaceToPak->dwWidth, m_rawSurfaceToPak->dwHeight, m_cscBufCurrIdx);
    }

    // dump copied surface from Ds+Copy kernel
    if (m_cscFlag)
    {
        CODECHAL_DEBUG_TOOL( 
            CODECHAL_ENCODE_CHK_STATUS_RETURN(m_debugInterface->DumpYUVSurface(
                &trackedBuffer->sCopiedSurface,
                CodechalDbgAttr::attrEncodeRawInputSurface,
                "Copied_SrcSurf")) // needs to consider YUV420
        )
    }

    return eStatus;
}

MOS_STATUS CodechalEncodeCscDs::InitKernelStateDS()
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    uint32_t kernelSize, combinedKernelSize, numKernelsToLoad;

    numKernelsToLoad = m_encoder->m_interlacedFieldDisabled ? 1 : CODEC_NUM_FIELDS_PER_FRAME;

    CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHal_GetKernelBinaryAndSize(
        m_encoder->m_kernelBase,
        m_encoder->m_kuid,
        &m_dsKernelBase,
        &combinedKernelSize));

    CODECHAL_KERNEL_HEADER currKrnHeader;
    for (uint32_t krnStateIdx = 0; krnStateIdx < numKernelsToLoad; krnStateIdx++)
    {
        kernelSize = combinedKernelSize;

        m_dsKernelState = &m_encoder->m_scaling4xKernelStates[krnStateIdx];

        CODECHAL_ENCODE_CHK_STATUS_RETURN(m_encoder->pfnGetKernelHeaderAndSize(
            m_dsKernelBase,
            ENC_SCALING4X,
            krnStateIdx,
            &currKrnHeader,
            &kernelSize))

        m_dsKernelState->KernelParams.iBTCount = m_dsBTCount[0];
        m_dsKernelState->KernelParams.iThreadCount = m_renderInterface->GetHwCaps()->dwMaxThreads;
        m_dsKernelState->KernelParams.iCurbeLength = m_dsCurbeLength[0];
        m_dsKernelState->KernelParams.iBlockWidth = CODECHAL_MACROBLOCK_WIDTH;
        m_dsKernelState->KernelParams.iBlockHeight = CODECHAL_MACROBLOCK_HEIGHT;
        m_dsKernelState->KernelParams.iIdCount = 1;
        m_dsKernelState->KernelParams.iInlineDataLength = m_dsInlineDataLength;

        m_dsKernelState->dwCurbeOffset = m_stateHeapInterface->GetSizeofCmdInterfaceDescriptorData();
        m_dsKernelState->KernelParams.pBinary = m_dsKernelBase + (currKrnHeader.KernelStartPointer << MHW_KERNEL_OFFSET_SHIFT);
        m_dsKernelState->KernelParams.iSize = kernelSize;
        CODECHAL_ENCODE_CHK_STATUS_RETURN(m_stateHeapInterface->CalculateSshAndBtSizesRequested(
            m_dsKernelState->KernelParams.iBTCount,
            &m_dsKernelState->dwSshSize,
            &m_dsKernelState->dwBindingTableSize));

        CODECHAL_ENCODE_CHK_NULL_RETURN(m_renderInterface->m_stateHeapInterface);
        CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHal_MhwInitISH(m_renderInterface->m_stateHeapInterface, m_dsKernelState));

        if (m_32xMeSupported)
        {
            m_dsKernelState = &m_encoder->m_scaling2xKernelStates[krnStateIdx];

            CODECHAL_ENCODE_CHK_STATUS_RETURN(m_encoder->pfnGetKernelHeaderAndSize(
                m_dsKernelBase,
                ENC_SCALING2X,
                krnStateIdx,
                &currKrnHeader,
                &kernelSize))

            m_dsKernelState->KernelParams.iBTCount = m_dsBTCount[1];
            m_dsKernelState->KernelParams.iThreadCount = m_renderInterface->GetHwCaps()->dwMaxThreads;
            m_dsKernelState->KernelParams.iCurbeLength = m_dsCurbeLength[1];
            m_dsKernelState->KernelParams.iBlockWidth = CODECHAL_MACROBLOCK_WIDTH;
            m_dsKernelState->KernelParams.iBlockHeight = CODECHAL_MACROBLOCK_HEIGHT;

            m_dsKernelState->dwCurbeOffset = m_stateHeapInterface->GetSizeofCmdInterfaceDescriptorData();
            m_dsKernelState->KernelParams.pBinary = m_dsKernelBase + (currKrnHeader.KernelStartPointer << MHW_KERNEL_OFFSET_SHIFT);
            m_dsKernelState->KernelParams.iSize = kernelSize;
            CODECHAL_ENCODE_CHK_STATUS_RETURN(m_stateHeapInterface->CalculateSshAndBtSizesRequested(
                m_dsKernelState->KernelParams.iBTCount,
                &m_dsKernelState->dwSshSize,
                &m_dsKernelState->dwBindingTableSize));

            CODECHAL_ENCODE_CHK_NULL_RETURN(m_renderInterface->m_stateHeapInterface);
            CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHal_MhwInitISH(m_renderInterface->m_stateHeapInterface, m_dsKernelState));
        }

        if (m_encoder->m_interlacedFieldDisabled)
        {
            m_encoder->m_scaling4xKernelStates[1] = m_encoder->m_scaling4xKernelStates[0];

            if (m_32xMeSupported)
            {
                m_encoder->m_scaling2xKernelStates[1] = m_encoder->m_scaling2xKernelStates[0];
            }
        }
    }

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS CodechalEncodeCscDs::SetCurbeDS4x()
{
    CODECHAL_ENCODE_FUNCTION_ENTER;
    
    Ds4xKernelCurbeData curbe;
    
    curbe.DW0_InputPictureWidth = m_curbeParams.dwInputPictureWidth;
    curbe.DW0_InputPictureHeight = m_curbeParams.dwInputPictureHeight;

    curbe.DW1_InputYBTIFrame = ds4xSrcYPlane;
    curbe.DW2_OutputYBTIFrame = ds4xDstYPlane;
    
    if (m_curbeParams.bFieldPicture)
    {
        curbe.DW3_InputYBTIBottomField = ds4xSrcYPlaneBtmField;
        curbe.DW4_OutputYBTIBottomField = ds4xDstYPlaneBtmField;
    }

    if (curbe.DW6_EnableMBFlatnessCheck = m_curbeParams.bFlatnessCheckEnabled)
    {
        curbe.DW5_FlatnessThreshold = 128;
        curbe.DW8_FlatnessOutputBTIFrame = ds4xDstFlatness;

        if (m_curbeParams.bFieldPicture)
        {
            curbe.DW9_FlatnessOutputBTIBottomField = ds4xDstFlatnessBtmField;
        }
    }

    curbe.DW6_EnableMBVarianceOutput = m_curbeParams.bMBVarianceOutputEnabled;
    curbe.DW6_EnableMBPixelAverageOutput = m_curbeParams.bMBPixelAverageOutputEnabled;
    curbe.DW6_EnableBlock8x8StatisticsOutput = m_curbeParams.bBlock8x8StatisticsEnabled;

    if (curbe.DW6_EnableMBVarianceOutput || curbe.DW6_EnableMBPixelAverageOutput)
    {
        curbe.DW10_MBVProcStatsBTIFrame = ds4xDstMbVProc;

        if (m_curbeParams.bFieldPicture)
        {
            curbe.DW11_MBVProcStatsBTIBottomField = ds4xDstMbVProcBtmField;
        }
    }

    CODECHAL_ENCODE_CHK_STATUS_RETURN(m_dsKernelState->m_dshRegion.AddData(
        &curbe,
        m_dsKernelState->dwCurbeOffset,
        sizeof(curbe)));

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS CodechalEncodeCscDs::SetCurbeDS2x()
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    Ds2xKernelCurbeData curbe;

    curbe.DW0_InputPictureWidth = m_curbeParams.dwInputPictureWidth;
    curbe.DW0_InputPictureHeight = m_curbeParams.dwInputPictureHeight;

    curbe.DW8_InputYBTIFrame = ds2xSrcYPlane;
    curbe.DW9_OutputYBTIFrame = ds2xDstYPlane;

    if (m_curbeParams.bFieldPicture)
    {
        curbe.DW10_InputYBTIBottomField = ds2xSrcYPlaneBtmField;
        curbe.DW11_OutputYBTIBottomField = ds2xDstYPlaneBtmField;
    }

    CODECHAL_ENCODE_CHK_STATUS_RETURN(m_dsKernelState->m_dshRegion.AddData(
        &curbe,
        m_dsKernelState->dwCurbeOffset,
        sizeof(curbe)));

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS CodechalEncodeCscDs::SendSurfaceDS(PMOS_COMMAND_BUFFER cmdBuffer)
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    MOS_STATUS eStatus = MOS_STATUS_SUCCESS;

    auto currPicIsFrame = m_surfaceParamsDS.bCurrPicIsFrame;

    auto verticalLineStride = m_verticalLineStride;
    auto verticalLineOffsetTop = CODECHAL_VLINESTRIDEOFFSET_TOP_FIELD;
    auto verticalLineOffsetBottom = CODECHAL_VLINESTRIDEOFFSET_BOT_FIELD;

    auto originalSurface = *m_surfaceParamsDS.psInputSurface;
    originalSurface.dwWidth = m_surfaceParamsDS.dwInputFrameWidth;
    originalSurface.dwHeight = m_surfaceParamsDS.dwInputFrameHeight;

    // Use actual width and height for scaling source, not padded allocated dimensions
    auto scaledSurface = m_surfaceParamsDS.psOutputSurface;
    scaledSurface->dwWidth = m_surfaceParamsDS.dwOutputFrameWidth;
    scaledSurface->dwHeight = m_surfaceParamsDS.dwOutputFrameHeight;

    // Account for field case
    if (!m_fieldScalingOutputInterleaved)
    {
        verticalLineStride = verticalLineOffsetTop = verticalLineOffsetBottom = 0;
        originalSurface.dwHeight =
            MOS_ALIGN_CEIL((currPicIsFrame) ? originalSurface.dwHeight : originalSurface.dwHeight / 2, 16);
        scaledSurface->dwHeight =
            MOS_ALIGN_CEIL((currPicIsFrame) ? scaledSurface->dwHeight : scaledSurface->dwHeight / 2, 16);
    }
    originalSurface.UPlaneOffset.iYOffset = originalSurface.dwHeight;

    // Source surface/s
    CODECHAL_SURFACE_CODEC_PARAMS surfaceParams;
    MOS_ZeroMemory(&surfaceParams, sizeof(surfaceParams));
    surfaceParams.bIs2DSurface = true;
    surfaceParams.bMediaBlockRW = true;
    if (m_surfaceParamsDS.bScalingOutUses16UnormSurfFmt)
    {
        // 32x scaling requires R16_UNROM
        surfaceParams.bUse16UnormSurfaceFormat = true;
    }
    else
    {
        surfaceParams.bUse32UnormSurfaceFormat = true;
    }
    surfaceParams.psSurface = &originalSurface;
    surfaceParams.dwCacheabilityControl =
        m_hwInterface->ComposeSurfaceCacheabilityControl(
            MOS_CODEC_RESOURCE_USAGE_SURFACE_HME_DOWNSAMPLED_ENCODE,
            (codechalL3 | codechalLLC));
    surfaceParams.dwVerticalLineStride = verticalLineStride;

    CODECHAL_ENCODE_CHK_NULL_RETURN(m_encoder->m_mmcState);
    CODECHAL_ENCODE_CHK_STATUS_RETURN(m_encoder->m_mmcState->SetSurfaceParams(&surfaceParams));

    if (currPicIsFrame)
    {
        // Frame
        surfaceParams.dwBindingTableOffset = m_dsBTISrcY;
        CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHal_SetRcsSurfaceState(
            m_hwInterface,
            cmdBuffer,
            &surfaceParams,
            m_dsKernelState));
    }
    else
    {
        // Top field
        surfaceParams.dwVerticalLineStrideOffset = verticalLineOffsetTop;
        surfaceParams.dwBindingTableOffset = m_dsBTISrcYTopField;
        CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHal_SetRcsSurfaceState(
            m_hwInterface,
            cmdBuffer,
            &surfaceParams,
            m_dsKernelState));

        // Bot field
        surfaceParams.dwOffset = m_surfaceParamsDS.dwInputBottomFieldOffset;
        surfaceParams.dwVerticalLineStrideOffset = verticalLineOffsetBottom;
        surfaceParams.dwBindingTableOffset = m_dsBTISrcYBtmField;
        CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHal_SetRcsSurfaceState(
            m_hwInterface,
            cmdBuffer,
            &surfaceParams,
            m_dsKernelState));
    }

    // Destination surface/s
    MOS_ZeroMemory(&surfaceParams, sizeof(surfaceParams));
    surfaceParams.bIs2DSurface = true;
    surfaceParams.bIsWritable = true;
    surfaceParams.bRenderTarget = true;
    surfaceParams.psSurface = scaledSurface;
    if (m_surfaceParamsDS.bScalingOutUses32UnormSurfFmt)
    {
        surfaceParams.bMediaBlockRW = true;
        surfaceParams.bUse32UnormSurfaceFormat = true;
    }
    else if (m_surfaceParamsDS.bScalingOutUses16UnormSurfFmt)
    {
        surfaceParams.bMediaBlockRW = true;
        surfaceParams.bUse16UnormSurfaceFormat = true;
    }
    surfaceParams.dwCacheabilityControl =
        m_hwInterface->ComposeSurfaceCacheabilityControl(
            MOS_CODEC_RESOURCE_USAGE_SURFACE_HME_DOWNSAMPLED_ENCODE_DST,
            codechalLLC);

    surfaceParams.dwVerticalLineStride = verticalLineStride;
    surfaceParams.bRenderTarget = true;
    surfaceParams.bIsWritable = true;

    if (currPicIsFrame)
    {
        // Frame
        surfaceParams.dwBindingTableOffset = m_dsBTIDstY;
        CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHal_SetRcsSurfaceState(
            m_hwInterface,
            cmdBuffer,
            &surfaceParams,
            m_dsKernelState));
    }
    else
    {
        // Top field
        surfaceParams.dwVerticalLineStrideOffset = verticalLineOffsetTop;
        surfaceParams.dwBindingTableOffset = m_dsBTIDstYTopField;
        CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHal_SetRcsSurfaceState(
            m_hwInterface,
            cmdBuffer,
            &surfaceParams,
            m_dsKernelState));

        // Bot field
        surfaceParams.dwOffset = m_surfaceParamsDS.dwOutputBottomFieldOffset;
        surfaceParams.dwVerticalLineStrideOffset = verticalLineOffsetBottom;
        surfaceParams.dwBindingTableOffset = m_dsBTIDstYBtmField;
        CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHal_SetRcsSurfaceState(
            m_hwInterface,
            cmdBuffer,
            &surfaceParams,
            m_dsKernelState));
    }

    if (m_surfaceParamsDS.bFlatnessCheckEnabled)
    {
        // flatness check surface
        MOS_ZeroMemory(&surfaceParams, sizeof(surfaceParams));
        surfaceParams.bIs2DSurface = true;
        surfaceParams.psSurface = m_surfaceParamsDS.psFlatnessCheckSurface;
        surfaceParams.dwCacheabilityControl =
            m_hwInterface->ComposeSurfaceCacheabilityControl(
                MOS_CODEC_RESOURCE_USAGE_SURFACE_FLATNESS_CHECK_ENCODE,
                codechalL3 | codechalLLC);
        surfaceParams.bMediaBlockRW = true;
        surfaceParams.dwVerticalLineStride = 0;
        surfaceParams.bRenderTarget = true;
        surfaceParams.bIsWritable = true;

        if (currPicIsFrame)
        {
            // Frame
            surfaceParams.dwBindingTableOffset = m_dsBTIDstFlatness;
            CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHal_SetRcsSurfaceState(
                m_hwInterface,
                cmdBuffer,
                &surfaceParams,
                m_dsKernelState));
        }
        else
        {
            // Top field
            surfaceParams.bUseHalfHeight = true;
            surfaceParams.dwVerticalLineStrideOffset = 0;
            surfaceParams.dwBindingTableOffset = m_dsBTIDstFlatnessTopField;
            CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHal_SetRcsSurfaceState(
                m_hwInterface,
                cmdBuffer,
                &surfaceParams,
                m_dsKernelState));

            // Bot field
            surfaceParams.dwOffset = m_surfaceParamsDS.dwFlatnessCheckBottomFieldOffset;
            surfaceParams.dwVerticalLineStrideOffset = 0;
            surfaceParams.dwBindingTableOffset = m_dsBTIDstFlatnessBtmField;
            CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHal_SetRcsSurfaceState(
                m_hwInterface,
                cmdBuffer,
                &surfaceParams,
                m_dsKernelState));
        }
    }

    if (m_surfaceParamsDS.bMBVProcStatsEnabled)
    {
        uint32_t size;
        MOS_ZeroMemory(&surfaceParams, sizeof(surfaceParams));
        surfaceParams.presBuffer = m_surfaceParamsDS.presMBVProcStatsBuffer;
        surfaceParams.dwCacheabilityControl =
            m_hwInterface->ComposeSurfaceCacheabilityControl(
                MOS_CODEC_RESOURCE_USAGE_MB_STATS_ENCODE,
                codechalL3 | codechalLLC);
        surfaceParams.bRenderTarget = true;
        surfaceParams.bIsWritable = true;
        surfaceParams.bRawSurface = true;

        if (currPicIsFrame)
        {
            if (m_surfaceParamsDS.bPreEncInUse)
            {
                size = ((originalSurface.dwWidth + 15) / 16) * ((originalSurface.dwHeight + 15) / 16) * 16 * sizeof(uint32_t);
            }
            else
            {
                size = ((originalSurface.dwWidth + 15) / 16) * 16 * sizeof(uint32_t) * ((originalSurface.dwHeight + 15) / 16);
            }
            surfaceParams.dwSize = size;
            surfaceParams.dwBindingTableOffset = m_dsBTIDstMbVProc;
            CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHal_SetRcsSurfaceState(
                m_hwInterface,
                cmdBuffer,
                &surfaceParams,
                m_dsKernelState));
        }
        else
        {
            if (m_surfaceParamsDS.bPreEncInUse)
            {
                size = ((originalSurface.dwWidth + 15) / 16) * ((originalSurface.dwHeight / 2 + 15) / 16) * 16 * sizeof(uint32_t);
            }
            else
            {
                size = ((originalSurface.dwWidth + 15) / 16) * 16 * sizeof(uint32_t) * ((originalSurface.dwHeight / 2 + 15) / 16);
            }
            surfaceParams.dwSize = size;

            // Top field
            surfaceParams.dwBindingTableOffset = m_dsBTIDstMbVProcTopField;
            CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHal_SetRcsSurfaceState(
                m_hwInterface,
                cmdBuffer,
                &surfaceParams,
                m_dsKernelState));

            // Bot field
            if (m_surfaceParamsDS.bPreEncInUse)
            {
                surfaceParams.presBuffer = m_surfaceParamsDS.presMBVProcStatsBotFieldBuffer;
            }
            surfaceParams.dwOffset = m_surfaceParamsDS.dwMBVProcStatsBottomFieldOffset;
            surfaceParams.dwBindingTableOffset = m_dsBTIDstMbVProcBtmField;
            CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHal_SetRcsSurfaceState(
                m_hwInterface,
                cmdBuffer,
                &surfaceParams,
                m_dsKernelState));
        }
    }

    return eStatus;
}

const uint8_t CodechalEncodeCscDs::GetBTCount()
{
    return (uint8_t)cscNumSurfaces;
}

void CodechalEncodeCscDs::Resize()
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    m_cscBufCountResize = CODEC_NUM_NON_REF_BUFFERS;
    for (uint8_t i = 0; i < CODEC_NUM_TRACKED_BUFFERS; i++)
    {
        if (m_cscBufAnteIdx != i && m_cscBufPenuIdx != i && m_cscBufLastIdx != i)
        {
            ReleaseSurfaceCsc(i);
        }
    }
}

MOS_STATUS CodechalEncodeCscDs::Initialize()
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    MOS_STATUS eStatus = MOS_STATUS_SUCCESS;

    if (m_cscKernelUID)
    {
        uint8_t* binary;
        CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHal_GetKernelBinaryAndSize(
            m_kernelBase,
            m_cscKernelUID,
            &binary,
            &m_combinedKernelSize));

        CODECHAL_ENCODE_CHK_NULL_RETURN(m_kernelBase = binary);

        m_hwInterface->GetStateHeapSettings()->dwIshSize +=
            MOS_ALIGN_CEIL(m_combinedKernelSize, (1 << MHW_KERNEL_OFFSET_SHIFT));
    }

    return eStatus;
}

MOS_STATUS CodechalEncodeCscDs::CheckCondition()
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    MOS_STATUS eStatus = MOS_STATUS_SUCCESS;

    MOS_SURFACE details;
    MOS_ZeroMemory(&details, sizeof(details));
    details.Format = Format_Invalid;
    CODECHAL_ENCODE_CHK_STATUS_RETURN(m_osInterface->pfnGetResourceInfo(m_osInterface, &m_rawSurfaceToEnc->OsResource, &details));

    auto cscFlagPrev = m_cscFlag;
    m_cscFlag = 0;
    m_cscRawSurfWidth = details.dwWidth;
    m_cscRawSurfHeight = details.dwHeight;
    m_colorRawSurface = cscColorNv12TileY; // by default assume NV12 Tile-Y format
    m_threadTraverseSizeX = 5;
    m_threadTraverseSizeY = 2;    // for NV12, thread space is 32x4

    // check raw surface's alignment
    if (m_cscEnableCopy && (details.dwWidth % m_rawSurfAlignment || details.dwHeight % m_rawSurfAlignment))
    {
        m_cscRequireCopy = 1;
    }

    // check raw surface's color/tile format
    if (m_cscEnableColor && !m_encoder->CheckSupportedFormat(&details))
    {
        CODECHAL_ENCODE_CHK_STATUS_RETURN(CheckRawColorFormat(details.Format));
    }

    // check raw surface's MMC state
    if (m_cscEnableMmc)
    {
        MOS_MEMCOMP_STATE mmcState;

        CODECHAL_ENCODE_CHK_STATUS_RETURN(m_osInterface->pfnGetMemoryCompressionMode(
            m_osInterface, &m_rawSurfaceToEnc->OsResource, &mmcState));

        // Gen9 HEVC only: HCP on SKL does not support MMC surface, invoke Ds+Copy kernel to decompress MMC surface
        m_cscRequireMmc = (MOS_MEMCOMP_DISABLED != mmcState);
    }

    // CSC no longer required, free existing CSC surface
    if (cscFlagPrev && !m_cscFlag)
    {
        Resize();
    }
    CODECHAL_ENCODE_NORMALMESSAGE("raw surf = %d x %d, tile = %d, color = %d, cscFlag = %d",
        details.dwWidth, details.dwHeight, details.TileType, m_colorRawSurface, m_cscFlag);

    return eStatus;
}

MOS_STATUS CodechalEncodeCscDs::WaitCscSurface(MOS_GPU_CONTEXT gpuContext, bool readOnly)
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    MOS_STATUS eStatus = MOS_STATUS_SUCCESS;

    auto syncParams = g_cInitSyncParams;
    syncParams.GpuContext = gpuContext;
    syncParams.bReadOnly = readOnly;
    syncParams.presSyncResource = &m_trackedBuffer[m_cscBufCurrIdx].sCopiedSurface.OsResource;
    
    CODECHAL_ENCODE_CHK_STATUS_RETURN(m_osInterface->pfnResourceWait(m_osInterface, &syncParams));
    m_osInterface->pfnSetResourceSyncTag(m_osInterface, &syncParams);

    return eStatus;
}

MOS_STATUS CodechalEncodeCscDs::KernelFunctions(
    KernelParams* params)
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    MOS_STATUS eStatus = MOS_STATUS_SUCCESS;

    CODECHAL_ENCODE_CHK_NULL_RETURN(params);

    bool useDsConvInCombinedKernel = m_useCommonKernel && !(CODECHAL_AVC == m_standard || CODECHAL_MPEG2 == m_standard);

    // call Ds+Copy
    if (m_cscFlag || useDsConvInCombinedKernel)
    {
        CODECHAL_ENCODE_CHK_STATUS_RETURN(CscKernel(params));
    }

    // call 4x DS
    if (m_scalingEnabled && !m_currRefList->b4xScalingUsed)
    {
        params->b32xScalingInUse = false;
        params->b16xScalingInUse = false;
        CODECHAL_ENCODE_CHK_STATUS_RETURN(DsKernel(params));
    }

    // call 16x/32x DS
    if (m_scalingEnabled && m_16xMeSupported)
    {
        // 4x downscaled images used as the input for 16x downscaling
        if (useDsConvInCombinedKernel)
        {
            params->stageDsConversion = dsStage16x;
            CODECHAL_ENCODE_CHK_STATUS_RETURN(CscKernel(params));
        }
        else
        {
            params->b16xScalingInUse = true;
            CODECHAL_ENCODE_CHK_STATUS_RETURN(DsKernel(params));
        }

        if (m_32xMeSupported)
        {
            // 16x downscaled images used as the input for 32x downscaling
            if (useDsConvInCombinedKernel)
            {
                params->stageDsConversion = dsStage32x;
                CODECHAL_ENCODE_CHK_STATUS_RETURN(CscKernel(params));
            }
            else
            {
                params->b32xScalingInUse = true;
                params->b16xScalingInUse = false;
                CODECHAL_ENCODE_CHK_STATUS_RETURN(DsKernel(params));
            }
        }
    }

    return eStatus;
}

MOS_STATUS CodechalEncodeCscDs::CscUsingSfc(ENCODE_INPUT_COLORSPACE colorSpace)
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    MOS_STATUS eStatus = MOS_STATUS_SUCCESS;

    // init SFC state
    if (!m_sfcState)
    {
        m_sfcState = (CODECHAL_ENCODE_SFC_STATE*)MOS_New(CODECHAL_ENCODE_SFC_STATE);
        CODECHAL_ENCODE_CHK_NULL_RETURN(m_sfcState);

        CODECHAL_ENCODE_CHK_STATUS_RETURN(InitSfcState());

        m_sfcState->InputSurfaceColorSpace = MHW_CSpace_sRGB;
    }

    // wait for raw surface on VEBox context
    auto syncParams = g_cInitSyncParams;
    syncParams.GpuContext = MOS_GPU_CONTEXT_VEBOX;
    syncParams.presSyncResource = &m_rawSurfaceToEnc->OsResource;
    syncParams.bReadOnly = true;
    CODECHAL_ENCODE_CHK_STATUS_RETURN(m_osInterface->pfnResourceWait(m_osInterface, &syncParams));
    m_osInterface->pfnSetResourceSyncTag(m_osInterface, &syncParams);

    // allocate CSC surface (existing surfaces will be re-used when associated frame goes out of RefList)
    CODECHAL_ENCODE_CHK_STATUS_RETURN(AllocateSurfaceCsc());

    // On-demand sync for CSC surface re-use
    CODECHAL_ENCODE_CHK_STATUS_RETURN(WaitCscSurface(MOS_GPU_CONTEXT_VEBOX, false));
    
    CODECHAL_ENCODE_SFC_PARAMS sfcParams;
    CODECHAL_ENCODE_CHK_STATUS_RETURN(SetParamsSfc(&sfcParams));

    // set-up color space
    switch (colorSpace)
    {
    case ECOLORSPACE_P601:
        m_sfcState->OutputSurfaceColorSpace = MHW_CSpace_BT601;
        break;
    case ECOLORSPACE_P709:
        m_sfcState->OutputSurfaceColorSpace = MHW_CSpace_BT709;
        break;
    case ECOLORSPACE_P2020:
        m_sfcState->OutputSurfaceColorSpace = MHW_CSpace_BT2020;
        break;
    default:
        CODECHAL_ENCODE_ASSERTMESSAGE("Unknow input color space = %d!", colorSpace);
        eStatus = MOS_STATUS_INVALID_PARAMETER;
    }

    CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHalEncodeSfc_SetParams(
        m_osInterface,
        m_sfcState,
        &sfcParams));

    CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHalEncodeSfc_RenderStart(
        m_hwInterface,
        m_osInterface,
        m_encoder,
        m_sfcState));

    CODECHAL_ENCODE_CHK_STATUS_RETURN(SetSurfacesToEncPak());

    return eStatus;
}

MOS_STATUS CodechalEncodeCscDs::CscKernel(
    KernelParams* params)
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    MOS_STATUS eStatus = MOS_STATUS_SUCCESS;

    CODECHAL_ENCODE_CHK_NULL_RETURN(params);

    if (!m_cscKernelState)
    {
        CODECHAL_ENCODE_CHK_NULL_RETURN(m_cscKernelState = MOS_New(MHW_KERNEL_STATE));

        CODECHAL_ENCODE_CHK_STATUS_RETURN(InitKernelStateCsc());
    }

    // allocate CSC surface (existing surfaces will be re-used when associated frame goes out of RefList)
    CODECHAL_ENCODE_CHK_STATUS_RETURN(AllocateSurfaceCsc());

    if (m_scalingEnabled)
    {
        CODECHAL_ENCODE_CHK_STATUS_RETURN(AllocateSurfaceDS());
    }

    if (m_2xScalingEnabled)
    {
        CODECHAL_ENCODE_CHK_STATUS_RETURN(AllocateSurface2xDS());
    }

    if (m_waitCscSurf)
    {
        // in case PAK hasn't yet consumed a surface sent earlier, wait before we can re-use
        CODECHAL_ENCODE_CHK_STATUS_RETURN(WaitCscSurface(m_renderContext, false));
    }

    // setup kernel params
    CODECHAL_ENCODE_CHK_STATUS_RETURN(SetKernelParamsCsc(params));

    PerfTagSetting perfTag;
    perfTag.Value = 0;
    perfTag.Mode = (uint16_t)m_mode & CODECHAL_ENCODE_MODE_BIT_MASK;
    perfTag.CallType = CODECHAL_ENCODE_PERFTAG_CALL_DS_CONVERSION_KERNEL;
    perfTag.PictureCodingType = m_pictureCodingType;
    m_osInterface->pfnSetPerfTag(m_osInterface, perfTag.Value);
    // Each scaling kernel buffer counts as a separate perf task
    m_osInterface->pfnResetPerfBufferID(m_osInterface);

    // if Single Task Phase is not enabled, use BT count for the kernel state.
    if (m_firstTaskInPhase == true || !m_singleTaskPhaseSupported)
    {
        auto maxBtCount = m_singleTaskPhaseSupported ?
            m_maxBtCount : m_cscKernelState->KernelParams.iBTCount;
        CODECHAL_ENCODE_CHK_STATUS_RETURN(m_stateHeapInterface->RequestSshSpaceForCmdBuf(maxBtCount));
        m_vmeStatesSize = m_hwInterface->GetKernelLoadCommandSize(maxBtCount);
        CODECHAL_ENCODE_CHK_STATUS_RETURN(m_encoder->VerifySpaceAvailable());
    }

    // setup CscDsCopy DSH and Interface Descriptor
    auto stateHeapInterface = m_renderInterface->m_stateHeapInterface;
    CODECHAL_ENCODE_CHK_NULL_RETURN(stateHeapInterface);
    CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHal_AssignDshAndSshSpace(
        stateHeapInterface,
        m_cscKernelState,
        false,
        0,
        false,
        m_storeData));

    MHW_INTERFACE_DESCRIPTOR_PARAMS idParams;
    MOS_ZeroMemory(&idParams, sizeof(idParams));
    idParams.pKernelState = m_cscKernelState;
    CODECHAL_ENCODE_CHK_STATUS_RETURN(m_stateHeapInterface->SetInterfaceDescriptor(1, &idParams));

    // send CURBE
    CODECHAL_ENCODE_CHK_STATUS_RETURN(SetCurbeCsc());

    CODECHAL_MEDIA_STATE_TYPE encFunctionType = CODECHAL_MEDIA_STATE_CSC_DS_COPY;
    CODECHAL_DEBUG_TOOL(
        CODECHAL_ENCODE_CHK_STATUS_RETURN(m_debugInterface->DumpCurbe(
            encFunctionType,
            m_cscKernelState));

        CODECHAL_ENCODE_CHK_STATUS_RETURN(m_debugInterface->DumpKernelRegion(
            encFunctionType,
            MHW_DSH_TYPE,
            m_cscKernelState));
    )

    MOS_COMMAND_BUFFER cmdBuffer;
    CODECHAL_ENCODE_CHK_STATUS_RETURN(m_osInterface->pfnGetCommandBuffer(m_osInterface, &cmdBuffer, 0));

    SendKernelCmdsParams sendKernelCmdsParams = SendKernelCmdsParams();
    sendKernelCmdsParams.EncFunctionType = encFunctionType;
    sendKernelCmdsParams.pKernelState = m_cscKernelState;
    CODECHAL_ENCODE_CHK_STATUS_RETURN(m_encoder->SendGenericKernelCmds(&cmdBuffer, &sendKernelCmdsParams));

    // add binding table
    CODECHAL_ENCODE_CHK_STATUS_RETURN(m_stateHeapInterface->SetBindingTable(m_cscKernelState));

    CODECHAL_ENCODE_CHK_STATUS_RETURN(SendSurfaceCsc(&cmdBuffer));

    CODECHAL_DEBUG_TOOL(
        CODECHAL_ENCODE_CHK_STATUS_RETURN(m_debugInterface->DumpKernelRegion(
            encFunctionType,
            MHW_SSH_TYPE,
            m_cscKernelState));
    )

    MHW_WALKER_PARAMS walkerParams;
    MOS_ZeroMemory(&walkerParams, sizeof(walkerParams));
    walkerParams.WalkerMode = m_walkerMode;
    walkerParams.UseScoreboard = m_useHwScoreboard;
    walkerParams.BlockResolution.x =
    walkerParams.GlobalResolution.x =
    walkerParams.GlobalOutlerLoopStride.x = m_walkerResolutionX;
    walkerParams.BlockResolution.y =
    walkerParams.GlobalResolution.y =
    walkerParams.GlobalInnerLoopUnit.y = m_walkerResolutionY;
    walkerParams.dwLocalLoopExecCount = 0xFFFF;  //MAX VALUE
    walkerParams.dwGlobalLoopExecCount = 0xFFFF;  //MAX VALUE

    // Raster scan walking pattern
    walkerParams.LocalOutLoopStride.y = 1;
    walkerParams.LocalInnerLoopUnit.x = 1;
    walkerParams.LocalEnd.x = m_walkerResolutionX - 1;

    if (m_groupIdSelectSupported)
    {
        walkerParams.GroupIdLoopSelect = m_groupId;
    }

    CODECHAL_ENCODE_CHK_STATUS_RETURN(m_renderInterface->AddMediaObjectWalkerCmd(&cmdBuffer, &walkerParams));

    CODECHAL_ENCODE_CHK_STATUS_RETURN(m_encoder->EndStatusReport(&cmdBuffer, encFunctionType));

    CODECHAL_ENCODE_CHK_STATUS_RETURN(m_stateHeapInterface->SubmitBlocks(m_cscKernelState));

    if (!m_singleTaskPhaseSupported || m_lastTaskInPhase)
    {
        CODECHAL_ENCODE_CHK_STATUS_RETURN(m_stateHeapInterface->UpdateGlobalCmdBufId());
        CODECHAL_ENCODE_CHK_STATUS_RETURN(m_miInterface->AddMiBatchBufferEnd(&cmdBuffer, nullptr));
    }

    CODECHAL_DEBUG_TOOL(CODECHAL_ENCODE_CHK_STATUS_RETURN(m_debugInterface->DumpCmdBuffer(
        &cmdBuffer,
        encFunctionType,
        nullptr)));

    CODECHAL_ENCODE_CHK_STATUS_RETURN(m_hwInterface->UpdateSSEuForCmdBuffer(
        &cmdBuffer, m_singleTaskPhaseSupported, m_lastTaskInPhase));

    m_osInterface->pfnReturnCommandBuffer(m_osInterface, &cmdBuffer, 0);

    if (!m_singleTaskPhaseSupported || m_lastTaskInPhase)
    {
        m_osInterface->pfnSubmitCommandBuffer(m_osInterface, &cmdBuffer, m_renderContextUsesNullHw);
        m_lastTaskInPhase = false;
    }

    if (dsDisabled == params->stageDsConversion)
    {
        // send appropriate surface to Enc/Pak depending on different CSC operation type
        CODECHAL_ENCODE_CHK_STATUS_RETURN(SetSurfacesToEncPak());
    }

    return eStatus;
}

MOS_STATUS CodechalEncodeCscDs::DsKernel(
    KernelParams* params)
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    MOS_STATUS eStatus = MOS_STATUS_SUCCESS;

    CODECHAL_ENCODE_CHK_NULL_RETURN(params);

    if (!m_firstField)
    {
        // Both fields are scaled when the first field comes in, no need to scale again
        return eStatus;
    }

    if (!m_dsKernelState)
    {
        CODECHAL_ENCODE_CHK_STATUS_RETURN(InitKernelStateDS());
    }

    if (m_scalingEnabled)
    {
        CODECHAL_ENCODE_CHK_STATUS_RETURN(AllocateSurfaceDS());
    }

    if (m_2xScalingEnabled)
    {
        CODECHAL_ENCODE_CHK_STATUS_RETURN(AllocateSurface2xDS());
    }

    PerfTagSetting perfTag;
    perfTag.Value = 0;
    perfTag.Mode = m_mode & CODECHAL_ENCODE_MODE_BIT_MASK;
    perfTag.CallType = CODECHAL_ENCODE_PERFTAG_CALL_SCALING_KERNEL;
    perfTag.PictureCodingType = m_pictureCodingType;
    m_osInterface->pfnSetPerfTag(m_osInterface, perfTag.Value);
    m_osInterface->pfnIncPerfBufferID(m_osInterface);
    // Each scaling kernel buffer counts as a separate perf task
    m_osInterface->pfnResetPerfBufferID(m_osInterface);

    bool fieldPicture = CodecHal_PictureIsField(m_encoder->m_currOriginalPic);
    m_dsKernelState = params->b32xScalingInUse ?
        &m_encoder->m_scaling2xKernelStates[fieldPicture] :
        &m_encoder->m_scaling4xKernelStates[fieldPicture];

    // If Single Task Phase is not enabled, use BT count for the kernel state.
    if (m_firstTaskInPhase || !m_singleTaskPhaseSupported)
    {
        auto maxBtCount = m_singleTaskPhaseSupported ?
            m_maxBtCount : m_dsKernelState->KernelParams.iBTCount;
        CODECHAL_ENCODE_CHK_STATUS_RETURN(m_stateHeapInterface->RequestSshSpaceForCmdBuf(maxBtCount));
        m_vmeStatesSize = m_hwInterface->GetKernelLoadCommandSize(maxBtCount);
        CODECHAL_ENCODE_CHK_STATUS_RETURN(m_encoder->VerifySpaceAvailable());
    }

    //Setup Scaling DSH
    auto stateHeapInterface = m_renderInterface->m_stateHeapInterface;
    CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHal_AssignDshAndSshSpace(
        stateHeapInterface,
        m_dsKernelState,
        false,
        0,
        false,
        m_storeData));

    MHW_INTERFACE_DESCRIPTOR_PARAMS idParams;
    MOS_ZeroMemory(&idParams, sizeof(idParams));
    idParams.pKernelState = m_dsKernelState;
    CODECHAL_ENCODE_CHK_STATUS_RETURN(m_stateHeapInterface->SetInterfaceDescriptor(1, &idParams));

    uint32_t scaleFactor, downscaledWidthInMb, downscaledHeightInMb; 
    uint32_t inputFrameWidth, inputFrameHeight, outputFrameWidth, outputFrameHeight;
    uint32_t inputBottomFieldOffset, outputBottomFieldOffset;
    PMOS_SURFACE inputSurface, outputSurface;
    auto trackedBuffer = &m_trackedBuffer[m_currScalingIdx];

    if (params->b32xScalingInUse)
    {
        scaleFactor = SCALE_FACTOR_32x;
        downscaledWidthInMb = m_downscaledWidth32x / CODECHAL_MACROBLOCK_WIDTH;
        downscaledHeightInMb = m_downscaledHeight32x / CODECHAL_MACROBLOCK_HEIGHT;
        if (fieldPicture)
        {
            downscaledHeightInMb = (downscaledHeightInMb + 1) >> 1 << 1;
        }

        inputSurface = &trackedBuffer->sScaled16xSurface;
        inputFrameWidth = m_downscaledWidth16x;
        inputFrameHeight = m_downscaledHeight16x;
        inputBottomFieldOffset = m_scaled16xBottomFieldOffset;

        outputSurface = &trackedBuffer->sScaled32xSurface;
        outputFrameWidth = m_downscaledWidth32x;
        outputFrameHeight = downscaledHeightInMb * CODECHAL_MACROBLOCK_HEIGHT;
        outputBottomFieldOffset = m_scaled32xBottomFieldOffset;
        m_lastTaskInPhase = params->bLastTaskInPhase32xDS;
        m_currRefList->b32xScalingUsed = true;
    }
    else if (params->b16xScalingInUse)
    {
        scaleFactor = SCALE_FACTOR_16x;
        downscaledWidthInMb = m_downscaledWidth16x / CODECHAL_MACROBLOCK_WIDTH;
        downscaledHeightInMb = m_downscaledHeight16x / CODECHAL_MACROBLOCK_HEIGHT; 
        if (fieldPicture)
        {
            downscaledHeightInMb = (downscaledHeightInMb + 1) >> 1 << 1;
        }

        inputSurface = &trackedBuffer->sScaled4xSurface;
        inputFrameWidth = m_downscaledWidth4x;
        inputFrameHeight = m_downscaledHeight4x;
        inputBottomFieldOffset = m_scaledBottomFieldOffset;

        outputSurface = &trackedBuffer->sScaled16xSurface;
        outputFrameWidth = m_downscaledWidth16x;
        outputFrameHeight = downscaledHeightInMb * CODECHAL_MACROBLOCK_HEIGHT;
        outputBottomFieldOffset = m_scaled16xBottomFieldOffset;
        m_lastTaskInPhase = params->bLastTaskInPhase16xDS;
        m_currRefList->b16xScalingUsed = true;
    }
    else
    {
        scaleFactor = SCALE_FACTOR_4x;
        downscaledWidthInMb = m_downscaledWidth4x / CODECHAL_MACROBLOCK_WIDTH;
        downscaledHeightInMb = m_downscaledHeight4x / CODECHAL_MACROBLOCK_HEIGHT;
        if (fieldPicture)
        {
            downscaledHeightInMb = (downscaledHeightInMb + 1) >> 1 << 1;
        }

        inputSurface = (params->bRawInputProvided) ? &params->sInputRawSurface : m_rawSurfaceToEnc;
        inputFrameWidth = m_oriFrameWidth;
        inputFrameHeight = m_oriFrameHeight;
        inputBottomFieldOffset = 0;

        outputSurface = &trackedBuffer->sScaled4xSurface;
        outputFrameWidth = m_downscaledWidth4x;
        outputFrameHeight = downscaledHeightInMb * CODECHAL_MACROBLOCK_HEIGHT;
        outputBottomFieldOffset = m_scaledBottomFieldOffset;
        m_lastTaskInPhase = params->bLastTaskInPhase4xDS;
        m_currRefList->b4xScalingUsed = true;
    }

    CODEC_PICTURE originalPic = (params->bRawInputProvided) ? params->inputPicture : m_encoder->m_currOriginalPic;
    FeiPreEncParams *preEncParams = nullptr;
    if (m_encoder->m_codecFunction == CODECHAL_FUNCTION_FEI_PRE_ENC)
    {
        preEncParams = (FeiPreEncParams*)m_encoder->m_encodeParams.pPreEncParams;
        CODECHAL_ENCODE_CHK_NULL_RETURN(preEncParams);
    }

    bool scaling4xInUse = !(params->b32xScalingInUse || params->b16xScalingInUse);
    m_curbeParams.pKernelState = m_dsKernelState;
    m_curbeParams.dwInputPictureWidth = inputFrameWidth;
    m_curbeParams.dwInputPictureHeight = inputFrameHeight;
    m_curbeParams.b16xScalingInUse = params->b16xScalingInUse;
    m_curbeParams.b32xScalingInUse = params->b32xScalingInUse;
    m_curbeParams.bFieldPicture = fieldPicture;
    // Enable flatness check only for 4x scaling.
    m_curbeParams.bFlatnessCheckEnabled = scaling4xInUse && m_flatnessCheckEnabled;
    m_curbeParams.bMBVarianceOutputEnabled = m_curbeParams.bMBPixelAverageOutputEnabled =
        preEncParams ? !preEncParams->bDisableStatisticsOutput : scaling4xInUse && m_mbStatsEnabled;
    m_curbeParams.bBlock8x8StatisticsEnabled = preEncParams ? preEncParams->bEnable8x8Statistics : false;

    if (params->b32xScalingInUse)
    {
        CODECHAL_ENCODE_CHK_STATUS_RETURN(SetCurbeDS2x());
    }
    else
    {
        CODECHAL_ENCODE_CHK_STATUS_RETURN(SetCurbeDS4x());
    }

    auto encFunctionType = params->b32xScalingInUse ? CODECHAL_MEDIA_STATE_32X_SCALING :
        (params->b16xScalingInUse ? CODECHAL_MEDIA_STATE_16X_SCALING : CODECHAL_MEDIA_STATE_4X_SCALING);
    CODECHAL_DEBUG_TOOL(
        CODECHAL_ENCODE_CHK_STATUS_RETURN(m_debugInterface->DumpKernelRegion(
            encFunctionType,
            MHW_DSH_TYPE,
            m_dsKernelState));
    CODECHAL_ENCODE_CHK_STATUS_RETURN(m_debugInterface->DumpCurbe(
        encFunctionType,
        m_dsKernelState));
    CODECHAL_ENCODE_CHK_STATUS_RETURN(m_debugInterface->DumpKernelRegion(
        encFunctionType,
        MHW_ISH_TYPE,
        m_dsKernelState));
    )

    MOS_COMMAND_BUFFER cmdBuffer;
    CODECHAL_ENCODE_CHK_STATUS_RETURN(m_osInterface->pfnGetCommandBuffer(m_osInterface, &cmdBuffer, 0));

    SendKernelCmdsParams sendKernelCmdsParams = SendKernelCmdsParams();
    sendKernelCmdsParams.EncFunctionType = encFunctionType;
    sendKernelCmdsParams.pKernelState = m_dsKernelState;
    CODECHAL_ENCODE_CHK_STATUS_RETURN(m_encoder->SendGenericKernelCmds(&cmdBuffer, &sendKernelCmdsParams));

    // Add binding table
    CODECHAL_ENCODE_CHK_STATUS_RETURN(m_stateHeapInterface->SetBindingTable(m_dsKernelState));

    // setup surface states
    m_surfaceParamsDS.bCurrPicIsFrame = !CodecHal_PictureIsField(originalPic);
    m_surfaceParamsDS.psInputSurface = inputSurface;
    m_surfaceParamsDS.dwInputFrameWidth = inputFrameWidth;
    m_surfaceParamsDS.dwInputFrameHeight = inputFrameHeight;
    m_surfaceParamsDS.psOutputSurface = outputSurface;
    m_surfaceParamsDS.dwOutputFrameWidth = outputFrameWidth;
    m_surfaceParamsDS.dwOutputFrameHeight = outputFrameHeight;
    m_surfaceParamsDS.dwInputBottomFieldOffset = (uint32_t)inputBottomFieldOffset;
    m_surfaceParamsDS.dwOutputBottomFieldOffset = (uint32_t)outputBottomFieldOffset;
    m_surfaceParamsDS.bScalingOutUses16UnormSurfFmt = params->b32xScalingInUse;
    m_surfaceParamsDS.bScalingOutUses32UnormSurfFmt = !params->b32xScalingInUse;

    if (preEncParams)
    {
        m_surfaceParamsDS.bPreEncInUse = true;
        if (params->bScalingforRef)
        {
            m_surfaceParamsDS.bMBVProcStatsEnabled = params->bStatsInputProvided;
            m_surfaceParamsDS.presMBVProcStatsBuffer = (params->bStatsInputProvided) ? &(params->sInputStatsBuffer) : nullptr;
            m_surfaceParamsDS.presMBVProcStatsBotFieldBuffer = (params->bStatsInputProvided) ? &(params->sInputStatsBotFieldBuffer) : nullptr;
        }
        else
        {
            m_surfaceParamsDS.bMBVProcStatsEnabled = !preEncParams->bDisableStatisticsOutput;
            m_surfaceParamsDS.presMBVProcStatsBuffer = &(preEncParams->resStatsBuffer);
            m_surfaceParamsDS.presMBVProcStatsBotFieldBuffer = &preEncParams->resStatsBotFieldBuffer;
        }
        m_surfaceParamsDS.dwMBVProcStatsBottomFieldOffset = m_mbVProcStatsBottomFieldOffset;
    }
    else if (m_mbStatsSupported)
    {
        //Currently Only Based on Flatness Check, later on Adaptive Transform Decision too
        m_surfaceParamsDS.bMBVProcStatsEnabled = scaling4xInUse && (m_flatnessCheckEnabled || m_mbStatsEnabled);
        m_surfaceParamsDS.presMBVProcStatsBuffer = &m_resMbStatsBuffer;
        m_surfaceParamsDS.dwMBVProcStatsBottomFieldOffset = m_mbStatsBottomFieldOffset;

        m_surfaceParamsDS.bFlatnessCheckEnabled = false; // Disabling flatness check as its encompassed in Mb stats
    }
    else
    {
        // Enable flatness check only for 4x scaling.
        m_surfaceParamsDS.bFlatnessCheckEnabled = scaling4xInUse && m_flatnessCheckEnabled;
        m_surfaceParamsDS.psFlatnessCheckSurface = &m_flatnessCheckSurface;
        m_surfaceParamsDS.dwFlatnessCheckBottomFieldOffset = m_flatnessCheckBottomFieldOffset;
    }

    CODECHAL_ENCODE_CHK_STATUS_RETURN(SendSurfaceDS(&cmdBuffer));

    // Add dump for scaling surface state heap here
    CODECHAL_DEBUG_TOOL(
        CODECHAL_ENCODE_CHK_STATUS_RETURN(m_debugInterface->DumpKernelRegion(
            encFunctionType,
            MHW_SSH_TYPE,
            m_dsKernelState));
    )

    uint32_t resolutionX, resolutionY;
    if (params->b32xScalingInUse)
    {
        resolutionX = downscaledWidthInMb;
        resolutionY = downscaledHeightInMb;
    }
    else
    {
        resolutionX = downscaledWidthInMb * 2; /* looping for Walker is needed at 8x8 block level */
        resolutionY = downscaledHeightInMb * 2;
        if (fieldPicture && (m_encoder->m_codecFunction == CODECHAL_FUNCTION_FEI_PRE_ENC))
        {
            resolutionY = MOS_ALIGN_CEIL(downscaledHeightInMb, 2) * 2;
        }
    }

    MHW_WALKER_PARAMS walkerParams;
    MOS_ZeroMemory(&walkerParams, sizeof(MHW_WALKER_PARAMS));
    walkerParams.WalkerMode = m_walkerMode;
    walkerParams.BlockResolution.x =
    walkerParams.GlobalResolution.x =
    walkerParams.GlobalOutlerLoopStride.x = resolutionX;
    walkerParams.BlockResolution.y =
    walkerParams.GlobalResolution.y =
    walkerParams.GlobalInnerLoopUnit.y = resolutionY;
    walkerParams.dwLocalLoopExecCount = 0xFFFF;  //MAX VALUE
    walkerParams.dwGlobalLoopExecCount = 0xFFFF;  //MAX VALUE

    // Raster scan walking pattern
    walkerParams.LocalOutLoopStride.y = 1;
    walkerParams.LocalInnerLoopUnit.x = 1;
    walkerParams.LocalEnd.x = resolutionX - 1;

    if (m_groupIdSelectSupported)
    {
        walkerParams.GroupIdLoopSelect = m_groupId;
    }

    CODECHAL_ENCODE_CHK_STATUS_RETURN(m_renderInterface->AddMediaObjectWalkerCmd(&cmdBuffer, &walkerParams));

    CODECHAL_ENCODE_CHK_STATUS_RETURN(m_encoder->EndStatusReport(&cmdBuffer, encFunctionType));

    CODECHAL_ENCODE_CHK_STATUS_RETURN(m_stateHeapInterface->SubmitBlocks(m_dsKernelState));

    if (!m_singleTaskPhaseSupported || m_lastTaskInPhase)
    {
        CODECHAL_ENCODE_CHK_STATUS_RETURN(m_stateHeapInterface->UpdateGlobalCmdBufId());
        CODECHAL_ENCODE_CHK_STATUS_RETURN(m_miInterface->AddMiBatchBufferEnd(&cmdBuffer, nullptr));
    }

    CODECHAL_DEBUG_TOOL(CODECHAL_ENCODE_CHK_STATUS_RETURN(m_debugInterface->DumpCmdBuffer(
        &cmdBuffer,
        encFunctionType,
        nullptr)));

    CODECHAL_ENCODE_CHK_STATUS_RETURN(m_hwInterface->UpdateSSEuForCmdBuffer(
        &cmdBuffer, m_singleTaskPhaseSupported, m_lastTaskInPhase));

    m_osInterface->pfnReturnCommandBuffer(m_osInterface, &cmdBuffer, 0);

    if (!m_singleTaskPhaseSupported || m_lastTaskInPhase)
    {
        m_osInterface->pfnSubmitCommandBuffer(m_osInterface, &cmdBuffer, m_renderContextUsesNullHw);
        m_lastTaskInPhase = false;
    }

    return eStatus;
}

CodechalEncodeCscDs::CodechalEncodeCscDs(PCODECHAL_ENCODER encoder)
    : m_useRawForRef(encoder->bUseRawForRef),
    m_waitForPak(encoder->bWaitForPAK),
    m_useCommonKernel(encoder->bUseCommonKernel),
    m_useHwScoreboard(encoder->bUseHwScoreboard),
    m_renderContextUsesNullHw(encoder->bRenderContextUsesNullHw),
    m_groupIdSelectSupported(encoder->bGroupIdSelectSupported),
    m_16xMeSupported(encoder->b16xMeSupported),
    m_32xMeSupported(encoder->b32xMeSupported),
    m_scalingEnabled(encoder->bScalingEnabled),
    m_2xScalingEnabled(encoder->b2xScalingEnabled),
    m_firstField(encoder->bFirstField),
    m_fieldScalingOutputInterleaved(encoder->bFieldScalingOutputInterleaved),
    m_flatnessCheckEnabled(encoder->bFlatnessCheckEnabled),
    m_mbStatsEnabled(encoder->bMbStatsEnabled),
    m_mbStatsSupported(encoder->bMbStatsSupported),
    m_singleTaskPhaseSupported(encoder->bSingleTaskPhaseSupported),
    m_firstTaskInPhase(encoder->bFirstTaskInPhase),
    m_lastTaskInPhase(encoder->bLastTaskInPhase),
    m_groupId(encoder->ucGroupId),
    m_currScalingIdx(encoder->ucCurrScalingIdx),
    m_outputChromaFormat(encoder->outputChromaFormat),
    m_pictureCodingType(encoder->wPictureCodingType),
    m_standard(encoder->Standard),
    m_mode(encoder->Mode),
    m_oriFrameWidth(encoder->dwOriFrameWidth),
    m_oriFrameHeight(encoder->dwOriFrameHeight),
    m_frameWidth(encoder->dwFrameWidth),
    m_frameHeight(encoder->dwFrameHeight),
    m_downscaledWidth4x(encoder->dwDownscaledWidth4x),
    m_downscaledHeight4x(encoder->dwDownscaledHeight4x),
    m_downscaledWidth16x(encoder->dwDownscaledWidth16x),
    m_downscaledHeight16x(encoder->dwDownscaledHeight16x),
    m_downscaledWidth32x(encoder->dwDownscaledWidth32x),
    m_downscaledHeight32x(encoder->dwDownscaledHeight32x),
    m_scaledBottomFieldOffset(encoder->ulScaledBottomFieldOffset),
    m_scaled16xBottomFieldOffset(encoder->ulScaled16xBottomFieldOffset),
    m_scaled32xBottomFieldOffset(encoder->ulScaled32xBottomFieldOffset),
    m_mbVProcStatsBottomFieldOffset(encoder->dwMBVProcStatsBottomFieldOffset),
    m_mbStatsBottomFieldOffset(encoder->dwMbStatsBottomFieldOffset),
    m_flatnessCheckBottomFieldOffset(encoder->ulFlatnessCheckBottomFieldOffset),
    m_verticalLineStride(encoder->dwVerticalLineStride),
    m_maxBtCount(encoder->dwMaxBtCount),
    m_vmeStatesSize(encoder->dwVMEStatesSize),
    m_storeData(encoder->dwStoreData),
    m_renderContext(encoder->RenderContext),
    m_walkerMode(encoder->WalkerMode),
    m_currRefList(encoder->pCurrRefList),
    m_flatnessCheckSurface(encoder->sFlatnessCheckSurface),
    m_resMbStatsBuffer(encoder->resMbStatsBuffer),
    m_rawSurfaceToEnc(encoder->psRawSurfaceToENC),
    m_rawSurfaceToPak(encoder->psRawSurfaceToPAK),
    m_trackedBuffer(encoder->trackedBuffer)
{
    // Initilize interface pointers
    m_osInterface = encoder->pOsInterface;
    m_hwInterface = encoder->pHwInterface;
    m_debugInterface = encoder->pDebugInterface;
    m_miInterface = m_hwInterface->GetMiInterface();
    m_renderInterface = m_hwInterface->GetRenderInterface();
    m_stateHeapInterface = m_renderInterface->m_stateHeapInterface->pStateHeapInterface;

    m_dsBTCount[0] = ds4xNumSurfaces;
    m_dsBTCount[1] = ds2xNumSurfaces;
    m_dsCurbeLength[0] = sizeof(Ds4xKernelCurbeData);
    m_dsCurbeLength[1] = sizeof(Ds2xKernelCurbeData);
    m_dsInlineDataLength = sizeof(DsKernelInlineData);

    // for Gen9+ the surface alignment is relaxed to 4x instead of 16x
    encoder->dwRawSurfAlignment = MHW_VDBOX_MFX_RAW_UV_PLANE_ALIGNMENT_GEN9;
}

CodechalEncodeCscDs::CodechalEncodeCscDs(CodechalEncoderState* encoder)
    : m_useRawForRef(encoder->m_useRawForRef),
    m_waitForPak(encoder->m_waitForPak),
    m_useCommonKernel(encoder->m_useCommonKernel),
    m_useHwScoreboard(encoder->m_useHwScoreboard),
    m_renderContextUsesNullHw(encoder->m_renderContextUsesNullHw),
    m_groupIdSelectSupported(encoder->m_groupIdSelectSupported),
    m_16xMeSupported(encoder->m_16xMeSupported),
    m_32xMeSupported(encoder->m_32xMeSupported),
    m_scalingEnabled(encoder->m_scalingEnabled),
    m_2xScalingEnabled(encoder->m_2xScalingEnabled),
    m_firstField(encoder->m_firstField),
    m_fieldScalingOutputInterleaved(encoder->m_fieldScalingOutputInterleaved),
    m_flatnessCheckEnabled(encoder->m_flatnessCheckEnabled),
    m_mbStatsEnabled(encoder->m_mbStatsEnabled),
    m_mbStatsSupported(encoder->m_mbStatsSupported),
    m_singleTaskPhaseSupported(encoder->m_singleTaskPhaseSupported),
    m_firstTaskInPhase(encoder->m_firstTaskInPhase),
    m_lastTaskInPhase(encoder->m_lastTaskInPhase),
    m_groupId(encoder->m_groupId),
    m_currScalingIdx(encoder->m_currScalingIdx),
    m_outputChromaFormat(encoder->m_outputChromaFormat),
    m_pictureCodingType(encoder->m_pictureCodingType),
    m_standard(encoder->m_standard),
    m_mode(encoder->m_mode),
    m_oriFrameWidth(encoder->m_oriFrameWidth),
    m_oriFrameHeight(encoder->m_oriFrameHeight),
    m_frameWidth(encoder->m_frameWidth),
    m_frameHeight(encoder->m_frameHeight),
    m_downscaledWidth4x(encoder->m_downscaledWidth4x),
    m_downscaledHeight4x(encoder->m_downscaledHeight4x),
    m_downscaledWidth16x(encoder->m_downscaledWidth16x),
    m_downscaledHeight16x(encoder->m_downscaledHeight16x),
    m_downscaledWidth32x(encoder->m_downscaledWidth32x),
    m_downscaledHeight32x(encoder->m_downscaledHeight32x),
    m_scaledBottomFieldOffset(encoder->m_scaledBottomFieldOffset),
    m_scaled16xBottomFieldOffset(encoder->m_scaled16xBottomFieldOffset),
    m_scaled32xBottomFieldOffset(encoder->m_scaled32xBottomFieldOffset),
    m_mbVProcStatsBottomFieldOffset(encoder->dwMBVProcStatsBottomFieldOffset),
    m_mbStatsBottomFieldOffset(encoder->m_mbStatsBottomFieldOffset),
    m_flatnessCheckBottomFieldOffset(encoder->m_flatnessCheckBottomFieldOffset),
    m_verticalLineStride(encoder->m_verticalLineStride),
    m_maxBtCount(encoder->m_maxBtCount),
    m_vmeStatesSize(encoder->m_vmeStatesSize),
    m_storeData(encoder->m_storeData),
    m_renderContext(encoder->m_renderContext),
    m_walkerMode(encoder->m_walkerMode),
    m_currRefList(encoder->m_currRefList),
    m_flatnessCheckSurface(encoder->m_flatnessCheckSurface),
    m_resMbStatsBuffer(encoder->m_resMbStatsBuffer),
    m_rawSurfaceToEnc(encoder->m_rawSurfaceToEnc),
    m_rawSurfaceToPak(encoder->m_rawSurfaceToPak),
    m_trackedBuffer(encoder->m_trackedBuffer)
{
    // Initilize interface pointers
    m_encoder = encoder;
    m_osInterface = encoder->GetOsInterface();
    m_hwInterface = encoder->GetHwInterface();
    m_debugInterface = encoder->GetDebugInterface();
    m_miInterface = m_hwInterface->GetMiInterface();
    m_renderInterface = m_hwInterface->GetRenderInterface();
    m_stateHeapInterface = m_renderInterface->m_stateHeapInterface->pStateHeapInterface;

    m_dsBTCount[0] = ds4xNumSurfaces;
    m_dsBTCount[1] = ds2xNumSurfaces;
    m_dsCurbeLength[0] = sizeof(Ds4xKernelCurbeData);
    m_dsCurbeLength[1] = sizeof(Ds2xKernelCurbeData);
    m_dsInlineDataLength = sizeof(DsKernelInlineData);

    // for Gen9+ the surface alignment is relaxed to 4x instead of 16x
    m_encoder->m_rawSurfAlignment = MHW_VDBOX_MFX_RAW_UV_PLANE_ALIGNMENT_GEN9;
}

CodechalEncodeCscDs::~CodechalEncodeCscDs()
{
    MOS_Delete(m_cscKernelState);
    m_cscKernelState = nullptr;

    // free Csc/Ds surface
    for (uint8_t i = 0; i < CODEC_NUM_TRACKED_BUFFERS; i++)
    {
        ReleaseSurfaceCsc(i);
        ReleaseSurfaceDS(i);
    }

    if (m_sfcState)
    {
        CodecHalEncodeSfc_Destroy(m_hwInterface, m_osInterface, m_sfcState);
        m_sfcState = nullptr;
    }
}
