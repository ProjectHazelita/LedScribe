#include "main_window.hpp"

#include "pixel_preview.hpp"

#include <QAction>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenuBar>
#include <QKeySequence>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <exception>
#include <memory>
#include <utility>

namespace ledscribe::gui
{
namespace
{
const QString project_filter = QStringLiteral("LedScribe 工程 (*.ledscribe.json);;JSON (*.json)");
auto plainLabel(const QString &text, QWidget *parent = nullptr) -> QLabel *
{
    auto *label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    return label;
}
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    registerEditorMetaTypes();
    model_ = new ProjectModel(this);
    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(150);
    buildUi();
    connectUi();
    thread_ = new QThread(this);
    worker_ = new EditorWorker;
    worker_->moveToThread(thread_);
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &EditorWorker::rendered, this, [this](RenderResultPtr result) {
        operation_ = Operation::Idle;
        acceptRendered(std::move(result));
    }, Qt::QueuedConnection);
    thread_->start();
    syncDocumentControls();
    messages_->setCurrentIndex(model_->index(0, 0));
    syncMessageControls();
    updateTitle();
    scheduleRender();
    resize(1100, 760);
    setMinimumSize(900, 640);
}

MainWindow::~MainWindow() { stopWorker(); }

void MainWindow::buildUi()
{
    auto *file = menuBar()->addMenu(tr("文件(&F)"));
    auto action = [this, file](const QString &name, QKeySequence::StandardKey shortcut,
                               auto handler) {
        auto *item = file->addAction(name);
        item->setShortcut(shortcut);
        connect(item, &QAction::triggered, this, handler);
    };
    action(tr("新建(&N)"), QKeySequence::New, &MainWindow::newProject);
    action(tr("打开(&O)…"), QKeySequence::Open, &MainWindow::openProject);
    action(tr("保存(&S)"), QKeySequence::Save, [this] { save(false); });
    action(tr("另存为(&A)…"), QKeySequence::SaveAs, [this] { save(true); });
    file->addSeparator();
    action(tr("退出(&Q)"), QKeySequence::Quit, [this] { close(); });

    auto *central = new QWidget(this);
    auto *outer = new QVBoxLayout(central);
    auto *splitter = new QSplitter(central);
    auto *left = new QWidget(splitter);
    auto *left_layout = new QVBoxLayout(left);
    left_layout->addWidget(plainLabel(tr("消息槽位（拖动排序）")));
    messages_ = new QListView(left);
    messages_->setModel(model_);
    messages_->setSelectionMode(QAbstractItemView::SingleSelection);
    messages_->setDragDropMode(QAbstractItemView::InternalMove);
    messages_->setDefaultDropAction(Qt::MoveAction);
    messages_->setDragEnabled(true);
    messages_->setAcceptDrops(true);
    messages_->setDropIndicatorShown(true);
    messages_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    left_layout->addWidget(messages_);
    auto *buttons = new QHBoxLayout;
    add_ = new QPushButton(tr("添加"), left);
    remove_ = new QPushButton(tr("删除"), left);
    buttons->addWidget(add_);
    buttons->addWidget(remove_);
    left_layout->addLayout(buttons);

    auto *right = new QWidget(splitter);
    auto *right_layout = new QVBoxLayout(right);
    auto *preview_group = new QGroupBox(tr("像素 / 逐帧预览"), right);
    auto *preview_layout = new QVBoxLayout(preview_group);
    auto *scroll = new QScrollArea(preview_group);
    preview_ = new PixelPreview;
    scroll->setWidget(preview_);
    scroll->setWidgetResizable(false);
    scroll->setMinimumHeight(145);
    preview_layout->addWidget(scroll);
    auto *frame_controls = new QHBoxLayout;
    previous_ = new QPushButton(tr("上一帧"));
    next_ = new QPushButton(tr("下一帧"));
    frame_ = new QSpinBox;
    frame_->setMinimum(1);
    frame_total_ = plainLabel(tr("/ 0 帧"));
    auto *scale = new QSpinBox;
    scale->setRange(4, 16);
    scale->setValue(10);
    frame_controls->addWidget(previous_);
    frame_controls->addWidget(frame_);
    frame_controls->addWidget(frame_total_);
    frame_controls->addWidget(next_);
    frame_controls->addStretch();
    frame_controls->addWidget(plainLabel(tr("整数缩放")));
    frame_controls->addWidget(scale);
    preview_layout->addLayout(frame_controls);
    connect(scale, &QSpinBox::valueChanged, preview_, &PixelPreview::setScale);
    preview_layout->addWidget(plainLabel(tr("像素/逐帧预览；滚动、闪烁、边框与特效由设备执行")));
    font_info_ = plainLabel({});
    preview_layout->addWidget(font_info_);
    right_layout->addWidget(preview_group);

    auto *message_group = new QGroupBox(tr("当前消息"), right);
    auto *message_form = new QFormLayout(message_group);
    text_ = new QLineEdit;
    text_->setMaxLength(32767);
    message_form->addRow(tr("文字"), text_);
    auto *settings = new QHBoxLayout;
    mode_ = new QComboBox;
    const std::pair<const char *, DisplayMode> modes[] = {
        {"左移", DisplayMode::left}, {"右移", DisplayMode::right},
        {"上移", DisplayMode::up}, {"下移", DisplayMode::down},
        {"静止", DisplayMode::still}, {"动画", DisplayMode::animation},
        {"飘雪", DisplayMode::drop}, {"画卷", DisplayMode::curtain},
        {"镭射", DisplayMode::laser}};
    for (const auto &[name, value] : modes)
        mode_->addItem(QString::fromUtf8(name), static_cast<int>(value));
    speed_ = new QSpinBox;
    speed_->setRange(1, 8);
    blink_ = new QCheckBox(tr("闪烁"));
    border_ = new QCheckBox(tr("边框"));
    settings->addWidget(mode_);
    settings->addWidget(plainLabel(tr("速度")));
    settings->addWidget(speed_);
    settings->addWidget(blink_);
    settings->addWidget(border_);
    settings->addStretch();
    message_form->addRow(tr("显示"), settings);
    right_layout->addWidget(message_group);

    auto *global_group = new QGroupBox(tr("全局字体设置（全部消息）"), right);
    auto *global_form = new QFormLayout(global_group);
    font_choice_ = new QComboBox;
    font_choice_->addItem(tr("自动（优先点阵）"), static_cast<int>(FontChoice::Auto));
    font_choice_->addItem(tr("自动轮廓"), static_cast<int>(FontChoice::OutlineAuto));
    font_choice_->addItem(tr("指定轮廓"), static_cast<int>(FontChoice::OutlineFile));
    global_form->addRow(tr("字体来源"), font_choice_);
    auto *font_row = new QHBoxLayout;
    font_file_ = new QLineEdit;
    font_browse_ = new QPushButton(tr("浏览…"));
    font_row->addWidget(font_file_);
    font_row->addWidget(font_browse_);
    global_form->addRow(tr("轮廓字体文件"), font_row);
    auto *vendor_row = new QHBoxLayout;
    vendor_directory_ = new QLineEdit;
    auto *vendor_browse = new QPushButton(tr("浏览…"));
    vendor_row->addWidget(vendor_directory_);
    vendor_row->addWidget(vendor_browse);
    global_form->addRow(tr("点阵字库目录"), vendor_row);
    auto *raster_row = new QHBoxLayout;
    font_size_ = new QSpinBox;
    font_size_->setRange(6, 256);
    threshold_ = new QSpinBox;
    threshold_->setRange(0, 255);
    raster_row->addWidget(plainLabel(tr("字号")));
    raster_row->addWidget(font_size_);
    raster_row->addWidget(plainLabel(tr("阈值")));
    raster_row->addWidget(threshold_);
    raster_row->addStretch();
    global_form->addRow(tr("轮廓光栅"), raster_row);
    right_layout->addWidget(global_group);
    right_layout->addStretch();
    splitter->setSizes({220, 850});
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    outer->addWidget(splitter, 1);
    auto *bottom = new QHBoxLayout;
    brightness_ = new QComboBox;
    for (const int value : {25, 50, 75, 100})
        brightness_->addItem(QString::number(value) + QLatin1Char('%'), value);
    bottom->addWidget(plainLabel(tr("亮度")));
    bottom->addWidget(brightness_);
    capacity_ = plainLabel(tr("消息未完成"));
    bottom->addWidget(capacity_, 1);
    outer->addLayout(bottom);
    status_ = plainLabel({});
    statusBar()->addWidget(status_, 1);
    setCentralWidget(central);

    connect(font_browse_, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, tr("选择轮廓字体"),
            font_file_->text(), tr("字体 (*.ttf *.otf *.ttc);;所有文件 (*)"));
        if (!path.isEmpty()) font_file_->setText(path);
    });
    connect(vendor_browse, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getExistingDirectory(this, tr("选择点阵字库目录"),
                                                           vendor_directory_->text());
        if (!path.isEmpty()) vendor_directory_->setText(path);
    });
}

