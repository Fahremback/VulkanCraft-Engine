#include "VoxelStructure.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <stdexcept>

namespace Engine::Voxel {
namespace {
constexpr std::string_view SourceMagic = "VCVOXEL_SOURCE";
constexpr std::array<char, 8> CookedMagic{'V','C','V','O','X','E','L','\0'};
constexpr uint32_t FormatVersion = 1;
constexpr uint64_t MaxVoxelCount = 256ull * 1024ull * 1024ull;
constexpr uint32_t MaxCollectionCount = 1u << 20;
constexpr uint32_t MaxStringBytes = 1u << 24;

bool valid_size(Int3 size, uint64_t& count) {
    if (size.x <= 0 || size.y <= 0 || size.z <= 0) return false;
    count = static_cast<uint64_t>(size.x) * static_cast<uint64_t>(size.y) * static_cast<uint64_t>(size.z);
    return count <= MaxVoxelCount && count <= std::numeric_limits<size_t>::max();
}
void set_error(std::string* error, std::string value) { if (error) *error = std::move(value); }

uint64_t hash_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    uint64_t hash = 1469598103934665603ull;
    char byte{};
    while (input.get(byte)) {
        hash ^= static_cast<uint8_t>(byte);
        hash *= 1099511628211ull;
    }
    return hash;
}

template<class T> bool write_scalar(std::ostream& output, const T& value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
    return static_cast<bool>(output);
}
template<class T> bool read_scalar(std::istream& input, T& value) {
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    return static_cast<bool>(input);
}
bool write_string(std::ostream& output, std::string_view value) {
    if (value.size() > MaxStringBytes) return false;
    const auto length = static_cast<uint32_t>(value.size());
    return write_scalar(output, length) &&
           static_cast<bool>(output.write(value.data(), static_cast<std::streamsize>(value.size())));
}
bool read_string(std::istream& input, std::string& value) {
    uint32_t length{};
    if (!read_scalar(input, length) || length > MaxStringBytes) return false;
    value.resize(length);
    return length == 0 || static_cast<bool>(input.read(value.data(), length));
}
bool write_int3(std::ostream& output, Int3 value) {
    return write_scalar(output, value.x) && write_scalar(output, value.y) && write_scalar(output, value.z);
}
bool read_int3(std::istream& input, Int3& value) {
    return read_scalar(input, value.x) && read_scalar(input, value.y) && read_scalar(input, value.z);
}
bool write_voxel(std::ostream& output, VoxelValue value) {
    return write_scalar(output, value.type) && write_scalar(output, value.material) && write_scalar(output, value.density);
}
bool read_voxel(std::istream& input, VoxelValue& value) {
    return read_scalar(input, value.type) && read_scalar(input, value.material) && read_scalar(input, value.density);
}

struct VoxelRun { uint32_t length{}; VoxelValue value; };
std::vector<VoxelRun> make_runs(const std::vector<VoxelValue>& voxels) {
    std::vector<VoxelRun> runs;
    for (const VoxelValue voxel : voxels) {
        if (!runs.empty() && runs.back().value == voxel && runs.back().length != std::numeric_limits<uint32_t>::max())
            ++runs.back().length;
        else
            runs.push_back({1, voxel});
    }
    return runs;
}

bool ensure_parent(const std::filesystem::path& path, std::string* error) {
    if (!path.has_parent_path()) return true;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (!ec) return true;
    set_error(error, "Cannot create voxel asset directory: " + ec.message());
    return false;
}
} // namespace

VoxelStructure::VoxelStructure(Int3 size, std::string name) : size_(size), name_(std::move(name)) {
    uint64_t count{};
    if (!valid_size(size, count)) throw std::invalid_argument("VoxelStructure dimensions are invalid or too large");
    voxels_.resize(static_cast<size_t>(count));
}

void VoxelStructure::set_pivot(Int3 pivot) {
    if (!contains(pivot)) throw std::out_of_range("VoxelStructure pivot is outside the asset");
    pivot_ = pivot;
}

bool VoxelStructure::contains(Int3 p) const noexcept {
    return p.x >= 0 && p.y >= 0 && p.z >= 0 && p.x < size_.x && p.y < size_.y && p.z < size_.z;
}

size_t VoxelStructure::linear_index(Int3 p) const {
    if (!contains(p)) throw std::out_of_range("Voxel position is outside the structure");
    return (static_cast<size_t>(p.z) * static_cast<size_t>(size_.y) + static_cast<size_t>(p.y)) *
           static_cast<size_t>(size_.x) + static_cast<size_t>(p.x);
}

