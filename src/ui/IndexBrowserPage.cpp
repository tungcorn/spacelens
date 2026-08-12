#include "ui/IndexBrowserPage.hpp"

#include "core/Classification.hpp"
#include "core/SizeFormatter.hpp"
#include "core/SizeParse.hpp"

#include <QAbstractItemView>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QTextEdit>
#include <QVBoxLayout>

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
    return QStringLiteral("%1 h").arg(min / 60.0, 0, 'f', 1);
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

}  // namespace

IndexBrowserPage::IndexBrowserPage(CleanupReview& review, QWidget* parent)
    : QWidget(parent)
    , m_review(review)
    , m_session(new IndexSession(this))
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
    m_refreshButton = new QPushButton(QStringLiteral("Refresh (USN)"), this);
    m_rebuildButton = new QPushButton(QStringLiteral("Full Rebuild"), this);
    m_cancelButton = new QPushButton(QStringLiteral("Cancel"), this);
    m_addReviewButton =
        new QPushButton(QStringLiteral("Add to Cleanup Review"), this);
    toolbar->addWidget(m_reloadButton);
    toolbar->addWidget(m_indexNewButton);
    toolbar->addWidget(m_refreshButton);
    toolbar->addWidget(m_rebuildButton);
    toolbar->addWidget(m_cancelButton);
    toolbar->addStretch(1);
    toolbar->addWidget(m_addReviewButton);
    rootLayout->addLayout(toolbar);

    auto* note = new QLabel(
        QStringLiteral(
            "Indexed view is a snapshot — not live filesystem truth. "
            "Refresh/Rebuild are explicit and never delete or move analyzed data."),
        this);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: #666;"));
    rootLayout->addWidget(note);

    auto* filterRow = new QHBoxLayout();
    filterRow->addWidget(new QLabel(QStringLiteral("Kind:"), this));
    m_kindFilter = new QComboBox(this);
    m_kindFilter->addItems({QStringLiteral("Files"), QStringLiteral("Folders"),
                            QStringLiteral("Both")});
    filterRow->addWidget(m_kindFilter);
    filterRow->addWidget(new QLabel(QStringLiteral("Min size:"), this));
    m_minSizeFilter = new QLineEdit(this);
    m_minSizeFilter->setPlaceholderText(QStringLiteral("e.g. 10MB"));
    m_minSizeFilter->setMaximumWidth(100);
    filterRow->addWidget(m_minSizeFilter);
    filterRow->addWidget(new QLabel(QStringLiteral("Ext:"), this));
    m_extFilter = new QLineEdit(this);
    m_extFilter->setPlaceholderText(QStringLiteral("gguf"));
    m_extFilter->setMaximumWidth(80);
    filterRow->addWidget(m_extFilter);
    filterRow->addWidget(new QLabel(QStringLiteral("Class:"), this));
    m_classFilter = new QComboBox(this);
    m_classFilter->addItem(QStringLiteral("Any"), QString());
    for (const char* name :
         {"BuildArtifact", "DependencyDirectory", "PackageCache", "IdeCache",
          "LogData", "TemporaryData", "DownloadedAiModel", "Archive",
          "ApplicationData", "SystemData", "UserData", "Unknown"}) {
        m_classFilter->addItem(QString::fromUtf8(name), QString::fromUtf8(name));
    }
    filterRow->addWidget(m_classFilter);
    filterRow->addWidget(new QLabel(QStringLiteral("Strength:"), this));
    m_strengthFilter = new QComboBox(this);
    m_strengthFilter->addItem(QStringLiteral("Any"), QString());
    for (const char* name :
         {"Strong", "Moderate", "ReviewOnly", "None"}) {
        m_strengthFilter->addItem(QString::fromUtf8(name),
                                  QString::fromUtf8(name));
    }
    filterRow->addWidget(m_strengthFilter);
    filterRow->addWidget(new QLabel(QStringLiteral("Limit:"), this));
    m_limitSpin = new QSpinBox(this);
    m_limitSpin->setRange(1, 5000);
    m_limitSpin->setValue(50);
    filterRow->addWidget(m_limitSpin);
    m_queryButton = new QPushButton(QStringLiteral("Query Index"), this);
    filterRow->addWidget(m_queryButton);
    filterRow->addStretch(1);
    rootLayout->addLayout(filterRow);

    m_rootMeta = new QLabel(QStringLiteral("No index selected."), this);
    m_rootMeta->setWordWrap(true);
    rootLayout->addWidget(m_rootMeta);
    m_queryMeta = new QLabel(QStringLiteral(""), this);
    rootLayout->addWidget(m_queryMeta);

    auto* splitter = new QSplitter(Qt::Horizontal, this);

    m_rootsList = new QListWidget(splitter);
    m_rootsList->setAlternatingRowColors(true);
    m_rootsList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_rootsList->setMinimumWidth(220);

    auto* rightSplit = new QSplitter(Qt::Vertical, splitter);
    m_hitsList = new QListWidget(rightSplit);
    m_hitsList->setAlternatingRowColors(true);
    m_hitsList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_hitsList->setUniformItemSizes(true);
    m_inspector = new QTextEdit(rightSplit);
    m_inspector->setReadOnly(true);
    rightSplit->setStretchFactor(0, 3);
    rightSplit->setStretchFactor(1, 2);

    splitter->addWidget(m_rootsList);
    splitter->addWidget(rightSplit);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    rootLayout->addWidget(splitter, 1);

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
    connect(m_rootsList, &QListWidget::itemSelectionChanged, this,
            &IndexBrowserPage::onRootSelectionChanged);
    connect(m_hitsList, &QListWidget::itemSelectionChanged, this,
            &IndexBrowserPage::onHitSelectionChanged);

    updateActionState();
}

