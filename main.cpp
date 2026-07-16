#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <atomic>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "githubAPI.hpp"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

using json = nlohmann::json;

static const char* kStateFilePath = "issue_tree_state.json";
static const char* kAuthFilePath = "issue_tree_auth.json";

struct LoadTask {
    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<bool> finished{false};
    std::atomic<float> progress{0.0f};
    std::mutex mutex;
    RepoInfo repo_info;
    std::vector<GitHubIssue> issues;
    std::string error_message;
    std::string status_message;
};

static LoadTask g_load_task;

struct AuthStore {
    std::string client_id;
    std::string access_token;
    std::string login;
    std::string name;
    bool remember_token = true;
};

static AuthStore g_auth_store;

struct GitHubAuthTask {
    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<bool> finished{false};
    std::mutex mutex;
    bool browser_opened = false;
    GitHubDeviceCode device_code;
    GitHubTokenResponse token_response;
    GitHubUserProfile profile;
    std::string error_message;
    std::string status_message;
};

static GitHubAuthTask g_github_auth_task;

static bool open_url_in_browser(const std::string& url) {
#if defined(_WIN32)
    const std::string command = "start \"\" \"" + url + "\"";
    return std::system(command.c_str()) == 0;
#elif defined(__APPLE__)
    const std::string command = "open \"" + url + "\" >/dev/null 2>&1 &";
    return std::system(command.c_str()) == 0;
#else
    const std::string command = "xdg-open \"" + url + "\" >/dev/null 2>&1 &";
    return std::system(command.c_str()) == 0;
#endif
}

struct AppState {
    char repo_url[512];
    char search_query[256];
    char label_filter[128];
    char author_filter[128];
    char milestone_filter[128];
    char saved_view_name[128];
    char bookmark_note[256];
    char github_client_id[128];
    bool has_repo = false;
    bool loading = false;
    bool show_prs = false;
    bool show_only_assigned = false;
    bool show_unlabeled_only = false;
    int state_filter = 0; // 0 = all, 1 = open, 2 = closed
    int sort_mode = 0;    // 0 = newest, 1 = oldest, 2 = comments
    RepoInfo repo_info;
    std::vector<GitHubIssue> issues;
    std::vector<std::string> saved_view_names;
    std::vector<std::string> recent_repositories;
    std::string status_message;
    std::string error_message;
    std::string github_auth_error;
    std::string github_auth_status;
    std::string github_access_token;
    std::string github_login;
    std::string github_name;
    std::string github_user_code;
    std::string github_verification_uri;
    std::string github_verification_uri_complete;
    bool authenticated = false;
    bool github_auth_running = false;
    bool github_remember_token = true;
    int selected_issue_number = -1;

    struct ViewPreset {
        std::string name;
        std::string search_query;
        std::string label_filter;
        std::string author_filter;
        std::string milestone_filter;
        bool show_prs = false;
        bool show_only_assigned = false;
        bool show_unlabeled_only = false;
        int state_filter = 0;
        int sort_mode = 0;
    };

    struct BookmarkEntry {
        int issue_number = 0;
        std::string title;
        std::string note;
    };

    std::vector<ViewPreset> saved_views;
    std::vector<BookmarkEntry> bookmarks;

    AppState() {
        std::snprintf(repo_url, sizeof(repo_url), "%s", "https://github.com/owner/repository");
        std::snprintf(search_query, sizeof(search_query), "%s", "");
        std::snprintf(label_filter, sizeof(label_filter), "%s", "");
        std::snprintf(author_filter, sizeof(author_filter), "%s", "");
        std::snprintf(milestone_filter, sizeof(milestone_filter), "%s", "");
        std::snprintf(saved_view_name, sizeof(saved_view_name), "%s", "Research view");
        std::snprintf(bookmark_note, sizeof(bookmark_note), "%s", "");
        std::snprintf(github_client_id, sizeof(github_client_id), "%s", "");
    }
};

static void reset_filters(AppState& state) {
    std::snprintf(state.search_query, sizeof(state.search_query), "%s", "");
    std::snprintf(state.label_filter, sizeof(state.label_filter), "%s", "");
    std::snprintf(state.author_filter, sizeof(state.author_filter), "%s", "");
    std::snprintf(state.milestone_filter, sizeof(state.milestone_filter), "%s", "");
    state.show_prs = false;
    state.show_only_assigned = false;
    state.show_unlabeled_only = false;
    state.state_filter = 0;
    state.sort_mode = 0;
}

static void add_recent_repository(AppState& state, const std::string& repo_url) {
    state.recent_repositories.erase(std::remove(state.recent_repositories.begin(), state.recent_repositories.end(), repo_url), state.recent_repositories.end());
    state.recent_repositories.insert(state.recent_repositories.begin(), repo_url);
    if (state.recent_repositories.size() > 5) {
        state.recent_repositories.resize(5);
    }
}

static void save_auth_store(const AuthStore& store) {
    json root;
    root["client_id"] = store.client_id;
    root["access_token"] = store.access_token;
    root["login"] = store.login;
    root["name"] = store.name;
    root["remember_token"] = store.remember_token;

    std::ofstream output(kAuthFilePath);
    if (output.is_open()) {
        output << root.dump(2);
    }
}

