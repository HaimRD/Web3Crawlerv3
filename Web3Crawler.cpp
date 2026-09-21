// Web3Crawler.cpp
// Build output: Web3Crawler.dll
//
// Exports:
//   Crawler_Init
//   Crawler_Start
//   Crawler_Stop
//   Crawler_GetStatusJson
//   Crawler_SearchText
//
// This is a lightweight Web3/domain crawler DLL for AutoPlay Media Studio.
// It reads seeds from data\seeds.txt, crawls HTTP/HTTPS pages, writes index.jsonl,
// and supports basic text search.
//
// Expected files in app data folder:
//   seeds.txt
//   index.jsonl
//   crawler.log
//
// Compile as a Windows DLL.

#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <winhttp.h>

#include <string>
#include <vector>
#include <queue>
#include <set>
#include <fstream>
#include <sstream>
#include <mutex>
#include <thread>
#include <atomic>
#include <algorithm>
#include <ctime>

#pragma comment(lib, "winhttp.lib")

// ------------------------------------------------------------
// Global state
// ------------------------------------------------------------

static std::wstring g_dataFolder;
static std::wstring g_seedsFile;
static std::wstring g_indexFile;
static std::wstring g_logFile;

static std::thread g_worker;
static std::atomic<bool> g_running(false);
static std::atomic<bool> g_stopRequested(false);

static std::mutex g_mutex;

static int g_pagesIndexed = 0;
static int g_pagesFailed = 0;
static int g_queueCount = 0;
static std::string g_lastMessage = "Idle";

static char g_statusBuffer[65536];
static char g_searchBuffer[262144];

// ------------------------------------------------------------
// Small helpers
// ------------------------------------------------------------

extern "C" __declspec(dllexport) long __stdcall Test_AddOne(long value)
{
    return value + 1;
}

static std::wstring Utf8ToWide(const std::string& input)
{
    if (input.empty())
    {
        return L"";
    }

    int sizeNeeded = MultiByteToWideChar(
        CP_UTF8,
        0,
        input.c_str(),
        -1,
        NULL,
        0
    );

    if (sizeNeeded <= 0)
    {
        return L"";
    }

    std::wstring result(sizeNeeded - 1, 0);

    MultiByteToWideChar(
        CP_UTF8,
        0,
        input.c_str(),
        -1,
        &result[0],
        sizeNeeded
    );

    return result;
}


static std::string WideToUtf8(const std::wstring& input)
{
    if (input.empty())
    {
        return "";
    }

    int sizeNeeded = WideCharToMultiByte(
        CP_UTF8,
        0,
        input.c_str(),
        -1,
        NULL,
        0,
        NULL,
        NULL
    );

    if (sizeNeeded <= 0)
    {
        return "";
    }

    std::string result(sizeNeeded - 1, 0);

    WideCharToMultiByte(
        CP_UTF8,
        0,
        input.c_str(),
        -1,
        &result[0],
        sizeNeeded,
        NULL,
        NULL
    );

    return result;
}


static std::string Trim(const std::string& value)
{
    size_t start = 0;
    while (start < value.size() && isspace((unsigned char)value[start]))
    {
        start++;
    }

    size_t end = value.size();
    while (end > start && isspace((unsigned char)value[end - 1]))
    {
        end--;
    }

    return value.substr(start, end - start);
}


static std::string ToLower(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c)
        {
            return (char)std::tolower(c);
        }
    );

    return value;
}


static bool StartsWith(const std::string& value, const std::string& prefix)
{
    if (value.size() < prefix.size())
    {
        return false;
    }

    return value.compare(0, prefix.size(), prefix) == 0;
}


static std::string ReplaceAll(std::string value, const std::string& from, const std::string& to)
{
    if (from.empty())
    {
        return value;
    }

    size_t pos = 0;

    while ((pos = value.find(from, pos)) != std::string::npos)
    {
        value.replace(pos, from.length(), to);
        pos += to.length();
    }

    return value;
}


