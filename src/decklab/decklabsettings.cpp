// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklabsettings.h"

#include <QBoxLayout>
#include <QComboBox>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTime>
#include <algorithm>
#include <cmath>

#include "decklab/decklabsystemui.h"
#include "decklab/decklabui.h"
#include "decklab/decklabutility.h"
#include "engine/decklabplaystart.h"
#include "moc_decklabsettings.cpp"
#include "waveform/waveformwidgetfactory.h"
#include "widget/decklabdeckwidgets.h"
#include "widget/decklabstempads.h"

using namespace decklab::ui;

namespace {
// Header of every category: the title and what the knob does here (the
// rail and the list teach the controller, TE style).
class CategoryHeader : public QWidget {
  public:
    CategoryHeader(const QString& title, QWidget* parent)
            : QWidget(parent),
              m_title(title) {
        setFixedHeight(44);
        setAccessibleName(title);
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(kGround));
        painter.fillRect(QRect(0, height() - 1, width(), 1), QColor(kLine));
        painter.setFont(uiFont(font(), 18, QFont::DemiBold));
        painter.setPen(QColor(kText));
        painter.drawText(QRect(16, 0, width() / 2, height()), Qt::AlignLeft | Qt::AlignVCenter, m_title);
        painter.setFont(capsFont(font(), 14));
        painter.setPen(QColor(kMuted));
        painter.drawText(QRect(width() / 2, 0, width() / 2 - 16, height()), Qt::AlignRight | Qt::AlignVCenter,
                DeckLabSettingsPage::tr("Knob: turn · push · back"));
    }

  private:
    QString m_title;
};

QString waveformName(WaveformWidgetType::Type type, const QString& fallback) {
    switch (type) {
    case WaveformWidgetType::Simple:
        return DeckLabSettingsPage::tr("One colour");
    case WaveformWidgetType::Filtered:
        return DeckLabSettingsPage::tr("Three bands");
    case WaveformWidgetType::HSV:
        return DeckLabSettingsPage::tr("Colour by energy");
    case WaveformWidgetType::RGB:
        return DeckLabSettingsPage::tr("Colour by frequency");
    case WaveformWidgetType::Stacked:
        return DeckLabSettingsPage::tr("Stacked bands");
    default:
        return fallback;
    }
}
} // namespace

DeckLabSettingsPage::DeckLabSettingsPage(
        UserSettingsPointer config, DeckLabSystemUi* systemUi, Hooks hooks, QWidget* parent)
        : QWidget(parent),
          m_config(std::move(config)),
          m_systemUi(systemUi),
          m_hooks(std::move(hooks)) {
    // Named so the surface sheet and the smoke checks find every control.
    setObjectName(QStringLiteral("decklab-settings"));
    setAttribute(Qt::WA_StyledBackground);
    auto* layout = hbox(0);
    setLayout(layout);
    m_categories = new QWidget(this);
    m_categories->setObjectName(QStringLiteral("decklab-settings-categories"));
    m_categories->setAttribute(Qt::WA_StyledBackground);
    m_categories->setFixedWidth(200);
    auto* links = vbox(m_categories, 0, 0);
    m_pages = new QStackedWidget(this);
    m_pages->setObjectName(QStringLiteral("decklab-settings-pages"));
    const QList<std::pair<Icon, QString>> names{
            {Icon::Controller, tr("Controller & audio")},
            {Icon::Decks, tr("Decks")},
            {Icon::Headphones, tr("Headphones")},
            {Icon::Screen, tr("Display")},
            {Icon::Usb, tr("Library")},
            {Icon::Power, tr("Unit")},
    };
    for (int index = 0; index < names.size(); ++index) {
        QWidget* content = nullptr;
        if (index == Unit) {
            m_system = m_systemUi ? m_systemUi->createSystemPage(m_pages) : new DeckLabSystemPage(m_pages);
            m_pages->addWidget(m_system);
            connect(m_system, &DeckLabSystemPage::backRequested, this, [this] {
                showCategory(m_lastCategory);
                setLevel(0);
            });
            connect(m_system, &DeckLabSystemPage::sectionChanged, this, [this](DeckLabSystemPage::Section) {
                if (m_level == 1 && m_knob) {
                    m_knob->clear();
                }
            });
        } else {
            m_pages->addWidget(page(index, names.at(index).second, &content));
            switch (index) {
            case ControllerAudio:
                buildControllerAudio(content);
                break;
            case Decks:
                buildDecks(content);
                break;
            case Headphones:
                buildHeadphones(content);
                break;
            case Display:
                buildDisplay(content);
                break;
            case Library:
                buildLibrary(content);
                break;
            default:
                break;
            }
            static_cast<QVBoxLayout*>(content->layout())->addStretch(1);
        }
        auto* link = new CategoryButton(names.at(index).first, names.at(index).second, m_categories,
                QStringLiteral("decklab-settings-category-%1").arg(index));
        connect(link, &QPushButton::clicked, this, [this, index] {
            showCategory(index);
            // A press opens the category: the knob moves into it.
            setLevel(1);
        });
        links->addWidget(link);
        m_links.append(link);
    }
    links->addStretch(1);
    layout->addWidget(m_categories);
    layout->addWidget(m_pages, 1);
    m_knob = new KnobNavigator(this, [this] { return knobBack(); });
    showCategory(Decks);
    setLevel(0);
}

