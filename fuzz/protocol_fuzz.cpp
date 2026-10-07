// libFuzzer harness: the client wire protocol (ArcaneCore/src/Arcane/Net).
//
// Wire format: LENGTH:TYPE|TOKEN|PAYLOAD\n -- ExtractLengthFramed (TcpSocket.hpp)
// strips the length frame, Message::ParseBody (Protocol.hpp) splits the body.
// Both run on bytes straight off the open internet in the Aphelyon servers.
//
// The input is a raw TCP byte stream. The first byte picks how the stream is
// cut into recv() chunks and which body cap the reader uses; the rest is the
// stream. The harness replays the server's reassembly loop (append chunk ->
// cap check -> extract frames until needMoreData -> erase consumed; error ==
// drop the connection) over the CHUNKED stream and over the WHOLE stream, and
// checks properties a correct parser must have:
//
//   1. Segmentation independence: the frames (and the drop decision) do not
//      depend on how TCP happened to split the bytes.
//   2. An accepted frame's LENGTH is plain ASCII decimal (the M-V4-5 rule:
//      "the length prefix must be all-numeric"), and the frame is exactly
//      LENGTH ':' BODY '\n' with |BODY| == LENGTH.
//   3. A body ParseBody accepts (type != kInvalidMsgId) has a TYPE field that
//      is plain decimal naming exactly that id -- no wrap, sign, whitespace
//      or trailing junk -- and re-serializes to an equal message.
//
// A violated property prints the property and aborts, so libFuzzer records a crash.

#include <Arcane/Net/Protocol.hpp>
#include <Arcane/Net/TcpSocket.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    // A violated property: name it on stderr, then abort so libFuzzer records
    // the input as a crash.
    [[noreturn]] void Fail(const char* property, int line)
    {
        std::fprintf(stderr, "protocol_fuzz: property violated (line %d): %s\n", line, property);
        std::abort();
    }
#define FUZZ_CHECK(cond) do { if (!(cond)) Fail(#cond, __LINE__); } while (0)

    // The server reassembly loop, minus the socket. Mirrors the documented
    // contract of ExtractLengthFramed: erase `consumed` on success, wait on
    // needMoreData, drop the connection on error, and never let the buffer
    // grow past MAX_RECEIVE_BUFFER_SIZE.
    struct Connection
    {
        std::string buffer;
        std::vector<std::string> bodies;
        bool dropped = false;
        size_t maxBody;

        explicit Connection(size_t cap) : maxBody(cap) {}

        void Feed(std::string_view chunk)
        {
            if (dropped) return;
            buffer.append(chunk);
            for (;;)
            {
                Arcane::LengthFrameResult r = Arcane::ExtractLengthFramed(buffer, maxBody);
                if (r.error) { dropped = true; return; }
                if (r.needMoreData) break;

                // Property 2: the consumed frame is LENGTH ':' BODY '\n'.
                FUZZ_CHECK(r.consumed > 0 && r.consumed <= buffer.size());
                const size_t colon = buffer.find(':');
                FUZZ_CHECK(colon != std::string::npos && colon > 0);
                for (size_t i = 0; i < colon; ++i)
                    FUZZ_CHECK(buffer[i] >= '0' && buffer[i] <= '9');   // LENGTH is plain decimal
                FUZZ_CHECK(r.consumed == colon + 1 + r.body.size() + 1);
                FUZZ_CHECK(buffer[r.consumed - 1] == '\n');
                FUZZ_CHECK(std::stoull(buffer.substr(0, colon)) == r.body.size());
                FUZZ_CHECK(r.body.size() <= maxBody);

                bodies.push_back(std::move(r.body));
                buffer.erase(0, r.consumed);
            }
            // The server's own cap: a peer that never completes a frame is cut off.
            if (buffer.size() > Arcane::ServerConfig::MAX_RECEIVE_BUFFER_SIZE)
                dropped = true;
        }
    };

    bool IsPlainDecimal(std::string_view s)
    {
        if (s.empty()) return false;
        for (char c : s)
            if (c < '0' || c > '9') return false;
        return true;
    }

    void CheckBody(const std::string& body)
    {
        const Arcane::Message msg = Arcane::Message::ParseBody(body);

        // Exercise the rest of the Message surface a server touches per message.
        (void)msg.HasToken();
        (void)msg.ToString();
        (void)Arcane::RequiresAuthentication(msg.type);
        (void)Arcane::IsRequestMessage(msg.type);

        if (msg.type == Arcane::kInvalidMsgId)
            return;

        // Property 3: the TYPE text names exactly this id.
        const size_t pipe = body.find('|');
        FUZZ_CHECK(pipe != std::string::npos);
        const std::string_view typeText(body.data(), pipe);
        FUZZ_CHECK(IsPlainDecimal(typeText));
        uint64_t typeValue = 0;
        const auto [end, ec] = std::from_chars(typeText.data(), typeText.data() + typeText.size(), typeValue);
        FUZZ_CHECK(ec == std::errc{} && end == typeText.data() + typeText.size());
        FUZZ_CHECK(typeValue == msg.type);   // no silent uint16 wrap

        // Round trip: Serialize -> frame -> parse gives the same message back.
        const std::string wire = msg.Serialize();
        const Arcane::LengthFrameResult again = Arcane::ExtractLengthFramed(wire, SIZE_MAX / 4);
        FUZZ_CHECK(!again.error && !again.needMoreData && again.consumed == wire.size());
        const Arcane::Message back = Arcane::Message::ParseBody(again.body);
        FUZZ_CHECK(back.type == msg.type && back.token == msg.token && back.payload == msg.payload);
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 1) return 0;
    const uint8_t mode = data[0];
    const std::string_view stream(reinterpret_cast<const char*>(data + 1), size - 1);

    // Two caps a server might pass: the default (MAX_RECEIVE_BUFFER_SIZE) and
    // the payload cap.
    const size_t cap = (mode & 0x80) ? Arcane::ServerConfig::MAX_PAYLOAD_SIZE
                                     : Arcane::ServerConfig::MAX_RECEIVE_BUFFER_SIZE;

    // Chunked replay: chunk sizes cycle through a small deterministic sequence
    // derived from the mode byte (1..16 bytes), so splits land everywhere.
    Connection chunked(cap);
    {
        size_t pos = 0;
        uint32_t state = mode | 1u;
        while (pos < stream.size() && !chunked.dropped)
        {
            state = state * 1103515245u + 12345u;
            const size_t n = std::min<size_t>(1 + ((state >> 16) & 0x0F), stream.size() - pos);
            chunked.Feed(stream.substr(pos, n));
            pos += n;
        }
    }

    Connection whole(cap);
    whole.Feed(stream);

    // Property 1: segmentation independence. The whole-buffer replay sees
    // every byte at once, so it may only differ by having MORE frames when the
    // chunked one was dropped for the buffer cap (it never is here: the cap is
    // checked after draining, and chunks are tiny).
    FUZZ_CHECK(chunked.dropped == whole.dropped);
    FUZZ_CHECK(chunked.bodies == whole.bodies);

    for (const std::string& body : whole.bodies)
        CheckBody(body);

    // ParseBody also sees the raw stream directly: a harness for body-level
    // edge cases (embedded NULs, newlines, many pipes) without framing.
    CheckBody(std::string(stream));
    return 0;
}