void MainWindow::connectUi()
{
    connect(model_, &ProjectModel::documentChanged, this, &MainWindow::documentChanged);
    connect(messages_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this] { syncMessageControls(); showSelectedPreview(); });
    connect(add_, &QPushButton::clicked, this, [this] {
        if (model_->appendMessage()) messages_->setCurrentIndex(model_->index(model_->rowCount() - 1, 0));
    });
    connect(remove_, &QPushButton::clicked, this, [this] {
        const int row = messages_->currentIndex().row();
        if (model_->removeMessage(row))
            messages_->setCurrentIndex(model_->index(qMin(row, model_->rowCount() - 1), 0));
    });
    connect(text_, &QLineEdit::textChanged, this, &MainWindow::updateMessage);
    connect(mode_, &QComboBox::currentIndexChanged, this, &MainWindow::updateMessage);
    connect(speed_, &QSpinBox::valueChanged, this, &MainWindow::updateMessage);
    connect(blink_, &QCheckBox::toggled, this, &MainWindow::updateMessage);
    connect(border_, &QCheckBox::toggled, this, &MainWindow::updateMessage);
    connect(font_choice_, &QComboBox::currentIndexChanged, this, &MainWindow::updateFontOptions);
    connect(font_file_, &QLineEdit::textChanged, this, &MainWindow::updateFontOptions);
    connect(vendor_directory_, &QLineEdit::textChanged, this, &MainWindow::updateFontOptions);
    connect(font_size_, &QSpinBox::valueChanged, this, &MainWindow::updateFontOptions);
    connect(threshold_, &QSpinBox::valueChanged, this, &MainWindow::updateFontOptions);
    connect(brightness_, &QComboBox::currentIndexChanged, this, [this] {
        model_->setBrightness(brightness_->currentData().toInt());
    });
    connect(debounce_, &QTimer::timeout, this, &MainWindow::dispatchRender);
    connect(previous_, &QPushButton::clicked, this, [this] { frame_->setValue(frame_->value() - 1); });
    connect(next_, &QPushButton::clicked, this, [this] { frame_->setValue(frame_->value() + 1); });
    connect(frame_, &QSpinBox::valueChanged, this, [this](int value) {
        preview_->setFrame(value - 1);
        updateFrames();
    });
}

