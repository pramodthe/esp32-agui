#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "core/error.h"
#include "core/session_types.h"
#include "core/state.h"

namespace agui {

enum class EventType {
    // Text message events
    TextMessageStart,
    TextMessageContent,
    TextMessageEnd,
    TextMessageChunk,

    // Thinking message events
    ThinkingTextMessageStart,
    ThinkingTextMessageContent,
    ThinkingTextMessageEnd,

    // Tool call events
    ToolCallStart,
    ToolCallArgs,
    ToolCallEnd,
    ToolCallChunk,
    ToolCallResult,

    // Thinking step events
    ThinkingStart,
    ThinkingEnd,

    // [device] Reasoning events (current AG-UI protocol; the THINKING_* family above is deprecated
    // and removed in protocol 1.0). See PATCHES.md. Mirrors sdks/typescript core events.ts.
    ReasoningStart,
    ReasoningMessageStart,
    ReasoningMessageContent,
    ReasoningMessageEnd,
    ReasoningMessageChunk,
    ReasoningEnd,
    ReasoningEncryptedValue,

    // State management events
    StateSnapshot,
    StateDelta,
    MessagesSnapshot,

    // Activity events
    ActivitySnapshot,
    ActivityDelta,

    // Run lifecycle events
    RunStarted,
    RunFinished,
    RunError,

    // Step events
    StepStarted,
    StepFinished,

    // Extension events
    Raw,
    Custom
};

// Common base fields for all events. Both fields are optional per the AG-UI protocol.
struct BaseEventData {
    std::optional<int64_t> timestamp;  // milliseconds since epoch

    std::optional<nlohmann::json> rawEvent;

    BaseEventData() = default;

    nlohmann::json toJson() const;
    static BaseEventData fromJson(const nlohmann::json& j);
};

class Event {
protected:
    BaseEventData m_baseData;

public:
    Event() = default;
    virtual ~Event() = default;

    // Non-copyable, movable
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    Event(Event&&) = default;
    Event& operator=(Event&&) = default;

    virtual EventType type() const = 0;
    virtual nlohmann::json toJson() const = 0;
    virtual void validate() const {}

    const BaseEventData& baseData() const { return m_baseData; }
    void setRawEvent(const nlohmann::json& raw);

protected:
    // Returns a JSON object pre-populated with base fields (timestamp, rawEvent).
    // Derived toJson() implementations should start from this.
    nlohmann::json baseFieldsToJson() const;
};

struct TextMessageStartEvent : public Event {
    MessageId messageId;
    std::optional<MessageRole> role;

    EventType type() const override { return EventType::TextMessageStart; }
    nlohmann::json toJson() const override;
    void validate() const override;
    static TextMessageStartEvent fromJson(const nlohmann::json& j);
};

struct TextMessageContentEvent : public Event {
    MessageId messageId;
    std::string delta;

    EventType type() const override { return EventType::TextMessageContent; }
    nlohmann::json toJson() const override;
    void validate() const override;
    static TextMessageContentEvent fromJson(const nlohmann::json& j);
};

struct TextMessageEndEvent : public Event {
    MessageId messageId;

    EventType type() const override { return EventType::TextMessageEnd; }
    nlohmann::json toJson() const override;
    void validate() const override;
    static TextMessageEndEvent fromJson(const nlohmann::json& j);
};

// Composite event carrying delta + optional role/name; messageId may be omitted if context was
// already established by a previous chunk with the same ID.
struct TextMessageChunkEvent : public Event {
    MessageId messageId;
    std::string delta;
    std::optional<MessageRole> role;
    std::optional<std::string> name;

    EventType type() const override { return EventType::TextMessageChunk; }
    nlohmann::json toJson() const override;
    static TextMessageChunkEvent fromJson(const nlohmann::json& j);
};

struct ThinkingTextMessageStartEvent : public Event {
    EventType type() const override { return EventType::ThinkingTextMessageStart; }
    nlohmann::json toJson() const override;
    static ThinkingTextMessageStartEvent fromJson(const nlohmann::json& j);
};

struct ThinkingTextMessageContentEvent : public Event {
    std::string delta;

    EventType type() const override { return EventType::ThinkingTextMessageContent; }
    nlohmann::json toJson() const override;
    static ThinkingTextMessageContentEvent fromJson(const nlohmann::json& j);
};

struct ThinkingTextMessageEndEvent : public Event {
    EventType type() const override { return EventType::ThinkingTextMessageEnd; }
    nlohmann::json toJson() const override;
    static ThinkingTextMessageEndEvent fromJson(const nlohmann::json& j);
};

struct ToolCallStartEvent : public Event {
    ToolCallId toolCallId;
    std::string toolCallName;
    std::optional<MessageId> parentMessageId;

