#pragma once

#include <QImage>
#include <QWidget>

#include <memory>

#include "protocol.hpp"

namespace ledscribe::gui
{
class PixelPreview : public QWidget
{
    Q_OBJECT

  public:
    explicit PixelPreview(QWidget *parent = nullptr);

    auto setBitmap(std::shared_ptr<const GlyphBitmap> bitmap, DisplayMode mode) -> void;
    auto clearBitmap() -> void;
    auto setFrame(int frame) -> void;
    auto setScale(int scale) -> void;
    [[nodiscard]] auto frameCount() const -> int;
    [[nodiscard]] auto frameIndex() const -> int;
    [[nodiscard]] auto frameImage() const -> QImage;
    [[nodiscard]] auto sizeHint() const -> QSize override;

  protected:
    auto paintEvent(QPaintEvent *event) -> void override;
    auto changeEvent(QEvent *event) -> void override;

  private:
    auto rebuildImage() -> void;
    auto updateGeometrySize() -> void;

    std::shared_ptr<const GlyphBitmap> bitmap_;
    DisplayMode mode_{DisplayMode::left};
    QImage image_;
    int frame_{};
    int scale_{10};
};
} // namespace ledscribe::gui
