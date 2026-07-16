#pragma once

#include <functional>
#include <string>
#include <vector>

struct RepoInfo {
    std::string owner;
    std::string repo;
};

struct GitHubDeviceCode {
    std::string device_code;
    std::string user_code;
    std::string verification_uri;
    std::string verification_uri_complete;
    int expires_in = 0;
    int interval = 5;
};

struct GitHubTokenResponse {
    std::string access_token;
    std::string token_type;
    std::string scope;
};

struct GitHubUserProfile {
    std::string login;
    std::string name;
    std::string avatar_url;
};

struct GitHubComment {
    std::string author;
    std::string body;
    std::string created_at;
    std::string updated_at;
    std::string url;
};

struct GitHubIssue {
    int number = 0;
    std::string title;
    std::string body;
    std::string state;
    std::string created_at;
    std::string updated_at;
    std::string closed_at;
    std::string url;
    std::string comments_url;
    std::string author;
    std::string milestone;
    int comment_count = 0;
    int reactions = 0;
    bool is_pull_request = false;
    std::vector<std::string> labels;
    std::vector<std::string> assignees;
    std::vector<GitHubComment> comments;
};

bool parse_github_repo_url(const std::string& repo_url, RepoInfo& repo_info);
bool request_github_device_code(const std::string& client_id, const std::string& scope, GitHubDeviceCode& device_code, std::string& error_message);
bool poll_github_device_token(const std::string& client_id, const std::string& device_code, int interval_seconds, GitHubTokenResponse& token_response, std::string& error_message);
bool fetch_github_user_profile(const std::string& access_token, GitHubUserProfile& profile, std::string& error_message);
bool fetch_github_issues(const std::string& owner, const std::string& repo, std::vector<GitHubIssue>& issues, std::string& error_message, std::function<void(float, const std::string&)> progress_callback = nullptr, const std::string& access_token = "");
bool fetch_issue_comments(const std::string& comments_url, std::vector<GitHubComment>& comments, std::string& error_message, const std::string& access_token = "");