#pragma once

#include <QScrollArea>
#include <QString>

class QFormLayout;
class QLabel;
class QVBoxLayout;
class QWidget;

namespace spacelens {

/// Compact label/value inspector. Skips empty and no-value fields.
class PropertyInspector final : public QScrollArea {
    Q_OBJECT

public:
    explicit PropertyInspector(QWidget* parent = nullptr);

    void clear();
    void setHeading(const QString& title, const QString& summary = {});
    void addRow(const QString& label, const QString& value);
    void addNote(const QString& note);
    [[nodiscard]] QString toPlainText() const;
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

private:
    QWidget* m_host = nullptr;
    QVBoxLayout* m_root = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_summary = nullptr;
    QWidget* m_formHost = nullptr;
    QFormLayout* m_form = nullptr;
    QLabel* m_note = nullptr;
    QString m_plain;
};

}  // namespace spacelens
