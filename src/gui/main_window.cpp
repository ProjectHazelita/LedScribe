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
#include <QMenu>
#include <QKeySequence>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <exception>
#include <memory>
#include <utility>
#include <algorithm>
#include <limits>

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
    connect(worker_, &EditorWorker::rendered, this, &MainWindow::acceptRendered,
            Qt::QueuedConnection);
    connect(worker_, &EditorWorker::devicesDiscovered, this, &MainWindow::acceptDevices,
            Qt::QueuedConnection);
    connect(worker_, &EditorWorker::discoveryFailed, this, &MainWindow::acceptDiscoveryFailure,
            Qt::QueuedConnection);
    connect(worker_, &EditorWorker::writeProgress, this, &MainWindow::acceptWriteProgress,
            Qt::QueuedConnection);
    connect(worker_, &EditorWorker::writeFinished, this, &MainWindow::acceptWriteFinished,
            Qt::QueuedConnection);
    connect(worker_, &EditorWorker::writeFailed, this, &MainWindow::acceptWriteFailure,
            Qt::QueuedConnection);
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
    file_menu_ = file;
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
    auto *devices = new QHBoxLayout;
    device_ = new QComboBox(central);
    device_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    device_->setMinimumContentsLength(30);
    device_->addItem(tr("未扫描设备"), -1);
    refresh_ = new QPushButton(tr("刷新设备"), central);
    devices->addWidget(plainLabel(tr("目标设备")));
    devices->addWidget(device_, 1);
    devices->addWidget(refresh_);
    outer->addLayout(devices);
    auto *splitter = new QSplitter(central);
    editor_ = splitter;
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
    progress_ = new QProgressBar(central);
    progress_->setRange(0, 1);
    progress_->setValue(0);
    progress_->setFormat(tr("%v / %m 报告"));
    progress_->setVisible(false);
    write_ = new QPushButton(tr("写入设备"), central);
    write_->setEnabled(false);
    bottom->addWidget(progress_);
    bottom->addWidget(write_);
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
    connect(refresh_, &QPushButton::clicked, this, &MainWindow::refreshDevices);
    connect(write_, &QPushButton::clicked, this, &MainWindow::writeProject);
    connect(device_, &QComboBox::currentIndexChanged, this, [this] {
        updateOperationControls();
    });
}

void MainWindow::syncDocumentControls()
{
    const auto &doc = model_->document();
    const QSignalBlocker a(font_choice_), b(font_file_), c(vendor_directory_),
        d(font_size_), e(threshold_), f(brightness_);
    font_choice_->setCurrentIndex(font_choice_->findData(static_cast<int>(doc.font_choice)));
    if (font_file_->text() != doc.font_file) font_file_->setText(doc.font_file);
    if (vendor_directory_->text() != doc.vendor_font_directory)
        vendor_directory_->setText(doc.vendor_font_directory);
    font_size_->setValue(doc.font_size);
    threshold_->setValue(doc.threshold);
    brightness_->setCurrentIndex(brightness_->findData(doc.brightness));
    const bool explicit_font = doc.font_choice == FontChoice::OutlineFile;
    font_file_->setEnabled(explicit_font && operation_ != Operation::Writing);
    font_browse_->setEnabled(explicit_font && operation_ != Operation::Writing);
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
    // Echoing setText back into a live editor resets its caret, undo and IME composition.
    if (text_->text() != entry.text) text_->setText(entry.text);
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
    updateOperationControls();
}

void MainWindow::dispatchRender()
{
    if (closing_ || !pending_ || operation_ != Operation::Idle) return;
    pending_ = false;
    operation_ = Operation::Rendering;
    updateOperationControls();
    RenderRequestPtr request = std::make_shared<RenderRequest>(RenderRequest{revision_, model_->document()});
    QMetaObject::invokeMethod(worker_, [worker = worker_, request = std::move(request)] {
        worker->render(request);
    }, Qt::QueuedConnection);
}

void MainWindow::acceptRendered(RenderResultPtr result)
{
    if (closing_) return;
    if (operation_ == Operation::Rendering) operation_ = Operation::Idle;
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
    updateOperationControls();
    resumePendingRender();
}

