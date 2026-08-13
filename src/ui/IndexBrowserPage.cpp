#include "ui/IndexBrowserPage.hpp"

#include "core/Classification.hpp"
#include "core/CleanupRevalidation.hpp"
#include "core/CleanupReview.hpp"
#include "core/FileTime.hpp"
#include "core/SizeFormatter.hpp"
#include "core/SizeParse.hpp"
#include "core/index/IndexOverview.hpp"
#include "platform/windows/CleanupMetadataReader.hpp"
#include "platform/windows/ExplorerIntegration.hpp"
#include "ui/DuplicateFilesDialog.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QMetaObject>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTableView>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <chrono>

namespace spacelens {
namespace {

QString fromWide(const std::wstring& s)
{
    return QString::fromStdWString(s);
}

std::wstring toWide(const QString& s)
{
    return s.toStdWString();
}

QString formatAge(std::uint64_t ageMs)
{
    if (ageMs < 1000) {
        return QStringLiteral("%1 ms").arg(ageMs);
    }
    const double sec = static_cast<double>(ageMs) / 1000.0;
    if (sec < 120.0) {
        return QStringLiteral("%1 s").arg(sec, 0, 'f', 1);
    }
    const double min = sec / 60.0;
    if (min < 120.0) {
        return QStringLiteral("%1 min").arg(min, 0, 'f', 1);
    }
    const double hours = min / 60.0;
    if (hours < 48.0) {
        return QStringLiteral("%1 h").arg(hours, 0, 'f', 1);
    }
    return QStringLiteral("%1 d").arg(hours / 24.0, 0, 'f', 1);
}

QString formatActivity(std::uint64_t lastWriteTicks)
{
    if (lastWriteTicks == 0) {
        return QStringLiteral("Unknown");
    }
    FILETIME nowFt{};
    ::GetSystemTimeAsFileTime(&nowFt);
    ULARGE_INTEGER now{};
    now.LowPart = nowFt.dwLowDateTime;
    now.HighPart = nowFt.dwHighDateTime;
    if (lastWriteTicks >= now.QuadPart) {
        return QStringLiteral("Recent");
    }
    const std::uint64_t delta = now.QuadPart - lastWriteTicks;
    const std::uint64_t days = delta / daysToTicks(1);
    if (days == 0) {
        return QStringLiteral("< 1 day");
    }
    if (days < 30) {
        return QStringLiteral("%1 d inactive").arg(days);
    }
    if (days < 365) {
        return QStringLiteral("%1 mo inactive").arg(days / 30);
    }
    return QStringLiteral("%1 y inactive").arg(days / 365);
}

Confidence parseConfidence(std::string_view text)
{
    if (text == "High" || text == "high") {
        return Confidence::High;
    }
    if (text == "Medium" || text == "medium") {
        return Confidence::Medium;
    }
    return Confidence::Low;
}

Reclaimability parseReclaimabilityLabel(std::string_view text)
{
    if (text == "LikelyRegenerable") {
        return Reclaimability::LikelyRegenerable;
    }
    if (text == "PossiblyRegenerable") {
        return Reclaimability::PossiblyRegenerable;
    }
    if (text == "NotApplicable") {
        return Reclaimability::NotApplicable;
    }
    return Reclaimability::Unknown;
}

CandidateStrength parseCandidateStrengthLabel(std::string_view text)
{
    if (text == "Strong") {
        return CandidateStrength::Strong;
    }
    if (text == "Moderate") {
        return CandidateStrength::Moderate;
    }
    if (text == "ReviewOnly") {
        return CandidateStrength::ReviewOnly;
    }
    return CandidateStrength::None;
}

QString freshnessIndicator(IndexFreshness f)
{
    switch (f) {
    case IndexFreshness::Fresh:
    case IndexFreshness::RefreshAvailable:
        return QStringLiteral("●");
    case IndexFreshness::AgedSnapshot:
        return QStringLiteral("○");
    case IndexFreshness::FullRebuildRequired:
    case IndexFreshness::Error:
        return QStringLiteral("⚠");
    case IndexFreshness::IncrementalUnavailable:
        return QStringLiteral("○");
    case IndexFreshness::Missing:
    default:
        return QStringLiteral("·");
    }
}

std::optional<ByteSize> minSizeFromUi(const QComboBox* combo,
                                      const QLineEdit* custom)
{
    if (!combo) {
        return std::nullopt;
    }
    const QString data = combo->currentData().toString();
    if (data.isEmpty() || data == QLatin1String("any")) {
        return std::nullopt;
    }
    if (data == QLatin1String("custom")) {
        const QString text = custom ? custom->text().trimmed() : QString();
        if (text.isEmpty()) {
            return std::nullopt;
        }
        const auto parsed = parseSize(text.toStdString());
        if (!parsed.error.empty()) {
            return std::nullopt;
        }
        return parsed.bytes;
    }
    bool ok = false;
    const qulonglong v = data.toULongLong(&ok);
    if (!ok) {
        return std::nullopt;
    }
    return static_cast<ByteSize>(v);
}

}  // namespace

// ---------------------------------------------------------------------------
// IndexHitTableModel
// ---------------------------------------------------------------------------

IndexHitTableModel::IndexHitTableModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

void IndexHitTableModel::setHits(std::vector<IndexHit> hits,
                                 std::uint64_t indexAgeMs,
                                 std::string indexedAtIso)
{
    beginResetModel();
    m_hits = std::move(hits);
    m_indexAgeMs = indexAgeMs;
    m_indexedAtIso = std::move(indexedAtIso);
    endResetModel();
}

void IndexHitTableModel::clear()
{
    beginResetModel();
    m_hits.clear();
    m_indexAgeMs = 0;
    m_indexedAtIso.clear();
    endResetModel();
}

int IndexHitTableModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_hits.size());
}

int IndexHitTableModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColCount;
}

QVariant IndexHitTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 ||
        index.row() >= static_cast<int>(m_hits.size())) {
        return {};
    }
    const auto& h = m_hits[static_cast<std::size_t>(index.row())];
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case ColName:
            return h.name.empty() ? QFileInfo(fromWide(h.path)).fileName()
                                  : fromWide(h.name);
        case ColSize:
            return QString::fromStdString(SizeFormatter::format(h.size_bytes));
        case ColType:
            return h.kind == IndexEntryKind::Directory ? QStringLiteral("Folder")
                                                       : QStringLiteral("File");
        case ColActivity:
            return formatActivity(h.last_write_ticks);
        case ColClassification:
            return QString::fromStdString(h.classification);
        case ColReclaim:
            return QString::fromStdString(h.candidate_strength);
        case ColPath:
            return fromWide(h.path);
        default:
            break;
        }
    }
    if (role == Qt::ToolTipRole) {
        return fromWide(h.path);
    }
    if (role == Qt::TextAlignmentRole && index.column() == ColSize) {
        return QVariant::fromValue(int(Qt::AlignRight | Qt::AlignVCenter));
    }
    return {};
}

QVariant IndexHitTableModel::headerData(int section, Qt::Orientation orientation,
                                        int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    switch (section) {
    case ColName:
        return QStringLiteral("Name");
    case ColSize:
        return QStringLiteral("Size");
    case ColType:
        return QStringLiteral("Type");
    case ColActivity:
        return QStringLiteral("Activity");
    case ColClassification:
        return QStringLiteral("Classification");
    case ColReclaim:
        return QStringLiteral("Reclaim");
    case ColPath:
        return QStringLiteral("Path");
    default:
        return {};
    }
}

const IndexHit* IndexHitTableModel::hitAt(int row) const
{
    if (row < 0 || row >= static_cast<int>(m_hits.size())) {
        return nullptr;
    }
    return &m_hits[static_cast<std::size_t>(row)];
}

