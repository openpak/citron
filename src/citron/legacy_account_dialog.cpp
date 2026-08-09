// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <thread>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>

#include <QApplication>
#include <QBuffer>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCursor>
#include <QEvent>
#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QListView>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "common/legacy_account.h"
#include "common/legacy_outgoing_requests.h"
#include "citron/legacy_account_dialog.h"
#include "citron/legacy_account_page_p.h"
#include "citron/legacy_avatar_cache.h"
#include "citron/legacy_compatible_titles.h"
#include "citron/legacy_controller.h"
#include "citron/legacy_friend_delegate.h"
#include "citron/legacy_history_delegate.h"
#include "citron/legacy_network_probe.h"
#include "citron/uisettings.h"

#ifdef ENABLE_WEB_SERVICE
#include "web_service/legacy_api.h"
#endif

namespace {

constexpr int kHeaderAvatarSize = 72;

QColor CardBg() {
    return UISettings::IsDarkTheme() ? QColor(30, 30, 34) : QColor(244, 244, 248);
}

QColor DimColor() {
    return UISettings::IsDarkTheme() ? QColor(150, 150, 158) : QColor(110, 110, 120);
}

QColor AccentColor() {
    const QString hex = QString::fromStdString(UISettings::values.accent_color.GetValue());
    if (QColor(hex).isValid()) {
        return QColor(hex);
    }
    const QColor pa = QApplication::palette().color(QPalette::Highlight);
    return (pa.isValid() && pa != Qt::black) ? pa : QColor(100, 149, 237);
}

QPixmap RoundedPixmap(const QPixmap& source, int size) {
    const qreal dpr = qApp->devicePixelRatio();
    QPixmap out(qMax(1, static_cast<int>(size * dpr)), qMax(1, static_cast<int>(size * dpr)));
    out.setDevicePixelRatio(dpr);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    clip.addEllipse(0, 0, size, size);
    painter.setClipPath(clip);
    if (source.isNull()) {
        painter.fillRect(0, 0, size, size, QColor(100, 149, 237));
    } else {
        painter.drawPixmap(0, 0, size, size, source);
    }
    return out;
}

QPixmap RoundedRectPixmap(const QPixmap& source, int size, int radius) {
    if (source.isNull()) {
        return {};
    }
    const qreal dpr = qApp->devicePixelRatio();
    QPixmap out(qMax(1, static_cast<int>(size * dpr)), qMax(1, static_cast<int>(size * dpr)));
    out.setDevicePixelRatio(dpr);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    clip.addRoundedRect(0, 0, size, size, radius, radius);
    painter.setClipPath(clip);
    painter.drawPixmap(0, 0, size, size, source);
    return out;
}

class HeaderCard : public QWidget {
public:
    explicit HeaderCard(QWidget* parent) : QWidget(parent) {
        auto* timer = new QTimer(this);
        timer->setInterval(33);
        connect(timer, &QTimer::timeout, this, [this] {
            phase += 0.035;
            update();
        });
        timer->start();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QRect card = rect().adjusted(1, 1, -1, -1);
        QPainterPath clip_path;
        clip_path.addRoundedRect(card, 12, 12);
        painter.setClipPath(clip_path);
        painter.fillPath(clip_path, CardBg());

        const QColor accent = AccentColor();
        const qreal breathe = 0.5 + 0.5 * std::sin(phase);

        const int glow_w = std::min(100, card.width() / 3);
        QColor edge = accent;
        edge.setAlphaF(0.22 * breathe);
        QColor edge_fade = accent;
        edge_fade.setAlphaF(0.0);

        QLinearGradient left_glow(card.left(), 0, card.left() + glow_w, 0);
        left_glow.setColorAt(0.0, edge);
        left_glow.setColorAt(1.0, edge_fade);
        painter.fillRect(QRect(card.left(), card.top(), glow_w, card.height()), left_glow);

        QLinearGradient right_glow(card.right() - glow_w, 0, card.right(), 0);
        right_glow.setColorAt(0.0, edge_fade);
        right_glow.setColorAt(1.0, edge);
        painter.fillRect(QRect(card.right() - glow_w, card.top(), glow_w, card.height()), right_glow);

        // Filled, layered wave bands -- same visual language as nexium-live's carousel backdrop:
        // each band is a solid fill down to the card's bottom edge, stacked with alternating
        // tint/shade and phase so they read as layered waves rather than decorative scribbles.
        struct WaveBand {
            qreal base_frac, amp_frac, phase_off, speed, shade;
            int alpha;
        };
        static constexpr WaveBand kBands[] = {
            {0.55, 0.05, 0.0, 1.5, 0.28, 70},
            {0.42, 0.07, 1.1, 1.2, -0.30, 60},
            {0.70, 0.035, 2.3, 0.9, 0.50, 50},
            {0.30, 0.09, 0.6, 1.7, -0.42, 42},
            {0.85, 0.03, 3.5, 0.7, 0.66, 34},
        };
        const auto shade = [](QColor c, qreal f) {
            const auto adj = [f](int v) {
                return f >= 0.0 ? static_cast<int>(v + (255 - v) * f) : static_cast<int>(v * (1.0 + f));
            };
            return QColor(adj(c.red()), adj(c.green()), adj(c.blue()));
        };
        constexpr int kSteps = 48;
        for (const auto& band : kBands) {
            const qreal base_y = card.top() + card.height() * band.base_frac;
            const qreal amp = card.height() * band.amp_frac;
            const qreal ph = phase * band.speed + band.phase_off;

            QPainterPath fill;
            fill.moveTo(card.left(), card.bottom());
            for (int s = 0; s <= kSteps; ++s) {
                const qreal nx = static_cast<qreal>(s) / kSteps;
                const qreal x = card.left() + nx * card.width();
                const qreal y = base_y + std::sin(nx * 2 * M_PI + ph) * amp +
                               std::cos(nx * 2 * M_PI * 1.7 + ph * 0.8) * amp * 0.4;
                fill.lineTo(x, y);
            }
            fill.lineTo(card.right(), card.bottom());
            fill.closeSubpath();

            QColor band_color = shade(accent, band.shade);
            band_color.setAlpha(band.alpha);
            painter.fillPath(fill, band_color);
        }

        painter.setClipping(false);
        painter.setPen(QPen(QColor(255, 255, 255, 20), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(clip_path);
    }

private:
    qreal phase = 0.0;
};

QListView* MakeCardList(QWidget* parent) {
    auto* view = new QListView(parent);
    view->setSelectionMode(QAbstractItemView::NoSelection);
    view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setFrameShape(QFrame::NoFrame);
    view->setMouseTracking(true);
    view->viewport()->setAttribute(Qt::WA_Hover);
    return view;
}

QLabel* MakeEmptyLabel(const QString& text) {
    auto* label = new QLabel(text);
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    QPalette pal = label->palette();
    pal.setColor(QPalette::WindowText, DimColor());
    label->setPalette(pal);
    return label;
}

QString TabWidgetStyle() {
    const QColor bg = CardBg();
    const QString bg_hex = bg.name();
    return QStringLiteral("QTabWidget::pane { border: none; background: %1; border-radius: 8px; }"
                          "QTabBar::tab { padding: 6px 14px; margin-right: 2px; "
                          "border-top-left-radius: 8px; border-top-right-radius: 8px; }"
                          "QTabBar::tab:selected { background: %1; }")
        .arg(bg_hex);
}

} // Anonymous namespace

OpenPakAccountDialog::OpenPakAccountDialog(OpenPakController* controller_, QWidget* parent)
    : QDialog(parent), controller(controller_) {
    setWindowTitle(tr("OpenPak Account"));
    resize(560, 660);

    auto* header_card = new HeaderCard(this);
    header_avatar = new QLabel;
    header_avatar->setFixedSize(kHeaderAvatarSize, kHeaderAvatarSize);
    header_avatar->setPixmap(RoundedPixmap({}, kHeaderAvatarSize));
    header_avatar->setCursor(Qt::PointingHandCursor);
    header_avatar->setToolTip(tr("Click to change your profile picture"));
    header_avatar->installEventFilter(this);

    header_name = new QLabel;
    QFont name_font = header_name->font();
    name_font.setPointSize(name_font.pointSize() + 4);
    name_font.setBold(true);
    header_name->setFont(name_font);

    edit_name_button = new QToolButton;
    edit_name_button->setText(QStringLiteral("✎")); // pencil
    edit_name_button->setCursor(Qt::PointingHandCursor);
    edit_name_button->setToolTip(tr("Change your username"));
    edit_name_button->setAutoRaise(true);
    connect(edit_name_button, &QToolButton::clicked, this,
           &OpenPakAccountDialog::OnEditUsername);

    auto* header_name_row = new QHBoxLayout;
    header_name_row->setSpacing(4);
    header_name_row->addWidget(header_name);
    header_name_row->addWidget(edit_name_button);
    header_name_row->addStretch();

    header_code = new QLabel;
    header_code->setCursor(Qt::PointingHandCursor);
    header_code->setToolTip(tr("Click to copy"));
    header_code->installEventFilter(this);
    QFont code_font(QStringLiteral("monospace"));
    header_code->setFont(code_font);
    header_code->setStyleSheet(QStringLiteral("QLabel { padding: 2px 8px; border-radius: 8px; "
                                              "background: rgba(255,255,255,20); }"));

    auto* header_text = new QVBoxLayout;
    header_text->setSpacing(6);
    header_text->addStretch();
    header_text->addLayout(header_name_row);
    header_text->addWidget(header_code, 0, Qt::AlignLeft);
    header_text->addStretch();

    auto* header_content = new QHBoxLayout;
    header_content->setSpacing(16);
    header_content->addWidget(header_avatar);
    header_content->addLayout(header_text);

    auto* header_row = new QHBoxLayout(header_card);
    header_row->setContentsMargins(16, 14, 16, 14);
    header_row->addStretch();
    header_row->addLayout(header_content);
    header_row->addStretch();

    friend_code_input = new QLineEdit;
    friend_code_input->setPlaceholderText(tr("SW-0000-0000-0000"));
    add_button = new QPushButton(tr("Add Friend"));
    auto* add_row = new QHBoxLayout;
    add_row->addWidget(friend_code_input);
    add_row->addWidget(add_button);

    friend_search = new QLineEdit;
    friend_search->setPlaceholderText(tr("Search friends..."));
    friend_search->setClearButtonEnabled(true);

    friends_view = MakeCardList(this);
    friends_model = new QStandardItemModel(this);
    friends_view->setModel(friends_model);
    friend_delegate = new OpenPakFriendDelegate(friends_view, this);
    friends_view->setItemDelegate(friend_delegate);
    connect(friends_view, &QListView::clicked, this, &OpenPakAccountDialog::OnFriendsViewClicked);
    connect(friend_search, &QLineEdit::textChanged, this, &OpenPakAccountDialog::ApplyFriendFilter);

    auto* friends_page = new QWidget;
    auto* friends_page_layout = new QVBoxLayout(friends_page);
    friends_page_layout->setContentsMargins(0, 0, 0, 0);
    friends_page_layout->setSpacing(8);
    friends_page_layout->addWidget(friend_search);
    friends_page_layout->addWidget(friends_view, 1);

    friends_stack = new QStackedWidget;
    friends_stack->addWidget(friends_page);
    friends_stack->addWidget(MakeEmptyLabel(tr("No friends yet — add one by friend code above.")));

    requests_view = MakeCardList(this);
    requests_model = new QStandardItemModel(this);
    requests_view->setModel(requests_model);
    request_delegate = new OpenPakFriendDelegate(requests_view, this);
    requests_view->setItemDelegate(request_delegate);
    connect(requests_view, &QListView::clicked, this, &OpenPakAccountDialog::OnFriendsViewClicked);
    requests_stack = new QStackedWidget;
    requests_stack->addWidget(requests_view);
    requests_stack->addWidget(MakeEmptyLabel(tr("No incoming friend requests.")));

    outgoing_requests_label = new QLabel(tr("Outgoing Friend Requests"));
    QFont outgoing_label_font = outgoing_requests_label->font();
    outgoing_label_font.setBold(true);
    outgoing_requests_label->setFont(outgoing_label_font);

    outgoing_requests_view = MakeCardList(this);
    outgoing_requests_model = new QStandardItemModel(this);
    outgoing_requests_view->setModel(outgoing_requests_model);
    outgoing_request_delegate = new OpenPakFriendDelegate(outgoing_requests_view, this);
    outgoing_requests_view->setItemDelegate(outgoing_request_delegate);
    connect(outgoing_requests_view, &QListView::clicked, this,
            &OpenPakAccountDialog::OnFriendsViewClicked);

    outgoing_requests_section = new QWidget;
    auto* outgoing_section_layout = new QVBoxLayout(outgoing_requests_section);
    outgoing_section_layout->setContentsMargins(0, 8, 0, 0);
    outgoing_section_layout->setSpacing(6);
    outgoing_section_layout->addWidget(outgoing_requests_label);
    outgoing_section_layout->addWidget(outgoing_requests_view);
    outgoing_requests_section->setVisible(false);

    history_view = MakeCardList(this);
    history_model = new QStandardItemModel(this);
    history_view->setModel(history_model);
    history_view->setItemDelegate(new OpenPakHistoryDelegate(this));
    history_stack = new QStackedWidget;
    history_stack->addWidget(history_view);
    history_stack->addWidget(MakeEmptyLabel(tr("No games played yet.")));

    cloud_save_icon = new QLabel;
    cloud_save_icon->setFixedSize(64, 64);
    cloud_save_icon->setAlignment(Qt::AlignCenter);

    cloud_save_title = new QLabel;
    QFont cloud_save_title_font = cloud_save_title->font();
    cloud_save_title_font.setBold(true);
    cloud_save_title_font.setPointSize(cloud_save_title_font.pointSize() + 1);
    cloud_save_title->setFont(cloud_save_title_font);
    cloud_save_title->setAlignment(Qt::AlignCenter);

    cloud_save_picker_group = new QButtonGroup(this);
    cloud_save_picker_row = new QHBoxLayout;
    cloud_save_picker_row->setSpacing(10);
    cloud_save_picker_container = new QWidget;
    cloud_save_picker_container->setLayout(cloud_save_picker_row);

    cloud_save_status = new QLabel;
    cloud_save_status->setWordWrap(true);
    cloud_save_status->setAlignment(Qt::AlignCenter);
    QPalette cloud_save_status_pal = cloud_save_status->palette();
    cloud_save_status_pal.setColor(QPalette::WindowText, DimColor());
    cloud_save_status->setPalette(cloud_save_status_pal);

    cloud_save_download_button = new QPushButton(tr("\xE2\x86\x93 Download Save"));
    cloud_save_download_button->setCursor(Qt::PointingHandCursor);
    cloud_save_download_button->setMinimumHeight(34);
    cloud_save_download_button->setMinimumWidth(180);
    cloud_save_download_button->setStyleSheet(
        QStringLiteral("QPushButton { background: %1; color: white; border: none; "
                       "border-radius: 8px; padding: 6px 18px; font-weight: 600; }"
                       "QPushButton:disabled { background: rgba(128,128,128,60); "
                       "color: rgba(255,255,255,110); }")
            .arg(AccentColor().name()));
    connect(cloud_save_download_button, &QPushButton::clicked, this, [this] {
        if (cloud_save_selected_title_id != 0) {
            controller->ManualSaveDownload(cloud_save_selected_title_id);
        }
    });

    auto* cloud_save_buttons = new QHBoxLayout;
    cloud_save_buttons->addStretch(1);
    cloud_save_buttons->addWidget(cloud_save_download_button);
    cloud_save_buttons->addStretch(1);

    auto* cloud_save_card = new QFrame;
    cloud_save_card->setObjectName(QStringLiteral("cloudSaveCard"));
    cloud_save_card->setStyleSheet(QStringLiteral("QFrame#cloudSaveCard { background: %1; "
                                                  "border-radius: 12px; }")
                                       .arg(CardBg().name()));
    auto* cloud_save_card_layout = new QVBoxLayout(cloud_save_card);
    cloud_save_card_layout->setContentsMargins(28, 28, 28, 28);
    cloud_save_card_layout->setSpacing(6);
    cloud_save_card_layout->addWidget(cloud_save_icon, 0, Qt::AlignHCenter);
    cloud_save_card_layout->addWidget(cloud_save_title, 0, Qt::AlignHCenter);
    cloud_save_card_layout->addWidget(cloud_save_picker_container, 0, Qt::AlignHCenter);
    cloud_save_card_layout->addSpacing(6);
    cloud_save_card_layout->addWidget(cloud_save_status);
    cloud_save_card_layout->addSpacing(12);
    cloud_save_card_layout->addLayout(cloud_save_buttons);

    auto* cloud_save_page = new QWidget;
    auto* cloud_save_layout = new QVBoxLayout(cloud_save_page);
    cloud_save_layout->setContentsMargins(20, 20, 20, 20);
    cloud_save_layout->addStretch(1);
    cloud_save_layout->addWidget(cloud_save_card);
    cloud_save_layout->addStretch(2);

    auto* requests_page = new QWidget;
    auto* requests_page_layout = new QVBoxLayout(requests_page);
    requests_page_layout->setContentsMargins(0, 0, 0, 0);
    requests_page_layout->setSpacing(0);
    requests_page_layout->addWidget(requests_stack, 1);
    requests_page_layout->addWidget(outgoing_requests_section);

    tabs = new QTabWidget;
    tabs->setStyleSheet(TabWidgetStyle());
    tabs->addTab(friends_stack, tr("Friends"));
    tabs->addTab(requests_page, tr("Requests"));
    tabs->addTab(history_stack, tr("Recently Played"));
    tabs->addTab(cloud_save_page, tr("Cloud Saves"));

    requests_badge = new QLabel(tabs->tabBar());
    requests_badge->setAlignment(Qt::AlignCenter);
    requests_badge->setFixedHeight(16);
    requests_badge->hide();

    status = new QLabel;
    status->setWordWrap(true);

    auto* notifications_toggle = new QCheckBox(tr("Notifications"));
    notifications_toggle->setChecked(UISettings::values.legacy_notifications_enabled.GetValue());
    connect(notifications_toggle, &QCheckBox::toggled, this,
            [](bool checked) { UISettings::values.legacy_notifications_enabled.SetValue(checked); });

    auto* notification_corner = new QComboBox;
    notification_corner->addItem(tr("Top Right"));
    notification_corner->addItem(tr("Top Left"));
    notification_corner->addItem(tr("Bottom Right"));
    notification_corner->addItem(tr("Bottom Left"));
    notification_corner->setCurrentIndex(
        std::clamp(UISettings::values.legacy_notification_corner.GetValue(), 0, 3));
    connect(notification_corner, &QComboBox::currentIndexChanged, this,
            [](int index) { UISettings::values.legacy_notification_corner.SetValue(index); });

    auto* status_row = new QHBoxLayout;
    status_row->addWidget(status, 1);
    status_row->addWidget(notifications_toggle);
    status_row->addWidget(notification_corner);

    nat_label = new QLabel(tr("NAT: Not tested"));
    ping_label = new QLabel(tr("Ping: --"));
    auto* test_connection_button = new QPushButton(tr("Test Connection"));

    auto* network_row = new QHBoxLayout;
    network_row->addWidget(nat_label);
    network_row->addWidget(ping_label);
    network_row->addStretch(1);
    network_row->addWidget(test_connection_button);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(12);
    layout->addWidget(header_card);
    layout->addLayout(add_row);
    layout->addWidget(tabs, 1);
    layout->addLayout(status_row);
    layout->addLayout(network_row);

    network_probe = new ExternalReferenceProbe(this);
    connect(network_probe, &ExternalReferenceProbe::NatStatusChanged, this,
            [this](ExternalReferenceProbe::NatStatus nat_status) {
                switch (nat_status) {
                case ExternalReferenceProbe::NatStatus::Checking:
                    nat_label->setText(tr("NAT: Checking..."));
                    break;
                case ExternalReferenceProbe::NatStatus::Open:
                    nat_label->setText(tr("NAT: Open"));
                    break;
                case ExternalReferenceProbe::NatStatus::Unknown:
                    nat_label->setText(tr("NAT: Unknown"));
                    break;
                }
            });
    connect(network_probe, &ExternalReferenceProbe::PingResult, this, [this](int ms) {
        ping_label->setText(ms >= 0 ? tr("Ping: %1 ms").arg(ms) : tr("Ping: --"));
    });
    connect(test_connection_button, &QPushButton::clicked, this, [this, test_connection_button] {
        network_probe->ProbeNat();
        network_probe->PingBackend();
        test_connection_button->setEnabled(false);
        QTimer::singleShot(10000, test_connection_button, [test_connection_button] {
            test_connection_button->setEnabled(true);
        });
    });

    connect(add_button, &QPushButton::clicked, this, &OpenPakAccountDialog::OnAdd);
    connect(friend_code_input, &QLineEdit::returnPressed, this, &OpenPakAccountDialog::OnAdd);

    header_name->setText(QString::fromStdString(Common::OpenPakAccount::GetUsername()));
    header_code->setText(QString::fromStdString(Common::OpenPakAccount::GetFriendCode()));

    RefreshFriends();
    RefreshHistory();
    RefreshCloudSaveTab();

    connect(controller, &OpenPakController::StatusChanged, this,
            [this, cloud_save_page](const QString& message) {
                if (tabs->currentWidget() == cloud_save_page) {
                    cloud_save_status->setText(message);
                }
            });

    refresh_timer.setInterval(15000);
    connect(&refresh_timer, &QTimer::timeout, this, &OpenPakAccountDialog::RefreshFriends);
    connect(&refresh_timer, &QTimer::timeout, this, &OpenPakAccountDialog::RefreshHistory);
    connect(&refresh_timer, &QTimer::timeout, this, &OpenPakAccountDialog::RefreshCloudSaveTab);
    refresh_timer.start();

    connect(tabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (tabs->widget(index) == history_stack) {
            RefreshHistory();
        }
    });

#ifdef ENABLE_WEB_SERVICE
    std::thread{[this, guard = QPointer<OpenPakAccountDialog>(this)] {
        auto profile = WebService::OpenPakApi::GetProfile();
        if (!profile.ok || profile.image_base64.empty() || !guard) {
            return;
        }
        QMetaObject::invokeMethod(
            guard.data(),
            [this, guard, image = profile.image_base64] {
                if (!guard) {
                    return;
                }
                const QPixmap avatar = OpenPak::AvatarCache::Get("self", image, kHeaderAvatarSize);
                if (!avatar.isNull()) {
                    header_avatar->setPixmap(RoundedPixmap(avatar, kHeaderAvatarSize));
                }
            },
            Qt::QueuedConnection);
    }}.detach();
#endif
}

OpenPakAccountDialog::~OpenPakAccountDialog() = default;

bool OpenPakAccountDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == header_code && event->type() == QEvent::MouseButtonRelease) {
        QApplication::clipboard()->setText(header_code->text());
        status->setText(tr("Friend code copied."));
        return true;
    }
    if (watched == header_avatar && event->type() == QEvent::MouseButtonRelease) {
        OnChangeAvatar();
        return true;
    }
    return QDialog::eventFilter(watched, event);
}