Int3 VoxelStructure::position_from_index(size_t index) const {
    if (index >= voxels_.size()) throw std::out_of_range("Voxel index is outside the structure");
    const size_t xSize = static_cast<size_t>(size_.x);
    const size_t ySize = static_cast<size_t>(size_.y);
    return {static_cast<int32_t>(index % xSize),
            static_cast<int32_t>((index / xSize) % ySize),
            static_cast<int32_t>(index / (xSize * ySize))};
}

VoxelValue VoxelStructure::get(Int3 p) const { return voxels_.at(linear_index(p)); }
bool VoxelStructure::set(Int3 p, VoxelValue value) {
    if (!contains(p)) return false;
    if (value.empty()) value = VoxelValue::air();
    VoxelValue& current = voxels_[linear_index(p)];
    if (current == value) return false;
    current = value;
    return true;
}
void VoxelStructure::add_socket(VoxelSocket socket) {
    if (socket.name.empty() || !contains(socket.position)) throw std::invalid_argument("Invalid voxel socket");
    const auto duplicate = std::find_if(sockets_.begin(), sockets_.end(), [&](const auto& entry) { return entry.name == socket.name; });
    if (duplicate != sockets_.end()) *duplicate = std::move(socket); else sockets_.push_back(std::move(socket));
}
void VoxelStructure::set_variant(std::string key, std::string assetReference) {
    if (key.empty()) throw std::invalid_argument("Voxel variant key cannot be empty");
    variants_[std::move(key)] = std::move(assetReference);
}
void VoxelStructure::add_entity(VoxelEntityReference entity) {
    if (entity.assetReference.empty() || !contains(entity.position)) throw std::invalid_argument("Invalid associated voxel entity");
    entities_.push_back(std::move(entity));
}

bool VoxelStructureIO::export_source(const VoxelStructure& s, const std::filesystem::path& path, std::string* error) {
    if (s.voxels_.empty() || !ensure_parent(path, error)) return false;
    std::ofstream output(path, std::ios::trunc);
    if (!output) { set_error(error, "Cannot open voxel source for writing"); return false; }
    output << SourceMagic << ' ' << FormatVersion << '\n'
           << "name " << std::quoted(s.name_) << '\n'
           << "size " << s.size_.x << ' ' << s.size_.y << ' ' << s.size_.z << '\n'
           << "pivot " << s.pivot_.x << ' ' << s.pivot_.y << ' ' << s.pivot_.z << '\n';
    output << "sockets " << s.sockets_.size() << '\n';
    for (const auto& socket : s.sockets_)
        output << std::quoted(socket.name) << ' ' << socket.position.x << ' ' << socket.position.y << ' ' << socket.position.z << '\n';
    std::vector<std::pair<std::string, std::string>> variants(s.variants_.begin(), s.variants_.end());
    std::sort(variants.begin(), variants.end());
    output << "variants " << variants.size() << '\n';
    for (const auto& [key, value] : variants) output << std::quoted(key) << ' ' << std::quoted(value) << '\n';
    output << "entities " << s.entities_.size() << '\n';
    for (const auto& entity : s.entities_)
        output << std::quoted(entity.assetReference) << ' ' << entity.position.x << ' ' << entity.position.y << ' ' << entity.position.z << '\n';
    const auto runs = make_runs(s.voxels_);
    output << "runs " << runs.size() << '\n';
    for (const auto& run : runs)
        output << run.length << ' ' << run.value.type << ' ' << run.value.material << ' ' << static_cast<uint32_t>(run.value.density) << '\n';
    if (!output) { set_error(error, "Failed while writing voxel source"); return false; }
    return true;
}