QString DeckLabSettingsPage::channel(int deck) const {
    return QStringLiteral("[Channel%1]").arg(deck);
}

QWidget* DeckLabSettingsPage::page(int category, const QString& heading, QWidget** content) {
    auto* holder = new QWidget(m_pages);
    holder->setObjectName(QStringLiteral("decklab-settings-page-%1").arg(category));
    auto* column = vbox(holder, 0, 0);
    column->addWidget(new CategoryHeader(heading, holder));
    auto* body = new QWidget(holder);
    body->setObjectName(QStringLiteral("decklab-settings-body-%1").arg(category));
    vbox(body, 0, 0);
    // Kinetic touch scrolling that never turns a swipe into a tap.
    column->addWidget(scrollArea(body, holder, QStringLiteral("decklab-settings-scroll-%1").arg(category)), 1);
    *content = body;
    return holder;
}

// ------------------------------------------------------ Controller & audio --

void DeckLabSettingsPage::buildControllerAudio(QWidget* content) {
    auto* rows = static_cast<QVBoxLayout*>(content->layout());
    if (m_systemUi) {
        // Controller, sound and Run Deck Setup (decklabsystemui.cpp).
        rows->addWidget(m_systemUi->createSetupPanel(content));
    }
    rows->addWidget(new SectionRule(tr("Health"), content));
    m_health = new InfoRow(tr("Playback"), content, QStringLiteral("decklab-settings-health"));
    m_health->setValue(tr("Smooth"));
    m_health->setLamp(Lamp::Go);
    rows->addWidget(m_health);
    auto* advanced = new ActionRow(tr("Advanced (desktop)…"), content, QStringLiteral("decklab-advanced-settings"));
    advanced->setHint(tr("The full desktop preferences, made for a mouse"));
    connect(advanced, &QPushButton::clicked, this, [this] {
        Confirm::ask(window(),
                tr("Open the desktop preferences?"),
                tr("These are Mixxx's full preferences: small controls made for a mouse and keyboard. "
                   "Everything you need for a gig is in Deck Lab's own settings."),
                tr("Open"),
                [this] {
                    if (m_hooks.advanced) {
                        m_hooks.advanced();
                    }
                },
                false);
    });
    rows->addWidget(advanced);
}

// ------------------------------------------------------------------ Decks --

