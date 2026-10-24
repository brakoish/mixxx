// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QColor>
#include <QList>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QVariant>
#include <functional>

#include "decklab/decklabui.h"

class QComboBox;

// r4-system (Signature 1): Settings as a utility list you drive with the
// browse knob, like a Pioneer UTILITY screen. One row anatomy everywhere:
// the label on the left, the current value on the right (readout type, a
// switch with ON/OFF printed inside, or segments), the reason on a second
// line when the row is locked. Every value is visible without a tap. Turn
// moves the 2 px focus ring (KnobNavigator), press edits, turn changes, press
// sets, back restores. Rows are painted: no desktop combo boxes, no sliders.
namespace decklab::ui {

/// Base row: 52 px (64 px with a second line), panel fill, line separator.
class UtilityRow : public TouchButton, public KnobTarget {
    Q_OBJECT
  public:
    UtilityRow(const QString& label, QWidget* parent, const QString& objectName);
    QString label() const {
        return m_label;
    }
    void setLabel(const QString& label);
    /// Muted second line (what the setting means).
    void setHint(const QString& hint);
    QString hint() const {
        return m_hint;
    }
    /// Locked: the label dims and the reason replaces the hint; input is ignored.
    void setReason(const QString& reason);
    QString reason() const {
        return m_reason;
    }
    /// Deck-colour badge before the label (deck rows).
    void setBadge(const QColor& color, const QString& text);
    /// Printed on the second line while the row is disabled from outside.
    void setDisabledHint(const QString& hint);
    bool knobPress() override;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;
    QString secondLine() const;
    /// Value area at the right of the first line.
    QRect valueRect() const;
    virtual int valueWidth() const;
    virtual void paintValue(QPainter& painter, const QRect& area) = 0;
    bool knobEditing() const {
        return m_knobEditing;
    }
    void setKnobEditing(bool editing);
    void updateHeight();

    QString m_label;
    QString m_hint;
    QString m_reason;
    QString m_disabledHint;
    QColor m_badge;
    QString m_badgeText;
    bool m_knobEditing = false;
};

/// ON/OFF switch printed inside the control. Checkable: a tap, or a knob
/// press, flips it and emits clicked(bool).
class SwitchRow : public UtilityRow {
    Q_OBJECT
  public:
    SwitchRow(const QString& label, QWidget* parent, const QString& objectName);
    /// "ON" or "OFF", as printed.
    QString valueText() const;
    bool knobPress() override;

  protected:
    int valueWidth() const override;
    void paintValue(QPainter& painter, const QRect& area) override;
};

/// A choice from a short list: 2-4 segments, or a value that cycles with the
/// knob and opens a picker sheet on a tap when there are more. The options
/// live in a hidden QComboBox (model()), so code that drives the old combo box
/// (findChild + activated) keeps working; a choice emits model()->activated.
class ChoiceRow : public UtilityRow {
    Q_OBJECT
  public:
    ChoiceRow(const QString& label, QWidget* parent, const QString& objectName, const QString& modelName);
    QComboBox* model() const {
        return m_model;
    }
    /// fullText is the model's item text; segment is what the row prints.
    void addOption(const QString& segment, const QString& fullText, const QVariant& data);
    void setSegmentText(int index, const QString& segment);
    QString segmentText(int index) const;
    /// Deck colours for a deck selector: the chosen segment fills with it.
    void setSegmentColors(const QList<QColor>& colors) {
        m_colors = colors;
        update();
    }
    /// Hide options beyond `count` (decks 3 and 4 in the two-deck layout).
    void setVisibleCount(int count);
    int currentIndex() const;
    /// The option the row shows (the knob's pending one while editing).
    int shownIndex() const;
    void choose(int index);
    bool segmented() const;
    bool knobPress() override;
    void knobTurn(int steps) override;
    void knobCommit() override;
    void knobCancel() override;

  protected:
    int valueWidth() const override;
    void paintValue(QPainter& painter, const QRect& area) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

  private:
    int optionCount() const;
    int segmentsWidth() const;
    QRect segmentRect(int index, const QRect& area) const;

    QComboBox* m_model;
    QList<QColor> m_colors;
    int m_visibleCount = -1;
    int m_pending = -1;
};

/// A number with drawn − and + (44 x 44 each) and a readout between them.
/// Held − or + repeats; a double tap on the readout restores the default.
class StepperRow : public UtilityRow {
    Q_OBJECT
  public:
    StepperRow(const QString& label, QWidget* parent, const QString& objectName);
    void setRange(int minimum, int maximum, int step);
    void setDefault(int value) {
        m_default = value;
    }
    void setFormat(std::function<QString(int)> format);
    /// Programmatic (a control changed): no changed() signal.
    void setValue(int value);
    int value() const {
        return m_value;
    }
    QString valueText() const;
    void stepBy(int steps);
    bool knobPress() override;
    void knobTurn(int steps) override;
    void knobCommit() override;
    void knobCancel() override;

  signals:
    /// The DJ changed it (tap, hold or knob).
    void changed(int value);

  protected:
    int valueWidth() const override;
    void paintValue(QPainter& painter, const QRect& area) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    int hit(const QPoint& point) const;

    std::function<QString(int)> m_format;
    QTimer m_repeat;
    int m_value = 0;
    int m_minimum = 0;
    int m_maximum = 100;
    int m_step = 1;
    int m_default = 0;
    int m_before = 0;
    int m_pressed = 0; // -1 minus, +1 plus
    int m_repeats = 0;
};

/// Navigates or launches: value text and a drawn chevron. A plain click.
class ActionRow : public UtilityRow {
    Q_OBJECT
  public:
    ActionRow(const QString& label, QWidget* parent, const QString& objectName);
    void setValue(const QString& value);
    QString value() const {
        return m_value;
    }
    void setLamp(Lamp lamp, bool shown = true);
    Lamp lamp() const {
        return m_lamp;
    }
    bool knobPress() override;

  protected:
    int valueWidth() const override;
    void paintValue(QPainter& painter, const QRect& area) override;

  private:
    QString m_value;
    Lamp m_lamp = Lamp::Off;
    bool m_hasLamp = false;
};

/// Read-only value with a lamp (Health). No press state; the knob skips it.
class InfoRow : public ActionRow {
    Q_OBJECT
  public:
    InfoRow(const QString& label, QWidget* parent, const QString& objectName);

  protected:
    void paintValue(QPainter& painter, const QRect& area) override;
};

/// "PER DECK" in 14 px caps with a line to the right edge (TE).
class SectionRule : public QWidget {
    Q_OBJECT
  public:
    SectionRule(const QString& text, QWidget* parent);
    QString text() const {
        return m_text;
    }

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QString m_text;
};

/// Settings category list item: pictogram, label, live value, a lamp.
class CategoryButton : public TouchButton {
    Q_OBJECT
  public:
    CategoryButton(Icon icon, const QString& label, QWidget* parent, const QString& objectName);
    void setValue(const QString& value, Lamp lamp = Lamp::Off, bool lampShown = false);
    QString value() const {
        return m_value;
    }

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    Icon m_icon;
    QString m_label;
    QString m_value;
    Lamp m_lamp = Lamp::Off;
    bool m_lampShown = false;
};

/// Bottom sheet listing options with a check on the current one. Tap or
/// knob press chooses; back closes. Hides the GL waveforms while shown.
class PickerSheet : public QWidget {
    Q_OBJECT
  public:
    static PickerSheet* open(QWidget* host,
            const QString& title,
            const QStringList& options,
            int current,
            std::function<void(int)> chosen);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

  private:
    explicit PickerSheet(QWidget* host);
};

} // namespace decklab::ui
