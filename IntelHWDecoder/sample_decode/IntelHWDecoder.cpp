#include "IntelHWDecoder.h"
#if defined(_WIN32) || defined(_WIN64)
#include "d3d_allocator.h"
#include "d3d11_allocator.h"
#include "d3d_device.h"
#include "d3d11_device.h"
#endif
#define __SYNC_WA // avoid sync issue on Media SDK side

IntelHWDecoder::IntelHWDecoder(const TCHAR* outfilename)
: m_bIsCompleteFrame(false)
, m_bPrintLatency(false)
, m_needDecodeHeader(true)
{
	m_bVppIsUsed = false;
	MSDK_ZERO_MEMORY(m_mfxBS);

	m_pmfxDEC = NULL;
	m_pmfxVPP = NULL;
	m_impl = 0;

	MSDK_ZERO_MEMORY(m_mfxVideoParams);
	MSDK_ZERO_MEMORY(m_mfxVppVideoParams);

	m_pGeneralAllocator = NULL;
	m_pmfxAllocatorParams = NULL;
	m_memType = SYSTEM_MEMORY;
	m_bExternalAlloc = false;
	m_bDecOutSysmem = false;
	MSDK_ZERO_MEMORY(m_mfxResponse);
	MSDK_ZERO_MEMORY(m_mfxVppResponse);

	m_pCurrentFreeSurface = NULL;
	m_pCurrentFreeVppSurface = NULL;
	m_pCurrentFreeOutputSurface = NULL;
	m_pCurrentOutputSurface = NULL;

//	m_pDeliverOutputSemaphore = NULL;
//	m_pDeliveredEvent = NULL;
//	m_error = MFX_ERR_NONE;
//	m_bStopDeliverLoop = false;

//	m_eWorkMode = MODE_PERFORMANCE;
//	m_bIsMVC = false;
//	m_bIsExtBuffers = false;
//	m_bIsVideoWall = false;
	m_bIsCompleteFrame = false;
	m_bPrintLatency = false;
	m_fourcc = 0;

	m_diMode = 0;
	m_vppOutWidth = 0;
	m_vppOutHeight = 0;

	MSDK_ZERO_MEMORY(m_VppDoNotUse);
	m_VppDoNotUse.Header.BufferId = MFX_EXTBUFF_VPP_DONOTUSE;
	m_VppDoNotUse.Header.BufferSz = sizeof(m_VppDoNotUse);

	m_VppDeinterlacing.Header.BufferId = MFX_EXTBUFF_VPP_DEINTERLACING;
	m_VppDeinterlacing.Header.BufferSz = sizeof(m_VppDeinterlacing);

	m_hwdev = NULL;

#ifdef LIBVA_SUPPORT
	m_export_mode = vaapiAllocatorParams::DONOT_EXPORT;
	m_bPerfMode = false;
#endif

	// prepare YUV file writer
	m_FileWriter.Init(/*_T("IntelHWDecoder_output.yuv")*/outfilename, 1);
}

IntelHWDecoder::~IntelHWDecoder()
{

}

