#include "githubAPI.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <thread>

using json = nlohmann::json;

static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

static void apply_common_headers(struct curl_slist*& headers, const std::string& access_token, const std::string& accept_header = "application/vnd.github+json") {
    headers = curl_slist_append(headers, "User-Agent: issue-tree-visualizer");
    headers = curl_slist_append(headers, ("Accept: " + accept_header).c_str());
    if (!access_token.empty()) {
        headers = curl_slist_append(headers, ("Authorization: Bearer " + access_token).c_str());
    }
}

static bool perform_json_post(CURL* curl, const std::string& url, const std::string& post_fields, const std::string& access_token, json& response_json, std::string& error_message, const std::string& accept_header = "application/vnd.github+json") {
    std::string read_buffer;
    struct curl_slist* headers = nullptr;
    apply_common_headers(headers, access_token, accept_header);
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_fields.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &read_buffer);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "issue-tree-visualizer");

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    if (res != CURLE_OK) {
        error_message = curl_easy_strerror(res);
        return false;
    }

    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    if (http_status != 200) {
        try {
            response_json = json::parse(read_buffer);
            if (response_json.is_object() && response_json.contains("error")) {
                error_message = response_json["error"].get<std::string>();
            } else {
                error_message = "GitHub request failed with status " + std::to_string(http_status);
            }
        } catch (...) {
            error_message = "GitHub request failed with status " + std::to_string(http_status);
        }
        return false;
    }

    try {
        response_json = json::parse(read_buffer);
    } catch (const std::exception& e) {
        error_message = e.what();
        return false;
    }

    return true;
}

static std::string trim_repo_suffix(const std::string& input) {
    std::string result = input;
    while (!result.empty() && (result.back() == '/' || result.back() == '\n' || result.back() == '\r' || result.back() == ' ' || result.back() == '\t')) {
        result.pop_back();
    }

    const std::string suffix = ".git";
    if (result.size() >= suffix.size() && result.compare(result.size() - suffix.size(), suffix.size(), suffix) == 0) {
        result.erase(result.size() - suffix.size());
    }

    return result;
}

bool parse_github_repo_url(const std::string& repo_url, RepoInfo& repo_info) {
    std::string normalized = trim_repo_suffix(repo_url);
    const std::string https_prefix = "https://github.com/";
    const std::string http_prefix = "http://github.com/";
    const std::string ssh_prefix = "git@github.com:";

    std::string path;
    if (normalized.find(https_prefix) == 0) {
        path = normalized.substr(https_prefix.size());
    } else if (normalized.find(http_prefix) == 0) {
        path = normalized.substr(http_prefix.size());
    } else if (normalized.find(ssh_prefix) == 0) {
        path = normalized.substr(ssh_prefix.size());
    } else {
        return false;
    }

    const size_t first_slash = path.find('/');
    if (first_slash == std::string::npos || first_slash == 0) {
        return false;
    }

    const size_t next_slash = path.find('/', first_slash + 1);
    repo_info.owner = path.substr(0, first_slash);
    repo_info.repo = next_slash == std::string::npos ? path.substr(first_slash + 1) : path.substr(first_slash + 1, next_slash - first_slash - 1);
    return !repo_info.owner.empty() && !repo_info.repo.empty();
}

static std::string get_json_string(const json& value, const char* key) {
    if (!value.contains(key) || value[key].is_null()) {
        return "";
    }
    return value[key].get<std::string>();
}

bool request_github_device_code(const std::string& client_id, const std::string& scope, GitHubDeviceCode& device_code, std::string& error_message) {
    device_code = GitHubDeviceCode{};
    error_message.clear();

    CURL* curl = curl_easy_init();
    if (!curl) {
        error_message = "Unable to initialize GitHub device flow request.";
        return false;
    }

    json response_json;
    char* escaped_client_id = curl_easy_escape(curl, client_id.c_str(), 0);
    char* escaped_scope = curl_easy_escape(curl, scope.c_str(), 0);
    std::string post_fields = "client_id=" + std::string(escaped_client_id ? escaped_client_id : "") + "&scope=" + std::string(escaped_scope ? escaped_scope : "");
    if (escaped_client_id) {
        curl_free(escaped_client_id);
    }
    if (escaped_scope) {
        curl_free(escaped_scope);
    }
    bool success = perform_json_post(curl, "https://github.com/login/device/code", post_fields, "", response_json, error_message, "application/json");

    if (success) {
        device_code.device_code = response_json.value("device_code", "");
        device_code.user_code = response_json.value("user_code", "");
        device_code.verification_uri = response_json.value("verification_uri", "https://github.com/login/device");
        device_code.verification_uri_complete = response_json.value("verification_uri_complete", "");
        device_code.expires_in = response_json.value("expires_in", 900);
        device_code.interval = response_json.value("interval", 5);
        if (device_code.device_code.empty() || device_code.user_code.empty()) {
            error_message = "GitHub did not return a valid device code.";
            success = false;
        }
    }

    curl_easy_cleanup(curl);
    return success;
}

