#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Path/tree mechanics only. The caller must verify/admit the complete archive
// BEFORE Build and retain every FD for the process lifetime, as locale already
// does. This helper neither parses receipts nor owns FDs nor grants ownership.
namespace bl2_orchid {

// The caller separately admits each campaign before selecting this policy.
// These fixed roots are a path allowlist, never an ownership/activation grant.
enum class Campaigns : unsigned { Orchid = 1, Sage = 2, OrchidAndSage = 3, Iris = 4, OrchidAndIris = 5, SageAndIris = 6, OrchidSageAndIris = 7 };

enum class NodeKind { Missing, File, Directory };
struct FileRow {
    std::string path;
    int fd = -1;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};
struct Child {
    std::string name;
    NodeKind kind = NodeKind::Missing;
};

class PathIndex {
public:
    static constexpr std::size_t kMaxPath = 1024;
    static constexpr std::size_t kMaxComponent = 255;
    static constexpr std::size_t kMaxFiles = 16384;
    static constexpr std::size_t kMaxDirectories = 16384;

    // Transactional: failure leaves output unchanged. Publish the successful
    // index as const and do not rebuild it while callers retain its pointers.
    static bool Build(const std::vector<FileRow>& rows, PathIndex& output,
                      const char** error = nullptr) {
        return BuildWithCampaigns(rows, Campaigns::Orchid, output, error);
    }

    // The caller must admit EVERY campaign selected here before supplying rows.
    // Unknown/zero policies reject; unrelated DLC roots are never accepted.
    static bool BuildWithCampaigns(const std::vector<FileRow>& rows,
                      Campaigns admitted, PathIndex& output,
                      const char** error = nullptr) {
        const auto fail = [error](const char* reason) {
            if (error) *error = reason;
            return false;
        };
        if (error) *error = nullptr;
        if (static_cast<unsigned>(admitted) == 0 || static_cast<unsigned>(admitted) > 7)
            return fail("invalid admitted DLC campaign policy");
        const bool orchid_only = admitted == Campaigns::Orchid;
        if (rows.size() > kMaxFiles) return fail(orchid_only
            ? "too many Orchid files" : "too many admitted DLC files");
        PathIndex next;
        next.campaigns_ = admitted;
        std::map<std::string, std::map<std::string, Child>> children;
        for (const auto& row : rows) {
            std::string key, display;
            bool trailing = false;
            if (!Clean(row.path, display, trailing) || trailing ||
                !next.Logical(display, key) || !next.CampaignFile(key))
                return fail(orchid_only ? "invalid Orchid file path"
                                        : "invalid admitted DLC file path");
            constexpr auto maximum = static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max());
            if (row.fd < 0 || row.offset > maximum || row.size > maximum - row.offset)
                return fail(orchid_only ? "invalid Orchid file region" : "invalid admitted DLC file region");
            if (!next.files_.emplace(key, FileRow{display, row.fd, row.offset, row.size}).second)
                return fail(orchid_only ? "duplicate Orchid file" : "duplicate admitted DLC file");
            std::size_t at = 0;
            for (;;) {
                const auto slash = key.find('/', at);
                if (slash == std::string::npos) break;
                const std::string parent = key.substr(0, slash);
                const auto end = key.find('/', slash + 1);
                const bool file = end == std::string::npos;
                const std::size_t length = file ? key.size() - slash - 1 : end - slash - 1;
                const auto name = display.substr(slash + 1, length);
                const auto child_key = key.substr(slash + 1, length);
                Child child{name, file ? NodeKind::File : NodeKind::Directory};
                auto& siblings = children[parent];
                const auto existing = siblings.find(child_key);
                if (existing != siblings.end() && existing->second.kind != child.kind)
                    return fail(orchid_only ? "Orchid file/directory conflict" : "admitted DLC file/directory conflict");
                siblings.emplace(child_key, std::move(child));
                if (children.size() > kMaxDirectories)
                    return fail(orchid_only ? "too many Orchid directories" : "too many admitted DLC directories");
                at = slash + 1;
            }
        }
        for (auto& item : children) {
            if (next.files_.find(item.first) != next.files_.end())
                return fail(orchid_only ? "Orchid file/directory conflict" : "admitted DLC file/directory conflict");
            auto& list = next.directories_[item.first];
            list.reserve(item.second.size());
            for (auto& entry : item.second) list.push_back(std::move(entry.second));
        }
        output = std::move(next);
        return true;
    }

    // resolved_dlc_root is the actual observed absolute path to the engine's
    // DLC directory, including its final /DLC component. It is optional for
    // relative requests. No cwd, guessed Android root or parent resolution.
    const FileRow* Find(std::string_view path,
                        std::string_view resolved_dlc_root = {}) const {
        std::string key;
        bool trailing = false;
        if (!Lookup(path, resolved_dlc_root, key, trailing) || trailing) return nullptr;
        const auto it = files_.find(key);
        return it == files_.end() ? nullptr : &it->second;
    }