static void load_auth_store(AuthStore& store) {
    std::ifstream input(kAuthFilePath);
    if (!input.is_open()) {
        return;
    }

    try {
        json root;
        input >> root;

        store.client_id = root.value("client_id", "");
        store.access_token = root.value("access_token", "");
        store.login = root.value("login", "");
        store.name = root.value("name", "");
        store.remember_token = root.value("remember_token", true);
    } catch (...) {
        store = AuthStore{};
    }
}

static json view_preset_to_json(const AppState::ViewPreset& preset) {
    return json{{"name", preset.name},
                {"search_query", preset.search_query},
                {"label_filter", preset.label_filter},
                {"author_filter", preset.author_filter},
                {"milestone_filter", preset.milestone_filter},
                {"show_prs", preset.show_prs},
                {"show_only_assigned", preset.show_only_assigned},
                {"show_unlabeled_only", preset.show_unlabeled_only},
                {"state_filter", preset.state_filter},
                {"sort_mode", preset.sort_mode}};
}

static AppState::ViewPreset view_preset_from_json(const json& value) {
    AppState::ViewPreset preset;
    preset.name = value.value("name", "Research view");
    preset.search_query = value.value("search_query", "");
    preset.label_filter = value.value("label_filter", "");
    preset.author_filter = value.value("author_filter", "");
    preset.milestone_filter = value.value("milestone_filter", "");
    preset.show_prs = value.value("show_prs", false);
    preset.show_only_assigned = value.value("show_only_assigned", false);
    preset.show_unlabeled_only = value.value("show_unlabeled_only", false);
    preset.state_filter = value.value("state_filter", 0);
    preset.sort_mode = value.value("sort_mode", 0);
    return preset;
}

static json bookmark_to_json(const AppState::BookmarkEntry& bookmark) {
    return json{{"issue_number", bookmark.issue_number}, {"title", bookmark.title}, {"note", bookmark.note}};
}

static AppState::BookmarkEntry bookmark_from_json(const json& value) {
    AppState::BookmarkEntry bookmark;
    bookmark.issue_number = value.value("issue_number", 0);
    bookmark.title = value.value("title", "");
    bookmark.note = value.value("note", "");
    return bookmark;
}

static void save_app_state(const AppState& state) {
    json root;
    root["repo_url"] = state.repo_url;
    root["search_query"] = state.search_query;
    root["label_filter"] = state.label_filter;
    root["author_filter"] = state.author_filter;
    root["milestone_filter"] = state.milestone_filter;
    root["show_prs"] = state.show_prs;
    root["show_only_assigned"] = state.show_only_assigned;
    root["show_unlabeled_only"] = state.show_unlabeled_only;
    root["state_filter"] = state.state_filter;
    root["sort_mode"] = state.sort_mode;

    root["recent_repositories"] = json::array();
    for (const std::string& recent_repo : state.recent_repositories) {
        root["recent_repositories"].push_back(recent_repo);
    }

    root["saved_views"] = json::array();
    for (const AppState::ViewPreset& preset : state.saved_views) {
        root["saved_views"].push_back(view_preset_to_json(preset));
    }

    root["bookmarks"] = json::array();
    for (const AppState::BookmarkEntry& bookmark : state.bookmarks) {
        root["bookmarks"].push_back(bookmark_to_json(bookmark));
    }

    std::ofstream output(kStateFilePath);
    if (output.is_open()) {
        output << root.dump(2);
    }
}

static void load_app_state(AppState& state) {
    std::ifstream input(kStateFilePath);
    if (!input.is_open()) {
        return;
    }

    try {
        json root;
        input >> root;

        std::snprintf(state.repo_url, sizeof(state.repo_url), "%s", root.value("repo_url", std::string(state.repo_url)).c_str());
        std::snprintf(state.search_query, sizeof(state.search_query), "%s", root.value("search_query", std::string(state.search_query)).c_str());
        std::snprintf(state.label_filter, sizeof(state.label_filter), "%s", root.value("label_filter", std::string(state.label_filter)).c_str());
        std::snprintf(state.author_filter, sizeof(state.author_filter), "%s", root.value("author_filter", std::string(state.author_filter)).c_str());
        std::snprintf(state.milestone_filter, sizeof(state.milestone_filter), "%s", root.value("milestone_filter", std::string(state.milestone_filter)).c_str());
        state.show_prs = root.value("show_prs", state.show_prs);
        state.show_only_assigned = root.value("show_only_assigned", state.show_only_assigned);
        state.show_unlabeled_only = root.value("show_unlabeled_only", state.show_unlabeled_only);
        state.state_filter = root.value("state_filter", state.state_filter);
        state.sort_mode = root.value("sort_mode", state.sort_mode);

        state.recent_repositories.clear();
        if (root.contains("recent_repositories") && root["recent_repositories"].is_array()) {
            for (const auto& item : root["recent_repositories"]) {
                if (item.is_string()) {
                    state.recent_repositories.push_back(item.get<std::string>());
                }
            }
        }

        state.saved_views.clear();
        if (root.contains("saved_views") && root["saved_views"].is_array()) {
            for (const auto& item : root["saved_views"]) {
                state.saved_views.push_back(view_preset_from_json(item));
            }
        }

        state.bookmarks.clear();
        if (root.contains("bookmarks") && root["bookmarks"].is_array()) {
            for (const auto& item : root["bookmarks"]) {
                state.bookmarks.push_back(bookmark_from_json(item));
            }
        }
    } catch (...) {
        state.error_message = "Unable to load saved app state.";
    }
}

