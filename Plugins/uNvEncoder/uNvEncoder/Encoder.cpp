#include "Encoder.h"
#include "Nvenc.h"


namespace uNvEncoder
{


Encoder::Encoder(const EncoderDesc &desc)
    : desc_(desc)
{
    try
    {
        CreateDevice();
        CreateNvenc();
        StartThread();
    }
    catch (const std::exception& e)
    {
        
        error_ = e.what();
        ::fprintf(stdout, "Encoder %s", error_.c_str());
    }
}


Encoder::~Encoder()
{
    try
    {
        StopThread();
        DestroyNvenc();
        DestroyDevice();
    }
    catch (const std::exception& e)
    {        
        error_ = e.what();
        ::fprintf(stdout, "~Encoder %s", error_.c_str());
    }
}


bool Encoder::IsValid() const
{
    return device_ && nvenc_ && nvenc_->IsValid();
}


void Encoder::CreateDevice()
{
    ComPtr<IDXGIDevice1> dxgiDevice;
    if (FAILED(GetUnityDevice()->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) 
    {
        ThrowError("Failed to get IDXGIDevice1.");
        return;
    }

    ComPtr<IDXGIAdapter> dxgiAdapter;
    if (FAILED(dxgiDevice->GetAdapter(&dxgiAdapter))) 
    {
        ThrowError("Failed to get IDXGIAdapter.");
        return;
    }

    constexpr auto driverType = D3D_DRIVER_TYPE_UNKNOWN;
    constexpr auto flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    constexpr D3D_FEATURE_LEVEL featureLevelsRequested[] =
    {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
        D3D_FEATURE_LEVEL_9_2,
        D3D_FEATURE_LEVEL_9_1
    };
    constexpr UINT numLevelsRequested = sizeof(featureLevelsRequested) / sizeof(D3D_FEATURE_LEVEL);
    D3D_FEATURE_LEVEL featureLevelsSupported;

    D3D11CreateDevice(
        dxgiAdapter.Get(),
        driverType,
        nullptr,
        flags,
        featureLevelsRequested,
        numLevelsRequested,
        D3D11_SDK_VERSION,
        &device_,
        &featureLevelsSupported,
        nullptr);
}


void Encoder::DestroyDevice()
{
    if(device_) device_->Release();
    device_ = nullptr;
}


void Encoder::CreateNvenc()
{
    NvencDesc desc = { 0 };
    desc.d3d11Device = device_;
    desc.width = desc_.width;
    desc.height = desc_.height;
    desc.format = desc_.format;
    desc.frameRate = desc_.frameRate;

    nvenc_ = std::make_unique<Nvenc>(desc);
    nvenc_->Initialize();
}


void Encoder::DestroyNvenc()
{
    nvenc_->Finalize();
    nvenc_.reset();
}

void Encoder::Resize(uint32_t width, uint32_t height)
{
    desc_.width = width;
    desc_.height = height;

    try
    {
        nvenc_->Resize(width, height);
    }
    catch (const std::exception & e)
    {        
        error_ = e.what();
        ::fprintf(stdout, "Resize %s", error_.c_str());
    }
}


void Encoder::StartThread()
{
    encodeThread_ = std::thread([&]
    {
        while (!shouldStopEncodeThread_)
        {
            WaitForEncodeRequest();
            UpdateGetEncodedData();
        }
    });
}


void Encoder::StopThread()
{
    shouldStopEncodeThread_ = true;
    encodeCond_.notify_one();

    if (encodeThread_.joinable())
    {
        encodeThread_.join();
    }
}


bool Encoder::Encode(const ComPtr<ID3D11Texture2D> &source, bool forceIdrFrame)
{
    try
    {
        nvenc_->Encode(source, forceIdrFrame);
    }
    catch (const std::exception& e)
    {        
        error_ = e.what();
        ::fprintf(stdout, "Encoder::Encode %s", error_.c_str());
        return false;
    }

    RequestGetEncodedData();
    return true;
}


bool Encoder::Encode(HANDLE sharedHandle, bool forceIdrFrame)
{
    ComPtr<ID3D11Texture2D> source;
    if (FAILED(GetUnityDevice()->OpenSharedResource(
        sharedHandle,
        __uuidof(ID3D11Texture2D),
        &source)))
    {
        return false;
    }

    return Encode(source, forceIdrFrame);
}


void Encoder::WaitForEncodeRequest()
{
    std::unique_lock<std::mutex> encodeLock(encodeMutex_);
    encodeCond_.wait(encodeLock, [&] 
    { 
        return isEncodeRequested || shouldStopEncodeThread_; 
    });
    isEncodeRequested = false;
}


void Encoder::RequestGetEncodedData()
{
    std::lock_guard<std::mutex> lock(encodeMutex_);
    isEncodeRequested = true;
    encodeCond_.notify_one();
}


void Encoder::UpdateGetEncodedData()
{
    std::vector<NvencEncodedData> data;

    try
    {
        nvenc_->GetEncodedData(data);
    }
    catch (const std::exception& e)
    {
        ::fprintf(stdout, "GetEncodedData %s", error_.c_str());
        error_ = e.what();
        return;
    }

    std::lock_guard<std::mutex> dataLock(encodeDataListMutex_);
    for (auto &ed : data)
    {
        encodedDataList_.push_back(std::move(ed));
    }
}


void Encoder::CopyEncodedDataList()
{
    std::lock_guard<std::mutex> lock(encodeDataListMutex_);

    encodedDataListCopied_.clear();
    std::swap(encodedDataListCopied_, encodedDataList_);
}


const std::vector<NvencEncodedData> & Encoder::GetEncodedDataList() const
{
    return encodedDataListCopied_;
}


}