void IndexBrowserPage::setBusy(bool busy)
{
    m_reloadButton->setEnabled(!busy);
    m_indexNewButton->setEnabled(!busy);
    m_queryButton->setEnabled(!busy);
    m_cancelButton->setEnabled(busy);
    updateActionState();
}

void IndexBrowserPage::updateActionState()
{
    const bool busy = m_session && m_session->isRunning();
    const auto root = selectedRoot();
    const bool hasRoot = root.has_value() && root->exists;
    m_refreshButton->setEnabled(hasRoot && !busy);
    m_rebuildButton->setEnabled((hasRoot || root.has_value()) && !busy);
    m_queryButton->setEnabled(hasRoot && !busy);
    m_addReviewButton->setEnabled(!m_hitsList->selectedItems().isEmpty() &&
                                  !busy);
    m_cancelButton->setEnabled(busy);
    m_reloadButton->setEnabled(!busy);
    m_indexNewButton->setEnabled(!busy);
}

void IndexBrowserPage::reloadRoots()
{
    clearHits();
    m_rootsList->clear();
    m_roots = listIndexSummaries();
    for (std::size_t i = 0; i < m_roots.size(); ++i) {
        const auto& r = m_roots[i];
        const QString path =
            r.rootPath.empty() ? fromWide(r.rootKey) : fromWide(r.rootPath);
        const QString text =
            QStringLiteral("%1\n  %2 · %3 files · age %4")
                .arg(path, QString::fromStdString(r.freshnessLabel),
                     QString::number(r.fileCount), formatAge(r.ageMs));
        auto* item = new QListWidgetItem(text, m_rootsList);
        item->setData(Qt::UserRole, static_cast<uint>(i));
    }
    if (m_roots.empty()) {
        m_rootMeta->setText(
            QStringLiteral("No published indexes found under LocalAppData\\SpaceLens."));
        emit statusMessage(QStringLiteral("No indexes found."));
    } else {
        m_rootMeta->setText(
            QStringLiteral("%1 published index(es). Select one to query.")
                .arg(m_roots.size()));
        emit statusMessage(
            QStringLiteral("Loaded %1 index(es).").arg(m_roots.size()));
    }
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
    m_activeRoot = selectedRoot();
    if (!m_activeRoot) {
        m_rootMeta->setText(QStringLiteral("No index selected."));
        updateActionState();
        return;
    }
    const auto& r = *m_activeRoot;
    QString text;
    text += QStringLiteral("Root: %1\n").arg(fromWide(r.rootPath));
    text += QStringLiteral("Freshness: %1").arg(QString::fromStdString(r.freshnessLabel));
    if (!r.reason.empty()) {
        text += QStringLiteral(" (%1)").arg(QString::fromStdString(r.reason));
    }
    text += QStringLiteral("\n");
    text += QStringLiteral("Files: %1 · Dirs: %2 · Logical: %3\n")
                .arg(r.fileCount)
                .arg(r.dirCount)
                .arg(QString::fromStdString(SizeFormatter::format(r.logicalBytes)));
    text += QStringLiteral("Indexed at: %1 · Age: %2\n")
                .arg(QString::fromStdString(r.indexedAtIso), formatAge(r.ageMs));
    text += QStringLiteral("Last method: %1 · Checkpoint: %2 · Incremental: %3")
                .arg(QString::fromStdString(r.lastRefreshMethod.empty()
                                                ? std::string("-")
                                                : r.lastRefreshMethod),
                     QString::fromStdString(r.checkpointStatus.empty()
                                                ? std::string("-")
                                                : r.checkpointStatus),
                     QString::fromUtf8(toString(r.incrementalState)));
    m_rootMeta->setText(text);
    updateActionState();
    updateInspector();
}

