#include "TestRunner.hpp"

#include "ui/FilterPopup.hpp"
#include "ui/MainWindow.hpp"
#include "ui/PropertyInspector.hpp"
#include "ui/TreemapWidget.hpp"
#include "ui/UiTheme.hpp"

#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyle>
#include <QWidget>

#include <cstring>
#include <string>

using namespace spacelens;

namespace {

QApplication& qtApp()
{
    static int argc = 1;
    static char arg0[] = "spacelens_gui_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
    applyApplicationChrome(app);
    return app;
}

QPushButton* navButton(QWidget& root, const char* id)
{
    const auto buttons = root.findChildren<QPushButton*>();
    for (QPushButton* button : buttons) {
        if (button->property("slId").toString() == QLatin1String(id)) {
            return button;
        }
    }
    return nullptr;
}

}  // namespace

SPACELENS_TEST(GuiUx_stylesheet_uses_palette_roles)
{
    qtApp();
    const QString sheet = uiStyleSheet();
    SPACELENS_REQUIRE(sheet.contains(QStringLiteral("palette(highlight)")));
    SPACELENS_REQUIRE(sheet.contains(QStringLiteral("palette(mid)")));
    SPACELENS_REQUIRE(!sheet.contains(QStringLiteral("#F4F5F7")));
    SPACELENS_REQUIRE(!sheet.contains(QStringLiteral("#666")));
}

SPACELENS_TEST(GuiUx_theme_helpers_follow_palette)
{
    qtApp();
    QPalette light;
    light.setColor(QPalette::Window, QColor(244, 245, 247));
    light.setColor(QPalette::WindowText, QColor(26, 26, 26));
    light.setColor(QPalette::Base, QColor(255, 255, 255));
    SPACELENS_REQUIRE(!paletteIsDark(light));
    SPACELENS_REQUIRE(contrastingTextColor(QColor(244, 245, 247)).lightness() < 80);

    QPalette dark;
    dark.setColor(QPalette::Window, QColor(32, 32, 32));
    dark.setColor(QPalette::WindowText, QColor(240, 240, 240));
    dark.setColor(QPalette::Base, QColor(20, 20, 20));
    SPACELENS_REQUIRE(paletteIsDark(dark));
    const QColor adapted =
        adjustClassificationFill(QColor(0x5B, 0x8F, 0xF9), true);
    SPACELENS_REQUIRE(adapted != QColor(0x5B, 0x8F, 0xF9));
    SPACELENS_REQUIRE(contrastingTextColor(QColor(20, 20, 20)).lightness() > 180);
}

SPACELENS_TEST(GuiUx_filter_button_count_label)
{
    qtApp();
    FilterButton button;
    SPACELENS_REQUIRE_EQ(button.text().toStdString(), std::string("Filters"));
    button.setActiveCount(0);
    SPACELENS_REQUIRE_EQ(button.text().toStdString(), std::string("Filters"));
    button.setActiveCount(3);
    SPACELENS_REQUIRE_EQ(button.text().toStdString(), std::string("Filters 3"));
    SPACELENS_REQUIRE_EQ(button.activeCount(), 3);
}

SPACELENS_TEST(GuiUx_treemap_keeps_system_palette_background)
{
    qtApp();
    TreemapWidget widget;
    QPalette pal = widget.palette();
    pal.setColor(QPalette::Window, QColor(18, 18, 18));
    pal.setColor(QPalette::Base, QColor(12, 12, 12));
    pal.setColor(QPalette::WindowText, QColor(240, 240, 240));
    widget.setPalette(pal);
    const QColor window = widget.palette().color(QPalette::Window);
    const QColor base = widget.palette().color(QPalette::Base);
    SPACELENS_REQUIRE(window == QColor(18, 18, 18));
    SPACELENS_REQUIRE(base == QColor(12, 12, 12));
    SPACELENS_REQUIRE(window != QColor(0xF4, 0xF5, 0xF7));
}

SPACELENS_TEST(GuiUx_noise_values_are_skipped)
{
    qtApp();
    SPACELENS_REQUIRE(isNoiseDisplayValue(QString()));
    SPACELENS_REQUIRE(isNoiseDisplayValue(QStringLiteral("Unknown")));
    SPACELENS_REQUIRE(isNoiseDisplayValue(QStringLiteral("none")));
    SPACELENS_REQUIRE(isNoiseDisplayValue(QStringLiteral("(none)")));
    SPACELENS_REQUIRE(isNoiseDisplayValue(QStringLiteral("0x0")));
    SPACELENS_REQUIRE(!isNoiseDisplayValue(QStringLiteral("Protected")));
    SPACELENS_REQUIRE(!isNoiseDisplayValue(QStringLiteral("UserData")));
    SPACELENS_REQUIRE_EQ(displayFolderName(QStringLiteral("C:/Users/demo/Downloads"))
                             .toStdString(),
                         std::string("Downloads"));
    const QString drive = displayFolderName(QStringLiteral("D:/"));
    SPACELENS_REQUIRE(drive.contains(QStringLiteral("D:")));
    SPACELENS_REQUIRE(!drive.contains(QStringLiteral("Downloads")));

    PropertyInspector inspector;
    inspector.setHeading(QStringLiteral("demo.bin"), QStringLiteral("12 MB"));
    inspector.addRow(QStringLiteral("Class"), QStringLiteral("Unknown"));
    inspector.addRow(QStringLiteral("Path"), QStringLiteral("C:/tmp/demo.bin"));
    const QString plain = inspector.toPlainText();
    SPACELENS_REQUIRE(plain.contains(QStringLiteral("demo.bin")));
    SPACELENS_REQUIRE(plain.contains(QStringLiteral("C:/tmp/demo.bin")));
    SPACELENS_REQUIRE(!plain.contains(QStringLiteral("Unknown")));
}

