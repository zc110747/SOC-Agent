#include "camera_agent/config.h"
#include "camera_agent/logger.h"

#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace ca {

namespace {

// --- Minimal YAML reader (supports nested maps + scalar values) -------------
struct YNode {
    enum Type { MAP, SCALAR } type = MAP;
    std::string scalar;
    std::map<std::string, YNode> children;
};

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string strip_quotes(const std::string& s) {
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front())
        return s.substr(1, s.size() - 2);
    return s;
}

int indent_of(const std::string& line) {
    int n = 0;
    while (n < static_cast<int>(line.size()) &&
           (line[n] == ' ' || line[n] == '\t')) ++n;
    return n;
}

// Parse a very small YAML subset used by camera-agent.yaml.
YNode parse_yaml(std::istream& in) {
    YNode root;
    root.type = YNode::MAP;
    // stack of (indent, node*) — top is the current parent map.
    std::vector<std::pair<int, YNode*>> stack;
    stack.push_back({-1, &root});

    std::string raw;
    while (std::getline(in, raw)) {
        // strip trailing CR and inline comments (outside quotes - good enough here)
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;

        // cut comment (naive: first '#' not inside quotes)
        size_t hash = line.find('#');
        if (hash != std::string::npos) {
            // only treat as comment if preceded by whitespace
            if (hash == 0 || std::isspace(static_cast<unsigned char>(line[hash - 1])))
                line = trim(line.substr(0, hash));
            if (line.empty()) continue;
        }

        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;

        const std::string key = trim(line.substr(0, colon));
        std::string val = trim(line.substr(colon + 1));
        const int indent = indent_of(raw);

        // pop stack until we find a parent with smaller indent
        while (stack.size() > 1 && stack.back().first >= indent)
            stack.pop_back();
        YNode* parent = stack.back().second;

        YNode node;
        if (val.empty()) {
            node.type = YNode::MAP;
            parent->children[key] = node;
            stack.push_back({indent, &parent->children[key]});
        } else {
            node.type = YNode::SCALAR;
            node.scalar = strip_quotes(val);
            parent->children[key] = node;
        }
    }
    return root;
}

const YNode* find(const YNode& root, const std::string& key) {
    auto it = root.children.find(key);
    return (it == root.children.end()) ? nullptr : &it->second;
}

int as_int(const YNode* n, int def) {
    if (!n || n->type != YNode::SCALAR) return def;
    return std::atoi(n->scalar.c_str());
}
std::string as_str(const YNode* n, const std::string& def) {
    if (!n || n->type != YNode::SCALAR) return def;
    return n->scalar;
}

} // namespace

Config make_default_config() {
    return Config{};
}

bool load_config(Config& cfg, const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        CA_LOG_WARN("Config file not found: {} (using defaults / CLI overrides)", path);
        return false;
    }

    YNode root = parse_yaml(f);

    if (auto* cam = find(root, "camera")) {
        cfg.camera.id     = as_int(find(*cam, "id"), cfg.camera.id);
        cfg.camera.width  = as_int(find(*cam, "width"), cfg.camera.width);
        cfg.camera.height = as_int(find(*cam, "height"), cfg.camera.height);
        cfg.camera.fps    = as_int(find(*cam, "fps"), cfg.camera.fps);
    }
    if (auto* enc = find(root, "encoder")) {
        cfg.encoder.codec            = as_str(find(*enc, "codec"), cfg.encoder.codec);
        cfg.encoder.bitrate          = as_int(find(*enc, "bitrate"), cfg.encoder.bitrate);
        cfg.encoder.keyframe_interval = as_int(find(*enc, "keyframe_interval"), cfg.encoder.keyframe_interval);
    }
    if (auto* st = find(root, "stream"))
        cfg.stream.id = as_str(find(*st, "id"), cfg.stream.id);
    if (auto* rtsp = find(root, "rtsp")) {
        cfg.rtsp.server = as_str(find(*rtsp, "server"), cfg.rtsp.server);
        cfg.rtsp.port   = as_int(find(*rtsp, "port"), cfg.rtsp.port);
    }
    cfg.device_id = as_str(find(root, "device_id"), cfg.device_id);
    cfg.log_level = as_str(find(root, "log_level"), cfg.log_level);
    return true;
}

} // namespace ca
