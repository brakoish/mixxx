// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklabsetupwizard.h"

#include <QBoxLayout>
#include <QButtonGroup>
#include <QComboBox>
#include <QEvent>
#include <QFileInfo>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <algorithm>
#include <utility>

#include "decklab/decklaboverlay.h"
#include "decklab/decklabsystempages.h"
#include "decklab/decklabinstruments.h"
#include "decklab/decklabui.h"
#include "library/decklabknob.h"
#include "moc_decklabsetupwizard.cpp"
#include "widget/decklabdeckwidgets.h"

using namespace decklab::ui;
using Step = DeckLabSetupWizard::Step;

namespace {
constexpr int kPickerIndex = 6;

void clearLayout(QLayout* layout) {
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        } else if (QLayout* child = item->layout()) {
            clearLayout(child);
        }
        delete item;
    }
}

bool sameFile(const QString& left, const QString& right) {
    if (left.isEmpty() || right.isEmpty()) {
        return false;
    }
    const QFileInfo a(left);
    const QFileInfo b(right);
    if (a.exists() && b.exists()) {
        return a.canonicalFilePath() == b.canonicalFilePath();
    }
    return a.fileName() == b.fileName();
}

QString routeData(const decklab::AudioRoute& route) {
    return route.isValid() ? route.device + QChar(0x1f) + QString::number(route.channelBase) : QString();
}

decklab::AudioRoute routeFrom(const QVariant& data) {
    const QString text = data.toString();
    const int split = text.lastIndexOf(QChar(0x1f));
    if (split <= 0) {
        return decklab::AudioRoute{};
    }
    return decklab::AudioRoute{text.left(split), text.mid(split + 1).toInt()};
}

// Big two/four deck choice drawn as deck outlines.
class LayoutChoice : public QPushButton {
  public:
    LayoutChoice(int decks, const QString& heading, const QString& detail, QWidget* parent, const QString& name)
            : QPushButton(heading, parent),
              m_decks(decks),
              m_heading(heading),
              m_detail(detail) {
        setObjectName(name);
        setCheckable(true);
        setFocusPolicy(Qt::NoFocus);
        setMinimumHeight(200);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    void setSuggested(bool suggested) {
        m_suggested = suggested;
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF frame = QRectF(rect()).adjusted(1, 1, -1, -1);
        painter.setPen(QPen(QColor(isChecked() || isDown() ? kSelect : kLine), isChecked() ? 2 : 1));
        painter.setBrush(QColor(isChecked() || isDown() ? kRaise : kPanel));
        painter.drawRoundedRect(frame, 4, 4);
        const int columns = m_decks;
        const double areaWidth = std::min(width() - 60.0, 70.0 * columns + 12.0 * (columns - 1));
        const double deckWidth = (areaWidth - 12.0 * (columns - 1)) / columns;
        const double deckHeight = std::min(height() * 0.42, deckWidth * 1.25);
        const double left = (width() - areaWidth) / 2;
        const double top = 26;
        for (int deck = 0; deck < columns; ++deck) {
            const QRectF outline(left + deck * (deckWidth + 12), top, deckWidth, deckHeight);
            // Deck colours are deck identity, so the chosen layout's preview uses them.
            painter.setPen(QPen(isChecked() ? deckLabDeckColor(deck + 1) : QColor(kMuted), 1.6));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(outline, 3, 3);
            const double radius = std::min(deckWidth, deckHeight) * 0.28;
            painter.drawEllipse(QPointF(outline.center().x(), outline.top() + deckHeight * 0.45), radius, radius);
            painter.setFont(uiFont(font(), 14, QFont::DemiBold));
            painter.drawText(QRectF(outline.left(), outline.bottom() - 22, outline.width(), 18), Qt::AlignCenter,
                    QString::number(deck + 1));
        }
        painter.setFont(uiFont(font(), 18, QFont::DemiBold));
        painter.setPen(QColor(kText));
        const double textTop = top + deckHeight + 18;
        painter.drawText(QRectF(10, textTop, width() - 20, 24), Qt::AlignCenter, m_heading);
        painter.setFont(uiFont(font(), 15, QFont::Medium));
        painter.setPen(QColor(kMuted));
        painter.drawText(QRectF(10, textTop + 28, width() - 20, 22), Qt::AlignCenter, m_detail);
        if (m_suggested) {
            QFont tagFont = uiFont(font(), 14, QFont::DemiBold);
            tagFont.setCapitalization(QFont::AllUppercase);
            painter.setFont(tagFont);
            const QString tag = DeckLabSetupWizard::tr("Suggested");
            const double tagWidth = QFontMetrics(tagFont).horizontalAdvance(tag) + 16;
            const QRectF pill(width() - tagWidth - 10, 8, tagWidth, 24);
            painter.setPen(QColor(kSelect));
            painter.setBrush(QColor(kRaise));
            painter.drawRoundedRect(pill, 3, 3);
            painter.setPen(QColor(kText));
            painter.drawText(pill, Qt::AlignCenter, tag);
        }
    }

  private:
    int m_decks;
    QString m_heading;
    QString m_detail;
    bool m_suggested = false;
};

} // namespace