    const std::vector<Child>* Children(std::string_view path,
                        std::string_view resolved_dlc_root = {}) const {
        std::string key;
        bool trailing = false;
        if (!Lookup(path, resolved_dlc_root, key, trailing)) return nullptr;
        const auto it = directories_.find(key);
        return it == directories_.end() ? nullptr : &it->second;
    }

    NodeKind Kind(std::string_view path,
                  std::string_view resolved_dlc_root = {}) const {
        std::string key;
        bool trailing = false;
        if (!Lookup(path, resolved_dlc_root, key, trailing)) return NodeKind::Missing;
        if (!trailing && files_.find(key) != files_.end()) return NodeKind::File;
        return directories_.find(key) != directories_.end()
            ? NodeKind::Directory : NodeKind::Missing;
    }

    std::size_t FileCount() const { return files_.size(); }

private:
    Campaigns campaigns_ = Campaigns::Orchid;
    std::map<std::string, FileRow> files_;
    std::map<std::string, std::vector<Child>> directories_;

    static std::string Fold(std::string_view input) {
        std::string out(input);
        for (char& c : out) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 'a' - 'A');
        return out;
    }

    // Converts separators only; rejects controls, drive/UNC syntax, wildcards,
    // repeated separators and overlong components. A single final slash is
    // retained as a directory requirement for file lookups.
    static bool Clean(std::string_view input, std::string& out, bool& trailing) {
        if (input.empty() || input.size() > kMaxPath) return false;
        out.assign(input);
        for (char& c : out) {
            const auto byte = static_cast<unsigned char>(c);
            if (byte < 32 || byte == 127 || c == ':' || c == '*' || c == '?') return false;
            if (c == '\\') c = '/';
        }
        if (out.find("//") != std::string::npos) return false;
        trailing = out.back() == '/';
        if (trailing) out.pop_back();
        if (out.empty()) return false;
        std::size_t begin = out.front() == '/' ? 1 : 0;
        while (begin < out.size()) {
            const auto end = out.find('/', begin);
            const auto length = (end == std::string::npos ? out.size() : end) - begin;
            if (length == 0 || length > kMaxComponent) return false;
            begin = end == std::string::npos ? out.size() : end + 1;
        }
        return true;
    }

    static bool NoDots(std::string_view path) {
        std::size_t at = path.front() == '/' ? 1 : 0;
        while (at < path.size()) {
            const auto end = path.find('/', at);
            const auto component = path.substr(at, end == std::string::npos ? path.size() - at : end - at);
            if (component == "." || component == "..") return false;
            at = end == std::string::npos ? path.size() : end + 1;
        }
        return true;
    }

    bool HasOrchid() const {
        return (static_cast<unsigned>(campaigns_) & 1u) != 0;
    }

    bool HasSage() const {
        return (static_cast<unsigned>(campaigns_) & 2u) != 0;
    }

    bool HasIris() const { return (static_cast<unsigned>(campaigns_) & 4u) != 0; }

    bool CampaignFile(std::string_view key) const {
        return (HasOrchid() && key.compare(0, 11, "dlc/orchid/") == 0) ||
               (HasSage() && key.compare(0, 9, "dlc/sage/") == 0) ||
               (HasIris() && key.compare(0, 9, "dlc/iris/") == 0);
    }

    bool Logical(std::string_view path, std::string& key) const {
        if (path.empty() || path.front() == '/' || !NoDots(path)) return false;
        key = Fold(path);
        return key == "dlc" || (HasOrchid() && key == "dlc/orchid") ||
               (HasSage() && key == "dlc/sage") || (HasIris() && key == "dlc/iris") || CampaignFile(key);
    }

    bool Lookup(std::string_view input, std::string_view resolved,
                       std::string& key, bool& trailing) const {
        std::string path;
        if (!Clean(input, path, trailing)) return false;
        if (path.front() == '/') {
            std::string root;
            bool root_slash = false;
            if (!Clean(resolved, root, root_slash) || root.front() != '/' || !NoDots(root)) return false;
            root = Fold(root);
            if (root.size() < 4 || root.compare(root.size() - 4, 4, "/dlc") != 0) return false;
            const auto absolute = Fold(path);
            if (absolute.compare(0, root.size(), root) != 0 ||
                (absolute.size() > root.size() && absolute[root.size()] != '/')) return false;
            path = "dlc" + path.substr(root.size());
        } else if (path.compare(0, 6, "../../") == 0) {
            path.erase(0, 6);
        }
        return Logical(path, key);
    }
};

}  // namespace bl2_orchid
