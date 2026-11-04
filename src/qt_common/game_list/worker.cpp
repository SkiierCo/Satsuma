// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QDir>
#include <QDirIterator>
#include <QDateTime>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>

#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/fs_filesystem.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/nup_writer.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/loose_updates.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/submission_package.h"
#include "core/loader/loader.h"

#include "qt_common/config/uisettings.h"
#include "qt_common/qt_common.h"

#include "qt_common/game_list/game_list_p.h"

#include "qt_common/game_list/model.h"
#include "qt_common/game_list/worker.h"

namespace {

QString GetGameListCachedObject(const std::string& filename, const std::string& ext,
                                const std::function<QString()>& generator) {
    if (!UISettings::values.cache_game_list || filename == "0000000000000000") {
        return generator();
    }

    const auto path =
        Common::FS::PathToUTF8String(Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) /
                                     "game_list" / fmt::format("{}.{}", filename, ext));

    void(Common::FS::CreateParentDirs(path));

    if (!Common::FS::Exists(path)) {
        const auto str = generator();

        QFile file{QString::fromStdString(path)};
        if (file.open(QFile::WriteOnly)) {
            file.write(str.toUtf8());
        }

        return str;
    }

    QFile file{QString::fromStdString(path)};
    if (file.open(QFile::ReadOnly)) {
        return QString::fromUtf8(file.readAll());
    }

    return generator();
}

std::pair<std::vector<u8>, std::string> GetGameListCachedObject(
    const std::string& filename, const std::string& ext,
    const std::function<std::pair<std::vector<u8>, std::string>()>& generator) {
    if (!UISettings::values.cache_game_list || filename == "0000000000000000") {
        return generator();
    }

    const auto game_list_dir =
        Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "game_list";
    const auto jpeg_name = fmt::format("{}.jpeg", filename);
    const auto app_name = fmt::format("{}.appname.txt", filename);

    const auto path1 = Common::FS::PathToUTF8String(game_list_dir / jpeg_name);
    const auto path2 = Common::FS::PathToUTF8String(game_list_dir / app_name);

    void(Common::FS::CreateParentDirs(path1));

    if (!Common::FS::Exists(path1) || !Common::FS::Exists(path2)) {
        const auto [icon, nacp] = generator();

        QFile file1{QString::fromStdString(path1)};
        if (!file1.open(QFile::WriteOnly)) {
            LOG_ERROR(Frontend, "Failed to open cache file.");
            return generator();
        }

        if (!file1.resize(icon.size())) {
            LOG_ERROR(Frontend, "Failed to resize cache file to necessary size.");
            return generator();
        }

        if (file1.write(reinterpret_cast<const char*>(icon.data()), icon.size()) !=
            s64(icon.size())) {
            LOG_ERROR(Frontend, "Failed to write data to cache file.");
            return generator();
        }

        QFile file2{QString::fromStdString(path2)};
        if (file2.open(QFile::WriteOnly)) {
            file2.write(nacp.data(), nacp.size());
        }

        return std::make_pair(icon, nacp);
    }

    QFile file1(QString::fromStdString(path1));
    QFile file2(QString::fromStdString(path2));

    if (!file1.open(QFile::ReadOnly)) {
        LOG_ERROR(Frontend, "Failed to open cache file for reading.");
        return generator();
    }

    if (!file2.open(QFile::ReadOnly)) {
        LOG_ERROR(Frontend, "Failed to open cache file for reading.");
        return generator();
    }

    std::vector<u8> vec(file1.size());
    if (file1.read(reinterpret_cast<char*>(vec.data()), vec.size()) !=
        static_cast<s64>(vec.size())) {
        return generator();
    }

    const auto data = file2.readAll();
    return std::make_pair(vec, data.toStdString());
}

void GetMetadataFromControlNCA(const FileSys::PatchManager& patch_manager, const FileSys::NCA& nca,
                               std::vector<u8>& icon, std::string& name) {
    std::tie(icon, name) = GetGameListCachedObject(
        fmt::format("{:016X}", patch_manager.GetTitleID()), {}, [&patch_manager, &nca] {
            const auto [nacp, icon_f] = patch_manager.ParseControlNCA(nca);
            return std::make_pair(icon_f->ReadAllBytes(), nacp->GetApplicationName());
        });
}