DeckLabSetupWizard::DeckLabSetupWizard(DeckLabSystem* system, QWidget* host)
        : QWidget(host),
          m_system(system) {
    setObjectName(QStringLiteral("decklab-setup-wizard"));
    applyStyle(this);
    setGeometry(host->rect());
    host->installEventFilter(this);
    // Drawn over the whole surface: the native GL waveform windows would
    // otherwise cover its middle (they are hidden while it is visible).
    decklab::overlay::suppressWaveformsWhileVisible(this);
    auto* root = vbox(this, 0, 0);

    auto* header = new QWidget(this);
    header->setFixedHeight(56);
    auto* headerRow = hbox(6);
    headerRow->setContentsMargins(18, 6, 8, 6);
    header->setLayout(headerRow);
    m_heading = title(tr("Deck Setup"), header);
    m_heading->setFixedWidth(140);
    headerRow->addWidget(m_heading);
    m_stepRow = hbox(2);
    m_progress = new DeckLabProgressLine(header);
    m_stepRow->addWidget(m_progress);
    // r5-polish: the progress line takes all the room between the title and
    // Close, so CONTROLLER fits in the seven-step first run (no stretch).
    headerRow->addLayout(m_stepRow, 1);
    m_close = button(tr("Close"), header, QStringLiteral("decklab-setup-close"), 44);
    m_close->setFixedWidth(132);
    connect(m_close, &QPushButton::clicked, this, [this] {
        // r5-polish: "Set up later" sticks: the first run stops opening at
        // start, and Settings offers Finish setup instead.
        if (m_firstRun && m_system) {
            m_system->deferFirstRun();
        }
        finish(false);
    });
    headerRow->addWidget(m_close);
    root->addWidget(header);

    m_pages = new QStackedWidget(this);
    root->addWidget(m_pages, 1);
    auto* welcomePage = new QWidget(m_pages);
    buildWelcome(welcomePage);
    m_pages->addWidget(welcomePage); // 0
    m_embedded = new DeckLabSystemPage(m_pages);
    m_embedded->setEmbedded(true);
    m_embedded->setSystem(system);
    m_pages->addWidget(m_embedded); // 1: Wi-Fi and time zone
    auto* controllerPage = new QWidget(m_pages);
    buildController(controllerPage);
    m_pages->addWidget(controllerPage); // 2
    auto* audioPage = new QWidget(m_pages);
    buildAudio(audioPage);
    m_pages->addWidget(audioPage); // 3
    auto* layoutPage = new QWidget(m_pages);
    buildLayout(layoutPage);
    m_pages->addWidget(layoutPage); // 4
    auto* donePage = new QWidget(m_pages);
    buildDone(donePage);
    m_pages->addWidget(donePage); // 5
    auto* pickerPage = new QWidget(m_pages);
    buildPicker(pickerPage);
    m_pages->addWidget(pickerPage); // 6 = kPickerIndex

    m_footer = new QWidget(this);
    m_footer->setFixedHeight(70);
    auto* footerRow = hbox(14);
    footerRow->setContentsMargins(16, 10, 16, 12);
    m_footer->setLayout(footerRow);
    m_back = button(tr("Back"), m_footer, QStringLiteral("decklab-setup-back"), 48);
    m_back->setFixedWidth(120);
    connect(m_back, &QPushButton::clicked, this, &DeckLabSetupWizard::back);
    footerRow->addWidget(m_back);
    m_message = new QLabel(m_footer);
    m_message->setObjectName(QStringLiteral("decklab-setup-message"));
    m_message->setWordWrap(true);
    m_message->setProperty("role", QStringLiteral("muted"));
    footerRow->addWidget(m_message, 1);
    m_next = primaryButton(tr("Next"), m_footer, QStringLiteral("decklab-setup-next"), 48);
    m_next->setFixedWidth(170);
    connect(m_next, &QPushButton::clicked, this, &DeckLabSetupWizard::next);
    footerRow->addWidget(m_next);
    root->addWidget(m_footer);
    // The password keyboard and time zone picker need the footer's height.
    connect(m_embedded, &DeckLabSystemPage::panelOpened, this, [this](bool open) {
        m_footer->setVisible(!open);
        update();
    });

    if (m_system) {
        const auto refresh = [this] {
            if (m_step == Step::Controller && isVisible() && m_pages->currentIndex() != kPickerIndex) {
                refreshControllers();
            }
        };
        connect(m_system.data(), &DeckLabSystem::controllersChanged, this, refresh);
        connect(m_system.data(), &DeckLabSystem::devicesChanged, this, refresh);
        connect(m_system.data(), &DeckLabSystem::controllersRescanned, this, [this] {
            m_detect->setEnabled(true);
            m_detect->setText(tr("Detect again"));
            if (m_step != Step::Controller) {
                return;
            }
            refreshControllers();
            const auto* info = selected();
            if (info && info->mixxxName.isEmpty()) {
                setMessage(tr("Still not detected. Try another USB port, or Restart to detect."), true);
            } else if (info) {
                setMessage(tr("%1 detected.").arg(info->name));
            }
        });
    }
    setSequence({Step::Controller, Step::Audio, Step::Layout, Step::Done});
    // r4-system: the knob drives Deck Setup (turn, press; back = Back).
    m_knob = new KnobNavigator(this, [this] {
        if (m_back->isVisible() && m_back->isEnabled()) {
            back();
        }
        return true;
    });
    m_toneMeter.setInterval(40);
    connect(&m_toneMeter, &QTimer::timeout, this, [this] {
        if (!m_playingPad) {
            m_toneMeter.stop();
            return;
        }
        // The pad's meter follows the tone: 50 ms fade in, 0.9 s, fade out.
        const double t = m_toneSince.elapsed() / 1000.0;
        const double envelope = std::clamp(std::min(t / 0.05, (0.9 - t) / 0.05), 0.0, 1.0);
        m_playingPad->setLevel(envelope * (48 + m_toneDbfs) / 48.0);
    });
}

bool DeckLabSetupWizard::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    if (event->type() == QEvent::FocusIn && watched == m_search) {
        m_keyboard->show();
    }
    return QWidget::eventFilter(watched, event);
}

void DeckLabSetupWizard::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(kGround));
    painter.fillRect(QRect(0, 56, width(), 1), QColor(kLine));
    if (m_footer->isVisible()) {
        painter.fillRect(QRect(0, height() - m_footer->height(), width(), 1), QColor(kLine));
    }
}

int DeckLabSetupWizard::pageIndex(Step step) const {
    switch (step) {
    case Step::Welcome:
        return 0;
    case Step::Wifi:
    case Step::Time:
        return 1;
    case Step::Controller:
        return 2;
    case Step::Audio:
        return 3;
    case Step::Layout:
        return 4;
    case Step::Done:
        return 5;
    }
    return 2;
}

QString DeckLabSetupWizard::stepName(Step step) const {
    switch (step) {
    case Step::Welcome:
        return tr("Welcome");
    case Step::Wifi:
        return tr("Wi-Fi");
    case Step::Time:
        return tr("Time");
    case Step::Controller:
        return tr("Controller");
    case Step::Audio:
        return tr("Sound");
    case Step::Layout:
        return tr("Decks");
    case Step::Done:
        return tr("Play");
    }
    return QString();
}

void DeckLabSetupWizard::setSequence(const QList<Step>& sequence) {
    m_sequence = sequence;
    // Seven steps need the room the title would take.
    m_heading->setVisible(sequence.size() <= 4);
    QStringList names;
    for (const auto step : sequence) {
        names.append(stepName(step));
    }
    m_progress->setSteps(names);
}

