#include "ipc/Protocol.hpp"

#include <array>
#include <format>
#include <utility>

namespace ozzsb {

namespace {

struct VerbInfo {
    Verb verb;
    std::string_view name;
    enum class Argument { None, Required, Optional } argument;
};

constexpr std::array kVerbs{
    VerbInfo{Verb::Play, "play", VerbInfo::Argument::Required},
    VerbInfo{Verb::Preview, "preview", VerbInfo::Argument::Required},
    VerbInfo{Verb::Toggle, "toggle", VerbInfo::Argument::Required},
    VerbInfo{Verb::Stop, "stop", VerbInfo::Argument::Optional},
    VerbInfo{Verb::List, "list", VerbInfo::Argument::None},
    VerbInfo{Verb::Reload, "reload", VerbInfo::Argument::None},
    VerbInfo{Verb::Status, "status", VerbInfo::Argument::None},
    VerbInfo{Verb::Inputs, "inputs", VerbInfo::Argument::None},
    VerbInfo{Verb::Ping, "ping", VerbInfo::Argument::None},
};

std::string_view trimLineEnd(std::string_view text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

}  // namespace

std::string_view verbName(Verb verb) {
    for (const auto& info : kVerbs) {
        if (info.verb == verb) {
            return info.name;
        }
    }
    std::unreachable();
}

std::expected<Request, std::string> parseRequest(std::string_view line) {
    line = trimLineEnd(line);
    if (line.find('\n') != std::string_view::npos) {
        return std::unexpected("request must be a single line");
    }
    const auto space = line.find(' ');
    const std::string_view word = line.substr(0, space);
    const std::string_view argument = space == std::string_view::npos ? std::string_view{} : line.substr(space + 1);

    for (const auto& info : kVerbs) {
        if (info.name != word) {
            continue;
        }
        if (info.argument == VerbInfo::Argument::Required && argument.empty()) {
            return std::unexpected(std::format("{} needs a sound name", info.name));
        }
        if (info.argument == VerbInfo::Argument::None && !argument.empty()) {
            return std::unexpected(std::format("{} takes no argument", info.name));
        }
        return Request{info.verb, std::string(argument)};
    }
    return std::unexpected(std::format("unknown command \"{}\"", word));
}

std::string formatRequest(const Request& request) {
    if (request.argument.empty()) {
        return std::format("{}\n", verbName(request.verb));
    }
    return std::format("{} {}\n", verbName(request.verb), request.argument);
}

Response Response::success(std::string message, std::string body) {
    return Response{true, std::move(message), std::move(body)};
}

Response Response::failure(std::string message) {
    return Response{false, std::move(message), {}};
}

std::string formatResponse(const Response& response) {
    std::string text = response.ok ? "ok" : "error";
    if (!response.message.empty()) {
        text += ' ';
        text += response.message;
    }
    text += '\n';
    text += response.body;
    if (!response.body.empty() && response.body.back() != '\n') {
        text += '\n';
    }
    return text;
}

std::expected<Response, std::string> parseResponse(std::string_view text) {
    const auto newline = text.find('\n');
    const std::string_view first = trimLineEnd(text.substr(0, newline));
    const std::string_view body = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);

    const auto space = first.find(' ');
    const std::string_view status = first.substr(0, space);
    const std::string_view message = space == std::string_view::npos ? std::string_view{} : first.substr(space + 1);
    if (status == "ok") {
        return Response{true, std::string(message), std::string(body)};
    }
    if (status == "error") {
        return Response{false, std::string(message), std::string(body)};
    }
    return std::unexpected(std::format("malformed reply \"{}\"", first));
}

}  // namespace ozzsb
