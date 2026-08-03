// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QUdpSocket;
class QTimer;

// One-shot UPnP IGD probe (the Friends dialog's "NAT" indicator) and backend latency check (its
// "Ping" indicator). Both are fire-and-forget; results come back via signals.
class ExternalReferenceProbe : public QObject {
    Q_OBJECT

public:
    enum class NatStatus { Checking, Open, Unknown };

    explicit ExternalReferenceProbe(QObject* parent = nullptr);
    ~ExternalReferenceProbe() override;

    void ProbeNat();
    void PingBackend();

signals:
    void NatStatusChanged(ExternalReferenceProbe::NatStatus status);
    void PingResult(int ms); // -1 on failure

private:
    void OnSsdpReadyRead();
    void FetchDeviceDescription(const QString& location);
    void OnDeviceDescriptionReply(QNetworkReply* reply, QString base_url);
    void CallGetExternalIpAddress(const QString& control_url, const QString& service_type);
    void OnGetExternalIpReply(QNetworkReply* reply);
    void Finish(NatStatus status);

    QNetworkAccessManager* network_manager;
    QUdpSocket* ssdp_socket = nullptr;
    QTimer* ssdp_timeout = nullptr;
    bool nat_resolved = false;
};