void IndexBrowserPage::clearHits()
{
    m_hits.clear();
    m_hitsList->clear();
    m_queryMeta->clear();
    m_inspector->clear();
}

void IndexBrowserPage::onQuery()
{
    auto root = selectedRoot();
    if (!root || !root->exists) {
        QMessageBox::information(this, QStringLiteral("SpaceLens"),
                                 QStringLiteral("Select a published index first."));
        return;
    }

    const int kind = m_kindFilter->currentIndex();
    const bool files = kind == 0 || kind == 2;
    const bool dirs = kind == 1 || kind == 2;

    std::optional<ByteSize> minSize;
    const QString minText = m_minSizeFilter->text().trimmed();
    if (!minText.isEmpty()) {
        const auto parsed = parseSize(minText.toStdString());
        if (!parsed.error.empty()) {
            QMessageBox::warning(
                this, QStringLiteral("SpaceLens"),
                QStringLiteral("Invalid min size: %1")
                    .arg(QString::fromStdString(parsed.error)));
            return;
        }
        minSize = parsed.bytes;
    }

    QString ext = m_extFilter->text().trimmed();
    if (ext.startsWith(QLatin1Char('.'))) {
        ext = ext.mid(1);
    }
    ext = ext.toLower();

    auto spec = makeBrowserQuerySpec(
        files, dirs, minSize, ext.toStdString(),
        m_classFilter->currentData().toString().toStdString(),
        m_strengthFilter->currentData().toString().toStdString(),
        static_cast<std::size_t>(m_limitSpin->value()));

    auto result = queryIndex(root->rootPath, spec);
    clearHits();
    m_activeRoot = root;

    if (!result.ok) {
        m_queryMeta->setText(
            QStringLiteral("Query failed: %1")
                .arg(QString::fromStdString(result.error)));
        emit statusMessage(QStringLiteral("Index query failed."));
        updateActionState();
        return;
    }

    m_hits.reserve(result.hits.size());
    for (auto& hit : result.hits) {
        HitRow row;
        row.hit = std::move(hit);
        row.indexAgeMs = result.age_ms;
        row.indexedAtIso = result.root.indexedAtIso;
        m_hits.push_back(std::move(row));
    }

    for (std::size_t i = 0; i < m_hits.size(); ++i) {
        const auto& h = m_hits[i].hit;
        const char* kindLabel =
            h.kind == IndexEntryKind::Directory ? "DIR" : "FILE";
        const QString text =
            QStringLiteral("[%1] %2    %3    %4 / %5")
                .arg(QString::fromUtf8(kindLabel), fromWide(h.path),
                     QString::fromStdString(SizeFormatter::format(h.size_bytes)),
                     QString::fromStdString(h.classification),
                     QString::fromStdString(h.candidate_strength));
        auto* item = new QListWidgetItem(text, m_hitsList);
        item->setData(Qt::UserRole, static_cast<uint>(i));
    }

    m_queryMeta->setText(
        QStringLiteral(
            "Snapshot query · matched %1 · returned %2 · logical %3 · age %4 · "
            "source persistent_index")
            .arg(result.matched_items)
            .arg(result.returned_items)
            .arg(QString::fromStdString(
                SizeFormatter::format(result.matched_logical_bytes)))
            .arg(formatAge(result.age_ms)));
    emit statusMessage(
        QStringLiteral("Query returned %1 hit(s).").arg(result.returned_items));
    updateActionState();
}

