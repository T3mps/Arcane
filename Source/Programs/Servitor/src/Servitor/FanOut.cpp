#include <Servitor/FanOut.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Servitor/Commands.hpp>

#include <Arcane/Remote/Redact.hpp>

#include <algorithm>
#include <exception>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace Servitor {

namespace {

std::string Dump(const nlohmann::json& j)
{
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

struct HostSlot {
    const LiveHost* host = nullptr;
    Arcane::Remote::Envelope envelope;
    int exitCode = 0;
};

// One worker: its own slot, nothing else shared. An escaped exception is this
// host's `internal` (exit 1), never a terminate.
void RunSlot(HostSlot& slot, const nlohmann::json& call, FanOutPost post, void* user)
{
    try {
        slot.exitCode = post(slot.host->descriptor, call, &slot.envelope, user);
    } catch (const std::exception& e) {
        slot.envelope = Arcane::Remote::MakeError(call.value("command", std::string{}), "internal", e.what(),
                                                  "report this as a servitor bug");
        slot.exitCode = 1;
    } catch (...) {
        slot.envelope = Arcane::Remote::MakeError(call.value("command", std::string{}), "internal",
                                                  "unknown failure while calling the host",
                                                  "report this as a servitor bug");
        slot.exitCode = 1;
    }
}

// The host's envelope as printed: ToJson plus client{exitCode, descriptor}, every
// secret redacted with THIS host's token (dump, redact, parse: the token is hex,
// so the JSON stays well formed).
nlohmann::json HostEnvelopeJson(const HostSlot& slot)
{
    nlohmann::json j = Arcane::Remote::ToJson(slot.envelope);
    j["client"] = {{"exitCode", slot.exitCode}, {"descriptor", PathUtf8(slot.host->path)}};
    const std::string redacted = Arcane::Remote::RedactSecrets(Dump(j), slot.host->descriptor.token);
    nlohmann::json parsed = Arcane::Remote::ParseJsonBounded(redacted, Arcane::Remote::kMaxReplyDepth);   // a bounded reply, re-parsed
    return parsed.is_discarded() ? nlohmann::json::object() : parsed;
}

FanOutResult Refuse(Arcane::Remote::Envelope env, const nlohmann::json& call, Format format)
{
    env.command = call.value("command", std::string{});
    FanOutResult result;
    result.wrapper = {{"hosts", nlohmann::json::array()}};
    result.exitCode = Arcane::Remote::ExitFromHost(env);
    Arcane::Remote::ClientBlock client;
    client.exitCode = result.exitCode;
    RenderOptions ro;
    ro.format = format;
    const Printed printed = Render(env, client, ro);
    result.text = printed.stdoutText;
    result.diagnostics = printed.stderrText;
    return result;
}

bool VerbIsDrive(std::string_view words)
{
    const std::string_view first = words.substr(0, words.find(' '));
    for (const nlohmann::json& verb : StaticManifest().at("verbs")) {
        if (verb.value("name", std::string{}) == first)
            return verb.value("access", std::string{}) == "drive";
    }
    return false;
}

struct LiveCtx {
    const Scan* scan = nullptr;
    GlobalOptions opt;
};

int PostLive(const Arcane::Remote::Descriptor& d, const nlohmann::json& call, Arcane::Remote::Envelope* out,
             void* user)
{
    const LiveCtx& ctx = *static_cast<const LiveCtx*>(user);
    for (const LiveHost& host : ctx.scan->live) {
        if (host.descriptor.pid != d.pid || host.descriptor.hostKind != d.hostKind)
            continue;
        CallRequest req;
        req.command = call.value("command", std::string{});
        req.args = call.contains("args") ? call.at("args") : nlohmann::json::object();
        PostedCall posted = PostToHost(host, req, ctx.opt, false);
        *out = std::move(posted.envelope);
        return posted.client.exitCode;
    }
    *out = Arcane::Remote::MakeError(call.value("command", std::string{}), "target.none",
                                     "the host left between the scan and the call", "rescan with servitor status");
    return Arcane::Remote::ExitFromHost(*out);
}

} // namespace

bool IsDriveCommand(const nlohmann::json& call)
{
    const std::string command = call.value("command", std::string{});
    if (command == "cvar.set" || command == "console.exec" || command == "host.quit" || command.starts_with("play."))
        return true;
    if (command == "capture.compare" && call.contains("args") && call.at("args").is_object())
        return call.at("args").value("bless", false);
    return false;
}

FanOutResult FanOut(const Scan& scan, std::string_view hostKindFilter, bool allKinds, const nlohmann::json& call,
                    bool drive, bool yes, Format format, FanOutPost post, void* user)
{
    if (drive && !yes) {
        return Refuse(Arcane::Remote::MakeError(call.value("command", std::string{}), "arg.inapplicable",
                                                "--all with a drive command changes every matching host",
                                                "pass --yes to confirm, or target one host with --host / --pid"),
                      call, format);
    }

    std::vector<HostSlot> slots;
    for (const LiveHost& host : scan.live) {
        if (allKinds || hostKindFilter == Arcane::Remote::ToString(host.descriptor.hostKind)) {
            slots.emplace_back();
            slots.back().host = &host;
        }
    }
    if (slots.empty()) {
        std::string message = "no live host matches --all";
        if (!allKinds)
            message += " " + std::string(hostKindFilter);
        return Refuse(Arcane::Remote::MakeError(call.value("command", std::string{}), "target.none",
                                                std::move(message), "start a host with --remote (servitor launch <kind>)"),
                      call, format);
    }

    // One thread per host; each owns its slot, and the main thread alone reads
    // them after the joins. Nothing is printed from a worker.
    if (slots.size() == 1) {
        RunSlot(slots.front(), call, post, user);
    } else {
        std::vector<std::thread> workers;
        workers.reserve(slots.size());
        for (HostSlot& slot : slots)
            workers.emplace_back([&slot, &call, post, user] { RunSlot(slot, call, post, user); });
        for (std::thread& t : workers)
            t.join();
    }

    FanOutResult result;
    result.wrapper = {{"hosts", nlohmann::json::array()}};
    std::vector<int> exits;
    std::string humanOut;
    std::string humanErr;
    std::string ndjson;
    for (const HostSlot& slot : slots) {
        exits.push_back(slot.exitCode);
        const std::string kind = Arcane::Remote::ToString(slot.host->descriptor.hostKind);
        const nlohmann::json envelope = HostEnvelopeJson(slot);
        const nlohmann::json entry = {{"pid", slot.host->descriptor.pid}, {"hostKind", kind}, {"envelope", envelope}};
        result.wrapper["hosts"].push_back(entry);
        if (format == Format::Ndjson) {
            nlohmann::json frame = entry;
            frame["type"] = "host";
            ndjson += Dump(frame) + "\n";
        } else if (format == Format::Human) {
            Arcane::Remote::ClientBlock client;
            client.exitCode = slot.exitCode;
            client.token = slot.host->descriptor.token;
            RenderOptions ro;
            ro.format = Format::Human;
            const Printed printed = Render(slot.envelope, client, ro);
            const std::string header = kind + " " + std::to_string(slot.host->descriptor.pid);
            humanOut += "== " + header + " ==\n" + printed.stdoutText;
            if (!printed.stderrText.empty())
                humanErr += "== " + header + " ==\n" + printed.stderrText;
        }
    }
    result.exitCode = Arcane::Remote::WorstExit(exits);
    switch (format) {
        case Format::Json:
            result.text = Dump(result.wrapper) + "\n";
            break;
        case Format::Ndjson:
            result.text = std::move(ndjson) +
                          Dump(nlohmann::json{{"type", "result"}, {"count", slots.size()}, {"exitCode", result.exitCode}}) +
                          "\n";
            break;
        case Format::Human:
            result.text = std::move(humanOut);
            result.diagnostics = std::move(humanErr);
            break;
    }
    return result;
}

int RunFanOut(const VerbRow& row, std::span<const std::string> rest, const GlobalOptions& opt)
{
    const std::string_view words = row.words;
    // status already lists every host; commands needs none: --all changes nothing.
    if (words == "status" || words == "commands")
        return row.local(rest, opt);

    Arcane::Remote::Envelope refusal;
    const auto dir = RemoteDirFor(opt, &refusal);
    if (!dir) {
        refusal.command = std::string(words);
        Arcane::Remote::ClientBlock client;
        client.exitCode = Arcane::Remote::ExitFromHost(refusal);
        return PrintEnvelope(refusal, client, opt);
    }
    Scan scan = ScanDirectory(*dir);
    LiveCtx ctx;
    ctx.scan = &scan;   // PostLive looks hosts up in the same scan FanOutVerb narrows
    ctx.opt = opt;
    return FanOutVerb(row, rest, opt, scan, &PostLive, &ctx, nullptr);
}

int FanOutVerb(const VerbRow& row, std::span<const std::string> rest, const GlobalOptions& opt, const Scan& scan,
               FanOutPost post, void* user, FanOutResult* captured)
{
    const std::string_view words = row.words;
    CallRequest req;
    std::string error;
    if (words == "call") {
        if (!ParseCallTokens(rest, req, &error))
            return PrintUsage(error, opt.format);
    } else if (row.command != nullptr) {
        req.command = row.command;
        if (row.args != nullptr && !row.args(rest, req.args, &error))
            return PrintUsage(error, opt.format);
        if (words == "quit" && QuitForced(rest))
            return PrintUsage("--all does not combine with quit --force: end each host by hand with --pid", opt.format);
        // Every host would write the same file (Task 27): each writes its own artifact instead.
        if (words.starts_with("screenshot ") && req.args.contains("out"))
            return PrintUsage("--all does not combine with screenshot --out: each host writes its own artifact", opt.format);
    } else {
        return PrintUsage("--all does not apply to " + std::string(words), opt.format);
    }

    Scan narrowed = scan;
    if (opt.pid != 0)
        std::erase_if(narrowed.live, [&](const LiveHost& h) { return h.descriptor.pid != opt.pid; });
    const std::string& kind = opt.allKind.empty() ? opt.hostKind : opt.allKind;

    const nlohmann::json call = {{"command", req.command}, {"args", req.args}};
    // Fail closed: `call` names any host command, so under --all it is always a drive.
    const bool drive = words == "call" || IsDriveCommand(call) || VerbIsDrive(words);
    const FanOutResult result = FanOut(narrowed, kind, kind.empty(), call, drive, opt.yes, opt.format, post, user);
    if (captured != nullptr) {
        *captured = result;
        return result.exitCode;
    }
    Printed printed;
    printed.stdoutText = result.text;
    printed.stderrText = result.diagnostics;
    return Emit(printed, result.exitCode);
}

} // namespace Servitor

#endif
