#ifndef _INTEL_HW_DECODER_H_
#define _INTEL_HW_DECODER_H_

#include "mfxdefs.h"

#include <vector>
#include <memory>
#include "mfx_buffering.h"
#include "sample_defs.h"
#include "general_allocator.h"
#include "base_allocator.h"
#include "hw_device.h"
#include "commondef.h"

struct DecoderInitParam
{
	//default is use HW lib
	mfxU32 videoType;
	MemType memType;
	bool    bIsMVC; // true if Multi-View Codec is in use
	bool    bLowLat; // low latency mode
	bool    bCalLat; // latency calculation, for print output
	mfxU32  nWallTimeout; //timeout for -wall option
	mfxU16  nAsyncDepth; // asyncronous queue
	mfxU16  gpuCopy; // GPU Copy mode (three-state option)
	mfxU16  nThreadsNum;
	mfxI32  SchedulingType;
	mfxI32  Priority;
	bool    bPerfMode;
	mfxU8*	pByteSequenceHeader; //data of sequence header for init decoder
	mfxU32  nBytesOfSeqHeader;   //length of sequence header data
	mfxU16  nOutWidth;
	mfxU16  nOutHeight;
	mfxU32  fourcc;

//	mfxI32  monitorType;
#if defined(LIBVA_SUPPORT)
	mfxI32  libvaBackend;
#endif // defined(MFX_LIBVA_SUPPORT)

	//sPluginParams pluginParams;

	DecoderInitParam()
	{
		MSDK_ZERO_MEMORY(*this);
	}
}; //struct DecoderInitParam

struct VideoParam
{
	int width;
	int height;
	int forcc;

	bool valid() const { return !(width <= 0 || height <= 0 || forcc == 0); }
};