void OpenPakAccountDialog::OnChangeAvatar() {
#ifdef ENABLE_WEB_SERVICE
    const QString path = QFileDialog::getOpenFileName(this, tr("Choose a profile picture"),
                                                       QString{}, tr("Images (*.png *.jpg *.jpeg)"));
    if (path.isEmpty()) {
        return;
    }

    QImage image(path);
    if (image.isNull()) {
        status->setText(tr("Couldn't read that image."));
        return;
    }
    const int side = std::min(image.width(), image.height());
    const QRect crop_rect((image.width() - side) / 2, (image.height() - side) / 2, side, side);
    const QImage square = image.copy(crop_rect).scaled(256, 256, Qt::KeepAspectRatio,
                                                        Qt::SmoothTransformation);

    QByteArray jpeg_bytes;
    QBuffer buffer(&jpeg_bytes);
    buffer.open(QIODevice::WriteOnly);
    square.save(&buffer, "JPG", 85);
    const std::string image_base64 = jpeg_bytes.toBase64().toStdString();

    header_avatar->setPixmap(RoundedPixmap(QPixmap::fromImage(square), kHeaderAvatarSize));
    status->setText(tr("Uploading profile picture..."));

    std::thread{[this, image_base64, guard = QPointer<OpenPakAccountDialog>(this)] {
        const std::string error = WebService::OpenPakApi::PushProfilePicture(image_base64);
        if (!guard) {
            return;
        }
        QMetaObject::invokeMethod(
            guard.data(),
            [this, guard, error] {
                if (!guard) {
                    return;
                }
                status->setText(error.empty() ? tr("Profile picture updated.")
                                             : QString::fromStdString(error));
            },
            Qt::QueuedConnection);
    }}.detach();
#else
    status->setText(tr("This build has no web services support."));
#endif
}

