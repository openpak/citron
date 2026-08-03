// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "citron/legacy_friend_delegate.h"

#include <algorithm>

#include <QApplication>
#include <QCursor>
#include <QFont>
#include <QFontMetrics>
#include <QListView>
#include <QModelIndex>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QStyle>
#include <QStyleOptionViewItem>

#include "citron/legacy_account_page_p.h"
#include "citron/legacy_avatar_cache.h"
#include "citron/uisettings.h"

namespace {
QRect CardRect(const QRect& cell_rect) {
    return cell_rect.adjusted(4, OpenPakFriendDelegate::kCardMarginV, -4,
                              -OpenPakFriendDelegate::kCardMarginV);
}
} // Anonymous namespace

OpenPakFriendDelegate::OpenPakFriendDelegate(QListView* view, QObject* parent)
    : QStyledItemDelegate(parent), list_view(view), anim_timer(new QTimer(this)) {
    anim_timer->setInterval(40);
    connect(anim_timer, &QTimer::timeout, this, &OpenPakFriendDelegate::AdvanceAnimations);
    anim_timer->start();
}

OpenPakFriendDelegate::~OpenPakFriendDelegate() = default;

void OpenPakFriendDelegate::initStyleOption(QStyleOptionViewItem* option,
                                             const QModelIndex& index) const {
    QStyledItemDelegate::initStyleOption(option, index);
    option->state &= ~(QStyle::State_Selected | QStyle::State_HasFocus | QStyle::State_Active |
                       QStyle::State_Sunken);
    option->showDecorationSelected = false;
}

QSize OpenPakFriendDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const {
    return QSize(0, kCardHeight);
}

void OpenPakFriendDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                   const QModelIndex& index) const {
    painter->save();
    painter->setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
                            QPainter::SmoothPixmapTransform);

    PaintBackground(painter, option.rect, index);

    const QRect card = CardRect(option.rect);
    const int avatar_left = card.left() + 8;
    const QRect avatar_rect(avatar_left, card.top() + (card.height() - kAvatarSize) / 2,
                            kAvatarSize, kAvatarSize);
    PaintAvatar(painter, avatar_rect, index);

    const bool is_request = index.data(OpenPakFriendItem::IsRequestRole).toBool();
    const QRect actions = ActionsRect(card);
    const QRect text_rect(avatar_rect.right() + 10, card.top(), actions.left() - 6 - avatar_rect.right() - 10,
                          card.height());
    PaintNameAndStatus(painter, text_rect, option, index);
    PaintActions(painter, card, is_request);

    painter->restore();
}

QRect OpenPakFriendDelegate::ActionsRect(const QRect& card_rect) const {
    constexpr int width = 132;
    constexpr int right_margin = 14;
    return QRect(card_rect.right() - right_margin - width, card_rect.top(), width,
                card_rect.height());
}

OpenPakFriendDelegate::ActionHit OpenPakFriendDelegate::HitTestActions(const QRect& cell_rect,
                                                                         const QPoint& pos,
                                                                         bool is_request) const {
    const QRect card = CardRect(cell_rect);
    const QRect actions = ActionsRect(card);
    if (!actions.contains(pos)) {
        return ActionHit::None;
    }

    if (!is_request) {
        // Single centered "Remove" pill occupying the right half of the actions rect.
        const QRect pill(actions.right() - 64, actions.top() + (actions.height() - 24) / 2, 64, 24);
        return pill.contains(pos) ? ActionHit::Secondary : ActionHit::None;
    }

    const int pill_w = 60;
    const QRect primary(actions.left(), actions.top() + (actions.height() - 24) / 2, pill_w, 24);
    const QRect secondary(actions.left() + pill_w + 4, actions.top() + (actions.height() - 24) / 2,
                          pill_w, 24);
    if (primary.contains(pos)) {
        return ActionHit::Primary;
    }
    if (secondary.contains(pos)) {
        return ActionHit::Secondary;
    }
    return ActionHit::None;
}

void OpenPakFriendDelegate::AdvanceAnimations() {
    if (!list_view) {
        return;
    }
    bool dirty = false;

    const QPoint mp = list_view->viewport()->mapFromGlobal(QCursor::pos());
    const QModelIndex hov_raw = list_view->indexAt(mp);
    const QPersistentModelIndex hov_key =
        hov_raw.isValid() ? QPersistentModelIndex(hov_raw) : QPersistentModelIndex{};

    if (hov_key.isValid() && !hover_prog.contains(hov_key)) {
        hover_prog[hov_key] = 0.0;
    }

    for (auto it = hover_prog.begin(); it != hover_prog.end();) {
        const bool hov = it.key() == hov_key;
        qreal& p = it.value();
        p = hov ? std::min(p + 0.12, 1.0) : std::max(p - 0.12, 0.0);
        dirty = true;
        it = (p <= 0.0 && !hov) ? hover_prog.erase(it) : std::next(it);
    }

    if (dirty) {
        list_view->viewport()->update();
    }
}

void OpenPakFriendDelegate::PaintBackground(QPainter* painter, const QRect& cell_rect,
                                             const QModelIndex& index) const {
    const QRect card = CardRect(cell_rect);
    const qreal hov = hover_prog.value(QPersistentModelIndex(index), 0.0);

    QColor bg = CardBg();
    if (hov > 0.0) {
        bg = bg.lighter(100 + static_cast<int>(hov * 14));
    }

    QPainterPath path;
    path.addRoundedRect(card, kCardRadius, kCardRadius);
    painter->fillPath(path, bg);
}

