// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklabrail.h"

#include <QBoxLayout>
#include <QCoreApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>
#include <utility>

#include "decklab/decklaboverlay.h"
#include "moc_decklabrail.cpp"

using namespace decklab::ui;
using decklab::rail::Alert;
using decklab::rail::Inputs;
using decklab::rail::Slot;
using decklab::rail::SlotView;

namespace decklab::rail {

SlotView slotView(Slot slot, const Inputs& in) {
    const auto text = [](const char* source) {
        return QCoreApplication::translate("DeckLabRail", source);
    };
    switch (slot) {
    case Slot::Controller:
        if (in.controllerNew) {
            return {Lamp::On, text("NEW")};
        }
        return {in.controller == Inputs::Controller::Working ? Lamp::Go
                        : in.controller == Inputs::Controller::Found ? Lamp::Caution
                                                                     : Lamp::Off,
                text("CTRL")};
    case Slot::Network:
        if (in.serviceAttached && !in.serviceOnline) {
            return {Lamp::Caution, text("WI-FI")};
        }
        return {in.wifi == Inputs::Wifi::Connected ? Lamp::Go : Lamp::Off, text("WI-FI")};
    case Slot::Recording:
        if (in.recording) {
            return {Lamp::Live, recordingTime(in.recordingSeconds)};
        }
        // A writable target is set: armed, ready to record (white, not red).
        return {in.usbRecordTarget ? Lamp::On : Lamp::Off, text("REC")};
    case Slot::Output:
        if (in.outputPending) {
            return {Lamp::Off, text("OUT")};
        }
        if (!in.outputWorking) {
            return {Lamp::LiveOutline, text("OUT")};
        }
        return {in.recentDropouts ? Lamp::Caution : Lamp::Go, text("OUT")};
    }
    return {};
}

SlotView usbView(const Inputs& in) {
    const auto text = [](const char* source) {
        return QCoreApplication::translate("DeckLabRail", source);
    };
    if (in.usbDrives <= 0) {
        return {Lamp::Off, text("USB")};
    }
    if (in.usbDrives > 1) {
        return {Lamp::Go, text("USB %1").arg(in.usbDrives)};
    }
    // r5-polish: the lamp says where (USB) and its state; the stick's name
    // ("DJ USB") is one tap away in Browse and in the status sheet. "RB"
    // was an abbreviation, and a name does not fit a 54 px slot.
    return {Lamp::Go, text("USB")};
}

Alert alertFor(const Inputs& in) {
    if (!in.outputWorking && !in.outputPending) {
        return Alert::NoOutput;
    }
    if (in.undervoltage) {
        return Alert::LowPower;
    }
    return Alert::None;
}

QString recordingTime(qint64 seconds) {
    seconds = std::max<qint64>(0, seconds);
    if (seconds < 3600) {
        return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1h%2").arg(seconds / 3600).arg((seconds % 3600) / 60, 2, 10, QLatin1Char('0'));
}

} // namespace decklab::rail

namespace {
void railFrame(QPainter& painter, const QWidget* widget, bool down) {
    painter.fillRect(widget->rect(), QColor(down ? kRaise : kPanel));
    painter.fillRect(QRect(widget->width() - 1, 0, 1, widget->height()), QColor(kLine));
    painter.fillRect(QRect(0, widget->height() - 1, widget->width(), 1), QColor(kLine));
}

void slotLabel(QPainter& painter, const QWidget* widget, const QRect& box, const SlotView& view) {
    // Times print as a readout; names as 14 px caps labels.
    const bool time = view.label.contains(QLatin1Char(':')) ||
            (view.label.size() > 2 && view.label.at(0).isDigit());
    painter.setFont(time ? numberFont(widget->font(), 16) : capsFont(widget->font(), 14));
    painter.setPen(QColor(view.lamp == Lamp::Off ? kMuted : kText));
    painter.drawText(box, Qt::AlignHCenter | Qt::AlignTop, view.label);
}
} // namespace

// ----------------------------------------------------------------- tab --

DeckLabRailTab::DeckLabRailTab(const QString& text, bool gear, QWidget* parent, const QString& objectName)
        : QPushButton(text, parent),
          m_gear(gear) {
    setObjectName(objectName);
    setAccessibleName(text);
    setFocusPolicy(Qt::NoFocus);
    setCheckable(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void DeckLabRailTab::setLamp(Lamp lamp) {
    if (m_lamp != lamp) {
        m_lamp = lamp;
        update();
    }
}

void DeckLabRailTab::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    railFrame(painter, this, isDown());
    const bool active = isChecked();
    if (active) {
        painter.fillRect(QRect(0, height() - 3, width() - 1, 3), QColor(kText));
    }
    const QColor ink(!isEnabled() ? kDim : active ? kText : kMuted);
    if (m_gear) {
        paintGlyph(painter, QRectF((width() - 24) / 2.0, (height() - 24) / 2.0 - 1, 24, 24), Glyph::Gear, ink);
    } else {
        painter.setFont(uiFont(font(), 16, QFont::DemiBold));
        painter.setPen(ink);
        const QRect box = rect().adjusted(4, 0, -4, -2);
        painter.drawText(box, Qt::AlignCenter,
                QFontMetrics(painter.font()).elidedText(text(), Qt::ElideRight, box.width()));
    }
    // Lab FX marks a wet effect with the "live" property (tickDecks).
    Lamp lamp = m_lamp;
    if (lamp == Lamp::Off && property("live").toBool()) {
        lamp = Lamp::On;
    }
    if (lamp != Lamp::Off) {
        paintLamp(painter, QPointF(width() - 10, 10), 3.5, lamp);
    }
}

// ----------------------------------------------------------------- USB --

DeckLabUsbLamp::DeckLabUsbLamp(QWidget* parent)
        : QPushButton(QCoreApplication::translate("DeckLabRail", "USB"), parent) {
    setObjectName(QStringLiteral("decklab-usb"));
    setAccessibleName(QCoreApplication::translate("DeckLabRail", "USB"));
    setAccessibleDescription(QCoreApplication::translate("DeckLabRail",
            "Tap to browse the USB stick. Hold for storage and eject."));
    setFocusPolicy(Qt::NoFocus);
    setProperty("rail", true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFixedSize(54, 44);
    m_view = decklab::rail::usbView(Inputs());
    m_long.setSingleShot(true);
    m_long.setInterval(600);
    connect(&m_long, &QTimer::timeout, this, [this] {
        m_longFired = true;
        setDown(false);
        emit longPressed();
    });
}

void DeckLabUsbLamp::setInputs(const Inputs& inputs) {
    const SlotView view = decklab::rail::usbView(inputs);
    if (view == m_view && m_recordTarget == inputs.usbRecordTarget && m_recording == inputs.recording) {
        return;
    }
    m_view = view;
    m_recordTarget = inputs.usbRecordTarget;
    m_recording = inputs.recording;
    setAccessibleName(inputs.usbDrives > 0 ? inputs.usbSummary : QCoreApplication::translate("DeckLabRail", "No USB drive"));
    update();
}

void DeckLabUsbLamp::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    railFrame(painter, this, isDown());
    const double center = width() / 2.0;
    paintLamp(painter, QPointF(center - (m_recordTarget ? 5 : 0), 13), 4, m_view.lamp);
    if (m_recordTarget) {
        // The recording drive: white when armed, live red while recording.
        paintLamp(painter, QPointF(center + 7, 13), 3, m_recording ? Lamp::Live : Lamp::On);
    }
    slotLabel(painter, this, QRect(0, 22, width() - 1, 20), m_view);
}

void DeckLabUsbLamp::mousePressEvent(QMouseEvent* event) {
    m_longFired = false;
    if (event->button() == Qt::LeftButton) {
        m_long.start();
    }
    QPushButton::mousePressEvent(event);
}

void DeckLabUsbLamp::mouseReleaseEvent(QMouseEvent* event) {
    m_long.stop();
    if (m_longFired) {
        // The long press already opened the sheet: no click on release.
        m_longFired = false;
        setDown(false);
        event->accept();
        return;
    }
    QPushButton::mouseReleaseEvent(event);
}

// ---------------------------------------------------------------- zone --

DeckLabStatusZone::DeckLabStatusZone(QWidget* parent)
        : QPushButton(parent) {
    setObjectName(QStringLiteral("decklab-status"));
    setFocusPolicy(Qt::NoFocus);
    setProperty("rail", true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    // Fixed whatever is lit: a lamp changing mid-mix never moves the tabs.
    setFixedSize(4 * kSlotWidth, 44);
    setInputs(Inputs());
}

void DeckLabStatusZone::setInputs(const Inputs& inputs) {
    QList<SlotView> views;
    for (Slot slot : {Slot::Controller, Slot::Network, Slot::Recording, Slot::Output}) {
        views.append(decklab::rail::slotView(slot, inputs));
    }
    const Alert alert = decklab::rail::alertFor(inputs);
    if (views == m_slots && alert == m_alert && m_recording == inputs.recording) {
        return;
    }
    m_slots = views;
    m_alert = alert;
    m_recording = inputs.recording;
    QStringList spoken;
    if (alert != Alert::None) {
        spoken.append(alertText());
    }
    for (const auto& view : std::as_const(m_slots)) {
        spoken.append(view.label);
    }
    setAccessibleName(QCoreApplication::translate("DeckLabRail", "Status: %1").arg(spoken.join(QStringLiteral(", "))));
    update();
}

QString DeckLabStatusZone::alertText() const {
    switch (m_alert) {
    case Alert::NoOutput:
        return QCoreApplication::translate("DeckLabRail", "NO OUTPUT · FIX");
    case Alert::LowPower:
        return QCoreApplication::translate("DeckLabRail", "LOW POWER");
    case Alert::None:
        break;
    }
    return QString();
}

void DeckLabStatusZone::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor(isDown() ? kRaise : kPanel));
    painter.fillRect(QRect(0, height() - 1, width(), 1), QColor(kLine));
    painter.fillRect(QRect(width() - 1, 0, 1, height()), QColor(kLine));
    if (m_alert == Alert::None) {
        for (int index = 0; index < m_slots.size(); ++index) {
            const QRect box(index * kSlotWidth, 0, kSlotWidth, height());
            paintLamp(painter, QPointF(box.center().x() + 0.5, 13), 4, m_slots.at(index).lamp);
            slotLabel(painter, this, QRect(box.left(), 22, box.width(), 20), m_slots.at(index));
        }
        return;
    }
    // The alert holds the zone until it is fixed; the whole zone is the button.
    const QRectF frame = QRectF(rect()).adjusted(3, 4, -4, -5);
    if (m_alert == Alert::NoOutput) {
        painter.setPen(QPen(QColor(kLive), 1.5));
        painter.setBrush(QColor(isDown() ? kRaise : kGround));
        painter.drawRoundedRect(frame, 4, 4);
    } else {
        paintCautionHatch(painter, frame, 4);
        painter.setPen(QPen(QColor(kText), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(frame, 4, 4);
    }
    paintGlyph(painter, QRectF(frame.left() + 6, (height() - 16) / 2.0, 16, 16), Glyph::Warning, QColor(kText));
    painter.setFont(capsFont(font(), 14));
    painter.setPen(QColor(kText));
    const QString text = m_alert == Alert::NoOutput ? QCoreApplication::translate("DeckLabRail", "NO OUTPUT")
                                                    : QCoreApplication::translate("DeckLabRail", "LOW POWER");
    painter.drawText(QRectF(frame.left() + 26, 0, frame.width() - 60, height()), Qt::AlignLeft | Qt::AlignVCenter, text);
    if (m_alert == Alert::NoOutput) {
        painter.drawText(QRectF(frame.right() - 44, 0, 28, height()), Qt::AlignRight | Qt::AlignVCenter,
                QCoreApplication::translate("DeckLabRail", "FIX"));
    }
    paintGlyph(painter, QRectF(frame.right() - 16, (height() - 14) / 2.0, 14, 14), Glyph::Chevron, QColor(kText));
    if (m_recording) {
        paintLamp(painter, QPointF(width() - 7, 7), 3, Lamp::Live);
    }
}

// -------------------------------------------------------------- notice --

DeckLabRailNotice::DeckLabRailNotice(QWidget* parent)
        : QPushButton(parent) {
    setObjectName(QStringLiteral("decklab-rail-notice"));
    setFocusPolicy(Qt::NoFocus);
    setProperty("rail", true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    hide();
    // 200 ms in (5 x 40 ms), 160 ms out: only while a notice moves.
    m_animation.setInterval(40);
    connect(&m_animation, &QTimer::timeout, this, [this] { step(); });
    m_hold.setSingleShot(true);
    connect(&m_hold, &QTimer::timeout, this, [this] {
        m_phase = 5;
        m_animation.start();
    });
}

void DeckLabRailNotice::showNotice(const decklab::rail::Notice& notice) {
    if (notice.text.isEmpty()) {
        return;
    }
    if (m_current.text == notice.text) {
        // The same news again: keep it up a little longer.
        m_current = notice;
        if (m_hold.isActive()) {
            m_hold.start(notice.milliseconds);
        }
        return;
    }
    if (active()) {
        for (const auto& queued : std::as_const(m_queue)) {
            if (queued.text == notice.text) {
                return;
            }
        }
        if (m_queue.size() >= 3) {
            m_queue.removeFirst();
        }
        m_queue.append(notice);
        return;
    }
    m_current = notice;
    m_phase = 0;
    setAccessibleName(notice.text);
    show();
    raise();
    update();
    m_animation.start();
}

void DeckLabRailNotice::step() {
    ++m_phase;
    if (m_phase == 5) {
        m_animation.stop();
        m_hold.start(std::max(1000, m_current.milliseconds));
    } else if (m_phase >= 9) {
        m_animation.stop();
        hide();
        m_current = decklab::rail::Notice();
        next();
        return;
    }
    update();
}

void DeckLabRailNotice::next() {
    if (!m_queue.isEmpty()) {
        showNotice(m_queue.takeFirst());
    }
}

bool DeckLabRailNotice::trigger() {
    if (!m_current.action) {
        return false;
    }
    const auto action = m_current.action;
    dismiss();
    action();
    return true;
}

void DeckLabRailNotice::dismiss() {
    m_animation.stop();
    m_hold.stop();
    m_current = decklab::rail::Notice();
    hide();
    next();
}

void DeckLabRailNotice::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor(kPanel));
    const double travel = m_phase <= 5 ? (5 - m_phase) / 5.0 : (m_phase - 5) / 4.0;
    const int offset = static_cast<int>(travel * width());
    const QRect box = rect().adjusted(offset, 0, offset, 0);
    painter.fillRect(box, QColor(isDown() ? kGround : kRaise));
    painter.fillRect(QRect(box.left(), 0, 3, height()), QColor(kText));
    painter.fillRect(QRect(0, height() - 1, width(), 1), QColor(kLine));
    paintLamp(painter, QPointF(box.left() + 18, height() / 2.0), 4, m_current.lamp);
    painter.setFont(capsFont(font(), 14));
    painter.setPen(QColor(kText));
    const int right = m_current.action ? 30 : 12;
    const QRect text(box.left() + 32, 0, std::max(0, box.width() - 32 - right), height());
    painter.drawText(text, Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics(painter.font()).elidedText(m_current.text, Qt::ElideRight, text.width()));
    if (m_current.action) {
        paintGlyph(painter, QRectF(box.right() - 24, (height() - 16) / 2.0, 16, 16), Glyph::Chevron, QColor(kText));
    }
}

// --------------------------------------------------------------- sheet --

struct DeckLabStatusSheet::Line {
    class View;
    QWidget* widget = nullptr;
    QString key;
    QString name;
    QString detail;
    Lamp lamp = Lamp::Off;
};

namespace {
class StatusLineView : public QWidget {
  public:
    StatusLineView(const QString& name, QWidget* parent)
            : QWidget(parent),
              m_name(name) {
        setFixedHeight(52);
    }
    void set(Lamp lamp, const QString& detail, int actionWidth) {
        m_lamp = lamp;
        m_detail = detail;
        m_actionWidth = actionWidth;
        setAccessibleName(m_name + QStringLiteral(": ") + detail);
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(QRect(16, height() - 1, width() - 32, 1), QColor(kLine));
        paintLamp(painter, QPointF(24, height() / 2.0), 4, m_lamp);
        painter.setFont(uiFont(font(), 16, QFont::DemiBold));
        painter.setPen(QColor(kText));
        painter.drawText(QRect(40, 0, 116, height()), Qt::AlignLeft | Qt::AlignVCenter, m_name);
        painter.setFont(uiFont(font(), 15, QFont::Medium));
        painter.setPen(QColor(kMuted));
        const int right = width() - 16 - m_actionWidth - 12;
        painter.drawText(QRect(160, 0, std::max(0, right - 160), height()), Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(painter.font()).elidedText(m_detail, Qt::ElideRight, std::max(0, right - 160)));
    }

  private:
    QString m_name;
    QString m_detail;
    Lamp m_lamp = Lamp::Off;
    int m_actionWidth = 0;
};
constexpr int kActionWidth = 176;
} // namespace

DeckLabStatusSheet::DeckLabStatusSheet(QWidget* surface, Actions actions)
        : QWidget(surface),
          m_actions(std::move(actions)) {
    setObjectName(QStringLiteral("decklab-status-sheet"));
    applyStyle(this);
    surface->installEventFilter(this);
    // Covers the page (and the waveforms) below the rail.
    decklab::overlay::suppressWaveformsWhileVisible(this);
    auto* outer = vbox(this, 0, 0);
    outer->addStretch(1);
    m_panel = new QWidget(this);
    m_panel->setObjectName(QStringLiteral("decklab-status-panel"));
    m_panel->setAttribute(Qt::WA_StyledBackground);
    m_panel->setStyleSheet(QStringLiteral("QWidget#decklab-status-panel {background:%1;border-top:1px solid %2;}")
                                   .arg(QLatin1String(kPanel), QLatin1String(kLine)));
    auto* layout = vbox(m_panel, 0, 0);
    layout->setContentsMargins(0, 0, 0, 8);
    auto* header = new QWidget(m_panel);
    header->setFixedHeight(52);
    auto* headerRow = hbox(8);
    headerRow->setContentsMargins(16, 4, 8, 4);
    header->setLayout(headerRow);
    headerRow->addWidget(title(tr("Status"), header), 1);
    auto* close = new GlyphButton(Glyph::Close, tr("Close"), header, QStringLiteral("decklab-status-close"));
    connect(close, &QPushButton::clicked, this, &QWidget::hide);
    headerRow->addWidget(close);
    layout->addWidget(header);
    const auto action = [this](Line* line, QPushButton* button) {
        auto* row = new QHBoxLayout(line->widget);
        row->setContentsMargins(0, 4, 16, 4);
        row->addStretch(1);
        button->setFixedWidth(kActionWidth);
        row->addWidget(button);
    };
    const auto go = [this](std::function<void()> run) {
        return [this, run = std::move(run)] {
            hide();
            if (run) {
                run();
            }
        };
    };
    // r5-polish: while setup is unfinished ("Set up later"), the first line
    // offers it. Hidden once it is done.
    Line* setup = addLine(QStringLiteral("setup"), tr("Setup"));
    auto* finish = button(tr("Finish setup"), setup->widget, QStringLiteral("decklab-status-finish-setup"), 44);
    connect(finish, &QPushButton::clicked, this, go([this] {
        if (m_actions.finishSetup) {
            m_actions.finishSetup();
        }
    }));
    action(setup, finish);
    m_setupLine = setup->widget;
    Line* sound = addLine(QStringLiteral("sound"), tr("Sound"));
    m_soundAction = button(tr("Deck Setup"), sound->widget, QStringLiteral("decklab-status-sound"), 44);
    connect(m_soundAction, &QPushButton::clicked, this, go([this] {
        if (m_actions.deckSetup) {
            m_actions.deckSetup(1);
        }
    }));
    action(sound, m_soundAction);
    Line* recording = addLine(QStringLiteral("recording"), tr("Recording"));
    m_record = new HoldButton(tr("Hold to record"), recording->widget, QStringLiteral("decklab-status-record"), 44);
    connect(m_record, &HoldButton::confirmed, this, [this] {
        if (m_actions.toggleRecording) {
            m_actions.toggleRecording();
        }
    });
    action(recording, m_record);
    Line* usb = addLine(QStringLiteral("usb"), tr("USB"));
    m_storage = button(tr("Storage"), usb->widget, QStringLiteral("decklab-status-storage"), 44);
    connect(m_storage, &QPushButton::clicked, this, go([this] {
        if (m_actions.system) {
            m_actions.system(5); // DeckLabSystemPage::Section::Storage
        }
    }));
    action(usb, m_storage);
    Line* network = addLine(QStringLiteral("network"), tr("Network"));
    m_wifi = button(tr("Wi-Fi"), network->widget, QStringLiteral("decklab-status-wifi"), 44);
    connect(m_wifi, &QPushButton::clicked, this, go([this] {
        if (m_actions.system) {
            m_actions.system(1); // Section::Wifi
        }
    }));
    action(network, m_wifi);
    Line* controller = addLine(QStringLiteral("controller"), tr("Controller"));
    m_controller = button(tr("Deck Setup"), controller->widget, QStringLiteral("decklab-status-controller"), 44);
    connect(m_controller, &QPushButton::clicked, this, go([this] {
        if (m_actions.deckSetup) {
            m_actions.deckSetup(0);
        }
    }));
    action(controller, m_controller);
    Line* unit = addLine(QStringLiteral("unit"), tr("Unit"));
    auto* unitAction = button(tr("Unit"), unit->widget, QStringLiteral("decklab-status-unit"), 44);
    connect(unitAction, &QPushButton::clicked, this, go([this] {
        if (m_actions.system) {
            m_actions.system(0); // Section::Home
        }
    }));
    action(unit, unitAction);
    for (auto* line : std::as_const(m_lines)) {
        layout->addWidget(line->widget);
    }
    outer->addWidget(m_panel);
    m_knob = new KnobNavigator(this, [this] {
        hide();
        return true;
    });
    hide();
    place();
}

DeckLabStatusSheet::Line* DeckLabStatusSheet::addLine(const QString& key, const QString& name) {
    auto* line = new Line;
    line->key = key;
    line->name = name;
    line->widget = new StatusLineView(name, m_panel);
    line->widget->setObjectName(QStringLiteral("decklab-status-line-") + key);
    m_lines.append(line);
    return line;
}

void DeckLabStatusSheet::place() {
    if (auto* surface = parentWidget()) {
        setGeometry(0, 44, surface->width(), std::max(0, surface->height() - 44));
    }
}

bool DeckLabStatusSheet::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() && event->type() == QEvent::Resize) {
        place();
    }
    return QWidget::eventFilter(watched, event);
}