static void initialize_auth(AppState& state) {
    load_auth_store(g_auth_store);
    std::snprintf(state.github_client_id, sizeof(state.github_client_id), "%s", g_auth_store.client_id.c_str());
    state.github_remember_token = g_auth_store.remember_token;
    if (!g_auth_store.access_token.empty()) {
        state.authenticated = true;
        state.github_access_token = g_auth_store.access_token;
        state.github_login = g_auth_store.login;
        state.github_name = g_auth_store.name;
    }
}

static void begin_github_auth(AppState& state) {
    if (g_github_auth_task.running.load()) {
        return;
    }

    if (g_github_auth_task.worker.joinable()) {
        g_github_auth_task.worker.join();
    }

    std::string client_id = state.github_client_id;
    if (client_id.empty()) {
        state.github_auth_error = "Enter your GitHub OAuth App client ID first.";
        return;
    }

    state.github_auth_error.clear();
    state.github_auth_status = "Requesting GitHub device code...";
    state.github_user_code.clear();
    state.github_verification_uri.clear();
    state.github_verification_uri_complete.clear();
    g_github_auth_task.running.store(true);
    g_github_auth_task.finished.store(false);
    g_github_auth_task.browser_opened = false;

    g_github_auth_task.worker = std::thread([client_id]() {
        GitHubDeviceCode device_code;
        std::string error_message;

        if (!request_github_device_code(client_id, "read:user", device_code, error_message)) {
            std::lock_guard<std::mutex> lock(g_github_auth_task.mutex);
            g_github_auth_task.error_message = error_message;
            g_github_auth_task.running.store(false);
            g_github_auth_task.finished.store(true);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_github_auth_task.mutex);
            g_github_auth_task.device_code = device_code;
            g_github_auth_task.status_message = "Opening GitHub in your browser. Approve the device code there.";
        }

        const std::string browser_url = !device_code.verification_uri_complete.empty() ? device_code.verification_uri_complete : device_code.verification_uri;
        if (!browser_url.empty()) {
            open_url_in_browser(browser_url);
        }

        {
            std::lock_guard<std::mutex> lock(g_github_auth_task.mutex);
            g_github_auth_task.browser_opened = true;
            if (!device_code.user_code.empty()) {
                g_github_auth_task.status_message = "GitHub opened. Confirm access and enter code " + device_code.user_code + ".";
            } else {
                g_github_auth_task.status_message = "GitHub opened. Confirm access in the browser.";
            }
        }

        GitHubTokenResponse token_response;
        if (!poll_github_device_token(client_id, device_code.device_code, device_code.interval, token_response, error_message)) {
            std::lock_guard<std::mutex> lock(g_github_auth_task.mutex);
            g_github_auth_task.error_message = error_message;
            g_github_auth_task.running.store(false);
            g_github_auth_task.finished.store(true);
            return;
        }

        GitHubUserProfile profile;
        if (!fetch_github_user_profile(token_response.access_token, profile, error_message)) {
            std::lock_guard<std::mutex> lock(g_github_auth_task.mutex);
            g_github_auth_task.error_message = error_message;
            g_github_auth_task.running.store(false);
            g_github_auth_task.finished.store(true);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_github_auth_task.mutex);
            g_github_auth_task.device_code = device_code;
            g_github_auth_task.token_response = token_response;
            g_github_auth_task.profile = profile;
            g_github_auth_task.status_message = "Connected as " + profile.login + ".";
            g_github_auth_task.error_message.clear();
        }

        g_github_auth_task.running.store(false);
        g_github_auth_task.finished.store(true);
    });
}

static void poll_github_auth_task(AppState& state) {
    if (!g_github_auth_task.finished.load()) {
        return;
    }

    if (g_github_auth_task.worker.joinable()) {
        g_github_auth_task.worker.join();
    }

    std::lock_guard<std::mutex> lock(g_github_auth_task.mutex);
    if (g_github_auth_task.error_message.empty()) {
        state.authenticated = true;
        state.github_access_token = g_github_auth_task.token_response.access_token;
        state.github_login = g_github_auth_task.profile.login;
        state.github_name = g_github_auth_task.profile.name;
        state.github_auth_status = g_github_auth_task.status_message;
        state.github_auth_error.clear();
        g_auth_store.client_id = state.github_client_id;
        if (state.github_remember_token) {
            g_auth_store.access_token = state.github_access_token;
            g_auth_store.login = state.github_login;
            g_auth_store.name = state.github_name;
            g_auth_store.remember_token = true;
            save_auth_store(g_auth_store);
        } else {
            g_auth_store.access_token.clear();
            g_auth_store.login.clear();
            g_auth_store.name.clear();
            g_auth_store.remember_token = false;
            save_auth_store(g_auth_store);
        }
    } else {
        state.github_auth_error = g_github_auth_task.error_message;
    }

    state.github_auth_running = false;
    g_github_auth_task.finished.store(false);
}

