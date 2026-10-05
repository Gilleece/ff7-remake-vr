#include "capture.h"

#include <cstdio>
#include <filesystem>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#pragma warning(push)
#pragma warning(disable : 4996 4244 4456 4457 4701 4703)
#include "stb_image_write.h"
#pragma warning(pop)

namespace ff7vr::xr {

bool CaptureManager::Init(ID3D11Device* device, const Logger* log, Blitter* blitter) {
    device_ = device;
    log_ = log;
    blitter_ = blitter;
    stop_ = false;
    worker_ = std::thread([this] { WorkerMain(); });
    return true;
}

void CaptureManager::Shutdown() {
    {
        std::lock_guard lk(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    for (int i = 0; i < 2; ++i) {
        rt_[i].Reset();
        staging_[i].Reset();
        rtW_[i] = rtH_[i] = 0;
    }
    device_.Reset();
    requests_.clear();
    jobs_.clear();
    pendingCount_ = 0;
    active_ = false;
}

void CaptureManager::Request(const CaptureRequest& req) {
    std::lock_guard lk(mutex_);
    requests_.push_back(req);
    pendingCount_.fetch_add(1, std::memory_order_release);
}

std::vector<CaptureResult> CaptureManager::Wait(uint32_t timeoutMs) {
    std::unique_lock lk(mutex_);
    cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return pendingCount_.load() == 0; });
    std::vector<CaptureResult> r;
    r.swap(results_);
    return r;
}

bool CaptureManager::BeginFrame(uint64_t frameId) {
    active_ = false;
    if (!HasPending()) return false;
    std::lock_guard lk(mutex_);
    if (requests_.empty()) return false;
    current_ = Job{};
    current_.frameId = frameId;
    current_.prefix = requests_.front().pathPrefix;
    requests_.pop_front();
    current_.eyes[0].error = current_.eyes[1].error = "eye not submitted this frame";
    active_ = true;
    return true;
}

void CaptureManager::CaptureEye(ID3D11DeviceContext* ctx, Eye eye, ID3D11Texture2D* tex, DXGI_FORMAT viewFormat, uint32_t w,
                                uint32_t h) {
    if (!active_) return;
    const int e = static_cast<int>(eye);
    Image& img = current_.eyes[e];
    img = Image{};
    if (!tex || w == 0 || h == 0) {
        img.error = "no image";
        return;
    }
    if (rtW_[e] != w || rtH_[e] != h || !rt_[e]) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (rt_[e]) blitter_->Forget(rt_[e].Get());
        rt_[e].Reset();
        staging_[e].Reset();
        HRESULT hr = device_->CreateTexture2D(&d, nullptr, &rt_[e]);
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (SUCCEEDED(hr)) hr = device_->CreateTexture2D(&d, nullptr, &staging_[e]);
        if (FAILED(hr)) {
            img.error = "capture texture creation failed " + HResultString(hr);
            rt_[e].Reset();
            staging_[e].Reset();
            rtW_[e] = rtH_[e] = 0;
            return;
        }
        rtW_[e] = w;
        rtH_[e] = h;
    }

    // Convert whatever the eye image format is into 8-bit sRGB.
    BlitSource src;
    src.texture = tex;
    src.viewFormat = viewFormat;
    src.encoding = ColorEncoding::Linear;  // the eye image holds the runtime-interpreted (linear-light) values of viewFormat
    BlitDest dst;
    dst.texture = rt_[e].Get();
    dst.viewFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    dst.width = w;
    dst.height = h;
    Blitter::Path path{};
    uint32_t ow = 0, oh = 0;
    if (!blitter_->Transfer(ctx, src, Rect{0, 0, w, h}, dst, &path, &ow, &oh)) {
        img.error = "capture blit failed";
        return;
    }
    ctx->CopyResource(staging_[e].Get(), rt_[e].Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = ctx->Map(staging_[e].Get(), 0, D3D11_MAP_READ, 0, &m);  // stalls until the GPU is done
    if (FAILED(hr)) {
        img.error = "capture map failed " + HResultString(hr);
        return;
    }
    img.w = ow;
    img.h = oh;
    img.rgb.resize(size_t(ow) * oh * 3);
    for (uint32_t y = 0; y < oh; ++y) {
        const uint8_t* s = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
        uint8_t* d = img.rgb.data() + size_t(y) * ow * 3;
        for (uint32_t x = 0; x < ow; ++x) {
            d[x * 3 + 0] = s[x * 4 + 0];
            d[x * 3 + 1] = s[x * 4 + 1];
            d[x * 3 + 2] = s[x * 4 + 2];
        }
    }
    ctx->Unmap(staging_[e].Get(), 0);
}

void CaptureManager::EndFrame() {
    if (!active_) return;
    active_ = false;
    {
        std::lock_guard lk(mutex_);
        jobs_.push_back(std::move(current_));
    }
    current_ = Job{};
    cv_.notify_all();
}

void CaptureManager::WorkerMain() {
    for (;;) {
        Job job;
        {
            std::unique_lock lk(mutex_);
            cv_.wait(lk, [&] { return stop_ || !jobs_.empty(); });
            if (jobs_.empty()) return;  // stop requested and nothing left
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        CaptureResult r;
        r.frameId = job.frameId;
        r.ok = true;
        static const char* kSuffix[2] = {"_L.png", "_R.png"};
        for (int e = 0; e < 2; ++e) {
            if (job.eyes[e].rgb.empty()) {
                r.ok = false;
                r.error += std::format("eye {}: {}; ", e, job.eyes[e].error);
                continue;
            }
            const std::string path = job.prefix + kSuffix[e];
            std::string err;
            if (WritePng(path, job.eyes[e], &err)) {
                r.files[e] = path;
            } else {
                r.ok = false;
                r.error += std::format("eye {}: {}; ", e, err);
            }
        }
        if (r.ok)
            log_->Info("capture frame {} -> {} , {}", r.frameId, r.files[0], r.files[1]);
        else
            log_->Error("capture frame {} failed: {}", r.frameId, r.error);
        {
            std::lock_guard lk(mutex_);
            results_.push_back(std::move(r));
            pendingCount_.fetch_sub(1, std::memory_order_release);
        }
        cv_.notify_all();
    }
}

bool CaptureManager::WritePng(const std::string& pathUtf8, const Image& img, std::string* err) {
    try {
        const std::filesystem::path p(Utf8ToWide(pathUtf8));
        if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
        FILE* f = nullptr;
        if (_wfopen_s(&f, p.c_str(), L"wb") != 0 || !f) {
            *err = "cannot open " + pathUtf8;
            return false;
        }
        auto write = [](void* ctx, void* data, int size) { fwrite(data, 1, static_cast<size_t>(size), static_cast<FILE*>(ctx)); };
        const int ok = stbi_write_png_to_func(write, f, static_cast<int>(img.w), static_cast<int>(img.h), 3, img.rgb.data(),
                                              static_cast<int>(img.w * 3));
        const bool closed = fclose(f) == 0;
        if (!ok || !closed) {
            *err = "png encode/write failed for " + pathUtf8;
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        *err = ex.what();
        return false;
    }
}

}  // namespace ff7vr::xr