void DeckLabStatusSheet::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    // The page dims behind the sheet.
    QColor scrim(kGround);
    scrim.setAlpha(150);
    painter.fillRect(rect(), scrim);
}

QStringList DeckLabStatusSheet::lines() const {
    QStringList result;
    for (const auto* line : m_lines) {
        if (line->widget->isVisibleTo(m_panel)) {
            result.append(line->name + QStringLiteral(": ") + line->detail);
        }
    }
    return result;
}

void DeckLabStatusSheet::refresh(const Inputs& in) {
    const auto set = [this](const QString& key, Lamp lamp, const QString& detail) {
        for (auto* line : std::as_const(m_lines)) {
            if (line->key == key) {
                line->lamp = lamp;
                line->detail = detail;
                static_cast<StatusLineView*>(line->widget)->set(lamp, detail, kActionWidth);
            }
        }
    };
    // Setup (r5-polish): only while it is unfinished.
    m_setupLine->setVisible(in.setupPending && m_actions.finishSetup);
    set(QStringLiteral("setup"), Lamp::On, tr("Not finished · Wi-Fi, time zone, controller, sound"));
    // Sound.
    if (!in.outputWorking && !in.outputPending) {
        set(QStringLiteral("sound"), Lamp::LiveOutline, tr("No sound output. Choose one in Deck Setup."));
        m_soundAction->setText(tr("Fix sound"));
    } else {
        QString detail = in.soundSummary.isEmpty() ? tr("Playing out") : in.soundSummary;
        if (in.dropouts > 0) {
            detail += tr(" · %n dropout(s)", nullptr, in.dropouts);
        }
        set(QStringLiteral("sound"), in.recentDropouts ? Lamp::Caution : Lamp::Go, detail);
        m_soundAction->setText(tr("Deck Setup"));
    }
    // Recording: live red only while it records.
    const QString target = in.recordTarget.isEmpty() ? tr("USB") : in.recordTarget;
    if (in.recording) {
        set(QStringLiteral("recording"), Lamp::Live,
                tr("Recording %1 to %2").arg(decklab::rail::recordingTime(in.recordingSeconds), target));
        m_record->setText(tr("Hold to stop"));
        m_record->setReason(QString());
    } else if (in.usbRecordTarget) {
        set(QStringLiteral("recording"), Lamp::On,
                in.recordHoursFree >= 0 ? tr("To %1 · about %2 h free").arg(target).arg(std::floor(in.recordHoursFree))
                                        : tr("To %1").arg(target));
        m_record->setText(tr("Hold to record"));
        m_record->setReason(QString());
    } else {
        set(QStringLiteral("recording"), Lamp::Off, tr("No recording drive. Pick one in Storage."));
        m_record->setText(tr("Hold to record"));
        m_record->setReason(tr("No recording drive"));
    }
    // USB.
    set(QStringLiteral("usb"), in.usbDrives > 0 ? Lamp::Go : Lamp::Off,
            in.usbDrives > 0 ? in.usbSummary : tr("No USB drive"));
    m_storage->setEnabled(in.serviceOnline);
    // Network.
    if (!in.serviceAttached) {
        set(QStringLiteral("network"), Lamp::Off, tr("Not available on this unit"));
    } else if (!in.serviceOnline) {
        set(QStringLiteral("network"), Lamp::Caution, tr("System service restarting · DJing is unaffected"));
    } else if (in.wifi == Inputs::Wifi::Connected) {
        set(QStringLiteral("network"), Lamp::Go,
                in.signal >= 0 ? tr("%1 · %2 %").arg(in.ssid).arg(in.signal) : in.ssid);
    } else {
        set(QStringLiteral("network"), Lamp::Off, in.wifi == Inputs::Wifi::Off ? tr("Wi-Fi off") : tr("Not connected"));
    }
    m_wifi->setEnabled(in.serviceOnline);
    // Controller.
    const QString name = in.controllerName.isEmpty() ? tr("Controller") : in.controllerName;
    if (in.controllerNew) {
        set(QStringLiteral("controller"), Lamp::On, tr("%1 · new, set it up").arg(name));
        m_controller->setText(tr("Set up"));
    } else if (in.controller == Inputs::Controller::Working) {
        set(QStringLiteral("controller"), Lamp::Go, tr("%1 · working").arg(name));
        m_controller->setText(tr("Deck Setup"));
    } else if (in.controller == Inputs::Controller::Found) {
        set(QStringLiteral("controller"), Lamp::Caution, tr("%1 · not responding yet").arg(name));
        m_controller->setText(tr("Fix"));
    } else {
        set(QStringLiteral("controller"), Lamp::Off, tr("No controller"));
        m_controller->setText(tr("Deck Setup"));
    }
    // Unit: heat and power.
    if (in.undervoltage) {
        set(QStringLiteral("unit"), Lamp::Caution, tr("Low power · use a 5.1 V 3 A USB-C supply"));
    } else if (in.heat == QStringLiteral("hot")) {
        set(QStringLiteral("unit"), Lamp::Caution, tr("Hot · %1 °C · give it air").arg(std::round(in.temperature)));
    } else if (in.heat == QStringLiteral("warm")) {
        set(QStringLiteral("unit"), Lamp::Caution, tr("Warm · %1 °C").arg(std::round(in.temperature)));
    } else if (in.temperature >= 0) {
        set(QStringLiteral("unit"), Lamp::Go, tr("%1 °C · fine").arg(std::round(in.temperature)));
    } else {
        set(QStringLiteral("unit"), Lamp::Off, in.serviceOnline ? tr("Fine") : tr("Details need the system service"));
    }
}

