// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2025 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/bcat/bcat_service.h"
#include "core/hle/service/bcat/delivery_cache_progress_service.h"
#include "core/hle/service/bcat/delivery_cache_storage_service.h"
#include "core/hle/service/bcat/service_creator.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/filesystem.h"

#include "common/logging.h"
#include "common/settings.h"
#include "core/file_sys/vfs/vfs.h"
#include "openpak/bcat.h"

namespace Service::BCAT {

namespace {

/// OpenPak's BCAT data for a title, written into the directory its delivery cache is read from
/// (<dir>/<file>, as IDeliveryCacheStorageService enumerates it). The news service's dataset
/// replaces whatever OpenPak wrote before; with nothing published, a cache OpenPak filled (it
/// carries the marker) is emptied, and one it never touched is left alone.
void DeliverOpenPak(const FileSys::VirtualDir& root, u64 title_id) {
    constexpr std::string_view Marker = ".openpak";

    if (root == nullptr) {
        return;
    }

    const openpak::bcat::Result fetched = openpak::bcat::Fetch(title_id);
    if (fetched.outcome == openpak::bcat::Outcome::Unreachable ||
        (fetched.outcome == openpak::bcat::Outcome::None && root->GetFile(Marker) == nullptr)) {
        return;
    }

    for (const auto& directory : root->GetSubdirectories()) {
        root->DeleteSubdirectoryRecursive(directory->GetName());
    }
    root->DeleteFile(Marker);

    for (const auto& file : fetched.files) {
        auto directory = root->GetSubdirectory(file.directory);
        if (directory == nullptr) {
            directory = root->CreateSubdirectory(file.directory);
        }
        const auto out = directory != nullptr ? directory->CreateFile(file.name) : nullptr;
        if (out == nullptr || !out->Resize(file.data.size()) ||
            out->WriteBytes(file.data) != file.data.size()) {
            LOG_WARNING(Service_BCAT, "[OpenPak] Could not write {}/{} for {:016X}",
                        file.directory, file.name, title_id);
        }
    }

    if (!fetched.files.empty()) {
        root->CreateFile(Marker);
    }

    LOG_INFO(Service_BCAT, "[OpenPak] {} BCAT file(s) delivered to {:016X}", fetched.files.size(),
             title_id);
}

/// The delivery cache filled from OpenPak's news service when the title asks for a sync, which
/// is when a console's bcat daemon would hand over what it downloaded. The fetch happens on the
/// sync call itself and is bounded by the library's timeouts; the title is waiting on the
/// progress event for exactly this.
class OpenPakBcatBackend final : public NullBcatBackend {
public:
    using NullBcatBackend::NullBcatBackend;

    bool Synchronize(TitleIDVersion title, ProgressServiceBackend& progress) override {
        progress.StartConnecting();
        DeliverOpenPak(dir_getter(title.title_id), title.title_id);
        progress.FinishDownload(ResultSuccess);
        return true;
    }

    bool SynchronizeDirectory(TitleIDVersion title, std::string name,
                              ProgressServiceBackend& progress) override {
        return Synchronize(title, progress);
    }
};

} // namespace

std::unique_ptr<BcatBackend> CreateBackendFromSettings([[maybe_unused]] Core::System& system,
                                                       DirectoryGetter getter) {
    if (Settings::values.enable_openpak.GetValue()) {
        return std::make_unique<OpenPakBcatBackend>(std::move(getter));
    }
    return std::make_unique<NullBcatBackend>(std::move(getter));
}

IServiceCreator::IServiceCreator(Core::System& system_, const char* name_)
    : ServiceFramework{system_, name_}, fsc{system.GetFileSystemController()}, progress{{
          ProgressServiceBackend{system_, "Normal"},
          ProgressServiceBackend{system_, "Directory"},
      }} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IServiceCreator::CreateBcatService>, "CreateBcatService"},
        {1, D<&IServiceCreator::CreateDeliveryCacheStorageService>, "CreateDeliveryCacheStorageService"},
        {2, D<&IServiceCreator::CreateDeliveryCacheStorageServiceWithApplicationId>, "CreateDeliveryCacheStorageServiceWithApplicationId"},
        {3, D<&IServiceCreator::CreateDeliveryCacheProgressService>, "CreateDeliveryCacheProgressService"},
        {4, D<&IServiceCreator::CreateDeliveryCacheProgressServiceWithApplicationId>, "CreateDeliveryCacheProgressServiceWithApplicationId"},
    };
    // clang-format on

    RegisterHandlers(functions);

    backend =
        CreateBackendFromSettings(system_, [this](u64 tid) { return fsc.GetBCATDirectory(tid); });
}

IServiceCreator::~IServiceCreator() = default;

Result IServiceCreator::CreateBcatService(ClientProcessId process_id,
                                          OutInterface<IBcatService> out_interface) {
    LOG_INFO(Service_BCAT, "called, process_id={}", process_id.pid);
    *out_interface = std::make_shared<IBcatService>(system, *backend);
    R_SUCCEED();
}

Result IServiceCreator::CreateDeliveryCacheStorageService(
    ClientProcessId process_id, OutInterface<IDeliveryCacheStorageService> out_interface) {
    LOG_INFO(Service_BCAT, "called, process_id={}", process_id.pid);

    const auto title_id = system.GetApplicationProcessProgramID();
    *out_interface =
        std::make_shared<IDeliveryCacheStorageService>(system, fsc.GetBCATDirectory(title_id));
    R_SUCCEED();
}

Result IServiceCreator::CreateDeliveryCacheStorageServiceWithApplicationId(
    u64 application_id, OutInterface<IDeliveryCacheStorageService> out_interface) {
    LOG_DEBUG(Service_BCAT, "called, application_id={:016X}", application_id);
    *out_interface = std::make_shared<IDeliveryCacheStorageService>(
        system, fsc.GetBCATDirectory(application_id));
    R_SUCCEED();
}

Result IServiceCreator::CreateDeliveryCacheProgressService(
    ClientProcessId process_id, OutInterface<IDeliveryCacheProgressService> out_interface) {
    LOG_INFO(Service_BCAT, "called, process_id={}", process_id.pid);

    auto& progress_backend = progress.at(static_cast<size_t>(SyncType::Normal));
    *out_interface = std::make_shared<IDeliveryCacheProgressService>(
        system, progress_backend.GetEvent(), progress_backend.GetImpl());
    R_SUCCEED();
}

Result IServiceCreator::CreateDeliveryCacheProgressServiceWithApplicationId(
    u64 application_id, OutInterface<IDeliveryCacheProgressService> out_interface) {
    LOG_INFO(Service_BCAT, "called, application_id={:016X}", application_id);

    auto& progress_backend = progress.at(static_cast<size_t>(SyncType::Normal));
    *out_interface = std::make_shared<IDeliveryCacheProgressService>(
        system, progress_backend.GetEvent(), progress_backend.GetImpl());
    R_SUCCEED();
}

} // namespace Service::BCAT
