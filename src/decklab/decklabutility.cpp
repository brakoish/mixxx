// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklabutility.h"

#include <QBoxLayout>
#include <QComboBox>
#include <QEvent>
#include <QFontMetrics>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <algorithm>
#include <utility>

#include "decklab/decklaboverlay.h"
#include "moc_decklabutility.cpp"

namespace decklab::ui {

namespace {
constexpr int kInset = 16;
constexpr int kSegmentRole = Qt::UserRole + 41;

QColor inkOn(const QColor& fill) {
    // Ink on light fills (white, deck colours), text on dark ones.
    return fill.lightnessF() > 0.45 ? QColor(kGround) : QColor(kText);
}

void drawElided(QPainter& painter, const QRect& rect, int flags, const QString& text) {
    painter.drawText(rect, flags, QFontMetrics(painter.font()).elidedText(text, Qt::ElideRight, std::max(0, rect.width())));
}
} // namespace

// ---------------------------------------------------------------- rows --

UtilityRow::UtilityRow(const QString& label, QWidget* parent, const QString& objectName)
        : TouchButton(label, parent, 52),
          m_label(label) {
    setObjectName(objectName);
    setAccessibleName(label);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void UtilityRow::setLabel(const QString& label) {
    if (m_label == label) {
        return;
    }
    m_label = label;
    setText(label);
    setAccessibleName(label);
    update();
}

void UtilityRow::setHint(const QString& hint) {
    if (m_hint == hint) {
        return;
    }
    m_hint = hint;
    updateHeight();
    update();
}

void UtilityRow::setReason(const QString& reason) {
    if (m_reason == reason) {
        return;
    }
    m_reason = reason;
    // Never disabled without a reason, and the reason sits next to it.
    setEnabled(reason.isEmpty());
    setAccessibleDescription(reason);
    updateHeight();
    update();
}

void UtilityRow::setBadge(const QColor& color, const QString& text) {
    m_badge = color;
    m_badgeText = text;
    update();
}

void UtilityRow::setDisabledHint(const QString& hint) {
    m_disabledHint = hint;
    updateHeight();
    update();
}

QString UtilityRow::secondLine() const {
    if (!m_reason.isEmpty()) {
        return m_reason;
    }
    if (!isEnabled() && !m_disabledHint.isEmpty()) {
        return m_disabledHint;
    }
    return m_hint;
}

void UtilityRow::changeEvent(QEvent* event) {
    TouchButton::changeEvent(event);
    if (event->type() == QEvent::EnabledChange && !m_disabledHint.isEmpty()) {
        updateHeight();
        update();
    }
}

void UtilityRow::updateHeight() {
    setTouchHeight(secondLine().isEmpty() ? 52 : 64);
}

bool UtilityRow::knobPress() {
    click();
    return false;
}

void UtilityRow::setKnobEditing(bool editing) {
    if (m_knobEditing != editing) {
        m_knobEditing = editing;
        update();
    }
}

int UtilityRow::valueWidth() const {
    return 160;
}

QRect UtilityRow::valueRect() const {
    const int width = std::min(valueWidth(), std::max(0, this->width() - 2 * kInset - 120));
    return QRect(this->width() - kInset - width, 0, width, height());
}

void UtilityRow::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool down = isDown() && !testAttribute(Qt::WA_TransparentForMouseEvents);
    painter.fillRect(rect(), QColor(down ? kRaise : kPanel));
    painter.fillRect(QRect(0, height() - 1, width(), 1), QColor(kLine));
    int left = kInset;
    if (m_badge.isValid()) {
        const QRectF badge(left, (height() - 24) / 2.0, 28, 24);
        painter.setPen(Qt::NoPen);
        painter.setBrush(m_badge);
        painter.drawRoundedRect(badge, 3, 3);
        painter.setFont(numberFont(font(), 16));
        painter.setPen(inkOn(m_badge));
        painter.drawText(badge, Qt::AlignCenter, m_badgeText);
        left += 36;
    }
    const QRect value = valueRect();
    const int textWidth = std::max(0, value.left() - left - 12);
    const QString second = secondLine();
    painter.setFont(uiFont(font(), 16, QFont::Medium));
    painter.setPen(QColor(isEnabled() ? kText : kDim));
    if (second.isEmpty()) {
        drawElided(painter, QRect(left, 0, textWidth, height()), Qt::AlignLeft | Qt::AlignVCenter, m_label);
    } else {
        drawElided(painter, QRect(left, 9, textWidth, 22), Qt::AlignLeft | Qt::AlignVCenter, m_label);
        painter.setFont(uiFont(font(), 15, QFont::Medium));
        // The reason is the thing to read: text colour, not muted.
        painter.setPen(QColor(second == m_hint ? kMuted : kText));
        // Never under the value: the second line ends where the value starts.
        drawElided(painter, QRect(left, 33, std::max(0, value.left() - left - 12), 22),
                Qt::AlignLeft | Qt::AlignVCenter, second);
    }
    paintValue(painter, value);
}

// -------------------------------------------------------------- switch --

SwitchRow::SwitchRow(const QString& label, QWidget* parent, const QString& objectName)
        : UtilityRow(label, parent, objectName) {
    setCheckable(true);
}

QString SwitchRow::valueText() const {
    return isChecked() ? tr("ON") : tr("OFF");
}

bool SwitchRow::knobPress() {
    click(); // toggles and emits clicked(bool)
    return false;
}

int SwitchRow::valueWidth() const {
    return 72;
}

void SwitchRow::paintValue(QPainter& painter, const QRect& area) {
    const QRectF pill(area.right() - 64 + 1, (height() - 32) / 2.0, 64, 32);
    const bool on = isChecked();
    painter.setPen(on ? Qt::NoPen : QPen(QColor(kLine), 1.5));
    painter.setBrush(on ? QColor(isEnabled() ? kText : kDim) : QColor(kGround));
    painter.drawRoundedRect(pill.adjusted(0.75, 0.75, -0.75, -0.75), 4, 4);
    painter.setFont(capsFont(font(), 14));
    painter.setPen(on ? QColor(kGround) : QColor(isEnabled() ? kMuted : kDim));
    painter.drawText(pill, Qt::AlignCenter, valueText());
}

// -------------------------------------------------------------- choice --

ChoiceRow::ChoiceRow(const QString& label, QWidget* parent, const QString& objectName, const QString& modelName)
        : UtilityRow(label, parent, objectName),
          m_model(new QComboBox(this)) {
    // The model is never shown: rows print the value, a tap or the knob
    // chooses. It keeps the old combo box's object name, items and signals.
    m_model->setObjectName(modelName);
    m_model->setAccessibleName(label);
    m_model->setFocusPolicy(Qt::NoFocus);
    m_model->hide();
    connect(m_model, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        if (!knobEditing()) {
            m_pending = -1;
        }
        update();
    });
    connect(m_model->model(), &QAbstractItemModel::rowsInserted, this, [this] {
        updateGeometry();
        update();
    });
    connect(m_model->model(), &QAbstractItemModel::rowsRemoved, this, [this] {
        updateGeometry();
        update();
    });
}