void MainWindow::syncDocumentControls()
{
    const auto &doc = model_->document();
    const QSignalBlocker a(font_choice_), b(font_file_), c(vendor_directory_),
        d(font_size_), e(threshold_), f(brightness_);
    font_choice_->setCurrentIndex(font_choice_->findData(static_cast<int>(doc.font_choice)));
    font_file_->setText(doc.font_file);
    vendor_directory_->setText(doc.vendor_font_directory);
    font_size_->setValue(doc.font_size);
    threshold_->setValue(doc.threshold);
    brightness_->setCurrentIndex(brightness_->findData(doc.brightness));
    const bool explicit_font = doc.font_choice == FontChoice::OutlineFile;
    font_file_->setEnabled(explicit_font);
    font_browse_->setEnabled(explicit_font);
}

void MainWindow::syncMessageControls()
{
    const int row = messages_->currentIndex().row();
    const bool valid = row >= 0 && row < model_->rowCount();
    for (auto *widget : {static_cast<QWidget *>(text_), static_cast<QWidget *>(mode_),
                         static_cast<QWidget *>(speed_), static_cast<QWidget *>(blink_),
                         static_cast<QWidget *>(border_)}) widget->setEnabled(valid);
    add_->setEnabled(model_->rowCount() < MAX_MESSAGES);
    remove_->setEnabled(valid && model_->rowCount() > 1);
    const QSignalBlocker a(text_), b(mode_), c(speed_), d(blink_), e(border_);
    if (!valid) { text_->clear(); return; }
    const auto &entry = model_->document().messages[row];
    text_->setText(entry.text);
    mode_->setCurrentIndex(mode_->findData(static_cast<int>(entry.settings.mode)));
    speed_->setValue(entry.settings.speed);
    blink_->setChecked(entry.settings.blink);
    border_->setChecked(entry.settings.border);
}

