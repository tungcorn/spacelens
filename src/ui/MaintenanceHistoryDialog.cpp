#include "ui/MaintenanceHistoryDialog.hpp"

#include "core/CleanupReviewStore.hpp"
#include "core/Maintenance.hpp"
#include "core/SizeFormatter.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTextEdit>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>

namespace spacelens {
namespace {

QString formatTicks(FileTimeTicks ticks)
{
    if (ticks == 0) {
        return QStringLiteral("(none)");
    }
    return QString::number(ticks);
}

QString operationSummary(const MaintenanceReceipt& receipt)
{
    return QStringLiteral("#%1  %2  recycled %3  blocked %4  failed %5  "
                          "cancelled %6  uncertain %7  %8")
        .arg(receipt.operationId)
        .arg(QString::fromUtf8(toString(receipt.status)))
        .arg(receipt.recycled)
        .arg(receipt.blocked)
        .arg(receipt.failed)
        .arg(receipt.cancelled)
        .arg(receipt.uncertain)
        .arg(QString::fromStdString(
            SizeFormatter::format(receipt.recycledLogicalBytes)));
}

QString itemDetails(const MaintenanceReceipt& receipt)
{
    QString text;
    text += QStringLiteral("Operation ID: %1\n").arg(receipt.operationId);
    text += QStringLiteral("Status: %1\n")
                .arg(QString::fromUtf8(toString(receipt.status)));
    text += QStringLiteral("Requested (FILETIME ticks): %1\n")
                .arg(formatTicks(receipt.requestedAt));
    text += QStringLiteral("Confirmed (FILETIME ticks): %1\n")
                .arg(formatTicks(receipt.confirmedAt));
    text += QStringLiteral("Completed (FILETIME ticks): %1\n")
                .arg(formatTicks(receipt.completedAt));
    text += QStringLiteral("Selected: %1    Eligible: %2\n")
                .arg(receipt.selectedCount)
                .arg(receipt.eligibleCount);
    text += QStringLiteral(
                "Recycled: %1    Blocked: %2    Failed: %3    Cancelled: %4    "
                "Uncertain: %5\n")
                .arg(receipt.recycled)
                .arg(receipt.blocked)
                .arg(receipt.failed)
                .arg(receipt.cancelled)
                .arg(receipt.uncertain);
    text += QStringLiteral("Recycled logical size: %1\n")
                .arg(QString::fromStdString(
                    SizeFormatter::format(receipt.recycledLogicalBytes)));
    if (receipt.unexpectedPermanentRemoval) {
        text += QStringLiteral(
            "Unexpected permanent removal: yes (not treated as success)\n");
    }
    text += QStringLiteral(
        "\nThis history is inspection only. SpaceLens cannot restore, "
        "permanently delete, retry, or empty the Recycle Bin.\n");
    text += QStringLiteral("\nItems:\n");
    for (const auto& item : receipt.items) {
        text += QStringLiteral("\n%1\n  result=%2  reason=%3\n  HRESULT=%4  "
                               "native=%5\n  recycle=%6\n  %7\n")
                    .arg(QString::fromStdWString(item.path))
                    .arg(QString::fromUtf8(toString(item.result)))
                    .arg(QString::fromUtf8(toString(item.blockReason)))
                    .arg(item.hresult)
                    .arg(item.nativeError)
                    .arg(item.recycleParsingName.empty()
                             ? QStringLiteral("(none)")
                             : QString::fromStdString(item.recycleParsingName))
                    .arg(QString::fromStdString(item.detail));
    }
    return text;
}

}  // namespace

MaintenanceHistoryDialog::MaintenanceHistoryDialog(
    CleanupReviewController& controller,
    QWidget* parent)
    : QDialog(parent)
    , m_controller(controller)
{
    setWindowTitle(QStringLiteral("Maintenance History"));
    resize(760, 480);
    auto* root = new QVBoxLayout(this);
    m_intro = new QLabel(
        QStringLiteral(
            "Recycle Bin operations recorded in this SpaceLens profile. "
            "History is evidence only — no Restore, Delete, Retry, or Empty."),
        this);
    m_intro->setWordWrap(true);
    root->addWidget(m_intro);
    m_list = new QListWidget(this);
    m_details = new QTextEdit(this);
    m_details->setReadOnly(true);
    root->addWidget(m_list, 1);
    root->addWidget(m_details, 2);
    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    auto* closeBtn = new QPushButton(QStringLiteral("Close"), this);
    buttons->addWidget(closeBtn);
    root->addLayout(buttons);
    connect(m_list, &QListWidget::itemSelectionChanged, this,
            &MaintenanceHistoryDialog::onSelectionChanged);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    refresh();
}

void MaintenanceHistoryDialog::selectOperation(quint64 operationId)
{
    for (int i = 0; i < m_list->count(); ++i) {
        if (m_list->item(i)->data(Qt::UserRole).toULongLong() == operationId) {
            m_list->setCurrentRow(i);
            return;
        }
    }
}

void MaintenanceHistoryDialog::onSelectionChanged()
{
    const auto receipts = m_controller.maintenanceReceipts();
    QListWidgetItem* row = m_list->currentItem();
    if (row == nullptr) {
        m_details->clear();
        return;
    }
    const auto id = row->data(Qt::UserRole).toULongLong();
    for (const auto& receipt : receipts) {
        if (receipt.operationId == id) {
            m_details->setPlainText(itemDetails(receipt));
            return;
        }
    }
    m_details->clear();
}

void MaintenanceHistoryDialog::refresh()
{
    m_list->clear();
    auto receipts = m_controller.maintenanceReceipts();
    std::reverse(receipts.begin(), receipts.end());
    for (const auto& receipt : receipts) {
        auto* row = new QListWidgetItem(operationSummary(receipt), m_list);
        row->setData(Qt::UserRole, QVariant::fromValue(receipt.operationId));
    }
    if (m_list->count() == 0) {
        m_details->setPlainText(
            QStringLiteral("No Recycle Bin operations have been recorded yet."));
    } else if (m_list->currentRow() < 0) {
        m_list->setCurrentRow(0);
    }
}

}  // namespace spacelens