// ---------------------------------------------------------------- rail --

DeckLabRail::DeckLabRail(QWidget* surface, QWidget* rail, Hooks hooks)
        : QObject(surface),
          m_surface(surface),
          m_rail(rail),
          m_hooks(std::move(hooks)),
          m_usb(new DeckLabUsbLamp(rail)),
          m_zone(new DeckLabStatusZone(rail)),
          m_notice(new DeckLabRailNotice(rail)) {
    connect(m_usb, &QPushButton::clicked, this, [this] {
        if (m_hooks.browseUsb) {
            m_hooks.browseUsb();
        }
    });
    connect(m_usb, &DeckLabUsbLamp::longPressed, this, [this] { openSheet(); });
    connect(m_zone, &QPushButton::clicked, this, [this] { zoneTapped(); });
    connect(m_notice, &QPushButton::clicked, this, [this] {
        if (!m_notice->trigger()) {
            openSheet();
        }
    });
    m_zone->installEventFilter(this);
    rail->installEventFilter(this);
}

void DeckLabRail::setNoticeSpan(QWidget* from) {
    if (m_noticeFrom) {
        m_noticeFrom->removeEventFilter(this);
    }
    m_noticeFrom = from;
    if (from) {
        from->installEventFilter(this);
    }
    placeNotice();
}