auto MainWindow::selectedDevice() const -> const DeviceInfo *
{
    if (!devices_ || device_->currentIndex() < 0) return nullptr;
    const int index = device_->currentData().toInt();
    if (index < 0 || static_cast<std::size_t>(index) >= devices_->size()) return nullptr;
    const auto &device = (*devices_)[static_cast<std::size_t>(index)];
    return device.discovery_error.empty() ? &device : nullptr;
}

auto MainWindow::canWrite() const -> bool
{
    if (closing_ || operation_ != Operation::Idle || !selectedDevice() || !result_ ||
        result_->revision != revision_ || !result_->global_error.isEmpty() ||
        !result_->layout || !result_->layout->fits) return false;
    const auto count = static_cast<std::size_t>(model_->rowCount());
    if (result_->images.size() != count || result_->fonts.size() != count ||
        static_cast<std::size_t>(result_->errors.size()) != count ||
        result_->layout->slot_count != count) return false;
    for (std::size_t index = 0; index < count; ++index)
        if (model_->document().messages[static_cast<qsizetype>(index)].text.isEmpty() ||
            result_->images[index].width <= 0 || !result_->fonts[index] ||
            !result_->errors[static_cast<qsizetype>(index)].isEmpty()) return false;
    return true;
}

void MainWindow::updateOperationControls()
{
    const bool writing = operation_ == Operation::Writing;
    editor_->setEnabled(!writing);
    brightness_->setEnabled(!writing);
    file_menu_->setEnabled(!writing);
    // Explicitly disable actions too: shortcuts must not bypass the frozen menu.
    for (auto *action : file_menu_->actions()) action->setEnabled(!writing);
    device_->setEnabled(!closing_ && !writing && operation_ != Operation::Discovering);
    refresh_->setEnabled(!closing_ && operation_ == Operation::Idle);
    write_->setEnabled(canWrite());
}

void MainWindow::invalidateDevices(const QString &placeholder)
{
    const QSignalBlocker blocker(device_);
    devices_.reset();
    device_->clear();
    device_->addItem(placeholder, -1);
}

void MainWindow::refreshDevices()
{
    if (closing_ || operation_ != Operation::Idle) return;
    const auto *selected = selectedDevice();
    preserved_device_id_ = selected ? QString::fromStdString(selected->device_id()) : QString{};
    operation_ = Operation::Discovering;
    invalidateDevices(tr("正在扫描设备…"));
    status_->setText(tr("正在读取设备描述符；尚未打开设备"));
    updateOperationControls();
    QMetaObject::invokeMethod(worker_, &EditorWorker::discover, Qt::QueuedConnection);
}

void MainWindow::acceptDevices(DeviceListPtr devices)
{
    if (closing_) return;
    QString preferred_id = preserved_device_id_;
    if (preferred_id.isEmpty())
        if (const auto *selected = selectedDevice())
            preferred_id = QString::fromStdString(selected->device_id());
    if (operation_ == Operation::Discovering) operation_ = Operation::Idle;
    devices_ = std::move(devices);
    {
        const QSignalBlocker blocker(device_);
        device_->clear();
        device_->addItem(tr("请选择设备"), -1);
        int usable_count = 0;
        int only_usable = -1;
        int preserved = -1;
        if (devices_)
        {
            for (std::size_t index = 0; index < devices_->size(); ++index)
            {
                const auto &info = (*devices_)[index];
                QString description = QString::fromStdString(info.description());
                if (!info.discovery_error.empty())
                    description += tr(" — 不可用：%1").arg(QString::fromStdString(info.discovery_error));
                device_->addItem(description, static_cast<int>(index));
                const int row = device_->count() - 1;
                device_->setItemData(row, description, Qt::ToolTipRole);
                if (!info.discovery_error.empty())
                {
                    auto *items = qobject_cast<QStandardItemModel *>(device_->model());
                    if (items && items->item(row)) items->item(row)->setEnabled(false);
                    continue;
                }
                ++usable_count;
                only_usable = row;
                if (QString::fromStdString(info.device_id()) == preferred_id) preserved = row;
            }
        }
        if (!devices_ || devices_->empty()) device_->setItemText(0, tr("未发现设备；连接后刷新"));
        else if (usable_count == 0) device_->setItemText(0, tr("无可用设备；请查看描述符错误"));
        device_->setCurrentIndex(preserved >= 0 ? preserved : (usable_count == 1 ? only_usable : 0));
    }
    preserved_device_id_.clear();
    status_->setText(tr("扫描完成；设备列表不代表已确认访问权限"));
    updateOperationControls();
    resumePendingRender();
}

