// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "citron/legacy_network_probe.h"

#include <thread>

#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUdpSocket>
#include <QXmlStreamReader>

#ifdef ENABLE_WEB_SERVICE
#include "web_service/legacy_api.h"
#endif

namespace {
constexpr int kSsdpTimeoutMs = 3000;
constexpr quint16 kSsdpPort = 1900;

QByteArray SsdpSearchRequest() {
    return QByteArrayLiteral("M-SEARCH * HTTP/1.1\r\n"
                             "HOST: 239.255.255.250:1900\r\n"
                             "MAN: \"ssdp:discover\"\r\n"
                             "MX: 2\r\n"
                             "ST: upnp:rootdevice\r\n\r\n");
}

QString ExtractHeader(const QString& response, const QString& header) {
    for (const QString& line : response.split(QStringLiteral("\r\n"))) {
        if (line.startsWith(header, Qt::CaseInsensitive)) {
            return line.mid(header.size()).trimmed();
        }
    }
    return {};
}
} // namespace

ExternalReferenceProbe::ExternalReferenceProbe(QObject* parent)
    : QObject(parent), network_manager(new QNetworkAccessManager(this)) {}

ExternalReferenceProbe::~ExternalReferenceProbe() = default;

void ExternalReferenceProbe::ProbeNat() {
    nat_resolved = false;
    emit NatStatusChanged(NatStatus::Checking);

    delete ssdp_socket;
    ssdp_socket = new QUdpSocket(this);
    ssdp_socket->bind(QHostAddress::AnyIPv4, 0);
    connect(ssdp_socket, &QUdpSocket::readyRead, this, &ExternalReferenceProbe::OnSsdpReadyRead);
    ssdp_socket->writeDatagram(SsdpSearchRequest(), QHostAddress(QStringLiteral("239.255.255.250")),
                               kSsdpPort);

    delete ssdp_timeout;
    ssdp_timeout = new QTimer(this);
    ssdp_timeout->setSingleShot(true);
    connect(ssdp_timeout, &QTimer::timeout, this, [this] { Finish(NatStatus::Unknown); });
    ssdp_timeout->start(kSsdpTimeoutMs);
}

void ExternalReferenceProbe::OnSsdpReadyRead() {
    while (ssdp_socket && ssdp_socket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(static_cast<int>(ssdp_socket->pendingDatagramSize()));
        ssdp_socket->readDatagram(datagram.data(), datagram.size());
        if (nat_resolved) {
            continue;
        }
        const QString location =
            ExtractHeader(QString::fromLatin1(datagram), QStringLiteral("LOCATION:"));
        if (!location.isEmpty()) {
            FetchDeviceDescription(location);
        }
    }
}

void ExternalReferenceProbe::FetchDeviceDescription(const QString& location) {
    const QUrl url(location);
    auto* reply = network_manager->get(QNetworkRequest(url));
    const QString base_url = url.scheme() + QStringLiteral("://") + url.authority();
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, base_url] { OnDeviceDescriptionReply(reply, base_url); });
}

void ExternalReferenceProbe::OnDeviceDescriptionReply(QNetworkReply* reply, QString base_url) {
    reply->deleteLater();
    if (nat_resolved || reply->error() != QNetworkReply::NoError) {
        return;
    }

    QXmlStreamReader reader(reply->readAll());
    QString service_type;
    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement()) {
            continue;
        }
        if (reader.name().compare(QStringLiteral("serviceType"), Qt::CaseInsensitive) == 0) {
            service_type = reader.readElementText();
        } else if (reader.name().compare(QStringLiteral("controlURL"), Qt::CaseInsensitive) == 0) {
            const QString control_url = reader.readElementText();
            if (service_type.contains(QStringLiteral("WANIPConnection")) ||
                service_type.contains(QStringLiteral("WANPPPConnection"))) {
                const QString full_url = control_url.startsWith(QStringLiteral("http"))
                                             ? control_url
                                             : base_url + control_url;
                CallGetExternalIpAddress(full_url, service_type);
                return;
            }
        }
    }
}

void ExternalReferenceProbe::CallGetExternalIpAddress(const QString& control_url,
                                                     const QString& service_type) {
    QNetworkRequest request{QUrl(control_url)};
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("text/xml; charset=\"utf-8\""));
    request.setRawHeader("SOAPAction",
                         (QStringLiteral("\"") + service_type + QStringLiteral("#GetExternalIPAddress\""))
                             .toUtf8());

    const QString body =
        QStringLiteral("<?xml version=\"1.0\"?><s:Envelope "
                       "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                       "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
                       "<s:Body><u:GetExternalIPAddress xmlns:u=\"%1\"/></s:Body></s:Envelope>")
            .arg(service_type);

    auto* reply = network_manager->post(request, body.toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply] { OnGetExternalIpReply(reply); });
}

void ExternalReferenceProbe::OnGetExternalIpReply(QNetworkReply* reply) {
    reply->deleteLater();
    if (nat_resolved) {
        return;
    }
    // A successful SOAP reply from the router's WAN service is evidence of a live UPnP IGD.
    Finish(reply->error() == QNetworkReply::NoError ? NatStatus::Open : NatStatus::Unknown);
}

void ExternalReferenceProbe::Finish(NatStatus status) {
    if (nat_resolved) {
        return;
    }
    nat_resolved = true;
    if (ssdp_timeout) {
        ssdp_timeout->stop();
    }
    if (ssdp_socket) {
        ssdp_socket->deleteLater();
        ssdp_socket = nullptr;
    }
    emit NatStatusChanged(status);
}

void ExternalReferenceProbe::PingBackend() {
#ifdef ENABLE_WEB_SERVICE
    std::thread{[this] {
        const auto ms = WebService::OpenPakApi::PingBackend();
        QMetaObject::invokeMethod(
            this, [this, ms] { emit PingResult(ms.value_or(-1)); }, Qt::QueuedConnection);
    }}.detach();
#else
    emit PingResult(-1);
#endif
}
