#pragma once

#include "protocol.hpp"

#include <QAbstractListModel>
#include <QString>
#include <QVector>

namespace ledscribe::gui
{

struct MessageEntry
{
    QString text;
    SlotSettings settings;
};

enum class FontChoice { Auto, OutlineAuto, OutlineFile };

struct ProjectDocument
{
    QVector<MessageEntry> messages{MessageEntry{}};
    int brightness{50};
    FontChoice font_choice{FontChoice::Auto};
    QString font_file;
    QString vendor_font_directory;
    int font_size{11};
    int threshold{96};
};

auto validate_project(const ProjectDocument &document) -> void;
auto load_project(const QString &path) -> ProjectDocument;
auto save_project(const QString &path, const ProjectDocument &document) -> void;

class ProjectModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    explicit ProjectModel(QObject *parent = nullptr);
    auto document() const noexcept -> const ProjectDocument &;
    auto replaceDocument(ProjectDocument document) -> void;
    auto setMessage(int row, MessageEntry message) -> bool;
    auto appendMessage() -> bool;
    auto removeMessage(int row) -> bool;
    auto setBrightness(int brightness) -> void;
    auto setFontOptions(FontChoice choice, QString file, QString vendor_directory,
                        int font_size, int threshold) -> void;

    auto rowCount(const QModelIndex &parent = {}) const -> int override;
    auto data(const QModelIndex &index, int role = Qt::DisplayRole) const -> QVariant override;
    auto setData(const QModelIndex &index, const QVariant &value,
                 int role = Qt::EditRole) -> bool override;
    auto flags(const QModelIndex &index) const -> Qt::ItemFlags override;
    auto moveRows(const QModelIndex &source_parent, int source_row, int count,
                  const QModelIndex &destination_parent, int destination_child) -> bool override;
    auto mimeTypes() const -> QStringList override;
    auto mimeData(const QModelIndexList &indexes) const -> QMimeData * override;
    auto canDropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                         const QModelIndex &parent) const -> bool override;
    auto dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                      const QModelIndex &parent) -> bool override;
    auto supportedDropActions() const -> Qt::DropActions override;
    auto supportedDragActions() const -> Qt::DropActions override;

Q_SIGNALS:
    void documentChanged();

private:
    ProjectDocument document_;
};

} // namespace ledscribe::gui
