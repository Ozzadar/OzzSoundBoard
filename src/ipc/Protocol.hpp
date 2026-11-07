#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace ozzsb {

// One request per connection: a single line "<verb> [argument]". The argument is the rest of the
// line, so sound names may contain spaces. The reply is "ok[ text]" or "error <message>", optionally
// followed by body lines, then the server closes the connection.
enum class Verb {
    Play,     // play <name>      into the virtual mic and the speakers
    Preview,  // preview <name>   speakers only
    Toggle,   // toggle <name>    stop if playing, otherwise play
    Stop,     // stop [name]      stop one sound or everything
    List,     // list             body: "<name>\t<path>" per sound
    Reload,   // reload           re-read the config file
    Status,   // status           daemon state
    Inputs,   // inputs           body: "<node name>\t<description>\t<flags>" per microphone,
              //                  flags: comma list of default, active, preferred
    Ping,     // ping
};

struct Request {
    Verb verb;
    std::string argument;

    bool operator==(const Request&) const = default;
};

std::expected<Request, std::string> parseRequest(std::string_view line);
std::string formatRequest(const Request& request);
std::string_view verbName(Verb verb);

struct Response {
    bool ok = true;
    std::string message;  // first-line text (error message when !ok)
    std::string body;     // lines after the first

    static Response success(std::string message = {}, std::string body = {});
    static Response failure(std::string message);

    bool operator==(const Response&) const = default;
};

std::string formatResponse(const Response& response);
std::expected<Response, std::string> parseResponse(std::string_view text);

}  // namespace ozzsb