static std::string JsonEscape(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 32);

    for (char c : value)
    {
        switch (c)
        {
            case '\\':
                out += "\\\\";
                break;

            case '"':
                out += "\\\"";
                break;

            case '\r':
                out += "\\r";
                break;

            case '\n':
                out += "\\n";
                break;

            case '\t':
                out += "\\t";
                break;

            default:
                out += c;
                break;
        }
    }

    return out;
}


static std::string NowText()
{
    std::time_t t = std::time(NULL);
    char buffer[64] = {0};

    tm localTime;
    localtime_s(&localTime, &t);

    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &localTime);

    return std::string(buffer);
}


static void SetLastMessage(const std::string& message)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_lastMessage = message;
}


static void LogLine(const std::string& line)
{
    if (g_logFile.empty())
    {
        return;
    }

    std::ofstream out(g_logFile, std::ios::app | std::ios::binary);

    if (!out)
    {
        return;
    }

    out << "[" << NowText() << "] " << line << "\r\n";
}


static std::string ReadTextFileUtf8(const std::wstring& path)
{
    std::ifstream in(path, std::ios::binary);

    if (!in)
    {
        return "";
    }

    std::ostringstream ss;
    ss << in.rdbuf();

    return ss.str();
}


static void AppendTextFileUtf8(const std::wstring& path, const std::string& text)
{
    std::ofstream out(path, std::ios::app | std::ios::binary);

    if (!out)
    {
        return;
    }

    out << text;
}


static void EnsureProtocol(std::string& url)
{
    url = Trim(url);

    if (url.empty())
    {
        return;
    }

    if (
        !StartsWith(ToLower(url), "http://") &&
        !StartsWith(ToLower(url), "https://")
    )
    {
        url = "http://" + url;
    }
}


static std::string StripHtmlTags(const std::string& html)
{
    std::string out;
    out.reserve(html.size());

    bool inTag = false;
    bool inEntity = false;

    for (char c : html)
    {
        if (c == '<')
        {
            inTag = true;
            out += ' ';
            continue;
        }

        if (c == '>')
        {
            inTag = false;
            out += ' ';
            continue;
        }

        if (!inTag)
        {
            if (c == '&')
            {
                inEntity = true;
                out += ' ';
                continue;
            }

            if (inEntity)
            {
                if (c == ';')
                {
                    inEntity = false;
                }

                continue;
            }

            out += c;
        }
    }

    return out;
}


static std::string CollapseWhitespace(const std::string& input)
{
    std::string out;
    out.reserve(input.size());

    bool lastSpace = false;

    for (char c : input)
    {
        if (isspace((unsigned char)c))
        {
            if (!lastSpace)
            {
                out += ' ';
                lastSpace = true;
            }
        }
        else
        {
            out += c;
            lastSpace = false;
        }
    }

    return Trim(out);
}


static std::string ExtractTitle(const std::string& html)
{
    std::string lower = ToLower(html);

    size_t start = lower.find("<title");
    if (start == std::string::npos)
    {
        return "";
    }

    start = lower.find(">", start);
    if (start == std::string::npos)
    {
        return "";
    }

    size_t end = lower.find("</title>", start);
    if (end == std::string::npos)
    {
        return "";
    }

    std::string title = html.substr(start + 1, end - start - 1);

    title = StripHtmlTags(title);
    title = CollapseWhitespace(title);

    return title;
}


static bool ParseUrl(
    const std::wstring& url,
    std::wstring& scheme,
    std::wstring& host,
    INTERNET_PORT& port,
    std::wstring& path
)
{
    URL_COMPONENTSW parts;
    ZeroMemory(&parts, sizeof(parts));

    wchar_t schemeBuffer[16] = {0};
    wchar_t hostBuffer[512] = {0};
    wchar_t pathBuffer[4096] = {0};

    parts.dwStructSize = sizeof(parts);

    parts.lpszScheme = schemeBuffer;
    parts.dwSchemeLength = _countof(schemeBuffer);

    parts.lpszHostName = hostBuffer;
    parts.dwHostNameLength = _countof(hostBuffer);

    parts.lpszUrlPath = pathBuffer;
    parts.dwUrlPathLength = _countof(pathBuffer);

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
    {
        return false;
    }

    scheme.assign(parts.lpszScheme, parts.dwSchemeLength);
    host.assign(parts.lpszHostName, parts.dwHostNameLength);
    path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);

    if (path.empty())
    {
        path = L"/";
    }

    port = parts.nPort;

    return true;
}


