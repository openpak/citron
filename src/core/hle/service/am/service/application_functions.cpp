// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/nextendo_friends.h"
#include "common/settings.h"
#include "common/uuid.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/savedata_factory.h"
#include "core/hle/kernel/k_transfer_memory.h"
#include "core/hle/kernel/k_thread.h"
#include "core/nextendo_guest_call.h"
#include "core/hle/service/am/am_results.h"
#include "core/hle/service/am/applet.h"
#include "core/hle/service/am/service/application_functions.h"
#include "core/hle/service/am/service/storage.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/filesystem/save_data_controller.h"
#include "core/hle/service/glue/glue_manager.h"
#include "core/hle/service/ns/application_manager_interface.h"
#include "core/hle/service/ns/service_getter_interface.h"
#include "core/hle/service/sm/sm.h"

namespace Service::AM {

IApplicationFunctions::IApplicationFunctions(Core::System& system_, std::shared_ptr<Applet> applet)
    : ServiceFramework{system_, "IApplicationFunctions"}, m_applet{std::move(applet)} {
    // [Nextendo] Weak-bound so a callback firing after this application (and its Applet) has
    // been torn down is a harmless no-op rather than a signal into a dead object. The next
    // application's own constructor overwrites this registration, so there is nothing to
    // explicitly unregister on exit.
    std::weak_ptr<Applet> weak_applet = m_applet;
    Common::NextendoFriends::SetInvitationSignalCallback([weak_applet] {
        if (const auto applet = weak_applet.lock()) {
            applet->friend_invitation_storage_channel_event.Signal();
        }
    });
    // clang-format off
    static const FunctionInfo functions[] = {
        {1, D<&IApplicationFunctions::PopLaunchParameter>, "PopLaunchParameter"},
        {10, nullptr, "CreateApplicationAndPushAndRequestToStart"},
        {11, nullptr, "CreateApplicationAndPushAndRequestToStartForQuest"},
        {12, nullptr, "CreateApplicationAndRequestToStart"},
        {13, nullptr, "CreateApplicationAndRequestToStartForQuest"},
        {14, nullptr, "CreateApplicationWithAttributeAndPushAndRequestToStartForQuest"},
        {15, nullptr, "CreateApplicationWithAttributeAndRequestToStartForQuest"},
        {20, D<&IApplicationFunctions::EnsureSaveData>, "EnsureSaveData"},
        {21, D<&IApplicationFunctions::GetDesiredLanguage>, "GetDesiredLanguage"},
        {22, D<&IApplicationFunctions::SetTerminateResult>, "SetTerminateResult"},
        {23, D<&IApplicationFunctions::GetDisplayVersion>, "GetDisplayVersion"},
        {24, nullptr, "GetLaunchStorageInfoForDebug"},
        {25, D<&IApplicationFunctions::ExtendSaveData>, "ExtendSaveData"},
        {26, D<&IApplicationFunctions::GetSaveDataSize>, "GetSaveDataSize"},
        {27, D<&IApplicationFunctions::CreateCacheStorage>, "CreateCacheStorage"},
        {28, D<&IApplicationFunctions::GetSaveDataSizeMax>, "GetSaveDataSizeMax"},
        {29, D<&IApplicationFunctions::GetCacheStorageMax>, "GetCacheStorageMax"},
        {30, D<&IApplicationFunctions::BeginBlockingHomeButtonShortAndLongPressed>, "BeginBlockingHomeButtonShortAndLongPressed"},
        {31, D<&IApplicationFunctions::EndBlockingHomeButtonShortAndLongPressed>, "EndBlockingHomeButtonShortAndLongPressed"},
        {32, D<&IApplicationFunctions::BeginBlockingHomeButton>, "BeginBlockingHomeButton"},
        {33, D<&IApplicationFunctions::EndBlockingHomeButton>, "EndBlockingHomeButton"},
        {34, nullptr, "SelectApplicationLicense"},
        {35, nullptr, "GetDeviceSaveDataSizeMax"},
        {36, nullptr, "GetLimitedApplicationLicense"},
        {37, nullptr, "GetLimitedApplicationLicenseUpgradableEvent"},
        {40, D<&IApplicationFunctions::NotifyRunning>, "NotifyRunning"},
        {50, D<&IApplicationFunctions::GetPseudoDeviceId>, "GetPseudoDeviceId"},
        {60, nullptr, "SetMediaPlaybackStateForApplication"},
        {65, D<&IApplicationFunctions::IsGamePlayRecordingSupported>, "IsGamePlayRecordingSupported"},
        {66, D<&IApplicationFunctions::InitializeGamePlayRecording>, "InitializeGamePlayRecording"},
        {67, D<&IApplicationFunctions::SetGamePlayRecordingState>, "SetGamePlayRecordingState"},
        {68, nullptr, "RequestFlushGamePlayingMovieForDebug"},
        {70, nullptr, "RequestToShutdown"},
        {71, nullptr, "RequestToReboot"},
        {72, nullptr, "RequestToSleep"},
        {80, nullptr, "ExitAndRequestToShowThanksMessage"},
        {90, D<&IApplicationFunctions::EnableApplicationCrashReport>, "EnableApplicationCrashReport"},
        {100, D<&IApplicationFunctions::InitializeApplicationCopyrightFrameBuffer>, "InitializeApplicationCopyrightFrameBuffer"},
        {101, D<&IApplicationFunctions::SetApplicationCopyrightImage>, "SetApplicationCopyrightImage"},
        {102, D<&IApplicationFunctions::SetApplicationCopyrightVisibility>, "SetApplicationCopyrightVisibility"},
        {110, D<&IApplicationFunctions::QueryApplicationPlayStatistics>, "QueryApplicationPlayStatistics"},
        {111, D<&IApplicationFunctions::QueryApplicationPlayStatisticsByUid>, "QueryApplicationPlayStatisticsByUid"},
        {112, D<&IApplicationFunctions::Cmd112>, "Cmd112"},
        {113, D<&IApplicationFunctions::Cmd113>, "Cmd113"},
        {120, D<&IApplicationFunctions::ExecuteProgram>, "ExecuteProgram"},
        {121, D<&IApplicationFunctions::ClearUserChannel>, "ClearUserChannel"},
        {122, D<&IApplicationFunctions::UnpopToUserChannel>, "UnpopToUserChannel"},
        {123, D<&IApplicationFunctions::GetPreviousProgramIndex>, "GetPreviousProgramIndex"},
        {124, nullptr, "EnableApplicationAllThreadDumpOnCrash"},
        {130, D<&IApplicationFunctions::GetGpuErrorDetectedSystemEvent>, "GetGpuErrorDetectedSystemEvent"},
        {131, nullptr, "SetDelayTimeToAbortOnGpuError"},
        {140, D<&IApplicationFunctions::GetFriendInvitationStorageChannelEvent>, "GetFriendInvitationStorageChannelEvent"},
        {141, &IApplicationFunctions::TryPopFromFriendInvitationStorageChannel, "TryPopFromFriendInvitationStorageChannel"},
        {150, D<&IApplicationFunctions::GetNotificationStorageChannelEvent>, "GetNotificationStorageChannelEvent"},
        {151, nullptr, "TryPopFromNotificationStorageChannel"},
        {160, D<&IApplicationFunctions::GetHealthWarningDisappearedSystemEvent>, "GetHealthWarningDisappearedSystemEvent"},
        {170, nullptr, "SetHdcpAuthenticationActivated"},
        {180, nullptr, "GetLaunchRequiredVersion"},
        {181, nullptr, "UpgradeLaunchRequiredVersion"},
        {190, nullptr, "SendServerMaintenanceOverlayNotification"},
        {200, nullptr, "GetLastApplicationExitReason"},
        {210, D<&IApplicationFunctions::GetLaunchRequiredVersionUpgrade>, "GetLaunchRequiredVersionUpgrade"},
        {211, nullptr, "GetLaunchRequiredVersionUpgradeStatus"},
        {220, D<&IApplicationFunctions::Cmd220>, "Cmd220"},
        {300, nullptr, "RequestToLaunchApplication"},
        {310, D<&IApplicationFunctions::Cmd310>, "Cmd310"},
        {320, D<&IApplicationFunctions::Cmd320>, "Cmd320"},
        {301, nullptr, "RequestToLaunchApplicationWithUserAndArguments"},
        {310, nullptr, "RequestToLaunchApplicationWithArgumentsAndUserSelectionAndError"},
        {330, D<&IApplicationFunctions::Unknown330>, "Unknown330"}, // [20.2.0+]
        {350, nullptr, "DeclareApplicationAlive"},
        {400, nullptr, "CreateApplicationResourceUsageSystemReportForDebug"},
        {401, nullptr, "WriteApplicationResourceUsageSystemReportForDebug"},
        {410, nullptr, "SetApplicationMemoryReservation"},
        {450, nullptr, "CreateApplicationForDevelop"},
        {460, nullptr, "GetApplicationControlProperty"},
        {461, nullptr, "GetApplicationDesiredLanguage"},
        {470, nullptr, "SetApplicationTerminateResult"},
        {480, nullptr, "GetApplicationRightsOnClient"},
        {490, nullptr, "GetApplicationCertificate"},
        {500, nullptr, "StartContinuousRecordingFlushForDebug"},
        {510, nullptr, "SetApplicationAliveCheckTimeoutEnabled"},
        {600, nullptr, "CreateApplicationAliveChecker"},
        {700, nullptr, "QueryApplicationPlayStatisticsForSystem"},
        {701, nullptr, "QueryApplicationPlayStatisticsByUidForSystem"},
        {702, nullptr, "QueryApplicationPlayStatisticsByPidForSystem"},
        {900, nullptr, "CreateApplicationAttributeUpdater"},
        {997, nullptr, "GetApplicationViewWithPromotionInfo"},
        {998, nullptr, "GetApplicationView"},
        {999, nullptr, "GetApplicationViewDeprecated"},
        {1000, nullptr, "CreateMovieMaker"},
        {1001, D<&IApplicationFunctions::PrepareForJit>, "PrepareForJit"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

IApplicationFunctions::~IApplicationFunctions() = default;

Result IApplicationFunctions::PopLaunchParameter(Out<SharedPointer<IStorage>> out_storage,
                                                 LaunchParameterKind launch_parameter_kind) {
    LOG_INFO(Service_AM, "called, kind={}", launch_parameter_kind);

    std::scoped_lock lk{m_applet->lock};

    auto& channel = launch_parameter_kind == LaunchParameterKind::UserChannel
                        ? m_applet->user_channel_launch_parameter
                        : m_applet->preselected_user_launch_parameter;

    if (channel.empty()) {
        LOG_WARNING(Service_AM, "Attempted to pop parameter {} but none was found!",
                    launch_parameter_kind);
        R_THROW(AM::ResultNoDataInChannel);
    }

    auto data = channel.back();
    channel.pop_back();

    *out_storage = std::make_shared<IStorage>(system, std::move(data));
    R_SUCCEED();
}

Result IApplicationFunctions::EnsureSaveData(Out<u64> out_size, Common::UUID user_id) {
    LOG_INFO(Service_AM, "called, uid={}", user_id.FormattedString());

    FileSys::SaveDataAttribute attribute{};
    attribute.program_id = m_applet->program_id;
    attribute.user_id = user_id.AsU128();
    attribute.type = FileSys::SaveDataType::Account;

    FileSys::VirtualDir save_data{};
    R_TRY(system.GetFileSystemController().OpenSaveDataController()->CreateSaveData(
        &save_data, FileSys::SaveDataSpaceId::User, attribute));

    // [Nextendo] A real console also provisions the title's BCAT delivery-cache storage here,
    // proactively, the first time EnsureSaveData runs -- not just the account save above. Ryujinx
    // never did this at all (upstream bug, not Nextendo-specific) until it hit Splatoon 3, which
    // requests its BCAT cache at startup and got refused with PermissionDenied against
    // never-provisioned storage; the game retried every ~500ms, forever, and never advanced past
    // that point. Splatoon 2 uses BCAT's legacy command path and never touches this, which is why
    // this went unnoticed for years. Matches Ryujinx-Nextendo's own fix (df268d0/e7d39fa,
    // ProcessLoaderHelper's EnsureApplicationBcatDeliveryCacheStorage): gate on the NACP's own
    // declared BcatDeliveryCacheStorageSize, so titles that don't use BCAT are unaffected.
    const FileSys::PatchManager pm{m_applet->program_id, system.GetFileSystemController(),
                                   system.GetContentProvider()};
    const auto metadata = pm.GetControlMetadata();
    if (metadata.first != nullptr && metadata.first->GetBCATDeliveryCacheStorageSize() > 0) {
        FileSys::SaveDataAttribute bcat_attribute{};
        bcat_attribute.program_id = m_applet->program_id;
        bcat_attribute.type = FileSys::SaveDataType::Bcat;

        FileSys::VirtualDir bcat_save_data{};
        const auto bcat_result = system.GetFileSystemController().OpenSaveDataController()->CreateSaveData(
            &bcat_save_data, FileSys::SaveDataSpaceId::User, bcat_attribute);
        if (bcat_result.IsError()) {
            LOG_WARNING(Service_AM, "Failed to provision BCAT delivery-cache storage: {}",
                       bcat_result.raw);
        }
    }

    *out_size = 0;
    R_SUCCEED();
}

Result IApplicationFunctions::GetDesiredLanguage(Out<u64> out_language_code) {
    // FIXME: all of this stuff belongs to ns
    // TODO(bunnei): This should be configurable
    LOG_DEBUG(Service_AM, "called");

    // Get supported languages from NACP, if possible
    // Default to 0 (all languages supported)
    u32 supported_languages = 0;

    const auto res = [this] {
        const FileSys::PatchManager pm{m_applet->program_id, system.GetFileSystemController(),
                                       system.GetContentProvider()};
        auto metadata = pm.GetControlMetadata();
        if (metadata.first != nullptr) {
            return metadata;
        }

        const FileSys::PatchManager pm_update{FileSys::GetUpdateTitleID(m_applet->program_id),
                                              system.GetFileSystemController(),
                                              system.GetContentProvider()};
        return pm_update.GetControlMetadata();
    }();

    if (res.first != nullptr) {
        supported_languages = res.first->GetSupportedLanguages();
    }

    // Call IApplicationManagerInterface implementation.
    auto& service_manager = system.ServiceManager();
    auto ns_am2 = service_manager.GetService<NS::IServiceGetterInterface>("ns:am2");

    std::shared_ptr<NS::IApplicationManagerInterface> app_man;
    R_TRY(ns_am2->GetApplicationManagerInterface(&app_man));

    // Get desired application language
    NS::ApplicationLanguage desired_language{};
    R_TRY(app_man->GetApplicationDesiredLanguage(&desired_language, supported_languages));

    // Convert to settings language code.
    R_TRY(app_man->ConvertApplicationLanguageToLanguageCode(out_language_code, desired_language));

    LOG_DEBUG(Service_AM, "got desired_language={:016X}", *out_language_code);
    R_SUCCEED();
}

Result IApplicationFunctions::SetTerminateResult(Result terminate_result) {
    LOG_INFO(Service_AM, "(STUBBED) called, result={:#x} ({:04}-{:04})",
             terminate_result.GetInnerValue(),
             static_cast<u32>(terminate_result.GetModule()) + 2000,
             terminate_result.GetDescription());

    std::scoped_lock lk{m_applet->lock};
    m_applet->terminate_result = terminate_result;

    R_SUCCEED();
}

Result IApplicationFunctions::GetDisplayVersion(Out<DisplayVersion> out_display_version) {
    LOG_DEBUG(Service_AM, "called");

    const auto res = [this] {
        const FileSys::PatchManager pm{m_applet->program_id, system.GetFileSystemController(),
                                       system.GetContentProvider()};
        auto metadata = pm.GetControlMetadata();
        if (metadata.first != nullptr) {
            return metadata;
        }

        const FileSys::PatchManager pm_update{FileSys::GetUpdateTitleID(m_applet->program_id),
                                              system.GetFileSystemController(),
                                              system.GetContentProvider()};
        return pm_update.GetControlMetadata();
    }();

    if (res.first != nullptr) {
        const auto& version = res.first->GetVersionString();
        std::memcpy(out_display_version->string.data(), version.data(),
                    std::min(version.size(), out_display_version->string.size()));
    } else {
        static constexpr char default_version[]{"1.0.0"};
        std::memcpy(out_display_version->string.data(), default_version, sizeof(default_version));
    }

    out_display_version->string[out_display_version->string.size() - 1] = '\0';
    R_SUCCEED();
}

Result IApplicationFunctions::ExtendSaveData(Out<u64> out_required_size, FileSys::SaveDataType type,
                                             Common::UUID user_id, u64 normal_size,
                                             u64 journal_size) {
    LOG_DEBUG(Service_AM, "called with type={} user_id={} normal={:#x} journal={:#x}",
              static_cast<u8>(type), user_id.FormattedString(), normal_size, journal_size);

    system.GetFileSystemController().OpenSaveDataController()->WriteSaveDataSize(
        type, m_applet->program_id, user_id.AsU128(), {normal_size, journal_size});

    // The following value is used to indicate the amount of space remaining on failure
    // due to running out of space. Since we always succeed, this should be 0.
    *out_required_size = 0;

    R_SUCCEED();
}

Result IApplicationFunctions::GetSaveDataSize(Out<u64> out_normal_size, Out<u64> out_journal_size,
                                              FileSys::SaveDataType type, Common::UUID user_id) {
    LOG_DEBUG(Service_AM, "called with type={} user_id={}", type, user_id.FormattedString());

    const auto size = system.GetFileSystemController().OpenSaveDataController()->ReadSaveDataSize(
        type, m_applet->program_id, user_id.AsU128());

    *out_normal_size = size.normal;
    *out_journal_size = size.journal;
    R_SUCCEED();
}

Result IApplicationFunctions::CreateCacheStorage(Out<u32> out_target_media,
                                                 Out<u64> out_required_size, u16 index,
                                                 u64 normal_size, u64 journal_size) {
    LOG_WARNING(Service_AM, "(STUBBED) called with index={} size={:#x} journal_size={:#x}", index,
                normal_size, journal_size);

    *out_target_media = 1; // Nand
    *out_required_size = 0;

    R_SUCCEED();
}

Result IApplicationFunctions::GetSaveDataSizeMax(Out<u64> out_max_normal_size,
                                                 Out<u64> out_max_journal_size) {
    LOG_WARNING(Service_AM, "(STUBBED) called");

    *out_max_normal_size = 0xFFFFFFF;
    *out_max_journal_size = 0xFFFFFFF;

    R_SUCCEED();
}

Result IApplicationFunctions::GetCacheStorageMax(Out<u32> out_cache_storage_index_max,
                                                 Out<u64> out_max_journal_size) {
    LOG_DEBUG(Service_AM, "called");

    std::vector<u8> nacp;
    R_TRY(system.GetARPManager().GetControlProperty(&nacp, m_applet->program_id));

    auto raw_nacp = std::make_unique<FileSys::RawNACP>();
    std::memcpy(raw_nacp.get(), nacp.data(), std::min(sizeof(*raw_nacp), nacp.size()));

    *out_cache_storage_index_max = static_cast<u32>(raw_nacp->cache_storage_max_index);
    *out_max_journal_size = static_cast<u64>(raw_nacp->cache_storage_data_and_journal_max_size);

    R_SUCCEED();
}

Result IApplicationFunctions::BeginBlockingHomeButtonShortAndLongPressed(s64 unused) {
    LOG_WARNING(Service_AM, "(STUBBED) called");

    std::scoped_lock lk{m_applet->lock};
    m_applet->home_button_long_pressed_blocked = true;
    m_applet->home_button_short_pressed_blocked = true;

    R_SUCCEED();
}

Result IApplicationFunctions::EndBlockingHomeButtonShortAndLongPressed() {
    LOG_WARNING(Service_AM, "(STUBBED) called");

    std::scoped_lock lk{m_applet->lock};
    m_applet->home_button_long_pressed_blocked = false;
    m_applet->home_button_short_pressed_blocked = false;

    R_SUCCEED();
}

Result IApplicationFunctions::BeginBlockingHomeButton(s64 timeout_ns) {
    LOG_WARNING(Service_AM, "(STUBBED) called, timeout_ns={}", timeout_ns);

    std::scoped_lock lk{m_applet->lock};
    m_applet->home_button_long_pressed_blocked = true;
    m_applet->home_button_short_pressed_blocked = true;
    m_applet->home_button_double_click_enabled = true;

    R_SUCCEED();
}

Result IApplicationFunctions::EndBlockingHomeButton() {
    LOG_WARNING(Service_AM, "(STUBBED) called");

    std::scoped_lock lk{m_applet->lock};
    m_applet->home_button_long_pressed_blocked = false;
    m_applet->home_button_short_pressed_blocked = false;
    m_applet->home_button_double_click_enabled = false;

    R_SUCCEED();
}

Result IApplicationFunctions::NotifyRunning(Out<bool> out_became_running) {
    LOG_WARNING(Service_AM, "(STUBBED) called");
    *out_became_running = true;
    R_SUCCEED();
}

Result IApplicationFunctions::GetPseudoDeviceId(Out<Common::UUID> out_pseudo_device_id) {
    LOG_DEBUG(Service_AM, "called");

    // Generate a persistent pseudo device ID for online play and telemetry
    // Based on hardware/system info for consistency across sessions
    // Using MakeRandomWithSeed to ensure deterministic generation
    const u32 device_seed = static_cast<u32>(system.GetApplicationProcessProgramID());
    *out_pseudo_device_id = Common::UUID::MakeRandomWithSeed(device_seed);

    LOG_DEBUG(Service_AM, "Generated PseudoDeviceId: {}",
              out_pseudo_device_id->FormattedString());

    R_SUCCEED();
}

Result IApplicationFunctions::IsGamePlayRecordingSupported(
    Out<bool> out_is_game_play_recording_supported) {
    LOG_WARNING(Service_AM, "(STUBBED) called");
    *out_is_game_play_recording_supported = m_applet->game_play_recording_supported;
    R_SUCCEED();
}

Result IApplicationFunctions::InitializeGamePlayRecording(
    u64 transfer_memory_size, InCopyHandle<Kernel::KTransferMemory> transfer_memory_handle) {
    LOG_WARNING(Service_AM, "(STUBBED) called");
    R_SUCCEED();
}

Result IApplicationFunctions::SetGamePlayRecordingState(
    GamePlayRecordingState game_play_recording_state) {
    LOG_WARNING(Service_AM, "(STUBBED) called");

    std::scoped_lock lk{m_applet->lock};
    m_applet->game_play_recording_state = game_play_recording_state;

    R_SUCCEED();
}

Result IApplicationFunctions::EnableApplicationCrashReport(bool enabled) {
    LOG_WARNING(Service_AM, "(STUBBED) called");

    std::scoped_lock lk{m_applet->lock};
    m_applet->application_crash_report_enabled = enabled;

    R_SUCCEED();
}

Result IApplicationFunctions::InitializeApplicationCopyrightFrameBuffer(
    s32 width, s32 height, u64 transfer_memory_size,
    InCopyHandle<Kernel::KTransferMemory> transfer_memory_handle) {
    LOG_WARNING(Service_AM, "(STUBBED) called");
    R_SUCCEED();
}

Result IApplicationFunctions::SetApplicationCopyrightImage(
    s32 x, s32 y, s32 width, s32 height, WindowOriginMode window_origin_mode,
    InBuffer<BufferAttr_HipcMapTransferAllowsNonSecure | BufferAttr_HipcMapAlias> image_data) {
    LOG_WARNING(Service_AM, "(STUBBED) called");
    R_SUCCEED();
}

Result IApplicationFunctions::SetApplicationCopyrightVisibility(bool visible) {
    LOG_WARNING(Service_AM, "(STUBBED) called, is_visible={}", visible);
    R_SUCCEED();
}

Result IApplicationFunctions::QueryApplicationPlayStatistics(
    Out<s32> out_entries,
    OutArray<ApplicationPlayStatistics, BufferAttr_HipcMapAlias> out_play_statistics,
    InArray<u64, BufferAttr_HipcMapAlias> application_ids) {
    LOG_WARNING(Service_AM, "(STUBBED) called");
    *out_entries = 0;
    R_SUCCEED();
}

Result IApplicationFunctions::QueryApplicationPlayStatisticsByUid(
    Out<s32> out_entries,
    OutArray<ApplicationPlayStatistics, BufferAttr_HipcMapAlias> out_play_statistics,
    Common::UUID user_id, InArray<u64, BufferAttr_HipcMapAlias> application_ids) {
    LOG_WARNING(Service_AM, "(STUBBED) called");
    *out_entries = 0;
    R_SUCCEED();
}

Result IApplicationFunctions::ExecuteProgram(ProgramSpecifyKind kind, u64 value) {
    LOG_WARNING(Service_AM, "(STUBBED) called, kind={}, value={}", kind, value);
    ASSERT(kind == ProgramSpecifyKind::ExecuteProgram ||
           kind == ProgramSpecifyKind::RestartProgram);

    // Copy user channel ownership into the system so that it will be preserved
    system.GetUserChannel() = m_applet->user_channel_launch_parameter;
    system.ExecuteProgram(value);
    R_SUCCEED();
}

Result IApplicationFunctions::ClearUserChannel() {
    LOG_DEBUG(Service_AM, "called");
    m_applet->user_channel_launch_parameter.clear();
    R_SUCCEED();
}

Result IApplicationFunctions::UnpopToUserChannel(SharedPointer<IStorage> storage) {
    LOG_DEBUG(Service_AM, "called");
    m_applet->user_channel_launch_parameter.push_back(storage->GetData());
    R_SUCCEED();
}

Result IApplicationFunctions::GetPreviousProgramIndex(Out<s32> out_previous_program_index) {
    LOG_WARNING(Service_AM, "(STUBBED) called");
    *out_previous_program_index = m_applet->previous_program_index;
    R_SUCCEED();
}

Result IApplicationFunctions::GetGpuErrorDetectedSystemEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_WARNING(Service_AM, "(STUBBED) called");
    *out_event = m_applet->gpu_error_detected_event.GetHandle();
    R_SUCCEED();
}

Result IApplicationFunctions::GetFriendInvitationStorageChannelEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_DEBUG(Service_AM, "called");
    *out_event = m_applet->friend_invitation_storage_channel_event.GetHandle();
    R_SUCCEED();
}

namespace {
// [Nextendo] Same deterministic pid->Uid derivation as friend.cpp's UidForPid (kept local
// rather than shared across a header for one 5-line pure function) -- needed here because real
// hardware's storage-channel payload is NOT a bare byte blob: switchbrew documents the popped
// IStorage as a 0x10-byte sender Uid followed by the actual application-defined payload at
// offset 0x10. We were previously pushing app_param at offset 0 with no Uid header at all --
// if the receiving game validates/reads that leading Uid before trusting the payload that
// follows (as its own PushInData-to-MyPage capture already showed it constructing a real
// version/length-prefixed parameter, i.e. it does structured parsing, not a raw copy), a
// missing header would explain a clean pop with no visible in-game effect afterward.
Common::UUID InvitationUidForPid(u64 pid) {
    std::array<u8, 16> raw{};
    std::memcpy(raw.data(), &pid, sizeof(pid));
    const u64 high = 0x1100000000000000ULL;
    std::memcpy(raw.data() + 8, &high, sizeof(high));
    return Common::UUID{raw};
}
} // namespace

// [Nextendo] RAW ctx handler -- see the header comment for why this is not D<>:
// ctx.GetThread() is the REAL requesting guest thread, while the D<>/cmif path
// executes on a ServerManager host thread whose GetCurrentThreadPointer() is a
// per-thread DUMMY KThread -- arming against that dummy never fires.
void IApplicationFunctions::TryPopFromFriendInvitationStorageChannel(
    HLERequestContext& ctx) {
    // [Nextendo] Real data now: SendFriendInvitation (friend.cpp) relays a sender's invite
    // through nextendo-account's own mailbox; NextendoController polls it in the background
    // (same pattern as the friends-list poll) into Common::NextendoFriends's pending-
    // invitation cache, which this call only ever reads synchronously -- never a network call
    // from an IPC handler. The storage content is the sender's Uid (0x10 bytes, real hardware's
    // documented header) followed by the app_param bytes verbatim, exactly as the sending game
    // constructed them via its own StartSendingFriendInvitation call: this channel relays the
    // payload byte-for-byte, so the receiving game's own parsing of its own format is what
    // actually needs to succeed, not anything decoded on our end -- only the Uid header itself
    // is our own construction, since real hardware's channel always carries one.
    const auto invitation = Common::NextendoFriends::PopPendingInvitation();
    if (!invitation) {
        LOG_DEBUG(Service_AM, "called, no invitation waiting");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(AM::ResultNoDataInChannel);
        return;
    }

    LOG_INFO(Service_AM,
             "[OpenPak] TryPopFromFriendInvitationStorageChannel: from_pid={} "
             "from_name='{}' app_param_size={}",
             invitation->from_pid, invitation->from_name, invitation->app_param.size());

    const auto uid = InvitationUidForPid(invitation->from_pid);
    std::vector<u8> storage_data(sizeof(uid) + invitation->app_param.size());
    std::memcpy(storage_data.data(), &uid, sizeof(uid));
    std::memcpy(storage_data.data() + sizeof(uid), invitation->app_param.data(),
                invitation->app_param.size());

    // [Nextendo] Outbound's own mailbox-consumption path can never join by itself (its
    // JoinSessionRequest is built from a hardcoded empty PlatformSessionID and
    // NetworkSystemSwitch.JoinSession is a no-op on this platform -- both confirmed by
    // full static disassembly, see outbound-re NOTES). A delivered invitation is
    // otherwise consumed and silently discarded. Drive the game's own real, working
    // join-by-code path (NetworkManager.Join + StartCoroutine) with the room code from
    // this invitation's application parameter instead. See nextendo_guest_call.h.
    // ... and arm the guest-call injection against ctx.GetThread() -- the REAL
    // requesting guest thread (Unity main).
    const auto& app_param = invitation->app_param;
    if (app_param.size() >= 6 && app_param.size() == 5 + app_param[4]) {
        const char* code_bytes = reinterpret_cast<const char*>(app_param.data()) + 5;
        const std::string_view room_code(code_bytes, app_param[4]);
        LOG_INFO(Service_AM, "[OpenPak] invitation room code: '{}'", room_code);
        // [Nextendo] RECORD only: the injection must fire when the USER accepts the
        // invite (toast click) from the multiplayer menu -- auto-firing on delivery
        // joins from whatever screen the joiner is on and crashes on the main menu
        // (observed live: guest svcBreak + freeze).
        Core::NextendoGuestCall::RecordInvite(ctx.GetThread(), *system.ApplicationProcess(),
                                              room_code);
    } else {
        LOG_INFO(Service_AM,
                 "[OpenPak] invitation app_param does not match the u32_le(1) || u8(len) || "
                 "ascii format (size={}) -- no join armed",
                 app_param.size());
    }

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    auto storage = std::make_shared<IStorage>(system, std::move(storage_data));
    // Match the cmif path: domain sessions register domain objects, non-domain
    // sessions register the HLE interface. Getting this wrong corrupts the session
    // (observed live: hle_ipc "object_id too big" assert + guest svcBreak + freeze).
    if (ctx.GetManager()->IsDomain()) {
        ctx.AddDomainObject(std::move(storage));
    } else {
        ctx.AddMoveInterface(std::move(storage));
    }
}

Result IApplicationFunctions::GetNotificationStorageChannelEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_DEBUG(Service_AM, "called");
    *out_event = m_applet->notification_storage_channel_event.GetHandle();
    R_SUCCEED();
}

Result IApplicationFunctions::GetHealthWarningDisappearedSystemEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_DEBUG(Service_AM, "called");
    *out_event = m_applet->health_warning_disappeared_system_event.GetHandle();
    R_SUCCEED();
}