mfxStatus IntelHWDecoder::init(DecoderInitParam* pParams)
{
	MSDK_CHECK_POINTER(pParams, MFX_ERR_NULL_PTR);

	mfxStatus sts = MFX_ERR_NONE;

	// for VP8 complete and single frame reader is a requirement
	// supports completeframe mode for latency oriented scenarios, m_bIsCompleteFrame sign this state
	if (pParams->bLowLat || pParams->bCalLat)
	{
		switch (pParams->videoType)
		{
		case MFX_CODEC_HEVC:
		case MFX_CODEC_AVC:
//			m_FileReader.reset(new CH264FrameReader());
			m_bIsCompleteFrame = true;
			m_bPrintLatency = pParams->bCalLat;
			break;
		case MFX_CODEC_JPEG:
//			m_FileReader.reset(new CJPEGFrameReader());
			m_bIsCompleteFrame = true;
			m_bPrintLatency = pParams->bCalLat;
			break;
		case CODEC_VP8:
//			m_FileReader.reset(new CIVFFrameReader());
			m_bIsCompleteFrame = true;
			m_bPrintLatency = pParams->bCalLat;
			break;
		default:
			return MFX_ERR_UNSUPPORTED; // latency mode is supported only for H.264 and JPEG codecs
		}
	}
/*	else
	{
		switch (pParams->videoType)
		{
		case CODEC_VP8:
			m_FileReader.reset(new CIVFFrameReader());
			break;
		default:
			m_FileReader.reset(new CSmplBitstreamReader());
			break;
		}
	}*/
	m_videoType = pParams->videoType;

	if (pParams->fourcc)
		m_fourcc = pParams->fourcc;

#ifdef LIBVA_SUPPORT
	if (pParams->bPerfMode)
		m_bPerfMode = true;
#endif

	if (pParams->nOutWidth)
		m_vppOutWidth = pParams->nOutWidth;
	if (pParams->nOutHeight)
		m_vppOutHeight = pParams->nOutHeight;


	m_memType = pParams->memType;

//	m_nMaxFps = pParams->nMaxFPS;
//	m_nFrames = pParams->nFrames ? pParams->nFrames : MFX_INFINITE;
//
//	if (MFX_CODEC_CAPTURE != pParams->videoType)
//	{
//		sts = m_FileReader->Init(pParams->strSrcFile);
//		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
//	}

	//real init work for Intel media sdk session
	mfxInitParam initPar;
	mfxExtThreadsParam threadsPar;
	mfxExtBuffer* extBufs[1];
	mfxVersion version;     // real API version with which library is initialized

	MSDK_ZERO_MEMORY(initPar);
	MSDK_ZERO_MEMORY(threadsPar);

	// we set version to 1.0 and later we will query actual version of the library which will got leaded
	initPar.Version.Major = 1;
	initPar.Version.Minor = 0;

	initPar.GPUCopy = pParams->gpuCopy;
	m_gpuCopy = pParams->gpuCopy;

	init_ext_buffer(threadsPar);

	bool needInitExtPar = false;

//	if (pParams->eDeinterlace)
//	{
//		m_diMode = pParams->eDeinterlace;
//	}

	if (pParams->nThreadsNum) {
		threadsPar.NumThread = pParams->nThreadsNum;
		needInitExtPar = true;
	}
	if (pParams->SchedulingType) {
		threadsPar.SchedulingType = pParams->SchedulingType;
		needInitExtPar = true;
	}
	if (pParams->Priority) {
		threadsPar.Priority = pParams->Priority;
		needInitExtPar = true;
	}
	if (needInitExtPar) {
		extBufs[0] = (mfxExtBuffer*)&threadsPar;
		initPar.ExtParam = extBufs;
		initPar.NumExtParam = 1;
	}

	// Init session, default use hw mode
	// if hw not support in this computer, fault
	//if (pParams->bUseHWLib) {
		// try searching on all display adapters
		initPar.Implementation = MFX_IMPL_HARDWARE_ANY;

		// if d3d11 surfaces are used ask the library to run acceleration through D3D11
		// feature may be unsupported due to OS or MSDK API version

		if (D3D11_MEMORY == pParams->memType)
			initPar.Implementation |= MFX_IMPL_VIA_D3D11;

		sts = m_mfxSession.InitEx(initPar);

		// MSDK API version may not support multiple adapters - then try initialize on the default
		if (MFX_ERR_NONE != sts) {
			initPar.Implementation = (initPar.Implementation & !MFX_IMPL_HARDWARE_ANY) | MFX_IMPL_HARDWARE;
			sts = m_mfxSession.InitEx(initPar);
		}
	//}
	//else {
	//	initPar.Implementation = MFX_IMPL_SOFTWARE;
	//	sts = m_mfxSession.InitEx(initPar);
	//}

	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	sts = m_mfxSession.QueryVersion(&version); // get real API version of the loaded library
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	sts = m_mfxSession.QueryIMPL(&m_impl); // get actual library implementation
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	if (pParams->bIsMVC && !CheckVersion(&version, MSDK_FEATURE_MVC)) {
		msdk_printf(MSDK_STRING("error: MVC is not supported in the %d.%d API version\n"),
			version.Major, version.Minor);
		return MFX_ERR_UNSUPPORTED;

	}
	if ((pParams->videoType == MFX_CODEC_JPEG) && !CheckVersion(&version, MSDK_FEATURE_JPEG_DECODE)) {
		msdk_printf(MSDK_STRING("error: Jpeg is not supported in the %d.%d API version\n"),
			version.Major, version.Minor);
		return MFX_ERR_UNSUPPORTED;
	}
	if (pParams->bLowLat && !CheckVersion(&version, MSDK_FEATURE_LOW_LATENCY)) {
		msdk_printf(MSDK_STRING("error: Low Latency mode is not supported in the %d.%d API version\n"),
			version.Major, version.Minor);
		return MFX_ERR_UNSUPPORTED;
	}

//	if (pParams->eDeinterlace &&
//		(pParams->eDeinterlace != MFX_DEINTERLACING_ADVANCED) &&
//		(pParams->eDeinterlace != MFX_DEINTERLACING_BOB))
//	{
//		msdk_printf(MSDK_STRING("error: Unsupported deinterlace value: %d\n"), pParams->eDeinterlace);
//		return MFX_ERR_UNSUPPORTED;
//	}

//	if (pParams->bRenderWin) {
//		m_bRenderWin = pParams->bRenderWin;
//		// note: currently position is unsupported for Wayland
//#if !defined(LIBVA_WAYLAND_SUPPORT)
//		m_nRenderWinX = pParams->nRenderWinX;
//		m_nRenderWinY = pParams->nRenderWinY;
//#endif
//	}

	// create decoder
	m_pmfxDEC = new MFXVideoDECODE(m_mfxSession);
	MSDK_CHECK_POINTER(m_pmfxDEC, MFX_ERR_MEMORY_ALLOC);

	// set video type in parameters
	m_mfxVideoParams.mfx.CodecId = pParams->videoType;

	// prepare bit stream
//	if (MFX_CODEC_CAPTURE != pParams->videoType)
//	{
		sts = InitMfxBitstream(&m_mfxBS, 1024 * 1024);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
//	}

//	if (CheckVersion(&version, MSDK_FEATURE_PLUGIN_API)) {
//		/* Here we actually define the following codec initialization scheme:
//		*  1. If plugin path or guid is specified: we load user-defined plugin (example: VP8 sample decoder plugin)
//		*  2. If plugin path not specified:
//		*    2.a) we check if codec is distributed as a mediasdk plugin and load it if yes
//		*    2.b) if codec is not in the list of mediasdk plugins, we assume, that it is supported inside mediasdk library
//		*/
//		// Load user plug-in, should go after CreateAllocator function (when all callbacks were initialized)
//		if (pParams->pluginParams.type == MFX_PLUGINLOAD_TYPE_FILE && strlen(pParams->pluginParams.strPluginPath))
//		{
//			m_pUserModule.reset(new MFXVideoUSER(m_mfxSession));
//			if (pParams->videoType == CODEC_VP8 || pParams->videoType == MFX_CODEC_HEVC)
//			{
//				m_pPlugin.reset(LoadPlugin(MFX_PLUGINTYPE_VIDEO_DECODE, m_mfxSession, pParams->pluginParams.pluginGuid, 1, pParams->pluginParams.strPluginPath, (mfxU32)strlen(pParams->pluginParams.strPluginPath)));
//			}
//			if (m_pPlugin.get() == NULL) sts = MFX_ERR_UNSUPPORTED;
//		}
//		else
//		{
//			if (AreGuidsEqual(pParams->pluginParams.pluginGuid, MSDK_PLUGINGUID_NULL))
//			{
//				mfxIMPL impl = pParams->bUseHWLib ? MFX_IMPL_HARDWARE : MFX_IMPL_SOFTWARE;
//				pParams->pluginParams.pluginGuid = msdkGetPluginUID(impl, MSDK_VDECODE, pParams->videoType);
//			}
//			if (!AreGuidsEqual(pParams->pluginParams.pluginGuid, MSDK_PLUGINGUID_NULL))
//			{
//				m_pPlugin.reset(LoadPlugin(MFX_PLUGINTYPE_VIDEO_DECODE, m_mfxSession, pParams->pluginParams.pluginGuid, 1));
//				if (m_pPlugin.get() == NULL) sts = MFX_ERR_UNSUPPORTED;
//			}
//			if (sts == MFX_ERR_UNSUPPORTED)
//			{
//				msdk_printf(MSDK_STRING("Default plugin cannot be loaded (possibly you have to define plugin explicitly)\n"));
//			}
//		}
//		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
//	}

	m_nAsyncDepth = pParams->nAsyncDepth;
	if (m_nAsyncDepth == 0) m_nAsyncDepth = 4;
	m_vppOutHeight = pParams->nOutHeight;
	m_vppOutWidth = pParams->nOutWidth;
	if (pParams->pByteSequenceHeader && pParams->nBytesOfSeqHeader)
	{
		if (m_mfxBS.MaxLength < pParams->nBytesOfSeqHeader)
		{
			ExtendMfxBitstream(&m_mfxBS, pParams->nBytesOfSeqHeader);
		}
		memcpy(m_mfxBS.Data, pParams->pByteSequenceHeader, pParams->nBytesOfSeqHeader);
		m_mfxBS.DataOffset = 0;
		m_mfxBS.DataLength = pParams->nBytesOfSeqHeader;
	}
	else
	{
		//if there have not sequence header in the DecodeInitParam, wo just create session, but not init it.
		return sts;
	}
	// Populate parameters. Involves DecodeHeader call
	sts = InitMfxParams(pParams);
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

//	if (m_bVppIsUsed)
//		m_bDecOutSysmem = pParams->bUseHWLib ? false : true;
//	else
//		m_bDecOutSysmem = pParams->memType == SYSTEM_MEMORY;
	m_bDecOutSysmem = false;

	if (m_bVppIsUsed)
	{
		m_pmfxVPP = new MFXVideoVPP(m_mfxSession);
		if (!m_pmfxVPP) return MFX_ERR_MEMORY_ALLOC;
	}

//	m_eWorkMode = pParams->mode;
//	if (m_eWorkMode == MODE_FILE_DUMP) {
//		// prepare YUV file writer
//		sts = m_FileWriter.Init(pParams->strDstFile, pParams->numViews);
//	}
//	else if ((m_eWorkMode != MODE_PERFORMANCE) && (m_eWorkMode != MODE_RENDERING)) {
//		msdk_printf(MSDK_STRING("error: unsupported work mode\n"));
//		sts = MFX_ERR_UNSUPPORTED;
//	}
//	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

//	m_monitorType = pParams->monitorType;
	// create device and allocator
#if defined(LIBVA_SUPPORT)
	m_libvaBackend = pParams->libvaBackend;
#endif // defined(MFX_LIBVA_SUPPORT)

	sts = CreateAllocator();
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	// in case of HW accelerated decode frames must be allocated prior to decoder initialization
	sts = AllocFrames();
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	sts = m_pmfxDEC->Init(&m_mfxVideoParams);
	if (MFX_WRN_PARTIAL_ACCELERATION == sts)
	{
		msdk_printf(MSDK_STRING("WARNING: partial acceleration\n"));
		MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
	}
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	if (m_bVppIsUsed)
	{
		//if (m_diMode)
		//	m_mfxVppVideoParams.vpp.Out.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;

		sts = m_pmfxVPP->Init(&m_mfxVppVideoParams);
		if (MFX_WRN_PARTIAL_ACCELERATION == sts)
		{
			msdk_printf(MSDK_STRING("WARNING: partial acceleration\n"));
			MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
		}
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
	}

	sts = m_pmfxDEC->GetVideoParam(&m_mfxVideoParams);
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

//	if (m_eWorkMode == MODE_RENDERING)
//	{
//		sts = CreateRenderingWindow(pParams, m_bIsMVC && (m_memType == D3D9_MEMORY));
//		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
//	}

	return sts;
}

