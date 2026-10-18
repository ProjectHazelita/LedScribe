#include <QAction>
#include <QAbstractButton>
#include <QApplication>
#include <QFile>
#include <QFileDialog>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListView>
#include <QMessageBox>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include "main_window.hpp"
#include "pixel_preview.hpp"
#include "project.hpp"
#include "worker.hpp"

using namespace ledscribe::gui;

namespace
{
auto writeBytes(const QString &path, const QByteArray &bytes) -> void
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        throw std::runtime_error("cannot create test fixture");
}

auto triggerFileAction(MainWindow &window, QKeySequence::StandardKey key) -> void
{
    for (auto *action : window.findChildren<QAction *>())
    {
        if (action->shortcut() == QKeySequence(key))
        {
            action->trigger();
            return;
        }
    }
    throw std::runtime_error("standard file action is unavailable");
}

auto dismissMessage(QMessageBox::StandardButton button) -> void
{
    QTimer::singleShot(0, [button]
    {
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *box = qobject_cast<QMessageBox *>(widget))
                if (auto *target = box->button(button)) target->click();
    });
}

auto chooseFile(const QString &path) -> void
{
    QTimer::singleShot(0, [path]
    {
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *dialog = qobject_cast<QFileDialog *>(widget))
            {
                dialog->selectFile(path);
                QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
            }
    });
}

auto makeFonts(const QString &directory) -> void
{
    QByteArray ascii(ASCII_FONT_SIZE, '\0');
    for (int row = 0; row < ROWS; ++row)
        ascii[static_cast<int>('A') * ROWS + row] = static_cast<char>(0x80U >> (row % 8));
    writeBytes(directory + "/ASC11", ascii);
    writeBytes(directory + "/HZK11", QByteArray(GBK_FONT_SIZE, '\0'));
}

auto bitmap(int width) -> std::shared_ptr<const GlyphBitmap>
{
    auto image = std::make_shared<GlyphBitmap>();
    image->width = width;
    image->pixels.resize(static_cast<std::size_t>(width * ROWS));
    for (int y = 0; y < ROWS; ++y)
        for (int x = 0; x < width; ++x)
            image->pixels[static_cast<std::size_t>(y * width + x)] =
                ((x * 7 + y * 3) % 13 < 5) ? 255 : 0;
    return image;
}

auto validResult(quint64 revision, int width = 8) -> RenderResultPtr
{
    auto result = std::make_shared<RenderResult>();
    result->revision = revision;
    result->images.push_back(*bitmap(width));
    result->fonts.emplace_back(RenderInfo{.source = FontSource::vendor,
                                         .font_path = {}, .fallback_reason = {}});
    result->errors.append(QString{});
    result->layout = measure_payload(result->images, result->settings);
    return result;
}
}

class GuiTests : public QObject
{
    Q_OBJECT
  private Q_SLOTS:
    void initTestCase()
    {
        registerEditorMetaTypes();
    }

    void modelMovesAndBoundaries()
    {
        ProjectModel model;
        QSignalSpy changes(&model, &ProjectModel::documentChanged);
        MessageEntry first;
        first.text = "first";
        first.settings = {.speed = 2, .mode = DisplayMode::animation, .blink = true, .border = false};
        MessageEntry second;
        second.text = "second";
        second.settings = {.speed = 7, .mode = DisplayMode::laser, .blink = false, .border = true};
        QVERIFY(model.setMessage(0, first));
        QVERIFY(model.appendMessage());
        QVERIFY(model.setMessage(1, second));
        QVERIFY(model.moveRows({}, 0, 1, {}, 2));
        const auto &moved = model.document().messages;
        QCOMPARE(moved[0].text, second.text);
        QCOMPARE(moved[0].settings.speed, 7);
        QCOMPARE(moved[0].settings.mode, DisplayMode::laser);
        QVERIFY(moved[0].settings.border);
        QVERIFY(!moved[0].settings.blink);
        QCOMPARE(moved[1].text, first.text);
        QCOMPARE(moved[1].settings.speed, 2);
        QCOMPARE(moved[1].settings.mode, DisplayMode::animation);
        QVERIFY(moved[1].settings.blink);
        QVERIFY(!moved[1].settings.border);
        const auto count = changes.count();
        QVERIFY(!model.setMessage(0, second));
        QVERIFY(!model.setMessage(-1, first));
        QVERIFY(!model.moveRows({}, 0, 1, {}, 1));
        QVERIFY(!model.moveRows(model.index(0), 0, 1, {}, 2));
        QVERIFY(!model.moveRows({}, 0, 2, {}, 2));
        QCOMPARE(changes.count(), count);
        while (model.rowCount() < MAX_MESSAGES) QVERIFY(model.appendMessage());
        const auto atMaximum = changes.count();
        QVERIFY(!model.appendMessage());
        QCOMPARE(changes.count(), atMaximum);
        while (model.rowCount() > 1) QVERIFY(model.removeMessage(0));
        const auto atMinimum = changes.count();
        QVERIFY(!model.removeMessage(0));
        QCOMPARE(changes.count(), atMinimum);
    }