int IndexHitTableModel::findRowByPath(const std::wstring& path) const
{
    for (std::size_t i = 0; i < m_hits.size(); ++i) {
        if (m_hits[i].path == path) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// IndexBrowserPage
// ---------------------------------------------------------------------------

IndexBrowserPage::IndexBrowserPage(CleanupReviewController& review, QWidget* parent)
    : QWidget(parent)
    , m_review(review)
    , m_session(new IndexSession(this))
    , m_hitModel(new IndexHitTableModel(this))
{
    buildUi();
    connect(m_session, &IndexSession::statusMessage, this,
            &IndexBrowserPage::onSessionStatus);
    connect(m_session, &IndexSession::buildFinished, this,
            &IndexBrowserPage::onBuildFinished);
    connect(m_session, &IndexSession::refreshFinished, this,
            &IndexBrowserPage::onRefreshFinished);
    reloadRoots();
}

IndexBrowserPage::~IndexBrowserPage()
{
    m_queryGeneration.fetch_add(1);
    if (m_queryWorker.joinable()) {
        m_queryWorker.request_stop();
        m_queryWorker.join();
    }
    if (m_session) {
        m_session->cancel();
    }
}

void IndexBrowserPage::buildUi()
{
    auto* rootLayout = new QVBoxLayout(this);

    auto* toolbar = new QHBoxLayout();
    m_reloadButton = new QPushButton(QStringLiteral("Reload List"), this);
    m_indexNewButton = new QPushButton(QStringLiteral("Index Folder…"), this);
    m_refreshButton = new QPushButton(QStringLiteral("Refresh Index"), this);
    m_rebuildButton = new QPushButton(QStringLiteral("Rebuild"), this);
    m_cancelButton = new QPushButton(QStringLiteral("Cancel"), this);
    toolbar->addWidget(m_reloadButton);
    toolbar->addWidget(m_indexNewButton);
    toolbar->addWidget(m_refreshButton);
    toolbar->addWidget(m_rebuildButton);
    toolbar->addWidget(m_cancelButton);
    toolbar->addStretch(1);
    rootLayout->addLayout(toolbar);

    auto* note = new QLabel(
        QStringLiteral(
            "Indexed discovery is a snapshot — not live filesystem truth. "
            "Refresh/Rebuild are explicit. Nothing here deletes or moves analyzed data."),
        this);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: #666;"));
    rootLayout->addWidget(note);

    // Discovery presets
    auto* presetRow = new QHBoxLayout();
    presetRow->addWidget(new QLabel(QStringLiteral("Discover:"), this));
    m_presetGroup = new QButtonGroup(this);
    m_presetGroup->setExclusive(true);
    const struct {
        const char* label;
        IndexDiscoveryPreset preset;
    } presets[] = {
        {"Largest", IndexDiscoveryPreset::Largest},
        {"Old & Large", IndexDiscoveryPreset::OldAndLarge},
        {"Developer Storage", IndexDiscoveryPreset::DeveloperStorage},
        {"Reclaim Candidates", IndexDiscoveryPreset::ReclaimCandidates},
        {"Custom", IndexDiscoveryPreset::Custom},
    };
    for (const auto& p : presets) {
        auto* btn = new QToolButton(this);
        btn->setText(QString::fromUtf8(p.label));
        btn->setCheckable(true);
        btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        m_presetGroup->addButton(btn, static_cast<int>(p.preset));
        presetRow->addWidget(btn);
    }
    if (auto* first = m_presetGroup->button(
            static_cast<int>(IndexDiscoveryPreset::Largest))) {
        first->setChecked(true);
    }
    presetRow->addStretch(1);
    rootLayout->addLayout(presetRow);
    connect(m_presetGroup, &QButtonGroup::idClicked, this,
            &IndexBrowserPage::onPresetClicked);

    // Search + filters
    auto* searchRow = new QHBoxLayout();
    searchRow->addWidget(new QLabel(QStringLiteral("Search:"), this));
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(
        QStringLiteral("name / path / extension (case-insensitive substring)"));
    m_searchEdit->setClearButtonEnabled(true);
    searchRow->addWidget(m_searchEdit, 1);
    rootLayout->addLayout(searchRow);

    auto* filterRow = new QHBoxLayout();
    filterRow->addWidget(new QLabel(QStringLiteral("Type:"), this));
    m_kindFilter = new QComboBox(this);
    m_kindFilter->addItems({QStringLiteral("All"), QStringLiteral("Files"),
                            QStringLiteral("Folders")});
    filterRow->addWidget(m_kindFilter);

    filterRow->addWidget(new QLabel(QStringLiteral("Min size:"), this));
    m_minSizeFilter = new QComboBox(this);
    m_minSizeFilter->addItem(QStringLiteral("Any"), QStringLiteral("any"));
    m_minSizeFilter->addItem(QStringLiteral("100 MB"),
                             QString::number(100ULL * 1024 * 1024));
    m_minSizeFilter->addItem(QStringLiteral("500 MB"),
                             QString::number(500ULL * 1024 * 1024));
    m_minSizeFilter->addItem(QStringLiteral("1 GB"),
                             QString::number(1024ULL * 1024 * 1024));
    m_minSizeFilter->addItem(QStringLiteral("5 GB"),
                             QString::number(5ULL * 1024 * 1024 * 1024));
    m_minSizeFilter->addItem(QStringLiteral("Custom…"),
                             QStringLiteral("custom"));
    filterRow->addWidget(m_minSizeFilter);
    m_minSizeCustom = new QLineEdit(this);
    m_minSizeCustom->setPlaceholderText(QStringLiteral("e.g. 250MB"));
    m_minSizeCustom->setMaximumWidth(90);
    m_minSizeCustom->setEnabled(false);
    filterRow->addWidget(m_minSizeCustom);
    connect(m_minSizeFilter, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
                m_minSizeCustom->setEnabled(
                    m_minSizeFilter->currentData().toString() ==
                    QLatin1String("custom"));
            });

    filterRow->addWidget(new QLabel(QStringLiteral("Activity:"), this));
    m_activityFilter = new QComboBox(this);
    m_activityFilter->addItem(QStringLiteral("Any"), 0);
    m_activityFilter->addItem(QStringLiteral("> 30 days"), 30);
    m_activityFilter->addItem(QStringLiteral("> 90 days"), 90);
    m_activityFilter->addItem(QStringLiteral("> 180 days"), 180);
    m_activityFilter->addItem(QStringLiteral("> 1 year"), 365);
    filterRow->addWidget(m_activityFilter);

    filterRow->addWidget(new QLabel(QStringLiteral("Ext:"), this));
    m_extFilter = new QLineEdit(this);
    m_extFilter->setPlaceholderText(QStringLiteral("gguf"));
    m_extFilter->setMaximumWidth(70);
    filterRow->addWidget(m_extFilter);

    filterRow->addWidget(new QLabel(QStringLiteral("Class:"), this));
    m_classFilter = new QComboBox(this);
    m_classFilter->addItem(QStringLiteral("All"), QString());
    for (const char* name :
         {"BuildArtifact", "DependencyDirectory", "PackageCache", "IdeCache",
          "LogData", "TemporaryData", "DownloadedAiModel", "Archive",
          "ApplicationData", "SystemData", "UserData", "Unknown"}) {
        m_classFilter->addItem(QString::fromUtf8(name), QString::fromUtf8(name));
    }
    filterRow->addWidget(m_classFilter);

    filterRow->addWidget(new QLabel(QStringLiteral("Reclaim:"), this));
    m_strengthFilter = new QComboBox(this);
    m_strengthFilter->addItem(QStringLiteral("All"), QString());
    for (const char* name : {"Strong", "Moderate", "ReviewOnly", "None"}) {
        m_strengthFilter->addItem(QString::fromUtf8(name),
                                  QString::fromUtf8(name));
    }
    filterRow->addWidget(m_strengthFilter);

    filterRow->addWidget(new QLabel(QStringLiteral("Sort:"), this));
    m_sortFilter = new QComboBox(this);
    m_sortFilter->addItem(QStringLiteral("Size"),
                          static_cast<int>(IndexSortKey::Size));
    m_sortFilter->addItem(QStringLiteral("Name"),
                          static_cast<int>(IndexSortKey::Name));
    m_sortFilter->addItem(QStringLiteral("Activity"),
                          static_cast<int>(IndexSortKey::LastWrite));
    m_sortFilter->addItem(QStringLiteral("Classification"),
                          static_cast<int>(IndexSortKey::Classification));
    m_sortFilter->addItem(QStringLiteral("Reclaim strength"),
                          static_cast<int>(IndexSortKey::CandidateStrength));
    filterRow->addWidget(m_sortFilter);
    connect(m_sortFilter, QOverload<int>::of(&QComboBox::activated), this,
            [this](int) { m_sortUserOverride = true; });

    filterRow->addWidget(new QLabel(QStringLiteral("Limit:"), this));
    m_limitSpin = new QSpinBox(this);
    m_limitSpin->setRange(1, 2000);
    m_limitSpin->setValue(200);
    filterRow->addWidget(m_limitSpin);

    m_queryButton = new QPushButton(QStringLiteral("Search Index"), this);
    filterRow->addWidget(m_queryButton);
    rootLayout->addLayout(filterRow);

    m_rootMeta = new QLabel(QStringLiteral("No index selected."), this);
    m_rootMeta->setWordWrap(true);
    m_rootMeta->setTextFormat(Qt::RichText);
    rootLayout->addWidget(m_rootMeta);

    m_breadcrumbBar = new QWidget(this);
    m_breadcrumbLayout = new QHBoxLayout(m_breadcrumbBar);
    m_breadcrumbLayout->setContentsMargins(0, 0, 0, 0);
    m_breadcrumbLayout->setSpacing(2);
    rootLayout->addWidget(m_breadcrumbBar);

    m_overviewLabel = new QLabel(QStringLiteral(""), this);
    m_overviewLabel->setWordWrap(true);
    m_overviewLabel->setTextFormat(Qt::RichText);
    m_overviewLabel->setStyleSheet(
        QStringLiteral("QLabel { background: #F0F2F5; padding: 8px 10px; "
                       "border-radius: 4px; }"));
    rootLayout->addWidget(m_overviewLabel);

    m_queryMeta = new QLabel(QStringLiteral(""), this);
    rootLayout->addWidget(m_queryMeta);
    m_selectionMeta = new QLabel(QStringLiteral(""), this);
    rootLayout->addWidget(m_selectionMeta);

    auto* splitter = new QSplitter(Qt::Horizontal, this);

    m_rootsList = new QListWidget(splitter);
    m_rootsList->setAlternatingRowColors(true);
    m_rootsList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_rootsList->setMinimumWidth(220);

    auto* rightSplit = new QSplitter(Qt::Vertical, splitter);

    m_treemap = new TreemapWidget(rightSplit);
    m_treemap->setMinimumHeight(160);

    auto* hitsHost = new QWidget(rightSplit);
    auto* hitsLayout = new QVBoxLayout(hitsHost);
    hitsLayout->setContentsMargins(0, 0, 0, 0);
    m_emptyLabel = new QLabel(
        QStringLiteral("Select an indexed root, choose a discovery mode, then "
                       "Search Index."),
        hitsHost);
    m_emptyLabel->setWordWrap(true);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setStyleSheet(QStringLiteral("color: #666; padding: 24px;"));
    hitsLayout->addWidget(m_emptyLabel);
    m_hitsView = new QTableView(hitsHost);
    m_hitsView->setModel(m_hitModel);
    m_hitsView->setAlternatingRowColors(true);
    m_hitsView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_hitsView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_hitsView->setSortingEnabled(false);
    m_hitsView->setShowGrid(false);
    m_hitsView->verticalHeader()->setVisible(false);
    m_hitsView->horizontalHeader()->setStretchLastSection(true);
    m_hitsView->horizontalHeader()->setDefaultSectionSize(110);
    m_hitsView->setColumnWidth(IndexHitTableModel::ColName, 160);
    m_hitsView->setColumnWidth(IndexHitTableModel::ColPath, 280);
    m_hitsView->setContextMenuPolicy(Qt::CustomContextMenu);
    m_hitsView->setVisible(false);
    hitsLayout->addWidget(m_hitsView, 1);

    m_inspector = new QTextEdit(rightSplit);
    m_inspector->setReadOnly(true);
    rightSplit->addWidget(m_treemap);
    rightSplit->addWidget(hitsHost);
    rightSplit->addWidget(m_inspector);
    rightSplit->setStretchFactor(0, 2);
    rightSplit->setStretchFactor(1, 3);
    rightSplit->setStretchFactor(2, 2);

    splitter->addWidget(m_rootsList);
    splitter->addWidget(rightSplit);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    rootLayout->addWidget(splitter, 1);

    auto* actionRow = new QHBoxLayout();
    m_openButton = new QPushButton(QStringLiteral("Open"), this);
    m_revealButton = new QPushButton(QStringLiteral("Show in Explorer"), this);
    m_copyPathButton = new QPushButton(QStringLiteral("Copy Path"), this);
    m_addReviewButton =
        new QPushButton(QStringLiteral("Add to Cleanup Review"), this);
    m_showReviewButton = new QPushButton(QStringLiteral("Cleanup Review"), this);
    m_findDuplicatesButton = new QPushButton(QStringLiteral("Find Duplicates"), this);
    actionRow->addWidget(m_openButton);
    actionRow->addWidget(m_revealButton);
    actionRow->addWidget(m_copyPathButton);
    actionRow->addStretch(1);
    actionRow->addWidget(m_findDuplicatesButton);
    actionRow->addWidget(m_addReviewButton);
    actionRow->addWidget(m_showReviewButton);
    rootLayout->addLayout(actionRow);

    connect(m_reloadButton, &QPushButton::clicked, this,
            &IndexBrowserPage::reloadRoots);
    connect(m_indexNewButton, &QPushButton::clicked, this,
            &IndexBrowserPage::onIndexNewRoot);
    connect(m_refreshButton, &QPushButton::clicked, this,
            &IndexBrowserPage::onRefreshIndex);
    connect(m_rebuildButton, &QPushButton::clicked, this,
            &IndexBrowserPage::onRebuildIndex);
    connect(m_cancelButton, &QPushButton::clicked, this,
            &IndexBrowserPage::onCancelJob);
    connect(m_queryButton, &QPushButton::clicked, this,
            &IndexBrowserPage::onQuery);
    connect(m_addReviewButton, &QPushButton::clicked, this,
            &IndexBrowserPage::onAddToReview);
    connect(m_showReviewButton, &QPushButton::clicked, this,
            &IndexBrowserPage::onShowReview);
    connect(m_findDuplicatesButton, &QPushButton::clicked, this,
            &IndexBrowserPage::onFindDuplicates);
    connect(m_openButton, &QPushButton::clicked, this,
            &IndexBrowserPage::onOpenSelected);
    connect(m_revealButton, &QPushButton::clicked, this,
            &IndexBrowserPage::onRevealSelected);
    connect(m_copyPathButton, &QPushButton::clicked, this,
            &IndexBrowserPage::onCopyPath);
    connect(m_rootsList, &QListWidget::itemSelectionChanged, this,
            &IndexBrowserPage::onRootSelectionChanged);
    connect(m_hitsView->selectionModel(),
            &QItemSelectionModel::selectionChanged, this,
            &IndexBrowserPage::onHitSelectionChanged);
    connect(m_hitsView, &QTableView::doubleClicked, this,
            &IndexBrowserPage::onHitDoubleClicked);
    connect(m_hitsView, &QTableView::customContextMenuRequested, this,
            &IndexBrowserPage::onHitsContextMenu);
    connect(m_searchEdit, &QLineEdit::returnPressed, this,
            &IndexBrowserPage::onQuery);
    connect(m_treemap, &TreemapWidget::itemClicked, this,
            &IndexBrowserPage::onTreemapItemClicked);
    connect(m_treemap, &TreemapWidget::itemDoubleClicked, this,
            &IndexBrowserPage::onTreemapItemDoubleClicked);

    auto* findSc = new QShortcut(QKeySequence::Find, this);
    connect(findSc, &QShortcut::activated, this, [this]() { focusSearch(); });
    auto* copySc = new QShortcut(QKeySequence::Copy, m_hitsView);
    connect(copySc, &QShortcut::activated, this,
            &IndexBrowserPage::onCopyPath);
    auto* refreshSc = new QShortcut(QKeySequence::Refresh, this);
    connect(refreshSc, &QShortcut::activated, this, [this]() {
        if (m_activeRoot && m_activeRoot->exists) {
            onQuery();
        }
    });

    applyPresetDefaults(IndexDiscoveryPreset::Largest);
    updateActionState();
}

void IndexBrowserPage::focusSearch()
{
    m_searchEdit->setFocus(Qt::ShortcutFocusReason);
    m_searchEdit->selectAll();
}

void IndexBrowserPage::applyPresetDefaults(IndexDiscoveryPreset preset)
{
    m_preset = preset;
    m_sortUserOverride = false;
    QSignalBlocker b1(m_kindFilter);
    QSignalBlocker b2(m_minSizeFilter);
    QSignalBlocker b3(m_activityFilter);
    QSignalBlocker b4(m_classFilter);
    QSignalBlocker b5(m_strengthFilter);
    QSignalBlocker b6(m_sortFilter);

    m_kindFilter->setCurrentIndex(0);  // All
    m_minSizeFilter->setCurrentIndex(0);
    m_activityFilter->setCurrentIndex(0);
    m_classFilter->setCurrentIndex(0);
    m_strengthFilter->setCurrentIndex(0);
    m_sortFilter->setCurrentIndex(0);  // Size

    switch (preset) {
    case IndexDiscoveryPreset::OldAndLarge:
        m_minSizeFilter->setCurrentIndex(1);  // 100 MB
        m_activityFilter->setCurrentIndex(2);  // > 90 days
        break;
    case IndexDiscoveryPreset::DeveloperStorage:
        // Multi-class handled in buildQuerySpec via preset; clear single class.
        break;
    case IndexDiscoveryPreset::ReclaimCandidates:
        m_sortFilter->setCurrentIndex(4);  // Reclaim strength
        break;
    case IndexDiscoveryPreset::Largest:
    case IndexDiscoveryPreset::Custom:
    default:
        break;
    }
}

void IndexBrowserPage::onPresetClicked(int id)
{
    applyPresetDefaults(static_cast<IndexDiscoveryPreset>(id));
    if (m_activeRoot && m_activeRoot->exists) {
        onQuery();
    }
}

IndexQuerySpec IndexBrowserPage::buildQuerySpec() const
{
    const int kind = m_kindFilter->currentIndex();
    // 0 All, 1 Files, 2 Folders
    bool files = kind == 0 || kind == 1;
    bool dirs = kind == 0 || kind == 2;
    if (kind == 0) {
        files = true;
        dirs = true;
    }

    std::optional<ByteSize> minSize =
        minSizeFromUi(m_minSizeFilter, m_minSizeCustom);
    std::optional<std::uint64_t> olderThan;
    const int days = m_activityFilter->currentData().toInt();
    if (days > 0) {
        olderThan = static_cast<std::uint64_t>(days);
    }

    QString ext = m_extFilter->text().trimmed();
    if (ext.startsWith(QLatin1Char('.'))) {
        ext = ext.mid(1);
    }
    ext = ext.toLower();

    IndexSortKey sortBy =
        static_cast<IndexSortKey>(m_sortFilter->currentData().toInt());
    bool sortDesc = true;
    if (sortBy == IndexSortKey::Name || sortBy == IndexSortKey::Classification) {
        sortDesc = false;
    }
    // Activity: larger inactive first => ascending last_write when filtering old
    if (sortBy == IndexSortKey::LastWrite) {
        sortDesc = false;  // oldest first
    }

    auto spec = makeDiscoveryQuery(
        m_preset, files, dirs, minSize, olderThan, ext.toStdString(),
        m_classFilter->currentData().toString().toStdString(),
        m_strengthFilter->currentData().toString().toStdString(),
        m_searchEdit->text().trimmed().toStdString(), m_browsePath, sortBy,
        sortDesc, static_cast<std::size_t>(m_limitSpin->value()));

    // When user picks a single class while Developer preset is active, prefer
    // the explicit class filter.
    if (m_preset == IndexDiscoveryPreset::DeveloperStorage &&
        !m_classFilter->currentData().toString().isEmpty()) {
        spec.classifications.clear();
        spec.classification =
            m_classFilter->currentData().toString().toStdString();
    }
    if (m_preset == IndexDiscoveryPreset::ReclaimCandidates &&
        !m_strengthFilter->currentData().toString().isEmpty()) {
        spec.candidateStrengths.clear();
        spec.candidateStrength =
            m_strengthFilter->currentData().toString().toStdString();
    }
    return spec;
}

void IndexBrowserPage::setBusy(bool busy)
{
    m_reloadButton->setEnabled(!busy);
    m_indexNewButton->setEnabled(!busy);
    m_queryButton->setEnabled(!busy);
    m_cancelButton->setEnabled(busy || m_queryRunning);
    updateActionState();
}

void IndexBrowserPage::updateActionState()
{
    const bool sessionBusy = m_session && m_session->isRunning();
    const bool busy = sessionBusy || m_queryRunning;
    const auto root = selectedRoot();
    const bool hasRoot = root.has_value() && root->exists;
    m_refreshButton->setEnabled(hasRoot && !busy);
    m_rebuildButton->setEnabled((hasRoot || root.has_value()) && !busy);
    m_queryButton->setEnabled(hasRoot && !busy);
    const bool hasSel = !selectedRows().empty();
    const bool single = selectedRows().size() == 1;
    m_addReviewButton->setEnabled(hasSel && !busy);
    m_showReviewButton->setEnabled(true);
    m_findDuplicatesButton->setEnabled(hasRoot && !busy);
    m_openButton->setEnabled(single && !busy);
    m_revealButton->setEnabled(single && !busy);
    m_copyPathButton->setEnabled(hasSel && !busy);
    m_cancelButton->setEnabled(busy);
    m_reloadButton->setEnabled(!busy);
    m_indexNewButton->setEnabled(!busy);
}

void IndexBrowserPage::reloadRoots()
{
    clearHits();
    m_browsePath.clear();
    m_rootsList->clear();
    m_roots = listIndexSummaries();
    for (std::size_t i = 0; i < m_roots.size(); ++i) {
        const auto& r = m_roots[i];
        const QString path =
            r.rootPath.empty() ? fromWide(r.rootKey) : fromWide(r.rootPath);
        const QString text =
            QStringLiteral("%1 %2\n  %3 · %4 · age %5")
                .arg(freshnessIndicator(r.freshness), path,
                     QString::fromStdString(r.freshnessLabel),
                     QString::fromStdString(
                         SizeFormatter::format(r.logicalBytes)),
                     formatAge(r.ageMs));
        auto* item = new QListWidgetItem(text, m_rootsList);
        item->setData(Qt::UserRole, static_cast<uint>(i));
    }
    if (m_roots.empty()) {
        m_rootMeta->setText(QStringLiteral(
            "No published indexes. Use Index Folder… to create one."));
        m_emptyLabel->setText(QStringLiteral(
            "No index selected.\n\nUse Index Folder… to build a persistent "
            "index, then discover large / old / reclaimable storage here."));
        m_emptyLabel->setVisible(true);
        m_hitsView->setVisible(false);
        emit statusMessage(QStringLiteral("No indexes found."));
    } else {
        m_rootMeta->setText(
            QStringLiteral("%1 published index(es). Select one to explore.")
                .arg(m_roots.size()));
        emit statusMessage(
            QStringLiteral("Loaded %1 index(es).").arg(m_roots.size()));
    }
    updateBreadcrumb();
    updateActionState();
}

std::optional<IndexRootSummary> IndexBrowserPage::selectedRoot() const
{
    const auto items = m_rootsList->selectedItems();
    if (items.isEmpty()) {
        return std::nullopt;
    }
    const auto idx = items.front()->data(Qt::UserRole).toUInt();
    if (idx >= m_roots.size()) {
        return std::nullopt;
    }
    return m_roots[idx];
}

void IndexBrowserPage::onRootSelectionChanged()
{
    clearHits();
    m_browsePath.clear();
    m_activeRoot = selectedRoot();
    updateRootHeader();
    updateBreadcrumb();
    updateActionState();
    updateInspector();
    if (m_activeRoot && m_activeRoot->exists) {
        onQuery();
    } else if (!m_activeRoot) {
        m_emptyLabel->setText(QStringLiteral("No index selected."));
        m_emptyLabel->setVisible(true);
        m_hitsView->setVisible(false);
    }
}

void IndexBrowserPage::updateRootHeader()
{
    if (!m_activeRoot) {
        m_rootMeta->setText(QStringLiteral("No index selected."));
        return;
    }
    const auto& r = *m_activeRoot;
    QString text;
    text += QStringLiteral("<b>%1</b><br/>").arg(fromWide(r.rootPath));
    text += QStringLiteral("Indexed: %1 ago · Freshness: <b>%2</b>")
                .arg(formatAge(r.ageMs),
                     QString::fromStdString(r.freshnessLabel));
    if (!r.reason.empty() &&
        (r.freshness == IndexFreshness::IncrementalUnavailable ||
         r.freshness == IndexFreshness::FullRebuildRequired ||
         r.freshness == IndexFreshness::Error)) {
        text += QStringLiteral(" (%1)").arg(QString::fromStdString(r.reason));
    }
    text += QStringLiteral("<br/>");
    text += QStringLiteral("Files: %1 · Folders: %2 · Logical size: %3<br/>")
                .arg(r.fileCount)
                .arg(r.dirCount)
                .arg(QString::fromStdString(
                    SizeFormatter::format(r.logicalBytes)));
    const QString method =
        r.lastRefreshMethod.empty()
            ? QStringLiteral("—")
            : (r.lastRefreshMethod == "usn"
                   ? QStringLiteral("Incremental")
                   : QStringLiteral("Full"));
    text += QStringLiteral("Last refresh: %1 · Checkpoint: %2")
                .arg(method, QString::fromStdString(
                                 r.checkpointStatus.empty() ? std::string("-")
                                                            : r.checkpointStatus));
    if (r.freshness == IndexFreshness::IncrementalUnavailable) {
        text += QStringLiteral(
            "<br/><i>Incremental refresh unavailable in this session. "
            "A full rebuild is still available.</i>");
    } else if (r.freshness == IndexFreshness::FullRebuildRequired) {
        text += QStringLiteral(
            "<br/><i>Index requires rebuild for incremental refresh. "
            "Snapshot remains queryable.</i>");
    }
    m_rootMeta->setText(text);
}

void IndexBrowserPage::updateBreadcrumb()
{
    while (QLayoutItem* child = m_breadcrumbLayout->takeAt(0)) {
        if (child->widget()) {
            child->widget()->deleteLater();
        }
        delete child;
    }
    if (!m_activeRoot || m_activeRoot->rootPath.empty()) {
        m_breadcrumbBar->setVisible(false);
        return;
    }
    m_breadcrumbBar->setVisible(true);

    std::vector<std::pair<QString, std::wstring>> segments;
    segments.emplace_back(fromWide(m_activeRoot->rootPath),
                          m_activeRoot->rootPath);

    if (!m_browsePath.empty() &&
        m_browsePath != m_activeRoot->rootPath) {
        // Split relative segments under root.
        std::wstring rel = m_browsePath;
        const std::wstring& root = m_activeRoot->rootPath;
        if (rel.size() > root.size()) {
            std::size_t pos = root.size();
            if (pos < rel.size() &&
                (rel[pos] == L'\\' || rel[pos] == L'/')) {
                ++pos;
            }
            std::wstring acc = root;
            while (pos < rel.size()) {
                std::size_t next = rel.find_first_of(L"\\/", pos);
                if (next == std::wstring::npos) {
                    next = rel.size();
                }
                const std::wstring part = rel.substr(pos, next - pos);
                if (!part.empty()) {
                    if (!acc.empty() && acc.back() != L'\\' &&
                        acc.back() != L'/') {
                        acc.push_back(L'\\');
                    }
                    acc += part;
                    segments.emplace_back(fromWide(part), acc);
                }
                pos = next + 1;
            }
        }
    }

    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (i > 0) {
            auto* sep = new QLabel(QStringLiteral(">"), m_breadcrumbBar);
            sep->setStyleSheet(QStringLiteral("color: #888; padding: 0 4px;"));
            m_breadcrumbLayout->addWidget(sep);
        }
        auto* btn = new QToolButton(m_breadcrumbBar);
        btn->setText(segments[i].first);
        btn->setAutoRaise(true);
        btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        const int segIndex = static_cast<int>(i);
        connect(btn, &QToolButton::clicked, this,
                [this, segIndex]() { onBreadcrumbClicked(segIndex); });
        m_breadcrumbLayout->addWidget(btn);
    }
    m_breadcrumbLayout->addStretch(1);
}