void MainWindow::updateMessage()
{
    const int row = messages_->currentIndex().row();
    if (row < 0 || row >= model_->rowCount()) return;
    MessageEntry entry;
    entry.text = text_->text();
    entry.settings.mode = static_cast<DisplayMode>(mode_->currentData().toInt());
    entry.settings.speed = speed_->value();
    entry.settings.blink = blink_->isChecked();
    entry.settings.border = border_->isChecked();
    model_->setMessage(row, std::move(entry));
}

void MainWindow::updateFontOptions()
{
    model_->setFontOptions(static_cast<FontChoice>(font_choice_->currentData().toInt()),
                          font_file_->text(), vendor_directory_->text(), font_size_->value(),
                          threshold_->value());
}

void MainWindow::documentChanged()
{
    modified_ = true;
    ++revision_;
    syncDocumentControls();
    syncMessageControls();
    updateTitle();
    scheduleRender();
}

void MainWindow::scheduleRender()
{
    result_.reset();
    preview_->clearBitmap();
    font_info_->clear();
    capacity_->setText(tr("消息未完成"));
    status_->setText(tr("预览更新中"));
    updateFrames();
    pending_ = true;
    debounce_->start();
}

void MainWindow::dispatchRender()
{
    if (closing_ || !pending_ || operation_ != Operation::Idle) return;
    pending_ = false;
    operation_ = Operation::Rendering;
    RenderRequestPtr request = std::make_shared<RenderRequest>(RenderRequest{revision_, model_->document()});
    QMetaObject::invokeMethod(worker_, [worker = worker_, request = std::move(request)] {
        worker->render(request);
    }, Qt::QueuedConnection);
}

void MainWindow::acceptRendered(RenderResultPtr result)
{
    if (closing_) return;
    if (result && result->revision == revision_)
    {
        result_ = std::move(result);
        if (result_->layout)
        {
            capacity_->setText(tr("容量：%1 / %2 字节%3")
                .arg(static_cast<qulonglong>(result_->layout->payload_bytes)).arg(MAX_PAYLOAD)
                .arg(result_->layout->fits ? QString{} : tr("（超出容量）")));
        }
        else capacity_->setText(tr("消息未完成"));
        showSelectedPreview();
    }
    if (pending_ && !debounce_->isActive()) dispatchRender();
}

void MainWindow::showSelectedPreview()
{
    preview_->clearBitmap();
    font_info_->clear();
    if (!result_) { updateFrames(); return; }
    const int row = messages_->currentIndex().row();
    if (!result_->global_error.isEmpty()) status_->setText(result_->global_error);
    else if (row >= 0 && row < result_->errors.size() && !result_->errors[row].isEmpty())
        status_->setText(result_->errors[row]);
    else if (row >= 0 && static_cast<std::size_t>(row) < result_->images.size() &&
             result_->images[static_cast<std::size_t>(row)].width > 0)
    {
        const auto index = static_cast<std::size_t>(row);
        preview_->setBitmap(std::shared_ptr<const GlyphBitmap>(result_, &result_->images[index]),
                            result_->settings.slots[index].mode);
        if (index < result_->fonts.size() && result_->fonts[index])
        {
            const auto &font = *result_->fonts[index];
            font_info_->setText(tr("字体：%1 · %2%3")
                .arg(font.source == FontSource::vendor ? tr("点阵") : tr("轮廓"))
                .arg(QString::fromStdString(font.font_path.string()))
                .arg(font.fallback_reason.empty() ? QString{} :
                     tr("；回退原因：%1").arg(QString::fromStdString(font.fallback_reason))));
        }
        if (result_->layout && !result_->layout->fits) status_->setText(tr("项目超出设备容量；请缩短消息"));
        else if (!result_->global_error.isEmpty()) status_->setText(result_->global_error);
        else if (!result_->layout) status_->setText(tr("当前消息预览就绪；其他消息未完成"));
        else status_->setText(tr("预览就绪"));
    }
    else status_->setText(tr("消息未完成"));
    updateFrames();
}

