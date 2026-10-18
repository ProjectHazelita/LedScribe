#include "pixel_preview.hpp"

#include <QEvent>
#include <QPainter>
#include <QPalette>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace ledscribe::gui
{
PixelPreview::PixelPreview(QWidget *parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    updateGeometrySize();
}

auto PixelPreview::setBitmap(std::shared_ptr<const GlyphBitmap> bitmap, DisplayMode mode) -> void
{
    if (!bitmap)
    {
        clearBitmap();
        return;
    }
    bitmap->validate();
    bitmap_ = std::move(bitmap);
    mode_ = mode;
    frame_ = 0;
    rebuildImage();
}

auto PixelPreview::clearBitmap() -> void
{
    bitmap_.reset();
    frame_ = 0;
    image_ = QImage{};
    updateGeometrySize();
    update();
}

auto PixelPreview::setFrame(int frame) -> void
{
    const int next = std::clamp(frame, 0, std::max(0, frameCount() - 1));
    if (next != frame_)
    {
        frame_ = next;
        rebuildImage();
    }
}

auto PixelPreview::setScale(int scale) -> void
{
    const int next = std::clamp(scale, 4, 16);
    if (next != scale_)
    {
        scale_ = next;
        updateGeometrySize();
        update();
    }
}

auto PixelPreview::frameCount() const -> int
{
    if (!bitmap_)
    {
        return 0;
    }
    return mode_ == DisplayMode::animation ? (bitmap_->width - 1) / DISPLAY_WIDTH + 1 : 1;
}

auto PixelPreview::frameIndex() const -> int
{
    return frame_;
}

auto PixelPreview::frameImage() const -> QImage
{
    return image_;
}

auto PixelPreview::sizeHint() const -> QSize
{
    const QSize pixels = image_.isNull() ? QSize(DISPLAY_WIDTH, ROWS) : image_.size();
    return pixels * scale_;
}

auto PixelPreview::updateGeometrySize() -> void
{
    setFixedSize(sizeHint());
    updateGeometry();
}

auto PixelPreview::rebuildImage() -> void
{
    if (!bitmap_)
    {
        return;
    }
    const bool animation = mode_ == DisplayMode::animation;
    const int width = animation ? DISPLAY_WIDTH : bitmap_->width;
    const int source_start = animation ? frame_ * DISPLAY_WIDTH : 0;
    image_ = QImage(width, ROWS, QImage::Format_RGB32);
    const QRgb dark = palette().color(QPalette::Base).rgb();
    const QRgb light = palette().color(QPalette::Text).rgb();
    image_.fill(dark);
    for (int y = 0; y < ROWS; ++y)
    {
        auto *line = reinterpret_cast<QRgb *>(image_.scanLine(y));
        for (int x = 0; x < width && x < bitmap_->width - source_start; ++x)
        {
            const auto offset = static_cast<std::size_t>(y) *
                                    static_cast<std::size_t>(bitmap_->width) +
                                static_cast<std::size_t>(source_start + x);
            if (bitmap_->pixels[offset] > 127)
            {
                line[x] = light;
            }
        }
    }
    updateGeometrySize();
    update();
}

auto PixelPreview::paintEvent(QPaintEvent *event) -> void
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.fillRect(rect(), palette().brush(QPalette::Base));
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    if (!image_.isNull())
    {
        painter.drawImage(QRect(QPoint(0, 0), image_.size() * scale_), image_);
    }
}

auto PixelPreview::changeEvent(QEvent *event) -> void
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange)
    {
        rebuildImage();
    }
}
} // namespace ledscribe::gui