bool VoxelStructureIO::import_source(const std::filesystem::path& path, VoxelStructure& result, std::string* error) {
    std::ifstream input(path);
    std::string magic, label, name;
    uint32_t version{};
    Int3 size{}, pivot{};
    if (!(input >> magic >> version) || magic != SourceMagic || version != FormatVersion ||
        !(input >> label >> std::quoted(name)) || label != "name" ||
        !(input >> label >> size.x >> size.y >> size.z) || label != "size" ||
        !(input >> label >> pivot.x >> pivot.y >> pivot.z) || label != "pivot") {
        set_error(error, "Invalid voxel source header"); return false;
    }
    uint64_t voxelCount{};
    if (!valid_size(size, voxelCount) || pivot.x < 0 || pivot.y < 0 || pivot.z < 0 ||
        pivot.x >= size.x || pivot.y >= size.y || pivot.z >= size.z) {
        set_error(error, "Invalid voxel source dimensions or pivot"); return false;
    }
    VoxelStructure parsed(size, std::move(name));
    parsed.pivot_ = pivot;
    uint32_t count{};
    if (!(input >> label >> count) || label != "sockets" || count > MaxCollectionCount) { set_error(error, "Invalid voxel sockets"); return false; }
    for (uint32_t i = 0; i < count; ++i) {
        VoxelSocket socket;
        if (!(input >> std::quoted(socket.name) >> socket.position.x >> socket.position.y >> socket.position.z) ||
            socket.name.empty() || !parsed.contains(socket.position)) { set_error(error, "Invalid voxel socket entry"); return false; }
        parsed.sockets_.push_back(std::move(socket));
    }
    if (!(input >> label >> count) || label != "variants" || count > MaxCollectionCount) { set_error(error, "Invalid voxel variants"); return false; }
    for (uint32_t i = 0; i < count; ++i) {
        std::string key, value;
        if (!(input >> std::quoted(key) >> std::quoted(value)) || key.empty()) { set_error(error, "Invalid voxel variant entry"); return false; }
        parsed.variants_[std::move(key)] = std::move(value);
    }
    if (!(input >> label >> count) || label != "entities" || count > MaxCollectionCount) { set_error(error, "Invalid voxel entities"); return false; }
    for (uint32_t i = 0; i < count; ++i) {
        VoxelEntityReference entity;
        if (!(input >> std::quoted(entity.assetReference) >> entity.position.x >> entity.position.y >> entity.position.z) ||
            entity.assetReference.empty() || !parsed.contains(entity.position)) { set_error(error, "Invalid voxel entity entry"); return false; }
        parsed.entities_.push_back(std::move(entity));
    }
    if (!(input >> label >> count) || label != "runs" || count == 0 || count > voxelCount) { set_error(error, "Invalid voxel run table"); return false; }
    size_t cursor = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t length{}, type{}, material{}, density{};
        if (!(input >> length >> type >> material >> density) || length == 0 || type > UINT16_MAX || material > UINT16_MAX || density > UINT8_MAX ||
            length > parsed.voxels_.size() - cursor) { set_error(error, "Invalid voxel run entry"); return false; }
        VoxelValue value{static_cast<uint16_t>(type), static_cast<uint16_t>(material), static_cast<uint8_t>(density)};
        if (value.empty()) value = {};
        std::fill_n(parsed.voxels_.begin() + static_cast<std::ptrdiff_t>(cursor), static_cast<size_t>(length), value);
        cursor += static_cast<size_t>(length);
    }
    if (cursor != parsed.voxels_.size()) { set_error(error, "Voxel runs do not fill declared dimensions"); return false; }
    result = std::move(parsed);
    return true;
}

bool VoxelStructureIO::cook(const VoxelStructure& s, const std::filesystem::path& path, std::string* error) {
    if (s.voxels_.empty() || !ensure_parent(path, error)) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) { set_error(error, "Cannot open cooked voxel asset for writing"); return false; }
    output.write(CookedMagic.data(), CookedMagic.size());
    const auto runs = make_runs(s.voxels_);
    if (!write_scalar(output, FormatVersion) || !write_string(output, s.name_) || !write_int3(output, s.size_) || !write_int3(output, s.pivot_) ||
        !write_scalar(output, static_cast<uint32_t>(s.sockets_.size()))) return false;
    for (const auto& socket : s.sockets_) if (!write_string(output, socket.name) || !write_int3(output, socket.position)) return false;
    std::vector<std::pair<std::string, std::string>> variants(s.variants_.begin(), s.variants_.end());
    std::sort(variants.begin(), variants.end());
    if (!write_scalar(output, static_cast<uint32_t>(variants.size()))) return false;
    for (const auto& [key, value] : variants) if (!write_string(output, key) || !write_string(output, value)) return false;
    if (!write_scalar(output, static_cast<uint32_t>(s.entities_.size()))) return false;
    for (const auto& entity : s.entities_) if (!write_string(output, entity.assetReference) || !write_int3(output, entity.position)) return false;
    if (runs.size() > UINT32_MAX || !write_scalar(output, static_cast<uint32_t>(runs.size()))) return false;
    for (const auto& run : runs) if (!write_scalar(output, run.length) || !write_voxel(output, run.value)) return false;
    if (!output) { set_error(error, "Failed while writing cooked voxel asset"); return false; }
    return true;
}