bool HasSupportedFileExtension(const std::string& file_name) {
    const QFileInfo file = QFileInfo(QString::fromStdString(file_name));
    return QtCommon::supported_file_extensions.contains(file.suffix(), Qt::CaseInsensitive);
}

bool IsExtractedNCAMain(const std::string& file_name) {
    return QFileInfo(QString::fromStdString(file_name)).fileName() == QStringLiteral("main");
}

QString FormatGameName(const std::string& physical_name) {
    const QString physical_name_as_qstring = QString::fromStdString(physical_name);
    const QFileInfo file_info(physical_name_as_qstring);

    if (IsExtractedNCAMain(physical_name)) {
        return file_info.dir().path();
    }

    return physical_name_as_qstring;
}

// Loose ExeFS dumps (<game>/exefs/main, <game>/base/exefs/main, updates in
// <game>/updates/<title id>/exefs/main) have no control data, so they are named after their
// game folder.
QString LooseExeFSName(const std::string& physical_name) {
    QDir dir = QFileInfo(QString::fromStdString(physical_name)).dir();
    if (dir.dirName().compare(QStringLiteral("exefs"), Qt::CaseInsensitive) != 0 || !dir.cdUp()) {
        return {};
    }

    // Storage levels sit between the game folder and its content directories.
    const auto is_storage_level = [](const QString& name) {
        if (name.compare(QStringLiteral("base"), Qt::CaseInsensitive) == 0 ||
            name.compare(QStringLiteral("update"), Qt::CaseInsensitive) == 0 ||
            name.compare(QStringLiteral("updates"), Qt::CaseInsensitive) == 0) {
            return true;
        }
        return !name.isEmpty() &&
               std::all_of(name.cbegin(), name.cend(), [](QChar c) {
                   return c.isDigit() || (c >= QLatin1Char('a') && c <= QLatin1Char('f')) ||
                          (c >= QLatin1Char('A') && c <= QLatin1Char('F'));
               });
    };

    bool is_update = false;
    while (is_storage_level(dir.dirName())) {
        if (dir.dirName().compare(QStringLiteral("update"), Qt::CaseInsensitive) == 0 ||
            dir.dirName().compare(QStringLiteral("updates"), Qt::CaseInsensitive) == 0) {
            is_update = true;
        }
        if (!dir.cdUp()) {
            return {};
        }
    }

    const QString name = dir.dirName();
    if (name.isEmpty()) {
        return {};
    }
    if (is_update && !name.contains(QStringLiteral("update"), Qt::CaseInsensitive)) {
        return QObject::tr("%1 (update)").arg(name);
    }
    return name;
}

// A loose dump's game list entry is its small main executable, so the size column would only
// show a few dozen MB. Sum the dump's real contents instead: the ExeFS directory plus the
// RomFS directory next to it, matching the loader's loose RomFS discovery.
u64 LooseDumpSize(const std::filesystem::path& main_path) {
    const auto exefs_dir = main_path.parent_path();

    u64 total{};
    std::error_code ec;
    const auto accumulate = [&total, &ec](const std::filesystem::path& dir) {
        for (std::filesystem::recursive_directory_iterator it{
                 dir, std::filesystem::directory_options::skip_permission_denied, ec};
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            std::error_code entry_ec;
            if (it->is_regular_file(entry_ec)) {
                const auto size = it->file_size(entry_ec);
                if (!entry_ec) {
                    total += size;
                }
            }
        }
    };

    accumulate(exefs_dir);
    const auto romfs_dir = exefs_dir.parent_path() / "romfs";
    if (std::filesystem::is_directory(romfs_dir, ec)) {
        accumulate(romfs_dir);
    }
    return total;
}

QString EntryCachePath(const std::string& candidate) {
    const auto key = QCryptographicHash::hash(QByteArray::fromStdString(candidate),
                                             QCryptographicHash::Sha256).toHex();
    return QString::fromStdString(Common::FS::PathToUTF8String(
        Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "game_list" / "entries-v1")) +
        QLatin1Char('/') + QString::fromLatin1(key) + QStringLiteral(".json");
}

