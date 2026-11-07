#include <gtest/gtest.h>

#include "ipc/Protocol.hpp"

using namespace ozzsb;

TEST(Protocol, ParsesVerbWithNameContainingSpaces) {
    auto request = parseRequest("play air horn\n");
    ASSERT_TRUE(request);
    EXPECT_EQ(request->verb, Verb::Play);
    EXPECT_EQ(request->argument, "air horn");
}

TEST(Protocol, ParsesEveryVerb) {
    EXPECT_EQ(parseRequest("preview x")->verb, Verb::Preview);
    EXPECT_EQ(parseRequest("toggle x")->verb, Verb::Toggle);
    EXPECT_EQ(parseRequest("stop")->verb, Verb::Stop);
    EXPECT_EQ(parseRequest("stop x")->argument, "x");
    EXPECT_EQ(parseRequest("list")->verb, Verb::List);
    EXPECT_EQ(parseRequest("reload")->verb, Verb::Reload);
    EXPECT_EQ(parseRequest("status")->verb, Verb::Status);
    EXPECT_EQ(parseRequest("inputs")->verb, Verb::Inputs);
    EXPECT_EQ(parseRequest("ping\r\n")->verb, Verb::Ping);
}

TEST(Protocol, RejectsBadRequests) {
    EXPECT_FALSE(parseRequest("play"));
    EXPECT_FALSE(parseRequest("play "));
    EXPECT_FALSE(parseRequest("list extra"));
    EXPECT_FALSE(parseRequest("explode now"));
    EXPECT_FALSE(parseRequest(""));
    EXPECT_FALSE(parseRequest("play a\nplay b"));
}

TEST(Protocol, RequestRoundTrips) {
    for (const Request& request : {Request{Verb::Play, "air horn"}, Request{Verb::Stop, {}}, Request{Verb::List, {}}}) {
        auto parsed = parseRequest(formatRequest(request));
        ASSERT_TRUE(parsed);
        EXPECT_EQ(*parsed, request);
    }
}

TEST(Protocol, ResponseRoundTrips) {
    for (const Response& response : {Response::success("playing"), Response::success(),
                                      Response::success("2 sounds", "a\t/a.wav\nb\t/b.wav\n"),
                                      Response::failure("unknown sound \"x\"")}) {
        auto parsed = parseResponse(formatResponse(response));
        ASSERT_TRUE(parsed);
        EXPECT_EQ(*parsed, response);
    }
}

TEST(Protocol, ResponseBodyGetsTrailingNewline) {
    EXPECT_EQ(formatResponse(Response::success("x", "line")), "ok x\nline\n");
}

TEST(Protocol, RejectsMalformedResponse) { EXPECT_FALSE(parseResponse("maybe\n")); }
