// Host unit tests for the agui_sdk [device] extensions (REASONING_* events, interrupt outcome,
// RunAgentInput.resume[]). Pure JSON/string logic — no ESP-IDF, no hardware. Build + run with
// test/run_host_tests.sh. Purpose: catch silent breakage when re-syncing the vendored SDK to a
// newer upstream commit (see ../PATCHES.md). Plain asserts, no gtest dependency.

#include <iostream>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "core/event.h"
#include "core/session_types.h"
#include "core/error.h"

using nlohmann::json;
using namespace agui;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        ++g_checks;                                                                  \
        if (!(cond)) {                                                               \
            ++g_failures;                                                            \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " << #cond << "\n"; \
        }                                                                            \
    } while (0)

// ---- REASONING_* events ----------------------------------------------------

static void test_reasoning_event_types() {
    // parseEventType <-> eventTypeToString round-trip for every reasoning string.
    const std::pair<const char*, EventType> cases[] = {
        {"REASONING_START", EventType::ReasoningStart},
        {"REASONING_MESSAGE_START", EventType::ReasoningMessageStart},
        {"REASONING_MESSAGE_CONTENT", EventType::ReasoningMessageContent},
        {"REASONING_MESSAGE_END", EventType::ReasoningMessageEnd},
        {"REASONING_MESSAGE_CHUNK", EventType::ReasoningMessageChunk},
        {"REASONING_END", EventType::ReasoningEnd},
        {"REASONING_ENCRYPTED_VALUE", EventType::ReasoningEncryptedValue},
    };
    for (const auto& c : cases) {
        CHECK(EventParser::parseEventType(c.first) == c.second);
        CHECK(EventParser::eventTypeToString(c.second) == c.first);
    }
}

static void test_reasoning_parse_and_fields() {
    json j = {{"type", "REASONING_MESSAGE_CONTENT"}, {"messageId", "m1"}, {"delta", "because"}};
    std::unique_ptr<Event> ev = EventParser::parse(j);
    CHECK(ev != nullptr);
    CHECK(ev->type() == EventType::ReasoningMessageContent);
    auto* rc = static_cast<ReasoningMessageContentEvent*>(ev.get());
    CHECK(rc->messageId == "m1");
    CHECK(rc->delta == "because");

    // toJson is faithful to the wire shape.
    json out = rc->toJson();
    CHECK(out["type"] == "REASONING_MESSAGE_CONTENT");
    CHECK(out["messageId"] == "m1");
    CHECK(out["delta"] == "because");
}

static void test_reasoning_roundtrips() {
    {
        json j = {{"type", "REASONING_START"}, {"messageId", "abc"}};
        auto e = ReasoningStartEvent::fromJson(j);
        CHECK(e.messageId == "abc");
        CHECK(e.toJson()["messageId"] == "abc");
        CHECK(e.toJson()["type"] == "REASONING_START");
    }
    {
        json j = {{"type", "REASONING_ENCRYPTED_VALUE"},
                  {"subtype", "message"},
                  {"entityId", "e9"},
                  {"encryptedValue", "Zm9v"}};
        auto e = ReasoningEncryptedValueEvent::fromJson(j);
        CHECK(e.subtype == "message");
        CHECK(e.entityId == "e9");
        CHECK(e.encryptedValue == "Zm9v");
        json out = e.toJson();
        CHECK(out["subtype"] == "message");
        CHECK(out["entityId"] == "e9");
        CHECK(out["encryptedValue"] == "Zm9v");
    }
    {
        // Chunk: messageId/delta are optional.
        json j = {{"type", "REASONING_MESSAGE_CHUNK"}, {"delta", "x"}};
        auto e = ReasoningMessageChunkEvent::fromJson(j);
        CHECK(e.delta.has_value() && *e.delta == "x");
        CHECK(!e.messageId.has_value());
        CHECK(e.toJson().contains("delta"));
        CHECK(!e.toJson().contains("messageId"));
    }
}

// ---- Interrupt outcome on RUN_FINISHED --------------------------------------

static void test_run_finished_interrupt() {
    json j = {
        {"type", "RUN_FINISHED"},
        {"threadId", "t1"},
        {"runId", "r1"},
        {"outcome",
         {{"type", "interrupt"},
          {"interrupts",
           json::array({{{"id", "i1"},
                         {"reason", "need_input"},
                         {"message", "Pick one"},
                         {"toolCallId", "tc1"},
                         {"responseSchema", {{"type", "boolean"}}},
                         {"expiresAt", 1750000000000LL}}})}}}};

    auto e = RunFinishedEvent::fromJson(j);
    CHECK(e.isInterrupt());
    CHECK(e.outcomeType.has_value() && *e.outcomeType == "interrupt");
    CHECK(e.interrupts.size() == 1);
    if (e.interrupts.size() == 1) {
        const Interrupt& it = e.interrupts[0];
        CHECK(it.id == "i1");
        CHECK(it.reason == "need_input");
        CHECK(it.message.has_value() && *it.message == "Pick one");
        CHECK(it.toolCallId.has_value() && *it.toolCallId == "tc1");
        CHECK(it.responseSchema.has_value() && (*it.responseSchema)["type"] == "boolean");
        CHECK(it.expiresAt.has_value() && *it.expiresAt == 1750000000000LL);
    }

    // Round-trip: toJson must re-emit the outcome+interrupts and parse back identically.
    json out = e.toJson();
    CHECK(out.contains("outcome"));
    CHECK(out["outcome"]["type"] == "interrupt");
    CHECK(out["outcome"]["interrupts"].size() == 1);
    auto e2 = RunFinishedEvent::fromJson(out);
    CHECK(e2.isInterrupt());
    CHECK(e2.interrupts.size() == 1 && e2.interrupts[0].id == "i1");
}

static void test_run_finished_plain() {
    // No outcome → not an interrupt, no interrupts, and toJson must not invent an outcome.
    json j = {{"type", "RUN_FINISHED"}, {"threadId", "t1"}, {"runId", "r1"}};
    auto e = RunFinishedEvent::fromJson(j);
    CHECK(!e.isInterrupt());
    CHECK(e.interrupts.empty());
    CHECK(!e.outcomeType.has_value());
    CHECK(!e.toJson().contains("outcome"));
}

// ---- RunAgentInput.resume[] -------------------------------------------------

static void test_resume_serialization() {
    RunAgentInput in;
    in.threadId = "t1";
    in.runId = "r1";
    ResumeEntry r;
    r.interruptId = "i1";
    r.status = ResumeStatus::Resolved;
    r.payload = json{{"answer", true}};
    in.resume.push_back(r);

    json out = in.toJson();
    CHECK(out.contains("resume"));
    CHECK(out["resume"].size() == 1);
    CHECK(out["resume"][0]["interruptId"] == "i1");
    CHECK(out["resume"][0]["status"] == "RESOLVED");
    CHECK(out["resume"][0]["payload"]["answer"] == true);

    // fromJson parses resume back.
    auto in2 = RunAgentInput::fromJson(out);
    CHECK(in2.resume.size() == 1);
    if (in2.resume.size() == 1) {
        CHECK(in2.resume[0].interruptId == "i1");
        CHECK(in2.resume[0].status == ResumeStatus::Resolved);
    }

    // Cancelled status round-trips.
    CHECK(ResumeEntry::statusToString(ResumeStatus::Cancelled) == "CANCELLED");
    CHECK(ResumeEntry::statusFromString("CANCELLED") == ResumeStatus::Cancelled);
}

static void test_resume_absent_when_empty() {
    // No resume entries → the key must be omitted (don't send an empty array).
    RunAgentInput in;
    in.threadId = "t1";
    in.runId = "r1";
    CHECK(!in.toJson().contains("resume"));
}

// ---- Regression guards for behavior a resync must NOT change ----------------

static void test_upstream_behavior_preserved() {
    // Deprecated THINKING_* still parse (we kept them alongside REASONING_*).
    CHECK(EventParser::parseEventType("THINKING_TEXT_MESSAGE_CONTENT") ==
          EventType::ThinkingTextMessageContent);

    // Unknown types still throw (forward-compat skip happens above this, in parseSseEventData).
    bool threw = false;
    try {
        EventParser::parseEventType("NOPE_NOT_A_TYPE");
    } catch (const AgentError&) {
        threw = true;
    }
    CHECK(threw);
}

int main() {
    test_reasoning_event_types();
    test_reasoning_parse_and_fields();
    test_reasoning_roundtrips();
    test_run_finished_interrupt();
    test_run_finished_plain();
    test_resume_serialization();
    test_resume_absent_when_empty();
    test_upstream_behavior_preserved();

    std::cout << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    if (g_failures) {
        std::cerr << g_failures << " CHECK(s) FAILED\n";
        return 1;
    }
    std::cout << "OK\n";
    return 0;
}