void DeckLabSetupWizard::start(Step step, const QString& preferredKey) {
    m_firstRun = false;
    m_preferredKey = preferredKey;
    setSequence({Step::Controller, Step::Audio, Step::Layout, Step::Done});
    m_close->setText(tr("Close"));
    show();
    raise();
    goTo(m_sequence.contains(step) ? step : Step::Controller);
}

void DeckLabSetupWizard::startFirstRun() {
    m_firstRun = true;
    m_preferredKey.clear();
    setSequence({Step::Welcome, Step::Wifi, Step::Time, Step::Controller, Step::Audio, Step::Layout, Step::Done});
    // "Set up later" defers it (no more automatic start); only finishing
    // sets first_run_done.
    m_close->setText(tr("Set up later"));
    show();
    raise();
    goTo(Step::Welcome);
}

void DeckLabSetupWizard::setMessage(const QString& text, bool error) {
    m_message->setProperty("role", error ? QStringLiteral("error") : QStringLiteral("muted"));
    m_message->style()->unpolish(m_message);
    m_message->style()->polish(m_message);
    m_message->setText(text);
}

void DeckLabSetupWizard::goTo(Step step) {
    m_step = step;
    m_pages->setCurrentIndex(pageIndex(step));
    m_footer->show();
    const int current = std::max<int>(0, m_sequence.indexOf(step));
    m_progress->setCurrent(current);
    // No Back on the first step (F-05); no "Set up later" on the last (F-03).
    m_back->setVisible(current > 0);
    m_close->setVisible(!(m_firstRun && step == Step::Done));
    m_back->setEnabled(current > 0);
    m_next->setText(step == Step::Done      ? tr("Play")
                    : step == Step::Welcome ? tr("Get started")
                                            : tr("Next"));
    setMessage(QString());
    switch (step) {
    case Step::Welcome:
        break;
    case Step::Wifi:
        m_embedded->showSection(DeckLabSystemPage::Section::Wifi);
        setMessage(tr("DJing works offline. Internet sets the clock and enables Get music."));
        break;
    case Step::Time:
        m_embedded->showSection(DeckLabSystemPage::Section::Time);
        setMessage(tr("Sets the clock and your recordings' dates."));
        break;
    case Step::Controller:
        m_skippedController = false;
        refreshControllers();
        break;
    case Step::Audio:
        enterAudio();
        break;
    case Step::Layout:
        renderLayout();
        break;
    case Step::Done:
        renderDone();
        break;
    }
    update();
}

void DeckLabSetupWizard::showControllerPage() {
    m_keyboard->hide();
    m_pages->setCurrentIndex(pageIndex(Step::Controller));
    m_footer->show();
    update();
}

void DeckLabSetupWizard::back() {
    if (m_pages->currentIndex() == kPickerIndex) {
        showControllerPage();
        return;
    }
    const int position = m_sequence.indexOf(m_step);
    if (position > 0) {
        goTo(m_sequence.at(position - 1));
    }
}

const DeckLabSystem::ControllerInfo* DeckLabSetupWizard::selected() const {
    if (m_skippedController) {
        return nullptr;
    }
    return m_selected >= 0 && m_selected < m_controllers.size() ? &m_controllers.at(m_selected) : nullptr;
}

void DeckLabSetupWizard::skipToDone() {
    m_skippedController = true;
    m_mappingApplied = false;
    goTo(Step::Done);
}

void DeckLabSetupWizard::next() {
    if (!m_system) {
        finish(false);
        return;
    }
    switch (m_step) {
    case Step::Welcome:
        goTo(Step::Wifi);
        return;
    case Step::Wifi:
        goTo(Step::Time);
        return;
    case Step::Time:
        goTo(Step::Controller);
        return;
    case Step::Controller: {
        m_skippedController = false;
        const auto* info = selected();
        if (!info) {
            goTo(Step::Audio); // set up audio only
            return;
        }
        if (info->mixxxName.isEmpty()) {
            setMessage(tr("Mixxx hasn't detected %1 yet. Tap Detect again.").arg(info->name), true);
            return;
        }
        if (m_mapping.isEmpty()) {
            setMessage(tr("Choose a mapping for %1 first.").arg(info->name), true);
            return;
        }
        if (!(info->open && sameFile(info->mapping, m_mapping))) {
            QString error;
            if (!m_system->applyMapping(info->mixxxName, m_mapping, &error)) {
                setMessage(error, true);
                return;
            }
        }
        m_mappingApplied = true;
        m_suggestedDecks = m_system->suggestedDeckCount(m_mapping);
        m_decks = m_suggestedDecks;
        goTo(Step::Audio);
        return;
    }
    case Step::Audio: {
        if (!m_audio.main.isValid()) {
            if (m_audioDevices.isEmpty()) {
                goTo(Step::Layout);
                setMessage(tr("No audio output yet. Run Deck Setup again after connecting one."), true);
            } else {
                setMessage(tr("Choose a Main output."), true);
            }
            return;
        }
        const auto current = m_system->currentAudioPlan();
        const bool unchanged = current.main == m_audio.main && current.headphones == m_audio.headphones &&
                m_system->audioOutputConfigured();
        if (!unchanged) {
            if (m_system->anyPlaying()) {
                setMessage(tr("Stop all decks first: changing outputs interrupts the sound."), true);
                return;
            }
            QString error;
            if (!m_system->applyAudio(m_audio, &error)) {
                setMessage(error, true);
                return;
            }
        }
        goTo(Step::Layout);
        return;
    }
    case Step::Layout:
        goTo(Step::Done);
        return;
    case Step::Done: {
        const auto* info = selected();
        decklab::DeviceProfile profile;
        if (info && m_mappingApplied) {
            profile.key = info->key;
            profile.name = info->name;
            profile.controller = info->mixxxName;
            profile.mapping = m_mapping;
        }
        profile.decks = m_decks;
        profile.mainDevice = m_audio.main.device;
        profile.mainBase = m_audio.main.channelBase;
        profile.headphonesDevice = m_audio.headphones.device;
        profile.headphonesBase = m_audio.headphones.channelBase;
        m_system->rememberSetup(profile);
        // r5-polish: finishing Deck Setup completes setup, first run or not
        // (Wi-Fi and the time zone stay in Settings › Unit).
        if (!m_system->firstRunDone()) {
            m_system->markFirstRunDone();
        }
        emit layoutChosen(m_decks);
        emit playRequested();
        finish(true);
        return;
    }
    }
}

void DeckLabSetupWizard::finish(bool completed) {
    hide();
    emit finished(completed);
    deleteLater();
}

