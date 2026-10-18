#include "project.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMimeData>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QSaveFile>

#include <cmath>
#include <stdexcept>
#include <utility>

namespace ledscribe::gui
{
namespace
{
constexpr auto move_mime_type = "application/x-ledscribe-message-move";

class MessageMimeData final : public QMimeData
{
public:
    MessageMimeData(const ProjectModel *model, const QModelIndex &index)
        : source(model), source_index(index)
    {
        setData(QString::fromLatin1(move_mime_type), QByteArrayLiteral("move"));
    }

    QPointer<const ProjectModel> source;
    QPersistentModelIndex source_index;
};

auto valid_message(const MessageEntry &message) -> bool
{
    return message.text.size() <= 32767 && !message.text.contains(QLatin1Char('\r')) &&
           !message.text.contains(QLatin1Char('\n')) && message.settings.speed >= 1 &&
           message.settings.speed <= 8 && static_cast<unsigned int>(message.settings.mode) <= 8;
}

auto same_message(const MessageEntry &a, const MessageEntry &b) -> bool
{
    return a.text == b.text && a.settings.speed == b.settings.speed &&
           a.settings.mode == b.settings.mode && a.settings.blink == b.settings.blink &&
           a.settings.border == b.settings.border;
}

auto validate_brightness(int brightness) -> void
{
    if (brightness != 25 && brightness != 50 && brightness != 75 && brightness != 100)
        throw std::invalid_argument("brightness must be 25, 50, 75 or 100");
}

auto validate_font(FontChoice choice, int size, int threshold) -> void
{
    if (choice != FontChoice::Auto && choice != FontChoice::OutlineAuto &&
        choice != FontChoice::OutlineFile)
        throw std::invalid_argument("invalid font source");
    if (size < 6 || size > 256)
        throw std::invalid_argument("font size must be between 6 and 256");
    if (threshold < 0 || threshold > 255)
        throw std::invalid_argument("threshold must be between 0 and 255");
}

auto same_document(const ProjectDocument &a, const ProjectDocument &b) -> bool
{
    if (a.brightness != b.brightness || a.font_choice != b.font_choice ||
        a.font_file != b.font_file || a.vendor_font_directory != b.vendor_font_directory ||
        a.font_size != b.font_size || a.threshold != b.threshold ||
        a.messages.size() != b.messages.size())
        return false;
    for (qsizetype i = 0; i < a.messages.size(); ++i)
        if (!same_message(a.messages[i], b.messages[i]))
            return false;
    return true;
}

auto require_string(const QJsonObject &object, const QString &key) -> QString
{
    const auto value = object.value(key);
    if (!value.isString())
        throw std::invalid_argument((key + QStringLiteral(" must be a string")).toStdString());
    return value.toString();
}

auto require_integer(const QJsonObject &object, const QString &key, int minimum, int maximum)
    -> int
{
    const auto value = object.value(key);
    if (!value.isDouble())
        throw std::invalid_argument((key + QStringLiteral(" must be an integer")).toStdString());
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < minimum || number > maximum ||
        std::trunc(number) != number)
        throw std::invalid_argument((key + QStringLiteral(" is outside its integer range")).toStdString());
    return static_cast<int>(number);
}

auto require_bool(const QJsonObject &object, const QString &key) -> bool
{
    const auto value = object.value(key);
    if (!value.isBool())
        throw std::invalid_argument((key + QStringLiteral(" must be a boolean")).toStdString());
    return value.toBool();
}

auto mode_string(DisplayMode mode) -> QString
{
    const auto name = display_mode_name(mode);
    return QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size()));
}

auto canonical_mode(const QString &name) -> DisplayMode
{
    for (unsigned int value = 0; value <= 8; ++value)
    {
        const auto mode = static_cast<DisplayMode>(value);
        if (name == mode_string(mode))
            return mode;
    }
    throw std::invalid_argument("mode must be a canonical display mode name");
}