void DeckLabSettingsPage::buildDecks(QWidget* content) {
    auto* rows = static_cast<QVBoxLayout*>(content->layout());
    m_layout = new ChoiceRow(tr("Decks on screen"), content, QStringLiteral("decklab-settings-layout"),
            QStringLiteral("decklab-deck-layout"));
    m_layout->addOption(tr("AUTO"), tr("Auto · connected controller"), 0);
    m_layout->addOption(QStringLiteral("2"), tr("Two decks"), 2);
    m_layout->addOption(QStringLiteral("4"), tr("Four decks"), 4);
    m_layout->model()->setCurrentIndex(
            std::max(0, m_layout->model()->findData(m_config->getValue(ConfigKey("[DeckLab]", "DeckLayout"), 0))));
    connect(m_layout->model(), qOverload<int>(&QComboBox::activated), this, [this](int index) {
        if (m_hooks.deckLayoutChosen) {
            m_hooks.deckLayoutChosen(m_layout->model()->itemData(index).toInt());
        }
        refreshLayoutRows();
        refreshValues();
    });
    rows->addWidget(m_layout);
    // Stems workstream: play stem files as stems or as a normal track.
    if (auto* stems = DeckLabStemPads::createPlaybackSetting(m_config, content)) {
        auto* holder = new QWidget(content);
        holder->setObjectName(QStringLiteral("decklab-settings-stems"));
        auto* line = vbox(holder, 0, 0);
        line->setContentsMargins(16, 6, 16, 8);
        line->addWidget(stems);
        rows->addWidget(holder);
    }
    // r6-playstart: does PLAY move the playhead onto another playing deck's
    // beat (Mixxx) or start exactly where the deck is (CDJ)? Every deck, any
    // controller; applied at once, no restart (decklabplaystart.h).
    m_playStart = new ChoiceRow(tr("PLAY starts"), content, QStringLiteral("decklab-settings-play-start"),
            QStringLiteral("decklab-play-start"));
    m_playStart->addOption(tr("EXACT"), tr("Exactly where it is (CDJ)"),
            static_cast<int>(decklab::PlayStartAlign::SyncLockOnly));
    m_playStart->addOption(tr("ON BEAT"), tr("On the other deck's beat (Mixxx)"),
            static_cast<int>(decklab::PlayStartAlign::Quantize));
    // What still lines decks up when PLAY starts exactly. Sync lock and a
    // controller's SYNC need Quantize for the phase (a touch SYNC does not).
    m_playStart->setHint(tr("Quantize on: sync lock or SYNC before PLAY still lines up"));
    const ConfigKey playStartControl = decklab::playStartAlignControlKey();
    const auto showPlayStart = [this](double v) {
        auto* model = m_playStart->model();
        const QSignalBlocker blocker(model);
        model->setCurrentIndex(std::max(0, model->findData(static_cast<int>(decklab::playStartAlignFromValue(v)))));
        m_playStart->update();
    };
    showPlayStart(m_hooks.value ? m_hooks.value(playStartControl.group, playStartControl.item)
                                : m_config->getValue(decklab::playStartAlignConfigKey(), 0));
    if (m_hooks.watch) {
        m_hooks.watch(playStartControl.group, playStartControl.item, showPlayStart);
    }
    connect(m_playStart->model(), qOverload<int>(&QComboBox::activated), this, [this, playStartControl](int index) {
        const int align = m_playStart->model()->itemData(index).toInt();
        m_config->setValue(decklab::playStartAlignConfigKey(), align);
        if (m_hooks.set) {
            m_hooks.set(playStartControl.group, playStartControl.item, align);
        }
    });
    rows->addWidget(m_playStart);
    rows->addWidget(new SectionRule(tr("Per deck"), content));
    m_deckChoice = new ChoiceRow(tr("Deck"), content, QStringLiteral("decklab-settings-deck"),
            QStringLiteral("decklab-settings-deck-model"));
    QList<QColor> colors;
    for (int deck = 1; deck <= 4; ++deck) {
        m_deckChoice->addOption(QString::number(deck), tr("Deck %1").arg(deck), deck);
        colors.append(deckLabDeckColor(deck));
    }
    // Deck identity: the chosen deck fills with its colour.
    m_deckChoice->setSegmentColors(colors);
    connect(m_deckChoice->model(), qOverload<int>(&QComboBox::activated), this, [this](int index) {
        selectDeck(index + 1);
    });
    rows->addWidget(m_deckChoice);
    const auto bindSwitch = [this, content, rows](const QString& label, const QString& name, const QString& item) {
        auto* row = new SwitchRow(label, content, name);
        connect(row, &QPushButton::clicked, this, [this, item](bool checked) {
            if (m_hooks.set) {
                m_hooks.set(channel(m_deck), item, checked ? 1 : 0);
            }
        });
        for (int deck = 1; deck <= 4; ++deck) {
            if (m_hooks.watch) {
                m_hooks.watch(channel(deck), item, [this, row, deck](double v) {
                    if (deck == m_deck) {
                        row->setChecked(v > 0);
                    }
                });
            }
        }
        rows->addWidget(row);
        return row;
    };
    m_quantize = bindSwitch(tr("Quantize"), QStringLiteral("decklab-settings-quantize"), QStringLiteral("quantize"));
    m_keylock = bindSwitch(tr("Key lock"), QStringLiteral("decklab-settings-keylock"), QStringLiteral("keylock"));
    m_slip = bindSwitch(tr("Slip"), QStringLiteral("decklab-settings-slip"), QStringLiteral("slip_enabled"));
    for (int deck = 1; deck <= 4; ++deck) {
        auto* range = new ChoiceRow(tr("Tempo range"), content, QStringLiteral("decklab-settings-tempo-range-%1").arg(deck),
                QStringLiteral("decklab-tempo-range-%1").arg(deck));
        range->model()->setAccessibleName(tr("Deck %1 tempo range").arg(deck));
        for (double amount : {0.08, 0.16, 0.50}) {
            range->addOption(tr("±%1 %").arg(amount * 100), QStringLiteral("±%1%").arg(amount * 100), amount);
        }
        auto* model = range->model();
        const auto showRange = [range, model](double v) {
            if (!std::isfinite(v) || v <= 0) {
                return;
            }
            const QSignalBlocker blocker(model);
            int index = -1;
            for (int n = 0; n < 3; ++n) {
                if (std::abs(model->itemData(n).toDouble() - v) < 1e-6) {
                    index = n;
                }
            }
            if (model->count() > 3) {
                model->removeItem(3);
            }
            if (index < 0) {
                // A range the controller set: shown as is, chosen from here never.
                range->addOption(tr("±%1 %").arg(v * 100, 0, 'g', 4),
                        tr("±%1 % (from controller)").arg(v * 100, 0, 'g', 4), v);
                index = 3;
            }
            model->setCurrentIndex(index);
            range->update();
        };
        const auto lockRange = [this, deck, range, model] {
            const bool playing = m_hooks.value && m_hooks.value(channel(deck), QStringLiteral("play")) > 0;
            const bool synced = m_hooks.value && m_hooks.value(channel(deck), QStringLiteral("sync_enabled")) > 0;
            // Say why it is locked, next to it (touch screens show no tool tips).
            range->setReason(playing ? tr("Locked while deck %1 plays").arg(deck)
                            : synced ? tr("Locked while deck %1 syncs").arg(deck)
                                     : QString());
            model->setEnabled(!playing && !synced);
        };
        if (m_hooks.value) {
            showRange(m_hooks.value(channel(deck), QStringLiteral("rateRange")));
        }
        if (m_hooks.watch) {
            m_hooks.watch(channel(deck), QStringLiteral("rateRange"), showRange);
            m_hooks.watch(channel(deck), QStringLiteral("play"), [lockRange](double) { lockRange(); });
            m_hooks.watch(channel(deck), QStringLiteral("sync_enabled"), [lockRange](double) { lockRange(); });
        }
        connect(model, qOverload<int>(&QComboBox::activated), this, [this, deck, model](int index) {
            const double amount = model->itemData(index).toDouble();
            if (std::isfinite(amount) && amount > 0 && m_hooks.value && m_hooks.set &&
                    !m_hooks.value(channel(deck), QStringLiteral("play")) &&
                    !m_hooks.value(channel(deck), QStringLiteral("sync_enabled"))) {
                m_hooks.set(channel(deck), QStringLiteral("rateRange"), amount);
            }
        });
        lockRange();
        range->setVisible(deck == m_deck);
        m_ranges[deck - 1] = range;
        rows->addWidget(range);
    }
    selectDeck(1);
}