void ChoiceRow::addOption(const QString& segment, const QString& fullText, const QVariant& data) {
    m_model->addItem(fullText, data);
    m_model->setItemData(m_model->count() - 1, segment, kSegmentRole);
    update();
}

void ChoiceRow::setSegmentText(int index, const QString& segment) {
    if (index < 0 || index >= m_model->count() || segmentText(index) == segment) {
        return;
    }
    m_model->setItemData(index, segment, kSegmentRole);
    update();
}

QString ChoiceRow::segmentText(int index) const {
    const QVariant segment = m_model->itemData(index, kSegmentRole);
    return segment.isValid() ? segment.toString() : m_model->itemText(index);
}

void ChoiceRow::setVisibleCount(int count) {
    if (m_visibleCount != count) {
        m_visibleCount = count;
        update();
    }
}

int ChoiceRow::optionCount() const {
    return m_visibleCount < 0 ? m_model->count() : std::min<int>(m_visibleCount, m_model->count());
}

int ChoiceRow::currentIndex() const {
    return m_model->currentIndex();
}

int ChoiceRow::shownIndex() const {
    return knobEditing() && m_pending >= 0 ? m_pending : m_model->currentIndex();
}

bool ChoiceRow::segmented() const {
    return optionCount() <= 4;
}

void ChoiceRow::choose(int index) {
    if (index < 0 || index >= optionCount() || !isEnabled()) {
        return;
    }
    m_model->setCurrentIndex(index);
    emit m_model->activated(index);
    update();
}