// --------------------------------------------------------------- Welcome --
void DeckLabSetupWizard::buildWelcome(QWidget* page) {
    auto* layout = vbox(page, 0, 12);
    layout->setContentsMargins(48, 24, 48, 12);
    layout->addStretch(1);
    auto* heading = new QLabel(tr("Welcome to Deck Lab"), page);
    heading->setObjectName(QStringLiteral("decklab-welcome-title"));
    // Hero type (36 px Barlow Semi Condensed SemiBold), not an off-scale 28.
    heading->setProperty("role", QStringLiteral("hero"));
    layout->addWidget(heading);
    layout->addWidget(body(tr("Let's get you playing. This takes about two minutes."), page));
    const QList<QPair<Icon, QString>> steps{
            {Icon::Wifi, tr("Wi-Fi, if you want internet (optional)")},
            {Icon::Clock, tr("Your time zone")},
            {Icon::Controller, tr("Your controller: plug it in now, or later")},
            {Icon::Speaker, tr("Speakers and headphones")},
    };
    auto* list = card(page);
    auto* listLayout = vbox(list, 6, 0);
    for (const auto& step : steps) {
        auto* row = new Row(step.second, QString(), list);
        row->setRowIcon(step.first);
        row->setStatic(true);
        listLayout->addWidget(row);
    }
    layout->addWidget(list);
    layout->addWidget(muted(tr("You can change all of this later in Settings."), page));
    layout->addStretch(2);
}

// ------------------------------------------------------------ Controller --
void DeckLabSetupWizard::buildController(QWidget* page) {
    auto* layout = vbox(page, 0, 0);
    m_controllerStack = new QStackedWidget(page);
    layout->addWidget(m_controllerStack);

    m_controllerMain = new QWidget(m_controllerStack);
    auto* columns = hbox(16);
    columns->setContentsMargins(16, 14, 16, 8);
    m_controllerMain->setLayout(columns);
    auto* left = new QWidget(m_controllerMain);
    left->setFixedWidth(340);
    auto* leftLayout = vbox(left, 0, 8);
    leftLayout->addWidget(sectionLabel(tr("CONNECTED CONTROLLERS"), left));
    m_controllerList = new QWidget(left);
    m_controllerLayout = vbox(m_controllerList, 0, 0);
    leftLayout->addWidget(scrollArea(m_controllerList, left, QStringLiteral("decklab-setup-controllers")), 1);
    auto* detectRow = hbox(8);
    m_detect = button(tr("Detect again"), left, QStringLiteral("decklab-setup-detect"), 48);
    connect(m_detect, &QPushButton::clicked, this, [this] {
        QString reason;
        if (!m_system || !m_system->rescanControllers(&reason)) {
            setMessage(reason, true);
            return;
        }
        m_detect->setEnabled(false);
        m_detect->setText(tr("Detecting…"));
        setMessage(tr("Looking for controllers. They pause for a moment."));
    });
    detectRow->addWidget(m_detect, 1);
    m_restart = button(tr("Restart to detect"), left, QStringLiteral("decklab-setup-restart"), 48);
    connect(m_restart, &QPushButton::clicked, this, [this] {
        Confirm::ask(window(), tr("Restart Deck Lab to detect it?"),
                tr("The DJ software restarts (about 15 seconds) and Deck Setup continues."),
                tr("Restart"), [this] {
                    QString reason;
                    if (!m_system || !m_system->restartToDetect(&reason)) {
                        setMessage(reason, true);
                    }
                });
    });
    detectRow->addWidget(m_restart, 1);
    leftLayout->addLayout(detectRow);
    columns->addWidget(left);

    auto* mapping = card(m_controllerMain);
    auto* mappingLayout = vbox(mapping, 18, 8);
    mappingLayout->addWidget(sectionLabel(tr("MAPPING"), mapping));
    m_mappingTitle = title(QString(), mapping);
    m_mappingTitle->setObjectName(QStringLiteral("decklab-setup-mapping"));
    m_mappingTitle->setWordWrap(true);
    mappingLayout->addWidget(m_mappingTitle);
    m_mappingReason = body(QString(), mapping);
    mappingLayout->addWidget(m_mappingReason);
    mappingLayout->addStretch(1);
    mappingLayout->addWidget(muted(tr("A mapping tells Deck Lab what every button, knob and jog wheel on "
                                      "your controller does."),
            mapping));
    m_change = button(tr("Change mapping…"), mapping, QStringLiteral("decklab-setup-change-mapping"), 48);
    connect(m_change, &QPushButton::clicked, this, &DeckLabSetupWizard::showMappingPicker);
    mappingLayout->addWidget(m_change);
    columns->addWidget(mapping, 1);
    m_controllerStack->addWidget(m_controllerMain);

    m_controllerEmpty = new QWidget(m_controllerStack);
    auto* empty = vbox(m_controllerEmpty, 40, 12);
    empty->addStretch(1);
    auto* emptyTitle = title(tr("Plug in your controller"), m_controllerEmpty);
    emptyTitle->setAlignment(Qt::AlignHCenter);
    empty->addWidget(emptyTitle);
    auto* emptyText = body(tr("Connect it with its USB cable and switch it on. Deck Lab finds it automatically. "
                              "Any controller Mixxx supports will work."),
            m_controllerEmpty);
    emptyText->setAlignment(Qt::AlignHCenter);
    empty->addWidget(emptyText);
    auto* emptyActions = hbox(10);
    emptyActions->addStretch(1);
    auto* emptyDetect = button(tr("Detect again"), m_controllerEmpty, QStringLiteral("decklab-setup-detect-empty"), 48);
    emptyDetect->setFixedWidth(200);
    connect(emptyDetect, &QPushButton::clicked, m_detect, &QPushButton::click);
    emptyActions->addWidget(emptyDetect);
    emptyActions->addStretch(1);
    empty->addLayout(emptyActions);
    auto* skip = muted(tr("No controller yet? Tap Next to set up audio only, or set up later in "
                          "Settings → Setup → Deck Setup."),
            m_controllerEmpty);
    skip->setAlignment(Qt::AlignHCenter);
    empty->addWidget(skip);
    auto* laterRow = hbox(10);
    laterRow->addStretch(1);
    m_skipLater = button(tr("Skip – set up later"), m_controllerEmpty, QStringLiteral("decklab-setup-skip-later"), 48);
    m_skipLater->setFixedWidth(240);
    connect(m_skipLater, &QPushButton::clicked, this, &DeckLabSetupWizard::skipToDone);
    laterRow->addWidget(m_skipLater);
    laterRow->addStretch(1);
    empty->addLayout(laterRow);
    empty->addStretch(2);
    m_controllerStack->addWidget(m_controllerEmpty);
}