void MainWindow::acceptDiscoveryFailure(QString error)
{
    if (closing_) return;
    if (operation_ == Operation::Discovering) operation_ = Operation::Idle;
    preserved_device_id_.clear();
    invalidateDevices(tr("扫描失败；请刷新设备"));
    status_->setText(tr("发现设备失败：%1").arg(error));
    updateOperationControls();
    resumePendingRender();
}

void MainWindow::writeProject()
{
    if (confirming_write_ || !canWrite()) return;
    const QScopedValueRollback confirming(confirming_write_, true);
    const auto snapshot = result_;
    const DeviceInfo target = *selectedDevice();
    const auto target_id = target.device_id();
    const quint64 revision = revision_;
    QMessageBox box(QMessageBox::Warning, tr("确认写入设备"),
        tr("设备 ID：%1\n消息数：%2\n传输字节：%3\n\n将覆盖该设备的内容，是否继续？")
            .arg(QString::fromStdString(target_id))
            .arg(static_cast<qulonglong>(snapshot->images.size()))
            .arg(static_cast<qulonglong>(snapshot->layout->payload_bytes)),
        QMessageBox::Ok | QMessageBox::Cancel, this);
    box.setTextFormat(Qt::PlainText);
    box.setDefaultButton(QMessageBox::Cancel);
    box.setEscapeButton(QMessageBox::Cancel);
    if (box.exec() != QMessageBox::Ok) return;
    // The modal event loop can deliver revisions, selection changes, or worker results.
    if (!canWrite() || revision_ != revision || result_ != snapshot ||
        !selectedDevice() || selectedDevice()->device_id() != target_id)
    {
        status_->setText(tr("工程或设备选择已变化，请重新确认后写入"));
        return;
    }
    operation_ = Operation::Writing;
    progress_->setRange(0, 1);
    progress_->setValue(0);
    progress_->setVisible(false); // The first real core callback establishes the total.
    status_->setText(tr("正在准备并写入设备；请勿拔线"));
    updateOperationControls();
    QMetaObject::invokeMethod(worker_, [worker = worker_, target, snapshot] {
        worker->write(target, snapshot);
    }, Qt::QueuedConnection);
}

void MainWindow::acceptWriteProgress(quint64 completed, quint64 total)
{
    if (closing_ || operation_ != Operation::Writing || total == 0) return;
    // Actual badge payloads are small; clamp conversions without inventing progress.
    const auto limit = static_cast<quint64>(std::numeric_limits<int>::max());
    progress_->setRange(0, static_cast<int>(std::min(total, limit)));
    progress_->setValue(static_cast<int>(std::min({completed, total, limit})));
    progress_->setVisible(true);
}

void MainWindow::acceptWriteFinished(QString message)
{
    if (closing_) return;
    if (operation_ == Operation::Writing) operation_ = Operation::Idle;
    status_->setText(message);
    updateOperationControls();
    resumePendingRender();
}

void MainWindow::acceptWriteFailure(QString error)
{
    if (closing_) return;
    if (operation_ == Operation::Writing) operation_ = Operation::Idle;
    preserved_device_id_.clear();
    invalidateDevices(tr("请刷新并重新选择设备"));
    status_->setText(tr("写入失败：%1。设备可能已收到部分内容；未自动重试。请刷新并重新选择设备。").arg(error));
    updateOperationControls();
    resumePendingRender();
}

void MainWindow::resumePendingRender()
{
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
    if (operation_ == Operation::Writing) return false;
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
    if (operation_ == Operation::Writing) return;
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
    if (operation_ == Operation::Writing) return;
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
    if (operation_ == Operation::Writing)
    {
        status_->setText(tr("正在写入，请完成后关闭"));
        event->ignore();
        return;
    }
    if (!confirmDiscard()) { event->ignore(); return; }
    stopWorker();
    event->accept();
}
} // namespace ledscribe::gui