bool ChoiceRow::knobPress() {
    if (!isEnabled() || optionCount() < 2) {
        return false;
    }
    m_pending = m_model->currentIndex();
    setKnobEditing(true);
    return true;
}

void ChoiceRow::knobTurn(int steps) {
    if (!knobEditing()) {
        return;
    }
    m_pending = std::clamp(std::max(0, m_pending) + steps, 0, optionCount() - 1);
    update();
}

void ChoiceRow::knobCommit() {
    const int pending = m_pending;
    setKnobEditing(false);
    m_pending = -1;
    if (pending >= 0 && pending != m_model->currentIndex()) {
        choose(pending);
    }
    update();
}

void ChoiceRow::knobCancel() {
    setKnobEditing(false);
    m_pending = -1;
    update();
}

int ChoiceRow::valueWidth() const {
    if (!segmented()) {
        const QFontMetrics metrics(uiFont(font(), 16, QFont::DemiBold));
        int widest = 0;
        for (int index = 0; index < optionCount(); ++index) {
            widest = std::max(widest, metrics.horizontalAdvance(segmentText(index)));
        }
        return widest + 36;
    }
    const QFontMetrics metrics(capsFont(font(), 14));
    int width = 0;
    for (int index = 0; index < optionCount(); ++index) {
        width += std::max(64, metrics.horizontalAdvance(segmentText(index)) + 28) + (index ? 4 : 0);
    }
    return width;
}

QRect ChoiceRow::segmentRect(int index, const QRect& area) const {
    const QFontMetrics metrics(capsFont(font(), 14));
    int total = 0;
    QList<int> widths;
    for (int n = 0; n < optionCount(); ++n) {
        widths.append(std::max(64, metrics.horizontalAdvance(segmentText(n)) + 28));
        total += widths.last() + (n ? 4 : 0);
    }
    // Squeeze evenly if the row is narrower than the natural width.
    const double scale = total > area.width() && total > 0 ? area.width() / static_cast<double>(total) : 1.0;
    int x = area.right() + 1 - static_cast<int>(total * scale);
    for (int n = 0; n < index; ++n) {
        x += static_cast<int>((widths.at(n) + 4) * scale);
    }
    return QRect(x, (height() - 44) / 2, static_cast<int>(widths.value(index) * scale), 44);
}