void DeckLabSetupWizard::refreshControllers() {
    QString keep = m_preferredKey;
    if (keep.isEmpty() && selected()) {
        keep = selected()->key;
    }
    m_controllers = m_system ? m_system->controllers() : QList<DeckLabSystem::ControllerInfo>();
    clearLayout(m_controllerLayout);
    m_skipLater->setVisible(m_firstRun);
    if (m_controllers.isEmpty()) {
        m_selected = -1;
        m_controllerStack->setCurrentWidget(m_controllerEmpty);
        return;
    }
    m_controllerStack->setCurrentWidget(m_controllerMain);
    int choice = -1;
    for (int index = 0; index < m_controllers.size(); ++index) {
        if (!keep.isEmpty() && m_controllers.at(index).key == keep) {
            choice = index;
        }
    }
    if (choice < 0) {
        // Prefer a controller that is not working yet: that's why we're here.
        for (int index = 0; index < m_controllers.size() && choice < 0; ++index) {
            if (!m_controllers.at(index).open) {
                choice = index;
            }
        }
    }
    m_selected = choice < 0 ? 0 : choice;
    for (int index = 0; index < m_controllers.size(); ++index) {
        const auto& info = m_controllers.at(index);
        QString state;
        if (info.mixxxName.isEmpty()) {
            state = tr("Not detected yet");
        } else if (info.open && !info.mapping.isEmpty()) {
            state = tr("Working · %1").arg(QFileInfo(info.mapping).completeBaseName().section(QLatin1Char('.'), 0, 0));
        } else {
            state = tr("Ready to set up");
        }
        auto* row = new Row(info.name, state, m_controllerList, QStringLiteral("decklab-setup-controller"));
        row->setRowIcon(Icon::Controller);
        row->setTags({info.hid ? tr("HID") : tr("MIDI")});
        row->setSelectedRow(index == m_selected);
        connect(row, &QPushButton::clicked, this, [this, index] { selectController(index); });
        m_controllerLayout->addWidget(row);
    }
    m_controllerLayout->addStretch(1);
    selectController(m_selected);
}

void DeckLabSetupWizard::selectController(int index) {
    if (index < 0 || index >= m_controllers.size()) {
        return;
    }
    const bool changed = m_selected != index || m_mapping.isEmpty();
    m_selected = index;
    int position = 0;
    for (int item = 0; item < m_controllerLayout->count(); ++item) {
        if (auto* row = qobject_cast<Row*>(m_controllerLayout->itemAt(item)->widget())) {
            row->setSelectedRow(position++ == index);
        }
    }
    const auto& info = m_controllers.at(index);
    if (changed) {
        m_mapping.clear();
        m_mappingName.clear();
        m_mappingScore = 0;
        if (!info.mapping.isEmpty() && QFileInfo::exists(info.mapping)) {
            m_mapping = info.mapping;
            m_mappingName = MappingInfo(info.mapping).getName();
            m_mappingScore = -1; // in use
        } else if (m_system) {
            const auto matches = m_system->suggestMappings(info);
            if (!matches.isEmpty()) {
                m_mapping = matches.first().mapping.getPath();
                m_mappingName = matches.first().mapping.getName();
                m_mappingScore = matches.first().score;
            }
        }
        m_audioChosen = m_audioForKey == info.key && m_audioChosen;
    }
    m_restart->setVisible(info.mixxxName.isEmpty() && m_system && m_system->isReady());
    renderMapping();
}

void DeckLabSetupWizard::renderMapping() {
    const auto* info = selected();
    if (!info) {
        return;
    }
    m_change->setText(m_mapping.isEmpty() ? tr("Choose mapping…") : tr("Change mapping…"));
    QString reason;
    if (m_mapping.isEmpty()) {
        m_mappingTitle->setText(tr("No mapping found"));
        reason = tr("None of the bundled mappings matched %1. Choose it from the list.").arg(info->name);
    } else {
        m_mappingTitle->setText(m_mappingName);
        if (m_mappingScore < 0) {
            reason = tr("In use for %1.").arg(info->name);
        } else if (m_mappingScore == 0) {
            reason = tr("Chosen by you.");
        } else if (m_mappingScore >= ControllerMappingMatcher::kExactScore) {
            reason = tr("Made for %1.").arg(info->name);
        } else if (m_mappingScore >= ControllerMappingMatcher::kConfidentScore) {
            reason = tr("Matches %1.").arg(info->name);
        } else {
            reason = tr("Probably right for %1. Check the name matches your controller.").arg(info->name);
        }
        if (m_system && m_system->suggestedDeckCount(m_mapping) == 4) {
            reason += QLatin1Char(' ') + tr("Supports four decks.");
        }
    }
    if (info->mixxxName.isEmpty()) {
        reason += QStringLiteral("\n\n") +
                tr("Mixxx hasn't detected this controller yet. Tap Detect again.");
    }
    m_mappingReason->setText(reason);
}

// ---------------------------------------------------------------- Picker --
void DeckLabSetupWizard::buildPicker(QWidget* page) {
    auto* layout = vbox(page, 0, 6);
    layout->setContentsMargins(16, 10, 16, 8);
    auto* top = hbox(8);
    auto* cancel = button(tr("Cancel"), page, QStringLiteral("decklab-setup-picker-cancel"), 44);
    cancel->setFixedWidth(110);
    connect(cancel, &QPushButton::clicked, this, [this] { showControllerPage(); });
    top->addWidget(cancel);
    m_search = new QLineEdit(page);
    m_search->setObjectName(QStringLiteral("decklab-setup-search"));
    m_search->setPlaceholderText(tr("Search mappings, e.g. FLX4"));
    m_search->installEventFilter(this);
    connect(m_search, &QLineEdit::textChanged, this, [this] { renderPicker(); });
    top->addWidget(m_search, 1);
    layout->addLayout(top);
    m_pickerList = new QWidget(page);
    m_pickerLayout = vbox(m_pickerList, 0, 0);
    layout->addWidget(scrollArea(m_pickerList, page, QStringLiteral("decklab-setup-mappings")), 1);
    m_keyboard = new Keyboard(page);
    m_keyboard->setTarget(m_search);
    m_keyboard->hide();
    connect(m_keyboard, &Keyboard::done, m_keyboard, &QWidget::hide);
    layout->addWidget(m_keyboard);
}