void IndexBrowserPage::onBreadcrumbClicked(int segmentIndex)
{
    if (!m_activeRoot) {
        return;
    }
    if (segmentIndex <= 0) {
        m_browsePath.clear();
    } else {
        // Rebuild path for segment.
        std::wstring acc = m_activeRoot->rootPath;
        // Re-run same split to get segment path.
        const std::wstring& rel = m_browsePath;
        const std::wstring& root = m_activeRoot->rootPath;
        int idx = 0;
        if (rel.size() > root.size()) {
            std::size_t pos = root.size();
            if (pos < rel.size() &&
                (rel[pos] == L'\\' || rel[pos] == L'/')) {
                ++pos;
            }
            while (pos < rel.size()) {
                std::size_t next = rel.find_first_of(L"\\/", pos);
                if (next == std::wstring::npos) {
                    next = rel.size();
                }
                const std::wstring part = rel.substr(pos, next - pos);
                if (!part.empty()) {
                    if (!acc.empty() && acc.back() != L'\\' &&
                        acc.back() != L'/') {
                        acc.push_back(L'\\');
                    }
                    acc += part;
                    ++idx;
                    if (idx == segmentIndex) {
                        m_browsePath = acc;
                        onQuery();
                        updateBreadcrumb();
                        return;
                    }
                }
                pos = next + 1;
            }
        }
        m_browsePath.clear();
    }
    updateBreadcrumb();
    onQuery();
}