QByteArray EntryFingerprint(const std::filesystem::path& candidate) {
    QStringList stamps;
    const auto stamp = [&stamps](const QFileInfo& info) {
        stamps.append(info.absoluteFilePath() + QLatin1Char('|') +
                      QString::number(info.size()) + QLatin1Char('|') +
                      QString::number(info.lastModified().toMSecsSinceEpoch()));
    };
    stamp(QFileInfo(QString::fromStdString(Common::FS::PathToUTF8String(candidate))));
    if (const auto root = FileSys::Nup::DumpRootFromMain(candidate)) {
        const auto qroot = QString::fromStdString(Common::FS::PathToUTF8String(*root));
        stamp(QFileInfo(qroot));
        QDirIterator it(qroot, QDir::AllEntries | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            stamp(it.fileInfo());
        }
    }
    stamps.sort();
    stamps.append(QString::number(static_cast<int>(Settings::values.language_index.GetValue())));
    for (const auto& [id, disabled] : Settings::values.disabled_addons) {
        stamps.append(QString::number(id));
        for (const auto& addon : disabled) {
            stamps.append(QString::fromStdString(addon));
        }
    }
    return QCryptographicHash::hash(stamps.join(QLatin1Char('\0')).toUtf8(),
                                     QCryptographicHash::Sha256).toHex();
}

QJsonArray ReadEntryCache(const std::string& candidate, const QByteArray& fingerprint) {
    QFile cache(EntryCachePath(candidate));
    if (!cache.open(QIODevice::ReadOnly) || cache.size() > 32 * 1024 * 1024) {
        return {};
    }
    const auto object = QJsonDocument::fromJson(cache.readAll()).object();
    if (object.value(QStringLiteral("fingerprint")).toString().toLatin1() != fingerprint) {
        return {};
    }
    const auto entries = object.value(QStringLiteral("entries")).toArray();
    if (entries.isEmpty() || entries.size() > 256) {
        return {};
    }
    for (const auto& value : entries) {
        const auto entry = value.toObject();
        if (!QFileInfo::exists(entry.value(QStringLiteral("path")).toString()) ||
            !entry.contains(QStringLiteral("type")) || !entry.contains(QStringLiteral("size"))) {
            return {};
        }
    }
    return entries;
}

void WriteEntryCache(const std::string& candidate, const QByteArray& fingerprint,
                     const QJsonArray& entries) {
    const auto path = EntryCachePath(candidate);
    if (entries.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath())) {
        return;
    }
    QSaveFile cache(path);
    if (!cache.open(QIODevice::WriteOnly)) {
        return;
    }
    const QJsonObject object{{QStringLiteral("fingerprint"), QString::fromLatin1(fingerprint)},
                              {QStringLiteral("entries"), entries}};
    if (cache.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) >= 0) {
        cache.commit();
    }
}

QList<QStandardItem*> CachedGameListEntry(const QJsonObject& entry,
                                          const PlayTime::PlayTimeManager& play_time_manager) {
    const auto path = entry.value(QStringLiteral("path")).toString().toStdString();
    const auto icon = QByteArray::fromBase64(entry.value(QStringLiteral("icon")).toString().toLatin1());
    const auto id = entry.value(QStringLiteral("id")).toString().toULongLong();
    const auto time = play_time_manager.GetPlayTime(id);
    const auto type = entry.value(QStringLiteral("type")).toString();
    const auto patches = entry.value(QStringLiteral("patches")).toString();
    const std::vector<u8> picture(icon.begin(), icon.end());
    return {new GameListItemPath(FormatGameName(path), picture,
                                entry.value(QStringLiteral("title")).toString(), type, id, time,
                                patches),
            new GameListItem(type),
            new GameListItemSize(entry.value(QStringLiteral("size")).toString().toULongLong()),
            new GameListItemPlayTime(time), new GameListItem(patches)};
}

