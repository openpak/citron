// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cinttypes>
#include <cstring>
#include <exception>
#include <vector>

#include "common/common_funcs.h"
#include "common/hex_util.h"
#include "common/logging.h"
#include "common/lz4_compression.h"
#include "common/settings.h"
#include "common/swap.h"
#include "core/core.h"
#include "core/file_sys/patch_manager.h"
#include "core/hle/kernel/code_set.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/loader/nextendo_s3_patches.h"
#include "core/loader/nso.h"
#include "core/memory.h"

#ifdef HAS_NCE
#include "core/arm/nce/patcher.h"
#endif

namespace Loader {
namespace {
struct MODHeader {
    u32_le magic;
    u32_le dynamic_offset;
    u32_le bss_start_offset;
    u32_le bss_end_offset;
    u32_le eh_frame_hdr_start_offset;
    u32_le eh_frame_hdr_end_offset;
    u32_le module_offset; // Offset to runtime-generated module object. typically equal to .bss base
};
static_assert(sizeof(MODHeader) == 0x1c, "MODHeader has incorrect size.");

std::vector<u8> DecompressSegment(const std::vector<u8>& compressed_data,
                                  const NSOSegmentHeader& header) {
    std::vector<u8> uncompressed_data =
        Common::Compression::DecompressDataLZ4(compressed_data, header.size);

    ASSERT_MSG(uncompressed_data.size() == header.size, "{} != {}", header.size,
               uncompressed_data.size());

    return uncompressed_data;
}

constexpr u32 PageAlignSizeNSO(u32 size) {
    return static_cast<u32>((size + Core::Memory::CITRON_PAGEMASK) & ~Core::Memory::CITRON_PAGEMASK);
}
} // Anonymous namespace

bool NSOHeader::IsSegmentCompressed(size_t segment_num) const {
    ASSERT_MSG(segment_num < 3, "Invalid segment {}", segment_num);
    return ((flags >> segment_num) & 1) != 0;
}

AppLoader_NSO::AppLoader_NSO(FileSys::VirtualFile file_) : AppLoader(std::move(file_)) {}

FileType AppLoader_NSO::IdentifyType(const FileSys::VirtualFile& in_file) {
    u32 magic = 0;
    if (in_file->ReadObject(&magic) != sizeof(magic)) {
        return FileType::Error;
    }

    if (Common::MakeMagic('N', 'S', 'O', '0') != magic) {
        return FileType::Error;
    }

    return FileType::NSO;
}

std::optional<VAddr> AppLoader_NSO::LoadModule(Kernel::KProcess& process, Core::System& system,
                                               const FileSys::VfsFile& nso_file, VAddr load_base,
                                               bool should_pass_arguments, bool load_into_process,
                                               std::optional<FileSys::PatchManager> pm,
                                               std::vector<Core::NCE::Patcher>* patches,
                                               s32 patch_index) {
    if (nso_file.GetSize() < sizeof(NSOHeader)) {
        return std::nullopt;
    }

    NSOHeader nso_header{};
    if (sizeof(NSOHeader) != nso_file.ReadObject(&nso_header)) {
        return std::nullopt;
    }

    if (nso_header.magic != Common::MakeMagic('N', 'S', 'O', '0')) {
        return std::nullopt;
    }

    // Allocate some space at the beginning if we are patching in PreText mode.
    const size_t module_start = [&]() -> size_t {
#ifdef HAS_NCE
        if (patches && load_into_process) {
            auto* patch = &patches->operator[](patch_index);
            if (patch->GetPatchMode() == Core::NCE::PatchMode::PreText) {
                return patch->GetSectionSize();
            }
        }
#endif
        return 0;
    }();

    // Build program image
    Kernel::CodeSet codeset;
    Kernel::PhysicalMemory program_image;
    for (std::size_t i = 0; i < nso_header.segments.size(); ++i) {
        std::vector<u8> data = nso_file.ReadBytes(nso_header.segments_compressed_size[i],
                                                  nso_header.segments[i].offset);
        if (nso_header.IsSegmentCompressed(i)) {
            data = DecompressSegment(data, nso_header.segments[i]);
        }
        program_image.resize(module_start + nso_header.segments[i].location +
                             static_cast<u32>(data.size()));
        std::memcpy(program_image.data() + module_start + nso_header.segments[i].location,
                    data.data(), data.size());
        codeset.segments[i].addr = module_start + nso_header.segments[i].location;
        codeset.segments[i].offset = module_start + nso_header.segments[i].location;
        codeset.segments[i].size = nso_header.segments[i].size;
    }

    if (should_pass_arguments && !Settings::values.program_args.GetValue().empty()) {
        const auto arg_data{Settings::values.program_args.GetValue()};

        codeset.DataSegment().size += NSO_ARGUMENT_DATA_ALLOCATION_SIZE;
        NSOArgumentHeader args_header{
            NSO_ARGUMENT_DATA_ALLOCATION_SIZE, static_cast<u32_le>(arg_data.size()), {}};
        const auto end_offset = program_image.size();
        program_image.resize(static_cast<u32>(program_image.size()) +
                             NSO_ARGUMENT_DATA_ALLOCATION_SIZE);
        std::memcpy(program_image.data() + end_offset, &args_header, sizeof(NSOArgumentHeader));
        std::memcpy(program_image.data() + end_offset + sizeof(NSOArgumentHeader), arg_data.data(),
                    arg_data.size());
    }

    codeset.DataSegment().size += nso_header.segments[2].bss_size;
    u32 image_size{
        PageAlignSizeNSO(static_cast<u32>(program_image.size()) + nso_header.segments[2].bss_size)};
    program_image.resize(image_size);

    for (std::size_t i = 0; i < nso_header.segments.size(); ++i) {
        codeset.segments[i].size = PageAlignSizeNSO(codeset.segments[i].size);
    }

    // Apply patches if necessary
    const auto name = nso_file.GetName();
    const bool has_nso_patch = pm && pm->HasNSOPatch(nso_header.build_id, name);
    const bool should_patch_nso =
        has_nso_patch || (load_into_process && Settings::values.dump_nso);
    if (pm && should_patch_nso) {
        std::span<u8> patchable_section(program_image.data() + module_start,
                                        program_image.size() - module_start);
        std::vector<u8> pi_header(sizeof(NSOHeader) + patchable_section.size());
        std::memcpy(pi_header.data(), &nso_header, sizeof(NSOHeader));
        std::memcpy(pi_header.data() + sizeof(NSOHeader), patchable_section.data(),
                    patchable_section.size());

        pi_header = pm->PatchNSO(pi_header, name);

        if (pi_header.size() < sizeof(NSOHeader)) {
            LOG_ERROR(Loader, "Patched NSO is too small: module={}, patched_size={:#x}", name,
                      pi_header.size());
            return std::nullopt;
        }

        const auto patched_size = pi_header.size() - sizeof(NSOHeader);
        if (patched_size != patchable_section.size()) {
            LOG_ERROR(Loader,
                      "Patched NSO size mismatch: module={}, original_size={:#x}, "
                      "patched_size={:#x}",
                      name, patchable_section.size(), patched_size);
            return std::nullopt;
        }

        std::copy(pi_header.begin() + sizeof(NSOHeader), pi_header.end(), patchable_section.data());
    }

    // [Nextendo] Stardew Valley 1.6.15.13 / update 0.20.0 clean-room interoperability patch.
    // The game's userspace OpenSSL stack accepts Nintendo's CA but rejects Nextendo's replacement
    // CA before it emits TLS Finished. Offline analysis of this exact build identified
    // X509_verify_cert at 0x79B4C10. Scope the bypass to the title, module, build ID, and expected
    // original prologue so another revision can never be patched accidentally.
    if (pm && pm->GetTitleID() == 0x0100E65002BB8000ULL && name == "main") {
        constexpr std::string_view stardew_build =
            "E7F845093E8CBC68DACF011CCB620D6667B5A20B";
        constexpr size_t verify_offset = 0x79B4C10;
        constexpr std::array<u8, 8> expected{{0xFE, 0x57, 0xBE, 0xA9,
                                              0xF4, 0x4F, 0x01, 0xA9}};
        // mov w0, #1; ret
        constexpr std::array<u8, 8> replacement{{0x20, 0x00, 0x80, 0x52,
                                                 0xC0, 0x03, 0x5F, 0xD6}};
        // The SDK's SSL-context setup reads a never-set certificate-acceptance flag and selects
        // between an always-accept verify callback (flag set) and a real-check callback that only
        // tolerates expiry-class errors (flag clear). With the real-check callback installed the
        // handshake is followed by a client-side gRPC UNAVAILABLE cancel before any HTTP/2
        // HEADERS (nn::Result 2321-4992) -- the same symptom Splatoon 3 shows without its
        // certificate-bypass patch. Force the flag read to 1 (identical to the S3 fix).
        constexpr size_t accept_flag_offset = 0x782F5D0;
        constexpr std::array<u8, 4> flag_expected{{0xAA, 0xE2, 0x40, 0x39}}; // ldrb w10,[x21,#0x38]
        constexpr std::array<u8, 4> flag_replacement{{0x2A, 0x00, 0x80, 0x52}}; // mov w10, #1
        const auto build_raw = Common::HexToString(nso_header.build_id);
        const auto build = build_raw.substr(0, build_raw.find_last_not_of('0') + 1);
        std::span<u8> image(program_image.data() + module_start,
                            program_image.size() - module_start);
        if (build != stardew_build) {
            LOG_ERROR(Loader,
                      "[OpenPak] Stardew: unsupported main build {}; certificate patch skipped",
                      build);
        } else if (verify_offset + expected.size() > image.size() ||
                   !std::equal(expected.begin(), expected.end(), image.begin() + verify_offset)) {
            LOG_ERROR(Loader,
                      "[OpenPak] Stardew: X509 verification prologue mismatch; certificate "
                      "patch skipped");
        } else {
            std::copy(replacement.begin(), replacement.end(), image.begin() + verify_offset);
            LOG_INFO(Loader,
                     "[OpenPak] Stardew: build-scoped X509 certificate compatibility patch "
                     "applied");
        }
        if (build != stardew_build) {
            // Build mismatch already logged above; nothing further to do.
        } else if (accept_flag_offset + flag_expected.size() > image.size() ||
                   !std::equal(flag_expected.begin(), flag_expected.end(),
                               image.begin() + accept_flag_offset)) {
            LOG_ERROR(Loader,
                      "[OpenPak] Stardew: certificate-acceptance flag read mismatch; "
                      "pin-bypass patch skipped");
        } else {
            std::copy(flag_replacement.begin(), flag_replacement.end(),
                      image.begin() + accept_flag_offset);
            LOG_INFO(Loader,
                     "[OpenPak] Stardew: build-scoped certificate-acceptance flag bypass "
                     "applied");
        }
    }

    // [Nextendo] Splatoon 3's built-in patches (certificate-pinning bypass, peer hostname fix)
    // run unconditionally here, independent of should_patch_nso above: they must apply even when
    // no mod patches exist, and must never go through the mod-patch path at all, since Splatoon 3
    // refuses to boot with any mod enabled (see main.cpp) and these patches need to survive that
    // ban rather than be blocked by it.
    if (pm && pm->GetTitleID() == 0x0100C2500FC20000ULL) {
        std::span<u8> patchable_section(program_image.data() + module_start,
                                        program_image.size() - module_start);
        std::vector<u8> pi_header(sizeof(NSOHeader) + patchable_section.size());
        std::memcpy(pi_header.data(), &nso_header, sizeof(NSOHeader));
        std::memcpy(pi_header.data() + sizeof(NSOHeader), patchable_section.data(),
                    patchable_section.size());

        pi_header = Loader::NextendoS3Patches::ApplyIfMatch(nso_header.build_id,
                                                            std::move(pi_header), name);

        if (pi_header.size() >= sizeof(NSOHeader) &&
            pi_header.size() - sizeof(NSOHeader) == patchable_section.size()) {
            std::copy(pi_header.begin() + sizeof(NSOHeader), pi_header.end(),
                      patchable_section.data());
        } else {
            LOG_ERROR(Loader, "[OpenPak] Splatoon 3 built-in patch changed the image size "
                              "unexpectedly; skipped");
        }
    }

#ifdef HAS_NCE
    // If we are computing the process code layout and using nce backend, patch.
    const auto& code = codeset.CodeSegment();
    auto* patch = patches ? &patches->operator[](patch_index) : nullptr;
    if (patch && !load_into_process) {
        // Patch SVCs and MRS calls in the guest code
        while (!patch->PatchText(program_image, code)) {
            patch = &patches->emplace_back();
        }
    } else if (patch) {
        // Relocate code patch and copy to the program_image.
        const auto build_id = Common::HexToString(nso_header.build_id);
        LOG_DEBUG(Loader,
                  "NCE relocating NSO module: name={}, build_id={}, patch_index={}, mode={}, "
                  "load_base={:#x}, image_size={:#x}, code_offset={:#x}, code_size={:#x}, "
                  "patch_section_size={:#x}",
                  name, build_id, patch_index, patch->GetPatchMode(), load_base,
                  program_image.size(), code.offset, code.size, patch->GetSectionSize());

        bool copied_patch_section;
        try {
            copied_patch_section =
                patch->RelocateAndCopy(load_base, code, program_image, &process.GetPostHandlers());
        } catch (const std::exception& ex) {
            LOG_CRITICAL(Loader,
                         "NCE failed while relocating NSO module: name={}, build_id={}, "
                         "patch_index={}, mode={}, load_base={:#x}, image_size={:#x}, "
                         "code_offset={:#x}, code_size={:#x}, patch_section_size={:#x}, "
                         "exception={}",
                         name, build_id, patch_index, patch->GetPatchMode(), load_base,
                         program_image.size(), code.offset, code.size, patch->GetSectionSize(),
                         ex.what());
            throw;
        }

        LOG_DEBUG(Loader,
                  "NCE relocated NSO module: name={}, build_id={}, patch_index={}, "
                  "copied_patch_section={}, final_image_size={:#x}",
                  name, build_id, patch_index, copied_patch_section, program_image.size());

        if (copied_patch_section) {
            // Update patch section.
            auto& patch_segment = codeset.PatchSegment();
            patch_segment.addr =
                patch->GetPatchMode() == Core::NCE::PatchMode::PreText ? 0 : image_size;
            patch_segment.size = static_cast<u32>(patch->GetSectionSize());
        }

        // Refresh image_size to take account the patch section if it was added by RelocateAndCopy
        image_size = static_cast<u32>(program_image.size());
    }
#endif

    // If we aren't actually loading (i.e. just computing the process code layout), we are done
    if (!load_into_process) {
        return load_base + image_size;
    }

    // Apply cheats if they exist and the program has a valid title ID
    if (pm) {
        system.SetApplicationProcessBuildID(nso_header.build_id);
        const auto cheats = pm->CreateCheatList(nso_header.build_id);
        if (!cheats.empty()) {
            system.RegisterCheatList(cheats, nso_header.build_id, load_base, image_size);
        }
    }

    // Load codeset for current process
    codeset.memory = std::move(program_image);
    process.LoadModule(std::move(codeset), load_base);

    return load_base + image_size;
}

AppLoader_NSO::LoadResult AppLoader_NSO::Load(Kernel::KProcess& process, Core::System& system) {
    if (is_loaded) {
        return {ResultStatus::ErrorAlreadyLoaded, {}};
    }

    modules.clear();

    // Load module
    const VAddr base_address = GetInteger(process.GetEntryPoint());
    if (!LoadModule(process, system, *file, base_address, true, true)) {
        return {ResultStatus::ErrorLoadingNSO, {}};
    }

    modules.insert_or_assign(base_address, file->GetName());
    LOG_DEBUG(Loader, "loaded module {} @ 0x{:X}", file->GetName(), base_address);

    is_loaded = true;
    return {ResultStatus::Success, LoadParameters{Kernel::KThread::DefaultThreadPriority,
                                                  Core::Memory::DEFAULT_STACK_SIZE}};
}

ResultStatus AppLoader_NSO::ReadNSOModules(Modules& out_modules) {
    out_modules = this->modules;
    return ResultStatus::Success;
}

} // namespace Loader
