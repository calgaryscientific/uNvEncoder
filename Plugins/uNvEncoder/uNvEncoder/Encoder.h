#pragma once

#include <cstdio>
#include <vector>
#include <memory>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <d3d11.h>
#include "Common.h"


namespace uNvEncoder
{


struct NvencEncodedData;


struct EncoderDesc
{
    int width; 
    int height;
    int frameRate;
    DXGI_FORMAT format;
};


class Encoder final
{
public:
    explicit Encoder(const EncoderDesc &desc);
    ~Encoder();
    bool IsValid() const;
    bool Encode(const ComPtr<ID3D11Texture2D> &source, bool forceIdrFrame);
    bool Encode(HANDLE sharedHandle, bool forceIdrFrame);
    void CopyEncodedDataList();
    const std::vector<NvencEncodedData> & GetEncodedDataList() const;
    const uint32_t GetWidth() { return desc_.width; }
    const uint32_t GetHeight() { return desc_.height; }
    const uint32_t GetFrameRate() const { return desc_.frameRate; }
    const DXGI_FORMAT GetFormat() const { return desc_.format; }
    bool HasError() const { return !error_.empty(); }
    const std::string & GetError() const { return error_; }
    void ClearError() { error_.clear(); }
	void Resize(uint32_t width, uint32_t height);

private:
    void CreateDevice();
    void DestroyDevice();
    void CreateNvenc();
    void DestroyNvenc();
    void StartThread();
    void StopThread();
    void WaitForEncodeRequest();
    void RequestGetEncodedData();
    void UpdateGetEncodedData();

    EncoderDesc desc_;
    ComPtr<ID3D11Device> device_;
    std::unique_ptr<class Nvenc> nvenc_;
    std::vector<NvencEncodedData> encodedDataList_;
    std::vector<NvencEncodedData> encodedDataListCopied_;
    std::thread encodeThread_;
    std::condition_variable encodeCond_;
    std::mutex encodeMutex_;
    std::mutex encodeDataListMutex_;
    bool shouldStopEncodeThread_ = false;
    bool isEncodeRequested = false;
    std::string error_;
};


}