static void begin_repository_load(AppState& state, const RepoInfo& repo_info) {
    if (g_load_task.running.load()) {
        return;
    }

    if (g_load_task.worker.joinable()) {
        g_load_task.worker.join();
    }

    state.loading = true;
    state.error_message.clear();
    state.status_message = "Starting repository load...";
    g_load_task.progress.store(0.0f);
    g_load_task.running.store(true);
    g_load_task.finished.store(false);

    const std::string access_token = state.github_access_token;

    g_load_task.worker = std::thread([repo_info, access_token]() {
        std::vector<GitHubIssue> fetched_issues;
        std::string error_message;
        std::string status_message;

        bool success = fetch_github_issues(
            repo_info.owner,
            repo_info.repo,
            fetched_issues,
            error_message,
            [&status_message](float progress, const std::string& message) {
                g_load_task.progress.store(progress);
                status_message = message;
            },
            access_token);

        {
            std::lock_guard<std::mutex> lock(g_load_task.mutex);
            g_load_task.repo_info = repo_info;
            g_load_task.issues = std::move(fetched_issues);
            g_load_task.error_message = error_message;
            g_load_task.status_message = status_message;
            g_load_task.progress.store(success ? 1.0f : 0.0f);
        }

        g_load_task.running.store(false);
        g_load_task.finished.store(true);
    });
}

static void poll_repository_load(AppState& state) {
    if (!g_load_task.finished.load()) {
        return;
    }

    if (g_load_task.worker.joinable()) {
        g_load_task.worker.join();
    }

    std::lock_guard<std::mutex> lock(g_load_task.mutex);
    if (g_load_task.error_message.empty()) {
        state.repo_info = g_load_task.repo_info;
        state.issues = g_load_task.issues;
        state.has_repo = true;
        state.selected_issue_number = state.issues.empty() ? -1 : state.issues.front().number;
        state.status_message = "Loaded " + std::to_string(state.issues.size()) + " items from " + state.repo_info.owner + "/" + state.repo_info.repo + ".";
        add_recent_repository(state, state.repo_url);
        save_app_state(state);
    } else {
        state.error_message = g_load_task.error_message;
        state.has_repo = false;
        state.issues.clear();
    }

    state.loading = false;
    g_load_task.finished.store(false);
}

static void shutdown_repository_load() {
    if (g_load_task.worker.joinable()) {
        g_load_task.worker.join();
    }
}

static void glfw_error_callback(int error, const char* description) {
    std::cerr << "Glfw Error " << error << ": " << description << std::endl;
}

static void apply_professional_style() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(12.0f, 12.0f);
    style.FramePadding = ImVec2(8.0f, 5.0f);
    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.IndentSpacing = 18.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.09f, 0.12f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.11f, 0.13f, 0.16f, 1.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.12f, 0.16f, 0.98f);
    colors[ImGuiCol_Border] = ImVec4(0.18f, 0.21f, 0.27f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.17f, 0.22f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.22f, 0.28f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.19f, 0.24f, 0.31f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.09f, 0.12f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.13f, 0.17f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.18f, 0.25f, 0.35f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.31f, 0.43f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.22f, 0.38f, 0.56f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.16f, 0.24f, 0.35f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.19f, 0.29f, 0.41f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.17f, 0.27f, 0.39f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.18f, 0.21f, 0.27f, 1.00f);
    colors[ImGuiCol_Text] = ImVec4(0.90f, 0.92f, 0.95f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.58f, 0.62f, 1.00f);
}

static ImVec4 state_color(const GitHubIssue& issue) {
    return issue.state == "closed" ? ImVec4(0.32f, 0.83f, 0.45f, 1.0f) : ImVec4(0.90f, 0.35f, 0.35f, 1.0f);
}

static std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

static bool contains_case_insensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) {
        return true;
    }
    return to_lower_copy(haystack).find(to_lower_copy(needle)) != std::string::npos;
}

static bool label_matches(const GitHubIssue& issue, const char* label_filter) {
    if (label_filter[0] == '\0') {
        return true;
    }
    for (const std::string& label : issue.labels) {
        if (contains_case_insensitive(label, label_filter)) {
            return true;
        }
    }
    return false;
}

static bool is_security_item(const GitHubIssue& issue) {
    if (contains_case_insensitive(issue.title, "security") ||
        contains_case_insensitive(issue.body, "security") ||
        contains_case_insensitive(issue.title, "vulnerability") ||
        contains_case_insensitive(issue.body, "vulnerability") ||
        contains_case_insensitive(issue.title, "cve") ||
        contains_case_insensitive(issue.body, "cve") ||
        contains_case_insensitive(issue.title, "exploit") ||
        contains_case_insensitive(issue.body, "exploit")) {
        return true;
    }

    for (const std::string& label : issue.labels) {
        if (contains_case_insensitive(label, "security") || contains_case_insensitive(label, "vulnerability") || contains_case_insensitive(label, "cve")) {
            return true;
        }
    }

    return false;
}

static const char* item_kind_label(const GitHubIssue& issue) {
    if (issue.is_pull_request) {
        return "PR";
    }
    if (is_security_item(issue)) {
        return "Security";
    }
    return "Issue";
}