void IndexBrowserPage::clearHits()
{
    m_hitModel->clear();
    m_queryMeta->clear();
    m_selectionMeta->clear();
    m_inspector->clear();
    m_hierarchyChildren.clear();
    m_overview = {};
    m_otherSelection.reset();
    if (m_treemap) {
        m_treemap->clear();
    }
    if (m_overviewLabel) {
        m_overviewLabel->clear();
    }
}

void IndexBrowserPage::onQuery()
{
    auto root = selectedRoot();
    if (!root || !root->exists) {
        QMessageBox::information(this, QStringLiteral("SpaceLens"),
                                 QStringLiteral("Select a published index first."));
        return;
    }
    if (m_minSizeFilter->currentData().toString() == QLatin1String("custom")) {
        const QString text = m_minSizeCustom->text().trimmed();
        if (!text.isEmpty()) {
            const auto parsed = parseSize(text.toStdString());
            if (!parsed.error.empty()) {
                QMessageBox::warning(
                    this, QStringLiteral("SpaceLens"),
                    QStringLiteral("Invalid min size: %1")
                        .arg(QString::fromStdString(parsed.error)));
                return;
            }
        }
    }

    const auto spec = buildQuerySpec();
    m_activeRoot = root;

    // Cancel previous query generation.
    const quint64 gen = m_queryGeneration.fetch_add(1) + 1;
    if (m_queryWorker.joinable()) {
        m_queryWorker.request_stop();
        m_queryWorker.join();
    }

    m_queryRunning = true;
    m_queryMeta->setText(QStringLiteral("Querying index…"));
    m_emptyLabel->setVisible(false);
    m_hitsView->setVisible(true);
    updateActionState();

    const std::wstring rootPath = root->rootPath;
    const std::wstring location =
        m_browsePath.empty() ? rootPath : m_browsePath;
    m_queryWorker = std::jthread([this, gen, rootPath, location, spec](
                                     std::stop_token stop) {
        if (stop.stop_requested()) {
            return;
        }
        PendingBrowsePayload payload;
        payload.discovery = queryIndex(rootPath, spec);
        if (stop.stop_requested() || m_queryGeneration.load() != gen) {
            return;
        }
        payload.hierarchy = queryHierarchyChildren(rootPath, location, 10000);
        if (stop.stop_requested() || m_queryGeneration.load() != gen) {
            return;
        }
        {
            std::lock_guard lock(m_queryMutex);
            m_pendingBrowse = std::move(payload);
        }
        QMetaObject::invokeMethod(
            this,
            [this, gen]() { onQueryFinished(gen); },
            Qt::QueuedConnection);
    });
}