mfxStatus IntelHWDecoder::InitMfxParams(DecoderInitParam *pParams)
{
	MSDK_CHECK_POINTER(m_pmfxDEC, MFX_ERR_NULL_PTR);
	mfxStatus sts = MFX_ERR_NONE;
//	mfxU32 &numViews = pParams->numViews;

/*	// try to find a sequence header in the stream
	// if header is not found this function exits with error (e.g. if device was lost and there's no header in the remaining stream)
	if (MFX_CODEC_CAPTURE == pParams->videoType)
	{
		m_mfxVideoParams.mfx.CodecId = MFX_CODEC_CAPTURE;
		m_mfxVideoParams.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
		m_mfxVideoParams.mfx.FrameInfo.Width = MSDK_ALIGN32(pParams->scrWidth);
		m_mfxVideoParams.mfx.FrameInfo.Height = MSDK_ALIGN32(pParams->scrHeight);
		m_mfxVideoParams.mfx.FrameInfo.CropW = pParams->scrWidth;
		m_mfxVideoParams.mfx.FrameInfo.CropH = pParams->scrHeight;
		m_mfxVideoParams.mfx.FrameInfo.FourCC = (m_fourcc == MFX_FOURCC_RGB4) ? MFX_FOURCC_RGB4 : MFX_FOURCC_NV12;

		if (!m_mfxVideoParams.mfx.FrameInfo.ChromaFormat)
		{
			if (MFX_FOURCC_NV12 == m_mfxVideoParams.mfx.FrameInfo.FourCC)
				m_mfxVideoParams.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
			else if (MFX_FOURCC_RGB4 == m_mfxVideoParams.mfx.FrameInfo.FourCC)
				m_mfxVideoParams.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV444;
		}
		m_bVppIsUsed = IsVppRequired(pParams);
	}
*/
	for (; MFX_CODEC_CAPTURE != pParams->videoType;)
	{
		// trying to find PicStruct information in AVI headers
		//if (m_mfxVideoParams.mfx.CodecId == MFX_CODEC_JPEG)
		//	MJPEG_AVI_ParsePicStruct(&m_mfxBS);

		// parse bit stream and fill mfx params
		sts = m_pmfxDEC->DecodeHeader(&m_mfxBS, &m_mfxVideoParams);
		if (!sts)
		{
			//decode header success
			m_bVppIsUsed = IsVppRequired(pParams);
		}

//		if (!sts &&
//			!(m_impl & MFX_IMPL_SOFTWARE) &&                        // hw lib
//			(m_mfxVideoParams.mfx.FrameInfo.BitDepthLuma == 10) &&  // hevc 10 bit
//			(m_mfxVideoParams.mfx.CodecId == MFX_CODEC_HEVC) &&
//			AreGuidsEqual(pParams->pluginParams.pluginGuid, MFX_PLUGINID_HEVCD_SW) && // sw hevc decoder
//			m_bVppIsUsed)
//		{
//			sts = MFX_ERR_UNSUPPORTED;
//			msdk_printf(MSDK_STRING("Error: Combination of (SW HEVC plugin in 10bit mode + HW lib VPP) isn't supported. Use -sw option.\n"));
//		}
//		if (m_pPlugin.get() && pParams->videoType == CODEC_VP8 && !sts) {
//			// force set format to nv12 as the vp8 plugin uses yv12
//			m_mfxVideoParams.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
//		}
/*		if (MFX_ERR_MORE_DATA == sts)
		{
			if (m_mfxBS.MaxLength == m_mfxBS.DataLength)
			{
				sts = ExtendMfxBitstream(&m_mfxBS, m_mfxBS.MaxLength * 2);
				MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
			}
			// read a portion of data
			sts = m_FileReader->ReadNextFrame(&m_mfxBS);
			if (MFX_ERR_MORE_DATA == sts &&
				!(m_mfxBS.DataFlag & MFX_BITSTREAM_EOS))
			{
				m_mfxBS.DataFlag |= MFX_BITSTREAM_EOS;
				sts = MFX_ERR_NONE;
			}
			MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

			continue;
		}
		else
		{
			// Enter MVC mode
			if (m_bIsMVC)
			{
				// Check for attached external parameters - if we have them already,
				// we don't need to attach them again
				if (NULL != m_mfxVideoParams.ExtParam)
					break;

				// allocate and attach external parameters for MVC decoder
				sts = AllocateExtBuffer<mfxExtMVCSeqDesc>();
				MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

				AttachExtParam();
				sts = m_pmfxDEC->DecodeHeader(&m_mfxBS, &m_mfxVideoParams);

				if (MFX_ERR_NOT_ENOUGH_BUFFER == sts)
				{
					sts = AllocateExtMVCBuffers();
					SetExtBuffersFlag();

					MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
					MSDK_CHECK_POINTER(m_mfxVideoParams.ExtParam, MFX_ERR_MEMORY_ALLOC);
					continue;
				}
			}

			// if input is interlaced JPEG stream
			if (m_mfxBS.PicStruct == MFX_PICSTRUCT_FIELD_TFF || m_mfxBS.PicStruct == MFX_PICSTRUCT_FIELD_BFF)
			{
				m_mfxVideoParams.mfx.FrameInfo.CropH *= 2;
				m_mfxVideoParams.mfx.FrameInfo.Height = MSDK_ALIGN16(m_mfxVideoParams.mfx.FrameInfo.CropH);
				m_mfxVideoParams.mfx.FrameInfo.PicStruct = m_mfxBS.PicStruct;
			}

			switch (pParams->nRotation)
			{
			case 0:
				m_mfxVideoParams.mfx.Rotation = MFX_ROTATION_0;
				break;
			case 90:
				m_mfxVideoParams.mfx.Rotation = MFX_ROTATION_90;
				break;
			case 180:
				m_mfxVideoParams.mfx.Rotation = MFX_ROTATION_180;
				break;
			case 270:
				m_mfxVideoParams.mfx.Rotation = MFX_ROTATION_270;
				break;
			default:
				return MFX_ERR_UNSUPPORTED;
			}

			break;
		}*/
		break;
	}//for
	
	if (MFX_ERR_MORE_DATA == sts)
	{
		msdk_printf(MSDK_STRING("WARNING: init sequeens header need more data\n"));
		return sts;
	}
	// check DecodeHeader status
	if (MFX_WRN_PARTIAL_ACCELERATION == sts)
	{
		msdk_printf(MSDK_STRING("WARNING: partial acceleration\n"));
		MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
	}
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	if (!m_mfxVideoParams.mfx.FrameInfo.FrameRateExtN || !m_mfxVideoParams.mfx.FrameInfo.FrameRateExtD) {
		msdk_printf(MSDK_STRING("pretending that stream is 30fps one\n"));
		m_mfxVideoParams.mfx.FrameInfo.FrameRateExtN = 30;
		m_mfxVideoParams.mfx.FrameInfo.FrameRateExtD = 1;
	}
	if (!m_mfxVideoParams.mfx.FrameInfo.AspectRatioW || !m_mfxVideoParams.mfx.FrameInfo.AspectRatioH) {
		msdk_printf(MSDK_STRING("pretending that aspect ratio is 1:1\n"));
		m_mfxVideoParams.mfx.FrameInfo.AspectRatioW = 1;
		m_mfxVideoParams.mfx.FrameInfo.AspectRatioH = 1;
	}

	// Videoparams for RGB4 JPEG decoder output
	if ((pParams->fourcc == MFX_FOURCC_RGB4) && (pParams->videoType == MFX_CODEC_JPEG))
	{
		m_mfxVideoParams.mfx.FrameInfo.FourCC = MFX_FOURCC_RGB4;
		m_mfxVideoParams.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV444;
	}

	// If MVC mode we need to detect number of views in stream
/*	if (m_bIsMVC)
	{
		mfxExtMVCSeqDesc* pSequenceBuffer;
		pSequenceBuffer = (mfxExtMVCSeqDesc*)GetExtBuffer(m_mfxVideoParams.ExtParam, m_mfxVideoParams.NumExtParam, MFX_EXTBUFF_MVC_SEQ_DESC);
		MSDK_CHECK_POINTER(pSequenceBuffer, MFX_ERR_INVALID_VIDEO_PARAM);

		mfxU32 i = 0;
		numViews = 0;
		for (i = 0; i < pSequenceBuffer->NumView; ++i)
		{
			// Some MVC streams can contain different information about
			//number of views and view IDs, e.x. numVews = 2
			//and ViewId[0, 1] = 0, 2 instead of ViewId[0, 1] = 0, 1.
			//numViews should be equal (max(ViewId[i]) + 1)
			//to prevent crashes during output files writing 
			if (pSequenceBuffer->View[i].ViewId >= numViews)
				numViews = pSequenceBuffer->View[i].ViewId + 1;
		}
	}
	else
	{
		numViews = 1;
	}
*/
	// specify memory type
	if (!m_bVppIsUsed)
		m_mfxVideoParams.IOPattern = (mfxU16)(m_memType != SYSTEM_MEMORY ? MFX_IOPATTERN_OUT_VIDEO_MEMORY : MFX_IOPATTERN_OUT_SYSTEM_MEMORY);
	else
		m_mfxVideoParams.IOPattern = (mfxU16)(MFX_IOPATTERN_OUT_VIDEO_MEMORY);

	m_mfxVideoParams.AsyncDepth = pParams->nAsyncDepth == 0 ? 4 : pParams->nAsyncDepth;

	return MFX_ERR_NONE;
}

