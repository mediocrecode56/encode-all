#include "thumbs.h"

#include <shlwapi.h>
#include <wincodec.h>

#include "../core/net.h"
#include "../core/util.h"

namespace ea::ui {

static HBITMAP decode(const std::string& data, int w, int h) {
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return nullptr;
    HBITMAP out = nullptr;
    IStream* s = SHCreateMemStream((const BYTE*)data.data(), (UINT)data.size());
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapClipper* clip = nullptr;
    IWICBitmapScaler* sc = nullptr;
    IWICFormatConverter* cv = nullptr;
    do {
        if (!s || FAILED(f->CreateDecoderFromStream(s, nullptr, WICDecodeMetadataCacheOnDemand, &dec))) break;
        if (FAILED(dec->GetFrame(0, &frame))) break;
        UINT sw, sh;
        frame->GetSize(&sw, &sh);
        if (!sw || !sh) break;
        // Crop to 16:9 (YouTube's 4:3 thumbnails carry black bars).
        WICRect rc{0, 0, (INT)sw, (INT)sh};
        double target = (double)w / h;
        if ((double)sw / sh < target) {
            rc.Height = (INT)(sw / target);
            rc.Y = (INT)(sh - rc.Height) / 2;
        } else {
            rc.Width = (INT)(sh * target);
            rc.X = (INT)(sw - rc.Width) / 2;
        }
        if (FAILED(f->CreateBitmapClipper(&clip)) || FAILED(clip->Initialize(frame, &rc))) break;
        if (FAILED(f->CreateBitmapScaler(&sc)) || FAILED(sc->Initialize(clip, w, h, WICBitmapInterpolationModeFant))) break;
        if (FAILED(f->CreateFormatConverter(&cv)) ||
            FAILED(cv->Initialize(sc, GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
            break;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HBITMAP b = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!b) break;
        if (FAILED(cv->CopyPixels(nullptr, w * 4, w * h * 4, (BYTE*)bits))) {
            DeleteObject(b);
            break;
        }
        out = b;
    } while (false);
    if (cv) cv->Release();
    if (sc) sc->Release();
    if (clip) clip->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (s) s->Release();
    f->Release();
    return out;
}

void Thumbs::start(std::function<void()> onReady) {
    onReady_ = std::move(onReady);
    for (int i = 0; i < 2; i++) threads_.emplace_back(&Thumbs::worker, this);
}

void Thumbs::stop() {
    stop_ = true;
    cv_.notify_all();
    for (auto& t : threads_) t.join();
    threads_.clear();
    for (auto& kv : cache_)
        if (kv.second) DeleteObject(kv.second);
    cache_.clear();
}

void Thumbs::setSize(int w, int h) {
    std::lock_guard<std::mutex> lk(mu_);
    if (w == w_ && h == h_) return;
    w_ = w;
    h_ = h;
    for (auto& kv : cache_)
        if (kv.second) DeleteObject(kv.second);
    cache_.clear();
    order_.clear();
}

HBITMAP Thumbs::get(const std::string& url) {
    if (url.empty()) return nullptr;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = cache_.find(url);
    if (it != cache_.end()) return it->second;
    if (!pending_[url]) {
        pending_[url] = true;
        todo_.push_front(url);  // newest first: the rows on screen now
        if (todo_.size() > 200) {
            pending_.erase(todo_.back());
            todo_.pop_back();
        }
        cv_.notify_one();
    }
    return nullptr;
}

void Thumbs::worker() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (!stop_) {
        std::string url;
        int w, h;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return stop_ || !todo_.empty(); });
            if (stop_) break;
            url = todo_.front();
            todo_.pop_front();
            w = w_;
            h = h_;
        }
        std::string body;
        HBITMAP b = nullptr;
        auto r = httpGet(widen(url), &body, L"", nullptr, &stop_, 4u << 20);
        if (r.ok) b = decode(body, w, h);
        {
            std::lock_guard<std::mutex> lk(mu_);
            pending_.erase(url);
            if (w != w_ || h != h_) {
                if (b) DeleteObject(b);
                continue;
            }
            cache_[url] = b;  // a failed load is cached as nullptr: no retry storm
            order_.push_back(url);
            while (order_.size() > 400) {
                auto old = cache_.find(order_.front());
                if (old != cache_.end()) {
                    if (old->second) DeleteObject(old->second);
                    cache_.erase(old);
                }
                order_.pop_front();
            }
        }
        if (b && onReady_) onReady_();
    }
    CoUninitialize();
}

}  // namespace ea::ui
