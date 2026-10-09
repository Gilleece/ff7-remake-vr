#include "capture.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <string_view>

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
    if (blitter_) ReleaseTextures();
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
    constexpr std::string_view kRaw = "+raw";
    if (current_.prefix.size() > kRaw.size() && current_.prefix.ends_with(kRaw)) {
        current_.prefix.resize(current_.prefix.size() - kRaw.size());
        current_.raw = true;
    }
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

void CaptureManager::CaptureRaw(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, uint32_t arraySlice, const std::string& suffix) {
    if (!active_ || !current_.raw) return;
    RawImage& img = current_.raws.emplace_back();
    img.suffix = suffix;
    if (!ctx || !tex) {
        img.error = "no texture";
        return;
    }
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    const DXGI_FORMAT fam = TypelessFamily(d.Format);
    const bool bgra = fam == DXGI_FORMAT_B8G8R8A8_TYPELESS || fam == DXGI_FORMAT_B8G8R8X8_TYPELESS;
    const bool rgb10 = fam == DXGI_FORMAT_R10G10B10A2_TYPELESS;
    if (!bgra && !rgb10 && fam != DXGI_FORMAT_R8G8B8A8_TYPELESS) {
        img.error = std::string("unsupported format ") + DxgiFormatName(d.Format);
        return;
    }
    if (d.SampleDesc.Count != 1 || arraySlice >= d.ArraySize) {
        img.error = "multisampled texture or bad slice";
        return;
    }
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Format = fam;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device_->CreateTexture2D(&sd, nullptr, &staging);
    if (FAILED(hr)) {
        img.error = "staging texture: " + HResultString(hr);
        return;
    }
    ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex, D3D11CalcSubresource(0, arraySlice, d.MipLevels), nullptr);
    D3D11_MAPPED_SUBRESOURCE m{};
    hr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) {
        img.error = "map: " + HResultString(hr);
        return;
    }
    img.w = d.Width;
    img.h = d.Height;
    img.rgba.resize(size_t(d.Width) * d.Height * 4);
    for (uint32_t y = 0; y < d.Height; ++y) {
        const uint8_t* s = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
        uint8_t* o = img.rgba.data() + size_t(y) * d.Width * 4;
        for (uint32_t x = 0; x < d.Width; ++x, s += 4, o += 4) {
            if (rgb10) {
                uint32_t v = 0;
                std::memcpy(&v, s, 4);
                auto to8 = [](uint32_t c) { return static_cast<uint8_t>((c * 255u + 511u) / 1023u); };
                o[0] = to8(v & 1023u);
                o[1] = to8((v >> 10) & 1023u);
                o[2] = to8((v >> 20) & 1023u);
                o[3] = static_cast<uint8_t>((v >> 30) * 85u);
            } else {
                o[0] = bgra ? s[2] : s[0];
                o[1] = s[1];
                o[2] = bgra ? s[0] : s[2];
                o[3] = s[3];
            }
        }
    }
    ctx->Unmap(staging.Get(), 0);
}

void CaptureManager::CaptureDepth(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, uint32_t w, uint32_t h, float nearZ, float farZ,
                                  const std::string& suffix) {
    if (!active_) return;
    RawImage& img = current_.raws.emplace_back();
    img.suffix = suffix;
    if (!ctx || !tex) {
        img.error = "no depth texture";
        return;
    }
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    DXGI_FORMAT fam = DXGI_FORMAT_UNKNOWN;
    switch (d.Format) {
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R32_FLOAT: fam = DXGI_FORMAT_R32_TYPELESS; break;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24G8_TYPELESS: fam = DXGI_FORMAT_R24G8_TYPELESS; break;
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R16_UNORM: fam = DXGI_FORMAT_R16_TYPELESS; break;
        default: break;
    }
    if (fam == DXGI_FORMAT_UNKNOWN || d.SampleDesc.Count != 1) {
        img.error = std::string("unsupported depth format ") + DxgiFormatName(d.Format);
        return;
    }
    w = std::min(w, d.Width);
    h = std::min(h, d.Height);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Format = fam;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device_->CreateTexture2D(&sd, nullptr, &staging);
    if (FAILED(hr)) {
        img.error = "depth staging texture: " + HResultString(hr);
        return;
    }
    // Depth-stencil resources are copied whole (D3D11 allows no sub-box for them).
    ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex, 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE m{};
    hr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) {
        img.error = "depth map: " + HResultString(hr);
        return;
    }
    img.w = w;
    img.h = h;
    img.rgba.resize(size_t(w) * h * 4);
    double sum = 0;
    float lo = 1e30f, hi = -1e30f;
    uint64_t zero = 0, n = 0;
    float centre = 0;
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
        uint8_t* o = img.rgba.data() + size_t(y) * w * 4;
        for (uint32_t x = 0; x < w; ++x, o += 4) {
            float v = 0;
            if (fam == DXGI_FORMAT_R32_TYPELESS) {
                std::memcpy(&v, row + size_t(x) * 4, 4);
            } else if (fam == DXGI_FORMAT_R24G8_TYPELESS) {
                uint32_t u = 0;
                std::memcpy(&u, row + size_t(x) * 4, 4);
                v = float(u & 0xFFFFFFu) / 16777215.0f;
            } else {
                uint16_t u = 0;
                std::memcpy(&u, row + size_t(x) * 2, 2);
                v = float(u) / 65535.0f;
            }
            if (!(v == v)) v = 0;  // NaN
            if (x == w / 2 && y == h / 2) centre = v;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            if (v <= 0.0f) ++zero;
            sum += v;
            ++n;
            const float g = v > 0.0f ? std::pow(std::min(v, 1.0f), 0.25f) : 0.0f;
            const uint8_t b = static_cast<uint8_t>(std::lround(g * 255.0f));
            o[0] = o[1] = o[2] = b;
            o[3] = 255;
        }
    }
    ctx->Unmap(staging.Get(), 0);
    // OpenXR's mapping: 1/z is linear in the depth value between nearZ (0) and farZ (1).
    const double invNear = std::isinf(nearZ) ? 0.0 : 1.0 / nearZ, invFar = std::isinf(farZ) ? 0.0 : 1.0 / farZ;
    const double invZ = invNear + double(centre) * (invFar - invNear);
    const std::string dist = invZ > 1e-12 ? std::format("{:.3f} m", 1.0 / invZ) : std::string("infinite");
    if (log_)
        log_->Info("capture depth{}: {}x{} {}, values {:.6g}..{:.6g}, mean {:.6g}, at 0 (far) {:.1f} %, centre {:.6g} = {} (nearZ {} farZ {})", suffix, w, h,
                   DxgiFormatName(d.Format), n ? lo : 0.0f, n ? hi : 0.0f, n ? sum / double(n) : 0.0, n ? 100.0 * double(zero) / double(n) : 0.0,
                   centre, dist, nearZ, farZ);
}