// ------------------------------------------------------------
// HTTP GET using WinHTTP
// ------------------------------------------------------------

static bool HttpGet(const std::string& urlUtf8, std::string& body, int timeoutSeconds)
{
    body.clear();

    std::wstring url = Utf8ToWide(urlUtf8);

    std::wstring scheme;
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;

    if (!ParseUrl(url, scheme, host, port, path))
    {
        return false;
    }

    bool isHttps = (_wcsicmp(scheme.c_str(), L"https") == 0);

    HINTERNET hSession = WinHttpOpen(
        L"AMS Web3Crawler/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );

    if (!hSession)
    {
        return false;
    }

    int timeoutMs = timeoutSeconds * 1000;

    WinHttpSetTimeouts(
        hSession,
        timeoutMs,
        timeoutMs,
        timeoutMs,
        timeoutMs
    );

    HINTERNET hConnect = WinHttpConnect(
        hSession,
        host.c_str(),
        port,
        0
    );

    if (!hConnect)
    {
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD flags = isHttps ? WINHTTP_FLAG_SECURE : 0;

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        L"GET",
        path.c_str(),
        NULL,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags
    );

    if (!hRequest)
    {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    BOOL sent = WinHttpSendRequest(
        hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS,
        0,
        WINHTTP_NO_REQUEST_DATA,
        0,
        0,
        0
    );

    if (!sent)
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    BOOL received = WinHttpReceiveResponse(hRequest, NULL);

    if (!received)
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);

    WinHttpQueryHeaders(
        hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &statusCode,
        &statusSize,
        WINHTTP_NO_HEADER_INDEX
    );

    if (statusCode < 200 || statusCode >= 400)
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD available = 0;

    do
    {
        available = 0;

        if (!WinHttpQueryDataAvailable(hRequest, &available))
        {
            break;
        }

        if (available == 0)
        {
            break;
        }

        std::vector<char> buffer(available + 1);
        DWORD downloaded = 0;

        if (!WinHttpReadData(
                hRequest,
                buffer.data(),
                available,
                &downloaded
            ))
        {
            break;
        }

        if (downloaded == 0)
        {
            break;
        }

        body.append(buffer.data(), downloaded);

        if (body.size() > 2097152)
        {
            break;
        }

    } while (available > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return !body.empty();
}


// ------------------------------------------------------------
// Basic link extraction
// ------------------------------------------------------------

static std::string GetBaseUrl(const std::string& url)
{
    std::string lower = ToLower(url);

    size_t schemeEnd = lower.find("://");

    if (schemeEnd == std::string::npos)
    {
        return url;
    }

    size_t pathStart = url.find("/", schemeEnd + 3);

    if (pathStart == std::string::npos)
    {
        return url;
    }

    return url.substr(0, pathStart);
}


static std::string ResolveUrl(const std::string& baseUrl, const std::string& href)
{
    std::string h = Trim(href);

    if (h.empty())
    {
        return "";
    }

    std::string lower = ToLower(h);

    if (
        StartsWith(lower, "mailto:") ||
        StartsWith(lower, "javascript:") ||
        StartsWith(lower, "tel:") ||
        StartsWith(lower, "#")
    )
    {
        return "";
    }

    if (StartsWith(lower, "http://") || StartsWith(lower, "https://"))
    {
        return h;
    }

    std::string base = GetBaseUrl(baseUrl);

    if (StartsWith(h, "//"))
    {
        return "http:" + h;
    }

    if (StartsWith(h, "/"))
    {
        return base + h;
    }

    return base + "/" + h;
}


static std::vector<std::string> ExtractLinks(const std::string& baseUrl, const std::string& html)
{
    std::vector<std::string> links;

    std::string lower = ToLower(html);

    size_t pos = 0;

    while ((pos = lower.find("href", pos)) != std::string::npos)
    {
        size_t eq = lower.find("=", pos);

        if (eq == std::string::npos)
        {
            break;
        }

        size_t q1 = html.find_first_of("\"'", eq + 1);

        if (q1 == std::string::npos)
        {
            pos = eq + 1;
            continue;
        }

        char quote = html[q1];

        size_t q2 = html.find(quote, q1 + 1);

        if (q2 == std::string::npos)
        {
            break;
        }

        std::string href = html.substr(q1 + 1, q2 - q1 - 1);
        std::string resolved = ResolveUrl(baseUrl, href);

        if (!resolved.empty())
        {
            links.push_back(resolved);
        }

        pos = q2 + 1;

        if (links.size() >= 100)
        {
            break;
        }
    }

    return links;
}


// ------------------------------------------------------------
// Settings parsing
// Very simple JSON value finder for known string/int keys.
// ------------------------------------------------------------

static std::string JsonGetString(const std::string& json, const std::string& key, const std::string& fallback)
{
    std::string pattern = "\"" + key + "\"";

    size_t p = json.find(pattern);

    if (p == std::string::npos)
    {
        return fallback;
    }

    p = json.find(":", p);

    if (p == std::string::npos)
    {
        return fallback;
    }

    p = json.find("\"", p);

    if (p == std::string::npos)
    {
        return fallback;
    }

    size_t start = p + 1;
    size_t end = start;

    bool escaped = false;

    while (end < json.size())
    {
        char c = json[end];

        if (escaped)
        {
            escaped = false;
        }
        else if (c == '\\')
        {
            escaped = true;
        }
        else if (c == '"')
        {
            break;
        }

        end++;
    }

    if (end >= json.size())
    {
        return fallback;
    }

    std::string value = json.substr(start, end - start);

    value = ReplaceAll(value, "\\\\", "\\");
    value = ReplaceAll(value, "\\\"", "\"");
    value = ReplaceAll(value, "\\r", "\r");
    value = ReplaceAll(value, "\\n", "\n");
    value = ReplaceAll(value, "\\t", "\t");

    return value;
}


static int JsonGetInt(const std::string& json, const std::string& key, int fallback)
{
    std::string pattern = "\"" + key + "\"";

    size_t p = json.find(pattern);

    if (p == std::string::npos)
    {
        return fallback;
    }

    p = json.find(":", p);

    if (p == std::string::npos)
    {
        return fallback;
    }

    p++;

    while (p < json.size() && isspace((unsigned char)json[p]))
    {
        p++;
    }

    size_t start = p;

    while (p < json.size() && (isdigit((unsigned char)json[p]) || json[p] == '-'))
    {
        p++;
    }

    if (p <= start)
    {
        return fallback;
    }

    return atoi(json.substr(start, p - start).c_str());
}


// ------------------------------------------------------------
// Indexing
// ------------------------------------------------------------

static void WriteIndexRecord(
    const std::string& title,
    const std::string& url,
    const std::string& text
)
{
    std::string snippet = text;

    if (snippet.size() > 600)
    {
        snippet = snippet.substr(0, 600);
    }

    std::string line =
        "{\"title\":\"" + JsonEscape(title) + "\"," +
        "\"url\":\"" + JsonEscape(url) + "\"," +
        "\"type\":\"web\"," +
        "\"text\":\"" + JsonEscape(snippet) + "\"," +
        "\"indexed_at\":\"" + JsonEscape(NowText()) + "\"}" +
        "\r\n";

    AppendTextFileUtf8(g_indexFile, line);
}


static std::vector<std::string> ReadSeeds(const std::wstring& path)
{
    std::vector<std::string> seeds;

    std::string content = ReadTextFileUtf8(path);
    std::istringstream ss(content);

    std::string line;

    while (std::getline(ss, line))
    {
        line = Trim(line);

        if (line.empty())
        {
            continue;
        }

        if (line[0] == '#')
        {
            continue;
        }

        EnsureProtocol(line);

        if (!line.empty())
        {
            seeds.push_back(line);
        }
    }

    return seeds;
}


struct QueueItem
{
    std::string url;
    int depth;
};


static void CrawlerThreadProc(std::string settingsJson)
{
    g_running = true;
    g_stopRequested = false;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pagesIndexed = 0;
        g_pagesFailed = 0;
        g_queueCount = 0;
        g_lastMessage = "Crawler started";
    }

    LogLine("Crawler started");

    int maxPages = JsonGetInt(settingsJson, "max_pages", 500);
    int maxDepth = JsonGetInt(settingsJson, "max_depth", 2);
    int delayMs = JsonGetInt(settingsJson, "delay_ms", 1000);
    int timeoutSeconds = JsonGetInt(settingsJson, "timeout_seconds", 10);

    std::vector<std::string> seeds = ReadSeeds(g_seedsFile);

    std::queue<QueueItem> q;
    std::set<std::string> visited;

    for (const std::string& seed : seeds)
    {
        q.push({seed, 0});
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_queueCount = (int)q.size();
    }

    while (!q.empty() && !g_stopRequested)
    {
        if (g_pagesIndexed >= maxPages)
        {
            break;
        }

        QueueItem item = q.front();
        q.pop();

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_queueCount = (int)q.size();
            g_lastMessage = "Fetching " + item.url;
        }

        if (visited.find(item.url) != visited.end())
        {
            continue;
        }

        visited.insert(item.url);

        std::string body;

        bool ok = HttpGet(item.url, body, timeoutSeconds);

        if (!ok)
        {
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_pagesFailed++;
                g_lastMessage = "Failed " + item.url;
            }

            LogLine("Failed: " + item.url);
            Sleep(delayMs);
            continue;
        }

        std::string title = ExtractTitle(body);

        if (title.empty())
        {
            title = item.url;
        }

        std::string text = CollapseWhitespace(StripHtmlTags(body));

        WriteIndexRecord(title, item.url, text);

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_pagesIndexed++;
            g_lastMessage = "Indexed " + item.url;
        }

        LogLine("Indexed: " + item.url);

        if (item.depth < maxDepth)
        {
            std::vector<std::string> links = ExtractLinks(item.url, body);

            for (const std::string& link : links)
            {
                if (visited.find(link) == visited.end())
                {
                    q.push({link, item.depth + 1});
                }

                if (q.size() > 1000)
                {
                    break;
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_queueCount = (int)q.size();
        }

        Sleep(delayMs);
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);

        if (g_stopRequested)
        {
            g_lastMessage = "Crawler stopped by user";
        }
        else
        {
            g_lastMessage = "Crawler finished";
        }

        g_queueCount = 0;
    }

    LogLine("Crawler finished");

    g_running = false;
    g_stopRequested = false;
}