static bool issue_matches_filters(const GitHubIssue& issue, const AppState& state) {
    if (state.state_filter == 1 && issue.state != "open") {
        return false;
    }
    if (state.state_filter == 2 && issue.state != "closed") {
        return false;
    }
    const std::string query = state.search_query;
    if (!contains_case_insensitive(issue.title, query) &&
        !contains_case_insensitive(issue.body, query) &&
        !contains_case_insensitive(issue.author, query) &&
        !contains_case_insensitive(issue.milestone, query)) {
        bool label_hit = false;
        for (const std::string& label : issue.labels) {
            if (contains_case_insensitive(label, query)) {
                label_hit = true;
                break;
            }
        }
        if (!label_hit) {
            return false;
        }
    }
    if (!label_matches(issue, state.label_filter)) {
        return false;
    }
    if (!contains_case_insensitive(issue.author, state.author_filter)) {
        return false;
    }
    if (!contains_case_insensitive(issue.milestone, state.milestone_filter)) {
        return false;
    }
    if (state.show_unlabeled_only && !issue.labels.empty()) {
        return false;
    }
    if (state.show_only_assigned && issue.assignees.empty()) {
        return false;
    }
    return true;
}

static std::string short_body_preview(const GitHubIssue& issue) {
    if (issue.body.empty()) {
        return "No description provided.";
    }
    const size_t limit = 140;
    if (issue.body.size() <= limit) {
        return issue.body;
    }
    return issue.body.substr(0, limit) + "...";
}

static std::string normalized_description(const GitHubIssue& issue) {
    if (issue.body.empty()) {
        return "No description provided.";
    }
    return issue.body;
}

static void draw_metric_card(const char* label, const std::string& value, const ImVec4& accent, float width) {
    (void)width;
    ImGui::BeginGroup();
    ImGui::TextColored(accent, "%s", label);
    ImGui::TextWrapped("%s", value.c_str());
    ImGui::EndGroup();
}
    static void render_comment_entry(const GitHubComment& comment, int index);
    static void render_issue_tree_item(const GitHubIssue& issue, int& selected_issue_number);

static void draw_status_banner(AppState& state) {
    const bool has_error = !state.error_message.empty();
    const bool has_status = !state.status_message.empty();
    if (!has_error && !has_status) {
        return;
    }

    ImVec4 border = has_error ? ImVec4(0.82f, 0.34f, 0.28f, 1.0f) : ImVec4(0.78f, 0.62f, 0.22f, 1.0f);
    ImVec4 fill = has_error ? ImVec4(0.26f, 0.12f, 0.12f, 1.0f) : ImVec4(0.24f, 0.19f, 0.10f, 1.0f);
    std::string message = has_error ? state.error_message : state.status_message;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, fill);
    ImGui::PushStyleColor(ImGuiCol_Border, border);
    ImGui::BeginChild("StatusBanner", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushStyleColor(ImGuiCol_Text, border);
    ImGui::TextUnformatted(has_error ? "[!]" : "[i]");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextWrapped("%s", message.c_str());
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - 26.0f);
    if (ImGui::SmallButton("X")) {
        if (has_error) {
            state.error_message.clear();
        } else {
            state.status_message.clear();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}

static void draw_recent_repo_badge(AppState& state, const std::string& recent_repo, bool& load_requested) {
    ImGui::PushID(recent_repo.c_str());
    if (ImGui::SmallButton(recent_repo.c_str())) {
        std::snprintf(state.repo_url, sizeof(state.repo_url), "%s", recent_repo.c_str());
        load_requested = true;
    }
    ImGui::PopID();
}

static void render_security_row(const GitHubIssue& issue, int& selected_issue_number) {
    const bool high = contains_case_insensitive(issue.title, "cve") || contains_case_insensitive(issue.body, "cve") || contains_case_insensitive(issue.title, "critical") || contains_case_insensitive(issue.body, "critical");
    const bool medium = !high && (contains_case_insensitive(issue.title, "security") || contains_case_insensitive(issue.body, "security") || contains_case_insensitive(issue.title, "vulnerability") || contains_case_insensitive(issue.body, "vulnerability"));
    ImVec4 severity = high ? ImVec4(0.94f, 0.36f, 0.33f, 1.0f) : (medium ? ImVec4(0.95f, 0.73f, 0.23f, 1.0f) : ImVec4(0.52f, 0.78f, 0.46f, 1.0f));
    if (ImGui::Selectable(("#" + std::to_string(issue.number) + " " + issue.title).c_str(), selected_issue_number == issue.number)) {
        selected_issue_number = issue.number;
    }
    ImGui::SameLine();
    ImGui::TextColored(severity, high ? "[High]" : (medium ? "[Medium]" : "[Low]"));
}

static void render_inspector_panel(const AppState& state, const GitHubIssue* issue) {
    if (!issue) {
        ImGui::TextWrapped("Load a repository, then select an issue, pull request, or security item to inspect its full metadata.");
        return;
    }

    ImVec4 accent = issue->is_pull_request ? ImVec4(0.61f, 0.72f, 0.98f, 1.0f) : (is_security_item(*issue) ? ImVec4(1.00f, 0.65f, 0.30f, 1.0f) : state_color(*issue));
    ImGui::TextColored(accent, "%s #%d", item_kind_label(*issue), issue->number);
    ImGui::TextWrapped("%s", issue->title.c_str());
    ImGui::TextDisabled("by %s", issue->author.empty() ? "unknown author" : issue->author.c_str());
    ImGui::Separator();

    ImGui::BeginChild("InspectorBody", ImVec2(0.0f, 0.0f), true);
    ImGui::TextColored(ImVec4(0.92f, 0.94f, 0.97f, 1.0f), "Description");
    ImGui::TextWrapped("%s", normalized_description(*issue).c_str());
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.92f, 0.94f, 0.97f, 1.0f), "Metadata");
    ImGui::TextDisabled("State: %s", issue->state.c_str());
    ImGui::TextDisabled("Created at: %s", issue->created_at.c_str());
    ImGui::TextDisabled("Updated at: %s", issue->updated_at.empty() ? "unknown" : issue->updated_at.c_str());
    ImGui::TextDisabled("Closed at: %s", issue->closed_at.empty() ? "pending" : issue->closed_at.c_str());
    ImGui::TextDisabled("Comments: %d", issue->comment_count);
    ImGui::TextDisabled("Reactions: %d", issue->reactions);
    if (!issue->milestone.empty()) {
        ImGui::TextDisabled("Milestone: %s", issue->milestone.c_str());
    }
    if (!issue->labels.empty()) {
        std::string labels;
        for (size_t index = 0; index < issue->labels.size(); ++index) {
            if (index > 0) {
                labels += ", ";
            }
            labels += issue->labels[index];
        }
        ImGui::TextDisabled("Labels: %s", labels.c_str());
    }
    if (!issue->assignees.empty()) {
        std::string assignees;
        for (size_t index = 0; index < issue->assignees.size(); ++index) {
            if (index > 0) {
                assignees += ", ";
            }
            assignees += issue->assignees[index];
        }
        ImGui::TextDisabled("Assignees: %s", assignees.c_str());
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.92f, 0.94f, 0.97f, 1.0f), "Discussion");
    if (issue->comments.empty()) {
        ImGui::TextDisabled("No discussion comments were returned for this item.");
    } else {
        for (size_t index = 0; index < issue->comments.size(); ++index) {
            render_comment_entry(issue->comments[index], static_cast<int>(index));
            if (index + 1 < issue->comments.size()) {
                ImGui::Separator();
            }
        }
    }
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::TextDisabled("Created at %s", issue->created_at.c_str());
    ImGui::TextDisabled("Closed at %s", issue->closed_at.empty() ? "pending" : issue->closed_at.c_str());
    ImGui::TextDisabled("Direct link: https://github.com/%s/%s/issues/%d", state.repo_info.owner.c_str(), state.repo_info.repo.c_str(), issue->number);
}