class IntelHWDecoder :
	public CBuffering
	//public CPipelineStatistics
{
public:
	IntelHWDecoder(const TCHAR* outfilename);
	virtual ~IntelHWDecoder();

	mfxStatus init(DecoderInitParam* pParams);
	mfxStatus decode(unsigned char* pdata, size_t len, unsigned int dts);
	/**
	* Method		getDecodedParam
	* @breif		获得解码后的数据对应参数
	* @param[out]	MediaParam para
	* @return		SUCCESS 成功
	FAIL_CODEC_FRAME_UNDECODED 尚未解码
	*/
	virtual mfxStatus getDecodedParam(VideoParam& param);
	virtual mfxStatus getDecodedData(unsigned char* output, int &outLen, int& pts);

	//virtual mfxStatus Init(sInputParams *pParams);
	//virtual mfxStatus RunDecoding();
	virtual void Close();
	virtual mfxStatus ResetDecoder(DecoderInitParam *pParams);
	virtual mfxStatus ResetDevice();

	void SetMultiView();
	//void SetExtBuffersFlag()       { m_bIsExtBuffers = true; }
	//virtual void PrintInfo();

protected: // functions
	//virtual mfxStatus CreateRenderingWindow(sInputParams *pParams, bool try_s3d);
	virtual mfxStatus InitMfxParams(DecoderInitParam *pParams);

	// function for allocating a specific external buffer
	//template <typename Buffer>
	//mfxStatus AllocateExtBuffer();
	//virtual void DeleteExtBuffers();

	//virtual mfxStatus AllocateExtMVCBuffers();
	//virtual void    DeallocateExtMVCBuffers();

	//virtual void AttachExtParam();

	virtual mfxStatus InitVppParams();
	virtual mfxStatus AllocAndInitVppFilters();
	virtual bool IsVppRequired(DecoderInitParam *pParams);

	virtual mfxStatus CreateAllocator();
	virtual mfxStatus CreateHWDevice();
	virtual mfxStatus AllocFrames();
	virtual void DeleteFrames();
	virtual void DeleteAllocator();

	/** \brief Performs SyncOperation on the current output surface with the specified timeout.
	*
	* @return MFX_ERR_NONE Output surface was successfully synced and delivered.
	* @return MFX_ERR_MORE_DATA Array of output surfaces is empty, need to feed decoder.
	* @return MFX_WRN_IN_EXECUTION Specified timeout have elapsed.
	* @return MFX_ERR_UNKNOWN An error has occurred.
	*/
	virtual mfxStatus SyncOutputSurface(mfxU32 wait);
	virtual mfxStatus DeliverOutput(mfxFrameSurface1* frame);
	mfxStatus getFrameData(mfxFrameSurface1* frame, unsigned char* buf, int& bufLen);
	mfxStatus copyFrameData(mfxFrameSurface1* pSurface, unsigned char* buf, int& bufLen);
		//virtual void PrintPerFrameStat(bool force = false);

	//virtual mfxStatus DeliverLoop(void);

	//static unsigned int MFX_STDCALL DeliverThreadFunc(void* ctx);

protected: // variables
	CSmplYUVWriter          m_FileWriter;
	//std::auto_ptr<CSmplBitstreamReader>  m_FileReader;
	mfxBitstream            m_mfxBS; // contains encoded data

	MFXVideoSession         m_mfxSession;
	mfxIMPL                 m_impl;
	MFXVideoDECODE*         m_pmfxDEC;
	MFXVideoVPP*            m_pmfxVPP;
	mfxVideoParam           m_mfxVideoParams;
	mfxVideoParam           m_mfxVppVideoParams;
//	std::auto_ptr<MFXVideoUSER>  m_pUserModule;
//	std::auto_ptr<MFXPlugin> m_pPlugin;
	std::vector<mfxExtBuffer *> m_ExtBuffers;
	bool                    m_bIsCompleteFrame;
	bool                    m_bPrintLatency;
	MemType                 m_memType;      // memory type of surfaces to use
	mfxU32					m_videoType;	//video type of the src encoded data
	mfxU32                  m_fourcc; // color format of vpp out, i420 by default
	bool                    m_bDecOutSysmem; // use system memory between Decoder and VPP, if false - video memory
	bool                    m_bVppIsUsed;
	GeneralAllocator*       m_pGeneralAllocator;
	mfxAllocatorParams*     m_pmfxAllocatorParams;
	bool                    m_bExternalAlloc; // use memory allocator as external for Media SDK
	mfxFrameAllocResponse   m_mfxResponse; // memory allocation response for decoder
	mfxFrameAllocResponse   m_mfxVppResponse;   // memory allocation response for vpp
	CHWDevice               *m_hwdev;
	mfxU16					m_gpuCopy;
	mfxU16					m_nAsyncDepth; // asyncronous queue
	mfxU16                  m_vppOutWidth;
	mfxU16                  m_vppOutHeight;

	msdkFrameSurface*       m_pCurrentFreeSurface; // surface detached from free surfaces array
	msdkFrameSurface*       m_pCurrentFreeVppSurface; // VPP surface detached from free VPP surfaces array
	msdkOutputSurface*      m_pCurrentFreeOutputSurface; // surface detached from free output surfaces array
	msdkOutputSurface*      m_pCurrentOutputSurface; // surface detached from output surfaces array
	mfxExtVPPDoNotUse       m_VppDoNotUse;      // for disabling VPP algorithms
	mfxExtVPPDeinterlacing  m_VppDeinterlacing;
	std::vector<mfxExtBuffer*> m_VppExtParams;
	mfxU16                  m_diMode;//标识是否启用反交错
	bool					m_needDecodeHeader;
	VideoParam				m_vFrameParam;
	/*

	MSDKSemaphore*          m_pDeliverOutputSemaphore; // to access to DeliverOutput method
	MSDKEvent*              m_pDeliveredEvent; // to signal when output surfaces will be processed
	mfxStatus               m_error; // error returned by DeliverOutput method
	bool                    m_bStopDeliverLoop;

	eWorkMode               m_eWorkMode; // work mode for the pipeline
	bool                    m_bIsMVC; // enables MVC mode (need to support several files as an output)
	bool                    m_bIsExtBuffers; // indicates if external buffers were allocated
	bool                    m_bIsVideoWall; // indicates special mode: decoding will be done in a loop

	mfxU32                  m_nTimeout; // enables timeout for video playback, measured in seconds
	mfxU32                  m_nMaxFps; // limit of fps, if isn't specified equal 0.
	mfxU32                  m_nFrames; //limit number of output frames

	std::vector<msdk_tick>  m_vLatency;

#if D3D_SURFACES_SUPPORT
	IGFXS3DControl          *m_pS3DControl;

	CDecodeD3DRender         m_d3dRender;
#endif

	bool                    m_bRenderWin;
	mfxU32                  m_nRenderWinX;
	mfxU32                  m_nRenderWinY;
	mfxU32                  m_nRenderWinW;
	mfxU32                  m_nRenderWinH;

	mfxU32                  m_export_mode;
	mfxI32                  m_monitorType;
#if defined(LIBVA_SUPPORT)
	mfxI32                  m_libvaBackend;
	bool                    m_bPerfMode;
#endif // defined(MFX_LIBVA_SUPPORT)
	*/
private:
	IntelHWDecoder(const IntelHWDecoder&);
	void operator=(const IntelHWDecoder&);

	mfxStatus getSequenceHeader(unsigned char* psrc, size_t srcLen, unsigned char** ppSeqHeader, size_t* seqHeaderLen);
	mfxStatus decodeHeader(unsigned char* sequenceHeader, size_t len);
};

#endif //_INTEL_HW_DECODER_H_