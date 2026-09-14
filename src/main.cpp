// LocalAppData 更新残留清理
//
//   * 扫描 %LOCALAPPDATA% 下的更新/安装残留目录与散落的安装包（exe / msi / nupkg）
//   * 单独列出 %LOCALAPPDATA%\Temp 中超过阈值的陈旧条目
//   * 排除规则（目录 + 关键词）可编辑、可持久化到 JSON 配置文件
//   * 复选框逐项确认（默认不勾选），实时统计勾选体积
//   * 支持按名称/日期/类型/大小 升序或降序排列
//   * 每项带一个文件夹图标，可直接在资源管理器中查看
//   * 可把清单导出成 HTML 报告（样式内联，双击即看）+ 同名 XML 数据，位置自选
//   * 清理前弹出不可恢复的确认面板，清理完成后给出结果汇总
//
// UI 使用 EUI-NEO 声明式 DSL，扫描/删除跑在 app::async 线程池里。
// 文件保存对话框、选择文件夹对话框使用 Win32 原生实现（框架只提供"打开文件"）。

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "eui_neo.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#include <shlobj.h>
#endif

namespace app {

namespace {

namespace fs = std::filesystem;

using Tokens = components::theme::ThemeColorTokens;

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------

constexpr int kThresholdDays[] = {1, 3, 7, 14, 30};
constexpr int kThresholdCount = static_cast<int>(sizeof(kThresholdDays) / sizeof(kThresholdDays[0]));
constexpr const char* kThresholdLabels[] = {"1 天", "3 天", "7 天", "14 天", "30 天"};

constexpr int kFolderScanDepth = 2;
constexpr int kFileScanDepth = 3;

constexpr const char* kSettingsFolderName = "LocalAppDataCleaner";
constexpr const char* kSettingsFileName = "settings.json";

constexpr float kRowHeight = 64.0f;
constexpr float kRowGap = 8.0f;

// 字号
constexpr float kFontTitle = 29.0f;
constexpr float kFontSubtitle = 14.0f;
constexpr float kFontRowName = 16.5f;
constexpr float kFontRowDetail = 13.0f;
constexpr float kFontRowSize = 15.0f;
constexpr float kFontStats = 18.0f;
constexpr float kFontStatsSmall = 13.0f;
constexpr float kFontLabel = 14.0f;
constexpr float kFontBadge = 12.5f;
constexpr float kFontNoticeTitle = 18.0f;
constexpr float kFontNoticeBody = 14.0f;

enum class SortKey {
    Name = 0,
    Date = 1,
    Type = 2,
    Size = 3
};

// ---------------------------------------------------------------------------
// 数据模型
// ---------------------------------------------------------------------------

enum class ItemKind {
    Folder,
    File,
    Temp
};

struct CleanupItem {
    fs::path path;
    std::string name;
    std::string sortName;
    std::string detail;
    std::string modifiedText;
    std::int64_t modifiedSeconds = 0;
    ItemKind kind = ItemKind::File;
    std::uint64_t bytes = 0;
    bool selected = false; // 默认不勾选
};

struct ScanOutcome {
    std::vector<CleanupItem> items;
    std::uint64_t bytes = 0;
    std::string error;
};

struct CleanOutcome {
    int removed = 0;
    int failed = 0;
    std::uint64_t freed = 0;
    std::vector<std::string> failures;
    std::string error;
};

// 排除规则：目录名精确匹配（任意层级），关键词按子串匹配（任意层级）。
// 全部以小写保存，匹配时不区分大小写。
struct ExcludeConfig {
    std::vector<std::string> directories;
    std::vector<std::string> keywords;
};

ExcludeConfig defaultExcludeConfig() {
    ExcludeConfig config;
    config.directories = {"packages", "programs"};
    config.keywords = {"site-packages", "node_modules", "python",     "pyinstaller", "microsoft",
                       "google",       "discord",      "slack",      "githubdesktop", "osu"};
    return config;
}

enum class Phase {
    Idle,
    Scanning,
    Ready,
    Cleaning
};

struct CleanerState {
    Phase phase = Phase::Idle;
    std::vector<CleanupItem> items;
    std::uint64_t totalBytes = 0;
    int thresholdIndex = 2;          // 默认 7 天，与脚本一致
    int scannedThresholdIndex = -1;  // 上次扫描实际使用的阈值
    int sortKey = static_cast<int>(SortKey::Size);
    bool sortDescending = true;

    // 排除规则与配置文件
    ExcludeConfig excludes = defaultExcludeConfig();
    bool settingsLoaded = false;
    std::string settingsError;
    int rulesVersion = 0;
    int scannedRulesVersion = -1;
    bool excludesOpen = false;
    std::string excludeDirDraft;
    std::string excludeKeywordDraft;

    bool confirmOpen = false;
    bool resultOpen = false;
    CleanOutcome report;

    std::string lastExportPath;
    std::string lastExportDirectory;

    bool toastVisible = false;
    std::string toastTitle;
    std::string toastMessage;
};

// ---------------------------------------------------------------------------
// 字符串、时间与格式化工具
// ---------------------------------------------------------------------------

std::string pathToUtf8(const fs::path& path) {
#if defined(__cpp_char8_t)
    const std::u8string value = path.u8string();
    return std::string(value.begin(), value.end());
#else
    return path.u8string();
#endif
}

std::string asciiLower(std::string value) {
    for (char& ch : value) {
        const unsigned char raw = static_cast<unsigned char>(ch);
        if (raw < 0x80) {
            ch = static_cast<char>(std::tolower(raw));
        }
    }
    return value;
}

std::string trimAscii(const std::string& value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string formatBytes(std::uint64_t bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }

    char buffer[64];
    if (unit == 0) {
        std::snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(bytes));
    } else if (value >= 100.0) {
        std::snprintf(buffer, sizeof(buffer), "%.0f %s", value, units[unit]);
    } else if (value >= 10.0) {
        std::snprintf(buffer, sizeof(buffer), "%.1f %s", value, units[unit]);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.2f %s", value, units[unit]);
    }
    return std::string(buffer);
}

std::int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// file_clock 与 system_clock 的时间原点不同，用两次 now() 的差值做换算。
std::int64_t toUnixSeconds(const fs::file_time_type& time) {
    const auto fileNow = fs::file_time_type::clock::now();
    const auto systemNow = std::chrono::system_clock::now();
    const auto delta = std::chrono::duration_cast<std::chrono::system_clock::duration>(time - fileNow);
    const auto systemTime = systemNow + delta;
    return std::chrono::duration_cast<std::chrono::seconds>(systemTime.time_since_epoch()).count();
}

std::string formatTimeText(std::int64_t seconds, const char* pattern) {
    if (seconds <= 0) {
        return "时间未知";
    }

    const std::time_t raw = static_cast<std::time_t>(seconds);
    std::tm local{};
    {
        static std::mutex guard;
        std::lock_guard<std::mutex> lock(guard);
        const std::tm* value = std::localtime(&raw);
        if (value == nullptr) {
            return "时间未知";
        }
        local = *value;
    }

    char buffer[64];
    if (std::strftime(buffer, sizeof(buffer), pattern, &local) == 0) {
        return "时间未知";
    }
    return std::string(buffer);
}

std::string formatTimestamp(std::int64_t seconds) {
    return formatTimeText(seconds, "%Y-%m-%d %H:%M");
}

std::string fileStamp(std::int64_t seconds) {
    return formatTimeText(seconds, "%Y%m%d-%H%M%S");
}

// 近似字宽：ASCII 约为字号的一半，其余（含中文）按整字号估算。
float roughTextWidth(const std::string& text, float fontSize) {
    float width = 0.0f;
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        std::size_t length = 1;
        unsigned int codepoint = lead;
        if ((lead & 0xE0) == 0xC0) {
            length = 2;
            codepoint = lead & 0x1Fu;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
            codepoint = lead & 0x0Fu;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
            codepoint = lead & 0x07u;
        }
        for (std::size_t offset = 1; offset < length && index + offset < text.size(); ++offset) {
            codepoint = (codepoint << 6u) | (static_cast<unsigned char>(text[index + offset]) & 0x3Fu);
        }
        width += (codepoint < 0x80u) ? fontSize * 0.55f : fontSize;
        index += length;
    }
    return width;
}

std::string elideText(const std::string& text, float fontSize, float maxWidth) {
    if (maxWidth <= 0.0f || roughTextWidth(text, fontSize) <= maxWidth) {
        return text;
    }

    const float ellipsisWidth = roughTextWidth("…", fontSize);
    std::string result;
    float width = 0.0f;
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        std::size_t length = 1;
        unsigned int codepoint = lead;
        if ((lead & 0xE0) == 0xC0) {
            length = 2;
            codepoint = lead & 0x1Fu;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
            codepoint = lead & 0x0Fu;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
            codepoint = lead & 0x07u;
        }
        for (std::size_t offset = 1; offset < length && index + offset < text.size(); ++offset) {
            codepoint = (codepoint << 6u) | (static_cast<unsigned char>(text[index + offset]) & 0x3Fu);
        }

        const float advance = (codepoint < 0x80u) ? fontSize * 0.55f : fontSize;
        if (width + advance + ellipsisWidth > maxWidth) {
            break;
        }
        result.append(text, index, length);
        width += advance;
        index += length;
    }
    return result + "…";
}

bool endsWith(const std::string& value, const char* suffix) {
    const std::size_t length = std::char_traits<char>::length(suffix);
    return value.size() >= length && value.compare(value.size() - length, length, suffix) == 0;
}

std::string xmlEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 16);
    for (char ch : value) {
        switch (ch) {
            case '&':
                escaped += "&amp;";
                break;
            case '<':
                escaped += "&lt;";
                break;
            case '>':
                escaped += "&gt;";
                break;
            case '"':
                escaped += "&quot;";
                break;
            case '\'':
                escaped += "&apos;";
                break;
            default:
                escaped += ch;
                break;
        }
    }
    return escaped;
}

std::string jsonEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 16);
    for (char ch : value) {
        const unsigned char raw = static_cast<unsigned char>(ch);
        switch (ch) {
            case '"':
                escaped += "\\\"";
                break;
            case '\\':
                escaped += "\\\\";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                if (raw < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", raw);
                    escaped += buffer;
                } else {
                    escaped += ch;
                }
                break;
        }
    }
    return escaped;
}

fs::path localAppDataRoot() {
#ifdef _WIN32
    if (const wchar_t* value = _wgetenv(L"LOCALAPPDATA"); value != nullptr && value[0] != L'\0') {
        return fs::path(value);
    }
#endif
    if (const char* value = std::getenv("LOCALAPPDATA"); value != nullptr && value[0] != '\0') {
        return fs::path(value);
    }
    return {};
}

// ---------------------------------------------------------------------------
// 原生对话框（保存文件 / 选择文件夹）
// ---------------------------------------------------------------------------

struct DialogResult {
    bool ok = false;
    bool cancelled = false;
    std::string path;
    std::string error;
};

#if defined(_WIN32)

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