// ------------------------------------------------------------
// Search helpers
// ------------------------------------------------------------

static std::string JsonLineGetValue(const std::string& line, const std::string& key)
{
    return JsonGetString(line, key, "");
}


static bool LineMatchesQuery(const std::string& line, const std::string& queryLower)
{
    if (queryLower.empty())
    {
        return true;
    }

    std::string lowerLine = ToLower(line);

    return lowerLine.find(queryLower) != std::string::npos;
}


// ------------------------------------------------------------
// Exported functions
// ------------------------------------------------------------

extern "C" __declspec(dllexport) int __stdcall Crawler_Init(const char* appDataPath)
{
    if (appDataPath == NULL)
    {
        return 0;
    }

    std::string folderUtf8 = Trim(appDataPath);

    if (folderUtf8.empty())
    {
        return 0;
    }

    g_dataFolder = Utf8ToWide(folderUtf8);

    if (!g_dataFolder.empty())
    {
        wchar_t last = g_dataFolder[g_dataFolder.size() - 1];

        if (last != L'\\' && last != L'/')
        {
            g_dataFolder += L"\\";
        }
    }

    g_seedsFile = g_dataFolder + L"seeds.txt";
    g_indexFile = g_dataFolder + L"index.jsonl";
    g_logFile = g_dataFolder + L"crawler.log";

    CreateDirectoryW(g_dataFolder.c_str(), NULL);

    {
        std::ofstream seeds(g_seedsFile, std::ios::app | std::ios::binary);
        std::ofstream index(g_indexFile, std::ios::app | std::ios::binary);
        std::ofstream log(g_logFile, std::ios::app | std::ios::binary);
    }

    SetLastMessage("Initialized");

    return 1;
}