void IndexBrowserPage::onRefreshIndex()
{
    auto root = selectedRoot();
    if (!root || root->rootPath.empty()) {
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
        this, QStringLiteral("Full rebuild"),
        QStringLiteral(
            "Rebuild the persistent index for:\n%1\n\n"
            "This rescans the tree (read-only). Previous published index stays "
            "until publish succeeds.")
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
    if (m_session) {
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
        emit statusMessage(
            QStringLiteral("Index build failed: %1")
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
        msg = QStringLiteral("USN refresh applied (%1 rows changed).")
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
            this, QStringLiteral("Incremental unavailable"),
            QStringLiteral(
                "%1\n\nThe previous index remains queryable. Use Full Rebuild "
                "when you want a new snapshot.")
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
    updateInspector();
    updateActionState();
}

std::vector<IndexBrowserPage::HitRow> IndexBrowserPage::selectedHits() const
{
    std::vector<HitRow> out;
    for (QListWidgetItem* item : m_hitsList->selectedItems()) {
        const auto idx = item->data(Qt::UserRole).toUInt();
        if (idx < m_hits.size()) {
            out.push_back(m_hits[idx]);
        }
    }
    return out;
}

void IndexBrowserPage::updateInspector()
{
    m_inspector->clear();
    const auto selected = selectedHits();
    if (selected.empty()) {
        if (m_activeRoot) {
            QString text;
            text += QStringLiteral("Index snapshot inspector\n\n");
            text += QStringLiteral("Root path\n%1\n\n")
                        .arg(fromWide(m_activeRoot->rootPath));
            text += QStringLiteral("DB path\n%1\n\n")
                        .arg(fromWide(m_activeRoot->dbPath));
            text += QStringLiteral("Freshness\n%1\n")
                        .arg(QString::fromStdString(m_activeRoot->freshnessLabel));
            text += QStringLiteral(
                "\nThis is not a live listing. Query the index to inspect "
                "entries.");
            m_inspector->setPlainText(text);
        }
        return;
    }
    if (selected.size() > 1) {
        ByteSize total = 0;
        for (const auto& h : selected) {
            total += h.hit.size_bytes;
        }
        m_inspector->setPlainText(
            QStringLiteral("%1 items selected — total logical %2\n"
                           "Source: persistent_index snapshot (age %3)")
                .arg(selected.size())
                .arg(QString::fromStdString(SizeFormatter::format(total)))
                .arg(formatAge(selected.front().indexAgeMs)));
        return;
    }

    const auto& row = selected.front();
    const auto& h = row.hit;
    QString text;
    text += QStringLiteral("Name\n%1\n\n")
                .arg(QFileInfo(fromWide(h.path)).fileName());
    text += QStringLiteral("Full path\n%1\n\n").arg(fromWide(h.path));
    text += QStringLiteral("Type\n%1\n\n")
                .arg(h.kind == IndexEntryKind::Directory
                         ? QStringLiteral("Directory")
                         : QStringLiteral("File"));
    text += QStringLiteral("Logical size\n%1\n\n")
                .arg(QString::fromStdString(SizeFormatter::format(h.size_bytes)));
    text += QStringLiteral("Classification\n%1\nConfidence\n%2\n\n")
                .arg(QString::fromStdString(h.classification),
                     QString::fromStdString(h.confidence));
    text += QStringLiteral("Location safety\n%1\n\n")
                .arg(QString::fromStdString(h.location_safety));
    text += QStringLiteral("Reclaimability\n%1\nCandidate strength\n%2\n\n")
                .arg(QString::fromStdString(h.reclaimability),
                     QString::fromStdString(h.candidate_strength));
    if (h.last_write_ticks != 0) {
        text += QStringLiteral("Last write (FILETIME ticks)\n%1\n\n")
                    .arg(h.last_write_ticks);
    }
    text += QStringLiteral("Source\npersistent_index\n");
    text += QStringLiteral("Snapshot age\n%1\n").arg(formatAge(row.indexAgeMs));
    if (!row.indexedAtIso.empty()) {
        text += QStringLiteral("Indexed at\n%1\n").arg(
            QString::fromStdString(row.indexedAtIso));
    }
    text += QStringLiteral(
        "\nNote: classification and reclaim strength are not permission to "
        "delete. Snapshot may be stale relative to the live filesystem.");
    m_inspector->setPlainText(text);
}

void IndexBrowserPage::onAddToReview()
{
    const auto selected = selectedHits();
    if (selected.empty()) {
        return;
    }
    int added = 0;
    for (const auto& row : selected) {
        CleanupCandidate c;
        c.path = row.hit.path;
        c.kind = row.hit.kind == IndexEntryKind::Directory ? ItemKind::Directory
                                                           : ItemKind::File;
        c.sizeAtSelection = row.hit.size_bytes;
        c.lastWriteTime = row.hit.last_write_ticks;
        c.classification.category =
            parseStorageCategory(row.hit.classification);
        c.classification.confidence = parseConfidence(row.hit.confidence);
        c.classification.reason = "From persistent index snapshot";
        c.reasonAdded = "Added from Index Browser";
        c.source = "persistent_index";
        c.indexAgeMs = row.indexAgeMs;
        c.indexIndexedAtIso = row.indexedAtIso;
        if (m_review.add(std::move(c)) != 0) {
            ++added;
        }
    }
    emit statusMessage(
        QStringLiteral("Cleanup Review: %1 item(s) (added %2 from index)")
            .arg(m_review.size())
            .arg(added));
}

}  // namespace spacelens