bool IntelHWDecoder::IsVppRequired(DecoderInitParam *pParams)
{
	bool bVppIsUsed = false;
	// JPEG and Capture decoders can provide output in nv12 and rgb4 formats
	if ((pParams->videoType == MFX_CODEC_JPEG) ||
		((pParams->videoType == MFX_CODEC_CAPTURE)))
	{
		bVppIsUsed = m_fourcc && (m_fourcc != MFX_FOURCC_NV12) && (m_fourcc != MFX_FOURCC_RGB4);
	}
	else
	{
		bVppIsUsed = m_fourcc && (m_fourcc != m_mfxVideoParams.mfx.FrameInfo.FourCC);
	}

	if ((m_mfxVideoParams.mfx.FrameInfo.CropW != pParams->nOutWidth) ||
		(m_mfxVideoParams.mfx.FrameInfo.CropH != pParams->nOutHeight))
	{
		bVppIsUsed |= pParams->nOutWidth && pParams->nOutHeight;
	}

//	if (pParams->eDeinterlace)
//	{
//		bVppIsUsed = true;
//	}
	return bVppIsUsed;
}
mfxStatus IntelHWDecoder::AllocFrames()
{
	MSDK_CHECK_POINTER(m_pmfxDEC, MFX_ERR_NULL_PTR);

	mfxStatus sts = MFX_ERR_NONE;

	mfxFrameAllocRequest Request;
	mfxFrameAllocRequest VppRequest[2];

	mfxU16 nSurfNum = 0; // number of surfaces for decoder
	mfxU16 nVppSurfNum = 0; // number of surfaces for vpp

	MSDK_ZERO_MEMORY(Request);
	MSDK_ZERO_MEMORY(VppRequest[0]);
	MSDK_ZERO_MEMORY(VppRequest[1]);

	sts = m_pmfxDEC->Query(&m_mfxVideoParams, &m_mfxVideoParams);
	MSDK_IGNORE_MFX_STS(sts, MFX_WRN_INCOMPATIBLE_VIDEO_PARAM);
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	// calculate number of surfaces required for decoder
	sts = m_pmfxDEC->QueryIOSurf(&m_mfxVideoParams, &Request);
	if (MFX_WRN_PARTIAL_ACCELERATION == sts)
	{
		msdk_printf(MSDK_STRING("WARNING: partial acceleration\n"));
		MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
		m_bDecOutSysmem = true;
	}
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	if (m_bVppIsUsed)
	{
		// respecify memory type between Decoder and VPP
		m_mfxVideoParams.IOPattern = (mfxU16)(m_bDecOutSysmem ?
										MFX_IOPATTERN_OUT_SYSTEM_MEMORY :
										MFX_IOPATTERN_OUT_VIDEO_MEMORY);

		// recalculate number of surfaces required for decoder
		sts = m_pmfxDEC->QueryIOSurf(&m_mfxVideoParams, &Request);
		MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);


		sts = InitVppParams();
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

		sts = m_pmfxVPP->Query(&m_mfxVppVideoParams, &m_mfxVppVideoParams);
		MSDK_IGNORE_MFX_STS(sts, MFX_WRN_INCOMPATIBLE_VIDEO_PARAM);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

		// VppRequest[0] for input frames request, VppRequest[1] for output frames request
		sts = m_pmfxVPP->QueryIOSurf(&m_mfxVppVideoParams, VppRequest);
		if (MFX_WRN_PARTIAL_ACCELERATION == sts) {
			msdk_printf(MSDK_STRING("WARNING: partial acceleration\n"));
			MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
		}
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

		if ((VppRequest[0].NumFrameSuggested < m_mfxVppVideoParams.AsyncDepth) ||
			(VppRequest[1].NumFrameSuggested < m_mfxVppVideoParams.AsyncDepth))
			return MFX_ERR_MEMORY_ALLOC;


		// If surfaces are shared by 2 components, c1 and c2. NumSurf = c1_out + c2_in - AsyncDepth + 1
		// The number of surfaces shared by vpp input and decode output
		nSurfNum = Request.NumFrameSuggested + VppRequest[0].NumFrameSuggested - m_mfxVideoParams.AsyncDepth + 1;

		// The number of surfaces for vpp output
		nVppSurfNum = VppRequest[1].NumFrameSuggested;

		// prepare allocation request
		Request.NumFrameSuggested = Request.NumFrameMin = nSurfNum;

		// surfaces are shared between vpp input and decode output
		Request.Type = MFX_MEMTYPE_EXTERNAL_FRAME | MFX_MEMTYPE_FROM_DECODE | MFX_MEMTYPE_FROM_VPPIN;
	}

	if ((Request.NumFrameSuggested < m_mfxVideoParams.AsyncDepth) &&
		(m_impl & MFX_IMPL_HARDWARE_ANY))
		return MFX_ERR_MEMORY_ALLOC;

	Request.Type |= (m_bDecOutSysmem) ?
		MFX_MEMTYPE_SYSTEM_MEMORY
		: MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET;

#ifdef LIBVA_SUPPORT
	if (!m_bVppIsUsed &&
		(m_export_mode != vaapiAllocatorParams::DONOT_EXPORT))
	{
		Request.Type |= MFX_MEMTYPE_EXPORT_FRAME;
	}
#endif

	// alloc frames for decoder
	sts = m_pGeneralAllocator->Alloc(m_pGeneralAllocator->pthis, &Request, &m_mfxResponse);
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	if (m_bVppIsUsed)
	{
		// alloc frames for VPP
#ifdef LIBVA_SUPPORT
		if (m_export_mode != vaapiAllocatorParams::DONOT_EXPORT)
		{
			VppRequest[1].Type |= MFX_MEMTYPE_EXPORT_FRAME;
		}
#endif
		VppRequest[1].NumFrameSuggested = VppRequest[1].NumFrameMin = nVppSurfNum;
		MSDK_MEMCPY_VAR(VppRequest[1].Info, &(m_mfxVppVideoParams.vpp.Out), sizeof(mfxFrameInfo));

		sts = m_pGeneralAllocator->Alloc(m_pGeneralAllocator->pthis, &VppRequest[1], &m_mfxVppResponse);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

		// prepare mfxFrameSurface1 array for decoder
		nVppSurfNum = m_mfxVppResponse.NumFrameActual;

		// AllocVppBuffers should call before AllocBuffers to set the value of m_OutputSurfacesNumber
		sts = AllocVppBuffers(nVppSurfNum);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
	}

	// prepare mfxFrameSurface1 array for decoder
	nSurfNum = m_mfxResponse.NumFrameActual;

	sts = AllocBuffers(nSurfNum);
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	for (int i = 0; i < nSurfNum; i++)
	{
		// initating each frame:
		MSDK_MEMCPY_VAR(m_pSurfaces[i].frame.Info, &(Request.Info), sizeof(mfxFrameInfo));
		if (m_bExternalAlloc) {
			m_pSurfaces[i].frame.Data.MemId = m_mfxResponse.mids[i];
		}
		else {
			sts = m_pGeneralAllocator->Lock(m_pGeneralAllocator->pthis, m_mfxResponse.mids[i], &(m_pSurfaces[i].frame.Data));
			MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
		}
	}

	// prepare mfxFrameSurface1 array for VPP
	for (int i = 0; i < nVppSurfNum; i++) {
		MSDK_MEMCPY_VAR(m_pVppSurfaces[i].frame.Info, &(VppRequest[1].Info), sizeof(mfxFrameInfo));
		if (m_bExternalAlloc) {
			m_pVppSurfaces[i].frame.Data.MemId = m_mfxVppResponse.mids[i];
		}
		else {
			sts = m_pGeneralAllocator->Lock(m_pGeneralAllocator->pthis, m_mfxVppResponse.mids[i], &(m_pVppSurfaces[i].frame.Data));
			if (MFX_ERR_NONE != sts) {
				return sts;
			}
		}
	}
	return MFX_ERR_NONE;
}

