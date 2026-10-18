#pragma once

#include "device.hpp"
#include "project.hpp"
#include "render.hpp"

#include <QMetaType>
#include <QObject>
#include <QStringList>

#include <memory>
#include <optional>
#include <vector>

namespace ledscribe::gui
{
struct RenderRequest
{
    quint64 revision{};
    ProjectDocument document;
};

struct RenderResult
{
    quint64 revision{};
    std::vector<GlyphBitmap> images;
    std::vector<std::optional<RenderInfo>> fonts;
    QStringList errors;
    QString global_error;
    DisplaySettings settings;
    std::optional<PayloadLayout> layout;
};

using RenderRequestPtr = std::shared_ptr<const RenderRequest>;
using RenderResultPtr = std::shared_ptr<const RenderResult>;
using DeviceListPtr = std::shared_ptr<const std::vector<DeviceInfo>>;

auto registerEditorMetaTypes() -> void;

class EditorWorker : public QObject
{
    Q_OBJECT

  public:
    explicit EditorWorker(QObject *parent = nullptr);

  public Q_SLOTS:
    void render(RenderRequestPtr request);
    void discover();
    void write(DeviceInfo device, RenderResultPtr result);

  Q_SIGNALS:
    void rendered(RenderResultPtr result);
    void devicesDiscovered(DeviceListPtr devices);
    void discoveryFailed(QString error);
    void writeProgress(quint64 completed, quint64 total);
    void writeFinished(QString message);
    void writeFailed(QString error);
};
} // namespace ledscribe::gui

Q_DECLARE_METATYPE(ledscribe::gui::RenderRequestPtr)
Q_DECLARE_METATYPE(ledscribe::gui::RenderResultPtr)
Q_DECLARE_METATYPE(ledscribe::gui::DeviceListPtr)
Q_DECLARE_METATYPE(DeviceInfo)
