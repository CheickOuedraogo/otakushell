#include "otaku/frame.hpp"

#include "otaku/module.hpp"
#include "otaku/render.hpp"
#include "otaku/wayland.hpp"

namespace otaku {

Frame::Frame(const FrameSpec& spec) : spec_(spec) {
    // Instantiate the modules declared in spec.order.
    for (const auto& mod : spec_.order) {
        if (auto m = create_module(mod, false))
            modules_.push_back(std::move(m));
    }
}

Frame::~Frame() = default;

void Frame::set_surface(std::unique_ptr<ISurface> surface) {
    surface_ = std::move(surface);
}

void Frame::configure(int32_t width, int32_t height) {
    if (!surface_) return;
    surface_->set_size(width, height);
}

void Frame::render() {
    if (!surface_) return;
    // A flat opaque background; module drawing is layered in later steps.
    render_background(*surface_, 0xEE1E1E2Eu, 10);
    surface_->present();
}

}  // namespace otaku