mfxStatus IntelHWDecoder::InitVppParams()
{
	m_mfxVppVideoParams.IOPattern = (mfxU16)(m_bDecOutSysmem ?
		MFX_IOPATTERN_IN_SYSTEM_MEMORY
		: MFX_IOPATTERN_IN_VIDEO_MEMORY);

	m_mfxVppVideoParams.IOPattern |= (m_memType != SYSTEM_MEMORY) ?
		MFX_IOPATTERN_OUT_VIDEO_MEMORY
		: MFX_IOPATTERN_OUT_SYSTEM_MEMORY;

	MSDK_MEMCPY_VAR(m_mfxVppVideoParams.vpp.In, &m_mfxVideoParams.mfx.FrameInfo, sizeof(mfxFrameInfo));
	MSDK_MEMCPY_VAR(m_mfxVppVideoParams.vpp.Out, &m_mfxVppVideoParams.vpp.In, sizeof(mfxFrameInfo));

	if (m_fourcc)
	{
		m_mfxVppVideoParams.vpp.Out.FourCC = m_fourcc;
	}

	if (m_vppOutWidth && m_vppOutHeight)
	{

		m_mfxVppVideoParams.vpp.Out.CropW = m_vppOutWidth;
		m_mfxVppVideoParams.vpp.Out.Width = MSDK_ALIGN16(m_vppOutWidth);
		m_mfxVppVideoParams.vpp.Out.CropH = m_vppOutHeight;
		m_mfxVppVideoParams.vpp.Out.Height = (MFX_PICSTRUCT_PROGRESSIVE == m_mfxVppVideoParams.vpp.Out.PicStruct) ?
			MSDK_ALIGN16(m_vppOutHeight) : MSDK_ALIGN32(m_vppOutHeight);
	}

	m_mfxVppVideoParams.AsyncDepth = m_mfxVideoParams.AsyncDepth;

	//先注释掉，可能不需要vpp，zhuqingquan
	m_VppExtParams.clear();
	AllocAndInitVppFilters();
	m_VppExtParams.push_back((mfxExtBuffer*)&m_VppDoNotUse);
	if (m_diMode)
	{
		m_VppExtParams.push_back((mfxExtBuffer*)&m_VppDeinterlacing);
	}

	m_mfxVppVideoParams.ExtParam = &m_VppExtParams[0];
	m_mfxVppVideoParams.NumExtParam = (mfxU16)m_VppExtParams.size();
	return MFX_ERR_NONE;
}

mfxStatus IntelHWDecoder::AllocAndInitVppFilters()
{
	m_VppDoNotUse.NumAlg = 4;

	m_VppDoNotUse.AlgList = new mfxU32[m_VppDoNotUse.NumAlg];
	if (!m_VppDoNotUse.AlgList) return MFX_ERR_NULL_PTR;

	m_VppDoNotUse.AlgList[0] = MFX_EXTBUFF_VPP_DENOISE; // turn off denoising (on by default)
	m_VppDoNotUse.AlgList[1] = MFX_EXTBUFF_VPP_SCENE_ANALYSIS; // turn off scene analysis (on by default)
	m_VppDoNotUse.AlgList[2] = MFX_EXTBUFF_VPP_DETAIL; // turn off detail enhancement (on by default)
	m_VppDoNotUse.AlgList[3] = MFX_EXTBUFF_VPP_PROCAMP; // turn off processing amplified (on by default)

	if (m_diMode)
	{
		m_VppDeinterlacing.Mode = m_diMode;
	}

	return MFX_ERR_NONE;
}

mfxStatus IntelHWDecoder::CreateAllocator()
{
	mfxStatus sts = MFX_ERR_NONE;

	m_pGeneralAllocator = new GeneralAllocator();
	if (m_memType != SYSTEM_MEMORY || !m_bDecOutSysmem)
	{
#if D3D_SURFACES_SUPPORT
		sts = CreateHWDevice();
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

		// provide device manager to MediaSDK
		mfxHDL hdl = NULL;
		mfxHandleType hdl_t =
#if MFX_D3D11_SUPPORT
			D3D11_MEMORY == m_memType ? MFX_HANDLE_D3D11_DEVICE :
#endif // #if MFX_D3D11_SUPPORT
			MFX_HANDLE_D3D9_DEVICE_MANAGER;

		sts = m_hwdev->GetHandle(hdl_t, &hdl);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
		sts = m_mfxSession.SetHandle(hdl_t, hdl);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

		// create D3D allocator
#if MFX_D3D11_SUPPORT
		if (D3D11_MEMORY == m_memType)
		{
			D3D11AllocatorParams *pd3dAllocParams = new D3D11AllocatorParams;
			MSDK_CHECK_POINTER(pd3dAllocParams, MFX_ERR_MEMORY_ALLOC);
			pd3dAllocParams->pDevice = reinterpret_cast<ID3D11Device *>(hdl);

			m_pmfxAllocatorParams = pd3dAllocParams;
		}
		else
#endif // #if MFX_D3D11_SUPPORT
		{
			D3DAllocatorParams *pd3dAllocParams = new D3DAllocatorParams;
			MSDK_CHECK_POINTER(pd3dAllocParams, MFX_ERR_MEMORY_ALLOC);
			pd3dAllocParams->pManager = reinterpret_cast<IDirect3DDeviceManager9 *>(hdl);

			m_pmfxAllocatorParams = pd3dAllocParams;
		}

		/* In case of video memory we must provide MediaSDK with external allocator
		thus we demonstrate "external allocator" usage model.
		Call SetAllocator to pass allocator to mediasdk */
		sts = m_mfxSession.SetFrameAllocator(m_pGeneralAllocator);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

		m_bExternalAlloc = true;
#elif LIBVA_SUPPORT
		sts = CreateHWDevice();
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
		/* It's possible to skip failed result here and switch to SW implementation,
		but we don't process this way */

		// provide device manager to MediaSDK
		VADisplay va_dpy = NULL;
		sts = m_hwdev->GetHandle(MFX_HANDLE_VA_DISPLAY, (mfxHDL *)&va_dpy);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
		sts = m_mfxSession.SetHandle(MFX_HANDLE_VA_DISPLAY, va_dpy);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

		vaapiAllocatorParams *p_vaapiAllocParams = new vaapiAllocatorParams;
		MSDK_CHECK_POINTER(p_vaapiAllocParams, MFX_ERR_MEMORY_ALLOC);

		p_vaapiAllocParams->m_dpy = va_dpy;
		if (m_eWorkMode == MODE_RENDERING) {
			if (m_libvaBackend == MFX_LIBVA_DRM_MODESET) {
				CVAAPIDeviceDRM* drmdev = dynamic_cast<CVAAPIDeviceDRM*>(m_hwdev);
				p_vaapiAllocParams->m_export_mode = vaapiAllocatorParams::CUSTOM_FLINK;
				p_vaapiAllocParams->m_exporter = dynamic_cast<vaapiAllocatorParams::Exporter*>(drmdev->getRenderer());
			}
			else if (m_libvaBackend == MFX_LIBVA_WAYLAND) {
				p_vaapiAllocParams->m_export_mode = vaapiAllocatorParams::PRIME;
			}
		}
		m_export_mode = p_vaapiAllocParams->m_export_mode;
		m_pmfxAllocatorParams = p_vaapiAllocParams;

		/* In case of video memory we must provide MediaSDK with external allocator
		thus we demonstrate "external allocator" usage model.
		Call SetAllocator to pass allocator to mediasdk */
		sts = m_mfxSession.SetFrameAllocator(m_pGeneralAllocator);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

		m_bExternalAlloc = true;
#endif
	}
	else
	{
#ifdef LIBVA_SUPPORT
		//in case of system memory allocator we also have to pass MFX_HANDLE_VA_DISPLAY to HW library

		if (MFX_IMPL_HARDWARE == MFX_IMPL_BASETYPE(m_impl))
		{
			sts = CreateHWDevice();
			MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

			// provide device manager to MediaSDK
			VADisplay va_dpy = NULL;
			sts = m_hwdev->GetHandle(MFX_HANDLE_VA_DISPLAY, (mfxHDL *)&va_dpy);
			MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
			sts = m_mfxSession.SetHandle(MFX_HANDLE_VA_DISPLAY, va_dpy);
			MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
		}
#endif
		// create system memory allocator
		//m_pGeneralAllocator = new SysMemFrameAllocator;
		//MSDK_CHECK_POINTER(m_pGeneralAllocator, MFX_ERR_MEMORY_ALLOC);

		/* In case of system memory we demonstrate "no external allocator" usage model.
		We don't call SetAllocator, MediaSDK uses internal allocator.
		We use system memory allocator simply as a memory manager for application*/
	}

	// initialize memory allocator
	sts = m_pGeneralAllocator->Init(m_pmfxAllocatorParams);
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	return MFX_ERR_NONE;
}