    void settersAreAtomic()
    {
        ProjectModel model;
        QSignalSpy changes(&model, &ProjectModel::documentChanged);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, model.setBrightness(42));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument,
                                 model.setFontOptions(FontChoice::Auto, {}, {}, 257, 96));
        QCOMPARE(model.document().brightness, 50);
        QCOMPARE(model.document().font_size, 11);
        QCOMPARE(changes.count(), 0);
        model.setBrightness(50);
        model.setFontOptions(FontChoice::Auto, {}, {}, 11, 96);
        QCOMPARE(changes.count(), 0);
        MessageEntry bad;
        bad.text = "one\ntwo";
        QVERIFY(!model.setMessage(0, bad));
        bad.text = "one\rtwo";
        QVERIFY(!model.setMessage(0, bad));
        bad.text = QString(32768, QChar('a'));
        QVERIFY(!model.setMessage(0, bad));
        bad.text = QString(32767, QChar('a'));
        QVERIFY(model.setMessage(0, bad));
        QCOMPARE(changes.count(), 1);
    }

    void projectRoundTripAndPaths()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        ProjectModel model;
        MessageEntry entry;
        entry.text = "A";
        entry.settings = {.speed = 8, .mode = DisplayMode::animation, .blink = true, .border = true};
        QVERIFY(model.setMessage(0, entry));
        QVERIFY(model.appendMessage()); // empty draft must survive round-trip
        model.setBrightness(75);
        const QString absentFont = directory.filePath("missing.ttf");
        const QString absentVendor = directory.filePath("missing-vendor");
        model.setFontOptions(FontChoice::OutlineFile, absentFont, absentVendor, 256, 255);
        QVERIFY(model.moveRows({}, 0, 1, {}, 2));
        const QString path = directory.filePath("project.ledscribe.json");
        save_project(path, model.document());
        const auto loaded = load_project(path);
        QCOMPARE(loaded.messages.size(), 2);
        QCOMPARE(loaded.messages[0].text, QString());
        QCOMPARE(loaded.messages[1].text, QString("A"));
        QCOMPARE(loaded.messages[1].settings.mode, DisplayMode::animation);
        QCOMPARE(loaded.messages[1].settings.speed, 8);
        QVERIFY(loaded.messages[1].settings.blink && loaded.messages[1].settings.border);
        QCOMPARE(loaded.brightness, 75);
        QCOMPARE(loaded.font_choice, FontChoice::OutlineFile);
        QCOMPARE(loaded.font_file, absentFont);
        QCOMPARE(loaded.vendor_font_directory, absentVendor);
        QCOMPARE(loaded.font_size, 256);
        QCOMPARE(loaded.threshold, 255);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        auto object = QJsonDocument::fromJson(file.readAll()).object();
        auto font = object["font"].toObject();
        font["file"] = "missing-relative.ttf";
        font["vendorDirectory"] = "missing-relative-directory";
        object["font"] = font;
        file.close();
        writeBytes(path, QJsonDocument(object).toJson());
        const auto relative = load_project(path);
        QCOMPARE(relative.font_file, directory.filePath("missing-relative.ttf"));
        QCOMPARE(relative.vendor_font_directory, directory.filePath("missing-relative-directory"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, save_project(directory.path(), loaded));
    }

    void invalidProjectsNeverReplace()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath("input.json");
        ProjectModel model;
        MessageEntry original;
        original.text = "preserve";
        QVERIFY(model.setMessage(0, original));
        save_project(path, model.document());
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject base = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        QVector<QJsonObject> invalid;
        auto changed = base;
        changed["version"] = 2; invalid.append(changed);
        changed = base; changed["brightness"] = "50"; invalid.append(changed);
        changed = base; changed["brightness"] = 51; invalid.append(changed);
        changed = base; changed.remove("font"); invalid.append(changed);
        changed = base; changed["messages"] = QJsonArray(); invalid.append(changed);
        changed = base;
        QJsonArray nine;
        for (int index = 0; index < 9; ++index) nine.append(base["messages"].toArray()[0]);
        changed["messages"] = nine; invalid.append(changed);
        for (const auto &mode : {QJsonValue("5"), QJsonValue(5), QJsonValue("invalid")})
        {
            changed = base;
            auto entry = base["messages"].toArray()[0].toObject();
            entry["mode"] = mode;
            changed["messages"] = QJsonArray{entry}; invalid.append(changed);
        }
        for (const QString &text : {QString("a\nb"), QString("a\rb"), QString(32768, QChar('x'))})
        {
            changed = base;
            auto entry = base["messages"].toArray()[0].toObject();
            entry["text"] = text;
            changed["messages"] = QJsonArray{entry}; invalid.append(changed);
        }
        changed = base;
        auto entry = base["messages"].toArray()[0].toObject();
        entry["speed"] = 1.5;
        changed["messages"] = QJsonArray{entry}; invalid.append(changed);
        changed = base;
        entry = base["messages"].toArray()[0].toObject();
        entry["blink"] = 1;
        changed["messages"] = QJsonArray{entry}; invalid.append(changed);
        for (const auto &object : invalid)
        {
            writeBytes(path, QJsonDocument(object).toJson());
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, model.replaceDocument(load_project(path)));
            QCOMPARE(model.document().messages[0].text, QString("preserve"));
        }
        writeBytes(path, "not JSON");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, model.replaceDocument(load_project(path)));
        QCOMPARE(model.document().messages[0].text, QString("preserve"));
    }

    void animationPixelsAndFrameBoundaries()
    {
        PixelPreview preview;
        for (int width : {45, 88, 176})
        {
            const auto source = bitmap(width);
            preview.setBitmap(source, DisplayMode::animation);
            const int frames = (width + DISPLAY_WIDTH - 1) / DISPLAY_WIDTH;
            QCOMPARE(preview.frameCount(), frames);
            for (int frame = 0; frame < frames; ++frame)
            {
                preview.setFrame(frame);
                const QImage rendered = preview.frameImage();
                QCOMPARE(rendered.size(), QSize(DISPLAY_WIDTH, ROWS));
                for (int y = 0; y < ROWS; ++y)
                    for (int x = 0; x < DISPLAY_WIDTH; ++x)
                    {
                        const int sourceX = frame * DISPLAY_WIDTH + x;
                        const bool lit = sourceX < width &&
                            source->pixels[static_cast<std::size_t>(y * width + sourceX)] > 127;
                        QCOMPARE(rendered.pixelColor(x, y),
                                 preview.palette().color(lit ? QPalette::Text : QPalette::Base));
                    }
            }
            preview.setFrame(-100);
            const QImage first = preview.frameImage();
            preview.setFrame(0);
            QCOMPARE(preview.frameImage(), first);
            preview.setFrame(1000);
            const QImage last = preview.frameImage();
            preview.setFrame(frames - 1);
            QCOMPARE(preview.frameImage(), last);
            preview.setBitmap(source, DisplayMode::left);
            QCOMPARE(preview.frameCount(), 1);
            QCOMPARE(preview.frameImage().size(), QSize(width, ROWS));
        }
        preview.clearBitmap();
        QVERIFY(preview.frameImage().isNull());
        QCOMPARE(preview.frameCount(), 0);
    }

    void workerDraftErrorsAndCapacity()
    {
        QTemporaryDir directory;
        makeFonts(directory.path());
        EditorWorker worker;
        QSignalSpy spy(&worker, &EditorWorker::rendered);
        auto request = std::make_shared<RenderRequest>();
        request->revision = 1;
        request->document.vendor_font_directory = directory.path();
        worker.render(request);
        QCOMPARE(spy.count(), 1);
        auto result = qvariant_cast<RenderResultPtr>(spy.takeFirst()[0]);
        QVERIFY(!result->layout.has_value());
        QCOMPARE(result->images.size(), std::size_t(1));
        QVERIFY(!result->errors[0].isEmpty());
        request->document.messages[0].text = "A";
        worker.render(request);
        result = qvariant_cast<RenderResultPtr>(spy.takeFirst()[0]);
        QVERIFY(result->layout && result->layout->fits);
        QCOMPARE(result->images[0].width, 8);
        QCOMPARE(result->images[0].pixels[0], uint8_t(255));
        QCOMPARE(result->images[0].pixels[1], uint8_t(0));
        QVERIFY(result->fonts[0].has_value());
        QCOMPARE(result->fonts[0]->source, FontSource::vendor);
        request->document.messages[0].text = QString(1000, QChar('A'));
        worker.render(request);
        result = qvariant_cast<RenderResultPtr>(spy.takeFirst()[0]);
        QVERIFY(result->layout && !result->layout->fits);
        QVERIFY(result->layout->payload_bytes > MAX_PAYLOAD);
        request->document.font_choice = FontChoice::OutlineFile;
        request->document.font_file = directory.filePath("missing.ttf");
        worker.render(request);
        result = qvariant_cast<RenderResultPtr>(spy.takeFirst()[0]);
        QVERIFY(!result->layout);
        QVERIFY(!result->global_error.isEmpty() || !result->errors[0].isEmpty());
    }

    void staleSnapshotsAndSelectionDoNotMutateDocument()
    {
        MainWindow window;
        auto *model = window.projectModel();
        QSignalSpy changes(model, &ProjectModel::documentChanged);
        const quint64 initial = window.revision();
        window.acceptRendered(validResult(initial));
        QVERIFY(window.renderResult());
        MessageEntry changed;
        changed.text = "new";
        QVERIFY(model->setMessage(0, changed));
        const quint64 current = window.revision();
        QVERIFY(current > initial);
        QVERIFY(!window.renderResult());
        window.acceptRendered(validResult(initial));
        QVERIFY(!window.renderResult());
        window.acceptRendered(validResult(current));
        QVERIFY(window.renderResult());
        QCOMPARE(window.renderResult()->revision, current);
        auto failed = std::make_shared<RenderResult>();
        failed->revision = current;
        failed->images.push_back(GlyphBitmap{.pixels = {}, .width = 0});
        failed->fonts.emplace_back(std::nullopt);
        failed->errors.append("invalid text");
        window.acceptRendered(failed);
        QVERIFY(!window.renderResult()->layout);
        QVERIFY(model->appendMessage());
        const auto revision = window.revision();
        const auto count = changes.count();
        const auto lists = window.findChildren<QListView *>();
        QVERIFY(!lists.empty());
        lists.front()->setCurrentIndex(model->index(1));
        QCOMPARE(window.revision(), revision);
        QCOMPARE(changes.count(), count);
        QVERIFY(window.isModified());
    }

    void fileCancellationAndSaveFailureKeepDirty()
    {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        MainWindow window;
        MessageEntry entry;
        entry.text = "preserve";
        QVERIFY(window.projectModel()->setMessage(0, entry));
        const auto revision = window.revision();
        dismissMessage(QMessageBox::Cancel);
        triggerFileAction(window, QKeySequence::New);
        QCOMPARE(window.projectModel()->document().messages[0].text, QString("preserve"));
        QCOMPARE(window.revision(), revision);
        QVERIFY(window.isModified());
        QTimer::singleShot(0, []
        {
            for (auto *widget : QApplication::topLevelWidgets())
                if (auto *dialog = qobject_cast<QFileDialog *>(widget)) dialog->reject();
        });
        triggerFileAction(window, QKeySequence::Save);
        QVERIFY(window.isModified());
        QTemporaryDir directory;
        const QString path = directory.filePath("saved.ledscribe.json");
        chooseFile(path);
        triggerFileAction(window, QKeySequence::Save);
        QVERIFY(!window.isModified());
        QCOMPARE(load_project(path).messages[0].text, entry.text);
        const auto savedRevision = window.revision();
        const auto savedTitle = window.windowTitle();
        QTimer::singleShot(0, []
        {
            for (auto *widget : QApplication::topLevelWidgets())
                if (auto *dialog = qobject_cast<QFileDialog *>(widget)) dialog->reject();
        });
        triggerFileAction(window, QKeySequence::Open);
        QCOMPARE(window.revision(), savedRevision);
        QCOMPARE(window.windowTitle(), savedTitle);
        QVERIFY(!window.isModified());
        const QString invalidPath = directory.filePath("invalid.json");
        writeBytes(invalidPath, "{invalid JSON");
        QTimer dismissError;
        connect(&dismissError, &QTimer::timeout, []
        {
            for (auto *widget : QApplication::topLevelWidgets())
                if (auto *box = qobject_cast<QMessageBox *>(widget))
                    if (auto *button = box->button(QMessageBox::Ok)) button->click();
        });
        dismissError.start(10);
        chooseFile(invalidPath);
        triggerFileAction(window, QKeySequence::Open);
        dismissError.stop();
        QCOMPARE(window.revision(), savedRevision);
        QCOMPARE(window.windowTitle(), savedTitle);
        QCOMPARE(window.projectModel()->document().messages[0].text, entry.text);
        QVERIFY(!window.isModified());
        QVERIFY(QFile::remove(invalidPath));
        entry.text = "keep after failure";
        QVERIFY(window.projectModel()->setMessage(0, entry));
        QVERIFY(QFile::remove(path));
        QVERIFY(QDir().rmdir(directory.path()));
        dismissMessage(QMessageBox::Ok);
        triggerFileAction(window, QKeySequence::Save);
        QVERIFY(window.isModified());
        QCOMPARE(window.projectModel()->document().messages[0].text, entry.text);
    }
};

QTEST_MAIN(GuiTests)
#include "gui_tests.moc"