void DeckLabSettingsPage::selectDeck(int deck) {
    m_deck = std::clamp(deck, 1, 4);
    if (m_deckChoice && m_deckChoice->currentIndex() != m_deck - 1) {
        const QSignalBlocker blocker(m_deckChoice->model());
        m_deckChoice->model()->setCurrentIndex(m_deck - 1);
        m_deckChoice->update();
    }
    for (int index = 0; index < 4; ++index) {
        if (m_ranges[index]) {
            m_ranges[index]->setVisible(index + 1 == m_deck);
        }
    }
    refreshDeckRows();
}

void DeckLabSettingsPage::refreshDeckRows() {
    if (!m_hooks.value) {
        return;
    }
    const QColor color = deckLabDeckColor(m_deck);
    for (auto* row : {m_quantize, m_keylock, m_slip}) {
        if (row) {
            row->setBadge(color, QString::number(m_deck));
        }
    }
    if (m_quantize) {
        m_quantize->setChecked(m_hooks.value(channel(m_deck), QStringLiteral("quantize")) > 0);
    }
    if (m_keylock) {
        m_keylock->setChecked(m_hooks.value(channel(m_deck), QStringLiteral("keylock")) > 0);
    }
    if (m_slip) {
        m_slip->setChecked(m_hooks.value(channel(m_deck), QStringLiteral("slip_enabled")) > 0);
    }
}