void IndexBrowserPage::onQueryFinished(quint64 generation)
{
    if (generation != m_queryGeneration.load()) {
        return;
    }
    PendingBrowsePayload payload;
    {
        std::lock_guard lock(m_queryMutex);
        if (!m_pendingBrowse) {
            m_queryRunning = false;
            updateActionState();
            return;
        }
        payload = std::move(*m_pendingBrowse);
        m_pendingBrowse.reset();
    }
    applyQueryResult(std::move(payload), generation);
}

void IndexBrowserPage::updateOverviewLabel(const StorageOverview& overview)
{
    if (!m_overviewLabel) {
        return;
    }
    QString text;
    text += QStringLiteral("<b>Storage overview</b> · ");
    text += fromWide(overview.locationPath);
    text += QStringLiteral("<br/>");
    text += QStringLiteral("Indexed logical size: <b>%1</b>")
                .arg(QString::fromStdString(
                    SizeFormatter::format(overview.locationLogicalBytes)));
    text += QStringLiteral(" · Direct children: %1 files, %2 folders")
                .arg(overview.directFileCount)
                .arg(overview.directDirCount);
    text += QStringLiteral(" · Snapshot age: %1")
                .arg(formatAge(overview.snapshotAgeMs));
    if (overview.hasLargestChild) {
        text += QStringLiteral("<br/>Largest immediate child: <b>%1</b> (%2)")
                    .arg(fromWide(overview.largestChildName),
                         QString::fromStdString(SizeFormatter::format(
                             overview.largestChildBytes)));
    }
    // Counts only — never a sum of recursive directory sizes as "reclaimable".
    QStringList intel;
    if (overview.developerCandidateCount > 0) {
        intel << QStringLiteral("%1 developer candidates")
                     .arg(overview.developerCandidateCount);
    }
    if (overview.strongReclaimCount > 0) {
        intel << QStringLiteral("%1 strong reclaim")
                     .arg(overview.strongReclaimCount);
    }
    if (overview.moderateReclaimCount > 0) {
        intel << QStringLiteral("%1 moderate reclaim")
                     .arg(overview.moderateReclaimCount);
    }
    if (overview.oldAndLargeCount > 0) {
        intel << QStringLiteral("%1 old & large")
                     .arg(overview.oldAndLargeCount);
    }
    if (!intel.isEmpty()) {
        text += QStringLiteral("<br/>Among direct children: ") +
                intel.join(QStringLiteral(" · "));
    }
    text += QStringLiteral(
        "<br/><span style='color:#666'>Sizes are logical bytes from the index "
        "snapshot (not physical size-on-disk).</span>");
    m_overviewLabel->setText(text);
}

void IndexBrowserPage::applyHierarchyResult(
    const HierarchyChildrenResult& hierarchy)
{
    m_hierarchyChildren.clear();
    m_otherSelection.reset();
    if (!hierarchy.ok) {
        if (m_treemap) {
            m_treemap->clear();
        }
        if (m_overviewLabel) {
            m_overviewLabel->setText(
                QStringLiteral("Overview unavailable: %1")
                    .arg(QString::fromStdString(hierarchy.error)));
        }
        return;
    }
    m_hierarchyChildren = hierarchy.children;
    m_overview = hierarchy.overview;
    updateOverviewLabel(m_overview);

    std::vector<TreemapDisplayItem> display;
    display.reserve(hierarchy.children.size());
    for (const auto& h : hierarchy.children) {
        TreemapDisplayItem d;
        d.path = h.path;
        d.name = h.name;
        d.sizeBytes = h.size_bytes;
        d.kind = h.kind;
        d.classification = h.classification;
        d.candidateStrength = h.candidate_strength;
        display.push_back(std::move(d));
    }
    if (m_treemap) {
        m_treemap->setItems(std::move(display), hierarchy.locationLogicalBytes);
    }
}

void IndexBrowserPage::applyQueryResult(PendingBrowsePayload payload,
                                        quint64 generation)
{
    if (generation != m_queryGeneration.load()) {
        return;
    }
    m_queryRunning = false;

    applyHierarchyResult(payload.hierarchy);

    auto& result = payload.discovery;
    if (!result.ok) {
        m_hitModel->clear();
        m_queryMeta->setText(QStringLiteral("Query failed: %1")
                                 .arg(QString::fromStdString(result.error)));
        m_emptyLabel->setText(
            result.error == "index_not_found"
                ? QStringLiteral("Index snapshot is unavailable.")
                : QStringLiteral("Query failed: %1")
                      .arg(QString::fromStdString(result.error)));
        m_emptyLabel->setVisible(true);
        m_hitsView->setVisible(false);
        emit statusMessage(QStringLiteral("Index query failed."));
        updateActionState();
        return;
    }

    m_hitModel->setHits(std::move(result.hits), result.age_ms,
                        result.root.indexedAtIso);

    if (m_hitModel->rowCount() == 0) {
        QString emptyMsg;
        if (m_preset == IndexDiscoveryPreset::ReclaimCandidates) {
            emptyMsg = QStringLiteral(
                "No strong reclaim candidates were found with the current "
                "filters.");
        } else if (!m_searchEdit->text().trimmed().isEmpty()) {
            emptyMsg = QStringLiteral("No results match these filters.");
        } else {
            emptyMsg = QStringLiteral("No results match these filters.");
        }
        m_emptyLabel->setText(emptyMsg);
        m_emptyLabel->setVisible(true);
        m_hitsView->setVisible(true);
    } else {
        m_emptyLabel->setVisible(false);
        m_hitsView->setVisible(true);
    }

    const QString presetName =
        QString::fromUtf8(toString(m_preset));
    m_queryMeta->setText(
        QStringLiteral(
            "%1 matches · %2 represented · Showing %3 · Query: %4 ms · "
            "Hierarchy: %5 ms · preset=%6 · source=persistent_index · age %7")
            .arg(result.matched_items)
            .arg(QString::fromStdString(
                SizeFormatter::format(result.matched_logical_bytes)))
            .arg(result.returned_items)
            .arg(result.query_elapsed_ms)
            .arg(payload.hierarchy.query_elapsed_ms)
            .arg(presetName)
            .arg(formatAge(result.age_ms)));

    emit statusMessage(
        QStringLiteral("Showing %1 of %2 match(es).")
            .arg(result.returned_items)
            .arg(result.matched_items));
    updateActionState();
    updateInspector();
}

