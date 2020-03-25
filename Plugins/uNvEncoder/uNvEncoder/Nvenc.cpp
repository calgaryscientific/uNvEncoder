#include <string>
#include <map>
#include "Nvenc.h"


namespace uNvEncoder
{


void OutputNvencApiError(const std::string &apiName, NVENCSTATUS status)
{
#define STATUS_STR_PAIR(Code) { Code, #Code },
    static const std::map<NVENCSTATUS, std::string> nvEncStatusErrorNameTable = {
        STATUS_STR_PAIR(NV_ENC_SUCCESS)
        STATUS_STR_PAIR(NV_ENC_ERR_NO_ENCODE_DEVICE)
        STATUS_STR_PAIR(NV_ENC_ERR_UNSUPPORTED_DEVICE)
        STATUS_STR_PAIR(NV_ENC_ERR_INVALID_ENCODERDEVICE)
        STATUS_STR_PAIR(NV_ENC_ERR_INVALID_DEVICE)
        STATUS_STR_PAIR(NV_ENC_ERR_DEVICE_NOT_EXIST)
        STATUS_STR_PAIR(NV_ENC_ERR_INVALID_PTR)
        STATUS_STR_PAIR(NV_ENC_ERR_INVALID_EVENT)
        STATUS_STR_PAIR(NV_ENC_ERR_INVALID_PARAM)
        STATUS_STR_PAIR(NV_ENC_ERR_INVALID_CALL)
        STATUS_STR_PAIR(NV_ENC_ERR_OUT_OF_MEMORY)
        STATUS_STR_PAIR(NV_ENC_ERR_ENCODER_NOT_INITIALIZED)
        STATUS_STR_PAIR(NV_ENC_ERR_UNSUPPORTED_PARAM)
        STATUS_STR_PAIR(NV_ENC_ERR_LOCK_BUSY)
        STATUS_STR_PAIR(NV_ENC_ERR_NOT_ENOUGH_BUFFER)
        STATUS_STR_PAIR(NV_ENC_ERR_INVALID_VERSION)
        STATUS_STR_PAIR(NV_ENC_ERR_MAP_FAILED)
        STATUS_STR_PAIR(NV_ENC_ERR_NEED_MORE_INPUT)
        STATUS_STR_PAIR(NV_ENC_ERR_ENCODER_BUSY)
        STATUS_STR_PAIR(NV_ENC_ERR_EVENT_NOT_REGISTERD)
        STATUS_STR_PAIR(NV_ENC_ERR_GENERIC)
        STATUS_STR_PAIR(NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY)
        STATUS_STR_PAIR(NV_ENC_ERR_UNIMPLEMENTED)
        STATUS_STR_PAIR(NV_ENC_ERR_RESOURCE_REGISTER_FAILED)
        STATUS_STR_PAIR(NV_ENC_ERR_RESOURCE_NOT_REGISTERED)
        STATUS_STR_PAIR(NV_ENC_ERR_RESOURCE_NOT_MAPPED)
    };
#undef STATUS_STR_PAIR

    const auto it = nvEncStatusErrorNameTable.find(status);
    const auto statusStr = it != nvEncStatusErrorNameTable.end() ? it->second : "Unknown";
    ThrowError(apiName + " call failed: " + statusStr);
}


template <class Api, class ...Args>
NVENCSTATUS CallNvencApi(const std::string &apiName, const Api &api, const Args &... args)
{
    const auto status = api(args...);
    if (status != NV_ENC_SUCCESS && status != NV_ENC_ERR_NEED_MORE_INPUT)
    {
        OutputNvencApiError(apiName, status);
    }

    return status;
}


#define CALL_NVENC_API(Api, ...) CallNvencApi(#Api, Api, __VA_ARGS__)



decltype(Nvenc::s_module) Nvenc::s_module = NULL;
decltype(Nvenc::s_nvenc) Nvenc::s_nvenc = { 0 };
decltype(Nvenc::s_referenceCount) Nvenc::s_referenceCount = 0;


void Nvenc::LoadModule()
{
    ++s_referenceCount;

    if (s_module != NULL) return;

#if defined(_WIN64)
    s_module = ::LoadLibraryA("nvEncodeAPI64.dll");
#else
    s_module = ::LoadLibraryA("nvEncodeAPI.dll");
#endif
    if (s_module == NULL) ThrowError("NVENC is not available.");

    if (const auto funcAddress = ::GetProcAddress(s_module, "NvEncodeAPIGetMaxSupportedVersion"))
    {
        using FuncType = decltype(NvEncodeAPIGetMaxSupportedVersion);
        const auto func = reinterpret_cast<FuncType*>(funcAddress);
        uint32_t version = 0;
        const auto res = func(&version);
        constexpr uint32_t currentVersion = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
        if (currentVersion > version) ThrowError("NVENC version is wrong.");
    }

    if (const auto funcAddress = ::GetProcAddress(s_module, "NvEncodeAPICreateInstance"))
    {
        using FuncType = decltype(NvEncodeAPICreateInstance);
        const auto func = reinterpret_cast<FuncType*>(funcAddress);
        s_nvenc = { NV_ENCODE_API_FUNCTION_LIST_VER };
        func(&s_nvenc);

        if (!s_nvenc.nvEncOpenEncodeSession)
        {
            ThrowError("Failed to load functions from DLL.");
        }
    }
}


void Nvenc::UnloadModule()
{
    if (--s_referenceCount > 0) return;

    if (s_module != NULL)
    {
        ::FreeLibrary(s_module);
        s_module = NULL;
    }
}


Nvenc::Nvenc(const NvencDesc &desc)
    : desc_(desc)
    , resources_(1)
{
}


Nvenc::~Nvenc()
{
}


void Nvenc::Initialize()
{
    if (isInitialized_) return;

    LoadModule();
    OpenEncodeSession();
    InitializeEncoder();

    CreateCompletionEvents();
    CreateInputTextures();
    RegisterResources();
    CreateBitstreamBuffers();

    isInitialized_ = true;
}


void Nvenc::Finalize()
{
    if (!isInitialized_) return;

    EndEncode();
    DestroyBitstreamBuffers();
    UnregisterResources();
    DestroyCompletionEvents();
    DestroyEncoder();
    UnloadModule();

    isInitialized_ = false;
}


void Nvenc::ThrowErrorIfNotInitialized()
{
    if (!IsValid()) ThrowError("NVENC has not been initialized yet.");
}


void Nvenc::OpenEncodeSession()
{
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS encSessionParams = { NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER };
    encSessionParams.device = desc_.d3d11Device.Get();
    encSessionParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    encSessionParams.apiVersion = NVENCAPI_VERSION;
    CALL_NVENC_API(s_nvenc.nvEncOpenEncodeSessionEx, &encSessionParams, &encoder_);
}

void Nvenc::Resize(const uint32_t width, const uint32_t height)
{
    if (desc_.width == width && desc_.height == height) return;

    EndEncode();
    ::fprintf(stdout, "EndEncode\n");

    DestroyBitstreamBuffers();
    ::fprintf(stdout, "DestroyBitStream\n");

    UnregisterResources();
    ::fprintf(stdout, "Unregister resource\ns");

    

    NV_ENC_RECONFIGURE_PARAMS reconfigureParams = { NV_ENC_RECONFIGURE_PARAMS_VER };
    memcpy(&reconfigureParams.reInitEncodeParams, &initializeParams_, sizeof(initializeParams_));
    ::fprintf(stdout, "reconfigureParams.reInitEncodeParams %p\n", &reconfigureParams.reInitEncodeParams);

    NV_ENC_CONFIG reInitCodecConfig = { NV_ENC_CONFIG_VER };

    memcpy(&reInitCodecConfig, initializeParams_.encodeConfig, sizeof(reInitCodecConfig));
    ::fprintf(stdout, "reInitCodecConfig %p\n", &reInitCodecConfig);

    reconfigureParams.reInitEncodeParams.encodeConfig = &reInitCodecConfig;

    
    reconfigureParams.reInitEncodeParams.encodeWidth = width;
    reconfigureParams.reInitEncodeParams.encodeHeight = height;
    reconfigureParams.reInitEncodeParams.darWidth = width;
    reconfigureParams.reInitEncodeParams.darHeight = height;

    ::fprintf(stdout, "reInitCodecConfig %p\n", encoder_);
    CALL_NVENC_API(s_nvenc.nvEncReconfigureEncoder, encoder_, &reconfigureParams);

    desc_.width = width;
    desc_.height = height;

    ::fprintf(stdout, "CreateInputTextures\n");
    CreateInputTextures();

    ::fprintf(stdout, "Register resources\n");
    RegisterResources();

    ::fprintf(stdout, "CreateBitstreamBuffers\n");
    CreateBitstreamBuffers();
}



void Nvenc::InitializeEncoder()
{
    NV_ENC_INITIALIZE_PARAMS initParams = { NV_ENC_INITIALIZE_PARAMS_VER };
    initParams.encodeGUID = NV_ENC_CODEC_H264_GUID;
    initParams.presetGUID = NV_ENC_PRESET_LOW_LATENCY_DEFAULT_GUID;
    initParams.encodeWidth = desc_.width;
    initParams.encodeHeight = desc_.height;
    initParams.darWidth = desc_.width;
    initParams.darHeight = desc_.height;
    initParams.frameRateNum = desc_.frameRate;
    initParams.frameRateDen = 1;
    initParams.enablePTD = 1;
    initParams.reportSliceOffsets = 0;
    initParams.enableSubFrameWrite = 0;
    initParams.maxEncodeWidth = 4096;
    initParams.maxEncodeHeight = 4096;
    initParams.enableMEOnlyMode = false;
    initParams.enableOutputInVidmem = false;
    initParams.enableEncodeAsync = true;

    uint32_t bitRate = (static_cast<unsigned int>(12.0f * desc_.width * desc_.height) / (1920 * 1080))* (100 * 10000);


    NV_ENC_PRESET_CONFIG presetConfig = { NV_ENC_PRESET_CONFIG_VER, { NV_ENC_CONFIG_VER } };
    CALL_NVENC_API(s_nvenc.nvEncGetEncodePresetConfig, encoder_, initParams.encodeGUID, initParams.presetGUID, &presetConfig);

    NV_ENC_CONFIG config = { NV_ENC_CONFIG_VER };
    memcpy(&config, &presetConfig.presetCfg, sizeof(NV_ENC_CONFIG));
    config.profileGUID = NV_ENC_H264_PROFILE_BASELINE_GUID;
    config.frameIntervalP = 1;
    config.gopLength = NVENC_INFINITE_GOPLENGTH;
    config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
    config.rcParams.targetQuality = 20;
    config.rcParams.maxBitRate = bitRate;
    initParams.encodeConfig = &config;

    config.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
    config.encodeCodecConfig.h264Config.maxNumRefFrames = 0;
    config.encodeCodecConfig.h264Config.idrPeriod = config.gopLength;

    CALL_NVENC_API(s_nvenc.nvEncInitializeEncoder, encoder_, &initParams);

    memcpy(&encodeConfig_, &config, sizeof(config));
    memcpy(&initializeParams_, &initParams, sizeof(initializeParams_));

    initializeParams_.encodeConfig = &encodeConfig_;
}


void Nvenc::CreateCompletionEvents()
{
    ThrowErrorIfNotInitialized();

    for (auto &resource : resources_)
    {
        resource.completionEvent_ = ::CreateEventA(NULL, FALSE, FALSE, NULL);
        NV_ENC_EVENT_PARAMS eventParams = { NV_ENC_EVENT_PARAMS_VER };
        eventParams.completionEvent = resource.completionEvent_;
        CALL_NVENC_API(s_nvenc.nvEncRegisterAsyncEvent, encoder_, &eventParams);
    }
}


void Nvenc::DestroyCompletionEvents()
{
    ThrowErrorIfNotInitialized();

    for (auto &resource : resources_)
    {
        if (!resource.completionEvent_) continue;

        NV_ENC_EVENT_PARAMS eventParams = { NV_ENC_EVENT_PARAMS_VER };
        eventParams.completionEvent = resource.completionEvent_;
        CALL_NVENC_API(s_nvenc.nvEncUnregisterAsyncEvent, encoder_, &eventParams);
        ::CloseHandle(resource.completionEvent_);
    }
}


void Nvenc::CreateBitstreamBuffers()
{
    ThrowErrorIfNotInitialized();

    for (auto &resource : resources_)
    {
        NV_ENC_CREATE_BITSTREAM_BUFFER createBitstreamBuffer = { NV_ENC_CREATE_BITSTREAM_BUFFER_VER };
        CALL_NVENC_API(s_nvenc.nvEncCreateBitstreamBuffer, encoder_, &createBitstreamBuffer);
        resource.bitstreamBuffer_ = createBitstreamBuffer.bitstreamBuffer;
    }
}


void Nvenc::CreateInputTextures()
{
    ThrowErrorIfNotInitialized();

    D3D11_TEXTURE2D_DESC desc = { 0 };
    desc.Width = desc_.width;
    desc.Height = desc_.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = desc_.format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    for (auto &resource : resources_)
    {
        if (FAILED(desc_.d3d11Device->CreateTexture2D(&desc, NULL, &resource.inputTexture_)))
        {
            ThrowError("Failed to create shared texture.");
            return;
        }

        ComPtr<IDXGIResource> dxgiResource;
        resource.inputTexture_.As(&dxgiResource);
        if (FAILED(dxgiResource->GetSharedHandle(&resource.inputTextureSharedHandle_)))
        {
            ThrowError("Failed to get shared handle.");
            return;
        }
    }
}


void Nvenc::RegisterResources()
{
    ThrowErrorIfNotInitialized();

    for (auto &resource : resources_)
    {
        NV_ENC_REGISTER_RESOURCE registerResource = { NV_ENC_REGISTER_RESOURCE_VER };
        registerResource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        registerResource.resourceToRegister = resource.inputTexture_.Get();
        registerResource.width = desc_.width;
        registerResource.height = desc_.height;
        registerResource.pitch = 0;
        registerResource.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
        registerResource.bufferUsage = NV_ENC_INPUT_IMAGE;
        CALL_NVENC_API(s_nvenc.nvEncRegisterResource, encoder_, &registerResource);

        resource.registeredResource_ = registerResource.registeredResource;
    }
}


void Nvenc::UnregisterResources()
{
    ThrowErrorIfNotInitialized();

    for (auto &resource : resources_)
    {
       if (!resource.registeredResource_) continue;
        CALL_NVENC_API(s_nvenc.nvEncUnregisterResource, encoder_, resource.registeredResource_);
    }
}


void Nvenc::DestroyBitstreamBuffers()
{
    ThrowErrorIfNotInitialized();

    for (auto &resource : resources_)
    {
        if (!resource.bitstreamBuffer_) continue;
        CALL_NVENC_API(s_nvenc.nvEncDestroyBitstreamBuffer, encoder_, resource.bitstreamBuffer_);
    }
}


void Nvenc::DestroyEncoder()
{
    ThrowErrorIfNotInitialized();

    CALL_NVENC_API(s_nvenc.nvEncDestroyEncoder, encoder_);
}


bool Nvenc::Encode(const ComPtr<ID3D11Texture2D> &source, bool forceIdrFrame)
{
    ThrowErrorIfNotInitialized();

    const auto index = GetInputIndex();
    auto &resource = resources_[index];

    if (resource.isEncoding_) 
    {
		return false;
    }
    resource.isEncoding_ = true;

	if (!CopyToInputTexture(index, source))
	{
		resource.isEncoding_ = false;
		return false;
	}

    MapInputResource(index);

    if (EncodeInputTexture(index, forceIdrFrame)) 
    {
        ++inputIndex_;
		return true;
    }
    else
    {
        resource.isEncoding_ = false;
		return false;
    }
}


bool Nvenc::CopyToInputTexture(int index, const ComPtr<ID3D11Texture2D> &texture)
{
    ThrowErrorIfNotInitialized();

    auto &resource = resources_[index];
    ComPtr<ID3D11Texture2D> inputTexture;

    if (FAILED(GetUnityDevice()->OpenSharedResource(
        resource.inputTextureSharedHandle_,
        __uuidof(ID3D11Texture2D),
        &inputTexture)))
    {
		::fprintf(stdout, "Unable to open shared texture, %d", GetLastError());
		return false;
    }

    ComPtr<ID3D11DeviceContext> context;
    GetUnityDevice()->GetImmediateContext(&context);
    context->CopyResource(inputTexture.Get(), texture.Get());
    context->Flush();

	inputTexture->Release();
	return true;
}


bool Nvenc::EncodeInputTexture(int index, bool forceIdrFrame)
{
    ThrowErrorIfNotInitialized();

    auto &resource = resources_[index];

    NV_ENC_PIC_PARAMS picParams = { NV_ENC_PIC_PARAMS_VER };
    picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    picParams.inputBuffer = resource.inputResource_;
    picParams.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB;
    picParams.inputWidth = desc_.width;
    picParams.inputHeight = desc_.height;
    picParams.outputBitstream = resource.bitstreamBuffer_;
    picParams.completionEvent = resource.completionEvent_;
    picParams.frameIdx = static_cast<uint32_t>(inputIndex_);
    if (forceIdrFrame)
    {
        picParams.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    }

    const auto status = CALL_NVENC_API(s_nvenc.nvEncEncodePicture, encoder_, &picParams);
    if (status != NV_ENC_SUCCESS && status != NV_ENC_ERR_NEED_MORE_INPUT)
    {
        return false;
    }

    return true;
}


void Nvenc::MapInputResource(int index)
{
    ThrowErrorIfNotInitialized();

    auto &resource = resources_[index];
    if (!resource.registeredResource_) return;

    NV_ENC_MAP_INPUT_RESOURCE mapInputResource = { NV_ENC_MAP_INPUT_RESOURCE_VER };
    mapInputResource.registeredResource = resource.registeredResource_;
    CALL_NVENC_API(s_nvenc.nvEncMapInputResource, encoder_, &mapInputResource);
    resource.inputResource_ = mapInputResource.mappedResource;
}


void Nvenc::UnmapInputResource(int index)
{
    ThrowErrorIfNotInitialized();

    auto &resource = resources_[index];

    if (!resource.inputResource_)
    {
        CALL_NVENC_API(s_nvenc.nvEncUnmapInputResource, encoder_, resource.inputResource_);
        resource.inputResource_ = nullptr;
    }
}


void Nvenc::GetEncodedData(std::vector<NvencEncodedData> &data)
{
    ThrowErrorIfNotInitialized();

    for (;outputIndex_ < inputIndex_; ++outputIndex_)
    {
        const auto index = GetOutputIndex();
        auto &resource = resources_[index];

        if (!resource.isEncoding_) 
        {
            ThrowError("Try to get an invalid bitstream.");
            continue;
        }

        constexpr DWORD duration = 10000;
        if (!WaitForCompletion(index, duration))
        {
            ThrowError("Timeout when getting an encoded bitstream.");
            continue;
        }

        NV_ENC_LOCK_BITSTREAM lockBitstream = { NV_ENC_LOCK_BITSTREAM_VER };
        lockBitstream.outputBitstream = resource.bitstreamBuffer_;
        lockBitstream.doNotWait = false;
        CALL_NVENC_API(s_nvenc.nvEncLockBitstream, encoder_, &lockBitstream);

        NvencEncodedData ed;
        ed.index = outputIndex_;
        ed.size = lockBitstream.bitstreamSizeInBytes;
        ed.buffer = std::make_unique<uint8_t[]>(ed.size);
        ::memcpy(ed.buffer.get(), lockBitstream.bitstreamBufferPtr, ed.size);
        data.push_back(std::move(ed));

        CALL_NVENC_API(s_nvenc.nvEncUnlockBitstream, encoder_, resource.bitstreamBuffer_);

        UnmapInputResource(index);

        resource.isEncoding_ = false;
    }
}


bool Nvenc::WaitForCompletion(int index, DWORD duration)
{
    ThrowErrorIfNotInitialized();

    auto &resource = resources_[index];

    if (::WaitForSingleObject(resource.completionEvent_, duration) == WAIT_FAILED)
    {
        ThrowError("Failed to wait for encode completion.");
        return false;
    }

    return true;
}


void Nvenc::EndEncode()
{
    ThrowErrorIfNotInitialized();

    if (inputIndex_ == 0U) return;

    SendEOS();

    std::vector<NvencEncodedData> data;
    GetEncodedData(data);
}


void Nvenc::SendEOS()
{
    ThrowErrorIfNotInitialized();

    auto &resource = resources_[GetInputIndex()];
    resource.isEncoding_ = true;

    NV_ENC_PIC_PARAMS picParams = { NV_ENC_PIC_PARAMS_VER };
    picParams.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    picParams.completionEvent = resource.completionEvent_;
    CALL_NVENC_API(s_nvenc.nvEncEncodePicture, encoder_, &picParams);

    ++inputIndex_;
}


}