void DeckLabSettingsPage::refreshLayoutRows() {
    const bool four = m_hooks.fourDecksShown && m_hooks.fourDecksShown();
    if (m_layout) {
        const int automatic = m_hooks.autoDecks ? m_hooks.autoDecks() : 2;
        // Auto says what it chose (S-01).
        m_layout->setSegmentText(0, tr("AUTO (%1)").arg(automatic));
    }
    if (m_deckChoice) {
        m_deckChoice->setVisibleCount(four ? 4 : 2);
        if (!four && m_deck > 2) {
            selectDeck(1);
        }
    }
    for (int deck = 3; deck <= 4; ++deck) {
        if (m_cues[deck - 1]) {
            m_cues[deck - 1]->setVisible(four);
        }
    }
}

// ------------------------------------------------------------- Headphones --

void DeckLabSettingsPage::buildHeadphones(QWidget* content) {
    auto* rows = static_cast<QVBoxLayout*>(content->layout());
    for (int deck = 1; deck <= 4; ++deck) {
        auto* cue = new SwitchRow(tr("Deck %1 cue").arg(deck), content, QStringLiteral("decklab-settings-cue-%1").arg(deck));
        cue->setBadge(deckLabDeckColor(deck), QString::number(deck));
        connect(cue, &QPushButton::clicked, this, [this, deck](bool checked) {
            if (m_hooks.set) {
                m_hooks.set(channel(deck), QStringLiteral("pfl"), checked ? 1 : 0);
            }
        });
        if (m_hooks.value) {
            cue->setChecked(m_hooks.value(channel(deck), QStringLiteral("pfl")) > 0);
        }
        if (m_hooks.watch) {
            m_hooks.watch(channel(deck), QStringLiteral("pfl"), [cue](double v) { cue->setChecked(v > 0); });
        }
        m_cues[deck - 1] = cue;
        rows->addWidget(cue);
    }
    m_split = new SwitchRow(tr("Split cue"), content, QStringLiteral("decklab-head-split"));
    m_split->setHint(tr("Cue in your left ear, Main in your right"));
    m_split->setAccessibleName(tr("Split cue"));
    connect(m_split, &QPushButton::clicked, this, [this](bool checked) {
        if (m_hooks.set) {
            m_hooks.set(QStringLiteral("[Master]"), QStringLiteral("headSplit"), checked ? 1 : 0);
        }
        refreshValues();
    });
    if (m_hooks.value) {
        m_split->setChecked(m_hooks.value(QStringLiteral("[Master]"), QStringLiteral("headSplit")) > 0);
    }
    if (m_hooks.watch) {
        m_hooks.watch(QStringLiteral("[Master]"), QStringLiteral("headSplit"), [this](double v) {
            m_split->setChecked(v > 0);
        });
    }
    rows->addWidget(m_split);
    // headMix: -1 = cue only, +1 = main only. The readout says both.
    m_mix = new StepperRow(tr("Headphone mix"), content, QStringLiteral("decklab-head-mix"));
    m_mix->setRange(0, 100, 10);
    m_mix->setDefault(50);
    m_mix->setHint(tr("Double-tap the value for an even mix"));
    m_mix->setFormat([](int value) {
        return DeckLabSettingsPage::tr("CUE %1 · MAIN %2").arg(100 - value).arg(value);
    });
    if (m_hooks.value) {
        m_mix->setValue(static_cast<int>(std::lround((m_hooks.value(QStringLiteral("[Master]"), QStringLiteral("headMix")) + 1) * 50)));
    }
    connect(m_mix, &StepperRow::changed, this, [this](int value) {
        if (m_hooks.set) {
            m_hooks.set(QStringLiteral("[Master]"), QStringLiteral("headMix"), value / 50.0 - 1);
        }
    });
    if (m_hooks.watch) {
        m_hooks.watch(QStringLiteral("[Master]"), QStringLiteral("headMix"), [this](double v) {
            m_mix->setValue(static_cast<int>(std::lround((v + 1) * 50)));
        });
    }
    rows->addWidget(m_mix);
}