auto font_source(FontChoice choice) -> QString
{
    switch (choice)
    {
    case FontChoice::Auto: return QStringLiteral("auto");
    case FontChoice::OutlineAuto: return QStringLiteral("outline-auto");
    case FontChoice::OutlineFile: return QStringLiteral("outline-file");
    }
    throw std::invalid_argument("invalid font source");
}

auto absolute_resource(const QString &resource, const QDir &directory) -> QString
{
    return resource.isEmpty() ? QString{} : directory.absoluteFilePath(resource);
}

auto file_error(const QString &path, const std::exception &error) -> std::runtime_error
{
    return std::runtime_error((path + QStringLiteral(": ") + QString::fromUtf8(error.what())).toStdString());
}
} // namespace

auto validate_project(const ProjectDocument &document) -> void
{
    if (document.messages.isEmpty() || document.messages.size() > 8)
        throw std::invalid_argument("project must contain between 1 and 8 messages");
    for (const auto &message : document.messages)
        if (!valid_message(message))
            throw std::invalid_argument("invalid message text or slot settings");
    validate_brightness(document.brightness);
    validate_font(document.font_choice, document.font_size, document.threshold);
}

auto load_project(const QString &path) -> ProjectDocument
{
    try
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            throw std::runtime_error(file.errorString().toStdString());
        const auto bytes = file.readAll();
        if (file.error() != QFileDevice::NoError)
            throw std::runtime_error(file.errorString().toStdString());
        QJsonParseError error;
        const auto json = QJsonDocument::fromJson(bytes, &error);
        if (error.error != QJsonParseError::NoError)
            throw std::runtime_error(error.errorString().toStdString());
        if (!json.isObject())
            throw std::invalid_argument("project root must be an object");
        const auto root = json.object();
        if (require_string(root, QStringLiteral("format")) != QStringLiteral("LedScribe"))
            throw std::invalid_argument("unrecognized project format");
        (void)require_integer(root, QStringLiteral("version"), 1, 1);
        ProjectDocument document;
        document.brightness = require_integer(root, QStringLiteral("brightness"), 25, 100);
        const auto font_value = root.value(QStringLiteral("font"));
        if (!font_value.isObject())
            throw std::invalid_argument("font must be an object");
        const auto font = font_value.toObject();
        const auto source = require_string(font, QStringLiteral("source"));
        if (source == QStringLiteral("auto"))
            document.font_choice = FontChoice::Auto;
        else if (source == QStringLiteral("outline-auto"))
            document.font_choice = FontChoice::OutlineAuto;
        else if (source == QStringLiteral("outline-file"))
            document.font_choice = FontChoice::OutlineFile;
        else
            throw std::invalid_argument("unrecognized font source");
        const auto directory = QFileInfo(path).absoluteDir();
        document.font_file = absolute_resource(require_string(font, QStringLiteral("file")), directory);
        document.vendor_font_directory = absolute_resource(
            require_string(font, QStringLiteral("vendorDirectory")), directory);
        document.font_size = require_integer(font, QStringLiteral("size"), 6, 256);
        document.threshold = require_integer(font, QStringLiteral("threshold"), 0, 255);
        const auto messages_value = root.value(QStringLiteral("messages"));
        if (!messages_value.isArray())
            throw std::invalid_argument("messages must be an array");
        const auto messages = messages_value.toArray();
        if (messages.isEmpty() || messages.size() > 8)
            throw std::invalid_argument("project must contain between 1 and 8 messages");
        document.messages.clear();
        document.messages.reserve(messages.size());
        for (const auto &value : messages)
        {
            if (!value.isObject())
                throw std::invalid_argument("each message must be an object");
            const auto object = value.toObject();
            MessageEntry message;
            message.text = require_string(object, QStringLiteral("text"));
            message.settings.mode = canonical_mode(require_string(object, QStringLiteral("mode")));
            message.settings.speed = require_integer(object, QStringLiteral("speed"), 1, 8);
            message.settings.blink = require_bool(object, QStringLiteral("blink"));
            message.settings.border = require_bool(object, QStringLiteral("border"));
            document.messages.append(std::move(message));
        }
        validate_project(document);
        return document;
    }
    catch (const std::exception &error)
    {
        throw file_error(path, error);
    }
}