QString FormatPatchNameVersions(const FileSys::PatchManager& patch_manager,
                                Loader::AppLoader& loader, bool updatable = true) {
    QString out;
    FileSys::VirtualFile update_raw;
    loader.ReadUpdateRaw(update_raw);
    for (const auto& patch : patch_manager.GetPatches(update_raw)) {
        const bool is_update = patch.name == "Update";
        if (!updatable && is_update) {
            continue;
        }

        const QString type =
            QString::fromStdString(patch.enabled ? patch.name : "[D] " + patch.name);

        if (patch.version.empty()) {
            out.append(QStringLiteral("%1\n").arg(type));
        } else {
            auto ver = patch.version;

            // Display container name for packed updates
            if (is_update && ver == "PACKED") {
                ver = Loader::GetFileTypeString(loader.GetFileType());
            }

            out.append(QStringLiteral("%1 (%2)\n").arg(type, QString::fromStdString(ver)));
        }
    }

    out.chop(1);
    return out;
}

QList<QStandardItem*> MakeGameListEntry(const std::string& path, const std::string& name,
                                        const std::size_t size, const std::vector<u8>& icon,
                                        Loader::AppLoader& loader, u64 program_id,
                                        const PlayTime::PlayTimeManager& play_time_manager,
                                        const FileSys::PatchManager& patch) {
    auto const file_type = loader.GetFileType();
    auto const file_type_string = QString::fromStdString(Loader::GetFileTypeString(file_type));

    const auto format_patches = [&patch, &loader] {
        return FormatPatchNameVersions(patch, loader, loader.IsRomFSUpdatable());
    };
    // Loose versions are discovered from neighboring directories at each library refresh.
    // A title-only cache would retain an obsolete update after folders were added or removed.
    const QString patch_versions = file_type == Loader::FileType::DeconstructedRomDirectory
        ? format_patches()
        : GetGameListCachedObject(fmt::format("{:016X}", patch.GetTitleID()), "pv.txt", format_patches);

    u64 play_time = play_time_manager.GetPlayTime(program_id);
    return QList<QStandardItem*>{
        new GameListItemPath(FormatGameName(path), icon, QString::fromStdString(name),
                             file_type_string, program_id, play_time, patch_versions),
        new GameListItem(file_type_string),
        new GameListItemSize(size),
        new GameListItemPlayTime(play_time),
        new GameListItem(patch_versions),
    };
}
} // Anonymous namespace

GameListWorker::GameListWorker(FileSys::VirtualFilesystem vfs_,
                               FileSys::ManualContentProvider* provider_,
                               QVector<UISettings::GameDir>& game_dirs_,
                               const PlayTime::PlayTimeManager& play_time_manager_,
                               Core::System& system_)
    : vfs{std::move(vfs_)}, provider{provider_}, game_dirs{game_dirs_},
      play_time_manager{play_time_manager_},
      system{system_} {
    // We want the game list to manage our lifetime.
    setAutoDelete(false);
}

GameListWorker::~GameListWorker() {
    this->disconnect();
    Stop();
}

void GameListWorker::Stop() {
    if (!stop_requested.exchange(true)) {
        processing_completed.Wait();
    }
}

void GameListWorker::ProcessEvents(GameListModel* model) {
    // An event may spin a nested event loop that replaces this worker, in which case stop.
    while (model->current_worker.get() == this) {
        std::function<void(GameListModel*)> func;
        {
            // Lock queue to protect concurrent modification.
            std::scoped_lock lk(lock);

            // If we can't pop a function, return.
            if (queued_events.empty()) {
                return;
            }

            // Pop a function.
            func = std::move(queued_events.back());
            queued_events.pop_back();
        }

        // Run the function.
        func(model);
    }
}

template <typename F>
void GameListWorker::RecordEvent(F&& func) {
    {
        // Lock queue to protect concurrent modification.
        std::scoped_lock lk(lock);

        // Add the function into the front of the queue.
        queued_events.emplace_front(std::move(func));
    }

    // Data now available.
    emit DataAvailable();
}