void OpenPakFriendDelegate::PaintAvatar(QPainter* painter, const QRect& r,
                                         const QModelIndex& index) const {
    const u64 pid = index.data(OpenPakFriendItem::PidRole).toULongLong();
    const std::string avatar_b64 = index.data(OpenPakFriendItem::AvatarB64Role).toString().toStdString();
    QPixmap pixmap = OpenPak::AvatarCache::Get(std::to_string(pid), avatar_b64, kAvatarSize);

    painter->save();
    QPainterPath clip;
    clip.addEllipse(r);
    painter->setClipPath(clip);

    if (!pixmap.isNull()) {
        painter->drawPixmap(r, pixmap);
    } else {
        const QString name = index.data(OpenPakFriendItem::NameRole).toString();
        const QChar initial = name.isEmpty() ? QChar(QLatin1Char('?')) : name.at(0).toUpper();
        painter->fillRect(r, AccentColor().darker(120));
        QFont f = QApplication::font();
        f.setBold(true);
        f.setPointSize(std::max(f.pointSize() + 2, 10));
        painter->setFont(f);
        painter->setPen(Qt::white);
        painter->drawText(r, Qt::AlignCenter, initial);
    }
    painter->restore();

    // Presence dot, bottom-right of the avatar.
    const int status = index.data(OpenPakFriendItem::PresenceRole).toInt();
    const int dot = 12;
    const QRect dot_rect(r.right() - dot + 2, r.bottom() - dot + 2, dot, dot);
    painter->save();
    painter->setPen(QPen(CardBg(), 2));
    painter->setBrush(PresenceColor(status));
    painter->drawEllipse(dot_rect);
    painter->restore();
}

void OpenPakFriendDelegate::PaintNameAndStatus(QPainter* painter, const QRect& r,
                                                const QStyleOptionViewItem& option,
                                                const QModelIndex& index) const {
    if (r.width() <= 0) {
        return;
    }

    const QString name = index.data(OpenPakFriendItem::NameRole).toString();
    const int status = index.data(OpenPakFriendItem::PresenceRole).toInt();
    const QString game = index.data(OpenPakFriendItem::GamePresenceRole).toString();

    QString status_text;
    switch (status) {
    case 1:
        status_text = tr("Online");
        break;
    case 2:
        status_text = game.isEmpty() ? tr("In a game") : tr("Playing %1").arg(game);
        break;
    default:
        status_text = tr("Offline");
        break;
    }

    const int half_h = r.height() / 2;
    QFont name_font = option.font;
    name_font.setBold(true);
    painter->setFont(name_font);
    painter->setPen(FgColor());
    painter->drawText(QRect(r.left(), r.top(), r.width(), half_h),
                      Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine,
                      QFontMetrics(name_font).elidedText(name, Qt::ElideRight, r.width()));

    QFont sub_font = option.font;
    sub_font.setPointSize(std::max(sub_font.pointSize() - 1, 7));
    painter->setFont(sub_font);
    painter->setPen(PresenceColor(status));
    painter->drawText(QRect(r.left(), r.top() + half_h, r.width(), r.height() - half_h),
                      Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine,
                      QFontMetrics(sub_font).elidedText(status_text, Qt::ElideRight, r.width()));
}

void OpenPakFriendDelegate::PaintActions(QPainter* painter, const QRect& card_rect,
                                          bool is_request) const {
    const QRect actions = ActionsRect(card_rect);

    painter->save();
    QFont f = QApplication::font();
    f.setPointSize(std::max(f.pointSize() - 1, 7));
    f.setBold(true);
    painter->setFont(f);

    auto draw_pill = [&](const QRect& pill, const QString& text, const QColor& color) {
        QPainterPath path;
        path.addRoundedRect(pill, pill.height() / 2.0, pill.height() / 2.0);
        QColor fill = color;
        fill.setAlpha(38);
        painter->fillPath(path, fill);
        painter->setPen(QPen(color, 1.2));
        painter->drawPath(path);
        painter->setPen(color);
        painter->drawText(pill, Qt::AlignCenter, text);
    };

    if (is_request) {
        const int pill_w = 60;
        const QRect primary(actions.left(), actions.top() + (actions.height() - 24) / 2, pill_w, 24);
        const QRect secondary(actions.left() + pill_w + 4, actions.top() + (actions.height() - 24) / 2,
                              pill_w, 24);
        draw_pill(primary, tr("Accept"), QColor(50, 195, 85));
        draw_pill(secondary, tr("Decline"), QColor(220, 80, 70));
    } else {
        const QRect pill(actions.right() - 64, actions.top() + (actions.height() - 24) / 2, 64, 24);
        draw_pill(pill, tr("Remove"), QColor(220, 80, 70));
    }

    painter->restore();
}

QColor OpenPakFriendDelegate::CardBg() const {
    return UISettings::IsDarkTheme() ? QColor(36, 36, 40) : QColor(240, 240, 244);
}

QColor OpenPakFriendDelegate::FgColor() const {
    return UISettings::IsDarkTheme() ? QColor(230, 230, 234) : QColor(22, 22, 28);
}

QColor OpenPakFriendDelegate::DimColor() const {
    return UISettings::IsDarkTheme() ? QColor(145, 145, 155) : QColor(100, 100, 112);
}

QColor OpenPakFriendDelegate::AccentColor() const {
    const QString hex = QString::fromStdString(UISettings::values.accent_color.GetValue());
    if (QColor(hex).isValid()) {
        return QColor(hex);
    }
    const QColor pa = QApplication::palette().color(QPalette::Highlight);
    return (pa.isValid() && pa != Qt::black) ? pa : QColor(100, 149, 237);
}

QColor OpenPakFriendDelegate::PresenceColor(int status) const {
    switch (status) {
    case 1:
        return QColor(100, 149, 237); // online, no game
    case 2:
        return QColor(50, 195, 85); // playing a game
    default:
        return QColor(120, 120, 120); // offline
    }
}