void IndexBrowserPage::onRefreshIndex()
{
    auto root = selectedRoot();
    if (!root || root->rootPath.empty()) {
        return;
    }
    if (root->freshness == IndexFreshness::IncrementalUnavailable) {
        QMessageBox::information(
            this, QStringLiteral("Incremental unavailable"),
            QStringLiteral(
                "Incremental refresh unavailable in this session "
                "(%1).\n\nA full rebuild is still available. SpaceLens will "
                "not prompt for elevation repeatedly.")
                .arg(QString::fromStdString(
                    root->reason.empty() ? std::string("access denied or journal "
                                                       "unavailable")
                                         : root->reason)));
        return;
    }
    if (!m_session->startRefresh(fromWide(root->rootPath))) {
        QMessageBox::warning(this, QStringLiteral("SpaceLens"),
                             QStringLiteral("An index job is already running."));
        return;
    }
    setBusy(true);
    emit statusMessage(QStringLiteral("Refreshing index…"));
}

void IndexBrowserPage::onRebuildIndex()
{
    auto root = selectedRoot();
    QString path = root ? fromWide(root->rootPath) : QString();
    if (path.isEmpty()) {
        path = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Select folder to re-index"),
            QStringLiteral("C:/"),
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (path.isEmpty()) {
            return;
        }
    }
    const auto reply = QMessageBox::question(
        this, QStringLiteral("Rebuild Index"),
        QStringLiteral(
            "Rebuild the persistent index for:\n%1\n\n"
            "This rescans the tree (read-only). Previous published index stays "
            "until publish succeeds. This is not automatic after failed "
            "incremental refresh.")
            .arg(path),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (reply != QMessageBox::Yes) {
        return;
    }
    if (!m_session->startBuild(path)) {
        QMessageBox::warning(this, QStringLiteral("SpaceLens"),
                             QStringLiteral("An index job is already running."));
        return;
    }
    setBusy(true);
    emit statusMessage(QStringLiteral("Full index rebuild started…"));
}

void IndexBrowserPage::onIndexNewRoot()
{
    const QString path = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Select folder to index"), QStringLiteral("C:/"),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (path.isEmpty()) {
        return;
    }
    if (!m_session->startBuild(path)) {
        QMessageBox::warning(this, QStringLiteral("SpaceLens"),
                             QStringLiteral("An index job is already running."));
        return;
    }
    setBusy(true);
    emit statusMessage(QStringLiteral("Indexing %1…").arg(path));
}

void IndexBrowserPage::onCancelJob()
{
    if (m_queryRunning) {
        m_queryGeneration.fetch_add(1);
        if (m_queryWorker.joinable()) {
            m_queryWorker.request_stop();
        }
        m_queryRunning = false;
        m_queryMeta->setText(QStringLiteral("Query cancelled."));
        updateActionState();
    }
    if (m_session && m_session->isRunning()) {
        m_session->cancel();
        emit statusMessage(QStringLiteral("Cancelling index job…"));
    }
}

void IndexBrowserPage::onBuildFinished(IndexBuildState state)
{
    auto result = m_session->takeBuildResult();
    setBusy(false);
    if (!result) {
        emit statusMessage(QStringLiteral("Index build finished (no result)."));
        return;
    }
    switch (state) {
    case IndexBuildState::Completed:
        emit statusMessage(QStringLiteral("Index build complete."));
        break;
    case IndexBuildState::Cancelled:
        emit statusMessage(QStringLiteral("Index build cancelled."));
        break;
    case IndexBuildState::Failed:
        emit statusMessage(QStringLiteral("Index build failed: %1")
                               .arg(QString::fromStdString(result->error)));
        break;
    }
    reloadRoots();
}

void IndexBrowserPage::onRefreshFinished(IndexRefreshOutcome outcome)
{
    auto result = m_session->takeRefreshResult();
    setBusy(false);
    if (!result) {
        emit statusMessage(QStringLiteral("Index refresh finished (no result)."));
        return;
    }
    QString msg;
    switch (outcome) {
    case IndexRefreshOutcome::Refreshed:
        msg = QStringLiteral("Updated using incremental refresh (%1 rows).")
                  .arg(result->rowsChanged);
        break;
    case IndexRefreshOutcome::AlreadyCurrent:
        msg = QStringLiteral("Index already current (no USN delta).");
        break;
    case IndexRefreshOutcome::FullRebuildRequired:
        msg = QStringLiteral("Full rebuild required: %1")
                  .arg(QString::fromStdString(result->reason));
        break;
    case IndexRefreshOutcome::Cancelled:
        msg = QStringLiteral("Refresh cancelled.");
        break;
    case IndexRefreshOutcome::Failed:
        msg = QStringLiteral("Refresh failed: %1")
                  .arg(QString::fromStdString(result->error.empty()
                                                  ? result->reason
                                                  : result->error));
        break;
    case IndexRefreshOutcome::IndexNotFound:
        msg = QStringLiteral("Index not found.");
        break;
    }
    emit statusMessage(msg);
    if (outcome == IndexRefreshOutcome::FullRebuildRequired) {
        QMessageBox::information(
            this, QStringLiteral("Full rebuild required"),
            QStringLiteral(
                "%1\n\nThe previous index remains queryable. Use Rebuild "
                "when you want a new snapshot. SpaceLens will not rebuild "
                "silently.")
                .arg(msg));
    }
    reloadRoots();
}

void IndexBrowserPage::onSessionStatus(const QString& message)
{
    emit statusMessage(message);
}

void IndexBrowserPage::onHitSelectionChanged()
{
    if (m_syncingSelection) {
        return;
    }
    m_otherSelection.reset();
    updateInspector();
    updateActionState();
    const auto rows = selectedRows();
    if (rows.empty()) {
        m_selectionMeta->clear();
        if (m_treemap) {
            m_treemap->clearSelection();
        }
        return;
    }
    ByteSize total = 0;
    for (int row : rows) {
        if (const auto* h = m_hitModel->hitAt(row)) {
            total += h->size_bytes;
        }
    }
    m_selectionMeta->setText(
        QStringLiteral("Selected: %1 item(s) · Logical size: %2")
            .arg(rows.size())
            .arg(QString::fromStdString(SizeFormatter::format(total))));

    // Highlight corresponding treemap cell when a single immediate child is selected.
    if (rows.size() == 1 && m_treemap) {
        if (const auto* h = m_hitModel->hitAt(rows.front())) {
            m_syncingSelection = true;
            m_treemap->setSelectedPath(h->path);
            m_syncingSelection = false;
        }
    }
}

std::vector<int> IndexBrowserPage::selectedRows() const
{
    std::vector<int> rows;
    if (!m_hitsView || !m_hitsView->selectionModel()) {
        return rows;
    }
    const auto indexes = m_hitsView->selectionModel()->selectedRows();
    rows.reserve(static_cast<std::size_t>(indexes.size()));
    for (const QModelIndex& idx : indexes) {
        rows.push_back(idx.row());
    }
    return rows;
}

std::vector<IndexHit> IndexBrowserPage::selectedHits() const
{
    std::vector<IndexHit> out;
    for (int row : selectedRows()) {
        if (const auto* h = m_hitModel->hitAt(row)) {
            out.push_back(*h);
        }
    }
    return out;
}

void IndexBrowserPage::showOtherInspector(const TreemapDisplayItem& item)
{
    QString text;
    text += QStringLiteral("Other (visualization aggregate)\n\n");
    text += QStringLiteral("%1 smaller items\n%2 combined logical size\n\n")
                .arg(item.otherItemCount)
                .arg(QString::fromStdString(SizeFormatter::format(item.sizeBytes)));
    text += QStringLiteral(
        "This rectangle is not a filesystem item, not a cleanup candidate, "
        "and not an indexed database row.\n\n"
        "Use the discovery table below (or clear size filters) to inspect "
        "individual smaller items. Double-click folders in the treemap to "
        "drill down.");
    if (m_overview.locationLogicalBytes > 0) {
        const double pct =
            100.0 * static_cast<double>(item.sizeBytes) /
            static_cast<double>(m_overview.locationLogicalBytes);
        text += QStringLiteral("\n\nShare of current location: %1%")
                    .arg(pct, 0, 'f', 1);
    }
    m_inspector->setPlainText(text);
}