SPACELENS_TEST(GuiUx_muted_text_stays_readable)
{
    qtApp();
    QPalette light;
    light.setColor(QPalette::Window, QColor(243, 243, 243));
    light.setColor(QPalette::WindowText, QColor(26, 26, 26));
    light.setColor(QPalette::Mid, QColor(180, 180, 180));
    const QColor muted = mutedTextColor(light);
    const int textL = light.color(QPalette::WindowText).lightness();
    const int mutedDelta = muted.lightness() > textL ? muted.lightness() - textL
                                                    : textL - muted.lightness();
    const int midL = light.color(QPalette::Mid).lightness();
    const int midDelta = midL > textL ? midL - textL : textL - midL;
    SPACELENS_REQUIRE(mutedDelta < midDelta);
    SPACELENS_REQUIRE(muted.lightness() < 120);
}

SPACELENS_TEST(GuiUx_cleanup_confirm_cta_wording)
{
    qtApp();
    const std::string primary(kCleanupConfirmPrimary);
    SPACELENS_REQUIRE(primary == "Move eligible files to Recycle Bin");
    SPACELENS_REQUIRE(primary.find("Clean") == std::string::npos);
    SPACELENS_REQUIRE(primary.find("Delete") == std::string::npos);
    SPACELENS_REQUIRE(primary.find("Free space") == std::string::npos);
    const std::string title(kCleanupConfirmTitle);
    SPACELENS_REQUIRE(title == "Move to Recycle Bin");
}

SPACELENS_TEST(GuiUx_workspace_switch_and_shortcuts)
{
    qtApp();
    MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen, true);
    auto* pages = window.findChild<QStackedWidget*>(QStringLiteral("slPages"));
    auto* live = navButton(window, "live");
    auto* indexed = navButton(window, "indexed");
    SPACELENS_REQUIRE(pages != nullptr);
    SPACELENS_REQUIRE(live != nullptr);
    SPACELENS_REQUIRE(indexed != nullptr);
    SPACELENS_REQUIRE(live->isChecked());
    SPACELENS_REQUIRE(!indexed->isChecked());
    SPACELENS_REQUIRE_EQ(pages->currentIndex(), 0);

    indexed->click();
    SPACELENS_REQUIRE(indexed->isChecked());
    SPACELENS_REQUIRE(!live->isChecked());
    SPACELENS_REQUIRE_EQ(pages->currentIndex(), 1);

    live->click();
    SPACELENS_REQUIRE(live->isChecked());
    SPACELENS_REQUIRE_EQ(pages->currentIndex(), 0);
}

SPACELENS_TEST(GuiUx_live_filter_count_and_command_enablement)
{
    qtApp();
    MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen, true);

    QPushButton* scan = nullptr;
    for (QPushButton* button : window.findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("Scan") &&
            button->objectName() == QStringLiteral("slPrimary")) {
            scan = button;
            break;
        }
    }
    auto* filter = window.findChild<FilterButton*>(QStringLiteral("slLiveFilter"));
    auto* kind = window.findChild<QComboBox*>(QStringLiteral("slLiveKind"));
    auto* ext = window.findChild<QLineEdit*>(QStringLiteral("slLiveExt"));
    auto* minSize = window.findChild<QLineEdit*>(QStringLiteral("slLiveMinSize"));
    SPACELENS_REQUIRE(scan != nullptr);
    SPACELENS_REQUIRE(filter != nullptr);
    SPACELENS_REQUIRE(kind != nullptr);
    SPACELENS_REQUIRE(ext != nullptr);
    SPACELENS_REQUIRE(minSize != nullptr);

    SPACELENS_REQUIRE(!scan->isEnabled());
    SPACELENS_REQUIRE_EQ(filter->activeCount(), 0);
    SPACELENS_REQUIRE_EQ(filter->text().toStdString(), std::string("Filters"));

    kind->setCurrentIndex(1);
    SPACELENS_REQUIRE_EQ(filter->activeCount(), 1);
    ext->setText(QStringLiteral("gguf"));
    SPACELENS_REQUIRE_EQ(filter->activeCount(), 2);
    minSize->setText(QStringLiteral("100MB"));
    SPACELENS_REQUIRE_EQ(filter->activeCount(), 3);
    SPACELENS_REQUIRE_EQ(filter->text().toStdString(), std::string("Filters 3"));

    QPushButton* explorer = nullptr;
    QPushButton* review = nullptr;
    for (QPushButton* button : window.findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("Show in Explorer")) {
            explorer = button;
        }
        if (button->text() == QStringLiteral("Cleanup Review")) {
            review = button;
        }
    }
    SPACELENS_REQUIRE(explorer != nullptr);
    SPACELENS_REQUIRE(review != nullptr);
    SPACELENS_REQUIRE(!explorer->isEnabled());
    SPACELENS_REQUIRE(review->isEnabled());
}
