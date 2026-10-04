// Win32 child processes (platform/Process.h): CreateProcessW without a console window, UTF-8
// pipes read by one thread per stream, and a job object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
// so that the whole process tree ends with its owner (also when the tool itself crashes).
#include "platform/Process.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include <windows.h>

namespace oe {
namespace {

// Lines waiting for ReadLine before the reader threads stop reading (back-pressure on the child).
constexpr size_t kMaxQueuedLines = 4096;

std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring NativePath(const std::string& path) {
    std::wstring w = Widen(path);
    std::replace(w.begin(), w.end(), L'/', L'\\');
    return w;
}

std::wstring EnvironmentValue(const wchar_t* name) {
    DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (!length) return std::wstring();
    std::wstring value(length, L'\0');
    DWORD copied = GetEnvironmentVariableW(name, value.data(), length);
    if (!copied || copied >= length) return std::wstring();
    value.resize(copied);
    return value;
}

std::vector<std::wstring> Split(const std::wstring& text, wchar_t separator) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(separator, start);
        if (end == std::wstring::npos) end = text.size();
        if (end > start) parts.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

bool IsFile(const std::wstring& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring FullPath(const std::wstring& path) {
    DWORD length = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!length) return path;
    std::wstring full(length, L'\0');
    DWORD copied = GetFullPathNameW(path.c_str(), length, full.data(), nullptr);
    if (!copied || copied >= length) return path;
    full.resize(copied);
    return full;
}

// Extension of the file name including the dot, lower case; empty when there is none.
std::wstring Extension(const std::wstring& path) {
    size_t name = path.find_last_of(L"\\/:");
    size_t dot = path.rfind(L'.');
    if (dot == std::wstring::npos || (name != std::wstring::npos && dot < name)) return std::wstring();
    std::wstring extension = path.substr(dot);
    for (wchar_t& c : extension) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return extension;
}

std::string WindowsError(const std::string& what) { return what + " (Windows error " + std::to_string(GetLastError()) + ")"; }

// Quoting understood by CommandLineToArgvW and the C runtime: backslashes are literal unless
// they precede a quote, where each is doubled.
void AppendArgument(std::wstring& line, const std::wstring& argument, bool alwaysQuote) {
    if (!line.empty()) line += L' ';
    if (!alwaysQuote && !argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        line += argument;
        return;
    }
    line += L'"';
    size_t backslashes = 0;
    for (wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        line.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        backslashes = 0;
        line += c;
    }
    line.append(backslashes * 2, L'\\');
    line += L'"';
}

// cmd.exe expands %NAME% and (with delayed expansion) !NAME! even inside quotes, and a quote
// would end the quoting that keeps & | < > ^ literal.
bool SafeForCommandScript(const std::string& text) {
    for (char c : text) {
        if (c == '"' || c == '%' || c == '!' || static_cast<unsigned char>(c) < 0x20) return false;
    }
    return true;
}

// The parent's environment with `additions` applied, as the sorted double-null block
// CreateProcessW expects (CREATE_UNICODE_ENVIRONMENT).
std::vector<wchar_t> EnvironmentBlock(const std::vector<std::pair<std::string, std::string>>& additions) {
    auto nameOf = [](const std::wstring& entry) { return entry.substr(0, entry.find(L'=', 1)); };  // "=C:=C:\dir" entries start with '='
    auto sameName = [](const std::wstring& a, const std::wstring& b) {
        return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
    };
    std::vector<std::wstring> entries;
    if (wchar_t* strings = GetEnvironmentStringsW()) {
        for (const wchar_t* p = strings; *p; p += wcslen(p) + 1) entries.emplace_back(p);
        FreeEnvironmentStringsW(strings);
    }
    for (const auto& addition : additions) {
        const std::wstring name = Widen(addition.first);
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const std::wstring& e) { return sameName(nameOf(e), name); }), entries.end());
        entries.push_back(name + L"=" + Widen(addition.second));
    }
    std::stable_sort(entries.begin(), entries.end(), [&](const std::wstring& a, const std::wstring& b) {
        const std::wstring x = nameOf(a), y = nameOf(b);
        return CompareStringOrdinal(x.data(), static_cast<int>(x.size()), y.data(), static_cast<int>(y.size()), TRUE) == CSTR_LESS_THAN;
    });
    std::vector<wchar_t> block;
    for (const std::wstring& entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { Close(); }
    void Close() {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = nullptr;
    }
    HANDLE Release() {
        HANDLE released = value;
        value = nullptr;
        return released;
    }
};