void IntelHWDecoder::DeleteFrames()
{
	FreeBuffers();

	m_pCurrentFreeSurface = NULL;
	MSDK_SAFE_FREE(m_pCurrentFreeOutputSurface);

	m_pCurrentFreeVppSurface = NULL;

	// delete frames
	if (m_pGeneralAllocator)
	{
		m_pGeneralAllocator->Free(m_pGeneralAllocator->pthis, &m_mfxResponse);
	}

	return;
}

void IntelHWDecoder::DeleteAllocator()
{
	// delete allocator
	MSDK_SAFE_DELETE(m_pGeneralAllocator);
	MSDK_SAFE_DELETE(m_pmfxAllocatorParams);
	MSDK_SAFE_DELETE(m_hwdev);
}

mfxStatus IntelHWDecoder::CreateHWDevice()
{
#if D3D_SURFACES_SUPPORT
	mfxStatus sts = MFX_ERR_NONE;

	HWND window = NULL;
//	bool render = (m_eWorkMode == MODE_RENDERING);

//	if (render) {
//		window = (D3D11_MEMORY == m_memType) ? NULL : m_d3dRender.GetWindowHandle();
//	}

#if MFX_D3D11_SUPPORT
	if (D3D11_MEMORY == m_memType)
		m_hwdev = new CD3D11Device();
	else
#endif // #if MFX_D3D11_SUPPORT
		m_hwdev = new CD3D9Device();

	if (NULL == m_hwdev)
		return MFX_ERR_MEMORY_ALLOC;

//	if (render && m_bIsMVC && m_memType == D3D9_MEMORY) {
//		sts = m_hwdev->SetHandle((mfxHandleType)MFX_HANDLE_GFXS3DCONTROL, m_pS3DControl);
//		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
//	}
	sts = m_hwdev->Init(
		window,
		0,//render ? (m_bIsMVC ? 2 : 1) : 0,
		MSDKAdapter::GetNumber(m_mfxSession));
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

//	if (render)
//		m_d3dRender.SetHWDevice(m_hwdev);
#elif LIBVA_SUPPORT
	mfxStatus sts = MFX_ERR_NONE;
	m_hwdev = CreateVAAPIDevice(m_libvaBackend);

	if (NULL == m_hwdev) {
		return MFX_ERR_MEMORY_ALLOC;
	}

	sts = m_hwdev->Init(&m_monitorType, (m_eWorkMode == MODE_RENDERING) ? 1 : 0, MSDKAdapter::GetNumber(m_mfxSession));
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

#if defined(LIBVA_WAYLAND_SUPPORT)
	if (m_eWorkMode == MODE_RENDERING) {
		mfxHDL hdl = NULL;
		mfxHandleType hdlw_t = (mfxHandleType)HANDLE_WAYLAND_DRIVER;
		Wayland *wld;
		sts = m_hwdev->GetHandle(hdlw_t, &hdl);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
		wld = (Wayland*)hdl;
		wld->SetRenderWinPos(m_nRenderWinX, m_nRenderWinY);
		wld->SetPerfMode(m_bPerfMode);
	}
#endif //LIBVA_WAYLAND_SUPPORT

#endif
	return MFX_ERR_NONE;
}

mfxStatus IntelHWDecoder::ResetDevice()
{
	return m_hwdev->Reset();
}