bool poll_github_device_token(const std::string& client_id, const std::string& device_code, int interval_seconds, GitHubTokenResponse& token_response, std::string& error_message) {
    token_response = GitHubTokenResponse{};
    error_message.clear();

    CURL* curl = curl_easy_init();
    if (!curl) {
        error_message = "Unable to initialize GitHub token polling request.";
        return false;
    }

    const int max_attempts = std::max(1, 900 / std::max(1, interval_seconds));
    int current_interval = std::max(1, interval_seconds);

    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        json response_json;
        char* escaped_client_id = curl_easy_escape(curl, client_id.c_str(), 0);
        char* escaped_device_code = curl_easy_escape(curl, device_code.c_str(), 0);
        std::string post_fields = "client_id=" + std::string(escaped_client_id ? escaped_client_id : "") +
                                  "&device_code=" + std::string(escaped_device_code ? escaped_device_code : "") +
                                  "&grant_type=urn:ietf:params:oauth:grant-type:device_code";
        if (escaped_client_id) {
            curl_free(escaped_client_id);
        }
        if (escaped_device_code) {
            curl_free(escaped_device_code);
        }

        std::string read_buffer;
        struct curl_slist* headers = nullptr;
        apply_common_headers(headers, "", "application/json");
        headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");

        curl_easy_setopt(curl, CURLOPT_URL, "https://github.com/login/oauth/access_token");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_fields.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &read_buffer);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "issue-tree-visualizer");

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        if (res != CURLE_OK) {
            error_message = curl_easy_strerror(res);
            curl_easy_cleanup(curl);
            return false;
        }

        long http_status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
        if (http_status != 200) {
            error_message = "GitHub token polling failed with status " + std::to_string(http_status);
            curl_easy_cleanup(curl);
            return false;
        }

        try {
            response_json = json::parse(read_buffer);
        } catch (const std::exception& e) {
            error_message = e.what();
            curl_easy_cleanup(curl);
            return false;
        }

        const std::string response_error = response_json.value("error", "");
        if (response_error.empty()) {
            token_response.access_token = response_json.value("access_token", "");
            token_response.token_type = response_json.value("token_type", "bearer");
            token_response.scope = response_json.value("scope", "");
            curl_easy_cleanup(curl);
            return !token_response.access_token.empty();
        }

        if (response_error == "authorization_pending") {
            std::this_thread::sleep_for(std::chrono::seconds(current_interval));
            continue;
        }
        if (response_error == "slow_down") {
            current_interval += 5;
            std::this_thread::sleep_for(std::chrono::seconds(current_interval));
            continue;
        }
        if (response_error == "access_denied") {
            error_message = "GitHub authorization was denied.";
            curl_easy_cleanup(curl);
            return false;
        }
        if (response_error == "expired_token") {
            error_message = "GitHub device code expired. Please try again.";
            curl_easy_cleanup(curl);
            return false;
        }

        error_message = response_error;
        curl_easy_cleanup(curl);
        return false;
    }

    error_message = "GitHub authorization timed out.";
    curl_easy_cleanup(curl);
    return false;
}

bool fetch_github_user_profile(const std::string& access_token, GitHubUserProfile& profile, std::string& error_message) {
    profile = GitHubUserProfile{};
    error_message.clear();

    CURL* curl = curl_easy_init();
    if (!curl) {
        error_message = "Unable to initialize GitHub user request.";
        return false;
    }

    std::string read_buffer;
    struct curl_slist* headers = nullptr;
    apply_common_headers(headers, access_token);

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.github.com/user");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &read_buffer);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "issue-tree-visualizer");

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        error_message = curl_easy_strerror(res);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return false;
    }

    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (http_status != 200) {
        error_message = "GitHub user request failed with status " + std::to_string(http_status);
        return false;
    }

    try {
        json response = json::parse(read_buffer);
        profile.login = get_json_string(response, "login");
        profile.name = get_json_string(response, "name");
        profile.avatar_url = get_json_string(response, "avatar_url");
        return !profile.login.empty();
    } catch (const std::exception& e) {
        error_message = e.what();
        return false;
    }
}

