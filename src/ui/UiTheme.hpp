#pragma once

#include <QColor>
#include <QPalette>
#include <QString>

#include <cstdint>

class QApplication;
class QPushButton;
class QWidget;

namespace spacelens {

inline constexpr int kUiSpace4 = 4;
inline constexpr int kUiSpace8 = 8;
inline constexpr int kUiSpace12 = 12;
inline constexpr int kUiSpace16 = 16;
inline constexpr int kUiSpace20 = 20;
inline constexpr int kUiSpace24 = 24;
inline constexpr int kUiRootPaneMin = 260;
inline constexpr int kUiRootPaneHint = 300;
inline constexpr int kUiRootRowHeight = 60;

inline constexpr const char* kCleanupConfirmPrimary =
    "Move eligible files to Recycle Bin";
inline constexpr const char* kCleanupConfirmTitle = "Move to Recycle Bin";

[[nodiscard]] bool paletteIsDark(const QPalette& palette);
[[nodiscard]] QString formatFileTimeLocal(std::uint64_t ticks);
[[nodiscard]] QColor mutedTextColor(const QPalette& palette);
[[nodiscard]] QColor subtleFillColor(const QPalette& palette);
[[nodiscard]] QColor hairlineColor(const QPalette& palette);
[[nodiscard]] QColor contrastingTextColor(const QColor& fill);
[[nodiscard]] QColor adjustClassificationFill(const QColor& fill, bool dark);

/// Targeted stylesheet. Uses palette() roles so light/dark follow the system.
[[nodiscard]] QString uiStyleSheet();

void applyApplicationChrome(QApplication& app);
void markPrimaryButton(QPushButton* button);
void applyPageMargins(QWidget* widget);

}  // namespace spacelens