void ChoiceRow::paintValue(QPainter& painter, const QRect& area) {
    const int shown = shownIndex();
    if (!segmented()) {
        painter.setFont(uiFont(font(), 16, QFont::DemiBold));
        painter.setPen(QColor(isEnabled() ? kText : kDim));
        drawElided(painter, area.adjusted(0, 0, -28, 0), Qt::AlignRight | Qt::AlignVCenter, segmentText(shown));
        paintGlyph(painter, QRectF(area.right() - 20, (height() - 20) / 2.0, 20, 20), Glyph::Chevron, QColor(kMuted));
        if (knobEditing()) {
            painter.fillRect(QRect(area.left(), height() / 2 + 13, area.width() - 28, 2), QColor(kText));
        }
        return;
    }
    const QFont caps = capsFont(font(), 14);
    painter.setFont(caps);
    for (int index = 0; index < optionCount(); ++index) {
        const QRectF segment = QRectF(segmentRect(index, area)).adjusted(0.5, 0.5, -0.5, -0.5);
        const bool selected = index == m_model->currentIndex();
        const QColor deck = m_colors.value(index);
        QColor fill = selected ? (deck.isValid() ? deck : QColor(kText)) : QColor(kGround);
        if (!isEnabled() && selected) {
            fill = QColor(kDim);
        }
        painter.setPen(selected ? Qt::NoPen : QPen(deck.isValid() ? deck : QColor(kLine), 1));
        painter.setBrush(fill);
        painter.drawRoundedRect(segment, 4, 4);
        if (knobEditing() && index == shown) {
            // The knob's pending choice: a white ring until the press sets it.
            painter.setPen(QPen(QColor(kText), 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(segment.adjusted(-3, -3, 3, 3), 5, 5);
        }
        painter.setPen(selected ? inkOn(fill) : QColor(isEnabled() ? kText : kDim));
        painter.drawText(segment, Qt::AlignCenter, segmentText(index));
    }
}

void ChoiceRow::mouseReleaseEvent(QMouseEvent* event) {
    const bool inside = rect().contains(event->position().toPoint());
    const bool wasDown = isDown();
    UtilityRow::mouseReleaseEvent(event);
    if (!inside || !wasDown || !isEnabled() || event->button() != Qt::LeftButton) {
        return;
    }
    if (!segmented()) {
        QStringList options;
        for (int index = 0; index < optionCount(); ++index) {
            options.append(segmentText(index));
        }
        QPointer<ChoiceRow> self(this);
        PickerSheet::open(window(), m_label, options, m_model->currentIndex(), [self](int index) {
            if (self) {
                self->choose(index);
            }
        });
        return;
    }
    const QRect area = valueRect();
    for (int index = 0; index < optionCount(); ++index) {
        if (segmentRect(index, area).adjusted(-2, -4, 2, 4).contains(event->position().toPoint())) {
            choose(index);
            return;
        }
    }
}

// ------------------------------------------------------------- stepper --

StepperRow::StepperRow(const QString& label, QWidget* parent, const QString& objectName)
        : UtilityRow(label, parent, objectName) {
    connect(&m_repeat, &QTimer::timeout, this, [this] {
        if (m_pressed != 0) {
            stepBy(m_pressed);
            ++m_repeats;
            m_repeat.setInterval(140);
        }
    });
}

void StepperRow::setRange(int minimum, int maximum, int step) {
    m_minimum = minimum;
    m_maximum = std::max(minimum, maximum);
    m_step = std::max(1, step);
    m_value = std::clamp(m_value, m_minimum, m_maximum);
    update();
}

void StepperRow::setFormat(std::function<QString(int)> format) {
    m_format = std::move(format);
    update();
}

void StepperRow::setValue(int value) {
    value = std::clamp(value, m_minimum, m_maximum);
    if (m_value != value) {
        m_value = value;
        setAccessibleDescription(valueText());
        update();
    }
}

QString StepperRow::valueText() const {
    return m_format ? m_format(m_value) : QString::number(m_value);
}

void StepperRow::stepBy(int steps) {
    // Snap to the step grid (a 5 % floor never yields 15, 25...).
    const int base = steps > 0 ? (m_value / m_step) * m_step : ((m_value + m_step - 1) / m_step) * m_step;
    const int next = std::clamp(base + steps * m_step, m_minimum, m_maximum);
    if (next == m_value) {
        return;
    }
    m_value = next;
    setAccessibleDescription(valueText());
    update();
    emit changed(m_value);
}

bool StepperRow::knobPress() {
    if (!isEnabled()) {
        return false;
    }
    m_before = m_value;
    setKnobEditing(true);
    return true;
}

void StepperRow::knobTurn(int steps) {
    if (knobEditing()) {
        stepBy(steps);
    }
}

void StepperRow::knobCommit() {
    setKnobEditing(false);
}

void StepperRow::knobCancel() {
    setKnobEditing(false);
    if (m_value != m_before) {
        m_value = m_before;
        update();
        emit changed(m_value);
    }
}

int StepperRow::valueWidth() const {
    return 204;
}

int StepperRow::hit(const QPoint& point) const {
    const QRect area = valueRect();
    if (QRect(area.left(), 0, 52, height()).contains(point)) {
        return -1;
    }
    if (QRect(area.right() - 51, 0, 52, height()).contains(point)) {
        return 1;
    }
    return 0;
}

void StepperRow::paintValue(QPainter& painter, const QRect& area) {
    const auto button = [&](const QRect& box, Glyph glyph, bool limit, bool pressed) {
        const QRectF frame = QRectF(box).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setPen(QColor(pressed ? kText : kLine));
        painter.setBrush(QColor(pressed ? kRaise : kGround));
        painter.drawRoundedRect(frame, 4, 4);
        // At a limit the button dims.
        paintGlyph(painter, QRectF(box.center().x() - 11, box.center().y() - 11, 22, 22), glyph,
                QColor(!isEnabled() || limit ? kDim : kText));
    };
    const int top = (height() - 44) / 2;
    button(QRect(area.left(), top, 44, 44), Glyph::Minus, m_value <= m_minimum, m_pressed < 0);
    button(QRect(area.right() - 43, top, 44, 44), Glyph::Plus, m_value >= m_maximum, m_pressed > 0);
    const QRect readout(area.left() + 48, 0, area.width() - 96, height());
    painter.setFont(numberFont(font(), 24));
    painter.setPen(QColor(isEnabled() ? kText : kDim));
    drawElided(painter, readout, Qt::AlignCenter, valueText());
    if (knobEditing()) {
        painter.fillRect(QRect(readout.left() + 8, height() / 2 + 15, readout.width() - 16, 2), QColor(kText));
    }
}

void StepperRow::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !isEnabled()) {
        UtilityRow::mousePressEvent(event);
        return;
    }
    m_pressed = hit(event->position().toPoint());
    if (m_pressed != 0) {
        // Steps on release (a swipe never steps); holding repeats.
        m_repeats = 0;
        m_repeat.setInterval(450);
        m_repeat.start();
        update();
        event->accept();
        return;
    }
    UtilityRow::mousePressEvent(event);
}