void DeckLabSetupWizard::showMappingPicker() {
    const auto* info = selected();
    if (!info || !m_system) {
        return;
    }
    m_footer->hide();
    m_pages->setCurrentIndex(kPickerIndex);
    m_search->clear();
    renderPicker();
    update();
}

void DeckLabSetupWizard::renderPicker() {
    const auto* info = selected();
    if (!info || !m_system) {
        return;
    }
    clearLayout(m_pickerLayout);
    QList<MappingInfo> all = m_system->mappings(info->hid);
    QStringList suggested;
    for (const auto& match : m_system->suggestMappings(*info)) {
        suggested.append(match.mapping.getPath());
    }
    std::stable_sort(all.begin(), all.end(), [&suggested](const MappingInfo& left, const MappingInfo& right) {
        const int a = suggested.indexOf(left.getPath());
        const int b = suggested.indexOf(right.getPath());
        if ((a >= 0) != (b >= 0)) {
            return a >= 0;
        }
        if (a >= 0) {
            return a < b;
        }
        return left.getName().compare(right.getName(), Qt::CaseInsensitive) < 0;
    });
    const QString filter = m_search->text().trimmed();
    int shown = 0;
    QStringList seen;
    for (const auto& mapping : std::as_const(all)) {
        if (seen.contains(mapping.getPath())) {
            continue;
        }
        seen.append(mapping.getPath());
        if (!filter.isEmpty() && !mapping.getName().contains(filter, Qt::CaseInsensitive) &&
                !QFileInfo(mapping.getPath()).fileName().contains(filter, Qt::CaseInsensitive)) {
            continue;
        }
        const QString author = mapping.getAuthor().trimmed();
        auto* row = new Row(mapping.getName(),
                author.isEmpty() ? QFileInfo(mapping.getPath()).fileName() : tr("by %1").arg(author.left(60)),
                m_pickerList,
                QStringLiteral("decklab-setup-mapping-choice"));
        QStringList tags;
        if (suggested.contains(mapping.getPath())) {
            tags.append(tr("Suggested"));
        }
        if (sameFile(mapping.getPath(), m_mapping)) {
            row->setSelectedRow(true);
        }
        row->setTags(tags);
        const QString path = mapping.getPath();
        const QString name = mapping.getName();
        connect(row, &QPushButton::clicked, this, [this, path, name] {
            m_mapping = path;
            m_mappingName = name;
            m_mappingScore = 0;
            showControllerPage();
            renderMapping();
        });
        m_pickerLayout->addWidget(row);
        if (++shown >= 120) {
            break;
        }
    }
    if (shown == 0) {
        m_pickerLayout->addWidget(muted(tr("No mapping matches “%1”.").arg(filter), m_pickerList));
    }
    m_pickerLayout->addStretch(1);
}

// ----------------------------------------------------------------- Audio --
// r4-system (Signature 5): a sound check, not a form. Main and Headphones are
// two panels, each with its output, big tone pads whose meter moves with the
// tone and, after a tone, YES / NO. The first tone plays at -24 dBFS; a
// second tap on the same pad adds 6 dB. The levels warning is always there.
void DeckLabSetupWizard::buildAudio(QWidget* page) {
    auto* layout = vbox(page, 0, 8);
    layout->setContentsMargins(16, 10, 16, 6);
    auto* top = hbox(12);
    auto* heading = new QLabel(tr("SOUND CHECK"), page);
    heading->setStyleSheet(QStringLiteral("font-size:14px;font-weight:600;color:#9AA3AF;"));
    top->addWidget(heading);
    // Test tones may play on a club PA or straight into headphones. They start
    // quiet (decklab::kTestToneDbfs), and the levels warning is always shown.
    auto* levels = new QLabel(tr("Turn MASTER and PHONES down before you test."), page);
    levels->setObjectName(QStringLiteral("decklab-setup-level-warning"));
    levels->setProperty("role", QStringLiteral("caution"));
    levels->setWordWrap(true);
    top->addWidget(levels, 1);
    layout->addLayout(top);
    auto* panels = hbox(8);
    const auto addPanel = [this, page, panels](bool headphones) {
        auto* panel = card(page);
        panel->setObjectName(headphones ? QStringLiteral("decklab-setup-phones-panel")
                                        : QStringLiteral("decklab-setup-main-panel"));
        auto* column = vbox(panel, 12, 6);
        auto* name = new QLabel(headphones ? tr("HEADPHONES") : tr("MAIN"), panel);
        name->setProperty("role", QStringLiteral("label"));
        column->addWidget(name);
        auto* combo = touchCombo(panel, headphones ? QStringLiteral("decklab-setup-phones") : QStringLiteral("decklab-setup-main"));
        column->addWidget(combo);
        auto* pads = hbox(8);
        if (headphones) {
            auto* test = new DeckLabTonePad(tr("Test"), panel, QStringLiteral("decklab-setup-test-phones"));
            connect(test, &QPushButton::clicked, this, [this] { testTone(true, -1); });
            m_testButtons.append(test);
            m_pads.append(test);
            pads->addWidget(test);
        } else {
            auto* left = new DeckLabTonePad(tr("Left"), panel, QStringLiteral("decklab-setup-test-left"));
            connect(left, &QPushButton::clicked, this, [this] { testTone(false, 0); });
            auto* right = new DeckLabTonePad(tr("Right"), panel, QStringLiteral("decklab-setup-test-right"));
            connect(right, &QPushButton::clicked, this, [this] { testTone(false, 1); });
            for (auto* pad : {left, right}) {
                m_testButtons.append(pad);
                m_pads.append(pad);
                pads->addWidget(pad);
            }
        }
        column->addLayout(pads);
        auto* question = new QLabel(panel);
        question->setObjectName(headphones ? QStringLiteral("decklab-setup-question-phones")
                                           : QStringLiteral("decklab-setup-question-main"));
        question->setProperty("role", QStringLiteral("body"));
        question->setText(headphones ? tr("Tap Test. A second tap is 6 dB louder.")
                                     : tr("Tap Left, then Right. A second tap is 6 dB louder."));
        column->addWidget(question);
        auto* answers = new QWidget(panel);
        auto* answerRow = hbox(8);
        answers->setLayout(answerRow);
        auto* yes = primaryButton(tr("Yes"), answers,
                headphones ? QStringLiteral("decklab-setup-heard-phones") : QStringLiteral("decklab-setup-heard-main"), 44);
        auto* no = button(headphones ? tr("No, try next") : tr("No, try next"), answers,
                headphones ? QStringLiteral("decklab-setup-not-heard-phones") : QStringLiteral("decklab-setup-not-heard-main"), 44);
        connect(yes, &QPushButton::clicked, this, [this, headphones] { answerTone(headphones, true); });
        connect(no, &QPushButton::clicked, this, [this, headphones] { answerTone(headphones, false); });
        answerRow->addWidget(yes, 1);
        answerRow->addWidget(no, 1);
        QSizePolicy keep = answers->sizePolicy();
        keep.setRetainSizeWhenHidden(true);
        answers->setSizePolicy(keep);
        answers->hide();
        column->addWidget(answers);
        (headphones ? m_phonesQuestion : m_mainQuestion) = question;
        (headphones ? m_phonesAnswers : m_mainAnswers) = answers;
        panels->addWidget(panel, 1);
        return combo;
    };
    m_main = addPanel(false);
    m_phones = addPanel(true);
    layout->addLayout(panels);
    connect(m_main, qOverload<int>(&QComboBox::activated), this, [this](int) { audioChoiceChanged(); });
    connect(m_phones, qOverload<int>(&QComboBox::activated), this, [this](int) { audioChoiceChanged(); });
    m_audioReason = muted(QString(), page);
    m_audioReason->setObjectName(QStringLiteral("decklab-setup-audio-reason"));
    m_audioReason->setWordWrap(false);
    layout->addWidget(m_audioReason);
    m_audioWarning = new QLabel(page);
    m_audioWarning->setObjectName(QStringLiteral("decklab-setup-audio-warning"));
    // Advice (Bluetooth lag, shared outputs), not an error.
    m_audioWarning->setProperty("role", QStringLiteral("caution"));
    m_audioWarning->setWordWrap(true);
    layout->addWidget(m_audioWarning);
    layout->addStretch(1);
}