void OpenPakAccountDialog::OnEditUsername() {
#ifdef ENABLE_WEB_SERVICE
    static const QRegularExpression valid_name(QStringLiteral("^[A-Za-z0-9_-]{3,16}$"));

    bool ok = false;
    const QString name = QInputDialog::getText(
                             this, tr("Change Username"),
                             tr("3-16 characters: letters, digits, '_' or '-'"), QLineEdit::Normal,
                             header_name->text(), &ok)
                             .trimmed();
    if (!ok || name.isEmpty() || name == header_name->text()) {
        return;
    }
    if (!valid_name.match(name).hasMatch()) {
        status->setText(tr("Invalid username."));
        return;
    }

    status->setText(tr("Updating username..."));
    const std::string new_name = name.toStdString();

    std::thread{[this, new_name, guard = QPointer<OpenPakAccountDialog>(this)] {
        const std::string error = WebService::OpenPakApi::SetUsername(new_name);
        if (!guard) {
            return;
        }
        QMetaObject::invokeMethod(
            guard.data(),
            [this, guard, error, new_name] {
                if (!guard) {
                    return;
                }
                if (error.empty()) {
                    Common::OpenPakAccount::Save(Common::OpenPakAccount::GetPid(), new_name,
                                                  Common::OpenPakAccount::GetFriendCode(),
                                                  Common::OpenPakAccount::GetToken());
                    header_name->setText(QString::fromStdString(new_name));
                    status->setText(tr("Username updated."));
                } else {
                    status->setText(QString::fromStdString(error));
                }
            },
            Qt::QueuedConnection);
    }}.detach();
#else
    status->setText(tr("This build has no web services support."));
#endif
}

