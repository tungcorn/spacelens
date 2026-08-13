#pragma once

#include "app/DuplicateDetectionSession.hpp"
#include "core/CleanupReviewStore.hpp"
#include "core/Duplicates.hpp"

#include <QDialog>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QTextEdit;

namespace spacelens {

class DuplicateFilesDialog final : public QDialog {
    Q_OBJECT

public:
    DuplicateFilesDialog(CleanupReviewController& review,
                         std::wstring rootPath,
                         std::uint64_t indexAgeMs,
                         std::string indexedAtIso,
                         QWidget* parent = nullptr);

private slots:
    void onFind();
    void onCancel();
    void onSelectionChanged();
    void onReveal();
    void onCopyPaths();
    void onCopyGroup();
    void onAddToReview();
    void onProgress(quint64 filesDone, quint64 filesTotal, quint64 bytesDone,
                    quint64 bytesTotal);
    void onFinished(bool completed, const QString& message);

private:
    void refresh();
    void updateActionState();
    void showStatus(const QString& message);
    [[nodiscard]] std::optional<std::size_t> selectedGroupIndex() const;
    [[nodiscard]] std::vector<std::wstring> selectedPaths() const;

    CleanupReviewController& m_review;
    std::wstring m_rootPath;
    std::uint64_t m_indexAgeMs = 0;
    std::string m_indexedAtIso;
    DuplicateDetectionSession m_session;
    DuplicateDetectionResult m_result;

    QLabel* m_summary = nullptr;
    QLineEdit* m_minSize = nullptr;
    QListWidget* m_list = nullptr;
    QTextEdit* m_details = nullptr;
    QProgressBar* m_progress = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_findButton = nullptr;
    QPushButton* m_cancelButton = nullptr;
    QPushButton* m_revealButton = nullptr;
    QPushButton* m_copyPathsButton = nullptr;
    QPushButton* m_copyGroupButton = nullptr;
    QPushButton* m_addReviewButton = nullptr;
};

}  // namespace spacelens
