#include "worker.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace ledscribe::gui
{
auto registerEditorMetaTypes() -> void
{
    qRegisterMetaType<RenderRequestPtr>();
    qRegisterMetaType<RenderResultPtr>();
    qRegisterMetaType<DeviceListPtr>();
    qRegisterMetaType<DeviceInfo>();
}

EditorWorker::EditorWorker(QObject *parent) : QObject(parent)
{
}

void EditorWorker::render(RenderRequestPtr request)
{
    auto result = std::make_shared<RenderResult>();
    try
    {
        if (!request)
        {
            throw std::invalid_argument("render request is empty");
        }
        result->revision = request->revision;
        const auto &document = request->document;
        const auto count = static_cast<std::size_t>(document.messages.size());
        result->images.resize(count, GlyphBitmap{.pixels = {}, .width = 0});
        result->fonts.resize(count);
        result->errors.reserve(document.messages.size());
        for (const auto &message : document.messages)
        {
            result->errors.append(message.text.isEmpty() ? QStringLiteral("消息文字为空")
                                                         : QString{});
        }
        validate_project(document);
        result->settings.brightness = document.brightness;
        for (std::size_t index = 0; index < count; ++index)
        {
            result->settings.slots[index] =
                document.messages[static_cast<qsizetype>(index)].settings;
        }

        const bool has_text = std::any_of(document.messages.cbegin(), document.messages.cend(),
                                          [](const MessageEntry &message)
                                          { return !message.text.isEmpty(); });
        // Empty drafts must not discover or load a font just to report their empty text.
        if (has_text)
        {
            RenderOptions options;
            options.font_size = document.font_size;
            options.threshold = document.threshold;
            options.vendor_font_dir =
                std::filesystem::path(document.vendor_font_directory.toStdU16String());
            options.no_vendor_font = document.font_choice != FontChoice::Auto;
            if (document.font_choice == FontChoice::OutlineFile)
            {
                if (document.font_file.isEmpty())
                {
                    throw std::invalid_argument("请选择轮廓字体文件");
                }
                options.font = std::filesystem::path(document.font_file.toStdU16String());
            }

            TextRenderer renderer(std::move(options));
            for (std::size_t index = 0; index < count; ++index)
            {
                const auto row = static_cast<qsizetype>(index);
                const auto &message = document.messages[row];
                if (message.text.isEmpty())
                {
                    continue;
                }
                try
                {
                    auto rendered_message = renderer.render(message.text.toStdString());
                    result->images[index] = std::move(rendered_message.image);
                    result->fonts[index] = std::move(rendered_message.font);
                }
                catch (const std::exception &error)
                {
                    result->errors[row] = QString::fromUtf8(error.what());
                }
            }
        }
        const bool all_valid = std::all_of(result->errors.cbegin(), result->errors.cend(),
                                           [](const QString &error) { return error.isEmpty(); });
        if (all_valid)
        {
            result->layout = measure_payload(result->images, result->settings);
        }
    }
    catch (const std::exception &error)
    {
        result->global_error = QString::fromUtf8(error.what());
        result->layout.reset();
    }
    Q_EMIT rendered(RenderResultPtr(std::move(result)));
}

void EditorWorker::discover()
{
    try
    {
        auto devices = std::make_shared<const std::vector<DeviceInfo>>(discover_devices(nullptr));
        Q_EMIT devicesDiscovered(std::move(devices));
    }
    catch (const std::exception &error)
    {
        Q_EMIT discoveryFailed(QString::fromUtf8(error.what()));
    }
}

void EditorWorker::write(DeviceInfo device, RenderResultPtr result)
{
    try
    {
        if (!result || !result->global_error.isEmpty() || result->images.empty() ||
            result->fonts.size() != result->images.size() ||
            static_cast<std::size_t>(result->errors.size()) != result->images.size() ||
            !result->layout || !result->layout->fits ||
            std::any_of(result->fonts.cbegin(), result->fonts.cend(),
                        [](const std::optional<RenderInfo> &font) { return !font; }) ||
            std::any_of(result->errors.cbegin(), result->errors.cend(),
                        [](const QString &error) { return !error.isEmpty(); }))
        {
            throw std::invalid_argument("当前预览未完成或无效，不能写入设备");
        }
        const auto layout = measure_payload(result->images, result->settings);
        if (!layout.fits)
        {
            throw std::invalid_argument("消息超出设备容量，不能写入设备");
        }
        const auto payload = build_payload(result->images, result->settings);
        const WriteProgressSink progress{
            .context = this,
            .notify = [](void *context, WriteProgress value) noexcept
            {
                auto *worker = static_cast<EditorWorker *>(context);
                Q_EMIT worker->writeProgress(static_cast<quint64>(value.reports_completed),
                                             static_cast<quint64>(value.reports_total));
            }};
        {
            auto badge = BadgeDevice::open(device, nullptr);
            badge.write(payload, WRITE_DELAY, TRANSFER_TIMEOUT, nullptr, progress);
        }
        Q_EMIT writeFinished(
            QStringLiteral("传输完成。可在胸牌上选择轮换或单槽位查看。"));
    }
    catch (const std::exception &error)
    {
        Q_EMIT writeFailed(QString::fromUtf8(error.what()));
    }
}
} // namespace ledscribe::gui