auto save_project(const QString &path, const ProjectDocument &document) -> void
{
    try
    {
        validate_project(document);
        const auto directory = QDir::current();
        const QJsonObject font{
            {QStringLiteral("source"), font_source(document.font_choice)},
            {QStringLiteral("file"), absolute_resource(document.font_file, directory)},
            {QStringLiteral("vendorDirectory"), absolute_resource(document.vendor_font_directory, directory)},
            {QStringLiteral("size"), document.font_size},
            {QStringLiteral("threshold"), document.threshold}};
        QJsonArray messages;
        for (const auto &message : document.messages)
            messages.append(QJsonObject{
                {QStringLiteral("text"), message.text},
                {QStringLiteral("mode"), mode_string(message.settings.mode)},
                {QStringLiteral("speed"), message.settings.speed},
                {QStringLiteral("blink"), message.settings.blink},
                {QStringLiteral("border"), message.settings.border}});
        const QJsonObject root{
            {QStringLiteral("format"), QStringLiteral("LedScribe")},
            {QStringLiteral("version"), 1},
            {QStringLiteral("brightness"), document.brightness},
            {QStringLiteral("font"), font},
            {QStringLiteral("messages"), messages}};
        const auto bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            throw std::runtime_error(file.errorString().toStdString());
        if (file.write(bytes) != bytes.size())
        {
            file.cancelWriting();
            throw std::runtime_error(file.errorString().toStdString());
        }
        if (!file.commit())
            throw std::runtime_error(file.errorString().toStdString());
    }
    catch (const std::exception &error)
    {
        throw file_error(path, error);
    }
}

ProjectModel::ProjectModel(QObject *parent) : QAbstractListModel(parent) {}

auto ProjectModel::document() const noexcept -> const ProjectDocument & { return document_; }

auto ProjectModel::replaceDocument(ProjectDocument document) -> void
{
    validate_project(document);
    if (same_document(document_, document))
        return;
    beginResetModel();
    document_ = std::move(document);
    endResetModel();
    Q_EMIT documentChanged();
}

auto ProjectModel::setMessage(int row, MessageEntry message) -> bool
{
    if (row < 0 || row >= document_.messages.size() || !valid_message(message) ||
        same_message(document_.messages[row], message))
        return false;
    document_.messages[row] = std::move(message);
    Q_EMIT dataChanged(index(row, 0), index(row, 0), {Qt::DisplayRole, Qt::EditRole});
    Q_EMIT documentChanged();
    return true;
}

auto ProjectModel::appendMessage() -> bool
{
    if (document_.messages.size() >= 8)
        return false;
    const auto row = rowCount();
    beginInsertRows({}, row, row);
    document_.messages.append(MessageEntry{});
    endInsertRows();
    Q_EMIT documentChanged();
    return true;
}

auto ProjectModel::removeMessage(int row) -> bool
{
    if (document_.messages.size() <= 1 || row < 0 || row >= document_.messages.size())
        return false;
    beginRemoveRows({}, row, row);
    document_.messages.removeAt(row);
    endRemoveRows();
    Q_EMIT documentChanged();
    return true;
}

auto ProjectModel::setBrightness(int brightness) -> void
{
    validate_brightness(brightness);
    if (document_.brightness == brightness)
        return;
    document_.brightness = brightness;
    Q_EMIT documentChanged();
}