void CaptureManager::ReleaseTextures() {
    for (int e = 0; e < 2; ++e) {
        if (rt_[e]) blitter_->Forget(rt_[e].Get());
        rt_[e].Reset();
        staging_[e].Reset();
        rtW_[e] = rtH_[e] = 0;
    }
}

void CaptureManager::EndFrame() {
    if (!active_) return;
    active_ = false;
    // The pixels are on the CPU now (CaptureEye maps and copies them). The eye-sized
    // conversion and staging textures are released rather than kept for the session
    // (at 4608x4224 they are 156 MB per eye); the next capture creates them again.
    ReleaseTextures();
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
        for (const RawImage& raw : job.raws) {
            const std::string path = job.prefix + raw.suffix;
            if (raw.rgba.empty()) {
                log_->Warn("capture raw {}: {}", path, raw.error);
                continue;
            }
            try {
                const std::filesystem::path p(Utf8ToWide(path));
                if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
                FILE* f = nullptr;
                if (_wfopen_s(&f, p.c_str(), L"wb") != 0 || !f) {
                    log_->Warn("capture raw: cannot open {}", path);
                    continue;
                }
                auto write = [](void* c, void* data, int size) { fwrite(data, 1, static_cast<size_t>(size), static_cast<FILE*>(c)); };
                const int ok = stbi_write_png_to_func(write, f, static_cast<int>(raw.w), static_cast<int>(raw.h), 4, raw.rgba.data(),
                                                      static_cast<int>(raw.w * 4));
                fclose(f);
                if (ok)
                    log_->Info("capture raw -> {} ({}x{})", path, raw.w, raw.h);
                else
                    log_->Warn("capture raw: png encode failed for {}", path);
            } catch (const std::exception& ex) {
                log_->Warn("capture raw {}: {}", path, ex.what());
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

bool WriteTexturePng(ID3D11DeviceContext* ctx, ID3D11Texture2D* texture, const std::string& pathUtf8, std::string* error) {
    std::string dummy;
    std::string& err = error ? *error : dummy;
    if (!ctx || !texture) {
        err = "no context or texture";
        return false;
    }
    D3D11_TEXTURE2D_DESC d{};
    texture->GetDesc(&d);
    const DXGI_FORMAT fam = TypelessFamily(d.Format);
    const bool bgra = fam == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    if (!bgra && fam != DXGI_FORMAT_R8G8B8A8_TYPELESS) {
        err = std::string("unsupported format ") + DxgiFormatName(d.Format);
        return false;
    }
    if (d.SampleDesc.Count != 1) {
        err = "multisampled texture";
        return false;
    }
    ComPtr<ID3D11Device> dev;
    texture->GetDevice(&dev);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = dev->CreateTexture2D(&sd, nullptr, &staging);
    if (FAILED(hr)) {
        err = "staging texture: " + HResultString(hr);
        return false;
    }
    ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture, D3D11CalcSubresource(0, 0, d.MipLevels), nullptr);
    D3D11_MAPPED_SUBRESOURCE m{};
    hr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) {
        err = "map: " + HResultString(hr);
        return false;
    }
    std::vector<uint8_t> rgba(size_t(d.Width) * d.Height * 4);
    for (uint32_t y = 0; y < d.Height; ++y) {
        const uint8_t* s = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
        uint8_t* o = rgba.data() + size_t(y) * d.Width * 4;
        for (uint32_t x = 0; x < d.Width; ++x, s += 4, o += 4) {
            o[0] = bgra ? s[2] : s[0];
            o[1] = s[1];
            o[2] = bgra ? s[0] : s[2];
            o[3] = s[3];
        }
    }
    ctx->Unmap(staging.Get(), 0);
    try {
        const std::filesystem::path p(Utf8ToWide(pathUtf8));
        if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
        FILE* f = nullptr;
        if (_wfopen_s(&f, p.c_str(), L"wb") != 0 || !f) {
            err = "cannot open " + pathUtf8;
            return false;
        }
        auto write = [](void* c, void* data, int size) { fwrite(data, 1, static_cast<size_t>(size), static_cast<FILE*>(c)); };
        const int ok = stbi_write_png_to_func(write, f, static_cast<int>(d.Width), static_cast<int>(d.Height), 4, rgba.data(),
                                              static_cast<int>(d.Width * 4));
        const bool closed = fclose(f) == 0;
        if (!ok || !closed) {
            err = "png encode/write failed for " + pathUtf8;
            return false;
        }
    } catch (const std::exception& ex) {
        err = ex.what();
        return false;
    }
    return true;
}

}  // namespace ff7vr::xr
