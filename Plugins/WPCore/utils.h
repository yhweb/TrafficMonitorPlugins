#pragma once

#include <string>
#include <map>
#include <unordered_map>

namespace utils
{
    struct HttpParams
    {
        std::unordered_map<std::string, std::string> data;
    };

    struct HttpHeaders
    {
        std::unordered_map<std::string, std::string> data;
    };

    // Returns the PEM-encoded root certificates of the Windows certificate store.
    // Prebuilt OpenSSL binaries frequently carry an OPENSSLDIR that does not exist,
    // which makes every HTTPS handshake fail with X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN
    // (error 19). Building the trust store from Windows itself keeps certificate
    // verification enabled while working on any machine.
    const std::string &getWindowsRootCaPem();

    int internetGet(const std::string &host, const std::string &path, std::string &content,
                    const HttpParams &params = {}, const HttpHeaders &headers = {});

    int internetGetWithRetry(const std::string &host, const std::string &path, std::string &content,
                             const HttpParams &params = {}, const HttpHeaders &headers = {},
                             int max_attempts = 3);

    std::string timestamp_string(std::int64_t sec);
    std::string timestamp_string_date(std::int64_t sec);
    std::string timestamp_string_time(std::int64_t sec);

    std::int64_t parse_iso_datetime_to_utc_seconds(std::string_view iso_datatime);
    std::int64_t parse_iso_datetime_to_local_seconds(std::string_view iso_datatime);

    std::string string_join(const std::vector<std::string> &parts, std::string_view delim);

    std::string_view get_direction_text(std::string_view compass_code);
    std::string_view get_precipitation_text(std::string_view type_code);
}