// ---------------------------------------------------------------- Display --

void DeckLabSettingsPage::buildDisplay(QWidget* content) {
    auto* rows = static_cast<QVBoxLayout*>(content->layout());
    m_keys = new ChoiceRow(tr("Key format"), content, QStringLiteral("decklab-settings-keys"),
            QStringLiteral("decklab-key-format"));
    m_keys->addOption(QStringLiteral("8A"), tr("Lancelot · 8A / 8B"), 0);
    m_keys->addOption(QStringLiteral("8m"), tr("Open Key · 8m / 8d"), 1);
    m_keys->addOption(QStringLiteral("Am"), tr("Musical · Am / C"), 2);
    const QString notation = m_config->getValueString(ConfigKey("[Key]", "KeyNotation"));
    m_keys->model()->setCurrentIndex(notation == "OpenKey" ? 1 : notation == "Traditional" ? 2 : 0);
    connect(m_keys->model(), qOverload<int>(&QComboBox::activated), this, [this](int index) {
        if (m_hooks.keyNotationChosen) {
            m_hooks.keyNotationChosen(index);
        }
        refreshValues();
    });
    rows->addWidget(m_keys);
    auto* waves = new ChoiceRow(tr("Waveforms"), content, QStringLiteral("decklab-settings-waveforms"),
            QStringLiteral("decklab-waveform-layout"));
    waves->addOption(tr("STACKED"), tr("Stacked"), 0);
    waves->addOption(tr("SIDE BY SIDE"), tr("Side by side"), 1);
    waves->model()->setCurrentIndex(m_hooks.sideBySide && m_hooks.sideBySide() ? 1 : 0);
    connect(waves->model(), qOverload<int>(&QComboBox::activated), this, [this, waves](int index) {
        if (m_hooks.waveformLayoutChosen) {
            m_hooks.waveformLayoutChosen(waves->model()->itemData(index).toInt() == 1);
        }
    });
    rows->addWidget(waves);
    auto* factory = WaveformWidgetFactory::instance();
    auto* motion = new ChoiceRow(tr("Waveform motion"), content, QStringLiteral("decklab-settings-motion"),
            QStringLiteral("decklab-waveform-fps"));
    motion->addOption(tr("SMOOTH 60"), tr("60 FPS · smoother motion"), 60);
    motion->addOption(tr("LIGHT 30"), tr("30 FPS · lower draw cost"), 30);
    motion->setHint(tr("Light saves processor time for the music"));
    motion->model()->setCurrentIndex(factory && factory->getFrameRate() > 30 ? 0 : 1);
    connect(motion->model(), qOverload<int>(&QComboBox::activated), this, [motion](int index) {
        if (auto* waveforms = WaveformWidgetFactory::instance()) {
            waveforms->setFrameRate(motion->model()->itemData(index).toInt());
        }
    });
    rows->addWidget(motion);
    auto* zoom = new StepperRow(tr("Zoom"), content, QStringLiteral("decklab-waveform-zoom"));
    zoom->setRange(1, 10, 1);
    zoom->setDefault(3);
    zoom->setFormat([](int value) { return QStringLiteral("×%1").arg(value); });
    if (m_hooks.value) {
        zoom->setValue(static_cast<int>(m_hooks.value(channel(1), QStringLiteral("waveform_zoom"))));
    }
    connect(zoom, &StepperRow::changed, this, [this](int value) {
        // Every deck, so linked and unlinked lanes agree (S-16).
        for (int deck = 1; deck <= 4 && m_hooks.set; ++deck) {
            m_hooks.set(channel(deck), QStringLiteral("waveform_zoom"), value);
        }
    });
    if (m_hooks.watch) {
        m_hooks.watch(channel(1), QStringLiteral("waveform_zoom"), [zoom](double v) {
            zoom->setValue(static_cast<int>(std::lround(v)));
        });
    }
    rows->addWidget(zoom);
    auto* grid = new StepperRow(tr("Beat grid"), content, QStringLiteral("decklab-beatgrid"));
    grid->setRange(0, 100, 10);
    grid->setDefault(60);
    grid->setFormat([](int value) { return QStringLiteral("%1 %").arg(value); });
    grid->setValue(factory ? factory->getBeatGridAlpha() : 60);
    connect(grid, &StepperRow::changed, this, [](int value) {
        if (auto* waveforms = WaveformWidgetFactory::instance()) {
            waveforms->setDisplayBeatGridAlpha(value);
        }
    });
    rows->addWidget(grid);
    auto* style = new ChoiceRow(tr("Waveform style"), content, QStringLiteral("decklab-settings-waveform-style"),
            QStringLiteral("decklab-waveform-style"));
    if (factory) {
        const auto& types = factory->getAvailableTypes();
        for (int index = 0; index < types.size(); ++index) {
            const auto type = types[index].getType();
            // Engine test renderers are not a look (S-18).
            if (type == WaveformWidgetType::Empty || type == WaveformWidgetType::VSyncTest) {
                continue;
            }
            const QString name = waveformName(type, types[index].getDisplayName());
            style->addOption(name, name, index);
            if (type == factory->getType()) {
                style->model()->setCurrentIndex(style->model()->count() - 1);
            }
        }
    }
    connect(style->model(), qOverload<int>(&QComboBox::activated), this, [style](int index) {
        if (auto* waveforms = WaveformWidgetFactory::instance()) {
            waveforms->setWidgetTypeFromHandle(style->model()->itemData(index).toInt());
        }
    });
    rows->addWidget(style);
}