static void render_workspace_section(const char* section_title, const std::vector<const GitHubIssue*>& items, int& selected_issue_number, bool security_section = false) {
    ImGui::BeginChild(section_title, ImVec2(0.0f, 0.0f), true);
    if (security_section) {
        for (const GitHubIssue* item : items) {
            render_security_row(*item, selected_issue_number);
        }
    } else {
        for (const GitHubIssue* item : items) {
            render_issue_tree_item(*item, selected_issue_number);
        }
    }
    ImGui::EndChild();
}

static void render_issue_tree_item(const GitHubIssue& issue, int& selected_issue_number) {
    ImVec4 accent = state_color(issue);
    std::string header = std::string("#") + std::to_string(issue.number) + "  " + issue.title;
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (selected_issue_number == issue.number) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    bool opened = ImGui::TreeNodeEx((void*)(intptr_t)issue.number, flags, "%s", header.c_str());
    if (ImGui::IsItemClicked()) {
        selected_issue_number = issue.number;
    }

    ImGui::SameLine();
    ImGui::TextColored(accent, "%s", item_kind_label(issue));

    if (opened) {
        ImGui::Indent();
        ImGui::TextWrapped("%s", short_body_preview(issue).c_str());
        ImGui::Unindent();
        ImGui::TreePop();
    }
}

static const GitHubIssue* find_selected_issue(const std::vector<GitHubIssue>& issues, int selected_issue_number) {
    for (const GitHubIssue& issue : issues) {
        if (issue.number == selected_issue_number) {
            return &issue;
        }
    }
    return nullptr;
}