class Win32Process final : public Process {
public:
    // Takes ownership of the handles; stdinWrite may be null.
    Win32Process(HANDLE process, HANDLE job, HANDLE stdinWrite, HANDLE stdoutRead, HANDLE stderrRead)
        : process_(process), job_(job), stdinWrite_(stdinWrite) {
        pipes_[0] = stdoutRead;
        pipes_[1] = stderrRead;
        for (int i = 0; i < 2; ++i) {
            finished_[i] = false;
            readers_[i] = std::thread([this, i] { ReadPipe(i); });
        }
    }

    ~Win32Process() override {
        Kill();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        space_.notify_all();
        CloseInput();
        // A process that escaped the job could still hold the pipes open: cancel the blocking
        // reads until both threads have seen `stopping_`.
        while (!finished_[0] || !finished_[1]) {
            CancelIoEx(pipes_[0], nullptr);
            CancelIoEx(pipes_[1], nullptr);
            Sleep(1);
        }
        for (std::thread& reader : readers_) reader.join();
        CloseHandle(pipes_[0]);
        CloseHandle(pipes_[1]);
        CloseHandle(process_);
        CloseHandle(job_);
    }

    bool Write(const std::string& bytes, std::string* error) override {
        if (!stdinWrite_) {
            if (error) *error = "the process input is closed";
            return false;
        }
        size_t done = 0;
        while (done < bytes.size()) {
            DWORD written = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - done, 65536));
            if (!WriteFile(stdinWrite_, bytes.data() + done, chunk, &written, nullptr)) {
                if (error) *error = WindowsError("cannot write to the process input");
                return false;
            }
            done += written;
        }
        return true;
    }

    void CloseInput() override {
        if (stdinWrite_) CloseHandle(stdinWrite_);
        stdinWrite_ = nullptr;
    }

    bool ReadLine(std::string& line, bool& isStderr) override {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this] { return !lines_.empty() || openPipes_ == 0; });
        if (lines_.empty()) return false;
        line = std::move(lines_.front().text);
        isStderr = lines_.front().isStderr;
        lines_.pop_front();
        lock.unlock();
        space_.notify_one();
        return true;
    }

    bool Running(int* exitCode) override {
        if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) return true;
        DWORD code = 0;
        GetExitCodeProcess(process_, &code);
        if (exitCode) *exitCode = static_cast<int>(code);
        return false;
    }

    void Kill() override {
        TerminateJobObject(job_, 1);
        WaitForSingleObject(process_, 2000);  // termination is asynchronous; Running() is false afterwards
    }

private:
    struct Line {
        std::string text;
        bool isStderr = false;
    };

    // False once the owner is being destroyed (the line is dropped).
    bool Push(std::string& text, bool isStderr) {
        if (!text.empty() && text.back() == '\r') text.pop_back();
        std::unique_lock<std::mutex> lock(mutex_);
        space_.wait(lock, [this] { return lines_.size() < kMaxQueuedLines || stopping_; });
        if (stopping_) return false;
        lines_.push_back({std::move(text), isStderr});
        text.clear();
        lock.unlock();
        ready_.notify_one();
        return true;
    }

    bool Stopping() {
        std::lock_guard<std::mutex> lock(mutex_);
        return stopping_;
    }

    void ReadPipe(int index) {
        std::string line;
        char buffer[4096];
        bool open = true;
        while (open) {
            DWORD read = 0;
            if (!ReadFile(pipes_[index], buffer, sizeof(buffer), &read, nullptr)) {
                if (GetLastError() == ERROR_OPERATION_ABORTED && !Stopping()) continue;
                break;  // ERROR_BROKEN_PIPE: every process holding the write end has exited
            }
            for (DWORD i = 0; i < read && open; ++i) {
                if (buffer[i] == '\n') open = Push(line, index == 1);
                else if (line.size() < kMaxProcessLineBytes) line += buffer[i];
            }
        }
        if (open && !line.empty()) Push(line, index == 1);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --openPipes_;
        }
        ready_.notify_all();
        finished_[index] = true;
    }

    HANDLE process_ = nullptr, job_ = nullptr, stdinWrite_ = nullptr;
    HANDLE pipes_[2] = {nullptr, nullptr};  // stdout, stderr (read ends)
    std::thread readers_[2];
    std::atomic<bool> finished_[2];
    std::mutex mutex_;
    std::condition_variable ready_, space_;
    std::deque<Line> lines_;
    int openPipes_ = 2;
    bool stopping_ = false;
};

