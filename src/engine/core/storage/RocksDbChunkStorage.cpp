// RocksDbChunkStorage — RocksDB-backed chunk store behind IChunkStorage.
//
// Promoted solution (META section 32 / DEPENDENCY_POLICY "chunks/event log"):
// world-save blobs persist in a real embedded key-value store, content
// addressed by BLAKE3. This is the ONLY translation unit that includes the
// rocksdb headers; consumers only see engine/storage/IChunkStoreFactory.hpp.
#include "engine/storage/IChunkStoreFactory.hpp"
#include "engine/voxel/IVoxelServices.hpp"
#include "engine/hashing/IHashProvider.hpp"

#include <rocksdb/db.h>

#include <memory>
#include <string>
#include <utility>

namespace engine {
namespace storage {
namespace {

const char* kLatestKey = "world:latest";
const char* kBlobPrefix = "blob:";

class RocksDbChunkStorage final : public engine::voxel::IChunkStorage {
public:
    ~RocksDbChunkStorage() override { close(); }

    // ---- IChunkStorage ----

    std::string serialize_world(std::string& errorOut) override {
        // The latest blob is cached from deserialize_world() or load_world().
        // Content addressing makes the returned bytes self-verifying.
        if (latestBlob_.empty()) {
            errorOut = "rocksdb store: no world blob cached (deserialize or "
                       "load first)";
            return {};
        }
        errorOut.clear();
        return latestBlob_;
    }

    bool deserialize_world(const std::string& data, std::string& errorOut) override {
        // Cache the blob and, if the DB is open, persist it immediately.
        latestBlob_ = data;
        if (db_ && !data.empty()) {
            if (!store_blob(data, errorOut)) return false;
        }
        errorOut.clear();
        return true;
    }

    bool save_world(const std::string& filePath, std::string& errorOut) override {
        if (latestBlob_.empty()) {
            errorOut = "rocksdb store: nothing to save (deserialize a world "
                       "blob first)";
            return false;
        }
        if (!open(filePath, errorOut)) return false;
        return store_blob(latestBlob_, errorOut);
    }

    bool load_world(const std::string& filePath, std::string& errorOut) override {
        if (!open(filePath, errorOut)) return false;
        if (!db_) {
            errorOut = "rocksdb store: database not open";
            return false;
        }
        std::string digest;
        const rocksdb::Status latestStatus =
            db_->Get(rocksdb::ReadOptions(), kLatestKey, &digest);
        if (latestStatus.IsNotFound()) {
            errorOut = "rocksdb store: no world saved at this database";
            return false;
        }
        if (!latestStatus.ok()) {
            errorOut = "rocksdb store: read 'world:latest' failed: " +
                       latestStatus.ToString();
            return false;
        }
        std::string blob;
        const std::string blobKey = std::string(kBlobPrefix) + digest;
        const rocksdb::Status blobStatus =
            db_->Get(rocksdb::ReadOptions(), blobKey, &blob);
        if (blobStatus.IsNotFound()) {
            errorOut = "rocksdb store: content-addressed blob missing (key " +
                       blobKey + ")";
            return false;
        }
        if (!blobStatus.ok()) {
            errorOut = "rocksdb store: read blob failed: " + blobStatus.ToString();
            return false;
        }
        // Content addressing is self-verifying: the blob must hash to the key.
        if (hash_->hash_hex(blob) != digest) {
            errorOut = "rocksdb store: blob corrupted (BLAKE3 mismatch)";
            return false;
        }
        latestBlob_ = blob;
        errorOut.clear();
        return true;
    }

private:
    void close() { db_.reset(); }

    bool open(const std::string& filePath, std::string& errorOut) {
        if (db_) {
            // Same database already open.
            if (openPath_ == filePath) return true;
            close();
        }
        rocksdb::Options options;
        options.create_if_missing = true;
        std::unique_ptr<rocksdb::DB> db;
        const rocksdb::Status status =
            rocksdb::DB::Open(options, filePath, &db);
        if (!status.ok()) {
            errorOut = "rocksdb store: cannot open database at " + filePath +
                       ": " + status.ToString();
            return false;
        }
        db_ = std::move(db);
        openPath_ = filePath;
        return true;
    }

    bool store_blob(const std::string& blob, std::string& errorOut) {
        if (!db_) {
            errorOut = "rocksdb store: database not open";
            return false;
        }
        const std::string digest = hash_->hash_hex(blob);
        rocksdb::WriteOptions writeOptions;
        const rocksdb::Status blobStatus = db_->Put(
            writeOptions, std::string(kBlobPrefix) + digest, blob);
        if (!blobStatus.ok()) {
            errorOut = "rocksdb store: put blob failed: " + blobStatus.ToString();
            return false;
        }
        const rocksdb::Status latestStatus =
            db_->Put(writeOptions, kLatestKey, digest);
        if (!latestStatus.ok()) {
            errorOut = "rocksdb store: put 'world:latest' failed: " +
                       latestStatus.ToString();
            return false;
        }
        return true;
    }

    std::shared_ptr<engine::hashing::IHashProvider> hash_ =
        engine::hashing::create_blake3_hash_provider();
    std::unique_ptr<rocksdb::DB> db_;
    std::string openPath_;
    std::string latestBlob_;
};

}  // namespace

std::shared_ptr<engine::voxel::IChunkStorage> create_rocksdb_chunk_storage() {
    return std::make_shared<RocksDbChunkStorage>();
}

}  // namespace storage
}  // namespace engine