void DeckLabSetupWizard::showQuestion(bool headphones, const QString& question, bool answers) {
    (headphones ? m_phonesQuestion : m_mainQuestion)->setText(question);
    (headphones ? m_phonesAnswers : m_mainAnswers)->setVisible(answers);
}

void DeckLabSetupWizard::answerTone(bool headphones, bool heard) {
    if (heard) {
        showQuestion(headphones, headphones ? tr("Headphones checked.") : tr("Main checked."), false);
        return;
    }
    // "No" moves to the next output pair, ready for another tone.
    QComboBox* combo = headphones ? m_phones : m_main;
    const int first = headphones ? 1 : 0; // headphones: index 0 is Off
    const int count = combo->count() - first;
    if (count > 1) {
        const int next = first + (std::max(0, combo->currentIndex() - first) + 1) % count;
        combo->setCurrentIndex(next);
        audioChoiceChanged();
        m_lastTone.clear();
        showQuestion(headphones, tr("Now on %1. Tap again.").arg(combo->currentText()), false);
    } else {
        showQuestion(headphones, tr("No other output. Check the cable and the volume."), false);
    }
}

void DeckLabSetupWizard::enterAudio() {
    const auto* info = selected();
    m_audioDevices = m_system ? m_system->audioDevices(info) : QList<decklab::AudioDevice>();
    const QString key = info ? info->key : QString();
    const decklab::AudioPlan policy = decklab::planAudio(m_audioDevices);
    if (!m_audioChosen || m_audioForKey != key) {
        // Start from the policy; keep a working current setup when the
        // policy has nothing better to offer.
        m_audio = policy;
        const auto current = m_system ? m_system->currentAudioPlan() : decklab::AudioPlan();
        if (!policy.main.isValid() && current.main.isValid()) {
            m_audio = current;
        }
        m_audioChosen = true;
        m_audioForKey = key;
    }
    m_audioReason->setText(policy.reason);
    fillAudioCombos();
    audioChoiceChanged();
}

void DeckLabSetupWizard::fillAudioCombos() {
    for (auto* combo : {m_main, m_phones}) {
        combo->clear();
    }
    m_phones->addItem(tr("Off"), QString());
    if (m_audioDevices.isEmpty()) {
        m_main->addItem(tr("No audio output found"), QString());
    }
    for (const auto& device : std::as_const(m_audioDevices)) {
        for (int base = 0; base + 2 <= device.outputs; base += 2) {
            QString text = device.outputs > 2
                    ? tr("%1 · outputs %2").arg(device.name, decklab::channelPairLabel(base))
                    : device.name;
            if (decklab::isHighLatency(device)) {
                text += tr("  (high latency)");
            }
            const QString data = routeData(decklab::AudioRoute{device.id, base});
            m_main->addItem(text, data);
            m_phones->addItem(text, data);
        }
    }
    const int main = m_main->findData(routeData(m_audio.main));
    m_main->setCurrentIndex(main >= 0 ? main : 0);
    const int phones = m_phones->findData(routeData(m_audio.headphones));
    m_phones->setCurrentIndex(phones >= 0 ? phones : 0);
    m_main->setEnabled(!m_audioDevices.isEmpty());
    m_phones->setEnabled(!m_audioDevices.isEmpty());
}

void DeckLabSetupWizard::audioChoiceChanged() {
    m_audio.main = routeFrom(m_main->currentData());
    m_audio.headphones = routeFrom(m_phones->currentData());
    QStringList warnings;
    for (const auto& device : std::as_const(m_audioDevices)) {
        if (device.id == m_audio.main.device && decklab::isHighLatency(device)) {
            warnings.append(tr("Bluetooth and system audio lag behind: fine for practice, not for mixing."));
        }
    }
    if (m_audio.headphones.isValid() && m_audio.headphones == m_audio.main) {
        warnings.append(tr("Main and headphones use the same outputs; you'll hear your cue on the main speakers."));
    }
    if (m_audioDevices.isEmpty()) {
        warnings.append(tr("No sound output found. Connect your controller's USB or a USB sound card."));
    }
    m_audioWarning->setText(warnings.join(QLatin1Char('\n')));
    m_audioWarning->setVisible(!warnings.isEmpty());
    for (auto* test : std::as_const(m_testButtons)) {
        const bool phones = test->objectName() == QStringLiteral("decklab-setup-test-phones");
        test->setEnabled((phones ? m_audio.headphones : m_audio.main).isValid() && m_system &&
                !m_system->isTestingTone());
    }
}