    EventType type() const override { return EventType::ToolCallStart; }
    nlohmann::json toJson() const override;
    void validate() const override;
    static ToolCallStartEvent fromJson(const nlohmann::json& j);
};

struct ToolCallArgsEvent : public Event {
    ToolCallId toolCallId;
    std::string delta;

    EventType type() const override { return EventType::ToolCallArgs; }
    nlohmann::json toJson() const override;
    void validate() const override;
    static ToolCallArgsEvent fromJson(const nlohmann::json& j);
};

struct ToolCallEndEvent : public Event {
    ToolCallId toolCallId;

    EventType type() const override { return EventType::ToolCallEnd; }
    nlohmann::json toJson() const override;
    void validate() const override;
    static ToolCallEndEvent fromJson(const nlohmann::json& j);
};

// Composite event; toolCallId may be omitted if context was established by a previous chunk.
struct ToolCallChunkEvent : public Event {
    ToolCallId toolCallId;
    std::optional<std::string> toolCallName;
    std::string delta;
    std::optional<MessageId> parentMessageId;

    EventType type() const override { return EventType::ToolCallChunk; }
    nlohmann::json toJson() const override;
    static ToolCallChunkEvent fromJson(const nlohmann::json& j);
};

struct ToolCallResultEvent : public Event {
    MessageId messageId;
    ToolCallId toolCallId;
    std::string content;
    std::optional<MessageRole> role;

    EventType type() const override { return EventType::ToolCallResult; }
    nlohmann::json toJson() const override;
    void validate() const override;
    static ToolCallResultEvent fromJson(const nlohmann::json& j);
};

struct ThinkingStartEvent : public Event {
    EventType type() const override { return EventType::ThinkingStart; }
    nlohmann::json toJson() const override;
    static ThinkingStartEvent fromJson(const nlohmann::json& j);
};

struct ThinkingEndEvent : public Event {
    EventType type() const override { return EventType::ThinkingEnd; }
    nlohmann::json toJson() const override;
    static ThinkingEndEvent fromJson(const nlohmann::json& j);
};

// [device] Reasoning events — current AG-UI protocol reasoning lifecycle (replaces deprecated
// THINKING_*). Fields mirror the TypeScript core events.ts. See PATCHES.md.
struct ReasoningStartEvent : public Event {
    MessageId messageId;

    EventType type() const override { return EventType::ReasoningStart; }
    nlohmann::json toJson() const override;
    static ReasoningStartEvent fromJson(const nlohmann::json& j);
};

struct ReasoningMessageStartEvent : public Event {
    MessageId messageId;   // role is always "reasoning"

    EventType type() const override { return EventType::ReasoningMessageStart; }
    nlohmann::json toJson() const override;
    static ReasoningMessageStartEvent fromJson(const nlohmann::json& j);
};

struct ReasoningMessageContentEvent : public Event {
    MessageId messageId;
    std::string delta;

    EventType type() const override { return EventType::ReasoningMessageContent; }
    nlohmann::json toJson() const override;
    void validate() const override;
    static ReasoningMessageContentEvent fromJson(const nlohmann::json& j);
};

struct ReasoningMessageEndEvent : public Event {
    MessageId messageId;

    EventType type() const override { return EventType::ReasoningMessageEnd; }
    nlohmann::json toJson() const override;
    static ReasoningMessageEndEvent fromJson(const nlohmann::json& j);
};

// Composite event; messageId may be omitted if a previous chunk established context. An empty
// delta closes the reasoning message.
struct ReasoningMessageChunkEvent : public Event {
    std::optional<MessageId> messageId;
    std::optional<std::string> delta;

    EventType type() const override { return EventType::ReasoningMessageChunk; }
    nlohmann::json toJson() const override;
    static ReasoningMessageChunkEvent fromJson(const nlohmann::json& j);
};

struct ReasoningEndEvent : public Event {
    MessageId messageId;

    EventType type() const override { return EventType::ReasoningEnd; }
    nlohmann::json toJson() const override;
    static ReasoningEndEvent fromJson(const nlohmann::json& j);
};

// Encrypted chain-of-thought attached to a message or tool call (opaque to the client).
struct ReasoningEncryptedValueEvent : public Event {
    std::string subtype;        // "message" | "tool-call"
    std::string entityId;
    std::string encryptedValue;

