// RegionChunkStorage — region-paged chunk store behind IChunkStorage.
//
// FALTANTES §4 item 1 (region files / paged backend): instead of persisting
// one monolithic world blob, a save is a DIRECTORY whose pages are per-region
// files plus a "world" manifest page. The world (facade) owns the page
// encoding; this backend only stores opaque bytes, page id -> file, with
// atomic (temp + rename) writes that also replace existing files on Windows.
//
// Pure std/filesystem — the headless equivalent of a region-file container
// (each region tile = regionSize x regionSize chunks) with no third-party
// backend. The RocksDB store remains the content-addressed blob backend.
#include "engine/storage/IChunkStoreFactory.hpp"
#include "engine/voxel/IVoxelServices.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace engine {
namespace storage {
namespace {

namespace fs = std::filesystem;

class RegionChunkStorage final : public engine::voxel::IChunkStorage {
public:
    // Journal directory (WAL, FALTANTES §4 item 8): <root>/wal/ holds the
    // undo entries of the save IN PROGRESS. Its presence during load means
    // that save was interrupted; recovery rolls it back.
    static constexpr const char* kWalDir = "wal";

    explicit RegionChunkStorage(int regionSize) { (void)regionSize; }

    // ---- IChunkStorage (coarse surface: opens/commits the page directory) ----

    std::string serialize_world(std::string& errorOut) override {
        errorOut = "region store: use the paged surface (save_world + save_page)";
        return {};
    }

    bool deserialize_world(const std::string& data, std::string& errorOut) override {
        errorOut = "region store: use the paged surface (load_world + load_page)";
        return false;
    }

    bool save_world(const std::string& filePath, std::string& errorOut) override {
        root_ = filePath;
        std::error_code ec;
        fs::create_directories(root_, ec);
        if (ec) {
            errorOut = "region store: cannot create '" + filePath + "': " + ec.message();
            return false;
        }
        // NO wipe: the world writes DELTA pages (FALTANTES §4 item 3) — only
        // tiles with changed chunks are rewritten, so unchanged pages from the
        // previous save must survive. Orphaned pages (a different world saved
        // into this dir, or tiles whose chunks were evicted) are inert: the
        // manifest lists exactly the current tiles, and load only reads those.
        //
        // WAL (FALTANTES §4 item 8): a save begins by dropping any journal
        // left by an INTERRUPTED earlier save. This save's save_page calls
        // journal the undo data they are about to overwrite; if this save is
        // interrupted (process death, power loss) the journal survives, and
        // the NEXT load_world rolls every partially-written page back to the
        // state it had when this save began. On success the world calls
        // commit_save(), which removes the journal so recovery is a no-op.
        clear_journal();
        return true;
    }

    bool load_world(const std::string& filePath, std::string& errorOut) override {
        root_ = filePath;
        std::error_code ec;
        if (!fs::is_directory(root_, ec)) {
            errorOut = "region store: no paged world at '" + filePath + "'";
            return false;
        }
        // WAL recovery (FALTANTES §4 item 8): a journal means a previous save
        // was interrupted mid-write. Roll every journaled page back to the
        // bytes it had when that save began, drop pages that did not exist
        // then, clean stale .tmp* artifacts, and remove the journal. After
        // recovery the directory is exactly the last COMMITTED save — never a
        // mix of old and new pages. A committed save (no journal) is untouched.
        if (fs::exists(fs::path(root_) / kWalDir, ec)) {
            recover_journal(errorOut);
        }
        // FALTANTES §4 item 20: a stray .tmp* partial file (a process killed
        // during temp+rename AFTER the journal was committed, or any orphan)
        // is inert but must not linger — sweep it even without a journal.
        sweep_stale_tmp();
        return true;
    }

    // ---- Paged surface (FALTANTES §4 item 1) ----

    bool supports_regions() const override { return true; }

    bool save_page(const std::string& pageId, const std::string& payload,
                   std::string& errorOut) override {
        if (root_.empty()) {
            errorOut = "region store: save_world(path) must precede save_page";
            return false;
        }
        const fs::path target = fs::path(root_) / page_id_to_file(pageId);
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            errorOut = "region store: cannot create page dir: " + ec.message();
            return false;
        }
        // WAL (FALTANTES §4 item 8): BEFORE overwriting, journal the undo
        // data — the current bytes of the page being replaced, or a tombstone
        // for a page that does not exist yet. Written atomically (temp +
        // rename), so a crash here leaves either no journal entry (page not
        // yet touched -> nothing to roll back) or a complete one. Recovery on
        // the next load_world restores these bytes, making the interrupted
        // save disappear.
        if (!journal_undo(pageId, errorOut)) return false;
        // Atomic replace: temp + rename (the checksum-verified page is fully
        // written before the swap — same contract as the monolithic save).
        return atomic_write(target, payload, errorOut);
    }