void DeckLabSetupWizard::testTone(bool headphones, int side) {
    if (!m_system) {
        return;
    }
    const decklab::AudioRoute route = headphones ? m_audio.headphones : m_audio.main;
    // A second tap on the same pad, same output, adds 6 dB (never more).
    const QString key = QStringLiteral("%1/%2/%3:%4").arg(headphones).arg(side).arg(route.device).arg(route.channelBase);
    m_toneDbfs = key == m_lastTone ? decklab::kTestToneBoostDbfs : decklab::kTestToneDbfs;
    m_lastTone = key;
    for (auto* test : std::as_const(m_testButtons)) {
        test->setEnabled(false);
    }
    const QString padName = headphones ? QStringLiteral("decklab-setup-test-phones")
            : side == 0                ? QStringLiteral("decklab-setup-test-left")
                                       : QStringLiteral("decklab-setup-test-right");
    for (auto* pad : std::as_const(m_pads)) {
        if (pad->objectName() == padName) {
            m_playingPad = pad;
            pad->setPlaying(true);
        }
    }
    m_toneSince.start();
    m_toneMeter.start();
    showQuestion(headphones, m_toneDbfs > decklab::kTestToneDbfs ? tr("Playing, 6 dB louder…") : tr("Playing…"), false);
    setMessage(QString());
    QPointer<DeckLabSetupWizard> self(this);
    m_system->playTestTone(
            route,
            side,
            [self, headphones, side](bool ok, const QString& error) {
                if (!self) {
                    return;
                }
                self->m_toneMeter.stop();
                if (self->m_playingPad) {
                    self->m_playingPad->setPlaying(false);
                }
                self->m_playingPad = nullptr;
                self->audioChoiceChanged();
                if (!ok) {
                    self->m_lastTone.clear();
                    self->showQuestion(headphones, error, false);
                    return;
                }
                self->showQuestion(headphones,
                        headphones ? tr("Heard it in the phones?")
                                : side == 0 ? tr("Only the LEFT speaker?")
                                            : tr("Only the RIGHT speaker?"),
                        true);
            },
            m_toneDbfs);
}

// ---------------------------------------------------------------- Layout --
void DeckLabSetupWizard::buildLayout(QWidget* page) {
    auto* layout = vbox(page, 0, 10);
    layout->setContentsMargins(16, 14, 16, 8);
    m_layoutReason = body(QString(), page);
    layout->addWidget(m_layoutReason);
    auto* choices = hbox(16);
    auto* two = new LayoutChoice(2, tr("Two decks"), tr("Big waveforms, the classic setup"), page,
            QStringLiteral("decklab-setup-two-decks"));
    auto* four = new LayoutChoice(4, tr("Four decks"), tr("Decks 3 and 4 on the controller's layers"), page,
            QStringLiteral("decklab-setup-four-decks"));
    m_two = two;
    m_four = four;
    auto* group = new QButtonGroup(page);
    group->setExclusive(true);
    group->addButton(two, 2);
    group->addButton(four, 4);
    connect(group, &QButtonGroup::idClicked, this, [this](int decks) { m_decks = decks; });
    choices->addWidget(two, 1);
    choices->addWidget(four, 1);
    layout->addLayout(choices, 1);
}

void DeckLabSetupWizard::renderLayout() {
    const auto* info = selected();
    static_cast<LayoutChoice*>(m_two)->setSuggested(m_suggestedDecks == 2);
    static_cast<LayoutChoice*>(m_four)->setSuggested(m_suggestedDecks == 4);
    m_two->setChecked(m_decks != 4);
    m_four->setChecked(m_decks == 4);
    if (!info || m_mapping.isEmpty()) {
        m_layoutReason->setText(tr("How many decks should the screen show?"));
    } else if (m_suggestedDecks == 4) {
        m_layoutReason->setText(tr("The %1 mapping can switch between four decks.").arg(info->name));
    } else {
        m_layoutReason->setText(tr("The %1 mapping plays two decks.").arg(info->name));
    }
}

// ------------------------------------------------------------------ Done --
void DeckLabSetupWizard::buildDone(QWidget* page) {
    auto* layout = vbox(page, 0, 10);
    layout->setContentsMargins(16, 14, 16, 8);
    m_doneTitle = title(tr("You're ready to play"), page);
    layout->addWidget(m_doneTitle);
    m_summary = card(page);
    m_summaryLayout = vbox(m_summary, 6, 0);
    layout->addWidget(m_summary);
    layout->addStretch(1);
}

void DeckLabSetupWizard::renderDone() {
    clearLayout(m_summaryLayout);
    const auto* info = selected();
    const auto add = [this](Icon icon, const QString& heading, const QString& detail, bool ok) {
        auto* row = new Row(heading, detail, m_summary, QStringLiteral("decklab-setup-summary"));
        row->setRowIcon(ok ? icon : Icon::Warning);
        row->setTags(ok ? QStringList{tr("Done")} : QStringList{tr("Skipped")});
        row->setStatic(true);
        m_summaryLayout->addWidget(row);
    };
    if (info && m_mappingApplied) {
        add(Icon::Controller, info->name, tr("%1 mapping is on").arg(m_mappingName), true);
    } else {
        add(Icon::Controller, tr("Controller"), tr("Not set up"), false);
    }
    QString mainName;
    QString phonesName;
    for (const auto& device : std::as_const(m_audioDevices)) {
        if (device.id == m_audio.main.device) {
            mainName = device.name;
        }
        if (device.id == m_audio.headphones.device) {
            phonesName = device.name;
        }
    }
    add(Icon::Speaker, tr("Main"),
            m_audio.main.isValid() ? tr("%1 · outputs %2").arg(mainName, decklab::channelPairLabel(m_audio.main.channelBase))
                                   : tr("No output"),
            m_audio.main.isValid());
    add(Icon::Headphones, tr("Headphones"),
            m_audio.headphones.isValid()
                    ? tr("%1 · outputs %2").arg(phonesName, decklab::channelPairLabel(m_audio.headphones.channelBase))
                    : tr("Off"),
            m_audio.headphones.isValid());
    add(Icon::Decks, tr("Layout"), m_decks == 4 ? tr("Four decks") : tr("Two decks"), true);
    m_doneTitle->setText(m_firstRun ? tr("Deck Lab is ready") : tr("You're ready to play"));
    setMessage(info && m_mappingApplied
                    ? tr("Saved. %1 is set up automatically when you plug it in.").arg(info->name)
                    : tr("Add a controller any time: Settings → Setup → Deck Setup."));
}