void IndexBrowserPage::selectTablePath(const std::wstring& path)
{
    if (!m_hitsView || !m_hitModel) {
        return;
    }
    const int row = m_hitModel->findRowByPath(path);
    m_syncingSelection = true;
    if (row >= 0) {
        m_hitsView->selectionModel()->select(
            m_hitModel->index(row, 0),
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        m_hitsView->scrollTo(m_hitModel->index(row, 0));
    } else if (m_hitsView->selectionModel()) {
        m_hitsView->selectionModel()->clearSelection();
    }
    m_syncingSelection = false;
}

void IndexBrowserPage::onTreemapItemClicked(const TreemapDisplayItem& item)
{
    if (m_syncingSelection) {
        return;
    }
    if (item.isOther) {
        m_otherSelection = item;
        if (m_hitsView && m_hitsView->selectionModel()) {
            m_syncingSelection = true;
            m_hitsView->selectionModel()->clearSelection();
            m_syncingSelection = false;
        }
        showOtherInspector(item);
        m_selectionMeta->setText(
            QStringLiteral("Selected: Other · %1 items · %2")
                .arg(item.otherItemCount)
                .arg(QString::fromStdString(
                    SizeFormatter::format(item.sizeBytes))));
        updateActionState();
        return;
    }
    m_otherSelection.reset();
    // Prefer hierarchy hit metadata for inspector when the path is not in the
    // discovery table (filters may hide it).
    selectTablePath(item.path);
    const int row = m_hitModel->findRowByPath(item.path);
    if (row < 0) {
        // Build a synthetic inspector from hierarchy children.
        const IndexHit* hier = nullptr;
        for (const auto& h : m_hierarchyChildren) {
            if (h.path == item.path) {
                hier = &h;
                break;
            }
        }
        if (hier) {
            QString text;
            text += QStringLiteral("%1\n%2\n\n")
                        .arg(fromWide(hier->name),
                             QString::fromStdString(
                                 SizeFormatter::format(hier->size_bytes)));
            text += QStringLiteral("Path\n%1\n\n").arg(fromWide(hier->path));
            text += QStringLiteral("Type\n%1\n\n")
                        .arg(hier->kind == IndexEntryKind::Directory
                                 ? QStringLiteral("Folder")
                                 : QStringLiteral("File"));
            if (m_overview.locationLogicalBytes > 0) {
                const double pct =
                    100.0 * static_cast<double>(hier->size_bytes) /
                    static_cast<double>(m_overview.locationLogicalBytes);
                text += QStringLiteral("Share of current location\n%1%\n\n")
                            .arg(pct, 0, 'f', 1);
            }
            text += QStringLiteral("Classification\n%1\nConfidence\n%2\n")
                        .arg(QString::fromStdString(hier->classification),
                             QString::fromStdString(hier->confidence));
            if (!hier->rule_id.empty()) {
                text += QStringLiteral("Matched rule\n%1\n")
                            .arg(QString::fromStdString(hier->rule_id));
            }
            text += QStringLiteral("\nActivity\n%1\n")
                        .arg(formatActivity(hier->last_write_ticks));
            text += QStringLiteral("Location safety\n%1\n\n")
                        .arg(QString::fromStdString(hier->location_safety));
            text +=
                QStringLiteral("Reclaimability\n%1\nCandidate strength\n%2\n\n")
                    .arg(QString::fromStdString(hier->reclaimability),
                         QString::fromStdString(hier->candidate_strength));
            text += QStringLiteral("Source\npersistent_index (hierarchy)\n");
            text += QStringLiteral("Snapshot age\n%1\n")
                        .arg(formatAge(m_overview.snapshotAgeMs));
            text += QStringLiteral(
                "\nLogical size from the index snapshot — not physical "
                "size-on-disk. Not permission to delete.");
            m_inspector->setPlainText(text);
            m_selectionMeta->setText(
                QStringLiteral("Selected: %1 · Logical size: %2")
                    .arg(fromWide(hier->name),
                         QString::fromStdString(
                             SizeFormatter::format(hier->size_bytes))));
        }
    } else {
        updateInspector();
    }
    updateActionState();
}

void IndexBrowserPage::onTreemapItemDoubleClicked(const TreemapDisplayItem& item)
{
    if (item.isOther) {
        // Visualization aggregate only — surface table view of smaller items.
        m_otherSelection = item;
        showOtherInspector(item);
        emit statusMessage(
            QStringLiteral("Other groups %1 smaller items — use the table or "
                           "filters to inspect them.")
                .arg(item.otherItemCount));
        return;
    }
    if (item.kind == IndexEntryKind::Directory) {
        browseInto(item.path);
    }
}

void IndexBrowserPage::updateInspector()
{
    m_inspector->clear();
    if (m_otherSelection) {
        showOtherInspector(*m_otherSelection);
        return;
    }
    const auto selected = selectedHits();
    if (selected.empty()) {
        if (m_activeRoot) {
            QString text;
            text += QStringLiteral("Storage discovery inspector\n\n");
            text += QStringLiteral("Root\n%1\n\n")
                        .arg(fromWide(m_activeRoot->rootPath));
            text += QStringLiteral("Freshness\n%1\n")
                        .arg(QString::fromStdString(m_activeRoot->freshnessLabel));
            if (m_overview.locationLogicalBytes > 0) {
                text += QStringLiteral("\nCurrent location\n%1\n")
                            .arg(fromWide(m_overview.locationPath));
                text += QStringLiteral("Indexed logical size\n%1\n")
                            .arg(QString::fromStdString(SizeFormatter::format(
                                m_overview.locationLogicalBytes)));
            }
            text += QStringLiteral(
                "\nClick the treemap or a table row to inspect. Double-click a "
                "folder rectangle to drill down. This is not a live listing.");
            m_inspector->setPlainText(text);
        }
        return;
    }
    if (selected.size() > 1) {
        ByteSize total = 0;
        for (const auto& h : selected) {
            total += h.size_bytes;
        }
        m_inspector->setPlainText(
            QStringLiteral("%1 items selected — total logical %2\n"
                           "Source: persistent_index snapshot (age %3)\n\n"
                           "Multi-select actions: Copy Paths, Add to Cleanup "
                           "Review.\nOpen/Reveal require a single selection.")
                .arg(selected.size())
                .arg(QString::fromStdString(SizeFormatter::format(total)))
                .arg(formatAge(m_hitModel->indexAgeMs())));
        return;
    }

    const auto& h = selected.front();
    QString text;
    text += QStringLiteral("%1\n%2\n\n")
                .arg(h.name.empty() ? QFileInfo(fromWide(h.path)).fileName()
                                    : fromWide(h.name),
                     QString::fromStdString(SizeFormatter::format(h.size_bytes)));
    text += QStringLiteral("Path\n%1\n\n").arg(fromWide(h.path));
    text += QStringLiteral("Type\n%1\n\n")
                .arg(h.kind == IndexEntryKind::Directory
                         ? QStringLiteral("Folder")
                         : QStringLiteral("File"));
    if (m_overview.locationLogicalBytes > 0) {
        const double pct = 100.0 * static_cast<double>(h.size_bytes) /
                           static_cast<double>(m_overview.locationLogicalBytes);
        text += QStringLiteral("Share of current location\n%1%\n\n")
                    .arg(pct, 0, 'f', 1);
    }
    text += QStringLiteral("Logical size\n%1\n\n")
                .arg(QString::fromStdString(SizeFormatter::format(h.size_bytes)));
    text += QStringLiteral("Classification\n%1\nConfidence\n%2\n")
                .arg(QString::fromStdString(h.classification),
                     QString::fromStdString(h.confidence));
    if (!h.rule_id.empty()) {
        text += QStringLiteral("Matched rule\n%1\n")
                    .arg(QString::fromStdString(h.rule_id));
    }
    text += QStringLiteral("\nActivity\n%1\n")
                .arg(formatActivity(h.last_write_ticks));
    text += QStringLiteral("Location safety\n%1\n\n")
                .arg(QString::fromStdString(h.location_safety));
    text += QStringLiteral("Reclaimability\n%1\nCandidate strength\n%2\n\n")
                .arg(QString::fromStdString(h.reclaimability),
                     QString::fromStdString(h.candidate_strength));
    text += QStringLiteral("Source\npersistent_index\n");
    text +=
        QStringLiteral("Snapshot age\n%1\n").arg(formatAge(m_hitModel->indexAgeMs()));
    if (!m_hitModel->indexedAtIso().empty()) {
        text += QStringLiteral("Indexed at\n%1\n")
                    .arg(QString::fromStdString(m_hitModel->indexedAtIso()));
    }
    text += QStringLiteral(
        "\nNote: classification and reclaim strength are not permission to "
        "delete. Snapshot paths may be stale relative to the live filesystem.");
    m_inspector->setPlainText(text);
}

bool IndexBrowserPage::ensurePathExists(const std::wstring& path,
                                        QString* message) const
{
    const DWORD attr = ::GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        const DWORD err = ::GetLastError();
        if (message) {
            if (err == ERROR_ACCESS_DENIED) {
                *message = QStringLiteral(
                    "Access denied for this path.\nThe index snapshot may be "
                    "stale or the process lacks permission.");
            } else {
                *message = QStringLiteral(
                    "This item no longer exists at the indexed path.\n"
                    "The index snapshot may be stale.\n\nUse Refresh Index or "
                    "Rebuild when you want an updated snapshot.");
            }
        }
        return false;
    }
    return true;
}