void StepperRow::mouseReleaseEvent(QMouseEvent* event) {
    if (m_pressed != 0) {
        const bool repeated = m_repeats > 0;
        const int direction = m_pressed;
        m_repeat.stop();
        m_pressed = 0;
        if (!repeated && hit(event->position().toPoint()) == direction) {
            stepBy(direction);
        }
        update();
        event->accept();
        return;
    }
    UtilityRow::mouseReleaseEvent(event);
}

void StepperRow::mouseDoubleClickEvent(QMouseEvent* event) {
    if (hit(event->position().toPoint()) == 0 && valueRect().contains(event->position().toPoint()) && isEnabled() &&
            m_value != m_default) {
        m_value = std::clamp(m_default, m_minimum, m_maximum);
        update();
        emit changed(m_value);
        event->accept();
        return;
    }
    UtilityRow::mouseDoubleClickEvent(event);
}

void StepperRow::hideEvent(QHideEvent* event) {
    m_repeat.stop();
    m_pressed = 0;
    UtilityRow::hideEvent(event);
}

// -------------------------------------------------------------- action --

ActionRow::ActionRow(const QString& label, QWidget* parent, const QString& objectName)
        : UtilityRow(label, parent, objectName) {
}

void ActionRow::setValue(const QString& value) {
    if (m_value != value) {
        m_value = value;
        updateGeometry();
        update();
    }
}

void ActionRow::setLamp(Lamp lamp, bool shown) {
    if (m_lamp != lamp || m_hasLamp != shown) {
        m_lamp = lamp;
        m_hasLamp = shown;
        update();
    }
}

bool ActionRow::knobPress() {
    click();
    return false;
}