static void render_comment_entry(const GitHubComment& comment, int index) {
    ImGui::PushID(index);
    ImGui::TextColored(ImVec4(0.88f, 0.92f, 0.97f, 1.0f), "%s", comment.author.empty() ? "unknown" : comment.author.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", comment.created_at.c_str());
    if (!comment.updated_at.empty() && comment.updated_at != comment.created_at) {
        ImGui::SameLine();
        ImGui::TextDisabled("updated %s", comment.updated_at.c_str());
    }
    ImGui::Spacing();
    ImGui::TextWrapped("%s", comment.body.c_str());
    ImGui::Spacing();
    ImGui::PopID();
}

static void render_auth_screen(AppState& state) {
    ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::Begin("Issue Tree Access", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);

    ImGui::TextColored(ImVec4(0.96f, 0.97f, 1.0f, 1.0f), "Issue Tree Access");
    ImGui::TextDisabled("Connect your GitHub account with the free device-flow login.");
    ImGui::Separator();

    ImGui::InputTextWithHint("##github_client_id", "GitHub OAuth App client ID", state.github_client_id, sizeof(state.github_client_id));
    ImGui::Checkbox("Remember token", &state.github_remember_token);
    ImGui::TextDisabled("You need a free GitHub OAuth App client ID. Device flow does not require a client secret.");

    ImGui::Spacing();
    if (ImGui::Button(g_github_auth_task.running.load() ? "Connecting..." : "Connect GitHub", ImVec2(-1.0f, 0.0f))) {
        state.github_auth_running = true;
        begin_github_auth(state);
    }

    if (g_github_auth_task.running.load()) {
        ImGui::Spacing();
        std::string auth_progress = "Waiting for GitHub authorization...";
        {
            std::lock_guard<std::mutex> lock(g_github_auth_task.mutex);
            if (!g_github_auth_task.status_message.empty()) {
                auth_progress = g_github_auth_task.status_message;
            }
            if (!g_github_auth_task.device_code.user_code.empty()) {
                ImGui::TextColored(ImVec4(0.95f, 0.97f, 1.0f, 1.0f), "Code: %s", g_github_auth_task.device_code.user_code.c_str());
                ImGui::TextWrapped("Open %s if your browser does not launch automatically.", g_github_auth_task.device_code.verification_uri.c_str());
            }
        }
        ImGui::TextDisabled("%s", auth_progress.c_str());
    }

    poll_github_auth_task(state);

    if (!state.github_auth_error.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.95f, 0.42f, 0.42f, 1.0f), "%s", state.github_auth_error.c_str());
    }

    if (!state.github_auth_status.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.45f, 0.80f, 0.98f, 1.0f), "%s", state.github_auth_status.c_str());
    }

    ImGui::Spacing();
    ImGui::TextDisabled("The app uses your GitHub token to load public repositories and raises your API limit.");

    ImGui::End();
}