void GameListWorker::AddTitlesToGameList(GameListDir* parent_dir) {
    using namespace FileSys;

    const auto& cache = system.GetContentProviderUnion();

    auto installed_games = cache.ListEntriesFilterOrigin(std::nullopt, TitleType::Application,
                                                         ContentRecordType::Program);

    if (parent_dir->type() == static_cast<int>(GameListItemType::SdmcDir)) {
        installed_games = cache.ListEntriesFilterOrigin(
            ContentProviderUnionSlot::SDMC, TitleType::Application, ContentRecordType::Program);
    } else if (parent_dir->type() == static_cast<int>(GameListItemType::UserNandDir)) {
        installed_games = cache.ListEntriesFilterOrigin(
            ContentProviderUnionSlot::UserNAND, TitleType::Application, ContentRecordType::Program);
    } else if (parent_dir->type() == static_cast<int>(GameListItemType::SysNandDir)) {
        installed_games = cache.ListEntriesFilterOrigin(
            ContentProviderUnionSlot::SysNAND, TitleType::Application, ContentRecordType::Program);
    }

    for (const auto& [slot, game] : installed_games) {
        if (slot == ContentProviderUnionSlot::FrontendManual) {
            continue;
        }

        const auto file = cache.GetEntryUnparsed(game.title_id, game.type);
        std::unique_ptr<Loader::AppLoader> loader = Loader::GetLoader(system, file);
        if (!loader) {
            continue;
        }

        std::vector<u8> icon;
        std::string name;
        u64 program_id = 0;
        const auto result = loader->ReadProgramId(program_id);

        if (result != Loader::ResultStatus::Success) {
            continue;
        }

        const PatchManager patch{program_id, system.GetFileSystemController(),
                                 system.GetContentProvider()};
        LOG_INFO(Frontend, "PatchManager initiated for id {:X}", program_id);
        const auto control = cache.GetEntry(game.title_id, ContentRecordType::Control);
        if (control != nullptr) {
            GetMetadataFromControlNCA(patch, *control, icon, name);
        }

        auto entry = MakeGameListEntry(file->GetFullPath(), name, file->GetSize(), icon, *loader,
                                       program_id, play_time_manager, patch);
        RecordEvent([=](GameListModel* model) { model->AddEntry(entry, parent_dir); });
    }
}

