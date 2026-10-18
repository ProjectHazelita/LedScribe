#pragma once

#include "project.hpp"
#include "worker.hpp"

#include <QMainWindow>

class QAction;
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QSpinBox;
class QThread;
class QTimer;

namespace ledscribe::gui
{
class PixelPreview;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
    auto projectModel() const noexcept -> ProjectModel * { return model_; }
    auto revision() const noexcept -> quint64 { return revision_; }
    auto renderResult() const noexcept -> RenderResultPtr { return result_; }
    auto isModified() const noexcept -> bool { return modified_; }

public Q_SLOTS:
    void acceptRendered(RenderResultPtr result);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    enum class Operation { Idle, Rendering };
    void buildUi();
    void connectUi();
    void syncDocumentControls();
    void syncMessageControls();
    void updateMessage();
    void updateFontOptions();
    void documentChanged();
    void scheduleRender();
    void dispatchRender();
    void showSelectedPreview();
    void updateFrames();
    void updateTitle();
    void showError(const QString &title, const QString &message);
    auto confirmDiscard() -> bool;
    auto save(bool save_as) -> bool;
    void newProject();
    void openProject();
    void stopWorker();

    ProjectModel *model_{};
    EditorWorker *worker_{};
    QThread *thread_{};
    QTimer *debounce_{};
    Operation operation_{Operation::Idle};
    quint64 revision_{};
    RenderResultPtr result_;
    bool pending_{};
    bool modified_{};
    bool closing_{};
    QString path_;

    QListView *messages_{};
    QPushButton *add_{};
    QPushButton *remove_{};
    QLineEdit *text_{};
    QComboBox *mode_{};
    QSpinBox *speed_{};
    QCheckBox *blink_{};
    QCheckBox *border_{};
    QComboBox *font_choice_{};
    QLineEdit *font_file_{};
    QLineEdit *vendor_directory_{};
    QPushButton *font_browse_{};
    QSpinBox *font_size_{};
    QSpinBox *threshold_{};
    QComboBox *brightness_{};
    PixelPreview *preview_{};
    QPushButton *previous_{};
    QPushButton *next_{};
    QSpinBox *frame_{};
    QLabel *frame_total_{};
    QLabel *font_info_{};
    QLabel *capacity_{};
    QLabel *status_{};
};
} // namespace ledscribe::gui
