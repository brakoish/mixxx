// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/decklabsmoke.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <memory>
#include "control/controlobject.h"
#include "util/cmdlineargs.h"
#include "widget/wdecklab.h"

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
        for (auto* b : surface->findChildren<QPushButton*>()) {
            if (!b->isVisible() || b->visibleRegion().isEmpty()) continue;
            const QRect rect(b->mapTo(surface, QPoint()), b->size());
            if (!surface->rect().contains(rect)) failures->append("Clipped button: " + b->text());
        }
        // The smoke check may navigate/configure views, never start audio.
        for (int deck = 1; deck <= 4; ++deck) if (ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), "play"))) failures->append("Unexpected playing deck");
        for (int slot = 1; slot <= 16; ++slot) if (ControlObject::get(ConfigKey(QStringLiteral("[Sampler%1]").arg(slot), "play"))) failures->append("Unexpected playing sampler");
    };
    QObject::connect(timer, &QTimer::timeout, surface, [=] {
        // Capture the real parser/Library/PlayerManager surface, including the
        // empty native waveform viewers. These are not moving-audio evidence.
        if (*step < 5) surface->selectPage(*step);
        else if (*step < 9) { surface->selectPage(3); surface->findChild<QStackedWidget*>("decklab-fx-pages")->setCurrentIndex(*step - 5); }
        else if (*step < 14) { surface->selectPage(4); surface->findChild<QStackedWidget*>("decklab-settings-pages")->setCurrentIndex(*step - 9); }
        else {
            timer->stop();
            QFile file(QDir(output).filePath("native-smoke.json"));
            const bool opened = file.open(QIODevice::WriteOnly);
            if (opened) file.write(QJsonDocument(QJsonObject{{"screens", *step}, {"failures", *failures}, {"audio_started", false}}).toJson());
            qApp->exit(opened && failures->isEmpty() ? 0 : 71); return;
        }
        surface->setFixedSize(800, 480);
        QTimer::singleShot(100, surface, [=] {
            check();
            if (!surface->grab().save(QDir(output).filePath(QStringLiteral("app-screen-%1.png").arg(*step, 2, 10, QLatin1Char('0'))))) failures->append("Screenshot failed");
            ++*step;
        });
    });
    timer->start(350);
}