bool fetch_issue_comments(const std::string& comments_url, std::vector<GitHubComment>& comments, std::string& error_message, const std::string& access_token) {
    comments.clear();
    error_message.clear();

    CURL* curl = curl_easy_init();
    if (!curl) {
        error_message = "Unable to initialize GitHub comments request.";
        return false;
    }

    struct curl_slist* headers = nullptr;
    apply_common_headers(headers, access_token);

    bool success = true;

    for (int page = 1; page <= 10; ++page) {
        std::string read_buffer;
        std::string url = comments_url + "?per_page=100&page=" + std::to_string(page);

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &read_buffer);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "issue-tree-visualizer");

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            error_message = curl_easy_strerror(res);
            success = false;
            break;
        }

        long http_status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
        if (http_status != 200) {
            error_message = "GitHub comments request failed with status " + std::to_string(http_status);
            success = false;
            break;
        }

        json response = json::parse(read_buffer);
        if (!response.is_array()) {
            error_message = "Unexpected GitHub comments response format.";
            success = false;
            break;
        }

        for (const auto& item : response) {
            GitHubComment comment;
            comment.body = get_json_string(item, "body");
            comment.created_at = get_json_string(item, "created_at");
            comment.updated_at = get_json_string(item, "updated_at");
            comment.url = get_json_string(item, "html_url");
            if (item.contains("user") && item["user"].is_object()) {
                comment.author = get_json_string(item["user"], "login");
            }
            comments.push_back(comment);
        }

        if (response.size() < 100) {
            break;
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return success;
}

bool fetch_github_issues(const std::string& owner, const std::string& repo, std::vector<GitHubIssue>& issues, std::string& error_message, std::function<void(float, const std::string&)> progress_callback, const std::string& access_token) {
    issues.clear();
    error_message.clear();

    CURL* curl = curl_easy_init();
    if (!curl) {
        error_message = "Unable to initialize GitHub request.";
        return false;
    }

    struct curl_slist* headers = nullptr;
    apply_common_headers(headers, access_token);

    bool success = true;
    long http_status = 0;

    if (progress_callback) {
        progress_callback(0.05f, "Connecting to GitHub...");
    }

    for (int page = 1; page <= 10; ++page) {
        std::string read_buffer;
        std::string url = "https://api.github.com/repos/" + owner + "/" + repo + "/issues?state=all&per_page=100&page=" + std::to_string(page);

        if (progress_callback) {
            float progress = 0.1f + (static_cast<float>(page - 1) / 10.0f) * 0.8f;
            progress_callback(progress, "Fetching issues page " + std::to_string(page) + "...");
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &read_buffer);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "issue-tree-visualizer");

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            error_message = curl_easy_strerror(res);
            success = false;
            break;
        }

        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
        if (http_status != 200) {
            try {
                json response = json::parse(read_buffer);
                if (response.is_object() && response.contains("message")) {
                    error_message = response["message"].get<std::string>();
                } else {
                    error_message = "GitHub request failed with status " + std::to_string(http_status);
                }
            } catch (...) {
                error_message = "GitHub request failed with status " + std::to_string(http_status);
            }
            success = false;
            break;
        }

        json response = json::parse(read_buffer);
        if (!response.is_array()) {
            error_message = "Unexpected GitHub response format.";
            success = false;
            break;
        }

        for (const auto& item : response) {
            GitHubIssue issue;
            issue.number = item.value("number", 0);
            issue.title = get_json_string(item, "title");
            issue.body = get_json_string(item, "body");
            issue.state = get_json_string(item, "state");
            issue.created_at = get_json_string(item, "created_at");
            issue.updated_at = get_json_string(item, "updated_at");
            issue.closed_at = get_json_string(item, "closed_at");
            issue.url = get_json_string(item, "html_url");
            issue.comments_url = get_json_string(item, "comments_url");
            issue.comment_count = item.value("comments", 0);
            issue.is_pull_request = item.contains("pull_request");
            issue.reactions = item.contains("reactions") && item["reactions"].is_object() ? item["reactions"].value("total_count", 0) : 0;

            if (item.contains("user") && item["user"].is_object()) {
                issue.author = get_json_string(item["user"], "login");
            }

            if (item.contains("milestone") && item["milestone"].is_object() && !item["milestone"].is_null()) {
                issue.milestone = get_json_string(item["milestone"], "title");
            }

            if (item.contains("labels") && item["labels"].is_array()) {
                for (const auto& label : item["labels"]) {
                    if (label.contains("name") && !label["name"].is_null()) {
                        issue.labels.push_back(label["name"].get<std::string>());
                    }
                }
            }

            if (item.contains("assignees") && item["assignees"].is_array()) {
                for (const auto& assignee : item["assignees"]) {
                    if (assignee.contains("login") && !assignee["login"].is_null()) {
                        issue.assignees.push_back(assignee["login"].get<std::string>());
                    }
                }
            }

            if (issue.comment_count > 0 && !issue.comments_url.empty()) {
                std::vector<GitHubComment> issue_comments;
                std::string comments_error;
                if (fetch_issue_comments(issue.comments_url, issue_comments, comments_error, access_token)) {
                    issue.comments = std::move(issue_comments);
                }
            }

            if (!issue.is_pull_request) {
                issues.push_back(issue);
            }
        }

        if (response.size() < 100) {
            break;
        }
    }

    if (progress_callback) {
        progress_callback(1.0f, success ? "Repository loaded." : "Load failed.");
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (!success) {
        issues.clear();
    }

    return success;
}