// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/decklabsmoke.h"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QHelpEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTimer>
#include <QToolTip>
#include <memory>
#include "control/controlobject.h"
#include "util/cmdlineargs.h"
#include "widget/wdecklab.h"
#include "widget/woverview.h"

void startDeckLabSmoke(WDeckLab* surface, UserSettingsPointer config) {
    const QString output = qEnvironmentVariable("DECKLAB_SMOKE_DIR");
    if (output.isEmpty() || !surface || !CmdlineArgs::Instance().getDeveloper()) return;
    // Do not turn a forgotten test environment variable into automation on a
    // personal profile. The build recipe creates this separate scratch profile.
    if (!QDir(config->getSettingsPath()).dirName().startsWith("decklab-native-smoke")) {
        qCritical("Refusing smoke check outside its dedicated profile"); qApp->exit(70); return;
    }
    auto* timer = new QTimer(surface);
    auto step = std::make_shared<int>(0);
    auto failures = std::make_shared<QJsonArray>();
    const auto check = [surface, failures] {
        if (surface->size() != QSize(800, 480)) failures->append("Surface is not 800x480");
        for (int deck = 1; deck <= 4; ++deck) {
            auto* overview = surface->findChild<WOverview*>(QStringLiteral("decklab-overview-%1").arg(deck));
            bool positionBound = false;
            // WBaseWidget owns these via unique_ptr, not the QObject child tree.
            // Its developer tooltip reports the actual display connection.
            if (overview) {
                QHelpEvent help(QEvent::ToolTip, QPoint(), overview->mapToGlobal(QPoint()));
                QCoreApplication::sendEvent(overview, &help); QToolTip::hideText();
                const QString prefix = QStringLiteral("DisplayConnection: [Channel%1],playposition ").arg(deck);
                for (const QString& line : overview->toolTip().split(QChar('\n')))
                    if (line.startsWith(prefix) && line.contains("Direction: TO_WIDGET")) positionBound = true;
            }
            if (!positionBound) failures->append(QStringLiteral("Deck %1 overview missing read-only playposition binding").arg(deck));
        }
        for (auto* b : surface->findChildren<QPushButton*>()) {
            if (!b->isVisible()) continue;
            QScrollArea* scroll = nullptr;
            for (auto* parent = b->parentWidget(); parent; parent = parent->parentWidget())
                if ((scroll = qobject_cast<QScrollArea*>(parent))) break;
            if (scroll) {
                scroll->ensureWidgetVisible(b, 0, 0); QCoreApplication::processEvents();
                if (!scroll->viewport()->rect().contains(QRect(b->mapTo(scroll->viewport(), QPoint()), b->size())))
                    failures->append(QString(QStringLiteral("Unreachable scroll control: ") + b->text()));
            }
            const QRect rect(b->mapTo(surface, QPoint()), b->size());
            if (!surface->rect().contains(rect)) failures->append(QString(QStringLiteral("Clipped button: ") + b->text()));
        }
        for (auto* scroll : surface->findChildren<QScrollArea*>()) {
            scroll->verticalScrollBar()->setValue(0); scroll->horizontalScrollBar()->setValue(0);
        }
        QCoreApplication::processEvents();
        // The smoke check may navigate/configure views, never start audio.
        for (int deck = 1; deck <= 4; ++deck) if (ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), "play"))) failures->append("Unexpected playing deck");
        for (int slot = 1; slot <= 16; ++slot) if (ControlObject::get(ConfigKey(QStringLiteral("[Sampler%1]").arg(slot), "play"))) failures->append("Unexpected playing sampler");
        for (int unit = 1; unit <= 2; ++unit)
            if (ControlObject::get(ConfigKey(QStringLiteral("[DeckLabEffectUnit%1_Effect4]").arg(unit), "loaded")) != 1)
                failures->append("Bundled native RMX processor is unavailable");
    };
    QObject::connect(timer, &QTimer::timeout, surface, [=] {
        // Capture the real parser/Library/PlayerManager surface, including the
        // empty native waveform viewers. These are not moving-audio evidence.
        if (*step < 5) surface->findChild<QPushButton*>(QStringLiteral("decklab-tab-%1").arg(*step))->click();
        else if (*step < 10) { surface->selectPage(3); surface->findChild<QPushButton*>(QStringLiteral("decklab-fx-view-%1").arg(*step - 5))->click(); }
        else if (*step < 15) { surface->selectPage(4); surface->findChild<QPushButton*>(QStringLiteral("decklab-settings-category-%1").arg(*step - 10))->click(); }
        else if (*step == 15) surface->openCollection();
        else if (*step == 16) surface->findChild<QPushButton*>("decklab-search")->click();
        else if (*step == 17 || *step == 18) {
            auto* layout=surface->findChild<QComboBox*>("decklab-deck-layout");
            QMetaObject::invokeMethod(layout,"activated",Q_ARG(int,*step==17?2:0));
            surface->selectPage(0);
        }
        else if (*step == 19 || *step == 20) {
            auto* waves=surface->findChild<QComboBox*>("decklab-waveform-layout");
            waves->setCurrentIndex(1);
            QMetaObject::invokeMethod(waves,"activated",Q_ARG(int,1));
            auto* layout=surface->findChild<QComboBox*>("decklab-deck-layout");
            QMetaObject::invokeMethod(layout,"activated",Q_ARG(int,*step==19?1:2));
            surface->selectPage(0);
        }
        else {
            timer->stop();
            QFile file(QDir(output).filePath("native-smoke.json"));
            const bool opened = file.open(QIODevice::WriteOnly);
            int processors = 0;
            for (int unit = 1; unit <= 2; ++unit)
                processors += ControlObject::get(ConfigKey(QStringLiteral("[DeckLabEffectUnit%1_Effect4]").arg(unit), "loaded")) == 1;
            if (opened) file.write(QJsonDocument(QJsonObject{{"screens", *step}, {"failures", *failures},
                    {"audio_started", false}, {"native_rmx_units_loaded", processors}}).toJson());
            qApp->exit(opened && failures->isEmpty() ? 0 : 71); return;
        }
        surface->setFixedSize(800, 480);
        QTimer::singleShot(100, surface, [=] {
            check();
            if (*step == 15 || *step == 16) {
                for (int deck = 1; deck <= 2; ++deck)
                    if (surface->findChild<QPushButton*>(QStringLiteral("decklab-load-%1").arg(deck))->isEnabled())
                        failures->append("Empty collection offers Load without a track selection");
            }
            if (!surface->grab().save(QDir(output).filePath(QStringLiteral("app-screen-%1.png").arg(*step, 2, 10, QLatin1Char('0'))))) failures->append("Screenshot failed");
            if (*step == 18) {
                // Wider HDMI touch panels must scale cleanly, not break.
                surface->setFixedSize(1024, 600);
                for (int pass = 0; pass < 3; ++pass) QCoreApplication::processEvents();
                if (!surface->grab().save(QDir(output).filePath(QStringLiteral("app-decks-1024x600.png")))) failures->append("Screenshot failed");
                surface->setFixedSize(800, 480);
                for (int pass = 0; pass < 3; ++pass) QCoreApplication::processEvents();
            }
            ++*step;
        });
    });
    timer->start(350);
}
