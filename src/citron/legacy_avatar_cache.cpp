// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <unordered_map>
#include <utility>

#include <QByteArray>

#include "citron/legacy_avatar_cache.h"

namespace OpenPak::AvatarCache {

namespace {

struct CacheEntry {
    std::string source_hash; // the base64 string itself; cheap enough and self-invalidating
    QPixmap pixmap;
};

std::unordered_map<std::string, CacheEntry>& Cache() {
    static std::unordered_map<std::string, CacheEntry> cache;
    return cache;
}

} // Anonymous namespace

QPixmap Get(const std::string& key, const std::string& image_base64, int size) {
    if (image_base64.empty()) {
        return {};
    }

    auto& cache = Cache();
    if (const auto it = cache.find(key); it != cache.end() && it->second.source_hash == image_base64) {
        return it->second.pixmap;
    }

    const QByteArray raw = QByteArray::fromBase64(QByteArray::fromStdString(image_base64));
    QPixmap pixmap;
    if (raw.isEmpty() || !pixmap.loadFromData(raw)) {
        cache.erase(key);
        return {};
    }

    pixmap = pixmap.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    cache[key] = CacheEntry{image_base64, pixmap};
    return pixmap;
}

void Clear() {
    Cache().clear();
}

} // namespace OpenPak::AvatarCache