void MainWindow::updateFrames()
{
    const int count = preview_->frameCount();
    const int index = preview_->frameIndex();
    const QSignalBlocker blocker(frame_);
    frame_->setRange(1, qMax(1, count));
    frame_->setValue(count > 0 ? index + 1 : 1);
    frame_->setEnabled(count > 1);
    previous_->setEnabled(count > 0 && index > 0);
    next_->setEnabled(count > 0 && index + 1 < count);
    frame_total_->setText(tr("/ %1 帧").arg(count));
}

void MainWindow::updateTitle()
{
    setWindowModified(modified_);
    setWindowTitle(tr("%1[*] — LedScribe").arg(path_.isEmpty() ? tr("未命名工程") : QFileInfo(path_).fileName()));
}

void MainWindow::showError(const QString &title, const QString &message)
{
    QMessageBox box(QMessageBox::Critical, title, message, QMessageBox::Ok, this);
    box.setTextFormat(Qt::PlainText);
    box.exec();
}

auto MainWindow::confirmDiscard() -> bool
{
    if (!modified_) return true;
    QMessageBox box(QMessageBox::Warning, tr("未保存的工程"), tr("是否保存当前工程的修改？"),
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
    box.setTextFormat(Qt::PlainText);
    box.setDefaultButton(QMessageBox::Save);
    const auto answer = box.exec();
    if (answer == QMessageBox::Save) return save(false);
    return answer == QMessageBox::Discard;
}

auto MainWindow::save(bool save_as) -> bool
{
    QString target = path_;
    if (save_as || target.isEmpty())
    {
        target = QFileDialog::getSaveFileName(this, tr("保存 LedScribe 工程"),
            target.isEmpty() ? QStringLiteral("未命名.ledscribe.json") : target, project_filter);
        if (target.isEmpty()) return false;
    }
    try { save_project(target, model_->document()); }
    catch (const std::exception &error)
    {
        showError(tr("保存失败"), QString::fromUtf8(error.what()));
        return false;
    }
    path_ = target;
    modified_ = false;
    updateTitle();
    return true;
}

void MainWindow::newProject()
{
    if (!confirmDiscard()) return;
    model_->replaceDocument(ProjectDocument{});
    path_.clear();
    modified_ = false;
    messages_->setCurrentIndex(model_->index(0, 0));
    syncDocumentControls();
    syncMessageControls();
    updateTitle();
}

void MainWindow::openProject()
{
    const auto path = QFileDialog::getOpenFileName(this, tr("打开 LedScribe 工程"), path_, project_filter);
    if (path.isEmpty()) return;
    try
    {
        auto document = load_project(path);
        if (!confirmDiscard()) return;
        model_->replaceDocument(std::move(document));
        path_ = path;
        modified_ = false;
        messages_->setCurrentIndex(model_->index(0, 0));
        syncDocumentControls();
        syncMessageControls();
        updateTitle();
    }
    catch (const std::exception &error) { showError(tr("打开失败"), QString::fromUtf8(error.what())); }
}

void MainWindow::stopWorker()
{
    closing_ = true;
    pending_ = false;
    debounce_->stop();
    if (thread_ && thread_->isRunning())
    {
        thread_->quit();
        thread_->wait();
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!confirmDiscard()) { event->ignore(); return; }
    stopWorker();
    event->accept();
}
} // namespace ledscribe::gui