mfxStatus IntelHWDecoder::decode(unsigned char* pdata, size_t len, unsigned int dts)
{
	dts;// unsigned int tmp = dts;
	if (pdata == NULL || len <= 0)
	{
		return MFX_ERR_NULL_PTR;
	}
	mfxStatus sts = MFX_ERR_NONE;
	if ((m_mfxBS.MaxLength - (m_mfxBS.DataLength+m_mfxBS.DataOffset)) < len)
	{
		sts = ExtendMfxBitstream(&m_mfxBS, m_mfxBS.MaxLength + len);
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
	}
	memcpy(m_mfxBS.Data + m_mfxBS.DataOffset + m_mfxBS.DataLength, pdata, len);
	m_mfxBS.DataLength += len;
	//unsigned char* sequenceHeader = NULL;
	//size_t headerLen = 0;
	////fetch the sequence header and decode it
	//if (MFX_ERR_NONE == getSequenceHeader(pdata, len, &sequenceHeader, &headerLen))
	//{
	//	sts = decodeHeader(sequenceHeader, headerLen);
	//	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
	//}
	if (m_needDecodeHeader)
	{
		sts = decodeHeader(pdata, len);
	}
	//docode the real data
	//m_mfxBS.Data = pdata;
	//m_mfxBS.DataLength = len - headerLen;
	//m_mfxBS.DataOffset = headerLen;
	//m_mfxBS.MaxLength = len;
	mfxFrameSurface1*   pOutSurface = NULL;
	mfxBitstream*       pBitstream = &m_mfxBS;
RetryOutput:
	if ((MFX_ERR_NONE == sts) || (MFX_ERR_MORE_DATA == sts) || (MFX_ERR_MORE_SURFACE == sts)) {
		SyncFrameSurfaces();
		SyncVppFrameSurfaces();
		if (!m_pCurrentFreeSurface) {
			m_pCurrentFreeSurface = m_FreeSurfacesPool.GetSurface();
		}
		if (!m_pCurrentFreeVppSurface) {
			m_pCurrentFreeVppSurface = m_FreeVppSurfacesPool.GetSurface();
		}
#ifndef __SYNC_WA
		if (!m_pCurrentFreeSurface || (!m_pCurrentFreeVppSurface)) {
#else
		if (!m_pCurrentFreeSurface || (!m_pCurrentFreeVppSurface && m_bVppIsUsed) || (m_OutputSurfacesPool.GetSurfaceCount() == m_mfxVideoParams.AsyncDepth)) {
#endif
			// we stuck with no free surface available, now we will sync...
			sts = SyncOutputSurface(MSDK_DEC_WAIT_INTERVAL);
			if (MFX_ERR_MORE_DATA == sts) {
				/*if ((m_eWorkMode == MODE_PERFORMANCE) || (m_eWorkMode == MODE_FILE_DUMP)) {
					sts = MFX_ERR_NOT_FOUND;
				}
				else if (m_eWorkMode == MODE_RENDERING) {
					if (m_synced_count != m_output_count) {
						sts = m_pDeliveredEvent->TimedWait(MSDK_DEC_WAIT_INTERVAL);
					}
					else {
						sts = MFX_ERR_NOT_FOUND;
					}
				}*/
				sts = MFX_ERR_NOT_FOUND;
				if (MFX_ERR_NOT_FOUND == sts) {
					msdk_printf(MSDK_STRING("fatal: failed to find output surface, that's a bug!\n"));
					return MFX_ERR_MORE_SURFACE;
				}
			}
			// note: MFX_WRN_IN_EXECUTION will also be treated as an error at this point
			//continue;
			goto RetryOutput;
		}

		if (!m_pCurrentFreeOutputSurface) {
			m_pCurrentFreeOutputSurface = GetFreeOutputSurface();
		}
		if (!m_pCurrentFreeOutputSurface) {
			sts = MFX_ERR_NOT_FOUND;
			return sts;
		}
	}

	if ((MFX_ERR_NONE == sts) || (MFX_ERR_MORE_DATA == sts) || (MFX_ERR_MORE_SURFACE == sts)) {
		//if (m_bIsCompleteFrame) {
		//	m_pCurrentFreeSurface->submit = m_timer_overall.Sync();
		//}
		pOutSurface = NULL;
		do {
			sts = m_pmfxDEC->DecodeFrameAsync(pBitstream, &(m_pCurrentFreeSurface->frame), &pOutSurface, &(m_pCurrentFreeOutputSurface->syncp));
			//if (pBitstream && MFX_ERR_MORE_DATA == sts && pBitstream->MaxLength == pBitstream->DataLength)
			//{
			//	mfxStatus status = ExtendMfxBitstream(pBitstream, pBitstream->MaxLength * 2);
			//	MSDK_CHECK_RESULT(status, MFX_ERR_NONE, status);
			//}

			if (MFX_WRN_DEVICE_BUSY == sts) {
				//if (m_bIsCompleteFrame) {
					//in low latency mode device busy leads to increasing of latency
					//msdk_printf(MSDK_STRING("Warning : latency increased due to MFX_WRN_DEVICE_BUSY\n"));
				//}
				mfxStatus _sts = SyncOutputSurface(MSDK_DEC_WAIT_INTERVAL);
				// note: everything except MFX_ERR_NONE are errors at this point
				if (MFX_ERR_NONE == _sts) {
					sts = MFX_WRN_DEVICE_BUSY;
				}
				else {
					sts = _sts;
					if (MFX_ERR_MORE_DATA == sts) {
						// we can't receive MFX_ERR_MORE_DATA and have no output - that's a bug
						sts = MFX_WRN_DEVICE_BUSY;//MFX_ERR_NOT_FOUND;
					}
				}
			}
		} while (MFX_WRN_DEVICE_BUSY == sts);

		if (sts > MFX_ERR_NONE) {
			// ignoring warnings...
			if (m_pCurrentFreeOutputSurface->syncp) {
				MSDK_SELF_CHECK(pOutSurface);
				// output is available
				sts = MFX_ERR_NONE;
			}
			else {
				// output is not available
				sts = MFX_ERR_MORE_SURFACE;
			}
		}
		else if ((MFX_ERR_MORE_DATA == sts) && pBitstream) {
			//if (m_bIsCompleteFrame && pBitstream->DataLength)
			//{
			//	// In low_latency mode decoder have to process bitstream completely
			//	msdk_printf(MSDK_STRING("error: Incorrect decoder behavior in low latency mode (bitstream length is not equal to 0 after decoding)\n"));
			//	sts = MFX_ERR_UNDEFINED_BEHAVIOR;
			//	continue;
			//}
		}
		else if ((MFX_ERR_MORE_DATA == sts) && !pBitstream) {
			// that's it - we reached end of stream; now we need to render bufferred data...
			do {
				sts = SyncOutputSurface(MSDK_DEC_WAIT_INTERVAL);
			} while (MFX_ERR_NONE == sts);

			//while (m_synced_count != m_output_count) {
			//	m_pDeliveredEvent->Wait();
			//}

			if (MFX_ERR_MORE_DATA == sts) {
				sts = MFX_ERR_NONE;
			}
			return sts;
		}
		//else if (MFX_ERR_INCOMPATIBLE_VIDEO_PARAM == sts) {
		//	bErrIncompatibleVideoParams = true;
		//	// need to go to the buffering loop prior to reset procedure
		//	pBitstream = NULL;
		//	sts = MFX_ERR_NONE;
		//	continue;
		//}
	}

	if ((MFX_ERR_NONE == sts) || (MFX_ERR_MORE_DATA == sts) || (MFX_ERR_MORE_SURFACE == sts)) {
		// if current free surface is locked we are moving it to the used surfaces array
		/*if (m_pCurrentFreeSurface->frame.Data.Locked)*/{
			m_UsedSurfacesPool.AddSurface(m_pCurrentFreeSurface);
			m_pCurrentFreeSurface = NULL;
		}
	}
	if (MFX_ERR_NONE == sts) {
		if (m_bVppIsUsed)
		{
			/*do {
				if ((m_pCurrentFreeVppSurface->frame.Info.CropW == 0) ||
					(m_pCurrentFreeVppSurface->frame.Info.CropH == 0)) {
					m_pCurrentFreeVppSurface->frame.Info.CropW = pOutSurface->Info.CropW;
					m_pCurrentFreeVppSurface->frame.Info.CropH = pOutSurface->Info.CropH;
					m_pCurrentFreeVppSurface->frame.Info.CropX = pOutSurface->Info.CropX;
					m_pCurrentFreeVppSurface->frame.Info.CropY = pOutSurface->Info.CropY;
				}
				if (pOutSurface->Info.PicStruct != m_pCurrentFreeVppSurface->frame.Info.PicStruct) {
					m_pCurrentFreeVppSurface->frame.Info.PicStruct = pOutSurface->Info.PicStruct;
				}
				if ((pOutSurface->Info.PicStruct == 0) && (m_pCurrentFreeVppSurface->frame.Info.PicStruct == 0)) {
					m_pCurrentFreeVppSurface->frame.Info.PicStruct = pOutSurface->Info.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
				}

				if (m_diMode && m_pCurrentFreeVppSurface)
					m_pCurrentFreeVppSurface->frame.Info.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;

				sts = m_pmfxVPP->RunFrameVPPAsync(pOutSurface, &(m_pCurrentFreeVppSurface->frame), NULL, &(m_pCurrentFreeOutputSurface->syncp));

				if (MFX_WRN_DEVICE_BUSY == sts) {
					MSDK_SLEEP(1); // just wait and then repeat the same call to RunFrameVPPAsync
				}
			} while (MFX_WRN_DEVICE_BUSY == sts);

			// process errors
			if (MFX_ERR_MORE_DATA == sts) { // will never happen actually
				continue;
			}
			else if (MFX_ERR_NONE != sts) {
				break;
			}

			m_UsedVppSurfacesPool.AddSurface(m_pCurrentFreeVppSurface);
			msdk_atomic_inc16(&(m_pCurrentFreeVppSurface->render_lock));

			m_pCurrentFreeOutputSurface->surface = m_pCurrentFreeVppSurface;
			m_OutputSurfacesPool.AddSurface(m_pCurrentFreeOutputSurface);

			m_pCurrentFreeOutputSurface = NULL;
			m_pCurrentFreeVppSurface = NULL;*/
		}
		else
		{
			msdkFrameSurface* surface = FindUsedSurface(pOutSurface);

			msdk_atomic_inc16(&(surface->render_lock));

			m_pCurrentFreeOutputSurface->surface = surface;
			m_OutputSurfacesPool.AddSurface(m_pCurrentFreeOutputSurface);
			m_pCurrentFreeOutputSurface = NULL;
		}
	}
	return sts;
}

mfxStatus IntelHWDecoder::getSequenceHeader(unsigned char* psrc, size_t srcLen, unsigned char** ppSeqHeader, size_t* seqHeaderLen)
{
	if (ppSeqHeader == NULL || NULL == seqHeaderLen || psrc==NULL || srcLen<=0)
		return MFX_ERR_NULL_PTR;
	if (srcLen <= 5)
		return MFX_ERR_MORE_DATA;
	unsigned char* ppps = NULL;
	unsigned char* psps = NULL;
	unsigned char* pcur = psrc;
	while ((size_t)(pcur-psrc)<(srcLen-5))
	{
		if (0x00 == pcur[0] && 0x00 == pcur[1] && 0x00 == pcur[2] && 0x01 == pcur[3])
		{
			int type = 0x1f & pcur[4];
			if ( type == 7)//sps
			{
				psps = pcur;
				pcur += 5;
			}
			else if(type==8)
			{
				ppps = pcur;
				pcur += 5;
			}
			else
			{
				*ppSeqHeader = (psps && psps < ppps) ? psps : ppps;
				if (*ppSeqHeader)
				{
					*seqHeaderLen = pcur - (*ppSeqHeader);
					return MFX_ERR_NONE;
				}
				return MFX_ERR_NOT_FOUND;
			}
		}
		else
		{
			++pcur;
		}
	}
	return MFX_ERR_NOT_FOUND;
}

mfxStatus IntelHWDecoder::decodeHeader(unsigned char* sequenceHeader, size_t len)
{
	if (sequenceHeader == NULL || len <= 0)
		return MFX_ERR_NULL_PTR;
	mfxStatus sts = MFX_ERR_UNKNOWN;
	DecoderInitParam pParams;
	pParams.videoType = m_videoType;
	pParams.pByteSequenceHeader = sequenceHeader;
	pParams.nBytesOfSeqHeader = len;
	pParams.nAsyncDepth = m_nAsyncDepth;
	pParams.fourcc = m_fourcc;
	//m_mfxBS.Data = sequenceHeader;
	//m_mfxBS.DataLength = len;
	//m_mfxBS.DataOffset = 0;
	pParams.nOutWidth = m_vppOutWidth;
	pParams.nOutHeight = m_vppOutHeight;
	
	sts = InitMfxParams(&pParams);
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	//	if (m_bVppIsUsed)
	//		m_bDecOutSysmem = pParams->bUseHWLib ? false : true;
	//	else
	//		m_bDecOutSysmem = pParams->memType == SYSTEM_MEMORY;
	m_bDecOutSysmem = false;

	if (m_bVppIsUsed)
	{
		m_pmfxVPP = new MFXVideoVPP(m_mfxSession);
		if (!m_pmfxVPP) return MFX_ERR_MEMORY_ALLOC;
	}

	//	m_eWorkMode = pParams->mode;
	//	if (m_eWorkMode == MODE_FILE_DUMP) {
	//		// prepare YUV file writer
	//		sts = m_FileWriter.Init(pParams->strDstFile, pParams->numViews);
	//	}
	//	else if ((m_eWorkMode != MODE_PERFORMANCE) && (m_eWorkMode != MODE_RENDERING)) {
	//		msdk_printf(MSDK_STRING("error: unsupported work mode\n"));
	//		sts = MFX_ERR_UNSUPPORTED;
	//	}
	//	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	//	m_monitorType = pParams->monitorType;
	// create device and allocator
#if defined(LIBVA_SUPPORT)
	m_libvaBackend = pParams->libvaBackend;
#endif // defined(MFX_LIBVA_SUPPORT)

	sts = CreateAllocator();
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	// in case of HW accelerated decode frames must be allocated prior to decoder initialization
	sts = AllocFrames();
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	sts = m_pmfxDEC->Init(&m_mfxVideoParams);
	if (MFX_WRN_PARTIAL_ACCELERATION == sts)
	{
		msdk_printf(MSDK_STRING("WARNING: partial acceleration\n"));
		MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
	}
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	if (m_bVppIsUsed)
	{
		//if (m_diMode)
		//	m_mfxVppVideoParams.vpp.Out.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;

		sts = m_pmfxVPP->Init(&m_mfxVppVideoParams);
		if (MFX_WRN_PARTIAL_ACCELERATION == sts)
		{
			msdk_printf(MSDK_STRING("WARNING: partial acceleration\n"));
			MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
		}
		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
	}

	sts = m_pmfxDEC->GetVideoParam(&m_mfxVideoParams);
	MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

	//	if (m_eWorkMode == MODE_RENDERING)
	//	{
	//		sts = CreateRenderingWindow(pParams, m_bIsMVC && (m_memType == D3D9_MEMORY));
	//		MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
	//	}

	m_needDecodeHeader = false;
	return sts;
}

mfxStatus IntelHWDecoder::SyncOutputSurface(mfxU32 wait)
{
	if (!m_pCurrentOutputSurface) {
		m_pCurrentOutputSurface = m_OutputSurfacesPool.GetSurface();
	}
	if (!m_pCurrentOutputSurface) {
		return MFX_ERR_MORE_DATA;
	}

	mfxStatus sts = m_mfxSession.SyncOperation(m_pCurrentOutputSurface->syncp, wait);

	if (MFX_WRN_IN_EXECUTION == sts) {
		return sts;
	}
	if (MFX_ERR_NONE == sts) {
		// we got completely decoded frame - pushing it to the delivering thread...
		//++m_synced_count;
		//if (m_bPrintLatency) {
		//	m_vLatency.push_back(m_timer_overall.Sync() - m_pCurrentOutputSurface->surface->submit);
		//}
		//else {
		//	PrintPerFrameStat();
		//}

		sts = DeliverOutput(&(m_pCurrentOutputSurface->surface->frame));

		//if (m_eWorkMode == MODE_PERFORMANCE) {
		//	m_output_count = m_synced_count;
		//	ReturnSurfaceToBuffers(m_pCurrentOutputSurface);
		//}
		//else if (m_eWorkMode == MODE_FILE_DUMP) {
		//	m_output_count = m_synced_count;
		//	sts = DeliverOutput(&(m_pCurrentOutputSurface->surface->frame));
		//	if (MFX_ERR_NONE != sts) {
		//		sts = MFX_ERR_UNKNOWN;
		//	}
		//	ReturnSurfaceToBuffers(m_pCurrentOutputSurface);
		//}
		//else if (m_eWorkMode == MODE_RENDERING) {
		//	if (m_nMaxFps)
		//	{
		//		//calculation of a time to sleep in order not to exceed a given fps
		//		mfxF64 currentTime = (m_output_count) ? CTimer::ConvertToSeconds(m_tick_overall) : 0.0;
		//		int time_to_sleep = (int)(1000 * ((double)m_output_count / m_nMaxFps - currentTime));
		//		if (time_to_sleep > 0)
		//		{
		//			MSDK_SLEEP(time_to_sleep);
		//		}
		//	}
		//	m_DeliveredSurfacesPool.AddSurface(m_pCurrentOutputSurface);
		//	m_pDeliveredEvent->Reset();
		//	m_pDeliverOutputSemaphore->Post();
		//}
		ReturnSurfaceToBuffers(m_pCurrentOutputSurface);
		m_pCurrentOutputSurface = NULL;
	}

	if (MFX_ERR_NONE != sts) {
		sts = MFX_ERR_UNKNOWN;
	}

	return sts;
}

mfxStatus IntelHWDecoder::DeliverOutput(mfxFrameSurface1* frame)
{
	//CAutoTimer timer_fwrite(m_tick_fwrite);

	mfxStatus res = MFX_ERR_NONE, sts = MFX_ERR_NONE;

	if (!frame) {
		return MFX_ERR_NULL_PTR;
	}

	if (m_bExternalAlloc) {
//		if (m_eWorkMode == MODE_FILE_DUMP) {
			res = m_pGeneralAllocator->Lock(m_pGeneralAllocator->pthis, frame->Data.MemId, &(frame->Data));
			if (MFX_ERR_NONE == res) {
				res = m_FileWriter.WriteNextFrame(frame);
				sts = m_pGeneralAllocator->Unlock(m_pGeneralAllocator->pthis, frame->Data.MemId, &(frame->Data));
			}
			if ((MFX_ERR_NONE == res) && (MFX_ERR_NONE != sts)) {
				res = sts;
			}
//		}
//		else if (m_eWorkMode == MODE_RENDERING) {
//#if D3D_SURFACES_SUPPORT
//			res = m_d3dRender.RenderFrame(frame, m_pGeneralAllocator);
//#elif LIBVA_SUPPORT
//			res = m_hwdev->RenderFrame(frame, m_pGeneralAllocator);
//#endif
//		}
	}
	else {
		res = m_FileWriter.WriteNextFrame(frame);
	}

	return res;
}

mfxStatus IntelHWDecoder::getDecodedParam(VideoParam& param)
{
	mfxStatus res = MFX_ERR_NONE;
	param;
	return res;
}
mfxStatus IntelHWDecoder::getDecodedData(unsigned char* output, int &outLen, int& pts)
{
	mfxStatus res = MFX_ERR_NONE;
	output; outLen; pts;
	return res;
}

void IntelHWDecoder::Close()
{

}

mfxStatus IntelHWDecoder::ResetDecoder(DecoderInitParam *pParams)
{
	mfxStatus res = MFX_ERR_NONE;
	pParams;
	return res;
}