int ActionRow::valueWidth() const {
    const QFontMetrics metrics(uiFont(font(), 16, QFont::DemiBold));
    const int text = m_value.isEmpty() ? 0 : metrics.horizontalAdvance(m_value) + 8;
    return std::min(text + (m_hasLamp ? 18 : 0) + 28, std::max(120, width() / 2));
}

void ActionRow::paintValue(QPainter& painter, const QRect& area) {
    paintGlyph(painter, QRectF(area.right() - 20, (height() - 20) / 2.0, 20, 20), Glyph::Chevron,
            QColor(isEnabled() ? kMuted : kDim));
    const QRect text = area.adjusted(m_hasLamp ? 18 : 0, 0, -28, 0);
    painter.setFont(uiFont(font(), 16, QFont::DemiBold));
    painter.setPen(QColor(isEnabled() ? kText : kDim));
    const QString shown = QFontMetrics(painter.font()).elidedText(m_value, Qt::ElideRight, std::max(0, text.width()));
    painter.drawText(text, Qt::AlignRight | Qt::AlignVCenter, shown);
    if (m_hasLamp) {
        const int textWidth = QFontMetrics(painter.font()).horizontalAdvance(shown);
        paintLamp(painter, QPointF(text.right() - textWidth - 10, height() / 2.0), 4, m_lamp);
    }
}

InfoRow::InfoRow(const QString& label, QWidget* parent, const QString& objectName)
        : ActionRow(label, parent, objectName) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
}

void InfoRow::paintValue(QPainter& painter, const QRect& area) {
    // Read-only: the value and its lamp, no chevron.
    painter.setFont(uiFont(font(), 16, QFont::DemiBold));
    painter.setPen(QColor(kText));
    const QString shown = QFontMetrics(painter.font()).elidedText(value(), Qt::ElideRight, std::max(0, area.width() - 18));
    painter.drawText(area, Qt::AlignRight | Qt::AlignVCenter, shown);
    const int textWidth = QFontMetrics(painter.font()).horizontalAdvance(shown);
    paintLamp(painter, QPointF(area.right() - textWidth - 10, height() / 2.0), 4, lamp());
}

// ------------------------------------------------------------- section --

SectionRule::SectionRule(const QString& text, QWidget* parent)
        : QWidget(parent),
          m_text(text) {
    setFixedHeight(34);
    setAccessibleName(text);
}

void SectionRule::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const QFont caps = capsFont(font(), 14);
    painter.setFont(caps);
    painter.setPen(QColor(kMuted));
    const QRect text(kInset, 12, width() - 2 * kInset, 20);
    painter.drawText(text, Qt::AlignLeft | Qt::AlignVCenter, m_text);
    const int end = kInset + QFontMetrics(caps).horizontalAdvance(m_text) + 12;
    if (end < width() - kInset) {
        painter.fillRect(QRect(end, text.center().y(), width() - kInset - end, 1), QColor(kLine));
    }
}

// ------------------------------------------------------------ category --