void GameListWorker::ScanFileSystem(ScanTarget target, const std::string& dir_path, bool deep_scan,
                                    GameListDir* parent_dir) {
    const auto process_candidate = [this, target,
                                    parent_dir](const std::filesystem::path& candidate_path) {
        auto path = candidate_path;
        auto physical_name = Common::FS::PathToUTF8String(path);
        const auto candidate_name = physical_name;
        const auto extension = QString::fromStdString(
            Common::FS::PathToUTF8String(path.extension())).toLower();
        if (target == ScanTarget::FillManualContentProvider &&
            extension != QStringLiteral(".nca") &&
            !(Settings::values.ext_content_from_game_dirs.GetValue() &&
              (extension == QStringLiteral(".nsp") || extension == QStringLiteral(".nsz") ||
               extension == QStringLiteral(".xci") || extension == QStringLiteral(".xcz")))) {
            return;
        }
        const bool use_cache = target == ScanTarget::PopulateGameList &&
                               UISettings::values.cache_game_list;
        const auto fingerprint = use_cache ? EntryFingerprint(path) : QByteArray{};
        if (use_cache) {
            const auto cached = ReadEntryCache(candidate_name, fingerprint);
            if (!cached.isEmpty()) {
                for (const auto& value : cached) {
                    const auto object = value.toObject();
                    const auto cached_path = object.value(QStringLiteral("path")).toString().toStdString();
                    if (object.value(QStringLiteral("loose")).toBool() &&
                        !loose_entries.insert(cached_path).second) {
                        continue;
                    }
                    const auto entry = CachedGameListEntry(object, play_time_manager);
                    RecordEvent([=](GameListModel* model) { model->AddEntry(entry, parent_dir); });
                }
                return;
            }
        }
        auto file = vfs->OpenFile(physical_name, FileSys::OpenMode::Read);
        if (!file) {
            return;
        }

        auto loader = Loader::GetLoader(system, file);
        if (!loader) {
            return;
        }

        const auto file_type = loader->GetFileType();
        if (file_type == Loader::FileType::Unknown || file_type == Loader::FileType::Error) {
            return;
        }

        if (target == ScanTarget::PopulateGameList &&
            (file_type == Loader::FileType::XCI || file_type == Loader::FileType::XCZ ||
             file_type == Loader::FileType::NSP || file_type == Loader::FileType::NSZ ||
             file_type == Loader::FileType::NUP) &&
            !Loader::IsBootableGameContainer(file, file_type)) {
            return;
        }

        u64 program_id = 0;
        const auto res2 = loader->ReadProgramId(program_id);

        if (target == ScanTarget::PopulateGameList &&
            file_type == Loader::FileType::DeconstructedRomDirectory &&
            res2 == Loader::ResultStatus::Success && program_id != 0) {
            const auto loose = FileSys::DiscoverLooseProgram(file->GetContainingDirectory(), program_id);
            const auto main = loose.base_exefs ? loose.base_exefs->GetFile("main") : nullptr;
            if (main) {
                // Redirect any version to the same canonical entry, even when only an
                // updates folder was configured as a library location.
                physical_name = main->GetFullPath();
                path = std::filesystem::u8path(physical_name);
                if (!loose_entries.insert(physical_name).second) {
                    return;
                }
                file = main;
                loader = Loader::GetLoader(system, file);
                if (!loader) {
                    return;
                }
            }
        }

        if (target == ScanTarget::FillManualContentProvider) {
            if (res2 == Loader::ResultStatus::Success && file_type == Loader::FileType::NCA) {
                provider->AddEntry(FileSys::TitleType::Application,
                                   FileSys::GetCRTypeFromNCAType(FileSys::NCA{file}.GetType()),
                                   program_id, file);
            } else if (Settings::values.ext_content_from_game_dirs.GetValue() &&
                       (file_type == Loader::FileType::XCI ||
                        file_type == Loader::FileType::XCZ ||
                        file_type == Loader::FileType::NSP ||
                        file_type == Loader::FileType::NSZ)) {
                void(provider->AddEntriesFromContainer(file));
            }
        } else {
            std::vector<u64> program_ids;
            loader->ReadProgramIds(program_ids);
            QJsonArray cache_entries;

            const auto addEntry = [this, &path, &cache_entries, physical_name, file,
                                   parent_dir](std::unique_ptr<Loader::AppLoader>& app_loader,
                                               const u64 id) {
                std::vector<u8> icon;
                [[maybe_unused]] const auto res1 = app_loader->ReadIcon(icon);

                std::string name = " ";
                const auto res3 = app_loader->ReadTitle(name);
                if ((res3 != Loader::ResultStatus::Success || name.empty() || name == " ") &&
                    IsExtractedNCAMain(physical_name)) {
                    if (const QString loose = LooseExeFSName(physical_name); !loose.isEmpty()) {
                        name = loose.toStdString();
                    }
                }

                const FileSys::PatchManager patch{id, system.GetFileSystemController(),
                                                  system.GetContentProvider(),
                                                  app_loader->GetFileType() == Loader::FileType::DeconstructedRomDirectory
                                                      ? file->GetContainingDirectory() : nullptr};

                const u64 size = IsExtractedNCAMain(physical_name)
                                     ? LooseDumpSize(path)
                                     : Common::FS::GetSize(physical_name);

                auto entry = MakeGameListEntry(physical_name, name, size, icon, *app_loader, id,
                                               play_time_manager, patch);

                cache_entries.append(QJsonObject{
                    {QStringLiteral("path"), QString::fromStdString(physical_name)},
                    {QStringLiteral("title"), QString::fromStdString(name)},
                    {QStringLiteral("icon"), QString::fromLatin1(QByteArray(
                        reinterpret_cast<const char*>(icon.data()), static_cast<int>(icon.size())).toBase64())},
                    {QStringLiteral("id"), QString::number(id)},
                    {QStringLiteral("size"), QString::number(size)},
                    {QStringLiteral("type"), entry[1]->text()},
                    {QStringLiteral("patches"), entry[4]->text()},
                    {QStringLiteral("loose"), app_loader->GetFileType() ==
                        Loader::FileType::DeconstructedRomDirectory}});

                RecordEvent([=](GameListModel* model) { model->AddEntry(entry, parent_dir); });
            };

            if (res2 == Loader::ResultStatus::Success && program_ids.size() > 1 &&
                (file_type == Loader::FileType::XCI || file_type == Loader::FileType::XCZ ||
                 file_type == Loader::FileType::NSP || file_type == Loader::FileType::NSZ)) {
                for (const auto id : program_ids) {
                    // dravee suggested this, only viable way to
                    // not show sub-games in qlaunch for now.
                    if ((id & 0xFFF) != 0) {
                        continue;
                    }
                    loader = Loader::GetLoader(system, file, id);
                    if (!loader) {
                        continue;
                    }

                    addEntry(loader, id);
                }
            } else {
                addEntry(loader, program_id);
            }
            if (use_cache && !stop_requested) {
                WriteEntryCache(candidate_name, fingerprint, cache_entries);
            }
        }
    };

    const auto callback = [this, target, deep_scan,
                           &process_candidate](const std::filesystem::path& path) -> bool {
        if (stop_requested) {
            // Breaks the callback loop.
            return false;
        }

        if (Common::FS::IsDir(path)) {
            watch_list.append(QString::fromStdString(Common::FS::PathToUTF8String(path)));

            // Probe only known loose dump layouts, including numeric version roots.
            if (!deep_scan && target == ScanTarget::PopulateGameList) {
                const auto directory = vfs->OpenDirectory(
                    Common::FS::PathToUTF8String(path), FileSys::OpenMode::Read);
                for (const auto& main : FileSys::ListLooseProgramMains(directory)) {
                    watch_list.append(QString::fromStdString(
                        main->GetContainingDirectory()->GetFullPath()));
                    process_candidate(std::filesystem::u8path(main->GetFullPath()));
                }

                // A game folder may hold its NUP container beside its icon and title sidecars.
                std::error_code ec;
                for (const auto& entry : std::filesystem::directory_iterator(
                         path, std::filesystem::directory_options::skip_permission_denied, ec)) {
                    std::error_code entry_ec;
                    if (!entry.is_regular_file(entry_ec)) {
                        continue;
                    }
                    const auto extension = Common::FS::PathToUTF8String(entry.path().extension());
                    if (QString::fromStdString(extension).compare(QStringLiteral(".nup"),
                                                                  Qt::CaseInsensitive) == 0) {
                        process_candidate(entry.path());
                    }
                }
            }

            return true;
        }

        const auto physical_name = Common::FS::PathToUTF8String(path);
        if (HasSupportedFileExtension(physical_name) || IsExtractedNCAMain(physical_name)) {
            process_candidate(path);
        }

        return true;
    };

    if (deep_scan) {
        Common::FS::IterateDirEntriesRecursively(dir_path, callback,
                                                 Common::FS::DirEntryFilter::All);
    } else {
        Common::FS::IterateDirEntries(dir_path, callback, Common::FS::DirEntryFilter::All);
    }
}