// 系统「另存为」对话框。取消与失败分开表示，方便调用方区分静默返回和报错。
DialogResult saveFileDialog(const std::string& suggestedName,
                            const std::string& initialDirectory,
                            const std::string& filterLabel,
                            const std::vector<std::string>& extensions) {
    DialogResult result;

    std::vector<wchar_t> fileBuffer(4096, L'\0');
    const std::wstring suggested = utf8ToWide(suggestedName);
    const std::size_t suggestedLength = std::min(suggested.size(), fileBuffer.size() - 1);
    std::copy(suggested.begin(), suggested.begin() + static_cast<std::ptrdiff_t>(suggestedLength), fileBuffer.begin());

    std::wstring filter;
    for (const std::string& extension : extensions) {
        const std::wstring wide = utf8ToWide(extension);
        filter += utf8ToWide(filterLabel) + L" (*." + wide + L")";
        filter.push_back(L'\0');
        filter += L"*." + wide;
        filter.push_back(L'\0');
    }
    filter += L"所有文件 (*.*)";
    filter.push_back(L'\0');
    filter += L"*.*";
    filter.push_back(L'\0');
    filter.push_back(L'\0');

    const std::wstring directory = utf8ToWide(initialDirectory);
    const std::wstring defaultExtension = extensions.empty() ? std::wstring() : utf8ToWide(extensions.front());

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = GetActiveWindow();
    dialog.lpstrFilter = filter.c_str();
    dialog.nFilterIndex = 1;
    dialog.lpstrFile = fileBuffer.data();
    dialog.nMaxFile = static_cast<DWORD>(fileBuffer.size());
    dialog.lpstrInitialDir = directory.empty() ? nullptr : directory.c_str();
    dialog.lpstrDefExt = defaultExtension.empty() ? nullptr : defaultExtension.c_str();
    dialog.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetSaveFileNameW(&dialog) == FALSE) {
        const DWORD code = CommDlgExtendedError();
        if (code == 0) {
            result.cancelled = true;
        } else {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "0x%08lx", static_cast<unsigned long>(code));
            result.error = std::string("保存对话框返回错误 ") + buffer;
        }
        return result;
    }

    result.ok = true;
    result.path = wideToUtf8(fileBuffer.data());
    return result;
}

int CALLBACK browseFolderCallback(HWND window, UINT message, LPARAM, LPARAM data) {
    if (message == BFFM_INITIALIZED && data != 0) {
        const wchar_t* path = reinterpret_cast<const wchar_t*>(data);
        SendMessageW(window, BFFM_SETSELECTIONW, TRUE, reinterpret_cast<LPARAM>(path));
    }
    return 0;
}

// 系统「选择文件夹」对话框，初始定位到 initialDirectory。
DialogResult chooseFolderDialog(const std::string& initialDirectory) {
    DialogResult result;

    // BIF_NEWDIALOGSTYLE 要求调用线程初始化过 COM；GLFW 通常已经调用过
    // OleInitialize，这里只是兜底，重复初始化会返回 S_FALSE 或模式冲突，忽略即可。
    const HRESULT comStatus = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::wstring start = utf8ToWide(initialDirectory);
    std::vector<wchar_t> environment(MAX_PATH * 4, L'\0');
    if (start.empty() || GetFileAttributesW(start.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", environment.data(),
                                                     static_cast<DWORD>(environment.size()));
        if (length > 0 && length < environment.size()) {
            start.assign(environment.data(), length);
        } else {
            start.clear();
        }
    }

    BROWSEINFOW browse{};
    browse.hwndOwner = GetActiveWindow();
    browse.lpszTitle = L"选择要排除的文件夹（该目录名将从扫描范围中跳过）";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
    browse.lpfn = browseFolderCallback;
    browse.lParam = start.empty() ? 0 : reinterpret_cast<LPARAM>(start.c_str());

    LPITEMIDLIST idList = SHBrowseForFolderW(&browse);
    if (idList == nullptr) {
        if (comStatus == S_OK) {
            CoUninitialize();
        }
        result.cancelled = true;
        return result;
    }

    std::vector<wchar_t> buffer(MAX_PATH * 4, L'\0');
    const bool converted = SHGetPathFromIDListW(idList, buffer.data()) != FALSE;
    CoTaskMemFree(idList);
    if (comStatus == S_OK) {
        CoUninitialize();
    }
    if (!converted) {
        result.error = "无法读取所选文件夹的路径。";
        return result;
    }

    result.ok = true;
    result.path = wideToUtf8(buffer.data());
    return result;
}

#else

DialogResult saveFileDialog(const std::string&, const std::string&, const std::string&, const std::vector<std::string>&) {
    DialogResult result;
    result.error = "当前平台暂不支持系统保存对话框。";
    return result;
}

DialogResult chooseFolderDialog(const std::string&) {
    DialogResult result;
    result.error = "当前平台暂不支持系统选择文件夹对话框。";
    return result;
}

#endif

// ---------------------------------------------------------------------------
// 配置持久化（JSON）
//
// 读用框架自带的 eui::json（yyjson），写是这个结构只有两个字符串数组，
// 手工拼 JSON 比引入写库更简单，也没有性能问题。
// ---------------------------------------------------------------------------

fs::path settingsFilePath() {
    const fs::path root = localAppDataRoot();
    if (root.empty()) {
        return {};
    }
    return root / kSettingsFolderName / kSettingsFileName;
}

bool saveSettings(const ExcludeConfig& config, const std::string& lastExportDirectory) {
    const fs::path file = settingsFilePath();
    if (file.empty()) {
        return false;
    }

    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (ec) {
        return false;
    }

    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }

    const auto writeArray = [&stream](const char* name, const std::vector<std::string>& values) {
        stream << "  \"" << name << "\": [";
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (index > 0) {
                stream << ", ";
            }
            stream << "\"" << jsonEscape(values[index]) << "\"";
        }
        stream << "]";
    };

    stream << "{\n";
    stream << "  \"version\": 1,\n";
    writeArray("excludeDirectories", config.directories);
    stream << ",\n";
    writeArray("excludeKeywords", config.keywords);
    stream << ",\n";
    stream << "  \"lastExportDirectory\": \"" << jsonEscape(lastExportDirectory) << "\"\n";
    stream << "}\n";
    stream.close();

    return static_cast<bool>(stream);
}

// 返回 true 表示成功读到配置；false 时 error 非空表示读到了但有问题。
bool loadSettings(ExcludeConfig& config, std::string& lastExportDirectory, std::string& error) {
    const fs::path file = settingsFilePath();
    if (file.empty()) {
        error = "无法定位 %LOCALAPPDATA%，配置将不会被保存。";
        return false;
    }

    std::error_code ec;
    if (!fs::exists(file, ec)) {
        return false; // 首次运行：直接用默认规则
    }

    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        error = "无法读取配置文件，已改用默认排除规则。";
        return false;
    }

    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    eui::json::Document document;
    if (!document.parse(text) || !document.valid()) {
        error = "配置文件不是合法的 JSON，已改用默认排除规则。";
        return false;
    }

    const auto readArray = [&document](const char* pointer, std::vector<std::string>& output) {
        const eui::json::Value value = document.atPointer(pointer);
        if (!value.valid() || value.type() != eui::json::Type::Array) {
            return false;
        }
        const std::size_t count = std::min<std::size_t>(value.size(), 1024);
        output.clear();
        for (std::size_t index = 0; index < count; ++index) {
            std::string item;
            if (value.at(index).string(item)) {
                const std::string trimmed = asciiLower(trimAscii(item));
                if (!trimmed.empty()) {
                    output.push_back(trimmed);
                }
            }
        }
        return true;
    };

    ExcludeConfig loaded;
    const bool hasDirectories = readArray("/excludeDirectories", loaded.directories);
    const bool hasKeywords = readArray("/excludeKeywords", loaded.keywords);
    if (!hasDirectories && !hasKeywords) {
        error = "配置文件里没有排除规则字段，已改用默认规则。";
        return false;
    }

    const ExcludeConfig defaults = defaultExcludeConfig();
    if (!hasDirectories) {
        loaded.directories = defaults.directories;
    }
    if (!hasKeywords) {
        loaded.keywords = defaults.keywords;
    }
    config = loaded;

    std::string directory;
    if (document.atPointer("/lastExportDirectory").string(directory)) {
        lastExportDirectory = directory;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 排除规则与扫描匹配
// ---------------------------------------------------------------------------

bool nameExcluded(const ExcludeConfig& config, const std::string& lowerName, bool isDirectory) {
    for (const std::string& keyword : config.keywords) {
        if (!keyword.empty() && lowerName.find(keyword) != std::string::npos) {
            return true;
        }
    }
    if (isDirectory) {
        for (const std::string& directory : config.directories) {
            if (directory == lowerName) {
                return true;
            }
        }
    }
    return false;
}

bool matchesFolderRule(const std::string& lowerName) {
    return lowerName.find("updater") != std::string::npos ||
           lowerName.find("installer") != std::string::npos ||
           lowerName == "pending";
}

// 安装包类残留：*.exe（名字里带 update / installer / setup），以及 *.msi、*.nupkg。
bool matchesFileRule(const std::string& lowerName) {
    if (endsWith(lowerName, ".msi") || endsWith(lowerName, ".nupkg")) {
        return true;
    }
    if (!endsWith(lowerName, ".exe")) {
        return false;
    }
    return lowerName.find("update") != std::string::npos ||
           lowerName.find("installer") != std::string::npos ||
           lowerName.find("setup") != std::string::npos;
}

fs::file_time_type cutoffTime(int daysOld) {
    return fs::file_time_type::clock::now() -
           std::chrono::hours(static_cast<std::chrono::hours::rep>(daysOld) * 24);
}

int thresholdDays(const CleanerState& state) {
    const int index = std::clamp(state.thresholdIndex, 0, kThresholdCount - 1);
    return kThresholdDays[index];
}

int kindRank(ItemKind kind) {
    switch (kind) {
        case ItemKind::Folder:
            return 0;
        case ItemKind::File:
            return 1;
        case ItemKind::Temp:
        default:
            return 2;
    }
}

const char* kindName(ItemKind kind) {
    switch (kind) {
        case ItemKind::Folder:
            return "folder";
        case ItemKind::Temp:
            return "temp";
        case ItemKind::File:
        default:
            return "file";
    }
}

const char* sortKeyName(int sortKey) {
    switch (static_cast<SortKey>(sortKey)) {
        case SortKey::Name:
            return "name";
        case SortKey::Date:
            return "date";
        case SortKey::Type:
            return "type";
        case SortKey::Size:
        default:
            return "size";
    }
}

void fillItemMetadata(CleanupItem& item, const fs::file_time_type& writeTime) {
    item.sortName = asciiLower(item.name);
    item.modifiedSeconds = toUnixSeconds(writeTime);
    item.modifiedText = formatTimestamp(item.modifiedSeconds);
}

// ---------------------------------------------------------------------------
// 排序
// ---------------------------------------------------------------------------

std::vector<std::size_t> buildOrder(const CleanerState& state) {
    std::vector<std::size_t> order(state.items.size());
    for (std::size_t index = 0; index < order.size(); ++index) {
        order[index] = index;
    }

    const auto less = [&state](std::size_t left, std::size_t right) {
        const CleanupItem& a = state.items[left];
        const CleanupItem& b = state.items[right];
        switch (static_cast<SortKey>(state.sortKey)) {
            case SortKey::Name:
                return a.sortName < b.sortName;
            case SortKey::Date:
                return a.modifiedSeconds < b.modifiedSeconds;
            case SortKey::Type: {
                const int rankA = kindRank(a.kind);
                const int rankB = kindRank(b.kind);
                if (rankA != rankB) {
                    return rankA < rankB;
                }
                return a.sortName < b.sortName;
            }
            case SortKey::Size:
            default:
                return a.bytes < b.bytes;
        }
    };

    std::stable_sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        return state.sortDescending ? less(right, left) : less(left, right);
    });
    return order;
}

// ---------------------------------------------------------------------------
// 导出：XML 清单 + XSLT 样式
// ---------------------------------------------------------------------------

