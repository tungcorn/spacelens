#include "ui/CleanupReviewDialog.hpp"

#include "core/SizeFormatter.hpp"
#include "platform/windows/ExplorerIntegration.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QApplication>
#include <QClipboard>

namespace spacelens {

CleanupReviewDialog::CleanupReviewDialog(CleanupReview& review, QWidget* parent)
    : QDialog(parent)
    , m_review(review)
{
    setWindowTitle(QStringLiteral("Cleanup Review"));
    resize(640, 420);

    auto* root = new QVBoxLayout(this);
    m_summary = new QLabel(this);
    root->addWidget(m_summary);

    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    root->addWidget(m_list, 1);

    auto* note = new QLabel(
        QStringLiteral("Planning only — no files will be deleted or moved."),
        this);
    note->setStyleSheet(QStringLiteral("color: #666;"));
    root->addWidget(note);

    auto* buttons = new QHBoxLayout();
    auto* openBtn = new QPushButton(QStringLiteral("Open"), this);
    auto* revealBtn = new QPushButton(QStringLiteral("Show in Explorer"), this);
    auto* removeBtn = new QPushButton(QStringLiteral("Remove from Review"), this);
    auto* clearBtn = new QPushButton(QStringLiteral("Clear Review"), this);
    auto* copyBtn = new QPushButton(QStringLiteral("Copy Report"), this);
    auto* closeBtn = new QPushButton(QStringLiteral("Close"), this);
    buttons->addWidget(openBtn);
    buttons->addWidget(revealBtn);
    buttons->addWidget(removeBtn);
    buttons->addWidget(clearBtn);
    buttons->addWidget(copyBtn);
    buttons->addStretch(1);
    buttons->addWidget(closeBtn);
    root->addLayout(buttons);

    connect(openBtn, &QPushButton::clicked, this, &CleanupReviewDialog::onOpen);
    connect(revealBtn, &QPushButton::clicked, this, &CleanupReviewDialog::onReveal);
    connect(removeBtn, &QPushButton::clicked, this,
            &CleanupReviewDialog::onRemoveSelected);
    connect(clearBtn, &QPushButton::clicked, this, &CleanupReviewDialog::onClear);
    connect(copyBtn, &QPushButton::clicked, this, &CleanupReviewDialog::onCopyReport);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);

    refresh();
}

void CleanupReviewDialog::refresh()
{
    m_list->clear();
    m_summary->setText(
        QStringLiteral("Selected: %1 items — Total logical size: %2")
            .arg(m_review.size())
            .arg(QString::fromStdString(
                SizeFormatter::format(m_review.totalLogicalSize()))));

    bool anyStaleSnapshot = false;
    for (const auto& item : m_review.items()) {
        QString sourceTag;
        if (item.source == "persistent_index") {
            anyStaleSnapshot = true;
            sourceTag = QStringLiteral(" · index snapshot age %1 ms")
                            .arg(item.indexAgeMs);
        }
        const QString text = QStringLiteral("[%1] %2  —  %3\n  %4 (%5)%6")
                                 .arg(QString::fromUtf8(toString(item.kind)))
                                 .arg(QString::fromStdWString(item.path))
                                 .arg(QString::fromStdString(
                                     SizeFormatter::format(item.sizeAtSelection)))
                                 .arg(QString::fromUtf8(
                                     toString(item.classification.category)))
                                 .arg(QString::fromUtf8(
                                     toString(item.classification.confidence)))
                                 .arg(sourceTag);
        auto* row = new QListWidgetItem(text, m_list);
        row->setData(Qt::UserRole, QVariant::fromValue(item.id));
        row->setData(Qt::UserRole + 1, QString::fromStdWString(item.path));
    }
    if (anyStaleSnapshot) {
        m_summary->setText(
            m_summary->text() +
            QStringLiteral(
                "\nSome items came from a persistent index snapshot and may be "
                "stale relative to the live filesystem."));
    }
}

void CleanupReviewDialog::onRemoveSelected()
{
    const auto selected = m_list->selectedItems();
    for (QListWidgetItem* row : selected) {
        const auto id = row->data(Qt::UserRole).toULongLong();
        (void)m_review.removeById(id);
    }
    refresh();
}

void CleanupReviewDialog::onClear()
{
    m_review.clear();
    refresh();
}

void CleanupReviewDialog::onCopyReport()
{
    QApplication::clipboard()->setText(
        QString::fromStdString(m_review.copyReport()));
}

void CleanupReviewDialog::onOpen()
{
    const auto selected = m_list->selectedItems();
    if (selected.size() != 1) {
        return;
    }
    const auto path = selected.front()->data(Qt::UserRole + 1).toString().toStdWString();
    if (!openWithDefaultApp(path) && !openInExplorer(path)) {
        QMessageBox::warning(this, QStringLiteral("SpaceLens"),
                             QStringLiteral("Could not open path."));
    }
}

void CleanupReviewDialog::onReveal()
{
    const auto selected = m_list->selectedItems();
    if (selected.size() != 1) {
        return;
    }
    const auto path = selected.front()->data(Qt::UserRole + 1).toString().toStdWString();
    if (!revealInExplorer(path) && !openInExplorer(path)) {
        QMessageBox::warning(this, QStringLiteral("SpaceLens"),
                             QStringLiteral("Could not reveal path."));
    }
}

}  // namespace spacelens