bool VoxelStructureIO::load_cooked(const std::filesystem::path& path, VoxelStructure& result, std::string* error) {
    std::ifstream input(path, std::ios::binary);
    std::array<char, 8> magic{};
    uint32_t version{};
    std::string name;
    Int3 size{}, pivot{};
    input.read(magic.data(), magic.size());
    if (!input || magic != CookedMagic || !read_scalar(input, version) || version != FormatVersion ||
        !read_string(input, name) || !read_int3(input, size) || !read_int3(input, pivot)) {
        set_error(error, "Invalid cooked voxel header"); return false;
    }
    uint64_t voxelCount{};
    if (!valid_size(size, voxelCount)) { set_error(error, "Invalid cooked voxel dimensions"); return false; }
    VoxelStructure parsed(size, std::move(name));
    if (!parsed.contains(pivot)) { set_error(error, "Invalid cooked voxel pivot"); return false; }
    parsed.pivot_ = pivot;
    uint32_t count{};
    if (!read_scalar(input, count) || count > MaxCollectionCount) return false;
    for (uint32_t i = 0; i < count; ++i) { VoxelSocket v; if (!read_string(input, v.name) || !read_int3(input, v.position) || v.name.empty() || !parsed.contains(v.position)) return false; parsed.sockets_.push_back(std::move(v)); }
    if (!read_scalar(input, count) || count > MaxCollectionCount) return false;
    for (uint32_t i = 0; i < count; ++i) { std::string k,v; if (!read_string(input,k)||!read_string(input,v)||k.empty()) return false; parsed.variants_[std::move(k)]=std::move(v); }
    if (!read_scalar(input, count) || count > MaxCollectionCount) return false;
    for (uint32_t i = 0; i < count; ++i) { VoxelEntityReference v; if (!read_string(input,v.assetReference)||!read_int3(input,v.position)||v.assetReference.empty()||!parsed.contains(v.position)) return false; parsed.entities_.push_back(std::move(v)); }
    if (!read_scalar(input, count) || count == 0 || count > voxelCount) return false;
    size_t cursor{};
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t length{}; VoxelValue value;
        if (!read_scalar(input,length)||!read_voxel(input,value)||length==0||length>parsed.voxels_.size()-cursor) return false;
        if (value.empty()) value = {};
        std::fill_n(parsed.voxels_.begin()+static_cast<std::ptrdiff_t>(cursor), length, value); cursor += length;
    }
    if (cursor != parsed.voxels_.size() || input.peek() != std::char_traits<char>::eof()) { set_error(error, "Invalid cooked voxel payload"); return false; }
    result = std::move(parsed);
    return true;
}

bool VoxelStructureImporter::supports_extension(std::string_view extension) const {
    std::string lower(extension);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower == ".voxelstructure";
}
ImportResult VoxelStructureImporter::import(const ImportRequest& request) const {
    VoxelStructure structure;
    std::string error;
    if (!VoxelStructureIO::import_source(request.source, structure, &error)) return {false, {}, std::move(error)};
    std::error_code ec;
    std::filesystem::create_directories(request.cookedDirectory, ec);
    if (ec) return {false, {}, "Cannot create cooked voxel directory: " + ec.message()};
    AssetMetadata metadata;
    metadata.id = UUID();
    metadata.type = AssetType::VoxelStructure;
    metadata.sourcePath = std::filesystem::weakly_canonical(request.source, ec);
    if (ec) { ec.clear(); metadata.sourcePath = request.source.lexically_normal(); }
    metadata.contentHash = hash_file(request.source);
    metadata.importerVersion = request.importerVersion;
    metadata.importSettings = request.settings;
    metadata.cookedPath = request.cookedDirectory / (std::to_string(metadata.contentHash) + ".vcvoxel");
    if (!VoxelStructureIO::cook(structure, metadata.cookedPath, &error)) return {false, {}, std::move(error)};
    metadata.isCooked = true;
    metadata.width = static_cast<uint32_t>(structure.size().x);
    metadata.height = static_cast<uint32_t>(structure.size().y);
    metadata.channels = static_cast<uint32_t>(structure.size().z);
    return {true, std::move(metadata), {}};
}

} // namespace Engine::Voxel