void GameListWorker::run() {
    watch_list.clear();
    loose_entries.clear();
    provider->ClearAllEntries();

    const auto DirEntryReady = [&](GameListDir* game_list_dir) {
        RecordEvent([=](GameListModel* model) { model->AddDirEntry(game_list_dir); });
    };

    for (UISettings::GameDir& game_dir : game_dirs) {
        if (stop_requested) {
            break;
        }

        GameListDir* game_list_dir;
        bool scan = false;

        if (game_dir.path == std::string("SDMC")) {
            game_list_dir = new GameListDir(game_dir, GameListItemType::SdmcDir);
        } else if (game_dir.path == std::string("UserNAND")) {
            game_list_dir = new GameListDir(game_dir, GameListItemType::UserNandDir);
        } else if (game_dir.path == std::string("SysNAND")) {
            game_list_dir = new GameListDir(game_dir, GameListItemType::SysNandDir);
        } else {
            const QString qpath = QString::fromStdString(game_dir.path);
            if (QDir(qpath).exists()) {
                watch_list.append(qpath);
            }

            game_list_dir = new GameListDir(game_dir);
            scan = true;
        }

        DirEntryReady(game_list_dir);
        if (scan) {
            ScanFileSystem(ScanTarget::FillManualContentProvider, game_dir.path, game_dir.deep_scan,
                           game_list_dir);
            ScanFileSystem(ScanTarget::PopulateGameList, game_dir.path, game_dir.deep_scan,
                           game_list_dir);
        } else {
            AddTitlesToGameList(game_list_dir);
        }
    }

    RecordEvent([this](GameListModel* model) { model->DonePopulating(watch_list); });
    processing_completed.Set();
}