Result IApplicationFunctions::PrepareForJit() {
    LOG_WARNING(Service_AM, "(STUBBED) called");

    std::scoped_lock lk{m_applet->lock};
    m_applet->jit_service_launched = true;

    R_SUCCEED();
}

Result IApplicationFunctions::GetLaunchRequiredVersionUpgrade(OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_WARNING(Service_AM, "(STUBBED) called");

    // TODO(ZEP): Add a dedicated launch_required_version_upgrade_event when implemented
    *out_event = m_applet->state_changed_event.GetHandle();

    R_SUCCEED();
}

Result IApplicationFunctions::Unknown330() {
    LOG_WARNING(Service_AM, "(STUBBED) called Unknown330 [20.2.0+]");
    R_SUCCEED();
}

Result IApplicationFunctions::Cmd112() {
    LOG_WARNING(Service_AM, "(STUBBED) called [20.0.0+]");
    R_SUCCEED();
}

Result IApplicationFunctions::Cmd113() {
    LOG_WARNING(Service_AM, "(STUBBED) called [20.0.0+]");
    R_SUCCEED();
}

Result IApplicationFunctions::Cmd220() {
    LOG_WARNING(Service_AM, "(STUBBED) called [20.0.0+]");
    R_SUCCEED();
}

Result IApplicationFunctions::Cmd310() {
    LOG_WARNING(Service_AM, "(STUBBED) called [20.0.0+]");
    R_SUCCEED();
}

Result IApplicationFunctions::Cmd320() {
    LOG_WARNING(Service_AM, "(STUBBED) called [20.0.0+]");
    R_SUCCEED();
}

} // namespace Service::AM