void OpenPakAccountDialog::SetBusy(bool busy) {
    add_button->setEnabled(!busy);
}

u64 OpenPakAccountDialog::SelectedPid(const QModelIndex& index) const {
    return index.isValid() ? index.data(OpenPakFriendItem::PidRole).toULongLong() : 0;
}

void OpenPakAccountDialog::ApplyFriendFilter(const QString& text) {
    for (int row = 0; row < friends_model->rowCount(); ++row) {
        const QString name =
            friends_model->index(row, 0).data(OpenPakFriendItem::NameRole).toString();
        friends_view->setRowHidden(row, !text.isEmpty() && !name.contains(text, Qt::CaseInsensitive));
    }
}

void OpenPakAccountDialog::UpdateRequestsBadge(int count) {
    if (count <= 0) {
        // A hidden tab button still reserves its layout space, which is what left "Requests"
        // looking permanently off-center; detaching it entirely lets the label re-center.
        tabs->tabBar()->setTabButton(1, QTabBar::RightSide, nullptr);
        requests_badge->hide();
        return;
    }
    requests_badge->setText(count > 99 ? QStringLiteral("99+") : QString::number(count));
    requests_badge->setStyleSheet(
        QStringLiteral("QLabel { background-color: #e0393e; color: white; font-size: 10px; "
                       "font-weight: bold; border-radius: 8px; padding: 0 5px; }"));
    requests_badge->adjustSize();
    tabs->tabBar()->setTabButton(1, QTabBar::RightSide, requests_badge);
    requests_badge->show();
}

