#pragma once

#include <QMainWindow>
#include <QString>

class QLineEdit;
class QPushButton;
class QLabel;
class QTreeWidget;
class QListWidget;
class QSplitter;

namespace spacelens {

/// Phase 1 shell: folder selection and layout for scan results.
/// Real scanning is wired in a later milestone.
class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override = default;

private slots:
    void onSelectFolder();
    void onScan();
    void onCancel();

private:
    void buildUi();
    void updateActionState();
    void setStatusMessage(const QString& message);

    QString m_rootPath;

    QLineEdit* m_pathEdit = nullptr;
    QPushButton* m_selectButton = nullptr;
    QPushButton* m_scanButton = nullptr;
    QPushButton* m_cancelButton = nullptr;

    QLabel* m_filesLabel = nullptr;
    QLabel* m_foldersLabel = nullptr;
    QLabel* m_processedLabel = nullptr;
    QLabel* m_elapsedLabel = nullptr;
    QLabel* m_errorsLabel = nullptr;
    QLabel* m_statusLabel = nullptr;

    QTreeWidget* m_folderTree = nullptr;
    QListWidget* m_largestList = nullptr;
};

}  // namespace spacelens