extern "C" __declspec(dllexport) int __stdcall Crawler_Start(const char* settingsJson)
{
    if (g_running)
    {
        return 2;
    }

    if (settingsJson == NULL)
    {
        return 0;
    }

    std::string settings = settingsJson;

    std::string seedsFile = JsonGetString(settings, "seeds_file", "");
    std::string indexFile = JsonGetString(settings, "index_file", "");
    std::string logFile = JsonGetString(settings, "log_file", "");

    if (!seedsFile.empty())
    {
        g_seedsFile = Utf8ToWide(seedsFile);
    }

    if (!indexFile.empty())
    {
        g_indexFile = Utf8ToWide(indexFile);
    }

    if (!logFile.empty())
    {
        g_logFile = Utf8ToWide(logFile);
    }

    g_stopRequested = false;

    try
    {
        g_worker = std::thread(CrawlerThreadProc, settings);
        g_worker.detach();
    }
    catch (...)
    {
        return 0;
    }

    return 1;
}


extern "C" __declspec(dllexport) int __stdcall Crawler_Stop()
{
    if (!g_running)
    {
        return 2;
    }

    g_stopRequested = true;

    SetLastMessage("Stop requested");

    return 1;
}


extern "C" __declspec(dllexport) const char* __stdcall Crawler_GetStatusJson()
{
    bool running = g_running.load();

    int pagesIndexed = 0;
    int pagesFailed = 0;
    int queueCount = 0;
    std::string lastMessage;

    {
        std::lock_guard<std::mutex> lock(g_mutex);

        pagesIndexed = g_pagesIndexed;
        pagesFailed = g_pagesFailed;
        queueCount = g_queueCount;
        lastMessage = g_lastMessage;
    }

    std::ostringstream ss;

    ss
        << "{"
        << "\"running\":" << (running ? "true" : "false") << ","
        << "\"pages_indexed\":" << pagesIndexed << ","
        << "\"pages_failed\":" << pagesFailed << ","
        << "\"queue_count\":" << queueCount << ","
        << "\"last_message\":\"" << JsonEscape(lastMessage) << "\","
        << "\"time\":\"" << JsonEscape(NowText()) << "\""
        << "}";

    std::string json = ss.str();

    ZeroMemory(g_statusBuffer, sizeof(g_statusBuffer));
    strncpy_s(g_statusBuffer, sizeof(g_statusBuffer), json.c_str(), _TRUNCATE);

    return g_statusBuffer;
}