// Creates a pipe whose `inherited` end goes to the child; the other end stays in this process.
bool CreateChildPipe(Handle& read, Handle& write, bool childReads) {
    SECURITY_ATTRIBUTES attributes = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    if (!CreatePipe(&read.value, &write.value, &attributes, 0)) return false;
    return SetHandleInformation(childReads ? write.value : read.value, HANDLE_FLAG_INHERIT, 0) != 0;
}

}  // namespace

bool PlatformProcessSupported() { return true; }

std::string PlatformFindExecutable(const std::string& name) {
    if (name.empty() || name.find('\0') != std::string::npos) return std::string();
    const std::wstring wide = NativePath(name);
    std::vector<std::wstring> extensions;
    if (!Extension(wide).empty()) extensions.push_back(std::wstring());  // "tool.exe" as given, then "tool.v2" + PATHEXT
    std::wstring pathExt = EnvironmentValue(L"PATHEXT");
    if (pathExt.empty()) pathExt = L".COM;.EXE;.BAT;.CMD";
    for (std::wstring& extension : Split(pathExt, L';')) extensions.push_back(std::move(extension));
    std::vector<std::wstring> directories;
    if (wide.find_first_of(L"\\:") != std::wstring::npos) {
        directories.push_back(std::wstring());
    } else {
        for (std::wstring& directory : Split(EnvironmentValue(L"PATH"), L';')) {
            directory.erase(std::remove(directory.begin(), directory.end(), L'"'), directory.end());
            if (directory.empty()) continue;
            if (directory.back() != L'\\') directory += L'\\';
            directories.push_back(std::move(directory));
        }
    }
    for (const std::wstring& directory : directories) {
        for (const std::wstring& extension : extensions) {
            const std::wstring candidate = directory + wide + extension;
            if (!IsFile(candidate)) continue;
            std::wstring full = FullPath(candidate);
            WIN32_FIND_DATAW data;  // the name as stored on disk ("codex.cmd", not the PATHEXT spelling "codex.CMD")
            HANDLE find = FindFirstFileW(full.c_str(), &data);
            if (find != INVALID_HANDLE_VALUE) {
                FindClose(find);
                full = full.substr(0, full.find_last_of(L'\\') + 1) + data.cFileName;
            }
            std::string found = Narrow(full);
            std::replace(found.begin(), found.end(), '\\', '/');
            return found;
        }
    }
    return std::string();
}

