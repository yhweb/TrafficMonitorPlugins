// DeviceRuntime.cpp - independent connection and sampling state for one plug
#include "pch.h"
#include "DeviceRuntime.h"
#include <filesystem>
#include <filesystem>

DeviceRuntime::DeviceRuntime(const DeviceConfig& config) : m_config(config) {}

DeviceRuntime::~DeviceRuntime() {
    Stop(true);
}

void DeviceRuntime::Start() {
    if (m_thread.joinable()) return;
    m_stop = false;
    LoadHistory();
    m_thread = std::thread(&DeviceRuntime::SampleLoop, this);
}

void DeviceRuntime::Stop(bool waitForThread) {
    m_stop = true;
    if (m_thread.joinable()) {
        if (waitForThread) m_thread.join();
        else m_thread.detach();
    }
    if (waitForThread) SaveHistory();
}

void DeviceRuntime::UpdateConfig(const DeviceConfig& config) {
    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        m_config = config;
    }
    m_pendingRemoval = false;
    ++m_configRevision;
}

void DeviceRuntime::SetSharedSettings(int updateIntervalSec, bool enableRecording) {
    m_updateIntervalSec = (std::max)(1, (std::min)(60, updateIntervalSec));
    m_enableRecording = enableRecording;
}

void DeviceRuntime::MarkPendingRemoval() {
    m_pendingRemoval = true;
    m_connected = false;
    ++m_configRevision;
}

DeviceConfig DeviceRuntime::GetConfig() const {
    std::lock_guard<std::mutex> lock(m_configMutex);
    return m_config;
}

std::string DeviceRuntime::ToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                                     nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                        result.data(), length, nullptr, nullptr);
    return result;
}

namespace {
int MigrateLegacyHistoryFiles(const std::wstring& legacyDirectory, const std::wstring& targetDirectory) {
    std::error_code ec;
    const std::filesystem::path target(targetDirectory);
    std::filesystem::create_directories(target, ec);

    int movedCount = 0;
    WIN32_FIND_DATAW findData{};
    HANDLE find = FindFirstFileW((legacyDirectory + L"\\*.csv").c_str(), &findData);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::filesystem::path source = std::filesystem::path(legacyDirectory) / findData.cFileName;
        const std::filesystem::path destination = target / findData.cFileName;
        if (std::filesystem::exists(destination, ec)) continue;
        if (MoveFileW(source.c_str(), destination.c_str())) ++movedCount;
    } while (FindNextFileW(find, &findData));
    FindClose(find);
    return movedCount;
}
}

void DeviceRuntime::LoadHistory() {
    if (!m_enableRecording.load()) return;
    const auto config = GetConfig();
    const auto path = ConfigManager::Instance().GetHistoryDirectory(config.itemId);
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        m_history.LoadFromFile(path);
        return;
    }

    if (config.itemId == L"MijiaPowerW") {
        // v1 布局（MijiaPower_history\YYYY-MM.csv）一次性搬移到本设备子目录
        const auto legacyPath = ConfigManager::Instance().GetLegacyHistoryDirectory();
        if (GetFileAttributesW(legacyPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (MigrateLegacyHistoryFiles(legacyPath, path) > 0) {
                m_history.LoadFromFile(path);
            }
        }
    }
}

void DeviceRuntime::SaveHistory() const {
    if (!m_enableRecording.load()) return;
    const auto config = GetConfig();
    m_history.SaveToFile(ConfigManager::Instance().GetHistoryDirectory(config.itemId));
}

void DeviceRuntime::SampleLoop() {
    std::unique_ptr<MiioDevice> device;
    unsigned long long activeRevision = 0;
    int elapsedMilliseconds = 1000000;
    ULONGLONG lastHistorySaveTick = GetTickCount64();

    while (!m_stop.load()) {
    try {
        Sleep(100);
        if (m_pendingRemoval.load()) {
            device.reset();
            m_connected = false;
            continue;
        }

        const auto revision = m_configRevision.load();
        if (revision != activeRevision) {
            activeRevision = revision;
            device.reset();
            m_connected = false;
            elapsedMilliseconds = 1000000;
        }

        elapsedMilliseconds += 100;
        const int intervalMilliseconds = m_updateIntervalSec.load() * 1000;
        if (elapsedMilliseconds < intervalMilliseconds) continue;
        elapsedMilliseconds = 0;

        const DeviceConfig config = GetConfig();
        if (config.deviceIp.empty() || config.deviceToken.empty()) {
            device.reset();
            m_connected = false;
            continue;
        }

        try {
            if (!device) {
                device = std::make_unique<MiioDevice>(ToUtf8(config.deviceIp),
                                                       ToUtf8(config.deviceToken), 5000);
            }

            double watts = 0.0;
            if (device->GetPower(watts)) {
                m_currentWatts = watts;
                m_connected = true;
                if (m_enableRecording.load()) m_history.AddSample(watts);
            } else {
                device.reset();
                m_connected = false;
            }
        } catch (...) {
            device.reset();
            m_connected = false;
        }

        // 运行期周期落盘，避免依赖插件卸载时机导致历史丢失
        if (m_enableRecording.load()) {
            const ULONGLONG now = GetTickCount64();
            if (now - lastHistorySaveTick >= 60000) {
                SaveHistory();
                lastHistorySaveTick = now;
            }
        }
    } catch (...) {
        // 任何逃逸异常都不允许终止宿主进程
        device.reset();
        m_connected = false;
    }
    }  // while

    try {
        SaveHistory();
    } catch (...) {
    }
}