extern "C" __declspec(dllexport) const char* __stdcall Crawler_SearchText(const char* query, int limit)
{
    std::string q;

    if (query != NULL)
    {
        q = Trim(query);
    }

    std::string qLower = ToLower(q);

    if (limit <= 0)
    {
        limit = 100;
    }

    if (limit > 1000)
    {
        limit = 1000;
    }

    std::string content = ReadTextFileUtf8(g_indexFile);

    std::istringstream ss(content);
    std::string line;

    std::ostringstream out;

    int count = 0;

    while (std::getline(ss, line))
    {
        if (count >= limit)
        {
            break;
        }

        line = Trim(line);

        if (line.empty())
        {
            continue;
        }

        if (!LineMatchesQuery(line, qLower))
        {
            continue;
        }

        std::string title = JsonLineGetValue(line, "title");
        std::string url = JsonLineGetValue(line, "url");
        std::string type = JsonLineGetValue(line, "type");

        if (title.empty())
        {
            title = url;
        }

        if (type.empty())
        {
            type = "web";
        }

        if (!url.empty())
        {
            out << title << " | " << url << " | " << type << "\r\n";
            count++;
        }
    }

    std::string result = out.str();

    ZeroMemory(g_searchBuffer, sizeof(g_searchBuffer));
    strncpy_s(g_searchBuffer, sizeof(g_searchBuffer), result.c_str(), _TRUNCATE);

    return g_searchBuffer;
}


BOOL APIENTRY DllMain(
    HMODULE hModule,
    DWORD ul_reason_for_call,
    LPVOID lpReserved
)
{
    switch (ul_reason_for_call)
    {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            break;

        case DLL_PROCESS_DETACH:
            g_stopRequested = true;
            break;
    }

    return TRUE;
}