void IndexBrowserPage::browseInto(const std::wstring& path)
{
    m_browsePath = path;
    m_otherSelection.reset();
    updateBreadcrumb();
    onQuery();
}

void IndexBrowserPage::onHitDoubleClicked(const QModelIndex& index)
{
    if (!index.isValid()) {
        return;
    }
    const auto* hit = m_hitModel->hitAt(index.row());
    if (!hit) {
        return;
    }
    if (hit->kind == IndexEntryKind::Directory) {
        browseInto(hit->path);
        return;
    }
    // Files: do not auto-execute. Show inspector only (already selected).
    emit statusMessage(
        QStringLiteral("File selected — use Open for the default app, or "
                       "Show in Explorer."));
}

void IndexBrowserPage::onHitsContextMenu(const QPoint& pos)
{
    const QModelIndex idx = m_hitsView->indexAt(pos);
    if (idx.isValid() && !m_hitsView->selectionModel()->isSelected(idx)) {
        m_hitsView->selectionModel()->select(
            idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
    const auto rows = selectedRows();
    if (rows.empty()) {
        return;
    }
    QMenu menu(this);
    if (rows.size() == 1) {
        menu.addAction(QStringLiteral("Open"), this,
                       &IndexBrowserPage::onOpenSelected);
        menu.addAction(QStringLiteral("Open Folder"), this,
                       &IndexBrowserPage::onOpenFolderSelected);
        menu.addAction(QStringLiteral("Show in Explorer"), this,
                       &IndexBrowserPage::onRevealSelected);
        menu.addSeparator();
    }
    menu.addAction(QStringLiteral("Copy Path(s)"), this,
                   &IndexBrowserPage::onCopyPath);
    menu.addAction(QStringLiteral("Copy Details"), this,
                   &IndexBrowserPage::onCopyDetails);
    menu.addSeparator();
    menu.addAction(QStringLiteral("Find Duplicates"), this,
                   &IndexBrowserPage::onFindDuplicates);
    menu.addAction(QStringLiteral("Add to Cleanup Review"), this,
                   &IndexBrowserPage::onAddToReview);
    menu.addAction(QStringLiteral("Open Cleanup Review"), this,
                   &IndexBrowserPage::onShowReview);
    if (rows.size() == 1) {
        if (const auto* h = m_hitModel->hitAt(rows.front());
            h && h->kind == IndexEntryKind::Directory) {
            menu.addSeparator();
            menu.addAction(QStringLiteral("Browse in Index"), this, [this, h]() {
                browseInto(h->path);
            });
        }
    }
    menu.exec(m_hitsView->viewport()->mapToGlobal(pos));
}

void IndexBrowserPage::onOpenSelected()
{
    const auto hits = selectedHits();
    if (hits.size() != 1) {
        return;
    }
    QString msg;
    if (!ensurePathExists(hits.front().path, &msg)) {
        QMessageBox::warning(this, QStringLiteral("Path unavailable"), msg);
        return;
    }
    if (!openWithDefaultApp(hits.front().path)) {
        QMessageBox::warning(this, QStringLiteral("SpaceLens"),
                             QStringLiteral("Could not open the selected item."));
    }
}

void IndexBrowserPage::onOpenFolderSelected()
{
    const auto hits = selectedHits();
    if (hits.size() != 1) {
        return;
    }
    QString msg;
    if (!ensurePathExists(hits.front().path, &msg)) {
        QMessageBox::warning(this, QStringLiteral("Path unavailable"), msg);
        return;
    }
    if (!openParentFolder(hits.front().path)) {
        QMessageBox::warning(
            this, QStringLiteral("SpaceLens"),
            QStringLiteral("Could not open the parent folder."));
    }
}

void IndexBrowserPage::onRevealSelected()
{
    const auto hits = selectedHits();
    if (hits.size() != 1) {
        return;
    }
    QString msg;
    if (!ensurePathExists(hits.front().path, &msg)) {
        QMessageBox::warning(this, QStringLiteral("Path unavailable"), msg);
        return;
    }
    if (!revealInExplorer(hits.front().path)) {
        QMessageBox::warning(
            this, QStringLiteral("SpaceLens"),
            QStringLiteral("Could not reveal the item in Explorer."));
    }
}

void IndexBrowserPage::onCopyPath()
{
    const auto hits = selectedHits();
    if (hits.empty()) {
        return;
    }
    QStringList lines;
    for (const auto& h : hits) {
        lines << fromWide(h.path);
    }
    QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
    emit statusMessage(
        QStringLiteral("Copied %1 path(s).").arg(hits.size()));
}

void IndexBrowserPage::onCopyDetails()
{
    const auto hits = selectedHits();
    if (hits.empty()) {
        return;
    }
    QString text;
    for (const auto& h : hits) {
        text += fromWide(h.path);
        text += QLatin1Char('\n');
        text += QStringLiteral("  size=%1 class=%2 strength=%3 reclaim=%4\n")
                    .arg(QString::fromStdString(SizeFormatter::format(h.size_bytes)),
                         QString::fromStdString(h.classification),
                         QString::fromStdString(h.candidate_strength),
                         QString::fromStdString(h.reclaimability));
    }
    text += QStringLiteral("source=persistent_index age=%1\n")
                .arg(formatAge(m_hitModel->indexAgeMs()));
    QApplication::clipboard()->setText(text);
    emit statusMessage(QStringLiteral("Copied details for %1 item(s).")
                           .arg(hits.size()));
}

void IndexBrowserPage::onShowReview()
{
    emit showReviewRequested();
}

void IndexBrowserPage::onFindDuplicates()
{
    const auto root = selectedRoot();
    if (!root || !root->exists) {
        emit statusMessage(
            QStringLiteral("Select an indexed root to find duplicates."));
        return;
    }
    DuplicateFilesDialog dialog(m_review, root->rootPath, root->ageMs,
                                root->indexedAtIso, this);
    dialog.exec();
}

void IndexBrowserPage::onAddToReview()
{
    const auto selected = selectedHits();
    if (selected.empty()) {
        return;
    }
    // Validate existence best-effort; still allow adding stale snapshot rows
    // with a note — review is planning-only.
    int missing = 0;
    int added = 0;
    int already = 0;
    int conflicts = 0;
    WindowsCleanupMetadataReader reader;
    FILETIME ft{};
    ::GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER now{};
    now.LowPart = ft.dwLowDateTime;
    now.HighPart = ft.dwHighDateTime;
    const FileTimeTicks addedAt = now.QuadPart;
    const auto root = selectedRoot();

    for (const auto& hit : selected) {
        QString msg;
        if (!ensurePathExists(hit.path, &msg)) {
            ++missing;
        }
        CleanupCandidate c;
        c.path = hit.path;
        c.kind = hit.kind == IndexEntryKind::Directory ? ItemKind::Directory
                                                       : ItemKind::File;
        c.sizeAtSelection = hit.size_bytes;
        c.lastWriteTime = hit.last_write_ticks;
        c.classification.category = parseStorageCategory(hit.classification);
        c.classification.confidence = parseConfidence(hit.confidence);
        c.classification.ruleId = hit.rule_id;
        c.classification.reason =
            hit.rule_id.empty() ? "From persistent index snapshot"
                                : ("rule:" + hit.rule_id);
        c.reasonAdded = "Added from Index Browser V2";
        c.source = "persistent_index";
        c.sourceRoot = root ? root->rootPath : std::wstring{};
        c.indexAgeMs = m_hitModel->indexAgeMs();
        c.indexIndexedAtIso = m_hitModel->indexedAtIso();
        c.capturedSafety = classifyLocation(c.path);
        c.capturedReclaimability = parseReclaimabilityLabel(hit.reclaimability);
        c.capturedCandidateStrength =
            parseCandidateStrengthLabel(hit.candidate_strength);
        prepareCleanupCandidateForAdd(c, reader, addedAt);

        const auto status = m_review.add(std::move(c));
        if (!status.ok) {
            emit statusMessage(QString::fromStdString(status.message));
            return;
        }
        if (status.add.result == CleanupAddResult::Added) {
            ++added;
        } else if (status.add.result == CleanupAddResult::DuplicateUpdated) {
            ++already;
        } else if (status.add.result == CleanupAddResult::IdentityConflict) {
            ++conflicts;
        }
    }
    QString status =
        QStringLiteral("Cleanup Review: %1 item(s) (added %2 from index, "
                       "already %3, identity conflicts %4)")
            .arg(m_review.review().size())
            .arg(added)
            .arg(already)
            .arg(conflicts);
    if (missing > 0) {
        status += QStringLiteral(" — %1 path(s) missing on disk (snapshot may "
                                 "be stale)")
                      .arg(missing);
        QMessageBox::information(
            this, QStringLiteral("Snapshot may be stale"),
            QStringLiteral(
                "%1 of the selected item(s) no longer exist at the indexed "
                "path.\nThey were still added to Cleanup Review with snapshot "
                "metadata for planning only.\n\nConsider Refresh Index or "
                "Rebuild.")
                .arg(missing));
    }
    emit statusMessage(status);
}

}  // namespace spacelens
