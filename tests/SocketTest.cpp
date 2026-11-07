#include <atomic>
#include <filesystem>
#include <random>
#include <thread>

#include <gtest/gtest.h>

#include "ipc/Socket.hpp"

using namespace ozzsb;

namespace {

std::filesystem::path tempSocket() {
    std::random_device random;
    return std::filesystem::temp_directory_path() / ("ozzsb-sock-" + std::to_string(random()) + ".sock");
}

Response echo(const Request& request) {
    if (request.verb == Verb::Ping) {
        return Response::success("pong");
    }
    if (request.verb == Verb::List) {
        return Response::success("2 sounds", "a\t/a.wav\nb\t/b.wav\n");
    }
    return Response::failure("got " + std::string(verbName(request.verb)) + " " + request.argument);
}

}  // namespace

TEST(Socket, RequestAndReply) {
    const auto path = tempSocket();
    ControlServer server;
    ASSERT_TRUE(server.start(path, echo));

    auto pong = sendRequest(path, Request{Verb::Ping, {}});
    ASSERT_TRUE(pong) << pong.error();
    EXPECT_EQ(*pong, Response::success("pong"));

    auto list = sendRequest(path, Request{Verb::List, {}});
    ASSERT_TRUE(list);
    EXPECT_EQ(list->body, "a\t/a.wav\nb\t/b.wav\n");

    auto play = sendRequest(path, Request{Verb::Play, "air horn"});
    ASSERT_TRUE(play);
    EXPECT_FALSE(play->ok);
    EXPECT_EQ(play->message, "got play air horn");

    server.stop();
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(Socket, SecondServerOnSameSocketIsRefused) {
    const auto path = tempSocket();
    ControlServer first;
    ASSERT_TRUE(first.start(path, echo));
    ControlServer second;
    EXPECT_FALSE(second.start(path, echo));
    EXPECT_TRUE(sendRequest(path, Request{Verb::Ping, {}}));
}

TEST(Socket, StaleSocketFileIsReplaced) {
    const auto path = tempSocket();
    {
        ControlServer server;
        ASSERT_TRUE(server.start(path, echo));
    }
    std::FILE* stale = std::fopen(path.c_str(), "w");
    std::fclose(stale);
    ControlServer server;
    ASSERT_TRUE(server.start(path, echo));
    EXPECT_TRUE(sendRequest(path, Request{Verb::Ping, {}}));
}

TEST(Socket, NoServerIsAnError) {
    EXPECT_FALSE(sendRequest(tempSocket(), Request{Verb::Ping, {}}));
}

TEST(Socket, IdleCallbackRuns) {
    const auto path = tempSocket();
    std::atomic<int> ticks{0};
    ControlServer server;
    ASSERT_TRUE(server.start(path, echo, [&] { ++ticks; }, std::chrono::milliseconds(10)));
    for (int i = 0; i < 100 && ticks.load() < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GE(ticks.load(), 3);
}