// ---------------------------------------------------------------- Library --

void DeckLabSettingsPage::buildLibrary(QWidget* content) {
    auto* rows = static_cast<QVBoxLayout*>(content->layout());
    auto* played = new SwitchRow(tr("Dim played tracks"), content, QStringLiteral("decklab-dim-played"));
    played->setChecked(m_config->getValue(ConfigKey("[Library]", "ApplyPlayedTrackColor"), true));
    connect(played, &QPushButton::clicked, this, [this](bool checked) {
        if (m_hooks.dimPlayedChosen) {
            m_hooks.dimPlayedChosen(checked);
        }
    });
    rows->addWidget(played);
    rows->addWidget(new SectionRule(tr("Apps"), content));
    // Recording and USB drives have one home now (Unit › Storage); the
    // separate "Record mix to USB" and "Check USB drives" launchers are gone.
    const QList<std::tuple<QString, QString, int>> apps{
            {tr("Get music…"), QStringLiteral("decklab-tool-get-music"), 2},
            {tr("Sample Maker…"), QStringLiteral("decklab-tool-sample-maker"), 3},
            {tr("USB drives & recording"), QStringLiteral("decklab-tool-10"), 10},
    };
    for (const auto& [label, name, id] : apps) {
        auto* row = new ActionRow(label, content, name);
        row->setDisabledHint(tr("Needs the system service"));
        connect(row, &QPushButton::clicked, this, [this, id = id] {
            if (m_hooks.tool) {
                m_hooks.tool(id);
            }
        });
        row->setEnabled(false);
        m_tools.append(row);
        rows->addWidget(row);
    }
}

// ------------------------------------------------------------- navigation --

void DeckLabSettingsPage::showCategory(int category) {
    category = std::clamp(category, 0, static_cast<int>(m_links.size()) - 1);
    if (category != Unit) {
        m_lastCategory = category;
    }
    m_category = category;
    // Unit takes the full width with its own breadcrumb header (R-14).
    m_categories->setVisible(category != Unit);
    m_pages->setCurrentIndex(category);
    for (int index = 0; index < m_links.size(); ++index) {
        m_links.at(index)->setChecked(index == category);
    }
    if (category == Unit && m_system && m_system->currentSection() != DeckLabSystemPage::Section::Home &&
            m_level == 0) {
        m_system->showSection(DeckLabSystemPage::Section::Home);
    }
    refreshValues();
}