std::unique_ptr<Process> PlatformStartProcess(const ProcessOptions& options, std::string* error) {
    auto fail = [error](const std::string& message) {
        if (error) *error = message;
        return nullptr;
    };
    const std::wstring executable = FullPath(NativePath(options.executable));
    if (options.executable.empty() || !IsFile(executable)) return fail("executable not found: " + options.executable);
    for (const std::string& argument : options.arguments) {
        if (argument.find('\0') != std::string::npos) return fail("a process argument contains a NUL character");
    }
    for (const auto& variable : options.environment) {
        if (variable.first.empty() || variable.first.find_first_of(std::string("=\0", 2)) != std::string::npos ||
            variable.second.find('\0') != std::string::npos) {
            return fail("invalid environment variable name or value: " + variable.first);
        }
    }
    const std::wstring directory = options.workingDirectory.empty() ? std::wstring() : FullPath(NativePath(options.workingDirectory));
    if (!directory.empty() && (GetFileAttributesW(directory.c_str()) == INVALID_FILE_ATTRIBUTES || IsFile(directory))) {
        return fail("working directory not found: " + options.workingDirectory);
    }

    // Command scripts cannot be started by CreateProcessW: run `cmd.exe /d /s /c "<script> <args>"`
    // with every part quoted (/s strips only the outer pair of quotes).
    const std::wstring extension = Extension(executable);
    const bool script = extension == L".cmd" || extension == L".bat";
    std::wstring application = executable, commandLine;
    if (script) {
        if (!SafeForCommandScript(Narrow(executable))) return fail("the command script path contains a character cmd.exe would expand: " + options.executable);
        for (const std::string& argument : options.arguments) {
            if (!SafeForCommandScript(argument)) {
                return fail("an argument of a command script (.cmd/.bat) contains a quote, percent sign, exclamation mark or control "
                            "character; pass such text through stdin or a file");
            }
        }
        wchar_t system[MAX_PATH];
        UINT length = GetSystemDirectoryW(system, MAX_PATH);
        if (!length || length >= MAX_PATH) return fail(WindowsError("cannot locate cmd.exe"));
        application = std::wstring(system, length) + L"\\cmd.exe";
        std::wstring inner;
        AppendArgument(inner, executable, true);
        for (const std::string& argument : options.arguments) AppendArgument(inner, Widen(argument), true);
        AppendArgument(commandLine, application, false);
        commandLine += L" /d /s /c \"" + inner + L"\"";
    } else {
        AppendArgument(commandLine, executable, false);
        for (const std::string& argument : options.arguments) AppendArgument(commandLine, Widen(argument), false);
    }
    if (commandLine.size() > 32766) return fail("the process command line is longer than 32766 characters");

    Handle stdinRead, stdinWrite, stdoutRead, stdoutWrite, stderrRead, stderrWrite;
    if (options.pipeStdin) {
        if (!CreateChildPipe(stdinRead, stdinWrite, true)) return fail(WindowsError("cannot create a pipe"));
    } else {
        SECURITY_ATTRIBUTES attributes = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        stdinRead.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_EXISTING, 0, nullptr);
        if (stdinRead.value == INVALID_HANDLE_VALUE) return fail(WindowsError("cannot open NUL"));
    }
    if (!CreateChildPipe(stdoutRead, stdoutWrite, false) || !CreateChildPipe(stderrRead, stderrWrite, false)) {
        return fail(WindowsError("cannot create a pipe"));
    }

    // Only these three handles are inherited, so children started at the same time from other
    // threads never keep each other's pipes open.
    HANDLE inherited[3] = {stdinRead.value, stdoutWrite.value, stderrWrite.value};
    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<unsigned char> attributeBuffer(attributeSize);
    LPPROC_THREAD_ATTRIBUTE_LIST attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data());
    if (!InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeSize)) return fail(WindowsError("cannot prepare the process"));
    if (!UpdateProcThreadAttribute(attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) {
        const std::string message = WindowsError("cannot prepare the process");
        DeleteProcThreadAttributeList(attributeList);
        return fail(message);
    }

    Handle job;
    job.value = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        const std::string message = WindowsError("cannot create a job object");
        DeleteProcThreadAttributeList(attributeList);
        return fail(message);
    }

    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = stdinRead.value;
    startup.StartupInfo.hStdOutput = stdoutWrite.value;
    startup.StartupInfo.hStdError = stderrWrite.value;
    startup.lpAttributeList = attributeList;
    std::vector<wchar_t> environment;
    if (!options.environment.empty()) environment = EnvironmentBlock(options.environment);
    PROCESS_INFORMATION info = {};
    // Suspended until it is inside the job, so nothing it starts can outlive the job.
    const DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT;
    const BOOL created = CreateProcessW(application.c_str(), commandLine.data(), nullptr, nullptr, TRUE, flags,
                                        environment.empty() ? nullptr : environment.data(), directory.empty() ? nullptr : directory.c_str(),
                                        &startup.StartupInfo, &info);
    const std::string createError = created ? std::string() : WindowsError("cannot start " + options.executable);
    DeleteProcThreadAttributeList(attributeList);
    if (!created) return fail(createError);
    Handle process, thread;
    process.value = info.hProcess;
    thread.value = info.hThread;
    if (!AssignProcessToJobObject(job.value, process.value)) {
        const std::string message = WindowsError("cannot put the process into its job");
        TerminateProcess(process.value, 1);
        return fail(message);
    }
    ResumeThread(thread.value);
    // The child owns its ends now; keeping them here would hide the end of its output.
    stdinRead.Close();
    stdoutWrite.Close();
    stderrWrite.Close();
    return std::make_unique<Win32Process>(process.Release(), job.Release(), stdinWrite.Release(), stdoutRead.Release(), stderrRead.Release());
}

}  // namespace oe
