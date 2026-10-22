// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QStyledItemDelegate>

// The native Qt menu delegate can ignore the skin's item min-height. Keep the
// open selector as usable by touch as the closed selector itself.
class DeckLabComboDelegate final : public QStyledItemDelegate {
  public:
    explicit DeckLabComboDelegate(QObject* parent) : QStyledItemDelegate(parent) {}
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(qMax(44, size.height()));
        return size;
    }
};