auto ProjectModel::setFontOptions(FontChoice choice, QString file, QString vendor_directory,
                                  int font_size, int threshold) -> void
{
    validate_font(choice, font_size, threshold);
    if (document_.font_choice == choice && document_.font_file == file &&
        document_.vendor_font_directory == vendor_directory && document_.font_size == font_size &&
        document_.threshold == threshold)
        return;
    document_.font_choice = choice;
    document_.font_file = std::move(file);
    document_.vendor_font_directory = std::move(vendor_directory);
    document_.font_size = font_size;
    document_.threshold = threshold;
    Q_EMIT documentChanged();
}

auto ProjectModel::rowCount(const QModelIndex &parent) const -> int
{
    return parent.isValid() ? 0 : static_cast<int>(document_.messages.size());
}

auto ProjectModel::data(const QModelIndex &index, int role) const -> QVariant
{
    if (index.model() != this || !index.isValid() || index.column() != 0 ||
        index.row() >= rowCount() || (role != Qt::DisplayRole && role != Qt::EditRole))
        return {};
    return document_.messages[index.row()].text;
}

auto ProjectModel::setData(const QModelIndex &index, const QVariant &value, int role) -> bool
{
    if (index.model() != this || !index.isValid() || index.column() != 0 ||
        index.row() >= rowCount() || role != Qt::EditRole || value.metaType() != QMetaType::fromType<QString>())
        return false;
    auto message = document_.messages[index.row()];
    message.text = value.toString();
    return setMessage(index.row(), std::move(message));
}

auto ProjectModel::flags(const QModelIndex &index) const -> Qt::ItemFlags
{
    if (!index.isValid())
        return Qt::ItemIsDropEnabled;
    if (index.model() != this || index.column() != 0 || index.row() >= rowCount())
        return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
}

auto ProjectModel::moveRows(const QModelIndex &source_parent, int source_row, int count,
                             const QModelIndex &destination_parent, int destination_child) -> bool
{
    if (source_parent.isValid() || destination_parent.isValid() || count != 1 || source_row < 0 ||
        source_row >= rowCount() || destination_child < 0 || destination_child > rowCount() ||
        destination_child == source_row || destination_child == source_row + 1)
        return false;
    if (!beginMoveRows({}, source_row, source_row, {}, destination_child))
        return false;
    document_.messages.move(source_row, destination_child > source_row ? destination_child - 1 : destination_child);
    endMoveRows();
    Q_EMIT documentChanged();
    return true;
}

auto ProjectModel::mimeTypes() const -> QStringList
{
    return {QString::fromLatin1(move_mime_type)};
}

auto ProjectModel::mimeData(const QModelIndexList &indexes) const -> QMimeData *
{
    if (indexes.size() != 1 || indexes.front().model() != this || !indexes.front().isValid() ||
        indexes.front().column() != 0 || indexes.front().row() >= rowCount())
        return nullptr;
    return new MessageMimeData(this, indexes.front());
}

auto ProjectModel::canDropMimeData(const QMimeData *data, Qt::DropAction action, int row,
                                   int column, const QModelIndex &parent) const -> bool
{
    const auto *message = dynamic_cast<const MessageMimeData *>(data);
    if (!message || message->source != this || !message->source_index.isValid() ||
        action != Qt::MoveAction || parent.isValid() || column > 0 || column < -1 ||
        row < -1 || row > rowCount())
        return false;
    const auto destination = row == -1 ? rowCount() : row;
    const auto source = message->source_index.row();
    return destination != source && destination != source + 1;
}

auto ProjectModel::dropMimeData(const QMimeData *data, Qt::DropAction action, int row,
                                int column, const QModelIndex &parent) -> bool
{
    if (!canDropMimeData(data, action, row, column, parent))
        return false;
    const auto *message = static_cast<const MessageMimeData *>(data);
    return moveRows({}, message->source_index.row(), 1, {}, row == -1 ? rowCount() : row);
}

auto ProjectModel::supportedDropActions() const -> Qt::DropActions { return Qt::MoveAction; }
auto ProjectModel::supportedDragActions() const -> Qt::DropActions { return Qt::MoveAction; }

} // namespace ledscribe::gui