// 导出用：HTML 报告把样式写在文件内部，不依赖任何外部资源，
// 双击就能在浏览器里看到表格（之前靠 XML + 外部 XSLT，本地 file:// 下
// 浏览器会拒绝加载同目录样式表，打开是白页）。
constexpr const char* kReportHtmlHead = R"html(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>LocalAppData 清理清单</title>
<style>
  :root { color-scheme: light; }
  body { font-family: "Segoe UI", "Microsoft YaHei", sans-serif; margin: 24px; background: #f5f6f8; color: #1d2027; }
  h1 { font-size: 22px; margin: 0 0 6px; }
  h2 { font-size: 16px; margin: 28px 0 8px; }
  h3 { font-size: 14px; margin: 14px 0 4px; color: #43506a; }
  .meta { color: #5a6270; font-size: 13px; margin: 0 0 18px; line-height: 1.8; }
  .meta b { color: #1d2027; }
  table { border-collapse: collapse; width: 100%; background: #fff; font-size: 13px; box-shadow: 0 1px 2px rgba(16, 24, 40, 0.08); }
  th, td { border: 1px solid #dfe3ea; padding: 6px 10px; text-align: left; vertical-align: top; }
  th { background: #eef1f6; font-weight: 600; white-space: nowrap; }
  tr.sel { background: #eaf2ff; }
  td.bool { text-align: center; width: 44px; }
  td.num { text-align: right; white-space: nowrap; }
  td.kind { white-space: nowrap; color: #43506a; }
  td.path { font-family: Consolas, "Courier New", monospace; font-size: 12px; color: #43506a; word-break: break-all; }
  ul { margin: 4px 0 0 18px; padding: 0; font-size: 13px; color: #43506a; }
  .empty { color: #7a8291; font-size: 13px; }
</style>
</head>
<body>
)html";

constexpr const char* kReportHtmlFoot = R"html(</body>
</html>
)html";

const char* kindLabel(ItemKind kind) {
    switch (kind) {
        case ItemKind::Folder:
            return "文件夹";
        case ItemKind::Temp:
            return "临时";
        case ItemKind::File:
        default:
            return "文件";
    }
}

void writeReportHtml(std::ostream& stream,
                     const CleanerState& state,
                     const std::vector<std::size_t>& order,
                     int daysOld,
                     const std::string& xmlFileName) {
    int selectedCount = 0;
    std::uint64_t selectedBytes = 0;
    for (const CleanupItem& item : state.items) {
        if (item.selected) {
            ++selectedCount;
            selectedBytes += item.bytes;
        }
    }

    stream << kReportHtmlHead;
    stream << "<h1>LocalAppData 清理清单</h1>\n";
    stream << "<p class=\"meta\">"
           << "生成时间 <b>" << xmlEscape(formatTimestamp(nowSeconds())) << "</b>"
           << " &#183; 扫描范围 <b>" << xmlEscape(pathToUtf8(localAppDataRoot())) << "</b>"
           << " &#183; 时间阈值 <b>" << daysOld << " 天</b>"
           << " &#183; 共 <b>" << state.items.size() << "</b> 项"
           << "（已勾选 <b>" << selectedCount << "</b> 项 / <b>" << formatBytes(selectedBytes) << "</b>）"
           << " &#183; 合计 <b>" << formatBytes(state.totalBytes) << "</b>";
    if (!xmlFileName.empty()) {
        stream << "<br>原始数据：" << xmlEscape(xmlFileName) << "（同目录，便于脚本处理）";
    }
    stream << "</p>\n";

    stream << "<table>\n<thead>\n<tr>"
           << "<th>勾选</th><th>类型</th><th>名称</th><th>大小</th><th>修改时间</th><th>完整路径</th>"
           << "</tr>\n</thead>\n<tbody>\n";

    if (order.empty()) {
        stream << "<tr><td class=\"empty\" colspan=\"6\">没有条目</td></tr>\n";
    }
    for (const std::size_t index : order) {
        if (index >= state.items.size()) {
            continue;
        }
        const CleanupItem& item = state.items[index];
        stream << "<tr" << (item.selected ? " class=\"sel\"" : "") << ">";
        stream << "<td class=\"bool\">" << (item.selected ? "&#10003;" : "&#8211;") << "</td>";
        stream << "<td class=\"kind\">" << kindLabel(item.kind) << "</td>";
        stream << "<td>" << xmlEscape(item.name) << "</td>";
        stream << "<td class=\"num\">" << xmlEscape(formatBytes(item.bytes)) << "</td>";
        stream << "<td>" << xmlEscape(item.modifiedText) << "</td>";
        stream << "<td class=\"path\">" << xmlEscape(pathToUtf8(item.path)) << "</td>";
        stream << "</tr>\n";
    }
    stream << "</tbody>\n</table>\n";

    stream << "<h2>当前生效的排除规则</h2>\n";
    stream << "<h3>排除目录（目录名精确匹配，任意层级）</h3>\n<ul>\n";
    if (state.excludes.directories.empty()) {
        stream << "<li class=\"empty\">（无）</li>\n";
    }
    for (const std::string& directory : state.excludes.directories) {
        stream << "<li>" << xmlEscape(directory) << "</li>\n";
    }
    stream << "</ul>\n";
    stream << "<h3>排除关键词（名称包含即跳过）</h3>\n<ul>\n";
    if (state.excludes.keywords.empty()) {
        stream << "<li class=\"empty\">（无）</li>\n";
    }
    for (const std::string& keyword : state.excludes.keywords) {
        stream << "<li>" << xmlEscape(keyword) << "</li>\n";
    }
    stream << "</ul>\n";

    stream << kReportHtmlFoot;
}

struct ExportOutcome {
    bool ok = false;
    bool cancelled = false;
    std::string path;
    std::string stylePath;
    std::size_t count = 0;
    std::string error;
};

void writeReportXml(std::ostream& stream,
                    const CleanerState& state,
                    const std::vector<std::size_t>& order,
                    int daysOld) {
    int selectedCount = 0;
    std::uint64_t selectedBytes = 0;
    for (const CleanupItem& item : state.items) {
        if (item.selected) {
            ++selectedCount;
            selectedBytes += item.bytes;
        }
    }

    stream << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    stream << "<cleanupReport"
           << " generated=\"" << xmlEscape(formatTimestamp(nowSeconds())) << "\""
           << " scope=\"" << xmlEscape(pathToUtf8(localAppDataRoot())) << "\""
           << " thresholdDays=\"" << daysOld << "\""
           << " sortKey=\"" << sortKeyName(state.sortKey) << "\""
           << " sortOrder=\"" << (state.sortDescending ? "descending" : "ascending") << "\""
           << " itemCount=\"" << state.items.size() << "\""
           << " totalBytes=\"" << state.totalBytes << "\""
           << " selectedCount=\"" << selectedCount << "\""
           << " selectedBytes=\"" << selectedBytes << "\">\n";

    stream << "  <excludeRules>\n";
    for (const std::string& directory : state.excludes.directories) {
        stream << "    <excludedDirectory>" << xmlEscape(directory) << "</excludedDirectory>\n";
    }
    for (const std::string& keyword : state.excludes.keywords) {
        stream << "    <excludedKeyword>" << xmlEscape(keyword) << "</excludedKeyword>\n";
    }
    stream << "  </excludeRules>\n";

    stream << "  <items>\n";
    for (const std::size_t index : order) {
        if (index >= state.items.size()) {
            continue;
        }
        const CleanupItem& item = state.items[index];
        stream << "    <item kind=\"" << kindName(item.kind) << "\""
               << " selected=\"" << (item.selected ? "true" : "false") << "\""
               << " size=\"" << xmlEscape(formatBytes(item.bytes)) << "\""
               << " bytes=\"" << item.bytes << "\""
               << " modified=\"" << xmlEscape(item.modifiedText) << "\">\n";
        stream << "      <name>" << xmlEscape(item.name) << "</name>\n";
        stream << "      <path>" << xmlEscape(pathToUtf8(item.path)) << "</path>\n";
        stream << "      <location>" << xmlEscape(item.detail) << "</location>\n";
        stream << "    </item>\n";
    }
    stream << "  </items>\n";
    stream << "</cleanupReport>\n";
}

// ---------------------------------------------------------------------------
// 后台：扫描与删除
// ---------------------------------------------------------------------------

std::uint64_t measurePath(const fs::path& target, const app::async::CancelToken& token) {
    std::error_code ec;
    if (!fs::is_directory(target, ec)) {
        const auto size = fs::file_size(target, ec);
        return ec ? 0u : static_cast<std::uint64_t>(size);
    }

    std::uint64_t total = 0;
    std::error_code iterError;
    auto iterator = fs::recursive_directory_iterator(target, fs::directory_options::skip_permission_denied, iterError);
    const fs::recursive_directory_iterator end;
    while (iterator != end) {
        if (token.canceled()) {
            break;
        }

        std::error_code entryError;
        const fs::directory_entry& entry = *iterator;
        if (entry.is_regular_file(entryError) && !entryError) {
            const auto size = entry.file_size(entryError);
            if (!entryError) {
                total += static_cast<std::uint64_t>(size);
            }
        }

        iterator.increment(iterError);
        if (iterError) {
            break;
        }
    }
    return total;
}

// 收集更新残留：匹配关键字的目录 + 散落的安装包。
// 命中目录后不再向内递归；被排除的目录整棵跳过，因此文件只需检查自身名字。
void collectUpdaterRemnants(const fs::path& root,
                            const fs::file_time_type& cutoff,
                            const ExcludeConfig& excludes,
                            const app::async::CancelToken& token,
                            std::vector<CleanupItem>& folders,
                            std::vector<CleanupItem>& files) {
    struct Frame {
        fs::path path;
        int depth = 0;
    };

    std::vector<Frame> stack;
    stack.push_back({root, 0});

    while (!stack.empty()) {
        if (token.canceled()) {
            return;
        }

        const Frame frame = stack.back();
        stack.pop_back();

        std::error_code iterError;
        auto iterator = fs::directory_iterator(frame.path, fs::directory_options::skip_permission_denied, iterError);
        const fs::directory_iterator end;
        while (iterator != end) {
            std::error_code entryError;
            const fs::directory_entry& entry = *iterator;
            const int depth = frame.depth + 1;
            const std::string name = pathToUtf8(entry.path().filename());
            const std::string lowerName = asciiLower(name);
            const fs::file_time_type writeTime = entry.last_write_time(entryError);
            const bool isDirectory = entry.is_directory(entryError);
            const bool excluded = nameExcluded(excludes, lowerName, isDirectory);

            if (!entryError && !excluded) {
                if (isDirectory) {
                    if (matchesFolderRule(lowerName) && depth <= kFolderScanDepth && writeTime < cutoff) {
                        CleanupItem item;
                        item.path = entry.path();
                        item.name = name;
                        item.detail = pathToUtf8(entry.path().parent_path());
                        item.kind = ItemKind::Folder;
                        item.bytes = measurePath(entry.path(), token);
                        item.selected = false;
                        fillItemMetadata(item, writeTime);
                        folders.push_back(std::move(item));
                        iterator.increment(iterError);
                        if (iterError) {
                            break;
                        }
                        continue;
                    }
                    if (depth < kFileScanDepth) {
                        stack.push_back({entry.path(), depth});
                    }
                } else if (matchesFileRule(lowerName) && depth <= kFileScanDepth && writeTime < cutoff) {
                    const auto size = entry.file_size(entryError);
                    if (!entryError) {
                        CleanupItem item;
                        item.path = entry.path();
                        item.name = name;
                        item.detail = pathToUtf8(entry.path().parent_path());
                        item.kind = ItemKind::File;
                        item.bytes = static_cast<std::uint64_t>(size);
                        item.selected = false;
                        fillItemMetadata(item, writeTime);
                        files.push_back(std::move(item));
                    }
                }
            }

            iterator.increment(iterError);
            if (iterError) {
                break;
            }
        }
    }
}

// %LOCALAPPDATA%\Temp：只统计超过阈值的顶层条目（含其下全部内容），同样受排除规则约束。
bool collectTempEntry(const fs::path& root,
                      const fs::file_time_type& cutoff,
                      const ExcludeConfig& excludes,
                      const app::async::CancelToken& token,
                      int daysOld,
                      CleanupItem& out) {
    const fs::path tempDir = root / "Temp";
    std::error_code ec;
    if (!fs::is_directory(tempDir, ec)) {
        return false;
    }

    std::uint64_t bytes = 0;
    std::size_t count = 0;

    std::error_code iterError;
    auto iterator = fs::directory_iterator(tempDir, fs::directory_options::skip_permission_denied, iterError);
    const fs::directory_iterator end;
    while (iterator != end) {
        if (token.canceled()) {
            break;
        }

        std::error_code entryError;
        const fs::directory_entry& entry = *iterator;
        const std::string lowerName = asciiLower(pathToUtf8(entry.path().filename()));
        const bool isDirectory = entry.is_directory(entryError);
        const fs::file_time_type writeTime = entry.last_write_time(entryError);
        if (!entryError && writeTime < cutoff && !nameExcluded(excludes, lowerName, isDirectory)) {
            bytes += measurePath(entry.path(), token);
            ++count;
        }

        iterator.increment(iterError);
        if (iterError) {
            break;
        }
    }

    if (bytes == 0) {
        return false;
    }

    std::error_code timeError;
    const fs::file_time_type tempWriteTime = fs::last_write_time(tempDir, timeError);

    out.path = tempDir;
    out.name = "临时文件夹（Temp）";
    out.detail = "%LOCALAPPDATA%\\Temp · 仅清理 " + std::to_string(daysOld) + " 天前且未被排除的 " +
                 std::to_string(count) + " 个条目";
    out.kind = ItemKind::Temp;
    out.bytes = bytes;
    out.selected = false;
    fillItemMetadata(out, timeError ? cutoff : tempWriteTime);
    return true;
}

ScanOutcome runScan(int daysOld, const ExcludeConfig& excludes, const app::async::CancelToken& token) {
    ScanOutcome outcome;

    const fs::path root = localAppDataRoot();
    std::error_code ec;
    if (root.empty() || !fs::is_directory(root, ec)) {
        outcome.error = "没有找到 %LOCALAPPDATA% 目录。";
        return outcome;
    }

    const fs::file_time_type cutoff = cutoffTime(daysOld);

    std::vector<CleanupItem> folders;
    std::vector<CleanupItem> files;
    collectUpdaterRemnants(root, cutoff, excludes, token, folders, files);

    if (token.canceled()) {
        return outcome;
    }

    for (CleanupItem& item : folders) {
        outcome.bytes += item.bytes;
        outcome.items.push_back(std::move(item));
    }
    for (CleanupItem& item : files) {
        outcome.bytes += item.bytes;
        outcome.items.push_back(std::move(item));
    }

    CleanupItem tempItem;
    if (collectTempEntry(root, cutoff, excludes, token, daysOld, tempItem)) {
        outcome.bytes += tempItem.bytes;
        outcome.items.push_back(std::move(tempItem));
    }

    return outcome;
}

CleanOutcome runClean(const std::vector<CleanupItem>& targets,
                      int daysOld,
                      const ExcludeConfig& excludes,
                      const app::async::CancelToken& token) {
    CleanOutcome outcome;
    const fs::file_time_type cutoff = cutoffTime(daysOld);

    for (const CleanupItem& item : targets) {
        if (token.canceled()) {
            break;
        }

        if (item.kind == ItemKind::Temp) {
            std::error_code iterError;
            auto iterator = fs::directory_iterator(item.path, fs::directory_options::skip_permission_denied, iterError);
            const fs::directory_iterator end;
            while (iterator != end) {
                if (token.canceled()) {
                    break;
                }

                const fs::directory_entry entry = *iterator;
                std::error_code entryError;
                const std::string lowerName = asciiLower(pathToUtf8(entry.path().filename()));
                const bool isDirectory = entry.is_directory(entryError);
                const fs::file_time_type writeTime = entry.last_write_time(entryError);
                const bool stale = !entryError && writeTime < cutoff && !nameExcluded(excludes, lowerName, isDirectory);

                // 先推进迭代器，再删除，避免迭代器悬空。
                iterator.increment(iterError);
                if (iterError) {
                    break;
                }

                if (!stale) {
                    continue;
                }

                const std::uint64_t bytes = measurePath(entry.path(), token);
                std::error_code removeError;
                fs::remove_all(entry.path(), removeError);
                if (removeError) {
                    ++outcome.failed;
                    outcome.failures.push_back(pathToUtf8(entry.path()) + " - " + removeError.message());
                } else {
                    ++outcome.removed;
                    outcome.freed += bytes;
                }
            }
            continue;
        }

        const std::uint64_t bytes = measurePath(item.path, token);
        std::error_code removeError;
        fs::remove_all(item.path, removeError);
        if (removeError) {
            ++outcome.failed;
            outcome.failures.push_back(pathToUtf8(item.path) + " - " + removeError.message());
        } else {
            ++outcome.removed;
            outcome.freed += bytes;
        }
    }

    return outcome;
}

// ---------------------------------------------------------------------------
// 动作
// ---------------------------------------------------------------------------

void showToast(CleanerState* state, std::string title, std::string message) {
    if (state == nullptr) {
        return;
    }
    state->toastTitle = std::move(title);
    state->toastMessage = std::move(message);
    state->toastVisible = true;
}

void persistSettings(const CleanerState* state) {
    if (state == nullptr) {
        return;
    }
    saveSettings(state->excludes, state->lastExportDirectory);
}

void startScan(CleanerState* state) {
    if (state == nullptr || state->phase == Phase::Cleaning) {
        return;
    }

    const int daysOld = thresholdDays(*state);
    const ExcludeConfig excludes = state->excludes;

    state->phase = Phase::Scanning;
    state->items.clear();
    state->totalBytes = 0;
    state->confirmOpen = false;
    state->resultOpen = false;
    state->scannedThresholdIndex = state->thresholdIndex;
    state->scannedRulesVersion = state->rulesVersion;

    app::async::restart(
        "cleaner.scan",
        [daysOld, excludes](const app::async::CancelToken& token) {
            return runScan(daysOld, excludes, token);
        },
        [state, daysOld](const app::async::Result<ScanOutcome>& result) {
            if (!result.ok || !result.value.error.empty()) {
                state->phase = Phase::Idle;
                showToast(state, "扫描失败",
                          result.ok ? result.value.error : std::string("扫描未能完成。"));
                app::requestUpdate();
                return;
            }

            state->items = result.value.items;
            state->totalBytes = result.value.bytes;
            state->phase = Phase::Ready;

            if (state->items.empty()) {
                showToast(state, "没有需要清理的项目",
                          "没有发现超过 " + std::to_string(daysOld) + " 天的更新残留；Temp 目录里也没有陈旧条目。");
            } else {
                showToast(state, "扫描完成",
                          "共发现 " + std::to_string(state->items.size()) + " 项，合计 " +
                              formatBytes(state->totalBytes) + "。默认未勾选，请逐项确认。");
            }
            app::requestUpdate();
        });
}

void cancelScan(CleanerState* state) {
    if (state == nullptr) {
        return;
    }
    app::async::cancel("cleaner.scan");
    state->phase = Phase::Idle;
    state->scannedThresholdIndex = state->thresholdIndex;
    state->scannedRulesVersion = state->rulesVersion;
    showToast(state, "已取消扫描", "可以随时重新开始扫描。");
}

void startClean(CleanerState* state) {
    if (state == nullptr || state->phase == Phase::Cleaning || state->phase == Phase::Scanning) {
        return;
    }

    std::vector<CleanupItem> targets;
    for (const CleanupItem& item : state->items) {
        if (item.selected) {
            targets.push_back(item);
        }
    }
    if (targets.empty()) {
        return;
    }

    const int daysOld = thresholdDays(*state);
    const ExcludeConfig excludes = state->excludes;
    state->phase = Phase::Cleaning;
    state->confirmOpen = false;

    app::async::restart(
        "cleaner.clean",
        [targets, daysOld, excludes](const app::async::CancelToken& token) {
            return runClean(targets, daysOld, excludes, token);
        },
        [state](const app::async::Result<CleanOutcome>& result) {
            state->phase = Phase::Ready;
            if (result.ok) {
                state->report = result.value;
            } else {
                state->report = CleanOutcome{};
                state->report.failed = 1;
                state->report.error = result.error.empty() ? "清理未能完成。" : result.error;
            }
            state->resultOpen = true;
            app::requestUpdate();
        });
}

// 打开条目所在目录：文件打开其父目录，目录/Temp 打开自身。
void openFolder(CleanerState* state, const std::string& utf8Path, ItemKind kind) {
    const fs::path target = (kind == ItemKind::File)
                                ? fs::u8path(utf8Path).parent_path()
                                : fs::u8path(utf8Path);

    std::error_code ec;
    const std::string text = pathToUtf8(target);
    if (!fs::is_directory(target, ec)) {
        showToast(state, "无法打开目录", "目录已经不存在：" + text);
        return;
    }
    if (!eui::platform::openUrl(text)) {
        showToast(state, "无法打开目录", text);
    }
}

bool pushUnique(std::vector<std::string>& values, const std::string& entry) {
    if (entry.empty()) {
        return false;
    }
    if (std::find(values.begin(), values.end(), entry) != values.end()) {
        return false;
    }
    values.push_back(entry);
    return true;
}

void addExcludeDirectoryFromText(CleanerState* state) {
    if (state == nullptr) {
        return;
    }
    const std::string entry = asciiLower(trimAscii(state->excludeDirDraft));
    state->excludeDirDraft.clear();
    if (entry.empty()) {
        return;
    }
    if (!pushUnique(state->excludes.directories, entry)) {
        showToast(state, "无需添加", "「" + entry + "」已经在排除目录里了。");
        return;
    }
    ++state->rulesVersion;
    persistSettings(state);
}

void browseExcludeDirectory(CleanerState* state) {
    if (state == nullptr) {
        return;
    }

    const fs::path root = localAppDataRoot();
    const std::string initial = root.empty() ? std::string() : pathToUtf8(root);
    const DialogResult result = chooseFolderDialog(initial);
    if (result.cancelled) {
        return;
    }
    if (!result.ok) {
        showToast(state, "选择文件夹失败", result.error);
        return;
    }

    // 排除规则按目录名匹配，因此这里只取选中目录的最后一段。
    const fs::path selected = fs::u8path(result.path);
    std::string name = pathToUtf8(selected.filename());
    if (name.empty()) {
        name = pathToUtf8(selected.parent_path().filename());
    }
    const std::string entry = asciiLower(trimAscii(name));
    if (entry.empty()) {
        showToast(state, "无法使用该目录", "请选择一个具体的文件夹：" + result.path);
        return;
    }

    if (!pushUnique(state->excludes.directories, entry)) {
        showToast(state, "无需添加", "目录名「" + entry + "」已经在排除列表里了。");
        return;
    }

    ++state->rulesVersion;
    persistSettings(state);
    showToast(state, "已添加排除目录", entry + "（" + result.path + "）");
}

void removeExcludeDirectory(CleanerState* state, std::size_t index) {
    if (state == nullptr || index >= state->excludes.directories.size()) {
        return;
    }
    state->excludes.directories.erase(state->excludes.directories.begin() + static_cast<std::ptrdiff_t>(index));
    ++state->rulesVersion;
    persistSettings(state);
}

void addExcludeKeyword(CleanerState* state) {
    if (state == nullptr) {
        return;
    }
    const std::string entry = asciiLower(trimAscii(state->excludeKeywordDraft));
    state->excludeKeywordDraft.clear();
    if (entry.empty()) {
        return;
    }
    if (!pushUnique(state->excludes.keywords, entry)) {
        showToast(state, "无需添加", "「" + entry + "」已经在排除关键词里了。");
        return;
    }
    ++state->rulesVersion;
    persistSettings(state);
}

void removeExcludeKeyword(CleanerState* state, std::size_t index) {
    if (state == nullptr || index >= state->excludes.keywords.size()) {
        return;
    }
    state->excludes.keywords.erase(state->excludes.keywords.begin() + static_cast<std::ptrdiff_t>(index));
    ++state->rulesVersion;
    persistSettings(state);
}

void restoreDefaultExcludes(CleanerState* state) {
    if (state == nullptr) {
        return;
    }
    state->excludes = defaultExcludeConfig();
    ++state->rulesVersion;
    persistSettings(state);
    showToast(state, "已恢复默认规则", "排除目录和关键词都回到内置默认值。");
}

void runExport(CleanerState* state) {
    if (state == nullptr || state->items.empty()) {
        return;
    }

    const std::string suggested = "localappdata_cleanup_report_" + fileStamp(nowSeconds()) + ".html";
    const DialogResult dialog = saveFileDialog(suggested, state->lastExportDirectory, "清理报告", {"html", "xml"});
    if (dialog.cancelled) {
        return;
    }
    if (!dialog.ok) {
        showToast(state, "导出失败", dialog.error);
        return;
    }

    // 用户可能选了 .html 也可能选了 .xml，统一按"去掉已知后缀"的基名处理，
    // 然后两个文件一起写：HTML 自带内联样式，双击即可阅读；XML 是纯数据。
    fs::path base = fs::u8path(dialog.path);
    const std::string extension = asciiLower(pathToUtf8(base.extension()));
    if (extension == ".html" || extension == ".htm" || extension == ".xml") {
        base.replace_extension();
    }
    const std::string baseText = pathToUtf8(base);
    const fs::path htmlFile = fs::u8path(baseText + ".html");
    const fs::path xmlFile = fs::u8path(baseText + ".xml");

    const std::vector<std::size_t> order = buildOrder(*state);
    const int daysOld = thresholdDays(*state);
    const std::string xmlName = pathToUtf8(xmlFile.filename());

    std::ofstream htmlStream(htmlFile, std::ios::binary | std::ios::trunc);
    if (!htmlStream) {
        showToast(state, "导出失败", "无法写入 " + pathToUtf8(htmlFile));
        return;
    }
    writeReportHtml(htmlStream, *state, order, daysOld, xmlName);
    htmlStream.close();
    if (!htmlStream) {
        showToast(state, "导出失败", "写入 HTML 报告时出错。");
        return;
    }

    bool xmlWritten = false;
    {
        std::ofstream xmlStream(xmlFile, std::ios::binary | std::ios::trunc);
        if (xmlStream) {
            writeReportXml(xmlStream, *state, order, daysOld);
            xmlStream.close();
            xmlWritten = static_cast<bool>(xmlStream);
        }
    }

    state->lastExportPath = pathToUtf8(htmlFile);
    state->lastExportDirectory = pathToUtf8(htmlFile.parent_path());
    persistSettings(state);

    showToast(state, "已导出清单",
              "报告 " + pathToUtf8(htmlFile.filename()) +
                  (xmlWritten ? "（数据 " + xmlName + "）" : "（XML 数据写入失败）"));
}

// ---------------------------------------------------------------------------
// 视觉令牌
// ---------------------------------------------------------------------------

Tokens themeColors() {
    Tokens tokens = components::theme::dark();
    tokens.background = {0.055f, 0.062f, 0.078f, 1.0f};
    tokens.surface = {0.098f, 0.106f, 0.132f, 1.0f};
    tokens.surfaceHover = {0.160f, 0.174f, 0.212f, 1.0f};
    tokens.surfaceActive = {0.205f, 0.222f, 0.268f, 1.0f};
    tokens.primary = {0.345f, 0.545f, 0.980f, 1.0f};
    tokens.border = {0.230f, 0.248f, 0.302f, 1.0f};
    tokens.text = {0.945f, 0.960f, 0.992f, 1.0f};
    return tokens;
}

eui::Color mutedText(const Tokens& tokens) {
    return components::theme::withAlpha(tokens.text, 0.64f);
}

eui::Color faintText(const Tokens& tokens) {
    return components::theme::withAlpha(tokens.text, 0.44f);
}

eui::Color warningText() {
    return {0.980f, 0.680f, 0.380f, 1.0f};
}

eui::Transition quickTransition() {
    return eui::Transition::make(0.16f, eui::Ease::OutCubic);
}

struct KindVisual {
    std::string label;
    eui::Color background;
    eui::Color foreground;
};

KindVisual kindVisual(const Tokens& tokens, ItemKind kind) {
    switch (kind) {
        case ItemKind::Folder:
            return {"文件夹",
                    components::theme::withAlpha(tokens.primary, 0.20f),
                    {0.600f, 0.740f, 1.000f, 1.0f}};
        case ItemKind::Temp:
            return {"临时",
                    {0.960f, 0.660f, 0.220f, 0.18f},
                    {0.980f, 0.760f, 0.360f, 1.0f}};
        case ItemKind::File:
        default:
            return {"文件",
                    components::theme::withAlpha(tokens.text, 0.10f),
                    components::theme::withAlpha(tokens.text, 0.74f)};
    }
}

// ---------------------------------------------------------------------------
// 页面片段
// ---------------------------------------------------------------------------

void drawHeader(eui::Ui& ui, const Tokens& tokens, const std::string& scopeText, float width, float height) {
    const components::theme::PageVisualTokens page = components::theme::pageVisuals(tokens);

    ui.column("header")
        .size(width, height)
        .gap(6.0f)
        .content([&] {
            components::text(ui, "header.title", tokens)
                .size(width, 38.0f)
                .text("LocalAppData 更新残留清理")
                .fontSize(kFontTitle)
                .lineHeight(36.0f)
                .color(page.titleColor)
                .build();

            components::text(ui, "header.subtitle", tokens)
                .size(width, 34.0f)
                .text(scopeText)
                .fontSize(kFontSubtitle)
                .lineHeight(18.0f)
                .maxWidth(width)
                .wrap(true)
                .color(page.subtitleColor)
                .build();
        })
        .build();
}

void drawToolbarRow(eui::Ui& ui,
                    const Tokens& tokens,
                    CleanerState* state,
                    float width,
                    float height,
                    bool hasItems,
                    bool busy,
                    int selectedCount) {
    const bool scanning = state->phase == Phase::Scanning;
    const bool allSelected = hasItems && selectedCount == static_cast<int>(state->items.size());
    const std::size_t ruleCount = state->excludes.directories.size() + state->excludes.keywords.size();

    ui.row("toolbar")
        .size(width, height)
        .gap(12.0f)
        .alignItems(eui::Align::CENTER)
        .content([&] {
            if (scanning) {
                components::button(ui, "toolbar.scan")
                    .theme(tokens, false)
                    .size(152.0f, 44.0f)
                    .text("取消扫描")
                    .icon(0xF00D)
                    .transition(quickTransition())
                    .onClick([state] { cancelScan(state); })
                    .build();
            } else {
                components::button(ui, "toolbar.scan")
                    .theme(tokens, true)
                    .size(152.0f, 44.0f)
                    .text("开始扫描")
                    .icon(0xF002)
                    .transition(quickTransition())
                    .disabled(state->phase == Phase::Cleaning)
                    .onClick([state] { startScan(state); })
                    .build();
            }

            components::text(ui, "toolbar.threshold.label", tokens)
                .size(74.0f, height)
                .text("时间阈值")
                .fontSize(kFontLabel)
                .lineHeight(20.0f)
                .color(mutedText(tokens))
                .verticalAlign(eui::VerticalAlign::Center)
                .build();

            std::vector<std::string> labels;
            labels.reserve(static_cast<std::size_t>(kThresholdCount));
            for (const char* label : kThresholdLabels) {
                labels.emplace_back(label);
            }

            components::segmented(ui, "toolbar.threshold")
                .theme(tokens)
                .size(300.0f, 38.0f)
                .items(std::move(labels))
                .selected(state->thresholdIndex)
                .transition(quickTransition())
                .onChange([state](int index) { state->thresholdIndex = index; })
                .build();

            components::button(ui, "toolbar.select")
                .theme(tokens, false)
                .size(108.0f, 44.0f)
                .text(allSelected ? "取消全选" : "全选")
                .icon(allSelected ? 0xF00D : 0xF00C)
                .fontSize(15.0f)
                .transition(quickTransition())
                .disabled(!hasItems || busy)
                .onClick([state, allSelected] {
                    for (CleanupItem& item : state->items) {
                        item.selected = !allSelected;
                    }
                })
                .build();

            components::button(ui, "toolbar.invert")
                .theme(tokens, false)
                .size(108.0f, 44.0f)
                .text("反选")
                .icon(0xF074)
                .fontSize(15.0f)
                .transition(quickTransition())
                .disabled(!hasItems || busy)
                .onClick([state] {
                    for (CleanupItem& item : state->items) {
                        item.selected = !item.selected;
                    }
                })
                .build();

            components::button(ui, "toolbar.excludes")
                .theme(tokens, false)
                .size(136.0f, 44.0f)
                .text("排除规则 " + std::to_string(ruleCount))
                .icon(0xF05E)
                .fontSize(15.0f)
                .transition(quickTransition())
                .disabled(state->phase == Phase::Cleaning)
                .onClick([state] { state->excludesOpen = true; })
                .build();
        })
        .build();
}

void drawSortRow(eui::Ui& ui,
                 const Tokens& tokens,
                 CleanerState* state,
                 float width,
                 float height,
                 bool hasItems,
                 bool staleInputs) {
    const bool descending = state->sortDescending;

    ui.row("sortbar")
        .size(width, height)
        .gap(12.0f)
        .alignItems(eui::Align::CENTER)
        .content([&] {
            components::text(ui, "sortbar.label", tokens)
                .size(44.0f, height)
                .text("排序")
                .fontSize(kFontLabel)
                .lineHeight(20.0f)
                .color(mutedText(tokens))
                .verticalAlign(eui::VerticalAlign::Center)
                .build();

            components::segmented(ui, "sortbar.key")
                .theme(tokens)
                .size(288.0f, 36.0f)
                .items({"名称", "日期", "类型", "大小"})
                .selected(state->sortKey)
                .transition(quickTransition())
                .onChange([state](int index) { state->sortKey = index; })
                .build();

            components::button(ui, "sortbar.direction")
                .theme(tokens, false)
                .size(112.0f, 38.0f)
                .text(descending ? "降序" : "升序")
                .icon(descending ? 0xF0D8 : 0xF0D7)
                .fontSize(15.0f)
                .transition(quickTransition())
                .disabled(!hasItems)
                .onClick([state] { state->sortDescending = !state->sortDescending; })
                .build();

            const std::string hint = staleInputs
                                         ? "阈值或排除规则已更改，请重新点击「开始扫描」"
                                         : "扫描结果默认全部不勾选，逐项确认后再清理";

            components::text(ui, "sortbar.hint", tokens)
                .size(std::max(0.0f, width - 44.0f - 288.0f - 112.0f - 36.0f), height)
                .text(hint)
                .fontSize(kFontStatsSmall)
                .lineHeight(18.0f)
                .color(staleInputs ? warningText() : faintText(tokens))
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
        })
        .build();
}

void centerNotice(eui::Ui& ui,
                  const Tokens& tokens,
                  float width,
                  float height,
                  unsigned int iconCodepoint,
                  const eui::Color& iconColor,
                  const std::string& title,
                  const std::string& subtitle) {
    const float iconSize = 42.0f;

    ui.column("notice")
        .size(width, height)
        .gap(10.0f)
        .justifyContent(eui::Align::CENTER)
        .alignItems(eui::Align::CENTER)
        .content([&] {
            ui.text("notice.icon")
                .size(iconSize, iconSize)
                .icon(iconCodepoint)
                .fontSize(iconSize)
                .lineHeight(iconSize)
                .color(iconColor)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .build();

            ui.text("notice.title")
                .size(width, 28.0f)
                .text(title)
                .fontSize(kFontNoticeTitle)
                .lineHeight(25.0f)
                .color(tokens.text)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .build();

            if (!subtitle.empty()) {
                ui.text("notice.subtitle")
                    .size(std::min(width, 620.0f), 62.0f)
                    .text(subtitle)
                    .fontSize(kFontNoticeBody)
                    .lineHeight(20.0f)
                    .maxWidth(std::min(width, 620.0f))
                    .wrap(true)
                    .color(mutedText(tokens))
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .build();
            }
        })
        .build();
}

void toggleSelection(CleanerState* state, std::size_t index, bool value) {
    if (state == nullptr || index >= state->items.size()) {
        return;
    }
    state->items[index].selected = value;
}

void drawCleanupRow(eui::Ui& ui,
                    const Tokens& tokens,
                    const CleanupItem& item,
                    CleanerState* state,
                    std::size_t index,
                    float width,
                    float height) {
    const std::string rowId = "list.row." + std::to_string(index);
    const bool checked = item.selected;
    const KindVisual kind = kindVisual(tokens, item.kind);

    const float checkWidth = 46.0f;
    const float badgeWidth = 66.0f;
    const float sizeWidth = 104.0f;
    const float openWidth = 34.0f;
    const float innerGap = 10.0f;
    const float textWidth = std::max(120.0f, width - checkWidth - badgeWidth - sizeWidth - openWidth -
                                                 innerGap * 4.0f);

    const std::string detailText = item.modifiedText + " · " + item.detail;
    const std::string openPath = pathToUtf8(item.path);

    ui.stack(rowId)
        .size(width, height)
        .content([&] {
            ui.rect(rowId + ".bg")
                .fill()
                .states(checked ? components::theme::withAlpha(tokens.primary, 0.14f)
                                : components::theme::withAlpha(tokens.text, 0.035f),
                        tokens.surfaceHover,
                        tokens.surfaceActive)
                .radius(10.0f)
                .transition(quickTransition())
                .onClick([state, index, checked] { toggleSelection(state, index, !checked); })
                .build();

            ui.row(rowId + ".content")
                .fill()
                .gap(innerGap)
                .alignItems(eui::Align::CENTER)
                .content([&] {
                    components::checkbox(ui, rowId + ".check")
                        .theme(tokens)
                        .size(checkWidth, 32.0f)
                        .checked(checked)
                        .transition(quickTransition())
                        .onChange([state, index](bool value) { toggleSelection(state, index, value); })
                        .build();

                    ui.stack(rowId + ".badge")
                        .size(badgeWidth, 26.0f)
                        .content([&] {
                            ui.rect(rowId + ".badge.bg")
                                .fill()
                                .color(kind.background)
                                .radius(7.0f)
                                .build();

                            ui.text(rowId + ".badge.text")
                                .fill()
                                .text(kind.label)
                                .fontSize(kFontBadge)
                                .lineHeight(17.0f)
                                .color(kind.foreground)
                                .horizontalAlign(eui::HorizontalAlign::Center)
                                .verticalAlign(eui::VerticalAlign::Center)
                                .build();
                        })
                        .build();

                    ui.column(rowId + ".info")
                        .size(textWidth, height)
                        .gap(3.0f)
                        .justifyContent(eui::Align::CENTER)
                        .content([&] {
                            ui.text(rowId + ".name")
                                .size(textWidth, 22.0f)
                                .text(elideText(item.name, kFontRowName, textWidth))
                                .fontSize(kFontRowName)
                                .lineHeight(22.0f)
                                .color(tokens.text)
                                .build();

                            ui.text(rowId + ".detail")
                                .size(textWidth, 19.0f)
                                .text(elideText(detailText, kFontRowDetail, textWidth))
                                .fontSize(kFontRowDetail)
                                .lineHeight(18.0f)
                                .color(faintText(tokens))
                                .build();
                        })
                        .build();

                    ui.text(rowId + ".size")
                        .size(sizeWidth, height)
                        .text(formatBytes(item.bytes))
                        .fontSize(kFontRowSize)
                        .lineHeight(20.0f)
                        .horizontalAlign(eui::HorizontalAlign::Right)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(checked ? tokens.text : mutedText(tokens))
                        .build();

                    ui.stack(rowId + ".open")
                        .size(openWidth, openWidth)
                        .content([&] {
                            ui.rect(rowId + ".open.bg")
                                .fill()
                                .states(components::theme::color(0.0f, 0.0f, 0.0f, 0.0f),
                                        tokens.surfaceHover,
                                        tokens.surfaceActive)
                                .radius(8.0f)
                                .transition(quickTransition())
                                .onClick([state, openPath, kind = item.kind] {
                                    openFolder(state, openPath, kind);
                                })
                                .build();

                            ui.image(rowId + ".open.icon")
                                .x(7.0f)
                                .y(7.0f)
                                .size(20.0f, 20.0f)
                                .source("folder.png")
                                .contain()
                                .build();
                        })
                        .build();
                })
                .build();
        })
        .build();
}

void drawList(eui::Ui& ui,
              const Tokens& tokens,
              CleanerState* state,
              const std::vector<std::size_t>& order,
              float width,
              float height,
              int daysOld) {
    ui.stack("list")
        .size(width, height)
        .content([&] {
            ui.rect("list.bg")
                .fill()
                .color(tokens.surface)
                .radius(14.0f)
                .border(1.0f, components::theme::withOpacity(tokens.border, 0.85f))
                .build();

            const float innerWidth = std::max(0.0f, width - 24.0f);
            const float innerHeight = std::max(0.0f, height - 24.0f);

            ui.stack("list.viewport")
                .x(12.0f)
                .y(12.0f)
                .size(innerWidth, innerHeight)
                .content([&] {
                    if (state->phase == Phase::Idle) {
                        centerNotice(ui, tokens, innerWidth, innerHeight,
                                     0xF002,
                                     components::theme::withAlpha(tokens.primary, 0.75f),
                                     "尚未扫描",
                                     "点击左上角的「开始扫描」，检查 %LOCALAPPDATA% 下的更新残留与临时文件。");
                        return;
                    }

                    if (state->phase == Phase::Scanning) {
                        centerNotice(ui, tokens, innerWidth, innerHeight,
                                     0xF021,
                                     components::theme::withAlpha(tokens.primary, 0.85f),
                                     "正在扫描…",
                                     "正在统计残留目录与临时文件的大小，文件较多时需要几秒钟。");
                        return;
                    }

                    if (state->items.empty()) {
                        centerNotice(ui, tokens, innerWidth, innerHeight,
                                     0xF058,
                                     {0.360f, 0.780f, 0.520f, 1.0f},
                                     "未发现可清理的更新残留",
                                     "当前阈值下没有匹配的更新/安装目录或安装包，Temp 目录中也没有陈旧条目。\n"
                                     "排除规则已生效，可在「排除规则」里检查。");
                        return;
                    }

                    components::scrollView(ui, "list.scroll")
                        .size(innerWidth, innerHeight)
                        .gap(kRowGap)
                        .step(48.0f)
                        .contentKey(std::to_string(state->items.size()) + "#" + std::to_string(daysOld))
                        .transition(quickTransition())
                        .content([&](eui::Ui& rowUi, float rowWidth, float) {
                            for (std::size_t position = 0; position < order.size(); ++position) {
                                const std::size_t index = order[position];
                                drawCleanupRow(rowUi, tokens, state->items[index], state, index, rowWidth, kRowHeight);
                            }
                        })
                        .build();
                })
                .build();
        })
        .build();
}

void drawFooter(eui::Ui& ui,
                const Tokens& tokens,
                CleanerState* state,
                float width,
                float height,
                bool hasItems,
                int selectedCount,
                std::uint64_t selectedBytes) {
    const bool busy = state->phase == Phase::Scanning || state->phase == Phase::Cleaning;
    const bool hasExport = !state->lastExportPath.empty();

    const float cleanWidth = 288.0f;
    const float exportWidth = 140.0f;
    const float openWidth = 34.0f;
    const float gaps = 12.0f * (hasExport ? 3.0f : 2.0f);
    const float openTotal = hasExport ? openWidth + 12.0f : 0.0f;
    const float statsWidth = std::max(0.0f, width - cleanWidth - exportWidth - openTotal - gaps);

    std::string headline;
    std::string subline;

    if (state->phase == Phase::Cleaning) {
        headline = "正在清理…";
        subline = "删除过程中请不要关闭窗口。";
    } else if (state->phase == Phase::Scanning) {
        headline = "正在扫描…";
        subline = "扫描完成后所有项目默认都是未勾选状态。";
    } else if (hasItems) {
        headline = "已选 " + std::to_string(selectedCount) + " / " + std::to_string(state->items.size()) +
                   " 项 · 预计释放 " + formatBytes(selectedBytes);
        subline = hasExport
                      ? "上次导出：" + elideText(state->lastExportPath, kFontStatsSmall, std::max(80.0f, statsWidth - 90.0f))
                      : "扫描结果合计 " + formatBytes(state->totalBytes) + " · 删除后无法恢复";
    } else {
        headline = "尚未扫描";
        subline = "选择时间阈值后点击「开始扫描」。";
    }

    ui.row("footer")
        .size(width, height)
        .gap(12.0f)
        .alignItems(eui::Align::CENTER)
        .content([&] {
            ui.column("footer.stats")
                .size(statsWidth, height)
                .gap(3.0f)
                .justifyContent(eui::Align::CENTER)
                .content([&] {
                    ui.text("footer.stats.headline")
                        .size(statsWidth, 26.0f)
                        .text(headline)
                        .fontSize(kFontStats)
                        .lineHeight(24.0f)
                        .color(tokens.text)
                        .build();

                    ui.text("footer.stats.subline")
                        .size(statsWidth, 19.0f)
                        .text(subline)
                        .fontSize(kFontStatsSmall)
                        .lineHeight(18.0f)
                        .color(faintText(tokens))
                        .build();
                })
                .build();

            components::button(ui, "footer.export")
                .theme(tokens, false)
                .size(exportWidth, 44.0f)
                .text("导出清单")
                .icon(0xF019)
                .fontSize(15.0f)
                .transition(quickTransition())
                .disabled(!hasItems || busy)
                .onClick([state] { runExport(state); })
                .build();

            if (hasExport) {
                const std::string folder = pathToUtf8(fs::u8path(state->lastExportPath).parent_path());

                ui.stack("footer.export.open")
                    .size(openWidth, openWidth)
                    .content([&] {
                        ui.rect("footer.export.open.bg")
                            .fill()
                            .states(components::theme::color(0.0f, 0.0f, 0.0f, 0.0f),
                                    tokens.surfaceHover,
                                    tokens.surfaceActive)
                            .radius(8.0f)
                            .transition(quickTransition())
                            .onClick([state, folder] { openFolder(state, folder, ItemKind::Folder); })
                            .build();

                        ui.image("footer.export.open.icon")
                            .x(7.0f)
                            .y(7.0f)
                            .size(20.0f, 20.0f)
                            .source("folder.png")
                            .contain()
                            .build();
                    })
                    .build();
            }

            components::button(ui, "footer.clean")
                .theme(tokens, true)
                .size(cleanWidth, 46.0f)
                .text(selectedCount > 0 ? "清理 " + std::to_string(selectedCount) + " 项 · " + formatBytes(selectedBytes)
                                        : std::string("清理选中项"))
                .icon(0xF2ED)
                .fontSize(15.0f)
                .transition(quickTransition())
                .disabled(!hasItems || selectedCount == 0 || busy)
                .onClick([state] { state->confirmOpen = true; })
                .build();
        })
        .build();
}

// ---------------------------------------------------------------------------
// 确认 / 排除规则 / 进行中 / 结果 / 提示
// ---------------------------------------------------------------------------

void drawConfirmRow(eui::Ui& ui,
                    const Tokens& tokens,
                    const CleanupItem& item,
                    int index,
                    float width,
                    float height) {
    const std::string rowId = "confirm.list.row." + std::to_string(index);
    const float sizeWidth = 96.0f;
    const float textWidth = std::max(80.0f, width - sizeWidth - 8.0f);

    const std::string label = item.kind == ItemKind::Temp ? item.name : pathToUtf8(item.path);

    ui.row(rowId)
        .size(width, height)
        .gap(8.0f)
        .alignItems(eui::Align::CENTER)
        .content([&] {
            ui.text(rowId + ".name")
                .size(textWidth, height)
                .text(elideText(label, 13.5f, textWidth))
                .fontSize(13.5f)
                .lineHeight(19.0f)
                .color(tokens.text)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();

            ui.text(rowId + ".size")
                .size(sizeWidth, height)
                .text(formatBytes(item.bytes))
                .fontSize(12.5f)
                .lineHeight(18.0f)
                .color(mutedText(tokens))
                .horizontalAlign(eui::HorizontalAlign::Right)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
        })
        .build();
}

void drawConfirmDialog(eui::Ui& ui,
                       const Tokens& tokens,
                       CleanerState* state,
                       const std::vector<std::size_t>& order,
                       float screenWidth,
                       float screenHeight,
                       int selectedCount,
                       std::uint64_t selectedBytes) {
    constexpr float kPanelWidth = 580.0f;
    constexpr float kPanelHeight = 470.0f;
    constexpr float kInset = 24.0f;

    const float contentWidth = kPanelWidth - kInset * 2.0f;
    const float listTop = 108.0f;
    const float listHeight = kPanelHeight - listTop - 78.0f;
    const float buttonWidth = 152.0f;
    const float buttonHeight = 44.0f;
    const float buttonY = kPanelHeight - 22.0f - buttonHeight;

    components::dialog(ui, "confirm")
        .open(state->confirmOpen)
        .screen(screenWidth, screenHeight)
        .theme(tokens)
        .size(kPanelWidth, kPanelHeight)
        .transition(quickTransition())
        .onOpenChange([state](bool open) { state->confirmOpen = open; })
        .content([&] {
            ui.text("confirm.title")
                .x(kInset)
                .y(22.0f)
                .size(contentWidth, 32.0f)
                .text("确认清理以下文件")
                .fontSize(22.0f)
                .lineHeight(29.0f)
                .color(tokens.text)
                .build();

            ui.text("confirm.warning")
                .x(kInset)
                .y(60.0f)
                .size(contentWidth, 42.0f)
                .text("共 " + std::to_string(selectedCount) + " 项，预计释放 " + formatBytes(selectedBytes) +
                      "。删除后不可恢复，请确认没有正在运行的更新程序。")
                .fontSize(13.5f)
                .lineHeight(20.0f)
                .maxWidth(contentWidth)
                .wrap(true)
                .color(warningText())
                .build();

            ui.stack("confirm.list.frame")
                .x(kInset)
                .y(listTop)
                .size(contentWidth, listHeight)
                .content([&] {
                    ui.rect("confirm.list.bg")
                        .fill()
                        .color(tokens.background)
                        .radius(10.0f)
                        .border(1.0f, components::theme::withOpacity(tokens.border, 0.9f))
                        .build();

                    components::scrollView(ui, "confirm.list")
                        .x(10.0f)
                        .y(10.0f)
                        .size(std::max(0.0f, contentWidth - 20.0f), std::max(0.0f, listHeight - 20.0f))
                        .gap(4.0f)
                        .step(40.0f)
                        .contentKey("confirm#" + std::to_string(selectedCount))
                        .transition(quickTransition())
                        .content([&](eui::Ui& rowUi, float rowWidth, float) {
                            int rowIndex = 0;
                            for (const std::size_t index : order) {
                                if (index >= state->items.size() || !state->items[index].selected) {
                                    continue;
                                }
                                drawConfirmRow(rowUi, tokens, state->items[index], rowIndex, rowWidth, 32.0f);
                                ++rowIndex;
                            }
                        })
                        .build();
                })
                .build();

            components::button(ui, "confirm.cancel")
                .position(kPanelWidth - kInset - buttonWidth, buttonY)
                .size(buttonWidth, buttonHeight)
                .theme(tokens, false)
                .text("取消")
                .transition(quickTransition())
                .onClick([state] { state->confirmOpen = false; })
                .build();

            components::button(ui, "confirm.ok")
                .position(kPanelWidth - kInset - buttonWidth * 2.0f - 12.0f, buttonY)
                .size(buttonWidth, buttonHeight)
                .theme(tokens, true)
                .text("确认清理")
                .icon(0xF2ED)
                .transition(quickTransition())
                .onClick([state] { startClean(state); })
                .build();
        })
        .build();
}

// 排除列表里的一行：目录用 isDirectory = true。
void drawExcludeRow(eui::Ui& ui,
                    const Tokens& tokens,
                    const std::string& entry,
                    CleanerState* state,
                    bool isDirectory,
                    std::size_t index,
                    float width,
                    float height) {
    const std::string rowId = std::string(isDirectory ? "exclude.dir.row." : "exclude.key.row.") +
                              std::to_string(index);
    const float removeWidth = 30.0f;
    const float textWidth = std::max(80.0f, width - removeWidth - 10.0f);

    ui.row(rowId)
        .size(width, height)
        .gap(10.0f)
        .alignItems(eui::Align::CENTER)
        .content([&] {
            ui.text(rowId + ".text")
                .size(textWidth, height)
                .text(elideText(entry, 13.5f, textWidth))
                .fontSize(13.5f)
                .lineHeight(19.0f)
                .color(tokens.text)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();

            components::button(ui, rowId + ".remove")
                .theme(tokens, false)
                .size(removeWidth, 26.0f)
                .icon(0xF00D)
                .fontSize(12.0f)
                .transition(quickTransition())
                .onClick([state, isDirectory, index] {
                    if (isDirectory) {
                        removeExcludeDirectory(state, index);
                    } else {
                        removeExcludeKeyword(state, index);
                    }
                })
                .build();
        })
        .build();
}

void drawExcludeList(eui::Ui& ui,
                     const Tokens& tokens,
                     CleanerState* state,
                     bool isDirectory,
                     float x,
                     float y,
                     float width,
                     float height) {
    const std::vector<std::string>& entries = isDirectory ? state->excludes.directories : state->excludes.keywords;

    ui.stack(std::string(isDirectory ? "exclude.dir.frame" : "exclude.key.frame"))
        .x(x)
        .y(y)
        .size(width, height)
        .content([&] {
            ui.rect(std::string(isDirectory ? "exclude.dir.bg" : "exclude.key.bg"))
                .fill()
                .color(tokens.background)
                .radius(10.0f)
                .border(1.0f, components::theme::withOpacity(tokens.border, 0.9f))
                .build();

            if (entries.empty()) {
                ui.text(std::string(isDirectory ? "exclude.dir.empty" : "exclude.key.empty"))
                    .x(12.0f)
                    .y(10.0f)
                    .size(std::max(0.0f, width - 24.0f), 22.0f)
                    .text(isDirectory ? "暂无排除目录（扫描时不会跳过任何目录名）"
                                      : "暂无排除关键词（扫描时不会按名字过滤）")
                    .fontSize(12.5f)
                    .lineHeight(18.0f)
                    .color(faintText(tokens))
                    .build();
                return;
            }

            components::scrollView(ui, std::string(isDirectory ? "exclude.dir.list" : "exclude.key.list"))
                .x(10.0f)
                .y(10.0f)
                .size(std::max(0.0f, width - 20.0f), std::max(0.0f, height - 20.0f))
                .gap(4.0f)
                .step(40.0f)
                .contentKey(std::string(isDirectory ? "excludedirs#" : "excludekeys#") + std::to_string(entries.size()))
                .transition(quickTransition())
                .content([&](eui::Ui& rowUi, float rowWidth, float) {
                    for (std::size_t index = 0; index < entries.size(); ++index) {
                        drawExcludeRow(rowUi, tokens, entries[index], state, isDirectory, index, rowWidth, 32.0f);
                    }
                })
                .build();
        })
        .build();
}

void drawExcludeDialog(eui::Ui& ui,
                       const Tokens& tokens,
                       CleanerState* state,
                       float screenWidth,
                       float screenHeight) {
    constexpr float kPanelWidth = 720.0f;
    constexpr float kPanelHeight = 680.0f;
    constexpr float kInset = 24.0f;

    const float contentWidth = kPanelWidth - kInset * 2.0f;
    const float browseWidth = 110.0f;
    const float addWidth = 90.0f;
    const float gap = 10.0f;

    const float dirInputWidth = std::max(160.0f, contentWidth - browseWidth - addWidth - gap * 2.0f);
    const float dirInputX = kInset;
    const float browseX = dirInputX + dirInputWidth + gap;
    const float dirAddX = browseX + browseWidth + gap;

    const float keyInputWidth = std::max(160.0f, contentWidth - addWidth - gap);
    const float keyAddX = kInset + keyInputWidth + gap;

    const float buttonWidth = 150.0f;
    const float buttonHeight = 44.0f;
    const float buttonY = kPanelHeight - 20.0f - buttonHeight;

    components::dialog(ui, "excludes")
        .open(state->excludesOpen)
        .screen(screenWidth, screenHeight)
        .theme(tokens)
        .size(kPanelWidth, kPanelHeight)
        .transition(quickTransition())
        .onOpenChange([state](bool open) { state->excludesOpen = open; })
        .content([&] {
            ui.text("exclude.title")
                .x(kInset)
                .y(20.0f)
                .size(contentWidth, 30.0f)
                .text("排除规则")
                .fontSize(22.0f)
                .lineHeight(28.0f)
                .color(tokens.text)
                .build();

            ui.text("exclude.note")
                .x(kInset)
                .y(54.0f)
                .size(contentWidth, 50.0f)
                .text("排除目录按「目录名精确匹配」跳过整棵子树；排除关键词只要名称包含就跳过，两者都不区分大小写、"
                      "作用于任何层级，改动后需要重新扫描。")
                .fontSize(12.5f)
                .lineHeight(17.0f)
                .maxWidth(contentWidth)
                .wrap(true)
                .color(faintText(tokens))
                .build();

            ui.text("exclude.dir.label")
                .x(kInset)
                .y(110.0f)
                .size(contentWidth, 22.0f)
                .text("排除目录（" + std::to_string(state->excludes.directories.size()) + " 条）")
                .fontSize(kFontLabel)
                .lineHeight(20.0f)
                .color(mutedText(tokens))
                .build();

            components::input(ui, "exclude.dir.input")
                .theme(tokens)
                .position(dirInputX, 136.0f)
                .size(dirInputWidth, 40.0f)
                .value(state->excludeDirDraft)
                .placeholder("直接输入目录名，例如 cache")
                .fontSize(14.0f)
                .transition(quickTransition())
                .onChange([state](const std::string& value) { state->excludeDirDraft = value; })
                .build();

            components::button(ui, "exclude.dir.browse")
                .position(browseX, 136.0f)
                .size(browseWidth, 40.0f)
                .theme(tokens, false)
                .text("浏览…")
                .icon(0xF07C)
                .fontSize(14.0f)
                .transition(quickTransition())
                .onClick([state] { browseExcludeDirectory(state); })
                .build();

            components::button(ui, "exclude.dir.add")
                .position(dirAddX, 136.0f)
                .size(addWidth, 40.0f)
                .theme(tokens, true)
                .text("添加")
                .icon(0xF067)
                .fontSize(14.0f)
                .transition(quickTransition())
                .onClick([state] { addExcludeDirectoryFromText(state); })
                .build();

            drawExcludeList(ui, tokens, state, true, kInset, 184.0f, contentWidth, 138.0f);

            ui.text("exclude.key.label")
                .x(kInset)
                .y(334.0f)
                .size(contentWidth, 22.0f)
                .text("排除关键词（" + std::to_string(state->excludes.keywords.size()) + " 条）")
                .fontSize(kFontLabel)
                .lineHeight(20.0f)
                .color(mutedText(tokens))
                .build();

            components::input(ui, "exclude.key.input")
                .theme(tokens)
                .position(kInset, 360.0f)
                .size(keyInputWidth, 40.0f)
                .value(state->excludeKeywordDraft)
                .placeholder("例如 site-packages、node_modules")
                .fontSize(14.0f)
                .transition(quickTransition())
                .onChange([state](const std::string& value) { state->excludeKeywordDraft = value; })
                .build();

            components::button(ui, "exclude.key.add")
                .position(keyAddX, 360.0f)
                .size(addWidth, 40.0f)
                .theme(tokens, true)
                .text("添加")
                .icon(0xF067)
                .fontSize(14.0f)
                .transition(quickTransition())
                .onClick([state] { addExcludeKeyword(state); })
                .build();

            drawExcludeList(ui, tokens, state, false, kInset, 408.0f, contentWidth, 138.0f);

            if (!state->settingsError.empty()) {
                ui.text("exclude.settings.error")
                    .x(kInset)
                    .y(556.0f)
                    .size(contentWidth, 40.0f)
                    .text(state->settingsError)
                    .fontSize(12.5f)
                    .lineHeight(18.0f)
                    .maxWidth(contentWidth)
                    .wrap(true)
                    .color(warningText())
                    .build();
            }

            components::button(ui, "exclude.restore")
                .position(kInset, buttonY)
                .size(buttonWidth, buttonHeight)
                .theme(tokens, false)
                .text("恢复默认规则")
                .icon(0xF0E2)
                .fontSize(14.0f)
                .transition(quickTransition())
                .onClick([state] { restoreDefaultExcludes(state); })
                .build();

            components::button(ui, "exclude.close")
                .position(kPanelWidth - kInset - 140.0f, buttonY)
                .size(140.0f, buttonHeight)
                .theme(tokens, true)
                .text("完成")
                .icon(0xF00C)
                .transition(quickTransition())
                .onClick([state] { state->excludesOpen = false; })
                .build();
        })
        .build();
}

void drawWorkingDialog(eui::Ui& ui, const Tokens& tokens, CleanerState* state, float screenWidth, float screenHeight) {
    constexpr float kPanelWidth = 380.0f;
    constexpr float kPanelHeight = 176.0f;

    components::dialog(ui, "working")
        .open(state->phase == Phase::Cleaning)
        .screen(screenWidth, screenHeight)
        .theme(tokens)
        .size(kPanelWidth, kPanelHeight)
        .transition(quickTransition())
        .onOpenChange([](bool) {})
        .content([&] {
            ui.text("working.icon")
                .x(24.0f)
                .y(28.0f)
                .size(30.0f, 30.0f)
                .icon(0xF021)
                .fontSize(29.0f)
                .lineHeight(29.0f)
                .color(components::theme::withAlpha(tokens.primary, 0.9f))
                .build();

            ui.text("working.title")
                .x(66.0f)
                .y(28.0f)
                .size(kPanelWidth - 90.0f, 32.0f)
                .text("正在清理…")
                .fontSize(20.0f)
                .lineHeight(27.0f)
                .color(tokens.text)
                .build();

            ui.text("working.message")
                .x(24.0f)
                .y(74.0f)
                .size(kPanelWidth - 48.0f, 70.0f)
                .text("正在删除已确认的项目，文件被占用时会自动跳过，请不要关闭窗口。")
                .fontSize(13.5f)
                .lineHeight(20.0f)
                .maxWidth(kPanelWidth - 48.0f)
                .wrap(true)
                .color(mutedText(tokens))
                .build();
        })
        .build();
}

void drawResultDialog(eui::Ui& ui, const Tokens& tokens, CleanerState* state, float screenWidth, float screenHeight) {
    constexpr float kPanelWidth = 580.0f;
    constexpr float kPanelHeight = 350.0f;
    constexpr float kInset = 24.0f;

    const CleanOutcome& report = state->report;
    const bool hasFailure = report.failed > 0 || !report.error.empty();
    const float contentWidth = kPanelWidth - kInset * 2.0f;
    const float buttonWidth = 152.0f;
    const float buttonHeight = 44.0f;
    const float buttonY = kPanelHeight - 22.0f - buttonHeight;

    const std::string title = hasFailure ? "清理完成（有项目未能删除）" : "清理完成";

    std::string summary;
    if (!report.error.empty()) {
        summary = report.error;
    } else {
        summary = "已删除 " + std::to_string(report.removed) + " 项，释放 " + formatBytes(report.freed) + " 空间。";
        if (hasFailure) {
            summary += "另有 " + std::to_string(report.failed) + " 项被占用或无权限，未能删除。";
        }
    }

    components::dialog(ui, "result")
        .open(state->resultOpen)
        .screen(screenWidth, screenHeight)
        .theme(tokens)
        .size(kPanelWidth, kPanelHeight)
        .transition(quickTransition())
        .onOpenChange([state](bool open) { state->resultOpen = open; })
        .content([&] {
            ui.text("result.icon")
                .x(kInset)
                .y(26.0f)
                .size(32.0f, 32.0f)
                .icon(hasFailure ? 0xF06A : 0xF058)
                .fontSize(30.0f)
                .lineHeight(30.0f)
                .color(hasFailure ? warningText() : eui::Color{0.360f, 0.800f, 0.540f, 1.0f})
                .build();

            ui.text("result.title")
                .x(kInset + 44.0f)
                .y(26.0f)
                .size(contentWidth - 44.0f, 34.0f)
                .text(title)
                .fontSize(21.0f)
                .lineHeight(28.0f)
                .color(tokens.text)
                .build();

            ui.text("result.summary")
                .x(kInset)
                .y(74.0f)
                .size(contentWidth, 42.0f)
                .text(summary)
                .fontSize(14.0f)
                .lineHeight(21.0f)
                .maxWidth(contentWidth)
                .wrap(true)
                .color(mutedText(tokens))
                .build();

            if (!report.failures.empty()) {
                const int shown = std::min<int>(4, static_cast<int>(report.failures.size()));
                const std::string firstFailure = elideText(report.failures.front(), 12.5f, contentWidth);
                std::string failureText = "未能删除（可能被占用）：" + firstFailure;
                if (static_cast<int>(report.failures.size()) > shown) {
                    failureText += " 等 " + std::to_string(report.failures.size()) + " 项";
                }

                ui.text("result.failures")
                    .x(kInset)
                    .y(126.0f)
                    .size(contentWidth, 62.0f)
                    .text(failureText)
                    .fontSize(12.5f)
                    .lineHeight(19.0f)
                    .maxWidth(contentWidth)
                    .wrap(true)
                    .color(faintText(tokens))
                    .build();
            }

            ui.text("result.hint")
                .x(kInset)
                .y(202.0f)
                .size(contentWidth, 38.0f)
                .text("关闭后会自动重新扫描，确认清理结果。")
                .fontSize(12.5f)
                .lineHeight(18.0f)
                .maxWidth(contentWidth)
                .wrap(true)
                .color(faintText(tokens))
                .build();

            components::button(ui, "result.ok")
                .position(kPanelWidth - kInset - buttonWidth, buttonY)
                .size(buttonWidth, buttonHeight)
                .theme(tokens, true)
                .text("完成")
                .icon(0xF00C)
                .transition(quickTransition())
                .onClick([state] {
                    state->resultOpen = false;
                    startScan(state);
                })
                .build();
        })
        .build();
}

void drawToast(eui::Ui& ui, const Tokens& tokens, CleanerState* state, float screenWidth, float screenHeight) {
    components::toast(ui, "toast")
        .visible(state->toastVisible)
        .screen(screenWidth, screenHeight)
        .theme(tokens)
        .size(420.0f, 108.0f)
        .title(state->toastTitle)
        .message(state->toastMessage)
        .icon(0xF058)
        .duration(4.0f)
        .transition(quickTransition())
        .onDismiss([state] { state->toastVisible = false; })
        .onAutoDismiss([state] { state->toastVisible = false; })
        .build();
}

} // namespace

// ---------------------------------------------------------------------------
// App 入口
// ---------------------------------------------------------------------------

const DslAppConfig& dslAppConfig() {
    static const DslAppConfig config = DslAppConfig{}
        .title("LocalAppData 更新残留清理")
        .pageId("localappdata_cleaner")
        .clearColor({0.055f, 0.062f, 0.078f, 1.0f})
        .windowSize(1235, 960)
        // 窗口图标取自可执行文件旁的 assets/；exe 自身的图标和版本信息由 CMake 的 .rc 资源提供。
        .iconPath("assets/19icon.png")
        .fps(90.0);
    return config;
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    const Tokens tokens = themeColors();
    CleanerState* state = &ui.state<CleanerState>("cleaner");

    // 排除规则只在首次 compose 时从配置文件读入，之后以内存中的状态为准。
    if (!state->settingsLoaded) {
        state->settingsLoaded = true;
        ExcludeConfig loaded;
        std::string lastDirectory;
        std::string error;
        if (loadSettings(loaded, lastDirectory, error)) {
            state->excludes = loaded;
            state->lastExportDirectory = lastDirectory;
            state->settingsError.clear();
        } else if (!error.empty()) {
            state->settingsError = error;
        }
    }

    const float width = screen.width;
    const float height = screen.height;
    const float pad = 26.0f;
    const float gap = 14.0f;
    const float contentWidth = std::max(360.0f, width - pad * 2.0f);
    const float headerHeight = 78.0f;
    const float toolbarHeight = 46.0f;
    const float sortHeight = 42.0f;
    const float footerHeight = 64.0f;
    const float listHeight = std::max(160.0f,
                                      height - pad * 2.0f - headerHeight - toolbarHeight - sortHeight -
                                          footerHeight - gap * 4.0f);

    const int daysOld = thresholdDays(*state);
    const bool staleInputs =
        state->scannedThresholdIndex >= 0 &&
        (state->scannedThresholdIndex != state->thresholdIndex || state->scannedRulesVersion != state->rulesVersion);
    const std::vector<std::size_t> order = buildOrder(*state);

    int selectedCount = 0;
    std::uint64_t selectedBytes = 0;
    for (const CleanupItem& item : state->items) {
        if (item.selected) {
            ++selectedCount;
            selectedBytes += item.bytes;
        }
    }

    const bool hasItems = !state->items.empty();
    const bool busy = state->phase == Phase::Scanning || state->phase == Phase::Cleaning;

    const fs::path root = localAppDataRoot();
    const std::string scopeText =
        std::string("扫描范围 ") + (root.empty() ? std::string("(未找到 %LOCALAPPDATA%)") : pathToUtf8(root)) +
        " · 匹配 updater / installer / pending 目录，以及 update*/installer*/setup*.exe、*.msi、*.nupkg，"
        "附带 Temp 陈旧条目";

    ui.stack("root")
        .size(width, height)
        .content([&] {
            ui.rect("root.bg")
                .fill()
                .color(tokens.background)
                .build();

            ui.column("page")
                .size(width, height)
                .padding(pad)
                .gap(gap)
                .content([&] {
                    drawHeader(ui, tokens, scopeText, contentWidth, headerHeight);
                    drawToolbarRow(ui, tokens, state, contentWidth, toolbarHeight, hasItems, busy, selectedCount);
                    drawSortRow(ui, tokens, state, contentWidth, sortHeight, hasItems, staleInputs);
                    drawList(ui, tokens, state, order, contentWidth, listHeight, daysOld);
                    drawFooter(ui, tokens, state, contentWidth, footerHeight, hasItems, selectedCount, selectedBytes);
                })
                .build();

            drawConfirmDialog(ui, tokens, state, order, width, height, selectedCount, selectedBytes);
            drawExcludeDialog(ui, tokens, state, width, height);
            drawWorkingDialog(ui, tokens, state, width, height);
            drawResultDialog(ui, tokens, state, width, height);
            drawToast(ui, tokens, state, width, height);
        })
        .build();
}

} // namespace app