void DeckLabSettingsPage::showUnit(DeckLabSystemPage::Section section) {
    showCategory(Unit);
    if (m_system) {
        m_system->showSection(section);
    }
    setLevel(1);
}

void DeckLabSettingsPage::setLevel(int level) {
    m_level = level;
    if (!m_knob) {
        return;
    }
    m_knob->setScope(level == 0 ? m_categories : m_pages->currentWidget());
    if (m_knob->current() && m_knob->scope()->isAncestorOf(m_knob->current())) {
        return;
    }
    if (level == 0 && m_category < m_links.size()) {
        m_knob->focus(m_links.at(m_category == Unit ? m_lastCategory : m_category));
        m_knob->clear();
    }
}

bool DeckLabSettingsPage::knobBack() {
    if (m_level == 1) {
        if (m_category == Unit && m_system) {
            // A full-height panel (password keyboard, time zone picker) closes first.
            for (const char* name : {"decklab-wifi-cancel", "decklab-time-picker-done"}) {
                for (auto* button : m_system->findChildren<QPushButton*>(QString::fromLatin1(name))) {
                    if (button->isVisible()) {
                        button->click();
                        return true;
                    }
                }
            }
            if (m_system->currentSection() != DeckLabSystemPage::Section::Home && !m_system->isEmbedded()) {
                const auto section = m_system->currentSection();
                m_system->showSection(DeckLabSystemPage::Section::Home);
                m_knob->setScope(m_system);
                if (auto* tile = m_system->tile(section)) {
                    m_knob->focus(tile);
                }
                return true;
            }
            showCategory(m_lastCategory);
        }
        setLevel(0);
        m_knob->focus(m_links.at(m_category));
        return true;
    }
    // Back from the category list leaves Settings (to Decks).
    if (m_hooks.leave) {
        m_hooks.leave();
    }
    return true;
}

void DeckLabSettingsPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refreshLayoutRows();
    refreshDeckRows();
    refreshValues();
}

void DeckLabSettingsPage::setHealth(double fps, int dropped, int overloads) {
    if (!m_health) {
        return;
    }
    Q_UNUSED(dropped);
    // A DJ can't judge "7 dropped frames": say whether it is smooth (S-22).
    QString value;
    Lamp lamp = Lamp::Go;
    if (overloads > 0) {
        value = tr("%n dropout(s)", nullptr, overloads);
        lamp = Lamp::Caution;
    } else if (fps > 0 && fps < 24) {
        value = tr("Busy · %1 FPS").arg(fps, 0, 'f', 0);
        lamp = Lamp::Caution;
    } else {
        value = tr("Smooth · no dropouts");
    }
    if (m_health->value() != value || m_health->lamp() != lamp) {
        m_health->setValue(value);
        m_health->setLamp(lamp);
    }
}

void DeckLabSettingsPage::refreshValues() {
    if (m_links.size() < 6) {
        return;
    }
    if (m_systemUi) {
        QString value;
        Lamp lamp = Lamp::Off;
        m_systemUi->readiness(&value, &lamp);
        m_links.at(ControllerAudio)->setValue(value, lamp, true);
    }
    if (m_layout) {
        const int chosen = m_layout->model()->currentData().toInt();
        const int automatic = m_hooks.autoDecks ? m_hooks.autoDecks() : 2;
        m_links.at(Decks)->setValue(chosen == 0 ? tr("Auto · %1 decks").arg(automatic) : tr("%1 decks").arg(chosen));
    }
    m_links.at(Headphones)->setValue(m_split && m_split->isChecked() ? tr("Split cue") : QString());
    if (m_keys) {
        m_links.at(Display)->setValue(tr("Key %1").arg(m_keys->segmentText(m_keys->currentIndex())));
    }
    const bool online = m_systemUi && m_systemUi->toolsConnected();
    m_links.at(Unit)->setValue(online ? QTime::currentTime().toString(QStringLiteral("HH:mm")) : tr("Offline"),
            online ? Lamp::Go : Lamp::Caution, m_systemUi != nullptr);
}