    bool load_page(const std::string& pageId, std::string& payloadOut,
                   std::string& errorOut) override {
        if (root_.empty()) {
            errorOut = "region store: load_world(path) must precede load_page";
            return false;
        }
        const fs::path file = fs::path(root_) / page_id_to_file(pageId);
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) {
            errorOut = "region store: page '" + pageId + "' not found";
            return false;
        }
        std::ifstream in(file, std::ios::binary);
        payloadOut.assign(std::istreambuf_iterator<char>(in),
                          std::istreambuf_iterator<char>());
        return true;
    }

    bool commit_save(std::string& errorOut) override {
        clear_journal();
        errorOut.clear();
        return true;
    }

    // ---- WAL helpers (FALTANTES §4 item 8) ----

    // Journals the undo data for a page about to be overwritten: the current
    // file bytes under <root>/wal/<file>.bak, or a .missing tombstone when the
    // page does not exist yet. Both written atomically (temp + rename) so a
    // crash mid-journal never leaves a torn entry.
    bool journal_undo(const std::string& pageId, std::string& errorOut) {
        // The journal entry uses the LEAF file name (no "pages/" prefix): the
        // journal lives in <root>/wal/, and recovery walks it non-recursively
        // to restore pages/<leaf>. A page id that would escape "pages" is
        // already sanitized by page_id_to_file.
        const std::string safeName =
            fs::path(page_id_to_file(pageId)).filename().string();
        const fs::path pagesDir = fs::path(root_) / "pages";
        const fs::path target = pagesDir / safeName;
        const fs::path walDir = fs::path(root_) / kWalDir;
        std::error_code ec;
        fs::create_directories(walDir, ec);
        if (ec) {
            errorOut = "region store: cannot create journal dir: " + ec.message();
            return false;
        }
        if (fs::is_regular_file(target, ec)) {
            std::ifstream in(target, std::ios::binary);
            const std::string oldBytes((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
            const fs::path bak = walDir / (safeName + ".bak");
            if (!atomic_write(bak, oldBytes, errorOut)) return false;
        } else {
            // Page never existed before this save: a tombstone tells recovery
            // to REMOVE the page (so a page created by an interrupted save
            // does not survive as a stale artifact).
            const fs::path marker = walDir / (safeName + ".missing");
            if (!atomic_write(marker, "", errorOut)) return false;
        }
        return true;
    }

    // Atomic temp + rename write (shared by pages and journal entries).
    static bool atomic_write(const fs::path& target, const std::string& bytes,
                             std::string& errorOut) {
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        const fs::path temp = fs::path(target).string() + ".tmp" +
                              std::to_string(process_id_());
        {
            std::ofstream out(temp, std::ios::binary);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        std::error_code renameEc;
        fs::rename(temp, target, renameEc);
        if (renameEc) {
            std::error_code removeEc;
            fs::remove_all(target, removeEc);
            (void)removeEc;
            std::error_code retryEc;
            fs::rename(temp, target, retryEc);
            if (retryEc) {
                std::error_code cleanupEc;
                fs::remove_all(temp, cleanupEc);
                (void)cleanupEc;
                errorOut = "region store: cannot write '" + target.string() +
                           "': " + retryEc.message();
                return false;
            }
        }
        return true;
    }

    // Removes the journal directory. Called at save begin (a new save
    // supersedes any interrupted one) and on commit (the save is permanent).
    void clear_journal() const {
        std::error_code ec;
        fs::remove_all(fs::path(root_) / kWalDir, ec);
        (void)ec;
    }

    // Rolls an interrupted save back to the last committed state: for every
    // journaled .bak, restore the old bytes; for every .missing tombstone,
    // delete the page; then drop stale .tmp* artifacts and the journal dir.
    // Failures are reported but recovery keeps going so as many pages as
    // possible return to the committed state.
    void recover_journal(std::string& errorOut) {
        const fs::path walDir = fs::path(root_) / kWalDir;
        const fs::path pagesDir = fs::path(root_) / "pages";
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(walDir, ec)) {
            if (!entry.is_regular_file()) continue;
            const std::string fileName = entry.path().filename().string();
            if (fileName.size() > 4 &&
                fileName.compare(fileName.size() - 4, 4, ".bak") == 0) {
                // Restore the pre-save bytes of the page.
                std::ifstream in(entry.path(), std::ios::binary);
                const std::string oldBytes((std::istreambuf_iterator<char>(in)),
                                           std::istreambuf_iterator<char>());
                const fs::path page =
                    pagesDir / fileName.substr(0, fileName.size() - 4);
                if (!atomic_write(page, oldBytes, errorOut)) {
                    // Keep going; report the first failure.
                }
            } else if (fileName.size() > 8 &&
                       fileName.compare(fileName.size() - 8, 8, ".missing") == 0) {
                const fs::path page =
                    pagesDir / fileName.substr(0, fileName.size() - 8);
                std::error_code removeEc;
                fs::remove_all(page, removeEc);
                (void)removeEc;
            }
        }
        // Drop stale .tmp* artifacts from a crashed temp+rename.
        sweep_stale_tmp();
        clear_journal();
    }

    // Drops stale .tmp* artifacts from a crashed temp+rename. A partial temp
    // file is never a page (page_ids() skips it) and is inert — sweeping keeps
    // the save directory clean. Called on every load, with or without a journal.
    void sweep_stale_tmp() const {
        const fs::path pagesDir = fs::path(root_) / "pages";
        std::error_code ec;
        if (!fs::is_directory(pagesDir, ec)) return;
        for (const auto& entry : fs::directory_iterator(pagesDir, ec)) {
            const std::string fileName = entry.path().filename().string();
            if (fileName.find(".tmp") != std::string::npos) {
                std::error_code removeEc;
                fs::remove_all(entry.path(), removeEc);
                (void)removeEc;
            }
        }
    }

    std::vector<std::string> page_ids() const override {
        std::vector<std::string> ids;
        if (root_.empty()) return ids;
        // Pages live under <root>/pages/ (page_id_to_file); the top level only
        // holds that one subdirectory, so iterate pages/ and list .dat files.
        const fs::path pagesDir = fs::path(root_) / "pages";
        std::error_code ec;
        if (!fs::is_directory(pagesDir, ec)) return ids;
        for (const auto& entry : fs::directory_iterator(pagesDir, ec)) {
            if (!entry.is_regular_file()) continue;
            const std::string fileName = entry.path().filename().string();
            const std::string suffix = ".dat";
            if (fileName.size() <= suffix.size() ||
                fileName.compare(fileName.size() - suffix.size(), suffix.size(),
                                 suffix) != 0) {
                continue;  // stale .tmp* from a crashed run is not a page
            }
            ids.push_back(file_to_page_id(fileName));
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    }

private:
    // Sanitizes a page id into a safe file name ("r.1.2" -> "pages/r_1_2.dat";
    // the manifest id "world" -> "pages/world.dat"). Keeps page ids out of the
    // path namespace so a page id can never escape the root.
    static std::string page_id_to_file(const std::string& pageId) {
        std::string safe = pageId;
        for (char& c : safe) {
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) {
                c = '_';
            }
        }
        return "pages/" + safe + ".dat";
    }

    static std::string file_to_page_id(const std::string& fileName) {
        const std::string prefix = "pages/";
        (void)prefix;  // fileName is already the leaf name
        // "r_1_2.dat" -> "r.1.2"; "world.dat" -> "world".
        std::string leaf = fileName;
        const std::string suffix = ".dat";
        if (leaf.size() > suffix.size() &&
            leaf.compare(leaf.size() - suffix.size(), suffix.size(), suffix) == 0) {
            leaf = leaf.substr(0, leaf.size() - suffix.size());
        }
        for (char& c : leaf) if (c == '_') c = '.';
        return leaf;
    }

    static int process_id_() {
        static const int pid = static_cast<int>(
#ifdef _WIN32
            _getpid()
#else
            getpid()
#endif
        );
        return pid;
    }

    std::string root_;
};

}  // namespace

std::shared_ptr<engine::voxel::IChunkStorage> create_region_chunk_storage(
    int regionSize) {
    return std::make_shared<RegionChunkStorage>(
        regionSize > 0 ? regionSize : 8);
}

}  // namespace storage
}  // namespace engine
