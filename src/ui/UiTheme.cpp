#include "ui/UiTheme.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLayout>
#include <QPushButton>
#include <QStyleFactory>
#include <QTimeZone>
#include <QWidget>

namespace spacelens {
namespace {

qreal relativeLuminance(const QColor& color)
{
    return 0.2126 * color.redF() + 0.7152 * color.greenF() +
           0.0722 * color.blueF();
}

QColor mixRgb(const QColor& a, const QColor& b, qreal amountA)
{
    const qreal amountB = 1.0 - amountA;
    return QColor(int(a.red() * amountA + b.red() * amountB),
                  int(a.green() * amountA + b.green() * amountB),
                  int(a.blue() * amountA + b.blue() * amountB));
}

}  // namespace

QString formatFileTimeLocal(std::uint64_t ticks)
{
    if (ticks == 0) {
        return {};
    }
    constexpr std::uint64_t kEpochDeltaSec = 11644473600ULL;
    const std::uint64_t seconds = ticks / 10'000'000ULL;
    if (seconds < kEpochDeltaSec) {
        return {};
    }
    const qint64 unixSec = static_cast<qint64>(seconds - kEpochDeltaSec);
    const QDateTime dt =
        QDateTime::fromSecsSinceEpoch(unixSec, QTimeZone::UTC).toLocalTime();
    if (!dt.isValid()) {
        return {};
    }
    return dt.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

bool paletteIsDark(const QPalette& palette)
{
    return relativeLuminance(palette.color(QPalette::Window)) < 0.45;
}

QColor mutedTextColor(const QPalette& palette)
{
    // Blend toward Window instead of Mid — Mid is too faint for secondary copy.
    return mixRgb(palette.color(QPalette::WindowText),
                  palette.color(QPalette::Window), 0.74);
}

QColor subtleFillColor(const QPalette& palette)
{
    const QColor window = palette.color(QPalette::Window);
    const QColor base = palette.color(QPalette::Base);
    if (paletteIsDark(palette)) {
        return window.lighter(112);
    }
    return QColor((window.red() + base.red()) / 2,
                  (window.green() + base.green()) / 2,
                  (window.blue() + base.blue()) / 2);
}

QColor hairlineColor(const QPalette& palette)
{
    QColor mid = palette.color(QPalette::Mid);
    if (!mid.isValid()) {
        mid = palette.color(QPalette::WindowText);
    }
    mid.setAlpha(90);
    return mid;
}

QColor contrastingTextColor(const QColor& fill)
{
    return relativeLuminance(fill) > 0.55 ? QColor(26, 26, 26)
                                          : QColor(244, 244, 244);
}

QColor adjustClassificationFill(const QColor& fill, bool dark)
{
    if (!dark) {
        return fill;
    }
    QColor adapted = fill.toHsv();
    adapted.setHsv(adapted.hue(),
                   qMin(160, adapted.saturation() + 10),
                   qMin(170, qMax(110, adapted.value() - 40)));
    return adapted;
}

bool isNoiseDisplayValue(const QString& value)
{
    const QString text = value.trimmed();
    if (text.isEmpty()) {
        return true;
    }
    if (text.compare(QStringLiteral("Unknown"), Qt::CaseInsensitive) == 0) {
        return true;
    }
    if (text.compare(QStringLiteral("(none)"), Qt::CaseInsensitive) == 0) {
        return true;
    }
    if (text.compare(QStringLiteral("None"), Qt::CaseInsensitive) == 0) {
        return true;
    }
    if (text.compare(QStringLiteral("0x0"), Qt::CaseInsensitive) == 0) {
        return true;
    }
    return false;
}

QString displayFolderName(const QString& path)
{
    const QString native = QDir::toNativeSeparators(path.trimmed());
    if (native.isEmpty()) {
        return native;
    }
    const QString name = QFileInfo(native).fileName();
    if (name.isEmpty()) {
        return native;
    }
    return name;
}

QString uiStyleSheet()
{
    return QStringLiteral(
        "QPushButton#slPrimary {"
        "  background: palette(highlight);"
        "  color: palette(highlighted-text);"
        "  border: 1px solid palette(highlight);"
        "  padding: 5px 14px;"
        "  font-weight: 600;"
        "}"
        "QPushButton#slPrimary:disabled {"
        "  background: palette(mid);"
        "  color: palette(midlight);"
        "  border-color: palette(mid);"
        "}"
        "QPushButton#slPrimary:focus {"
        "  outline: 2px solid palette(highlight);"
        "  outline-offset: 1px;"
        "}"
        "QPushButton#slSecondary {"
        "  padding: 5px 12px;"
        "}"
        "QPushButton#slTertiary, QToolButton#slTertiary {"
        "  border: none;"
        "  padding: 4px 8px;"
        "  background: transparent;"
        "}"
        "QPushButton#slTertiary:hover, QToolButton#slTertiary:hover {"
        "  background: palette(midlight);"
        "}"
        "QWidget#slWorkspaceBar {"
        "  background: palette(window);"
        "}"
        "QWidget#slWorkspaceGroup {"
        "  background: palette(base);"
        "  border: 1px solid palette(mid);"
        "}"
        "QPushButton#slWorkspace {"
        "  border: none;"
        "  padding: 4px 12px;"
        "  min-width: 72px;"
        "  background: transparent;"
        "}"
        "QPushButton#slWorkspace:checked {"
        "  background: palette(highlight);"
        "  color: palette(highlighted-text);"
        "  font-weight: 600;"
        "}"
        "QPushButton#slWorkspace:focus {"
        "  outline: 1px solid palette(highlight);"
        "  outline-offset: -1px;"
        "}"
        "QLabel#slPageTitle {"
        "  font-size: 16px;"
        "  font-weight: 600;"
        "}"
        "QLabel#slPageSubtitle {"
        "  color: palette(window-text);"
        "  font-size: 12px;"
        "}"
        "QLabel#slMetricLabel {"
        "  font-size: 10px;"
        "  font-weight: 600;"
        "  color: palette(window-text);"
        "}"
        "QLabel#slMetricValue {"
        "  font-size: 15px;"
        "  font-weight: 600;"
        "}"
        "QLabel#slRootTitle {"
        "  font-weight: 600;"
        "}"
        "QLabel#slRootMeta, QLabel#slHint, QLabel#slPropLabel {"
        "  color: palette(window-text);"
        "  font-size: 11px;"
        "}"
        "QLabel#slPropLabel {"
        "  font-weight: 600;"
        "}"
        "QLabel#slPropValue {"
        "  color: palette(window-text);"
        "}"
        "QLabel#slTrust {"
        "  color: palette(window-text);"
        "  font-size: 11px;"
        "  font-weight: 600;"
        "  padding: 2px 8px;"
        "  border: 1px solid palette(mid);"
        "  background: palette(base);"
        "}"
        "QWidget#slSegmentBar {"
        "  background: palette(base);"
        "  border: 1px solid palette(mid);"
        "}"
        "QToolButton#slSegment {"
        "  padding: 4px 10px;"
        "  border: none;"
        "  background: transparent;"
        "}"
        "QToolButton#slSegment:checked {"
        "  background: palette(highlight);"
        "  color: palette(highlighted-text);"
        "  font-weight: 600;"
        "}"
        "QFrame#slHairline {"
        "  background: palette(mid);"
        "  max-height: 1px;"
        "  min-height: 1px;"
        "}"
        "QLabel#slEmptyTitle {"
        "  font-size: 14px;"
        "  font-weight: 600;"
        "}"
        "QLabel#slEmptyBody {"
        "  color: palette(window-text);"
        "}"
        "QFrame#slZaloScanCard {"
        "  background: palette(window);"
        "  border: 1px solid palette(mid);"
        "  border-radius: 10px;"
        "}"
        "QFrame#slZaloStatBox {"
        "  background: palette(base);"
        "  border: 1px solid palette(mid);"
        "  border-radius: 6px;"
        "}"
        "QLabel#slZaloStatTitle {"
        "  color: palette(placeholder-text);"
        "  font-size: 10px;"
        "  font-weight: 600;"
        "}"
        "QLabel#slZaloStatValue {"
        "  color: palette(highlight);"
        "  font-size: 16px;"
        "  font-weight: 700;"
        "}"
        "QFrame#slZaloChip {"
        "  background: palette(alternate-base);"
        "  border: 1px solid palette(mid);"
        "  border-radius: 6px;"
        "}"
        "QLabel#slZaloPathIndicator {"
        "  color: palette(placeholder-text);"
        "  font-size: 11px;"
        "}");
}

void applyApplicationChrome(QApplication& app)
{
    app.setStyleSheet(uiStyleSheet());
}

void applyVisualReviewPalette(QApplication& app, const QString& theme)
{
    const QString key = theme.trimmed().toLower();
    if (key != QLatin1String("dark") && key != QLatin1String("light")) {
        return;
    }
    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        app.setStyle(fusion);
    }
    QPalette palette;
    if (key == QLatin1String("dark")) {
        palette.setColor(QPalette::Window, QColor(32, 32, 32));
        palette.setColor(QPalette::WindowText, QColor(236, 236, 236));
        palette.setColor(QPalette::Base, QColor(22, 22, 22));
        palette.setColor(QPalette::AlternateBase, QColor(40, 40, 40));
        palette.setColor(QPalette::Text, QColor(236, 236, 236));
        palette.setColor(QPalette::Button, QColor(45, 45, 45));
        palette.setColor(QPalette::ButtonText, QColor(236, 236, 236));
        palette.setColor(QPalette::Highlight, QColor(0, 120, 212));
        palette.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
        palette.setColor(QPalette::Mid, QColor(92, 92, 92));
        palette.setColor(QPalette::Midlight, QColor(64, 64, 64));
        palette.setColor(QPalette::Light, QColor(72, 72, 72));
        palette.setColor(QPalette::Dark, QColor(18, 18, 18));
        palette.setColor(QPalette::PlaceholderText, QColor(186, 186, 186));
        palette.setColor(QPalette::ToolTipBase, QColor(45, 45, 45));
        palette.setColor(QPalette::ToolTipText, QColor(236, 236, 236));
    } else {
        palette.setColor(QPalette::Window, QColor(243, 243, 243));
        palette.setColor(QPalette::WindowText, QColor(26, 26, 26));
        palette.setColor(QPalette::Base, QColor(255, 255, 255));
        palette.setColor(QPalette::AlternateBase, QColor(247, 247, 247));
        palette.setColor(QPalette::Text, QColor(26, 26, 26));
        palette.setColor(QPalette::Button, QColor(243, 243, 243));
        palette.setColor(QPalette::ButtonText, QColor(26, 26, 26));
        palette.setColor(QPalette::Highlight, QColor(0, 120, 212));
        palette.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
        palette.setColor(QPalette::Mid, QColor(160, 160, 160));
        palette.setColor(QPalette::Midlight, QColor(227, 227, 227));
        palette.setColor(QPalette::Light, QColor(255, 255, 255));
        palette.setColor(QPalette::Dark, QColor(160, 160, 160));
        palette.setColor(QPalette::PlaceholderText, QColor(96, 96, 96));
        palette.setColor(QPalette::ToolTipBase, QColor(255, 255, 255));
        palette.setColor(QPalette::ToolTipText, QColor(26, 26, 26));
    }
    app.setPalette(palette);
    app.setStyleSheet(uiStyleSheet());
}

void markPrimaryButton(QPushButton* button)
{
    if (button == nullptr) {
        return;
    }
    button->setObjectName(QStringLiteral("slPrimary"));
    button->setDefault(true);
    button->setAutoDefault(false);
}

void markSecondaryButton(QAbstractButton* button)
{
    if (button == nullptr) {
        return;
    }
    button->setObjectName(QStringLiteral("slSecondary"));
}

void markTertiaryButton(QAbstractButton* button)
{
    if (button == nullptr) {
        return;
    }
    button->setObjectName(QStringLiteral("slTertiary"));
}

void applyPageMargins(QWidget* widget)
{
    if (widget == nullptr || widget->layout() == nullptr) {
        return;
    }
    widget->layout()->setContentsMargins(kUiSpace16, kUiSpace12, kUiSpace16,
                                         kUiSpace12);
    widget->layout()->setSpacing(kUiSpace12);
}

}  // namespace spacelens