bool DeckLabRail::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Move || event->type() == QEvent::Resize || event->type() == QEvent::Show ||
            event->type() == QEvent::LayoutRequest) {
        placeNotice();
    }
    return QObject::eventFilter(watched, event);
}

void DeckLabRail::placeNotice() {
    QWidget* from = m_noticeFrom ? m_noticeFrom.data() : m_usb;
    const int left = from->geometry().left();
    const int right = m_zone->geometry().right();
    if (right > left) {
        m_notice->setGeometry(left, 0, right - left + 1, 44);
    }
}

void DeckLabRail::setInputs(const Inputs& inputs) {
    if (inputs == m_inputs) {
        return;
    }
    m_inputs = inputs;
    m_usb->setInputs(inputs);
    m_zone->setInputs(inputs);
    if (m_sheet && m_sheet->isVisible()) {
        m_sheet->refresh(inputs);
    }
}

void DeckLabRail::notify(const decklab::rail::Notice& notice) {
    placeNotice();
    m_notice->showNotice(notice);
}

void DeckLabRail::zoneTapped() {
    if (m_zone->alert() == Alert::NoOutput && m_hooks.deckSetup) {
        // FIX: Deck Setup at the sound step.
        m_hooks.deckSetup(1);
        return;
    }
    openSheet();
}

DeckLabStatusSheet* DeckLabRail::openSheet() {
    if (!m_surface) {
        return nullptr;
    }
    if (!m_sheet) {
        DeckLabStatusSheet::Actions actions;
        actions.deckSetup = m_hooks.deckSetup;
        actions.system = m_hooks.system;
        actions.toggleRecording = m_hooks.toggleRecording;
        actions.browseUsb = m_hooks.browseUsb;
        actions.finishSetup = m_hooks.finishSetup;
        m_sheet = new DeckLabStatusSheet(m_surface, actions);
    }
    m_sheet->refresh(m_inputs);
    m_sheet->show();
    m_sheet->raise();
    return m_sheet;
}
