/*
 * Dulcamara - production Lapy one-shot helper client regression.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "../src/platform/elevation/elevation.cpp"

// glibc declares these libc functions noexcept; Darwin does not.
#if defined(__APPLE__)
#define TEST_LIBC_NOEXCEPT
#else
#define TEST_LIBC_NOEXCEPT noexcept
#endif

namespace test
{
struct State
{
    bool prepare_fails{};
    bool helper_missing{};
    bool corrupt_read{};
    unsigned data_open_failures{};
    unsigned data_open_calls{};
    unsigned data_list_failures{};
    unsigned data_list_calls{};
    unsigned sleeps{};
    unsigned prepare_calls{};
    unsigned socket_calls{};
    unsigned closes{};
    bool helper_read{};
    std::string data;
    std::vector<std::uint8_t> replies;
    std::vector<std::uint8_t> sent;
    std::size_t received{};
};

State state;

void reset()
{
    state = {};
}

bool starts_with(const char *value, std::string_view prefix)
{
    return std::string_view{value}.starts_with(prefix);
}

void helper_replies(elevation::Status status = elevation::Status::ok)
{
    elevation::wire::Message prepare{};
    prepare.pid = 4242;
    prepare.kind = elevation::wire::Kind::prepare;
    auto response = prepare;
    response.kind = elevation::wire::Kind::response;
    response.status = status;
    state.replies.resize(2 * sizeof(prepare));
    std::memcpy(state.replies.data(), &prepare, sizeof(prepare));
    std::memcpy(state.replies.data() + sizeof(prepare), &response, sizeof(response));
}
} // namespace test

extern "C"
{
    pid_t getpid() TEST_LIBC_NOEXCEPT
    {
        return 4242;
    }

    uid_t geteuid() TEST_LIBC_NOEXCEPT
    {
        return 1000;
    }

    int seteuid(uid_t) TEST_LIBC_NOEXCEPT
    {
        ++test::state.prepare_calls;
        if (test::state.prepare_fails)
        {
            errno = EPERM;
            return -1;
        }
        return 0;
    }

    int open(const char *path, int flags, ...)
    {
        (void)flags;
        if (test::starts_with(path, "/data/.lapy_probe_"))
        {
            ++test::state.data_open_calls;
            test::state.data.clear();
            if (test::state.data_open_calls <= test::state.data_open_failures)
            {
                errno = EACCES;
                return -1;
            }
            return 12;
        }
        if (std::strcmp(path, "/app0/lapy.elf") == 0)
        {
            if (test::state.helper_missing)
            {
                errno = ENOENT;
                return -1;
            }
            return 13;
        }
        errno = ENOENT;
        return -1;
    }

    // /data that opens and writes but refuses a listing: what ShadowMountPlus 1.7
    // exposes to a sandboxed app. Not elevation.
    DIR *opendir(const char *path)
    {
        if (std::strcmp(path, "/data") != 0)
        {
            errno = ENOENT;
            return nullptr;
        }
        ++test::state.data_list_calls;
        if (test::state.data_list_calls <= test::state.data_list_failures)
        {
            errno = EPERM;
            return nullptr;
        }
        return reinterpret_cast<DIR *>(&test::state);
    }

    int closedir(DIR *)
    {
        return 0;
    }

    ssize_t write(int descriptor, const void *buffer, size_t size)
    {
        if (descriptor != 12)
        {
            errno = EBADF;
            return -1;
        }
        const auto count = std::min(size, std::size_t{3});
        const auto bytes = std::string_view{static_cast<const char *>(buffer), count};
        test::state.data.append(bytes);
        return static_cast<ssize_t>(count);
    }

    ssize_t read(int descriptor, void *buffer, size_t size)
    {
        if (descriptor == 13)
        {
            if (test::state.helper_read)
                return 0;
            test::state.helper_read = true;
            const char elf[] = "\x7f"
                               "ELF";
            const auto count = std::min(size, sizeof(elf) - 1);
            std::memcpy(buffer, elf, count);
            return static_cast<ssize_t>(count);
        }
        if (descriptor != 12)
        {
            errno = EBADF;
            return -1;
        }
        const auto count = std::min(size, test::state.data.size());
        std::memcpy(buffer, test::state.data.data(), count);
        if (test::state.corrupt_read && count != 0)
            static_cast<char *>(buffer)[0] = 'X';
        return static_cast<ssize_t>(count);
    }

    off_t lseek(int descriptor, off_t offset, int whence) TEST_LIBC_NOEXCEPT
    {
        return descriptor == 12 && offset == 0 && whence == SEEK_SET ? 0 : -1;
    }

    int close(int)
    {
        ++test::state.closes;
        return 0;
    }

    int unlink(const char *) TEST_LIBC_NOEXCEPT
    {
        return 0;
    }

    int usleep(useconds_t)
    {
        ++test::state.sleeps;
        return 0;
    }

    int sceNetSocket(const char *, int, int, int)
    {
        ++test::state.socket_calls;
        return 20;
    }

    int sceNetSocketClose(int)
    {
        ++test::state.closes;
        return 0;
    }

    int sceNetConnect(int, const void *address, std::uint32_t length)
    {
        assert(length == sizeof(NetSockaddrIn));
        const auto &endpoint = *static_cast<const NetSockaddrIn *>(address);
        assert(endpoint.address == 0x0100007f && endpoint.port == 0x3d23);
        return 0;
    }

    int sceNetSetsockopt(int, int, int, const void *, std::uint32_t)
    {
        return 0;
    }

    int sceNetSend(int, const void *bytes, std::size_t size, int)
    {
        const auto count = std::min(size, std::size_t{7});
        const auto *start = static_cast<const std::uint8_t *>(bytes);
        test::state.sent.insert(test::state.sent.end(), start, start + count);
        return static_cast<int>(count);
    }

    int sceNetRecv(int, void *bytes, std::size_t size, int)
    {
        const auto remaining = test::state.replies.size() - test::state.received;
        if (remaining == 0)
            return -1;
        const auto count = std::min({size, std::size_t{3}, remaining});
        std::memcpy(bytes, test::state.replies.data() + test::state.received, count);
        test::state.received += count;
        return static_cast<int>(count);
    }
}

int main()
{
    using elevation::Capability;
    using elevation::Status;

    // 1. Already unlocked: no helper invocation needed.
    test::reset();
    assert(elevation::request(Capability::filesystem) == Status::ok);
    assert(std::strcmp(elevation::path(), "existing") == 0);
    assert(test::state.prepare_calls == 0 && test::state.socket_calls == 0);

    // 2. /data open denied initially -> streams /app0/lapy.elf, exchanges prepare/prepared, verifies /data.
    test::reset();
    test::state.data_open_failures = 1;
    test::helper_replies();
    assert(elevation::request(Capability::filesystem) == Status::ok);
    assert(std::strcmp(elevation::path(), "helper") == 0);
    assert(test::state.prepare_calls == 1 && test::state.socket_calls == 1);
    assert(test::state.sent.size() == 4 + 2 * sizeof(elevation::wire::Message));

    // 3. ShadowMountPlus 1.7: /data file open succeeds, opendir("/data") returns EPERM -> invokes helper, waits.
    test::reset();
    test::state.data_list_failures = 1;
    test::helper_replies();
    assert(elevation::request(Capability::filesystem) == Status::ok);
    assert(std::strcmp(elevation::path(), "helper") == 0);
    assert(test::state.prepare_calls == 1 && test::state.socket_calls == 1 && test::state.data_list_calls == 2);

    // 4. ShadowMountPlus timeout when opendir("/data") never succeeds after helper ok.
    test::reset();
    test::state.data_list_failures = 1000;
    test::helper_replies();
    assert(elevation::request(Capability::filesystem) == Status::timeout);
    assert(std::strcmp(elevation::path(), "helper") == 0);
    assert(test::state.sleeps == 20);

    // 5. Target title mismatch reported by helper.
    test::reset();
    test::state.data_open_failures = 1;
    test::helper_replies(Status::target_mismatch);
    assert(elevation::request(Capability::filesystem) == Status::target_mismatch);
    assert(test::state.socket_calls == 1 && test::state.prepare_calls == 1);

    // 6. seteuid() fails during helper prepare handshake.
    test::reset();
    test::state.data_open_failures = 1;
    test::state.prepare_fails = true;
    test::helper_replies();
    assert(elevation::request(Capability::filesystem) == Status::prepare_failed);
    assert(test::state.socket_calls == 1 && test::state.prepare_calls == 1);

    // 7. Missing /app0/lapy.elf helper returns Status::unavailable.
    test::reset();
    test::state.data_open_failures = 1;
    test::state.helper_missing = true;
    assert(elevation::request(Capability::filesystem) == Status::unavailable);
    assert(test::state.socket_calls == 0);

    // 8. Corrupt readback during /data probe fails with Status::apply_failed.
    test::reset();
    test::state.corrupt_read = true;
    assert(elevation::request(Capability::filesystem) == Status::apply_failed);
    assert(test::state.socket_calls == 0);

    // 9. Unsupported capability rejected before touching filesystem or sockets.
    test::reset();
    assert(elevation::request(static_cast<Capability>(2)) == Status::unsupported_capability);
    assert(test::state.data_open_calls == 0 && test::state.socket_calls == 0);

    // 10. Null helper path returns Status::invalid_request.
    test::reset();
    test::state.data_open_failures = 1;
    assert(elevation::request(Capability::filesystem, nullptr) == Status::invalid_request);

    // 11. Golden 24-byte wire request matches ELV1 v1 layout.
    elevation::wire::Message request{};
    request.pid = 4242;
    const std::array<std::uint8_t, 24> golden{'E', 'L', 'V', '1', 1,    0,    24, 0, 1, 0, 0, 0,
                                              1,   0,   0,   0,   0x92, 0x10, 0,  0, 0, 0, 0, 0};
    assert(std::memcmp(&request, golden.data(), golden.size()) == 0);
}
