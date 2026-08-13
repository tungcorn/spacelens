#include "ui/OrdinaryLocationsDialog.hpp"

#include "platform/windows/CleanupMetadataReader.hpp"
#include "platform/windows/LocationVolume.hpp"

#include <QAbstractItemView>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace spacelens {
namespace {

FileTimeTicks nowFileTimeTicks()
{
    FILETIME ft{};
    ::GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

QString statusLabel(OrdinaryLocationStatus status)
{
    switch (status) {
    case OrdinaryLocationStatus::Active:
        return QStringLiteral("Active · Volume matched");
    case OrdinaryLocationStatus::VolumeMismatch:
        return QStringLiteral("Volume changed · Reconfirmation required");
    case OrdinaryLocationStatus::VolumeUnavailable:
        return QStringLiteral("Volume identity unavailable");
    case OrdinaryLocationStatus::PathUnavailable:
        return QStringLiteral("Path unavailable");
    case OrdinaryLocationStatus::Invalid:
        return QStringLiteral("Invalid");
    }
    return QStringLiteral("Invalid");
}

}  // namespace

OrdinaryLocationsDialog::OrdinaryLocationsDialog(
    CleanupReviewController& controller, QWidget* parent)
    : QDialog(parent)
    , m_controller(controller)
{
    setWindowTitle(QStringLiteral("User-declared ordinary locations"));
    resize(640, 420);

    auto* layout = new QVBoxLayout(this);
    m_intro = new QLabel(
        QStringLiteral(
            "These locations are treated as ordinary user-managed storage.\n"
            "This does not mark their files as safe to remove.\n"
            "Built-in protected and sensitive rules always take precedence."),
        this);
    m_intro->setWordWrap(true);
    layout->addWidget(m_intro);

    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(m_list, 1);

    auto* buttons = new QHBoxLayout();
    m_addButton = new QPushButton(QStringLiteral("Add Location…"), this);
    m_removeButton = new QPushButton(QStringLiteral("Remove"), this);
    buttons->addWidget(m_addButton);
    buttons->addWidget(m_removeButton);
    buttons->addStretch(1);
    auto* closeButton = new QPushButton(QStringLiteral("Close"), this);
    buttons->addWidget(closeButton);
    layout->addLayout(buttons);

    connect(m_addButton, &QPushButton::clicked, this,
            &OrdinaryLocationsDialog::onAdd);
    connect(m_removeButton, &QPushButton::clicked, this,
            &OrdinaryLocationsDialog::onRemove);
    connect(m_list, &QListWidget::itemSelectionChanged, this,
            &OrdinaryLocationsDialog::onSelectionChanged);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);

    refresh();
}

void OrdinaryLocationsDialog::refresh()
{
    WindowsCleanupMetadataReader probe;
    WindowsVolumeIdentityReader volumes;
    const auto policy = m_controller.refreshOrdinaryLocations(probe, volumes);
    m_list->clear();
    for (const auto& declaration : policy.declarations) {
        auto* item = new QListWidgetItem(
            QStringLiteral("%1\n  %2")
                .arg(QString::fromStdWString(declaration.configuredPath),
                     statusLabel(declaration.status)),
            m_list);
        item->setData(Qt::UserRole, QVariant::fromValue(declaration.id));
    }
    onSelectionChanged();
}

void OrdinaryLocationsDialog::onSelectionChanged()
{
    m_removeButton->setEnabled(m_list->currentItem() != nullptr &&
                               !m_controller.reviewMutationsBlocked());
    m_addButton->setEnabled(!m_controller.reviewMutationsBlocked());
}

void OrdinaryLocationsDialog::onAdd()
{
    if (m_controller.reviewMutationsBlocked()) {
        QMessageBox::information(
            this, QStringLiteral("Safety"),
            QStringLiteral(
                "Location declarations cannot change while review or "
                "maintenance is running."));
        return;
    }
    const QString chosen = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Choose an ordinary user-managed location"));
    if (chosen.isEmpty()) {
        return;
    }

    const QString native = QDir::toNativeSeparators(chosen);
    const QString confirmText = QStringLiteral(
        "Mark this location as ordinary user-managed storage?\n\n"
        "%1\n\n"
        "This does not mark files as safe to remove.\n"
        "Protected and sensitive rules still apply.")
                                    .arg(native);
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("Confirm ordinary location"));
    box.setText(confirmText);
    box.setIcon(QMessageBox::Question);
    auto* accept = box.addButton(QStringLiteral("Declare Location"),
                                 QMessageBox::AcceptRole);
    box.addButton(QStringLiteral("Cancel"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() != accept) {
        return;
    }

    WindowsCleanupMetadataReader probe;
    WindowsVolumeIdentityReader volumes;
    const auto outcome = m_controller.addOrdinaryLocation(
        native.toStdWString(), probe, volumes, nowFileTimeTicks());
    if (outcome.result == OrdinaryLocationAddResult::AlreadyExists) {
        QMessageBox::information(
            this, QStringLiteral("Safety"),
            QStringLiteral("This ordinary location is already declared."));
        refresh();
        return;
    }
    if (outcome.result != OrdinaryLocationAddResult::Added &&
        outcome.result != OrdinaryLocationAddResult::VolumeUnavailable) {
        QMessageBox::warning(
            this, QStringLiteral("Could not declare location"),
            QString::fromStdString(outcome.message.empty()
                                       ? toString(outcome.result)
                                       : outcome.message));
        return;
    }
    if (outcome.result == OrdinaryLocationAddResult::VolumeUnavailable) {
        QMessageBox::information(
            this, QStringLiteral("Location declared"),
            QStringLiteral(
                "The location was saved, but volume identity is unavailable. "
                "It will not authorize maintenance until the volume can be "
                "confirmed."));
    }
    refresh();
}

void OrdinaryLocationsDialog::onRemove()
{
    if (m_controller.reviewMutationsBlocked()) {
        return;
    }
    auto* item = m_list->currentItem();
    if (item == nullptr) {
        return;
    }
    const auto id = item->data(Qt::UserRole).toULongLong();
    const auto confirm = QMessageBox::question(
        this, QStringLiteral("Remove ordinary location"),
        QStringLiteral(
            "Remove this user-declared ordinary location?\n\n"
            "Matching paths return to Unknown unless another rule covers them. "
            "Cleanup Review items and index data are not changed."));
    if (confirm != QMessageBox::Yes) {
        return;
    }
    const auto status = m_controller.removeOrdinaryLocation(id);
    if (!status.ok) {
        QMessageBox::warning(this, QStringLiteral("Could not remove location"),
                             QString::fromStdString(status.message));
        return;
    }
    refresh();
}

}  // namespace spacelens