void OpenPakAccountDialog::OnFriendsViewClicked(const QModelIndex& index) {
    if (!index.isValid()) {
        return;
    }
    auto* view = qobject_cast<QListView*>(sender());
    if (view == outgoing_requests_view) {
        const QRect cell_rect = view->visualRect(index);
        const QPoint pos = view->viewport()->mapFromGlobal(QCursor::pos());
        const auto hit = outgoing_request_delegate->HitTestActions(cell_rect, pos, false);
        if (hit != OpenPakFriendDelegate::ActionHit::None) {
            const std::string code =
                index.data(OpenPakFriendItem::FriendCodeRole).toString().toStdString();
            Common::OpenPakOutgoingRequests::Remove(code);
            outgoing_requests_model->removeRow(index.row());
            outgoing_requests_section->setVisible(outgoing_requests_model->rowCount() > 0);
        }
        return;
    }

    auto* delegate = view == requests_view ? request_delegate : friend_delegate;
    const bool is_request = index.data(OpenPakFriendItem::IsRequestRole).toBool();

    const QRect cell_rect = view->visualRect(index);
    const QPoint pos = view->viewport()->mapFromGlobal(QCursor::pos());
    const auto hit = delegate->HitTestActions(cell_rect, pos, is_request);
    if (hit == OpenPakFriendDelegate::ActionHit::None) {
        return;
    }

    const u64 pid = SelectedPid(index);
    if (pid == 0) {
        return;
    }

#ifdef ENABLE_WEB_SERVICE
    if (is_request) {
        if (hit == OpenPakFriendDelegate::ActionHit::Primary) {
            RunAsync([pid] { return WebService::OpenPakApi::AcceptFriend(pid); });
        } else {
            RunAsync([pid] { return WebService::OpenPakApi::DeclineFriend(pid); });
        }
    } else {
        const QString name = index.data(OpenPakFriendItem::NameRole).toString();
        const auto confirm = QMessageBox::question(
            this, tr("Remove Friend"), tr("Remove %1 from your friends list?").arg(name),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (confirm != QMessageBox::Yes) {
            return;
        }
        RunAsync([pid] { return WebService::OpenPakApi::RemoveFriend(pid); });
    }
#endif
}

void OpenPakAccountDialog::RunAsync(std::function<std::string()> task,
                                     std::function<void()> on_success) {
#ifdef ENABLE_WEB_SERVICE
    SetBusy(true);
    status->setText(tr("Working..."));

    std::thread{[this, work = std::move(task), success_cb = std::move(on_success),
                guard = QPointer<OpenPakAccountDialog>(this)] {
        const std::string result = work();
        if (!guard) {
            return;
        }

        QMetaObject::invokeMethod(
            guard.data(),
            [this, guard, error = result, success_cb] {
                if (!guard) {
                    return;
                }
                SetBusy(false);
                if (error.empty()) {
                    RefreshFriends();
                    if (controller) {
                        controller->RefreshFriendCache();
                    }
                    if (success_cb) {
                        success_cb();
                    }
                } else {
                    status->setText(QString::fromStdString(error));
                }
            },
            Qt::QueuedConnection);
    }}.detach();
#endif
}

void OpenPakAccountDialog::OnAdd() {
#ifdef ENABLE_WEB_SERVICE
    const std::string code = friend_code_input->text().trimmed().toStdString();
    if (code.empty()) {
        status->setText(tr("Enter a friend code first."));
        return;
    }
    friend_code_input->clear();
    RunAsync([code] { return WebService::OpenPakApi::AddFriendByCode(code); }, [this, code] {
        Common::OpenPakOutgoingRequests::Add(code);
        if (controller) {
            controller->NotifyFriendRequestSent(QString::fromStdString(code));
        }
    });
#endif
}

void OpenPakAccountDialog::RefreshFriends() {
#ifdef ENABLE_WEB_SERVICE
    SetBusy(true);
    status->setText(tr("Loading..."));

    std::thread{[this, guard = QPointer<OpenPakAccountDialog>(this)] {
        auto fetched = WebService::OpenPakApi::GetFriends();
        if (!guard) {
            return;
        }

        QMetaObject::invokeMethod(
            guard.data(),
            [this, guard, list = std::move(fetched)]() mutable {
                if (!guard) {
                    return;
                }
                SetBusy(false);
                friends_model->clear();
                requests_model->clear();

                if (!list.ok) {
                    status->setText(QString::fromStdString(list.error));
                    return;
                }

                const std::string local_app_id = controller ? controller->GetLocalAppId() : std::string{};
                std::unordered_map<std::string, int> group_size;
                for (const auto& entry : list.friends) {
                    if (entry.presence_status != 0 && !entry.app_id.empty()) {
                        ++group_size[entry.app_id];
                    }
                }
                // Rank: 0 = playing what I'm playing, 1..N = other games (bigger group first),
                // N+1 = online with no game, N+2 = offline. Name breaks ties within a rank.
                const auto rank = [&](const WebService::OpenPakApi::Friend& f) -> int {
                    if (f.presence_status == 0) {
                        return static_cast<int>(group_size.size()) + 2;
                    }
                    if (f.app_id.empty()) {
                        return static_cast<int>(group_size.size()) + 1;
                    }
                    if (!local_app_id.empty() && f.app_id == local_app_id) {
                        return 0;
                    }
                    return 1; // refined below by group size, same tier is fine for a stable sort
                };
                std::stable_sort(list.friends.begin(), list.friends.end(),
                                 [&](const WebService::OpenPakApi::Friend& a, const WebService::OpenPakApi::Friend& b) {
                                     const int ra = rank(a);
                                     const int rb = rank(b);
                                     if (ra != rb) {
                                         return ra < rb;
                                     }
                                     if (ra == 1 && a.app_id != b.app_id) {
                                         return group_size[a.app_id] > group_size[b.app_id];
                                     }
                                     return a.name < b.name;
                                 });

                for (const auto& entry : list.friends) {
                    const QString game =
                        controller ? controller->ResolveGameName(entry.app_id, entry.app_name)
                                   : QString{};
                    friends_model->appendRow(new OpenPakFriendItem(
                        entry.pid, QString::fromStdString(entry.name),
                        QString::fromStdString(entry.friend_code), entry.presence_status, game,
                        QString::fromStdString(entry.image_base64), false));
                }
                for (const auto& entry : list.requests) {
                    requests_model->appendRow(new OpenPakFriendItem(
                        entry.pid, QString::fromStdString(entry.name),
                        QString::fromStdString(entry.friend_code), entry.presence_status,
                        QString{}, QString::fromStdString(entry.image_base64), true));
                }

                std::vector<std::string> accepted_codes;
                accepted_codes.reserve(list.friends.size());
                for (const auto& entry : list.friends) {
                    accepted_codes.push_back(entry.friend_code);
                }
                Common::OpenPakOutgoingRequests::PruneAccepted(accepted_codes);

                outgoing_requests_model->clear();
                for (const auto& entry : Common::OpenPakOutgoingRequests::Get()) {
                    outgoing_requests_model->appendRow(new OpenPakFriendItem(
                        0, QString::fromStdString(entry.friend_code),
                        QString::fromStdString(entry.friend_code), 0, QString{}, QString{}, false,
                        tr("Cancel")));
                }
                outgoing_requests_section->setVisible(outgoing_requests_model->rowCount() > 0);

                status->setText(tr("%1 friend(s), %2 request(s).")
                                    .arg(list.friends.size())
                                    .arg(list.requests.size()));
                UpdateRequestsBadge(static_cast<int>(list.requests.size()));

                friends_stack->setCurrentIndex(friends_model->rowCount() > 0 ? 0 : 1);
                requests_stack->setCurrentIndex(requests_model->rowCount() > 0 ? 0 : 1);
                ApplyFriendFilter(friend_search->text());
            },
            Qt::QueuedConnection);
    }}.detach();
#else
    status->setText(tr("This build has no web services support."));
    SetBusy(false);
#endif
}

void OpenPakAccountDialog::RefreshCloudSaveTab() {
    const std::string app_id_hex = controller ? controller->GetLocalAppId() : std::string{};

    u64 running_title_id = 0;
    if (!app_id_hex.empty()) {
        try {
            running_title_id = std::stoull(app_id_hex, nullptr, 16);
        } catch (const std::exception&) {
        }
    }
    const bool is_running_eligible =
        running_title_id != 0 && OpenPak::CompatibleTitles::Table().count(running_title_id);

    if (is_running_eligible) {
        cloud_save_icon->setVisible(true);
        cloud_save_title->setVisible(true);
        cloud_save_picker_container->setVisible(false);
        cloud_save_download_button->setVisible(false);

        const QString game_name = controller->ResolveGameName(app_id_hex);
        const QString game_icon = controller->ResolveGameIcon(app_id_hex);
        cloud_save_title->setText(game_name);
        const QPixmap icon = RoundedRectPixmap(
            OpenPak::AvatarCache::Get(app_id_hex, game_icon.toStdString(), 64), 64, 10);
        if (!icon.isNull()) {
            cloud_save_icon->setPixmap(icon);
        } else {
            cloud_save_icon->clear();
        }
        cloud_save_status->setText(tr("Save uploads automatically when you stop this game."));
        return;
    }

    cloud_save_icon->setVisible(false);
    cloud_save_title->setVisible(false);
    cloud_save_download_button->setVisible(true);
    cloud_save_picker_container->setVisible(true);
    RebuildCloudSaveTitlePicker();

    if (!app_id_hex.empty()) {
        cloud_save_status->setText(tr("The running game doesn't support cloud saves."));
    } else if (!cloud_save_probing.empty()) {
        cloud_save_status->setText(tr("Checking for cloud saves..."));
    } else if (cloud_save_picker_row->count() > 0) {
        cloud_save_status->setText(tr("Pick a title to download its cloud save."));
    } else {
        cloud_save_status->setText(tr("No cloud saves found for your installed games."));
    }
}

void OpenPakAccountDialog::RebuildCloudSaveTitlePicker() {
    for (QAbstractButton* button : cloud_save_picker_group->buttons()) {
        cloud_save_picker_group->removeButton(button);
        button->deleteLater();
    }
    QLayoutItem* item;
    while ((item = cloud_save_picker_row->takeAt(0)) != nullptr) {
        delete item;
    }

    bool selection_still_valid = false;
    for (const auto& [program_id, version] : OpenPak::CompatibleTitles::Table()) {
        const std::string title_id_hex = fmt::format("{:016X}", program_id);
        const QString name = controller->ResolveGameName(title_id_hex);
        const QString icon_b64 = controller->ResolveGameIcon(title_id_hex);
        if (icon_b64.isEmpty()) {
            continue; // Not installed locally -- nothing to show for it.
        }

        const auto has_data_it = cloud_save_has_data.find(program_id);
        if (has_data_it == cloud_save_has_data.end()) {
            ProbeCloudSaveAvailability(program_id);
            continue; // Don't show until we actually know there's a cloud save.
        }
        if (!has_data_it->second) {
            continue;
        }

        auto* button = new QToolButton;
        button->setCheckable(true);
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setIconSize(QSize(48, 48));
        button->setIcon(RoundedRectPixmap(
            OpenPak::AvatarCache::Get(title_id_hex, icon_b64.toStdString(), 48), 48, 8));
        button->setText(QFontMetrics(button->font()).elidedText(name, Qt::ElideRight, 92));
        button->setToolTip(name);
        button->setCursor(Qt::PointingHandCursor);
        button->setAutoRaise(true);
        button->setFixedWidth(104);
        if (program_id == cloud_save_selected_title_id) {
            button->setChecked(true);
            selection_still_valid = true;
        }
        connect(button, &QToolButton::clicked, this, [this, program_id] {
            cloud_save_selected_title_id = program_id;
            cloud_save_download_button->setEnabled(true);
        });

        cloud_save_picker_group->addButton(button);
        cloud_save_picker_row->addWidget(button);
    }

    if (!selection_still_valid) {
        cloud_save_selected_title_id = 0;
    }
    cloud_save_download_button->setEnabled(cloud_save_selected_title_id != 0);
}

void OpenPakAccountDialog::ProbeCloudSaveAvailability(u64 title_id) {
#ifdef ENABLE_WEB_SERVICE
    if (cloud_save_probing.count(title_id)) {
        return;
    }
    cloud_save_probing.insert(title_id);

    const std::string title_id_hex = fmt::format("{:016x}", title_id);
    std::thread{[this, title_id, title_id_hex, guard = QPointer<OpenPakAccountDialog>(this)] {
        const auto save = WebService::OpenPakApi::PullSave(title_id_hex);
        const bool has_data = save.has_value() && !save->empty();

        QMetaObject::invokeMethod(
            guard.data(),
            [this, guard, title_id, has_data] {
                if (!guard) {
                    return;
                }
                cloud_save_probing.erase(title_id);
                cloud_save_has_data[title_id] = has_data;
                RefreshCloudSaveTab();
            },
            Qt::QueuedConnection);
    }}.detach();
#endif
}

void OpenPakAccountDialog::RefreshHistory() {
#ifdef ENABLE_WEB_SERVICE
    std::thread{[this, guard = QPointer<OpenPakAccountDialog>(this)] {
        auto fetched = WebService::OpenPakApi::GetHistory();
        if (!fetched.ok || !guard) {
            return;
        }

        QMetaObject::invokeMethod(
            guard.data(),
            [this, guard, list = std::move(fetched)] {
                if (!guard) {
                    return;
                }
                history_model->clear();
                for (const auto& entry : list.entries) {
                    // Prefer the locally installed game's own NACP over the server's copy.
                    const QString local_name =
                        controller ? controller->ResolveGameName(entry.title_id) : QString{};
                    const QString local_icon =
                        controller ? controller->ResolveGameIcon(entry.title_id) : QString{};
                    const QString name = (!local_name.isEmpty() && local_name != tr("a game"))
                                             ? local_name
                                             : QString::fromStdString(entry.name);
                    if (name.contains(QStringLiteral(".nca"), Qt::CaseInsensitive)) {
                        continue;
                    }
                    const QString icon =
                        !local_icon.isEmpty() ? local_icon : QString::fromStdString(entry.icon_base64);
                    history_model->appendRow(new OpenPakHistoryItem(
                        QString::fromStdString(entry.title_id), name, icon, entry.seconds,
                        QString::fromStdString(entry.last_played)));
                }
                history_stack->setCurrentIndex(history_model->rowCount() > 0 ? 0 : 1);
            },
            Qt::QueuedConnection);
    }}.detach();
#endif
}