CategoryButton::CategoryButton(Icon icon, const QString& label, QWidget* parent, const QString& objectName)
        : TouchButton(label, parent, 56),
          m_icon(icon),
          m_label(label) {
    setObjectName(objectName);
    setAccessibleName(label);
    setCheckable(true);
    // The knob's first turn lands on the selected category.
    setProperty("knobHome", true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void CategoryButton::setValue(const QString& value, Lamp lamp, bool lampShown) {
    if (m_value == value && m_lamp == lamp && m_lampShown == lampShown) {
        return;
    }
    m_value = value;
    m_lamp = lamp;
    m_lampShown = lampShown;
    setAccessibleDescription(value);
    update();
}

void CategoryButton::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool selected = isChecked();
    painter.fillRect(rect(), QColor(selected || isDown() ? kRaise : kGround));
    painter.fillRect(QRect(0, height() - 1, width(), 1), QColor(kLine));
    if (selected) {
        // Browse's selection: raised fill and a 3 px white left edge.
        painter.fillRect(QRect(0, 0, 3, height()), QColor(kText));
    }
    paintIcon(painter, QRectF(14, (height() - 20) / 2.0, 20, 20), m_icon, QColor(selected ? kText : kMuted));
    const int right = width() - 10;
    painter.setFont(uiFont(font(), 16, QFont::DemiBold));
    painter.setPen(QColor(kText));
    if (m_value.isEmpty()) {
        drawElided(painter, QRect(44, 0, right - 44, height()), Qt::AlignLeft | Qt::AlignVCenter, m_label);
        return;
    }
    drawElided(painter, QRect(44, 7, right - 44, 22), Qt::AlignLeft | Qt::AlignVCenter, m_label);
    // The lamp sits on the value line, so the label keeps the full width.
    int valueLeft = 44;
    if (m_lampShown) {
        paintLamp(painter, QPointF(48, 39), 4, m_lamp);
        valueLeft = 58;
    }
    painter.setFont(capsFont(font(), 14));
    painter.setPen(QColor(kMuted));
    drawElided(painter, QRect(valueLeft, 29, right - valueLeft, 20), Qt::AlignLeft | Qt::AlignVCenter, m_value);
}

// --------------------------------------------------------------- sheet --

PickerSheet::PickerSheet(QWidget* host)
        : QWidget(host) {
    setObjectName(QStringLiteral("decklab-picker-sheet"));
    setAttribute(Qt::WA_DeleteOnClose);
    setGeometry(host->rect());
    host->installEventFilter(this);
    decklab::overlay::suppressWaveformsWhileVisible(this);
}

bool PickerSheet::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(watched, event);
}

void PickerSheet::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    QColor scrim(kGround);
    scrim.setAlpha(155); // about 60 %
    painter.fillRect(rect(), scrim);
}

PickerSheet* PickerSheet::open(QWidget* host,
        const QString& heading,
        const QStringList& options,
        int current,
        std::function<void(int)> chosen) {
    auto* sheet = new PickerSheet(host);
    applyStyle(sheet);
    auto* outer = vbox(sheet, 0, 0);
    outer->addStretch(1);
    auto* panel = card(sheet);
    panel->setObjectName(QStringLiteral("decklab-picker-panel"));
    auto* layout = vbox(panel, 0, 0);
    auto* header = new QWidget(panel);
    header->setFixedHeight(52);
    auto* headerRow = hbox(8);
    headerRow->setContentsMargins(16, 4, 4, 4);
    header->setLayout(headerRow);
    headerRow->addWidget(title(heading, header), 1);
    auto* close = new GlyphButton(Glyph::Close, tr("Close"), header, QStringLiteral("decklab-picker-close"));
    connect(close, &QPushButton::clicked, sheet, &QWidget::close);
    headerRow->addWidget(close);
    layout->addWidget(header);
    auto* list = new QWidget(panel);
    auto* rows = vbox(list, 0, 0);
    for (int index = 0; index < options.size(); ++index) {
        auto* row = new ActionRow(options.at(index), list, QStringLiteral("decklab-picker-option"));
        row->setValue(index == current ? tr("Current") : QString());
        row->setLamp(Lamp::On, index == current);
        connect(row, &QPushButton::clicked, sheet, [sheet, chosen, index] {
            sheet->close();
            if (chosen) {
                chosen(index);
            }
        });
        rows->addWidget(row);
    }
    if (options.size() > 6) {
        auto* scroll = scrollArea(list, panel, QStringLiteral("decklab-picker-scroll"));
        scroll->setFixedHeight(6 * 52);
        layout->addWidget(scroll);
    } else {
        layout->addWidget(list);
    }
    outer->addWidget(panel);
    auto* knob = new KnobNavigator(sheet, [sheet] {
        sheet->close();
        return true;
    });
    sheet->show();
    sheet->raise();
    // Start the ring on the current option.
    const auto candidates = list->findChildren<ActionRow*>();
    if (current >= 0 && current < candidates.size()) {
        knob->focus(candidates.at(current));
        knob->clear();
    }
    return sheet;
}

} // namespace decklab::ui
