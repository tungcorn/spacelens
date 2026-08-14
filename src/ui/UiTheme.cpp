#include "ui/UiTheme.hpp"

#include <QApplication>
#include <QDateTime>
#include <QLayout>
#include <QPushButton>
#include <QTimeZone>
#include <QWidget>

namespace spacelens {
namespace {

qreal relativeLuminance(const QColor& color)
{
    return 0.2126 * color.redF() + 0.7152 * color.greenF() +
           0.0722 * color.blueF();
}

}  // namespace

QString formatFileTimeLocal(std::uint64_t ticks)
{
    if (ticks == 0) {
        return QStringLiteral("(none)");
    }
    constexpr std::uint64_t kEpochDeltaSec = 11644473600ULL;
    const std::uint64_t seconds = ticks / 10'000'000ULL;
    if (seconds < kEpochDeltaSec) {
        return QStringLiteral("(none)");
    }
    const qint64 unixSec = static_cast<qint64>(seconds - kEpochDeltaSec);
    const QDateTime dt =
        QDateTime::fromSecsSinceEpoch(unixSec, QTimeZone::UTC).toLocalTime();
    if (!dt.isValid()) {
        return QStringLiteral("(none)");
    }
    return dt.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

bool paletteIsDark(const QPalette& palette)
{
    return relativeLuminance(palette.color(QPalette::Window)) < 0.45;
}

QColor mutedTextColor(const QPalette& palette)
{
    QColor text = palette.color(QPalette::WindowText);
    text.setAlpha(paletteIsDark(palette) ? 180 : 160);
    return text;
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
        "QPushButton#slWorkspace {"
        "  border: 1px solid transparent;"
        "  padding: 6px 14px;"
        "  min-width: 88px;"
        "}"
        "QPushButton#slWorkspace:checked {"
        "  background: palette(highlight);"
        "  color: palette(highlighted-text);"
        "  border-color: palette(highlight);"
        "  font-weight: 600;"
        "}"
        "QPushButton#slWorkspace:focus {"
        "  border-color: palette(highlight);"
        "}"
        "QLabel#slPageTitle {"
        "  font-size: 16px;"
        "  font-weight: 600;"
        "}"
        "QLabel#slPageSubtitle {"
        "  color: palette(mid);"
        "}"
        "QLabel#slMetricLabel {"
        "  font-size: 10px;"
        "  font-weight: 600;"
        "  color: palette(mid);"
        "}"
        "QLabel#slMetricValue {"
        "  font-size: 15px;"
        "  font-weight: 600;"
        "}"
        "QLabel#slRootTitle {"
        "  font-weight: 600;"
        "}"
        "QLabel#slRootMeta, QLabel#slTrust, QLabel#slHint {"
        "  color: palette(mid);"
        "}"
        "QToolButton#slSegment {"
        "  padding: 5px 10px;"
        "  border: 1px solid palette(mid);"
        "}"
        "QToolButton#slSegment:checked {"
        "  background: palette(highlight);"
        "  color: palette(highlighted-text);"
        "  border-color: palette(highlight);"
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
        "  color: palette(mid);"
        "}");
}

void applyApplicationChrome(QApplication& app)
{
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

void applyPageMargins(QWidget* widget)
{
    if (widget == nullptr || widget->layout() == nullptr) {
        return;
    }
    widget->layout()->setContentsMargins(kUiSpace20, kUiSpace16, kUiSpace20,
                                         kUiSpace16);
    widget->layout()->setSpacing(kUiSpace12);
}

}  // namespace spacelens