static void draw_dashboard(AppState& state) {
    ImGui::SetNextWindowSize(ImVec2(1480, 940), ImGuiCond_FirstUseEver);
    ImGui::Begin("GitHub Issue Tree Explorer");

    const float full_width = ImGui::GetContentRegionAvail().x;
    const float header_height = 260.0f;
    const float left_width = std::max(420.0f, std::min(full_width * 0.56f, full_width * 0.46f));
    const float right_width = full_width - left_width - ImGui::GetStyle().ItemSpacing.x;

    ImGui::BeginChild("TopStrip", ImVec2(0.0f, header_height), true);
    ImGui::TextColored(ImVec4(0.95f, 0.97f, 1.0f, 1.0f), "GitHub Repository Explorer");
    ImGui::TextDisabled("Engineering dashboard for issues, pull requests, and security review.");
    ImGui::SameLine();
    if (!state.github_login.empty()) {
        ImGui::TextDisabled("Connected as %s", state.github_name.empty() ? state.github_login.c_str() : state.github_name.c_str());
    }
    ImGui::Separator();

    bool load_requested = false;
    ImGui::BeginChild("RepoControls", ImVec2(0.0f, 76.0f), true);
    ImGui::TextColored(ImVec4(0.92f, 0.94f, 0.97f, 1.0f), "Repository URL");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 128.0f);
    load_requested = ImGui::InputText("##repo_url", state.repo_url, sizeof(state.repo_url), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button(state.loading ? "Loading..." : "Load", ImVec2(120.0f, 0.0f))) {
        load_requested = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset", ImVec2(90.0f, 0.0f))) {
        reset_filters(state);
        save_app_state(state);
    }
    ImGui::EndChild();

    if (ImGui::BeginChild("RecentReposStrip", ImVec2(0.0f, 42.0f), true)) {
        ImGui::TextDisabled("Recent repositories");
        ImGui::SameLine();
        if (state.recent_repositories.empty()) {
            ImGui::TextDisabled("No recent repositories yet.");
        } else {
            for (const std::string& recent_repo : state.recent_repositories) {
                draw_recent_repo_badge(state, recent_repo, load_requested);
                ImGui::SameLine();
            }
        }
        ImGui::EndChild();
    }

    if (ImGui::CollapsingHeader("Advanced Filters & Sorting", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::BeginChild("AdvancedFilterPanel", ImVec2(0.0f, 90.0f), true);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.28f);
        ImGui::InputTextWithHint("##search_query", "Search title, body, author, label", state.search_query, sizeof(state.search_query));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.22f);
        ImGui::InputTextWithHint("##author_filter", "Author filter", state.author_filter, sizeof(state.author_filter));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.22f);
        ImGui::InputTextWithHint("##milestone_filter", "Milestone filter", state.milestone_filter, sizeof(state.milestone_filter));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.22f);
        ImGui::InputTextWithHint("##label_filter", "Label filter", state.label_filter, sizeof(state.label_filter));
        ImGui::Checkbox("Assigned only", &state.show_only_assigned);
        ImGui::SameLine();
        ImGui::Checkbox("Unlabeled only", &state.show_unlabeled_only);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.92f, 0.94f, 0.97f, 1.0f), "State");
        ImGui::SameLine();
        ImGui::RadioButton("All", &state.state_filter, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Open", &state.state_filter, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Closed", &state.state_filter, 2);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.92f, 0.94f, 0.97f, 1.0f), "Sort");
        ImGui::SameLine();
        ImGui::RadioButton("Newest", &state.sort_mode, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Oldest", &state.sort_mode, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Most comments", &state.sort_mode, 2);
        ImGui::EndChild();
    }

    ImGui::EndChild();

    draw_status_banner(state);

    if (load_requested && !state.loading) {
        RepoInfo parsed;
        state.error_message.clear();
        state.status_message.clear();
        state.selected_issue_number = -1;

        if (!parse_github_repo_url(state.repo_url, parsed)) {
            state.error_message = "Enter a valid GitHub repository URL such as https://github.com/owner/repo.";
        } else {
            begin_repository_load(state, parsed);
        }
    }

    if (state.loading || g_load_task.running.load()) {
        float progress = g_load_task.progress.load();
        std::string progress_text = "Loading repository...";
        {
            std::lock_guard<std::mutex> lock(g_load_task.mutex);
            if (!g_load_task.status_message.empty()) {
                progress_text = g_load_task.status_message;
            }
        }
        ImGui::ProgressBar(progress, ImVec2(0.0f, 0.0f), progress_text.c_str());
    }

    poll_repository_load(state);

    int issue_count = 0;
    int pr_count = 0;
    int security_count = 0;
    for (const GitHubIssue& issue : state.issues) {
        if (!issue_matches_filters(issue, state)) {
            continue;
        }
        if (issue.is_pull_request) {
            ++pr_count;
        } else if (is_security_item(issue)) {
            ++security_count;
        } else {
            ++issue_count;
        }
    }

    ImGui::BeginChild("MetricsRow", ImVec2(0.0f, 52.0f), true);
    const float metric_width = (ImGui::GetContentRegionAvail().x - 20.0f) / 3.0f;
    ImGui::BeginChild("MetricIssues", ImVec2(metric_width, 0.0f), true);
    draw_metric_card("Issues", std::to_string(issue_count) + " items", ImVec4(0.40f, 0.85f, 0.55f, 1.0f), metric_width);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("MetricPRs", ImVec2(metric_width, 0.0f), true);
    draw_metric_card("Pull Requests", std::to_string(pr_count) + " items", ImVec4(0.55f, 0.72f, 0.98f, 1.0f), metric_width);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("MetricSecurity", ImVec2(0.0f, 0.0f), true);
    draw_metric_card("Security", std::to_string(security_count) + " items", ImVec4(1.00f, 0.65f, 0.30f, 1.0f), metric_width);
    ImGui::EndChild();
    ImGui::EndChild();

    auto sort_issues = [state](std::vector<const GitHubIssue*>& items) {
        std::sort(items.begin(), items.end(), [&state](const GitHubIssue* lhs, const GitHubIssue* rhs) {
            if (state.sort_mode == 2) {
                return lhs->comment_count > rhs->comment_count;
            }
            if (state.sort_mode == 1) {
                return lhs->created_at < rhs->created_at;
            }
            return lhs->created_at > rhs->created_at;
        });
    };

    std::vector<const GitHubIssue*> visible_issues;
    for (const GitHubIssue& issue : state.issues) {
        if (issue_matches_filters(issue, state)) {
            visible_issues.push_back(&issue);
        }
    }

    std::vector<const GitHubIssue*> issue_items;
    std::vector<const GitHubIssue*> pr_items;
    std::vector<const GitHubIssue*> security_items;
    for (const GitHubIssue* issue : visible_issues) {
        if (issue->is_pull_request) {
            pr_items.push_back(issue);
        } else if (is_security_item(*issue)) {
            security_items.push_back(issue);
        } else {
            issue_items.push_back(issue);
        }
    }

    sort_issues(issue_items);
    sort_issues(pr_items);
    sort_issues(security_items);

    ImGui::BeginChild("WorkspaceArea", ImVec2(0.0f, 0.0f), true);
    ImGui::BeginGroup();
    ImGui::BeginChild("WorkspaceLeft", ImVec2(left_width, 0.0f), true);
    if (ImGui::BeginTabBar("WorkspaceTabs")) {
        if (ImGui::BeginTabItem("Issues")) {
            render_workspace_section("IssuesSection", issue_items, state.selected_issue_number);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Pull Requests")) {
            render_workspace_section("PullRequestsSection", pr_items, state.selected_issue_number);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Security Threats")) {
            render_workspace_section("SecuritySection", security_items, state.selected_issue_number, true);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("WorkspaceRight", ImVec2(right_width, 0.0f), true);
    ImGui::TextColored(ImVec4(0.95f, 0.97f, 1.0f, 1.0f), "Deep Inspector View");
    ImGui::TextDisabled("Metadata, description, timeline, and direct link.");
    ImGui::Separator();
    render_inspector_panel(state, find_selected_issue(state.issues, state.selected_issue_number));
    ImGui::EndChild();
    ImGui::EndGroup();
    ImGui::EndChild();

    ImGui::End();
}

int main(int, char**) {
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        return 1;
    }

    const char* glsl_version = "#version 330";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    GLFWwindow* window = glfwCreateWindow(1440, 860, "GitHub Issue Tree Explorer", NULL, NULL);
    if (window == NULL) {
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    apply_professional_style();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    AppState state;
    load_app_state(state);
    initialize_auth(state);
    ImVec4 clear_color = ImVec4(0.07f, 0.09f, 0.12f, 1.00f);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (state.authenticated) {
            draw_dashboard(state);
        } else {
            render_auth_screen(state);
        }

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    save_app_state(state);
    shutdown_repository_load();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}