    EventType type() const override { return EventType::ReasoningEncryptedValue; }
    nlohmann::json toJson() const override;
    static ReasoningEncryptedValueEvent fromJson(const nlohmann::json& j);
};

struct StateSnapshotEvent : public Event {
    nlohmann::json snapshot;

    EventType type() const override { return EventType::StateSnapshot; }
    nlohmann::json toJson() const override;
    void validate() const override;
    static StateSnapshotEvent fromJson(const nlohmann::json& j);
};

struct StateDeltaEvent : public Event {
    nlohmann::json delta;  // JSON Patch array (RFC 6902)

    EventType type() const override { return EventType::StateDelta; }
    nlohmann::json toJson() const override;
    void validate() const override;
    static StateDeltaEvent fromJson(const nlohmann::json& j);
};

struct MessagesSnapshotEvent : public Event {
    std::vector<Message> messages;

    EventType type() const override { return EventType::MessagesSnapshot; }
    nlohmann::json toJson() const override;
    void validate() const override;
    static MessagesSnapshotEvent fromJson(const nlohmann::json& j);
};

// Complete snapshot of an activity message. When replace=true (default), overwrites existing content.
struct ActivitySnapshotEvent : public Event {
    MessageId messageId;
    std::string activityType;   // e.g., "PLAN", "SEARCH"
    nlohmann::json content;
    bool replace = true;

    EventType type() const override { return EventType::ActivitySnapshot; }
    nlohmann::json toJson() const override;
    void validate() const override;
    static ActivitySnapshotEvent fromJson(const nlohmann::json& j);
};

// Incremental update to an activity message using JSON Patch (RFC 6902).
struct ActivityDeltaEvent : public Event {
    MessageId messageId;
    std::string activityType;
    std::vector<JsonPatchOp> patch;

    EventType type() const override { return EventType::ActivityDelta; }
    nlohmann::json toJson() const override;
    void validate() const override;
    static ActivityDeltaEvent fromJson(const nlohmann::json& j);
};

struct RunStartedEvent : public Event {
    ThreadId threadId;
    RunId runId;

    EventType type() const override { return EventType::RunStarted; }
    nlohmann::json toJson() const override;
    void validate() const override;
    static RunStartedEvent fromJson(const nlohmann::json& j);
};

struct RunFinishedEvent : public Event {
    ThreadId threadId;
    RunId runId;
    nlohmann::json result;
    // [device] Optional run outcome (current protocol). outcomeType is "success" or "interrupt";
    // when "interrupt", `interrupts` holds the open HITL interrupts to resolve. See PATCHES.md.
    std::optional<std::string> outcomeType;
    std::vector<Interrupt> interrupts;

    bool isInterrupt() const { return outcomeType.has_value() && *outcomeType == "interrupt"; }

    EventType type() const override { return EventType::RunFinished; }
    nlohmann::json toJson() const override;
    void validate() const override;
    static RunFinishedEvent fromJson(const nlohmann::json& j);
};

struct RunErrorEvent : public Event {
    std::string message;
    std::optional<std::string> code;

    EventType type() const override { return EventType::RunError; }
    nlohmann::json toJson() const override;
    void validate() const override;
    static RunErrorEvent fromJson(const nlohmann::json& j);
};

struct StepStartedEvent : public Event {
    std::string stepName;

    EventType type() const override { return EventType::StepStarted; }
    nlohmann::json toJson() const override;
    void validate() const override;
    static StepStartedEvent fromJson(const nlohmann::json& j);
};

struct StepFinishedEvent : public Event {
    std::string stepName;

    EventType type() const override { return EventType::StepFinished; }
    nlohmann::json toJson() const override;
    void validate() const override;
    static StepFinishedEvent fromJson(const nlohmann::json& j);
};

struct RawEvent : public Event {
    nlohmann::json event;
    std::optional<std::string> source;

    EventType type() const override { return EventType::Raw; }
    nlohmann::json toJson() const override;
    static RawEvent fromJson(const nlohmann::json& j);
};

struct CustomEvent : public Event {
    std::string name;
    nlohmann::json value;

    EventType type() const override { return EventType::Custom; }
    nlohmann::json toJson() const override;
    static CustomEvent fromJson(const nlohmann::json& j);
};

class EventParser {
public:
    // Parses a JSON object into an Event. Throws AgentError on failure.
    static std::unique_ptr<Event> parse(const nlohmann::json& j);

    static EventType parseEventType(const std::string& typeStr);
    static std::string eventTypeToString(EventType type);
};

}  // namespace agui
